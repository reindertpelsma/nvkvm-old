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

#include "nvkvm.h"

/* Forward declarations for module-internal functions */
static void nvkvm_vma_open(struct vm_area_struct *vma);
static void nvkvm_vma_close(struct vm_area_struct *vma);
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len);

static const struct vm_operations_struct nvkvm_vm_ops = {
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

int nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	struct {
		struct nvkvm_hdr      hdr;
		struct nvkvm_req_mmap req;
	} *msg;
	struct nvkvm_inflight *inf;
	struct nvkvm_mmap_region *region;
	struct nvkvm_resp_mmap resp;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	unsigned long gpa_base;
	int ret;

	/* Basic validation: size must be page-aligned and non-zero */
	if (!vma_len || (vma_len & ~PAGE_MASK))
		return -EINVAL;
	if (vma_len > SZ_1G)          /* sanity cap */
		return -EINVAL;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}
	init_completion(&inf->done);
	inf->req_id = req_id;

	msg->hdr.type    = cpu_to_le32(NVKVM_REQ_MMAP);
	msg->hdr.req_id  = cpu_to_le32(req_id);
	msg->req.fd_token = cpu_to_le32(ctx->fd_token);
	/* Translate kernel VM_* flags to userspace PROT_* for the host mmap.
	 * VM_READ/WRITE/EXEC == PROT_READ/WRITE/EXEC numerically on x86. */
	msg->req.prot     = cpu_to_le32(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC));
	msg->req.flags    = cpu_to_le32(vma->vm_flags & (VM_SHARED | VM_MAYSHARE));
	msg->req.offset   = cpu_to_le64((u64)vma->vm_pgoff << PAGE_SHIFT);
	msg->req.length   = cpu_to_le64(vma_len);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret)
		goto out_msg;

	resp.status   = inf->status;
	resp.gpa_base = inf->retval;
	resp.length   = vma_len;   /* host should match our request */

	if (resp.status) {
		ret = -(int)resp.status;
		goto out_msg;
	}

	gpa_base = (unsigned long)resp.gpa_base;

	/*
	 * Validate the returned GPA. The host places mmap regions in a
	 * dedicated GPA window (BAR 1) that is known at init time.
	 * We check that [gpa_base, gpa_base+vma_len) falls within it.
	 */
	if (!nvkvm_gpa_in_mmap_window(gpa_base, vma_len)) {
		pr_warn("nvkvm: host returned GPA %lx outside mmap window\n",
			gpa_base);
		ret = -EIO;
		goto out_unmap;
	}

	/* Set caching attributes: write-combine for framebuffer/BAR pages */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start, gpa_base >> PAGE_SHIFT,
			      vma_len, vma->vm_page_prot);
	if (ret)
		goto out_unmap;

	/* Track the mapping so we can munmap on fd close */
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region) {
		/* mapping already live; can't roll back cleanly — keep it */
		goto out_msg;
	}
	region->mmap_token = resp.mmap_token;
	region->gpa_base   = gpa_base;
	region->length     = vma_len;
	region->vma        = vma;

	spin_lock(&ctx->mmap_lock);
	list_add_tail(&region->list, &ctx->mmap_regions);
	spin_unlock(&ctx->mmap_lock);

	vma->vm_ops          = &nvkvm_vm_ops;
	vma->vm_private_data = region;

	kfree(inf);
	kfree(msg);
	return 0;

out_unmap:
	/* Ask host to unmap the region it just set up */
	{
		struct {
			struct nvkvm_hdr        hdr;
			struct nvkvm_req_munmap req;
		} umsg = {
			.hdr.type    = cpu_to_le32(NVKVM_REQ_MUNMAP),
			.hdr.req_id  = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_req_id)),
			.req.mmap_token = cpu_to_le32(resp.mmap_token),
		};
		struct nvkvm_inflight uinf;
		init_completion(&uinf.done);
		uinf.req_id = le32_to_cpu(umsg.hdr.req_id);
		nvkvm_send_sync(&nvkvm, &umsg, sizeof(umsg), &uinf);
	}
out_msg:
	kfree(inf);
	kfree(msg);
	return ret;
}

void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_mmap_region *region, *tmp;
	LIST_HEAD(to_free);

	spin_lock(&ctx->mmap_lock);
	list_splice_init(&ctx->mmap_regions, &to_free);
	spin_unlock(&ctx->mmap_lock);

	list_for_each_entry_safe(region, tmp, &to_free, list) {
		/* Send munmap request to host */
		struct {
			struct nvkvm_hdr        hdr;
			struct nvkvm_req_munmap req;
		} umsg = {
			.hdr.type    = cpu_to_le32(NVKVM_REQ_MUNMAP),
			.hdr.req_id  = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_req_id)),
			.req.mmap_token = cpu_to_le32(region->mmap_token),
		};
		struct nvkvm_inflight uinf;
		init_completion(&uinf.done);
		uinf.req_id = le32_to_cpu(umsg.hdr.req_id);
		nvkvm_send_sync(&nvkvm, &umsg, sizeof(umsg), &uinf);

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
