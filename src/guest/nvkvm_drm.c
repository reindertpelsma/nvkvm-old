// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_drm.c — minimal nvidia-drm render-node emulation for the guest.
 *
 * The NVIDIA Vulkan/EGL userspace enumerates the GPU through the DRM render
 * node /dev/dri/renderD128, NOT through /dev/nvidia* (which carry compute).
 * The ICD stats the node, derives its rdev major, and requires
 * /sys/dev/char/<major>:128/device/drm to exist before opening it.  Both the
 * canonical DRM major (226) and that sysfs tree are owned by the kernel DRM
 * core and can only be obtained by registering a real DRM device — a raw cdev
 * cannot claim major 226.  So we register a render-only drm_driver here.
 *
 * Division of labour (mirrors the access-model split — guest kernel owns
 * intra-VM semantics, host owns the hardware):
 *   - VERSION              → answered by the DRM core from driver->name/date/...
 *                            (driver-constant: "nvidia-drm").
 *   - nvidia private ioctls (GET_DEV_INFO, DMABUF_SUPPORTED, …) → forwarded to
 *     the host /dev/dri/renderD128 through the SAME per-mm isolate the
 *     process's /dev/nvidia0 uses, so the returned gpu_id correlates with the
 *     RM device.  QEMU's DRM allowlist (default-deny) gates what reaches the
 *     host; the stub opens the host render node and runs the ioctl.
 *
 * Render-path GEM ioctls (IMPORT_USERSPACE_MEMORY / MAP_OFFSET / EXPORT_DMABUF)
 * are added in a later milestone with proper guest-VA marshalling (no raw guest
 * pointer ever reaches the VMM/stub).
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>

#include "nvkvm.h"

#define NVKVM_PCI_VENDOR_NVIDIA 0x10de

#define NVKVM_DRM_COMMAND_BASE 0x40

/* Param structs — sizes must match the host nvidia-drm-ioctl.h exactly so the
 * DRM core copies the right number of bytes in/out. */
struct drm_nvidia_get_dev_info_params {       /* 36 bytes, all scalars */
	__u32 gpu_id, mig_device, primary_index, supports_alloc;
	__u32 generic_page_kind, page_kind_generation, sector_layout;
	__u32 supports_sync_fd, supports_semsurf;
};

/* Semaphore-surface fence ioctls (render-path sync, #84). Sizes/layout MUST
 * match host nvidia-drm-ioctl.h so the DRM core copies the right byte count. */
struct drm_nvidia_semsurf_fence_ctx_create_params {  /* 32 bytes */
	__u64 index;             /* IN  */
	__u64 nvkms_params_ptr;  /* IN  user ptr to NVKMS import params */
	__u64 nvkms_params_size; /* IN  */
	__u32 handle;            /* OUT GEM handle to fence context */
	__u32 __pad;
};
struct drm_nvidia_semsurf_fence_create_params {      /* 24 bytes */
	__u32 fence_context_handle; /* IN  */
	__u32 timeout_value_ms;     /* IN  */
	__u64 wait_value;           /* IN  */
	__s32 fd;                   /* OUT sync fd */
	__u32 __pad;
};

/* Forward an already-kernel-copied DRM param blob to the host render node via
 * the process's isolate.  The DRM core handled the user<->kernel copy using
 * _IOC_SIZE(cmd); we just relay `data` and let the host write results back. */
static int nvkvm_drm_forward(struct drm_file *file, unsigned int cmd, void *data)
{
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	__u64 fault = 0;
	long r;

	if (!ctx)
		return -EBADF;
	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, _IOC_SIZE(cmd),
					  NULL, 0, 0, &fault);
	return (r < 0) ? (int)r : 0;
}

#define NVKVM_DRM_FWD(suffix, CMD)					\
	static int nvkvm_drm_fwd_##suffix(struct drm_device *dev,	\
					  void *data,			\
					  struct drm_file *file)	\
	{ (void)dev; return nvkvm_drm_forward(file, (CMD), data); }

NVKVM_DRM_FWD(get_dev_info,
	      DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x03,
		       struct drm_nvidia_get_dev_info_params))
NVKVM_DRM_FWD(dmabuf_supported, DRM_IO(NVKVM_DRM_COMMAND_BASE + 0x0f))
NVKVM_DRM_FWD(semsurf_fence_create,
	      DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x15,
		       struct drm_nvidia_semsurf_fence_create_params))

/*
 * SEMSURF_FENCE_CTX_CREATE embeds a userspace pointer `nvkms_params_ptr`
 * (-> nvkms_params_size bytes, IN only) that the host kernel reads.  The host
 * has no access to guest VAs, so stage those bytes in the aux slot, zero the
 * pointer (the stub substitutes a host VA at offset 8), and forward.  Mirrors
 * the RM_CONTROL / NVKMS aux pattern; handle@24 comes back inline in `data`.
 */
static int nvkvm_drm_fwd_semsurf_fence_ctx_create(struct drm_device *dev,
						  void *data,
						  struct drm_file *file)
{
	struct drm_nvidia_semsurf_fence_ctx_create_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x14,
				    struct drm_nvidia_semsurf_fence_ctx_create_params);
	void *aux = NULL;
	size_t aux_sz = 0;
	__u64 orig_ptr;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;

	orig_ptr = p->nvkms_params_ptr;
	if (orig_ptr && p->nvkms_params_size > 0 &&
	    p->nvkms_params_size <= NVKVM_SHM_SLOT_DEFAULT_SIZE) {
		aux_sz = p->nvkms_params_size;
		aux = kzalloc(aux_sz, GFP_KERNEL);
		if (!aux)
			return -ENOMEM;
		if (copy_from_user(aux, (void __user *)(uintptr_t)orig_ptr,
				   aux_sz)) {
			kfree(aux);
			return -EFAULT;
		}
		p->nvkms_params_ptr = 0;   /* stub fills host VA at offset 8 */
	}

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data,
					  sizeof(*p), aux, aux_sz, 0, &fault);

	/* Restore the caller's ptr; the kernel only reads it (IN). */
	p->nvkms_params_ptr = orig_ptr;
	kfree(aux);
	return (r < 0) ? (int)r : 0;
}

/* Indexed by (DRM_NVIDIA_* number) = (nr - DRM_COMMAND_BASE).  Gaps have a NULL
 * .func, which the DRM core rejects with -EINVAL (default-deny here too). */
static const struct drm_ioctl_desc nvkvm_drm_ioctls[] = {
	[0x03] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x03,
				   struct drm_nvidia_get_dev_info_params),
		   .func = nvkvm_drm_fwd_get_dev_info,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_GET_DEV_INFO" },
	[0x0f] = { .cmd = DRM_IO(NVKVM_DRM_COMMAND_BASE + 0x0f),
		   .func = nvkvm_drm_fwd_dmabuf_supported,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_DMABUF_SUPPORTED" },
	[0x14] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x14,
				   struct drm_nvidia_semsurf_fence_ctx_create_params),
		   .func = nvkvm_drm_fwd_semsurf_fence_ctx_create,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_SEMSURF_FENCE_CTX_CREATE" },
	[0x15] = { .cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x15,
				   struct drm_nvidia_semsurf_fence_create_params),
		   .func = nvkvm_drm_fwd_semsurf_fence_create,
		   .flags = DRM_RENDER_ALLOW, .name = "NVIDIA_SEMSURF_FENCE_CREATE" },
};

/* Each open of the render node gets its own forwarding context, sharing the
 * process's per-mm session + isolate (so renderD128 ⇄ /dev/nvidia0 correlate). */
static int nvkvm_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct nvkvm_fd_ctx *ctx;

	(void)dev;
	ctx = nvkvm_fd_ctx_open_dev(NVKVM_DEV_DRM_RD(0), O_RDWR);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	file->driver_priv = ctx;
	return 0;
}

static void nvkvm_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct nvkvm_fd_ctx *ctx = file->driver_priv;

	(void)dev;
	if (ctx) {
		nvkvm_fd_ctx_close(ctx);
		file->driver_priv = NULL;
	}
}

static const struct file_operations nvkvm_drm_fops = {
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl   = drm_compat_ioctl,
	.poll           = drm_poll,
	.read           = drm_read,
	.llseek         = noop_llseek,
};

static const struct drm_driver nvkvm_drm_driver = {
	.driver_features = DRIVER_RENDER,
	.open            = nvkvm_drm_open,
	.postclose       = nvkvm_drm_postclose,
	.ioctls          = nvkvm_drm_ioctls,
	.num_ioctls      = ARRAY_SIZE(nvkvm_drm_ioctls),
	.fops            = &nvkvm_drm_fops,
	/* VERSION values — driver-constant, verified against host nvidia-drm. */
	.name            = "nvidia-drm",
	.desc            = "NVIDIA DRM driver",
	.date            = "20160202",
	.major           = 0,
	.minor           = 0,
	.patchlevel      = 0,
};

/* The NVIDIA Vulkan ICD requires the GPU's PCI device to be bound to the
 * "nvidia" kernel driver (it reads DRIVER=nvidia from the device's uevent).
 * Our emulated identity device has no real hardware, so this driver only CLAIMS
 * the device — it touches no registers/BARs; all GPU I/O is forwarded via the
 * virtio device + GPA window.  Naming it "nvidia" makes the uevent match the
 * host exactly. */
static int nvkvm_gpu_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	(void)id;
	pci_set_drvdata(pdev, NULL);
	pci_info(pdev, "nvkvm: claimed NVIDIA-id device for DRM identity (no HW access)\n");
	return 0;
}

static void nvkvm_gpu_pci_remove(struct pci_dev *pdev)
{
	(void)pdev;
}

static const struct pci_device_id nvkvm_gpu_pci_ids[] = {
	{ PCI_DEVICE(NVKVM_PCI_VENDOR_NVIDIA, PCI_ANY_ID) },
	{ 0 }
};

static struct pci_driver nvkvm_gpu_pci_driver = {
	.name     = "nvidia",
	.id_table = nvkvm_gpu_pci_ids,
	.probe    = nvkvm_gpu_pci_probe,
	.remove   = nvkvm_gpu_pci_remove,
};
static bool nvkvm_gpu_pci_registered;

/* Register the render node under the emulated NVIDIA PCI device (vendor 0x10DE)
 * as parent, so the DRM core builds /sys/.../<nvidia-pci>/drm/renderD128 and the
 * ICD's renderD128 -> device -> vendor walk sees 0x10DE.  QEMU exposes that
 * identity-only PCI device (nvkvm-gpu).  If it is absent, fall back to the
 * virtio device as parent (node still registers, but the ICD won't bind it).
 * Graphics is optional: failure is logged, not fatal. */
int nvkvm_drm_init(struct device *fallback_parent)
{
	struct pci_dev *pdev;
	struct device *parent;
	struct drm_device *ddev;
	int ret;

	/* Bind the emulated NVIDIA-id device to the "nvidia" driver first, so it
	 * is bound (DRIVER=nvidia) by the time we parent the render node to it.
	 * pci_register_driver probes matching devices synchronously. */
	ret = pci_register_driver(&nvkvm_gpu_pci_driver);
	if (ret)
		pr_warn("nvkvm: pci_register_driver(nvidia) failed: %d\n", ret);
	else
		nvkvm_gpu_pci_registered = true;

	pdev = pci_get_device(NVKVM_PCI_VENDOR_NVIDIA, PCI_ANY_ID, NULL);
	parent = pdev ? &pdev->dev : fallback_parent;
	if (!pdev)
		pr_warn("nvkvm: no NVIDIA-id PCI device found; DRM parent falls back "
			"to virtio (Vulkan ICD will not bind)\n");

	ddev = drm_dev_alloc(&nvkvm_drm_driver, parent);
	if (IS_ERR(ddev)) {
		pr_warn("nvkvm: drm_dev_alloc failed: %ld (graphics disabled)\n",
			PTR_ERR(ddev));
		pci_dev_put(pdev);
		return PTR_ERR(ddev);
	}
	ret = drm_dev_register(ddev, 0);
	if (ret) {
		pr_warn("nvkvm: drm_dev_register failed: %d (graphics disabled)\n",
			ret);
		drm_dev_put(ddev);
		pci_dev_put(pdev);
		return ret;
	}
	nvkvm.drm_dev = ddev;
	/* The drm_device now holds its own reference on the parent; drop ours. */
	pci_dev_put(pdev);
	pr_info("nvkvm: registered nvidia-drm render node under %s (primary minor %d)\n",
		dev_name(parent),
		ddev->primary ? ddev->primary->index : -1);
	return 0;
}

void nvkvm_drm_fini(void)
{
	if (nvkvm.drm_dev) {
		drm_dev_unregister(nvkvm.drm_dev);
		drm_dev_put(nvkvm.drm_dev);
		nvkvm.drm_dev = NULL;
	}
	if (nvkvm_gpu_pci_registered) {
		pci_unregister_driver(&nvkvm_gpu_pci_driver);
		nvkvm_gpu_pci_registered = false;
	}
}
