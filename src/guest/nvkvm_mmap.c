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

int nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	unsigned long gpa_base;
	struct nvkvm_mmap_region *region;
	int ret;

	/* Basic validation: size must be page-aligned and non-zero */
	if (!vma_len || (vma_len & ~PAGE_MASK))
		return -EINVAL;
	if (vma_len > SZ_1G)
		return -EINVAL;

	/* Prefer the new isolate path when available */
	if (ctx->handle_id && ctx->session->isolate_id)
		return nvkvm_mmap_request_isolate(ctx, vma);

	/* ── Legacy path ─────────────────────────────────────────────────── */
	struct {
		struct nvkvm_hdr      hdr;
		struct nvkvm_req_mmap req;
	} *msg;
	struct nvkvm_inflight *inf;
	struct nvkvm_resp_mmap resp;
	__u32 txn_id = atomic_inc_return(&nvkvm.next_txn_id);

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}
	init_completion(&inf->done);
	inf->txn_id = txn_id;

	msg->hdr.type     = cpu_to_le32(NVKVM_REQ_MMAP);
	msg->hdr.txn_id   = cpu_to_le32(txn_id);
	msg->req.fd_token = cpu_to_le32(ctx->fd_token);
	msg->req.prot     = cpu_to_le32(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC));
	msg->req.flags    = cpu_to_le32(vma->vm_flags & (VM_SHARED | VM_MAYSHARE));
	msg->req.offset   = cpu_to_le64((u64)vma->vm_pgoff << PAGE_SHIFT);
	msg->req.length   = cpu_to_le64(vma_len);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret)
		goto out_msg;

	resp.status   = inf->status;
	resp.gpa_base = inf->retval;

	if (resp.status) {
		ret = -(int)resp.status;
		goto out_msg;
	}

	gpa_base = (unsigned long)resp.gpa_base;

	if (!nvkvm_gpa_in_mmap_window(gpa_base, vma_len)) {
		pr_warn("nvkvm: host returned GPA %lx outside mmap window\n",
			gpa_base);
		ret = -EIO;
		goto out_unmap_legacy;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start, gpa_base >> PAGE_SHIFT,
			      vma_len, vma->vm_page_prot);
	if (ret)
		goto out_unmap_legacy;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		goto out_msg;

	region->mmap_token = resp.mmap_token;
	region->gpa_base   = gpa_base;
	region->length     = vma_len;
	region->offset     = (u64)vma->vm_pgoff << PAGE_SHIFT;
	region->vma        = vma;

	spin_lock(&ctx->mmap_lock);
	list_add_tail(&region->list, &ctx->mmap_regions);
	spin_unlock(&ctx->mmap_lock);

	vma->vm_ops          = &nvkvm_vm_ops;
	vma->vm_private_data = region;

	kfree(inf);
	kfree(msg);
	return 0;

out_unmap_legacy: {
		struct {
			struct nvkvm_hdr        hdr;
			struct nvkvm_req_munmap req;
		} *umsg;
		struct nvkvm_inflight uinf;
		umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
		if (umsg) {
			umsg->hdr.type    = cpu_to_le32(NVKVM_REQ_MUNMAP);
			umsg->hdr.txn_id  = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_txn_id));
			umsg->req.mmap_token = cpu_to_le32(resp.mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			kfree(umsg);
		}
	}
out_msg:
	kfree(inf);
	kfree(msg);
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
static int nvkvm_cpu_page_migrate(struct nvkvm_fd_ctx *ctx,
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
	if (ret != 1)
		return (ret < 0) ? ret : -EFAULT;

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
	if (ret)
		goto err_slot;

	/* Upload the page content to the memfd. */
	ret = nvkvm_virtio_write_memory_handle(handle_id, 0, shm_slot, PAGE_SIZE);
	if (ret)
		goto err_handle;

	nvkvm_slot_free(&nvkvm, shm_slot);
	shm_slot = -1;

	/* Send the handle to the isolate and map it at page_gva. */
	ret = nvkvm_virtio_copy_handle_to_isolate(handle_id,
						  ctx->session->isolate_id);
	if (ret)
		goto err_handle;

	ret = nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id, handle_id,
					   page_gva, 0, PAGE_SIZE,
					   prot, MAP_SHARED,
					   (unsigned int)ctx->session->id,
					   &gpa_base, &mmap_token);
	if (ret)
		goto err_handle;

	/* Track the migration for writeback and cleanup at fd close. */
	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp) {
		/* Mapping is live; accept the tracking leak. */
		return 0;
	}
	cp->page        = page;
	cp->gva         = page_gva;
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
			if (!umsg) goto next;
			umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
			umsg->hdr.txn_id     = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_txn_id));
			umsg->req.isolate_id = cpu_to_le32(isolate_id);
			umsg->req.mmap_token = cpu_to_le32(region->mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			kfree(umsg);
		} else {
			struct {
				struct nvkvm_hdr        hdr;
				struct nvkvm_req_munmap req;
			} *umsg;
			struct nvkvm_inflight uinf;
			umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
			if (!umsg) goto next;
			umsg->hdr.type    = cpu_to_le32(NVKVM_REQ_MUNMAP);
			umsg->hdr.txn_id  = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_txn_id));
			umsg->req.mmap_token = cpu_to_le32(region->mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			kfree(umsg);
		}
next:
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
