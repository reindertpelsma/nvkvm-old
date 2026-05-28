// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_mmap.c — mmap handling for the nvkvm guest module
 *
 * When guest userspace calls mmap() on a /dev/nvidia* fd, we:
 *  1. Ask the host to map the corresponding region of the real nvidia device.
 *  2. The host maps it, pins or registers the pages with KVM, and returns
 *     the guest physical address (GPA) base.
 *  3. We use remap_pfn_range() to insert those physical pages into the
 *     requesting process's VMA.
 *
 * The critical invariant: the pages mapped into the guest VA must be the
 * same physical pages the NVIDIA driver mapped on the host. No copy, no bounce
 * buffer. This means GPU BAR pages (VRAM, doorbells, command rings) are
 * directly accessible from guest userspace — essential for zero-copy GPU
 * compute paths.
 *
 * Validation: we validate the GPA range returned by the host against the
 * known mmap window BAR before calling remap_pfn_range. A malicious host
 * could abuse this, but we are not defending against the hypervisor.
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/pfn.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>

#include "nvkvm.h"

/* Forward declarations for module-internal functions */
static void nvkvm_vma_open(struct vm_area_struct *vma);
static void nvkvm_vma_close(struct vm_area_struct *vma);
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len);


const struct vm_operations_struct nvkvm_vm_ops = {
	.open  = nvkvm_vma_open,
	.close = nvkvm_vma_close,
};

static void nvkvm_vma_open(struct vm_area_struct *vma)
{
	/* Nothing to do; reference counting is on the mmap_region. */
}

static void nvkvm_vma_close(struct vm_area_struct *vma)
{
	struct nvkvm_mmap_region *region = vma->vm_private_data;
	if (!region)
		return;

	/* Mark VMA as gone; the actual host munmap is deferred to fd close
	 * or explicit unmap, consistent with how the NVIDIA driver expects
	 * mappings to outlive individual VMAs in some cases. */
	region->vma = NULL;
}

/*
 * nvkvm_mmap_request_isolate — mmap via the isolate path (new API).
 *
 * Sends MMAP_ON_ISOLATE which causes QEMU to:
 *   1. mmap the handle fd at any host VA (QVA)
 *   2. register GPA→QVA in a KVM memory slot
 *   3. tell the isolate to map the same fd at gva (MAP_FIXED)
 *
 * The guest then uses remap_pfn_range to insert GPA pages into the VMA.
 */
static int nvkvm_mmap_request_isolate(struct nvkvm_fd_ctx *ctx,
				      struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	__u64 gva    = vma->vm_start;
	__u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	__u32 prot   = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		       (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0) |
		       (vma->vm_flags & VM_EXEC  ? PROT_EXEC  : 0);
	__u32 map_flags = MAP_SHARED;
	__u64 gpa_base;
	__u32 mmap_token;
	struct nvkvm_mmap_region *region;
	int ret;

	ret = nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id,
					   ctx->handle_id,
					   gva, offset, vma_len,
					   prot, map_flags,
					   (unsigned int)ctx->session->id,
					   &gpa_base, &mmap_token);
	if (ret)
		return ret;

	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa_base, vma_len)) {
		pr_warn("nvkvm: MMAP_ON_ISOLATE returned GPA %llx outside window\n",
			gpa_base);
		return -EIO;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa_base >> PAGE_SHIFT),
			      vma_len, vma->vm_page_prot);
	if (ret) {
		/* Ask host to undo the mmap.  Use kmalloc, not stack — see
		 * comment in nvkvm_mmap_release_fd about CONFIG_VMAP_STACK. */
		struct {
			struct nvkvm_hdr                   hdr;
			struct nvkvm_req_munmap_on_isolate req;
		} *umsg;
		struct nvkvm_inflight uinf;
		umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
		if (umsg) {
			umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
			umsg->hdr.txn_id     = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_txn_id));
			umsg->req.isolate_id = cpu_to_le32(ctx->session->isolate_id);
			umsg->req.mmap_token = cpu_to_le32(mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			kfree(umsg);
		}
		return ret;
	}

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return 0; /* mapping live; leak the region tracking */

	region->mmap_token = mmap_token;
	region->handle_id  = ctx->handle_id;
	region->gpa_base   = (unsigned long)gpa_base;
	region->length     = vma_len;
	region->offset     = offset;
	region->vma        = vma;

	spin_lock(&ctx->mmap_lock);
	list_add_tail(&region->list, &ctx->mmap_regions);
	spin_unlock(&ctx->mmap_lock);

	vma->vm_ops          = &nvkvm_vm_ops;
	vma->vm_private_data = region;
	return 0;
}

static int nvkvm_mmap_request_uvm_realize(struct nvkvm_fd_ctx *ctx,
					  struct vm_area_struct *vma);

int nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;

	/* Basic validation: size must be page-aligned and non-zero */
	if (!vma_len || (vma_len & ~PAGE_MASK))
		return -EINVAL;
	if (vma_len > SZ_1G)
		return -EINVAL;

	/*
	 * Open establishes ctx->handle_id and ctx->session->isolate_id; mmap
	 * with either missing is a logic bug, not a fallback.
	 */
	if (!ctx->handle_id || !ctx->session->isolate_id)
		return -EBADF;

	/* Step E plan: UVM mmap → REALIZE path.  Disabled until the
	 * realize-on-existing-fd refactor lands (the current fresh-fd
	 * design loses the RM↔UVM bindings the original fd built up). */
	(void)nvkvm_mmap_request_uvm_realize;

	return nvkvm_mmap_request_isolate(ctx, vma);
}

/*
 * UVM realize path (state-machine Step E).
 *
 * Build a state snapshot from ctx->uvm_state, pick the matching intent
 * by (gva, length), upload both into shm slots, and issue a single
 * REALIZE_UVM_MAPPING request.  QEMU does the privileged work and
 * returns a GPA we map into the VMA with remap_pfn_range.
 */
static int nvkvm_mmap_request_uvm_realize(struct nvkvm_fd_ctx *ctx,
					  struct vm_area_struct *vma)
{
	struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
	struct nvkvm_uvm_mapping_intent *m, *match = NULL;
	struct nvkvm_uvm_gpu_reg *g;
	struct nvkvm_uvm_vas_reg *v;
	struct nvkvm_uvm_range_group *r;
	struct nvkvm_uvm_state_snapshot *snap = NULL;
	struct nvkvm_uvm_realization *real = NULL;
	void *intent_slot_ptr = NULL;
	int state_slot = -1, intent_slot = -1;
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	__u64 gva    = vma->vm_start;
	__u32 prot   = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		       (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
	__u32 map_flags = MAP_SHARED;
	__u64 gpa_base = 0, realize_token = 0;
	__u32 rm_status = 0;
	__u32 intent_size = 0;
	int ret;

	mutex_lock(&st->lock);
	list_for_each_entry(m, &st->intents, list) {
		if (m->base == gva && m->length == vma_len) {
			match = m;
			break;
		}
	}
	if (!match) {
		mutex_unlock(&st->lock);
		pr_warn("nvkvm: UVM mmap %llx+%lx without matching intent\n",
			gva, vma_len);
		return -EINVAL;
	}

	state_slot = nvkvm_slot_alloc(&nvkvm);
	if (state_slot < 0) { ret = -ENOSPC; goto err_unlock; }
	intent_slot = nvkvm_slot_alloc(&nvkvm);
	if (intent_slot < 0) { ret = -ENOSPC; goto err_unlock; }

	snap = nvkvm_slot_addr(&nvkvm, state_slot);
	intent_slot_ptr = nvkvm_slot_addr(&nvkvm, intent_slot);
	if (!snap || !intent_slot_ptr ||
	    nvkvm.slot_size < sizeof(*snap) ||
	    nvkvm.slot_size < match->params_size) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	memset(snap, 0, sizeof(*snap));
	snap->init_flags = cpu_to_le64(st->init_flags);

	__u32 n_gpus = 0;
	list_for_each_entry(g, &st->registered_gpus, list) {
		if (n_gpus >= NVKVM_UVM_MAX_REG_GPUS) break;
		memcpy(snap->gpus[n_gpus].gpu_uuid, g->gpu_uuid, 16);
		n_gpus++;
	}
	snap->n_gpus = cpu_to_le32(n_gpus);

	__u32 n_vas = 0;
	list_for_each_entry(v, &st->registered_va_spaces, list) {
		if (n_vas >= NVKVM_UVM_MAX_VA_SPACES) break;
		memcpy(snap->va_spaces[n_vas].gpu_uuid, v->gpu_uuid, 16);
		snap->va_spaces[n_vas].rm_ctrl_fd_handle_id =
			cpu_to_le32(v->rm_ctrl_fd_handle_id);
		snap->va_spaces[n_vas].h_client   = cpu_to_le32(v->h_client);
		snap->va_spaces[n_vas].h_va_space = cpu_to_le32(v->h_va_space);
		n_vas++;
	}
	snap->n_va_spaces = cpu_to_le32(n_vas);

	__u32 n_rgs = 0;
	list_for_each_entry(r, &st->range_groups, list) {
		if (n_rgs >= NVKVM_UVM_MAX_RANGE_GROUPS) break;
		snap->range_group_ids[n_rgs] = cpu_to_le64(r->range_group_id);
		n_rgs++;
	}
	snap->n_range_groups = cpu_to_le32(n_rgs);

	memcpy(intent_slot_ptr, match->params, match->params_size);
	intent_size = (__u32)match->params_size;
	wmb();
	mutex_unlock(&st->lock);

	ret = nvkvm_virtio_realize_uvm_mapping(ctx->session->isolate_id,
					       ctx->handle_id,
					       NVKVM_UVM_REALIZE_MODE_SEM_POOL,
					       (unsigned int)ctx->session->id,
					       gva, vma_len, 0,
					       prot, map_flags,
					       (__u32)state_slot,
					       (__u32)intent_slot,
					       intent_size,
					       &gpa_base, &realize_token,
					       &rm_status);

	nvkvm_slot_free(&nvkvm, state_slot);
	state_slot = -1;
	nvkvm_slot_free(&nvkvm, intent_slot);
	intent_slot = -1;

	if (ret) {
		pr_warn("nvkvm: REALIZE_UVM_MAPPING failed: %d rm_status=0x%x\n",
			ret, rm_status);
		return ret;
	}
	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa_base, vma_len)) {
		pr_warn("nvkvm: REALIZE returned GPA %llx outside window\n",
			gpa_base);
		return -EIO;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa_base >> PAGE_SHIFT),
			      vma_len, vma->vm_page_prot);
	if (ret)
		return ret;

	real = kzalloc(sizeof(*real), GFP_KERNEL);
	if (real) {
		real->realize_token = realize_token;
		real->gva    = gva;
		real->gpa    = gpa_base;
		real->length = vma_len;
		mutex_lock(&st->lock);
		list_add_tail(&real->list, &st->realizations);
		mutex_unlock(&st->lock);
	}
	return 0;

err_unlock:
	if (state_slot  >= 0) nvkvm_slot_free(&nvkvm, state_slot);
	if (intent_slot >= 0) nvkvm_slot_free(&nvkvm, intent_slot);
	mutex_unlock(&st->lock);
	return ret;
}

/*
 * nvkvm_cpu_page_migrate — pin a guest physical page and upload it to a
 * memfd so the isolate can access it at the same GVA via MAP_FIXED.
 *
 * On first access the page is pinned, copied to QEMU via shared memory, and
 * mapped in the isolate.  Subsequent accesses to the same page_gva are no-ops
 * (the mapping is already live).
 */
int nvkvm_cpu_page_migrate(struct nvkvm_fd_ctx *ctx,
			   unsigned long page_gva, unsigned long prot)
{
	struct nvkvm_cpu_page *cp;
	struct page *page = NULL;
	int shm_slot = -1;
	void *slot_ptr;
	__u32 handle_id = 0;
	__u64 gpa_base;
	__u32 mmap_token;
	int ret;

	/* Already mapped? */
	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list) {
		if (cp->gva == page_gva) {
			mutex_unlock(&ctx->cpu_pages_lock);
			return 0;
		}
	}
	mutex_unlock(&ctx->cpu_pages_lock);

	/* Pin the physical page (write access so writeback can update it). */
	ret = get_user_pages_fast(page_gva, 1, FOLL_WRITE, &page);
	if (ret != 1) {
		pr_warn("nvkvm: cpu_page_migrate gup gva=0x%lx ret=%d\n",
			page_gva, ret);
		return (ret < 0) ? ret : -EFAULT;
	}

	/* Copy page content into a shared memory slot. */
	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0) { ret = -ENOSPC; goto err_page; }

	slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!slot_ptr || nvkvm.slot_size < PAGE_SIZE) {
		ret = -ENOMEM;
		goto err_slot;
	}

	{
		void *kaddr = kmap_local_page(page);
		memcpy(slot_ptr, kaddr, PAGE_SIZE);
		kunmap_local(kaddr);
	}
	wmb();

	/* Create a QEMU-side memfd for this page. */
	ret = nvkvm_virtio_open_memory_handle((unsigned int)ctx->session->id,
					      PAGE_SIZE, &handle_id);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate open_memory_handle gva=0x%lx ret=%d\n",
			page_gva, ret);
		goto err_slot;
	}

	/* Upload the page content to the memfd. */
	ret = nvkvm_virtio_write_memory_handle(handle_id, 0, shm_slot, PAGE_SIZE);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate write_memory_handle gva=0x%lx ret=%d\n",
			page_gva, ret);
		goto err_handle;
	}

	nvkvm_slot_free(&nvkvm, shm_slot);
	shm_slot = -1;

	/* Send the handle to the isolate and map it at page_gva. */
	ret = nvkvm_virtio_copy_handle_to_isolate(handle_id,
						  ctx->session->isolate_id);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate copy_handle_to_isolate gva=0x%lx isolate=%u ret=%d\n",
			page_gva, ctx->session->isolate_id, ret);
		goto err_handle;
	}

	ret = nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id, handle_id,
					   page_gva, 0, PAGE_SIZE,
					   prot, MAP_SHARED,
					   (unsigned int)ctx->session->id,
					   &gpa_base, &mmap_token);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate mmap_on_isolate gva=0x%lx isolate=%u ret=%d\n",
			page_gva, ctx->session->isolate_id, ret);
		goto err_handle;
	}

	/* Stash the GPA on the tracking node so a later range-swap can install
	 * it into libcuda's VMA in one shot.  Per-page VM_PFNMAP toggling on a
	 * VMA that still holds anon pages breaks gup_fast for the unmigrated
	 * neighbours, so we don't touch the guest VMA here. */

	/* Track the migration for writeback and cleanup at fd close. */
	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp) {
		/* Mapping is live; accept the tracking leak. */
		return 0;
	}
	cp->page        = page;
	cp->gva         = page_gva;
	cp->gpa         = gpa_base;
	cp->handle_id   = handle_id;
	cp->mmap_token  = mmap_token;
	cp->prot        = (__u32)prot;

	mutex_lock(&ctx->cpu_pages_lock);
	list_add_tail(&cp->list, &ctx->cpu_pages);
	mutex_unlock(&ctx->cpu_pages_lock);
	return 0;

err_handle:
	nvkvm_virtio_close_handle(handle_id);
err_slot:
	if (shm_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_slot);
err_page:
	put_page(page);
	return ret;
}

/*
 * nvkvm_efault_resolve — map a faulting GVA into the isolate after an EFAULT
 * from ioctl_on_isolate.
 *
 * GPU VMA (set up via MMAP_ON_ISOLATE): re-send the mmap command (handles
 * races where the isolate lost the mapping).
 *
 * CPU VMA (anonymous / file-backed, not GPU): pin the faulting page and upload
 * it to the isolate via a memfd (CPU userptr physical page migration).
 */
int nvkvm_efault_resolve(struct nvkvm_fd_ctx *ctx, __u64 fault_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long page_gva;
	unsigned long prot;
	int ret;

	if (!ctx->session->isolate_id)
		return -ENOENT;

	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)fault_addr);
	if (!vma || vma->vm_start > (unsigned long)fault_addr) {
		mmap_read_unlock(mm);
		return -EFAULT;
	}

	if (vma->vm_ops == &nvkvm_vm_ops && vma->vm_private_data) {
		/* GPU VMA — re-send MMAP_ON_ISOLATE */
		struct nvkvm_mmap_region *region = vma->vm_private_data;
		__u32 handle_id = region->handle_id;
		__u64 gva       = vma->vm_start;
		__u64 length    = vma->vm_end - vma->vm_start;
		__u64 offset    = region->offset;
		__u32 gprot     = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
				  (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
		mmap_read_unlock(mm);

		if (!handle_id)
			return -ENOENT;

		__u64 gpa_base;
		__u32 mmap_token;
		return nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id,
						    handle_id, gva, offset,
						    length, gprot, MAP_SHARED,
						    (unsigned int)ctx->session->id,
						    &gpa_base, &mmap_token);
	}

	/* CPU VMA — migrate the faulting page */
	page_gva = (unsigned long)fault_addr & PAGE_MASK;
	prot     = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		   (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
	mmap_read_unlock(mm);

	ret = nvkvm_cpu_page_migrate(ctx, page_gva, prot);
	return ret;
}

/*
 * nvkvm_cpu_pages_writeback — for every writable migrated CPU page, read the
 * current memfd content back into the original guest physical page.
 *
 * Called after ioctl_on_isolate so that DtoH copies written by the NVIDIA
 * driver into the isolate's mapping are reflected back to the guest.
 */
void nvkvm_cpu_pages_writeback(struct nvkvm_fd_ctx *ctx)
{
#define NVKVM_WB_BATCH  64
	struct { __u32 handle_id; struct page *page; } batch[NVKVM_WB_BATCH];
	int n = 0;
	struct nvkvm_cpu_page *cp;

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list) {
		if (!(cp->prot & PROT_WRITE) || n >= NVKVM_WB_BATCH)
			continue;
		get_page(cp->page);
		batch[n].handle_id = cp->handle_id;
		batch[n].page      = cp->page;
		n++;
	}
	mutex_unlock(&ctx->cpu_pages_lock);

	for (int i = 0; i < n; i++) {
		int shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0)
			goto put;

		if (nvkvm_virtio_read_memory_handle(batch[i].handle_id, 0,
						    shm_slot, PAGE_SIZE) == 0) {
			void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
			if (slot_ptr) {
				void *kaddr = kmap_local_page(batch[i].page);
				rmb();
				memcpy(kaddr, slot_ptr, PAGE_SIZE);
				kunmap_local(kaddr);
				set_page_dirty(batch[i].page);
			}
		}
		nvkvm_slot_free(&nvkvm, shm_slot);
put:
		put_page(batch[i].page);
	}
#undef NVKVM_WB_BATCH
}

/*
 * nvkvm_cpu_pages_free — clean up all CPU page migrations for this fd.
 * Unmaps each page from the isolate, closes the handle, and releases the pin.
 */
void nvkvm_cpu_pages_free(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_cpu_page *cp, *tmp;
	LIST_HEAD(to_free);
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;

	mutex_lock(&ctx->cpu_pages_lock);
	list_splice_init(&ctx->cpu_pages, &to_free);
	mutex_unlock(&ctx->cpu_pages_lock);

	list_for_each_entry_safe(cp, tmp, &to_free, list) {
		if (isolate_id) {
			nvkvm_virtio_munmap_on_isolate(isolate_id, cp->mmap_token);
			nvkvm_virtio_close_handle_on_isolate(cp->handle_id,
							     isolate_id);
		}
		nvkvm_virtio_close_handle(cp->handle_id);
		put_page(cp->page);
		list_del(&cp->list);
		kfree(cp);
	}
}

void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_mmap_region *region, *tmp;
	LIST_HEAD(to_free);
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;

	spin_lock(&ctx->mmap_lock);
	list_splice_init(&ctx->mmap_regions, &to_free);
	spin_unlock(&ctx->mmap_lock);

	/*
	 * The two send-sync call sites below previously built `umsg` on the
	 * stack and passed it to sg_init_one().  On CONFIG_VMAP_STACK kernels
	 * (Ubuntu's default) virt_to_page() returns a bogus physical page for
	 * vmapped stack, so QEMU's DMA read sees zeros — hdr.type lands as 0
	 * and the QEMU dispatch rejects it with "unknown request type 0".
	 * The whole virtio queue then deadlocks because the inflight record
	 * never completes.  Copy into a kmalloc'd buffer (same workaround
	 * simple_req uses).
	 */
	list_for_each_entry_safe(region, tmp, &to_free, list) {
		if (region->handle_id && isolate_id) {
			struct {
				struct nvkvm_hdr                   hdr;
				struct nvkvm_req_munmap_on_isolate req;
			} *umsg;
			struct nvkvm_inflight uinf;
			umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
			if (umsg) {
				umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
				umsg->hdr.txn_id     = cpu_to_le32(
						atomic_inc_return(&nvkvm.next_txn_id));
				umsg->req.isolate_id = cpu_to_le32(isolate_id);
				umsg->req.mmap_token = cpu_to_le32(region->mmap_token);
				init_completion(&uinf.done);
				uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
				nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
				kfree(umsg);
			}
		}
		list_del(&region->list);
		kfree(region);
	}
}

/*
 * nvkvm_gpa_in_mmap_window — check whether [base, base+len) falls inside
 * the dedicated mmap GPA window that the host allocated for us.
 * The window boundaries are populated during virtio init from the BAR.
 */
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len)
{
	/* The mmap window is exposed as a second memory region by the host;
	 * its start GPA and size are stored in nvkvm.mmap_window_* at init. */
	if (!nvkvm.mmap_window_gpa_base || !nvkvm.mmap_window_len)
		return false;
	if (gpa_base < nvkvm.mmap_window_gpa_base)
		return false;
	if (gpa_base + len > nvkvm.mmap_window_gpa_base +
			     nvkvm.mmap_window_len)
		return false;
	if (gpa_base + len < gpa_base)   /* overflow check */
		return false;
	return true;
}

/*
 * nvkvm_cpu_pages_migrate_range — migrate every guest page covering
 * [gva, gva+len) onto memfds shared with the isolate.  Pages already
 * migrated are skipped (cpu_page_migrate dedupes by gva).  Used to set
 * up the OS_DESCRIPTOR backing before forwarding NV_ESC_RM_ALLOC_MEMORY
 * so the kernel pin_user_pages call on the stub's task finds the same
 * physical pages libcuda is writing to in the guest.
 */
int nvkvm_cpu_pages_migrate_range(struct nvkvm_fd_ctx *ctx,
				  __u64 gva, __u64 len, unsigned long prot)
{
	unsigned long start = (unsigned long)gva & PAGE_MASK;
	unsigned long end   = ((unsigned long)gva + len + PAGE_SIZE - 1) &
			      PAGE_MASK;
	unsigned long off;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int ret;

	if (!len)
		return 0;
	if (end < start)   /* overflow */
		return -EINVAL;
	if (end - start > (16ULL << 20))   /* sanity: 16 MB max per call */
		return -E2BIG;

	/*
	 * Step 1: per-page migrate.  Pin each guest anon page, ship its
	 * content into a memfd in QEMU, MAP_FIXED the memfd at the same
	 * VA in the stub's mm, and record the (gva, gpa) in ctx->cpu_pages.
	 * Pages already migrated are no-ops.  No VMA mutation in the guest
	 * yet — that has to wait until every page has been gup'd, otherwise
	 * the first VM_PFNMAP toggle would block gup_fast on the remainder.
	 */
	for (off = start; off < end; off += PAGE_SIZE) {
		ret = nvkvm_cpu_page_migrate(ctx, off, prot);
		if (ret) {
			pr_warn("nvkvm: migrate_range pin gva=0x%lx ret=%d\n",
				off, ret);
			return ret;
		}
	}

	/*
	 * Step 2: single VMA swap.  Drop the anonymous PTEs in [start, end),
	 * mark the VMA VM_PFNMAP|VM_IO, and remap each page onto its memfd-
	 * backed GPA in our mmap window.  After this libcuda writes hit the
	 * memfd, which the stub already has mapped at the same VA via the
	 * step-1 MAP_FIXED — single physical backing across both processes
	 * and across the host kernel's pin_user_pages on the stub's mm.
	 */
	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start || vma->vm_end < end) {
		mmap_write_unlock(mm);
		pr_warn("nvkvm: migrate_range no VMA covering [%lx, %lx)\n",
			start, end);
		return -EFAULT;
	}

	zap_page_range_single(vma, start, end - start, NULL);
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	for (off = start; off < end; off += PAGE_SIZE) {
		struct nvkvm_cpu_page *cp;
		__u64 page_gpa = 0;

		mutex_lock(&ctx->cpu_pages_lock);
		list_for_each_entry(cp, &ctx->cpu_pages, list) {
			if (cp->gva == off) {
				page_gpa = cp->gpa;
				break;
			}
		}
		mutex_unlock(&ctx->cpu_pages_lock);
		if (!page_gpa) {
			mmap_write_unlock(mm);
			pr_warn("nvkvm: migrate_range no GPA for gva=0x%lx\n",
				off);
			return -EFAULT;
		}

		ret = remap_pfn_range(vma, off,
				      (unsigned long)(page_gpa >> PAGE_SHIFT),
				      PAGE_SIZE, vma->vm_page_prot);
		if (ret) {
			mmap_write_unlock(mm);
			pr_warn("nvkvm: migrate_range remap_pfn_range gva=0x%lx gpa=0x%llx ret=%d\n",
				off, page_gpa, ret);
			return ret;
		}
	}
	mmap_write_unlock(mm);
	return 0;
}
