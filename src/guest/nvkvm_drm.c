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
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>   /* #102: dumb buffers for KMS scanout */

#include "nvkvm.h"

#define NVKVM_PCI_VENDOR_NVIDIA 0x10de

#define NVKVM_DRM_COMMAND_BASE 0x40

/* ── GEM handle bridging ─────────────────────────────────────────────────────
 *
 * nvidia-private ioctls that mint GEM objects (e.g. SEMSURF_FENCE_CTX_CREATE)
 * run in the STUB's render-node DRM file, so the handle they return is valid in
 * the stub's GEM table — not the guest DRM core's.  But the Vulkan ICD then
 * uses that handle with DRM *core* ioctls (GEM_CLOSE, GEM_MAP_OFFSET) that the
 * guest's built-in DRM core resolves against the guest file's GEM table, where
 * it doesn't exist -> -ENOENT, and the ICD bails.
 *
 * Bridge the two namespaces: for every handle a forwarded ioctl returns, create
 * a lightweight proxy drm_gem_object in the guest file (no backing pages — it
 * never holds real memory; the hardware object lives in the stub).  The guest
 * IDR assigns its own handle, so we keep the stub handle in the proxy and
 * rewrite the returned handle to the guest one; calls that feed a handle back
 * to a forwarded ioctl translate guest->stub first.  GEM_CLOSE on the guest
 * frees the proxy and forwards a GEM_CLOSE(stub_handle) to release the real
 * object.  All translation is intra-VM (guest kernel owns GEM semantics); the
 * stub only ever sees its own handles. */
struct nvkvm_gem_object {
	struct drm_gem_object base;
	struct nvkvm_fd_ctx  *ctx;         /* isolate to forward GEM_CLOSE to */
	__u32                 stub_handle; /* handle in the stub's DRM file   */
};

#define to_nvkvm_gem(o) container_of(o, struct nvkvm_gem_object, base)

static void nvkvm_gem_free(struct drm_gem_object *obj)
{
	struct nvkvm_gem_object *ng = to_nvkvm_gem(obj);

	/* Release the real object in the stub.  Guest GEM handles are released
	 * before the driver's postclose runs, so ctx is still live here. */
	if (ng->ctx) {
		struct drm_gem_close close = { .handle = ng->stub_handle };
		__u64 fault = 0;

		nvkvm_virtio_ioctl_on_isolate(ng->ctx, DRM_IOCTL_GEM_CLOSE,
					      &close, sizeof(close),
					      NULL, 0, 0, &fault);
	}
	drm_gem_object_release(obj);
	kfree(ng);
}

static const struct drm_gem_object_funcs nvkvm_gem_funcs = {
	.free = nvkvm_gem_free,
};

/* Create a guest proxy GEM for a stub-side handle; returns the guest handle. */
static int nvkvm_gem_proxy_create(struct drm_file *file,
				  struct nvkvm_fd_ctx *ctx,
				  __u32 stub_handle, __u32 *guest_handle)
{
	struct nvkvm_gem_object *ng;
	int ret;

	ng = kzalloc(sizeof(*ng), GFP_KERNEL);
	if (!ng)
		return -ENOMEM;
	drm_gem_private_object_init(file->minor->dev, &ng->base, PAGE_SIZE);
	ng->base.funcs = &nvkvm_gem_funcs;
	/*
	 * Audit G-6 (latent): ng->ctx is cached WITHOUT a refcount.  Safe today
	 * because guest GEM handles are released before nvkvm_drm_postclose
	 * closes the ctx (normal drm_release ordering).  BUT once a dma-buf
	 * export / FLINK path lets a GEM object outlive its drm_file, a later
	 * nvkvm_gem_free would deref a freed ctx → guest-kernel UAF.  When the
	 * dma-buf present path (docs/design/virtual_modeset.md) wires export,
	 * take a ref on ctx here and drop it in nvkvm_gem_free.
	 */
	ng->ctx        = ctx;
	ng->stub_handle = stub_handle;
	ret = drm_gem_handle_create(file, &ng->base, guest_handle);
	/* The handle (or the proxy on failure) now owns the only ref. */
	drm_gem_object_put(&ng->base);
	return ret;
}

/* Resolve a guest GEM handle to the stub handle it proxies, or 0 if unknown. */
static __u32 nvkvm_gem_to_stub(struct drm_file *file, __u32 guest_handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, guest_handle);
	__u32 sh = 0;

	if (obj) {
		if (obj->funcs == &nvkvm_gem_funcs)
			sh = to_nvkvm_gem(obj)->stub_handle;
		drm_gem_object_put(obj);
	}
	return sh;
}

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

/*
 * SEMSURF_FENCE_CREATE takes fence_context_handle (a GEM handle from
 * CTX_CREATE) — translate the guest proxy handle to the stub's before
 * forwarding, restore after.  (fd@16 is an OUT sync fd; cross-boundary
 * sync-fd passback is a separate milestone.)
 */
static int nvkvm_drm_fwd_semsurf_fence_create(struct drm_device *dev,
					      void *data,
					      struct drm_file *file)
{
	struct drm_nvidia_semsurf_fence_create_params *p = data;
	struct nvkvm_fd_ctx *ctx = file->driver_priv;
	unsigned int cmd = DRM_IOWR(NVKVM_DRM_COMMAND_BASE + 0x15,
				    struct drm_nvidia_semsurf_fence_create_params);
	__u32 guest_h = p->fence_context_handle;
	__u32 stub_h;
	__u64 fault = 0;
	long r;

	(void)dev;
	if (!ctx)
		return -EBADF;
	stub_h = nvkvm_gem_to_stub(file, guest_h);
	if (stub_h)
		p->fence_context_handle = stub_h;

	r = nvkvm_virtio_ioctl_on_isolate(ctx, cmd, data, sizeof(*p),
					  NULL, 0, 0, &fault);

	p->fence_context_handle = guest_h;   /* round-trip the caller's handle */
	return (r < 0) ? (int)r : 0;
}

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
	if (r < 0)
		return (int)r;

	/*
	 * The stub created the fence-context GEM object in its own DRM file and
	 * wrote its handle to p->handle.  Mint a guest-core proxy GEM so the ICD's
	 * subsequent core GEM ioctls (GEM_CLOSE) resolve, and hand back the guest
	 * handle instead of the stub's.
	 */
	if (p->handle) {
		__u32 guest_handle = 0;
		int gret = nvkvm_gem_proxy_create(file, ctx, p->handle,
						  &guest_handle);
		if (gret)
			return gret;
		p->handle = guest_handle;
	}
	return 0;
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

const struct file_operations nvkvm_drm_fops = {   /* F-4: non-static for embedded-fd type-check */
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl   = drm_compat_ioctl,
	.poll           = drm_poll,
	.read           = drm_read,
	.mmap           = drm_gem_mmap,   /* #102: mmap dumb (shmem) scanout buffers */
	.llseek         = noop_llseek,
};

static const struct drm_driver nvkvm_drm_driver = {
	/* DRIVER_GEM: the DRM core only inits the per-file GEM object_idr (via
	 * drm_gem_open) and wires the core GEM ioctls (GEM_CLOSE, etc.) when this
	 * is set — required for our proxy GEM handles to resolve. */
	/* DRIVER_MODESET|ATOMIC: the guest-emulated virtual KMS head (#102,
	 * nvkvm_kms.c) lives on this same device so render + scanout share one DRM
	 * device (no cross-device PRIME). */
	.driver_features = DRIVER_RENDER | DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.open            = nvkvm_drm_open,
	.postclose       = nvkvm_drm_postclose,
	.ioctls          = nvkvm_drm_ioctls,
	.num_ioctls      = ARRAY_SIZE(nvkvm_drm_ioctls),
	.fops            = &nvkvm_drm_fops,
	/* #102: shmem-backed dumb buffers for the virtual KMS head's scanout
	 * (compositor/modetest fbs). Distinct from the proxy GEM render objects. */
	.dumb_create     = drm_gem_shmem_dumb_create,
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
	/* #102: bring up the guest-emulated virtual KMS head before the device
	 * goes live (mode objects must exist at register time). Non-fatal: on
	 * failure we still register as a render-only node. */
	ret = nvkvm_kms_init(ddev);
	if (ret)
		pr_warn("nvkvm: virtual KMS head init failed: %d (render-only)\n", ret);

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
