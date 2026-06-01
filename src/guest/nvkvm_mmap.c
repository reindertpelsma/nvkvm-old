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

#include <linux/ktime.h>
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
		/* Range entries (bulk migrate) have page==NULL: the guest VMA is
		 * remapped to the memfd GPA, so it reads the GPU's writes directly
		 * — no per-page writeback, and no page to deref. */
		if (!cp->page || !(cp->prot & PROT_WRITE) || n >= NVKVM_WB_BATCH)
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
		if (cp->page)            /* range entries are page==NULL */
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
/*
 * Bulk migration chunk size.  Each chunk = ONE memfd + ONE mmap_on_isolate +
 * ONE remap_pfn_range (vs the old per-4KB-page path: ~5 forwarded round-trips
 * EACH — measured 2.44s for a 16MB OS_DESCRIPTOR).  Chunking (rather than one
 * memfd for the whole range) bounds the transient memory: during migration the
 * data lives in BOTH the pinned guest pages AND the memfd, so a multi-GB single
 * memfd would need ~2x the RAM.  2MB amortizes the ~4 fixed per-chunk forwards
 * over 512 pages (negligible) while keeping the in-flight memfd small.
 */
#define NVKVM_MIG_CHUNK   (2UL << 20)
#define NVKVM_MIG_MAXCHK  8          /* 16MB cap / 2MB = 8 chunks max */

int nvkvm_cpu_pages_migrate_range(struct nvkvm_fd_ctx *ctx,
				  __u64 gva, __u64 len, unsigned long prot)
{
	unsigned long start = (unsigned long)gva & PAGE_MASK;
	unsigned long end   = ((unsigned long)gva + len + PAGE_SIZE - 1) &
			      PAGE_MASK;
	unsigned long range_len, npages, coff;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct page **pages = NULL;
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;
	size_t slot_bytes = nvkvm.slot_size;
	struct nvkvm_cpu_page *cpdup;
	long got = 0;
	int ret = 0, nck = 0, i;
	struct { unsigned long base, clen; __u64 gpa; __u32 handle, token; }
		ck[NVKVM_MIG_MAXCHK];
	ktime_t _t0 = ktime_get();   /* DIAG */

	if (!len)
		return 0;
	if (end < start)                  /* overflow */
		return -EINVAL;
	if (end - start > (16ULL << 20))  /* sanity: 16 MB max per call */
		return -E2BIG;
	if (!isolate_id)
		return -ENOENT;
	if (slot_bytes < PAGE_SIZE)
		return -ENOMEM;

	/* Dedup: if the start page is already migrated (range re-registered),
	 * the VMA is already VM_PFNMAP and gup would fail — treat as done. */
	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cpdup, &ctx->cpu_pages, list)
		if (cpdup->gva == start) { mutex_unlock(&ctx->cpu_pages_lock); return 0; }
	mutex_unlock(&ctx->cpu_pages_lock);

	/* A new registration changes the set of valid ranges — drop any cached
	 * VALIDATE results so a stale "valid" can't survive a free+remap. */
	nvkvm_session_vcache_clear(ctx->session);

	range_len = end - start;
	npages    = range_len >> PAGE_SHIFT;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* Pin every page up front — BEFORE any VMA mutation, so a later
	 * VM_PFNMAP toggle can't break gup_fast on the remainder. */
	while (got < npages) {
		long n = get_user_pages_fast(start + (got << PAGE_SHIFT),
					     npages - got, FOLL_WRITE, pages + got);
		if (n <= 0) { ret = (n < 0) ? (int)n : -EFAULT; goto err_unpin; }
		got += n;
	}

	/* Phase 1: one memfd + batched upload + copy + mmap per chunk.  No VMA
	 * mutation yet. */
	for (coff = 0; coff < range_len; coff += NVKVM_MIG_CHUNK) {
		unsigned long clen = min((unsigned long)NVKVM_MIG_CHUNK,
					 range_len - coff);
		__u32 handle = 0, token = 0;
		__u64 gpa = 0;
		unsigned long uoff;

		if (nck >= NVKVM_MIG_MAXCHK) { ret = -E2BIG; goto err_handles; }

		ret = nvkvm_virtio_open_memory_handle(
			(unsigned int)ctx->session->id, clen, &handle);
		if (ret)
			goto err_handles;

		/* Upload clen bytes in slot-sized batches (one forward per slot,
		 * vs one per 4KB page in the old path). */
		for (uoff = 0; uoff < clen; uoff += slot_bytes) {
			unsigned long this = min((unsigned long)slot_bytes,
						 clen - uoff);
			unsigned long p;
			int slot = nvkvm_slot_alloc(&nvkvm);
			void *sp;

			if (slot < 0) { ret = -ENOSPC; goto chunk_fail; }
			sp = nvkvm_slot_addr(&nvkvm, slot);
			if (!sp) { nvkvm_slot_free(&nvkvm, slot); ret = -ENOMEM; goto chunk_fail; }
			for (p = 0; p < this; p += PAGE_SIZE) {
				unsigned long pidx = (coff + uoff + p) >> PAGE_SHIFT;
				void *ka = kmap_local_page(pages[pidx]);
				memcpy((char *)sp + p, ka, PAGE_SIZE);
				kunmap_local(ka);
			}
			wmb();
			ret = nvkvm_virtio_write_memory_handle(handle, uoff, slot,
							       (__u32)this);
			nvkvm_slot_free(&nvkvm, slot);
			if (ret) goto chunk_fail;
			continue;
chunk_fail:
			nvkvm_virtio_close_handle(handle);
			goto err_handles;
		}

		ret = nvkvm_virtio_copy_handle_to_isolate(handle, isolate_id);
		if (ret) { nvkvm_virtio_close_handle(handle); goto err_handles; }

		ret = nvkvm_virtio_mmap_on_isolate(isolate_id, handle,
						   start + coff, 0, clen,
						   (__u32)prot, MAP_SHARED,
						   (unsigned int)ctx->session->id,
						   &gpa, &token);
		if (ret) {
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			nvkvm_virtio_close_handle(handle);
			goto err_handles;
		}
		if (!nvkvm_gpa_in_mmap_window(gpa, clen)) {
			nvkvm_virtio_munmap_on_isolate(isolate_id, token);
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			nvkvm_virtio_close_handle(handle);
			ret = -EIO; goto err_handles;
		}
		ck[nck].base = start + coff; ck[nck].clen = clen;
		ck[nck].gpa = gpa; ck[nck].handle = handle; ck[nck].token = token;
		nck++;
	}

	/* Phase 2: ONE VMA swap — zap the whole range, mark PFNMAP, remap each
	 * chunk's contiguous GPA.  After this libcuda reads/writes the memfd via
	 * the GPA window directly (the stub maps the same memfd) — no per-page
	 * writeback needed. */
	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start || vma->vm_end < end) {
		mmap_write_unlock(mm);
		ret = -EFAULT; goto err_handles;
	}
	zap_page_range_single(vma, start, range_len, NULL);
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	/*
	 * CACHED (write-back), NOT pgprot_noncached.  The GPA window is backed by
	 * a memfd — normal host RAM in a KVM RAM memslot — not real device MMIO.
	 * On x86 the guest's WB view, the stub's WB view of the same memfd, and
	 * the GPU's DMA are all cache-coherent (DMA snoops), so WB is correct.
	 * Mapping it UC made the guest's post-DtoH read of the result a stream of
	 * uncached, unprefetched loads — measured 0.07 GB/s vs 9.6 GB/s on the
	 * host (130x).  HtoD was unaffected because the guest fills the buffer
	 * while it is still cached anon memory (before this swap) and the stub
	 * then reads the memfd as host RAM — the guest never reads through the
	 * window on HtoD.  Leaving vm_page_prot at its default keeps it WB.
	 */
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	for (i = 0; i < nck; i++) {
		ret = remap_pfn_range(vma, ck[i].base,
				      (unsigned long)(ck[i].gpa >> PAGE_SHIFT),
				      ck[i].clen, vma->vm_page_prot);
		if (ret) { mmap_write_unlock(mm); goto err_handles; }
	}
	mmap_write_unlock(mm);

	/* The VMA now points at the GPAs (memfds); release the original anon
	 * page pins. */
	for (i = 0; i < got; i++)
		put_page(pages[i]);
	kvfree(pages);

	/* Track one range entry per chunk (page==NULL → no writeback, no
	 * put_page at cleanup; munmap_on_isolate(token)+close_handle suffice). */
	for (i = 0; i < nck; i++) {
		struct nvkvm_cpu_page *cp = kzalloc(sizeof(*cp), GFP_KERNEL);
		if (!cp)
			continue;   /* mapping live; accept tracking leak */
		cp->page       = NULL;
		cp->gva        = ck[i].base;
		cp->gpa        = ck[i].gpa;
		cp->handle_id  = ck[i].handle;
		cp->mmap_token = ck[i].token;
		cp->prot       = (__u32)prot;
		mutex_lock(&ctx->cpu_pages_lock);
		list_add_tail(&cp->list, &ctx->cpu_pages);
		mutex_unlock(&ctx->cpu_pages_lock);
	}
	pr_info("nvkvm DIAG: migrate_range(bulk) %lu pages, %d chunks in %lld us\n",
		npages, nck, ktime_to_us(ktime_sub(ktime_get(), _t0)));
	return 0;

err_handles:
	for (i = 0; i < nck; i++) {
		nvkvm_virtio_munmap_on_isolate(isolate_id, ck[i].token);
		nvkvm_virtio_close_handle_on_isolate(ck[i].handle, isolate_id);
		nvkvm_virtio_close_handle(ck[i].handle);
	}
err_unpin:
	for (i = 0; i < got; i++)
		put_page(pages[i]);
	kvfree(pages);
	pr_warn("nvkvm: migrate_range(bulk) failed ret=%d at chunk %d\n", ret, nck);
	return ret;
}
