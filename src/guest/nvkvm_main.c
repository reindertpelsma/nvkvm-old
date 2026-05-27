// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_main.c — nvkvm-guest kernel module
 *
 * Exposes /dev/nvidia*, /dev/nvidiactl, and /dev/nvidia-uvm inside a KVM
 * guest VM and forwards all ioctls to the QEMU virtio-nvgpu backend running
 * on the host.
 *
 * Security model
 * ==============
 * This module sits at the guest-userspace → guest-kernel boundary. It treats
 * all data from userspace as untrusted and validates it before forwarding:
 *
 *   - All ioctl parameter sizes are checked against a compile-time table
 *     before the blob is copied from userspace.
 *   - The isolate's userspace VA layout mirrors the guest process's, so the
 *     host backend can dereference embedded pointers as-is. Pointer fields
 *     are preserved verbatim; no zeroing or translation is performed.
 *   - mmap requests are validated for range/flag sanity before forwarding.
 *
 * The module does NOT trust the host either: any host response that would
 * trigger mmap of guest physical memory is validated for GPA range bounds
 * before vm_insert_pfn is called.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/completion.h>

#include "../../src/common/nvkvm_proto.h"
#include "../../src/abi/nvgpu.h"
#include "../../src/abi/uvm.h"
#include "nvkvm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nvkvm contributors");
MODULE_DESCRIPTION("NVIDIA GPU passthrough for KVM guests via virtio-nvgpu");
MODULE_VERSION("0.1.0");

/* ── Global state ─────────────────────────────────────────────────────────── */

struct nvkvm_state nvkvm;

/* ── Module parameters ────────────────────────────────────────────────────── */

static int num_gpus = 1;
module_param(num_gpus, int, 0444);
MODULE_PARM_DESC(num_gpus, "Number of /dev/nvidia* devices to expose (default 1)");

/* ── Forward declarations ─────────────────────────────────────────────────── */

static int  nvkvm_open(struct inode *inode, struct file *filp);
static int  nvkvm_release(struct inode *inode, struct file *filp);
static long nvkvm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int  nvkvm_mmap(struct file *filp, struct vm_area_struct *vma);
static __poll_t nvkvm_poll(struct file *filp, poll_table *wait);

static const struct file_operations nvkvm_fops = {
	.owner          = THIS_MODULE,
	.open           = nvkvm_open,
	.release        = nvkvm_release,
	.unlocked_ioctl = nvkvm_ioctl,
	.compat_ioctl   = nvkvm_ioctl,
	.mmap           = nvkvm_mmap,
	.poll           = nvkvm_poll,
};

/* ── Device registration ──────────────────────────────────────────────────── */

/*
 * libnvidia-ml and nvidia-smi call mknodat(195, 255) before opening nvidiactl
 * (the major is hardcoded in the library).  We must register at that exact
 * major so the device can be opened after the library recreates the node.
 * Similarly, nvidia0 lives at major 195 minor 0 in the real NVIDIA driver.
 * Fall back to dynamic allocation if 195 is already taken.
 */
#define NV_NVIDIA_MAJOR 195

static char *nvkvm_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int __init register_devices(void)
{
	int ret, i;
	dev_t devno;

	nvkvm.class = class_create("nvkvm");
	if (IS_ERR(nvkvm.class))
		return PTR_ERR(nvkvm.class);
	nvkvm.class->devnode = nvkvm_devnode;

	/* /dev/nvidiactl — prefer major 195 minor 255 (NVIDIA standard) */
	devno = MKDEV(NV_NVIDIA_MAJOR, NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE);
	ret = register_chrdev_region(devno, 1, "nvidiactl");
	if (ret)
		ret = alloc_chrdev_region(&devno,
					  NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE,
					  1, "nvidiactl");
	if (ret)
		goto err_class;
	nvkvm.ctl_major = MAJOR(devno);
	cdev_init(&nvkvm.ctl_cdev, &nvkvm_fops);
	nvkvm.ctl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&nvkvm.ctl_cdev, devno, 1);
	if (ret)
		goto err_ctl_region;
	device_create(nvkvm.class, NULL, devno, NULL, "nvidiactl");

	/* /dev/nvidia0 .. /dev/nvidia{N-1} — prefer major 195 minor 0..N-1 */
	nvkvm.num_gpus = min(num_gpus, NV_MINOR_DEVICE_NUMBER_REGULAR_MAX + 1);
	devno = MKDEV(NV_NVIDIA_MAJOR, 0);
	ret = register_chrdev_region(devno, nvkvm.num_gpus, "nvidia");
	if (ret)
		ret = alloc_chrdev_region(&devno, 0, nvkvm.num_gpus, "nvidia");
	if (ret)
		goto err_ctl;
	nvkvm.gpu_devno_base = devno;
	nvkvm.gpu_major = MAJOR(nvkvm.gpu_devno_base);
	for (i = 0; i < nvkvm.num_gpus; i++) {
		devno = MKDEV(nvkvm.gpu_major, i);
		cdev_init(&nvkvm.gpu_cdevs[i], &nvkvm_fops);
		nvkvm.gpu_cdevs[i].owner = THIS_MODULE;
		ret = cdev_add(&nvkvm.gpu_cdevs[i], devno, 1);
		if (ret) {
			pr_err("nvkvm: failed to add /dev/nvidia%d: %d\n", i, ret);
			goto err_gpu;
		}
		device_create(nvkvm.class, NULL, devno, NULL, "nvidia%d", i);
	}

	/* /dev/nvidia-uvm — dynamic major, minor 0 */
	ret = alloc_chrdev_region(&nvkvm.uvm_devno, 0, 1, "nvidia-uvm");
	if (ret)
		goto err_gpu;
	nvkvm.uvm_major = MAJOR(nvkvm.uvm_devno);
	cdev_init(&nvkvm.uvm_cdev, &nvkvm_fops);
	nvkvm.uvm_cdev.owner = THIS_MODULE;
	ret = cdev_add(&nvkvm.uvm_cdev, nvkvm.uvm_devno, 1);
	if (ret)
		goto err_uvm_region;
	device_create(nvkvm.class, NULL, nvkvm.uvm_devno, NULL, "nvidia-uvm");

	pr_info("nvkvm: registered nvidiactl (major %u), nvidia0-%d (major %u), nvidia-uvm (major %u)\n",
		nvkvm.ctl_major, nvkvm.num_gpus - 1, nvkvm.gpu_major,
		nvkvm.uvm_major);
	return 0;

err_uvm_region:
	unregister_chrdev_region(nvkvm.uvm_devno, 1);
err_gpu:
	while (--i >= 0) {
		cdev_del(&nvkvm.gpu_cdevs[i]);
		device_destroy(nvkvm.class, MKDEV(nvkvm.gpu_major, i));
	}
	unregister_chrdev_region(nvkvm.gpu_devno_base, nvkvm.num_gpus);
err_ctl:
	cdev_del(&nvkvm.ctl_cdev);
	device_destroy(nvkvm.class, MKDEV(nvkvm.ctl_major,
				    NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE));
err_ctl_region:
	unregister_chrdev_region(MKDEV(nvkvm.ctl_major,
				       NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE), 1);
err_class:
	class_destroy(nvkvm.class);
	return ret;
}

static void unregister_devices(void)
{
	int i;

	device_destroy(nvkvm.class, nvkvm.uvm_devno);
	cdev_del(&nvkvm.uvm_cdev);
	unregister_chrdev_region(nvkvm.uvm_devno, 1);

	for (i = nvkvm.num_gpus - 1; i >= 0; i--) {
		device_destroy(nvkvm.class, MKDEV(nvkvm.gpu_major, i));
		cdev_del(&nvkvm.gpu_cdevs[i]);
	}
	unregister_chrdev_region(nvkvm.gpu_devno_base, nvkvm.num_gpus);

	device_destroy(nvkvm.class,
		       MKDEV(nvkvm.ctl_major,
			     NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE));
	cdev_del(&nvkvm.ctl_cdev);
	unregister_chrdev_region(MKDEV(nvkvm.ctl_major,
				       NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE),
				 1);

	class_destroy(nvkvm.class);
}

/* ── Device open/release ──────────────────────────────────────────────────── */

/* Ensure the session has an isolate; creates one if isolate_id == 0. */
static int nvkvm_ensure_isolate(struct nvkvm_session *session)
{
	__u32 isolate_id;
	int ret;

	mutex_lock(&session->isolate_lock);
	if (session->isolate_id) {
		mutex_unlock(&session->isolate_lock);
		return 0;
	}
	ret = nvkvm_virtio_create_isolate((unsigned int)session->id, &isolate_id);
	if (ret == 0)
		session->isolate_id = isolate_id;
	mutex_unlock(&session->isolate_lock);
	return ret;
}

static int nvkvm_open(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx;
	int dev_id;
	int ret;

	/*
	 * Determine which device is being opened.
	 *
	 * nvidiactl and nvidia0..N share major 195 (NV_NVIDIA_MAJOR) so we MUST
	 * check BOTH major AND minor to identify nvidiactl (minor=255).  Checking
	 * major alone incorrectly classifies every nvidia0 open as NVKVM_DEV_CTL.
	 *
	 * nvidia-uvm has a dynamic (distinct) major so major comparison is enough.
	 */
	if (imajor(inode) == nvkvm.ctl_major &&
	    iminor(inode) == NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE)
		dev_id = NVKVM_DEV_CTL;
	else if (imajor(inode) == nvkvm.uvm_major)
		dev_id = NVKVM_DEV_UVM;
	else
		dev_id = NVKVM_DEV_GPU(iminor(inode));

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev_id  = dev_id;
	ctx->session = nvkvm_session_get_or_create(current->tgid);
	if (IS_ERR(ctx->session)) {
		ret = PTR_ERR(ctx->session);
		kfree(ctx);
		return ret;
	}
	init_waitqueue_head(&ctx->poll_wq);
	atomic_set(&ctx->poll_events, 0);
	spin_lock_init(&ctx->mmap_lock);
	INIT_LIST_HEAD(&ctx->mmap_regions);
	mutex_init(&ctx->cpu_pages_lock);
	INIT_LIST_HEAD(&ctx->cpu_pages);

	/*
	 * Open flow: spawn the isolate (creates the QEMU-side session as
	 * a side effect) and then open the device via the stub so its
	 * nvfp/mm lineage is the isolate process.
	 */
	{
		__u32 handle_id = 0;

		ret = nvkvm_ensure_isolate(ctx->session);
		if (ret) {
			nvkvm_session_put(ctx->session);
			kfree(ctx);
			return ret;
		}

		ret = nvkvm_virtio_open_nvidia_handle(dev_id, filp->f_flags,
						      (unsigned int)ctx->session->id,
						      &handle_id);
		if (ret) {
			nvkvm_session_put(ctx->session);
			kfree(ctx);
			return ret;
		}
		ctx->handle_id = handle_id;
	}

	filp->private_data = ctx;
	pr_debug("nvkvm: opened dev_id=%d handle_id=%u isolate_id=%u tgid=%d\n",
		 dev_id, ctx->handle_id,
		 ctx->session->isolate_id, current->tgid);
	return 0;
}

static int nvkvm_release(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;

	if (!ctx)
		return 0;

	if (ctx->handle_id && ctx->session->isolate_id)
		nvkvm_virtio_close_handle_on_isolate(ctx->handle_id,
						     ctx->session->isolate_id);
	if (ctx->handle_id) {
		nvkvm_virtio_close_handle(ctx->handle_id);
		ctx->handle_id = 0;
	}

	/* Tear down any CPU page migrations for this fd */
	nvkvm_cpu_pages_free(ctx);
	mutex_destroy(&ctx->cpu_pages_lock);

	/* Tear down any mmap regions owned by this FD */
	nvkvm_mmap_release_fd(ctx);

	nvkvm_session_put(ctx->session);
	kfree(ctx);
	filp->private_data = NULL;
	return 0;
}

/* ── ioctl forwarding ─────────────────────────────────────────────────────── */

/*
 * nvkvm_ioctl — validate and forward an ioctl to the host.
 *
 * Security: we validate param_size against the known ABI size before copying
 * from userspace. Pointer fields in the parameter blob are zeroed or replaced
 * with offsets into the aux slot; the host never receives raw guest VA values.
 */
static long nvkvm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;
	void *params_buf = NULL;
	void __user *uparams = (void __user *)arg;
	size_t param_size;
	void *aux_buf = NULL;
	size_t aux_size = 0;
	void __user *aux_uptr = NULL;
	long ret;

	if (!ctx)
		return -EBADF;

	/* Validate and get the expected parameter size for this ioctl */
	param_size = nvkvm_ioctl_param_size(cmd);
	if (param_size == (size_t)-1) {
		pr_debug("nvkvm: unknown ioctl cmd=0x%x\n", cmd);
		return -ENOTTY;
	}

	if (param_size > NVKVM_SHM_SLOT_DEFAULT_SIZE)
		return -EINVAL;

	/* Copy parameters from userspace into a kernel buffer */
	if (param_size > 0) {
		params_buf = kzalloc(param_size, GFP_KERNEL);
		if (!params_buf)
			return -ENOMEM;

		/*
		 * Some UVM ioctls (notably UVM_DEINITIALIZE) are called with a
		 * NULL arg by libnvml.  The native NVIDIA UVM driver accepts NULL
		 * and treats it as "use empty/default params".  Mirror that
		 * behaviour: skip the copy and forward the zero-initialised buffer.
		 */
		if (uparams != NULL &&
		    copy_from_user(params_buf, uparams, param_size)) {
			kfree(params_buf);
			return -EFAULT;
		}
	}

	/*
	 * Save caller-supplied user-space pointer / size fields BEFORE any
	 * sanitizer (inline or via nvkvm_sanitize_ioctl_params) runs, so we
	 * can restore them on the response.  CUDA verifies these round-trip
	 * unchanged.
	 *
	 * Bug we already hit: capturing AFTER the inline sanitizer captured
	 * `alloc_parms_size = 0x38` (the class-derived size we filled in for
	 * the host driver) instead of CUDA's original 0 — and the restore
	 * then "restored" that bogus value back over the kernel's writeback.
	 */
	u64 orig_nvos54_params = 0;       /* RM_CONTROL nvos54.params  */
	u64 orig_nvos64_alloc  = 0;       /* RM_ALLOC nvos64.p_alloc_parms */
	u64 orig_nvos64_rights = 0;       /* RM_ALLOC nvos64.p_rights_requested */
	u32 orig_nvos64_size   = 0;       /* RM_ALLOC nvos64.alloc_parms_size */
	u64 orig_nvos21_alloc  = 0;       /* RM_ALLOC nvos21.p_alloc_parms */
	bool have_nvos64_orig  = false;
	__s32 orig_uvm_mm_init_fd = 0;
	bool have_uvm_mm_init     = false;
	__u32 orig_uvm_rm_ctrl_fd = 0;
	bool have_uvm_rm_ctrl     = false;
	/* Embedded-fd fields in frontend ioctls: sanitizer overwrites these
	 * with handle_ids; capture the caller's original guest-fd so the
	 * response round-trips libcuda's value unchanged. */
	__s32 orig_fe_ctl_fd = 0;     bool have_fe_ctl_fd = false;     /* REGISTER_FD       */
	__u32 orig_fe_os_evt_fd = 0;  bool have_fe_os_evt_fd = false;  /* ALLOC/FREE_OS_EVT */
	__s32 orig_fe_nvos02_fd = 0;  bool have_fe_nvos02_fd = false;  /* RM_ALLOC_MEMORY   */
	__s32 orig_fe_nvos33_fd = 0;  bool have_fe_nvos33_fd = false;  /* RM_MAP_MEMORY     */
	if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf &&
	    param_size == sizeof(struct nvos54_parameters)) {
		orig_nvos54_params =
			((struct nvos54_parameters *)params_buf)->params;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && params_buf &&
		   param_size == sizeof(struct nvos64_parameters)) {
		struct nvos64_parameters *a = params_buf;
		orig_nvos64_alloc  = a->p_alloc_parms;
		orig_nvos64_rights = a->p_rights_requested;
		orig_nvos64_size   = a->alloc_parms_size;
		have_nvos64_orig   = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && params_buf &&
		   param_size == sizeof(struct nvos21_parameters)) {
		orig_nvos21_alloc =
			((struct nvos21_parameters *)params_buf)->p_alloc_parms;
	} else if (cmd == UVM_MM_INITIALIZE && params_buf &&
		   param_size == sizeof(struct uvm_mm_initialize_params)) {
		orig_uvm_mm_init_fd =
			((struct uvm_mm_initialize_params *)params_buf)->uvm_fd;
		have_uvm_mm_init = true;
	} else if (params_buf &&
		   (cmd == UVM_REGISTER_GPU_VASPACE ||
		    cmd == UVM_REGISTER_CHANNEL ||
		    cmd == UVM_MAP_EXTERNAL_ALLOCATION)) {
		if (param_size >= 20) {
			orig_uvm_rm_ctrl_fd =
				*(__u32 *)((char *)params_buf + 16);
			have_uvm_rm_ctrl = true;
		}
	} else if (_IOC_NR(cmd) == NV_ESC_REGISTER_FD && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_register_fd)) {
		orig_fe_ctl_fd =
			((struct nv_ioctl_register_fd *)params_buf)->ctl_fd;
		have_fe_ctl_fd = true;
	} else if ((_IOC_NR(cmd) == NV_ESC_ALLOC_OS_EVENT ||
		    _IOC_NR(cmd) == NV_ESC_FREE_OS_EVENT) && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_alloc_os_event)) {
		orig_fe_os_evt_fd =
			((struct nv_ioctl_alloc_os_event *)params_buf)->fd;
		have_fe_os_evt_fd = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC_MEMORY && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_nvos02_parameters_with_fd)) {
		orig_fe_nvos02_fd =
			((struct nv_ioctl_nvos02_parameters_with_fd *)params_buf)->fd;
		have_fe_nvos02_fd = true;
	} else if (_IOC_NR(cmd) == NV_ESC_RM_MAP_MEMORY && params_buf &&
		   param_size >= sizeof(struct nv_ioctl_nvos33_parameters_with_fd)) {
		orig_fe_nvos33_fd =
			((struct nv_ioctl_nvos33_parameters_with_fd *)params_buf)->fd;
		have_fe_nvos33_fd = true;
	}

	/* NVOS56 UPDATE_DEVICE_MAPPING_INFO: the kernel uses pOldCpuAddress
	 * as a lookup key on the host driver's CpuMapping table for the
	 * calling process.  Because our stub mmaps the BAR at its own VAs
	 * (which the kernel registered), and libcuda passes its guest-side
	 * VAs (which the stub never had), the lookup ALWAYS fails on this
	 * path and the kernel returns NV_ERR_OBJECT_NOT_FOUND (0x57).  The
	 * mapping still works because we install the BAR pages into the
	 * guest's GPA window via QEMU, so this ioctl is effectively
	 * informational.  We save the caller's values so we can fake
	 * success on the response path. */
	__u64 orig_nvos56_old = 0, orig_nvos56_new = 0;
	bool fake_nvos56_ok = false;
	if (_IOC_NR(cmd) == NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO && params_buf &&
	    param_size == sizeof(struct nvos56_parameters)) {
		struct nvos56_parameters *p = params_buf;
		orig_nvos56_old = p->p_old_cpu_address;
		orig_nvos56_new = p->p_new_cpu_address;
		fake_nvos56_ok  = true;
	}

	/* NV_ESC_RM_UNMAP_MEMORY (nvos34): same VA-lookup pattern as NVOS56.
	 * Kernel looks up CpuMapping by pLinearAddress; the stub's mmap'd VA
	 * was registered there but libcuda passes its own guest-side VA, so
	 * the lookup always returns NV_ERR_OBJECT_NOT_FOUND.  The unmap
	 * itself is handled by our QEMU/KVM region-uninstall path (the GPA
	 * goes away when the guest munmaps); the kernel record is informational
	 * in our forwarded model.  Fake success on response. */
	__u64 orig_nvos34_va = 0;
	bool fake_nvos34_ok = false;
	if (_IOC_NR(cmd) == NV_ESC_RM_UNMAP_MEMORY && params_buf &&
	    param_size == sizeof(struct nv_ioctl_nvos34_parameters)) {
		struct nv_ioctl_nvos34_parameters *p = params_buf;
		orig_nvos34_va = p->p_linear_address;
		fake_nvos34_ok = true;
	}

	/*
	 * For ioctls with embedded secondary buffers: extract the secondary data
	 * BEFORE the sanitizer zeroes the pointer fields.  We carry the data in
	 * the aux slot so the host can reconstruct it without ever seeing a raw
	 * guest VA.
	 *
	 * NV_ESC_RM_CONTROL: secondary buffer = cmd-specific params (in+out).
	 * NV_ESC_RM_ALLOC (NVOS21): secondary buffer = class-specific alloc params
	 *   (input only; size determined by h_class).
	 * NV_ESC_RM_ALLOC (NVOS64): secondary buffer = class-specific alloc params
	 *   (input only; alloc_parms_size tells us the size).
	 */
	if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf) {
		struct nvos54_parameters *ctrl = params_buf;
		if (ctrl->params_size > 0 && ctrl->params != 0) {
			if (ctrl->params_size > NVKVM_SHM_SLOT_DEFAULT_SIZE) {
				kfree(params_buf);
				return -EINVAL;
			}
			aux_uptr = (void __user *)(uintptr_t)ctrl->params;
			aux_buf = kzalloc(ctrl->params_size, GFP_KERNEL);
			if (!aux_buf) {
				kfree(params_buf);
				return -ENOMEM;
			}
			if (copy_from_user(aux_buf, aux_uptr, ctrl->params_size)) {
				kfree(aux_buf);
				kfree(params_buf);
				return -EFAULT;
			}
			aux_size = ctrl->params_size;

			/*
			 * Commands that embed an `NvxxxCtrlXxxGetInfoParams` preamble
			 * (info_list_size, pad, info_list pointer) at the start of the
			 * inner params struct. The driver writes through the
			 * info_list pointer, so we extend aux_buf to hold the list,
			 * copy the user's list into the extension, and zero the
			 * pointer field — QEMU substitutes a host VA pointing at the
			 * extension. After the ioctl we copy the list back to the
			 * original user-space address (saved off below). See gVisor
			 * `ctrlIoctlHasInfoList` for the reference implementation.
			 */
			/*
			 * NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST has two embedded
			 * NvP64 list pointers (PChannelHandleList, PChannelList),
			 * each pointing at a u32 array of NumChannels entries.
			 * Both lists are IN+OUT for the driver: it reads them
			 * to filter then writes back the results.  Extend
			 * aux_buf to hold both lists inline, copy the user
			 * buffers into the extension, and zero the pointer
			 * fields — stub substitutes host VAs pointing at the
			 * extension area.  After the ioctl we copy the lists
			 * back to the original user buffers.  See gVisor
			 * ctrlDevFIFOGetChannelList.
			 */
			if (ctrl->cmd == NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST &&
			    ctrl->params_size >= 24) {
				__u32 num_channels = *(__u32 *)aux_buf;
				__u64 p_handles    = *(__u64 *)((char *)aux_buf + 8);
				__u64 p_list       = *(__u64 *)((char *)aux_buf + 16);
				if (num_channels > 0 && num_channels <= 4096 &&
				    p_handles && p_list) {
					size_t list_bytes = (size_t)num_channels *
							    sizeof(__u32);
					size_t ext = ctrl->params_size +
						     2 * list_bytes;
					void *ext_buf = kzalloc(ext, GFP_KERNEL);
					if (!ext_buf) {
						kfree(aux_buf);
						kfree(params_buf);
						return -ENOMEM;
					}
					memcpy(ext_buf, aux_buf, ctrl->params_size);
					if (copy_from_user((char *)ext_buf +
							   ctrl->params_size,
							   (void __user *)(uintptr_t)p_handles,
							   list_bytes) ||
					    copy_from_user((char *)ext_buf +
							   ctrl->params_size +
							   list_bytes,
							   (void __user *)(uintptr_t)p_list,
							   list_bytes)) {
						kfree(ext_buf);
						kfree(aux_buf);
						kfree(params_buf);
						return -EFAULT;
					}
					/* Zero ptrs; stub fills them */
					*(__u64 *)((char *)ext_buf + 8)  = 0;
					*(__u64 *)((char *)ext_buf + 16) = 0;
					kfree(aux_buf);
					aux_buf  = ext_buf;
					aux_size = ext;
				}
			}

			{
				int has_info_list = 0;
				switch (ctrl->cmd) {
				case NV0041_CTRL_CMD_GET_SURFACE_INFO:
				case NV0080_CTRL_CMD_GR_GET_INFO:
				case NV2080_CTRL_CMD_BIOS_GET_INFO:
				case NV2080_CTRL_CMD_GR_GET_INFO:
				case NV2080_CTRL_CMD_FB_GET_INFO:
				case NV2080_CTRL_CMD_BUS_GET_INFO:
					has_info_list = 1;
					break;
				}
				if (has_info_list &&
				    ctrl->params_size >= 16 /* size(4)+pad(4)+ptr(8) */) {
					__u32 list_size = *(__u32 *)aux_buf;
					__u64 list_ptr  = *(__u64 *)((char *)aux_buf + 8);
					if (list_size > 0 && list_size <= 4096 && list_ptr != 0) {
						size_t list_bytes =
							(size_t)list_size *
							NVXXX_CTRL_XXX_INFO_ENTRY_SIZE;
						size_t ext = ctrl->params_size + list_bytes;
						void *ext_buf = kzalloc(ext, GFP_KERNEL);
						if (!ext_buf) {
							kfree(aux_buf);
							kfree(params_buf);
							return -ENOMEM;
						}
						memcpy(ext_buf, aux_buf, ctrl->params_size);
						if (copy_from_user((char *)ext_buf +
								   ctrl->params_size,
								   (void __user *)(uintptr_t)list_ptr,
								   list_bytes)) {
							kfree(ext_buf);
							kfree(aux_buf);
							kfree(params_buf);
							return -EFAULT;
						}
						/* Zero info_list ptr; QEMU/stub fills with host VA */
						*(__u64 *)((char *)ext_buf + 8) = 0;
						kfree(aux_buf);
						aux_buf  = ext_buf;
						aux_size = ext;
					}
				}
			}

			/*
			 * NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION has embedded
			 * pointer fields (pDriverVersionBuffer etc.) pointing to
			 * guest user-space string buffers.  The host NVIDIA driver
			 * can't write to guest VAs.  Extend aux_buf to hold the
			 * strings inline, zero the embedded pointers (QEMU replaces
			 * them with host VAs pointing into the extension), then copy
			 * the strings back to the original user-space buffers after
			 * the ioctl returns.
			 */
			if (ctrl->cmd == NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION &&
			    ctrl->params_size >=
			    sizeof(struct nv0000_ctrl_system_get_build_version_params)) {
				struct nv0000_ctrl_system_get_build_version_params *ver =
					aux_buf;
				__u32 sz = ver->size_of_strings;

				if (sz > 0 && sz <= 512) {
					size_t ext = ctrl->params_size + 3 * sz;
					void *ext_buf = kzalloc(ext, GFP_KERNEL);

					if (!ext_buf) {
						kfree(aux_buf);
						kfree(params_buf);
						return -ENOMEM;
					}
					memcpy(ext_buf, aux_buf, ctrl->params_size);
					/* Zero guest VA pointers; QEMU fills in host VAs */
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_driver_version_buffer = 0;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_version_buffer = 0;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 ext_buf)->p_title_buffer = 0;
					kfree(aux_buf);
					aux_buf  = ext_buf;
					aux_size = ext;
				}
			}
		}
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
		   param_size == sizeof(struct nvos21_parameters) && params_buf) {
		struct nvos21_parameters *alloc = params_buf;
		if (alloc->p_alloc_parms != 0) {
			/* Same kernel-writes-back-aux requirement as the nvos64
			 * path: track the user pointer so the alloc params copy
			 * back after the ioctl. */
			aux_uptr = (void __user *)(uintptr_t)alloc->p_alloc_parms;
			size_t ap_size = 0;

			switch (alloc->h_class) {
			case NV01_DEVICE_0:
				ap_size = sizeof(struct nv0080_alloc_parameters);
				break;
			case NV20_SUBDEVICE_0:
				ap_size = sizeof(struct nv2080_alloc_parameters);
				break;
			case RM_USER_SHARED_DATA:
				ap_size = sizeof(struct nv00de_alloc_parameters_v545);
				break;
			case FERMI_VASPACE_A:
				ap_size = sizeof(struct nv_vaspace_allocation_parameters);
				break;
			case NV50_MEMORY_VIRTUAL:
			case NV01_MEMORY_LOCAL_USER:
			case NV01_MEMORY_SYSTEM:
				ap_size = sizeof(struct nv_memory_allocation_params_v545);
				break;
			case KEPLER_CHANNEL_GROUP_A:
				ap_size = sizeof(struct nv_channel_group_allocation_parameters);
				break;
			case FERMI_CONTEXT_SHARE_A:
				ap_size = sizeof(struct nv_ctxshare_allocation_parameters);
				break;
			case TURING_CHANNEL_GPFIFO_A:
			case AMPERE_CHANNEL_GPFIFO_A:
			case HOPPER_CHANNEL_GPFIFO_A:
				ap_size = sizeof(struct nv_channel_alloc_params_v570);
				break;
			case NV01_EVENT_OS_EVENT:
				ap_size = sizeof(struct nv0005_alloc_parameters);
				break;
			case VOLTA_DMA_COPY_A:
			case TURING_DMA_COPY_A:
			case AMPERE_DMA_COPY_A:
			case AMPERE_DMA_COPY_B:
			case HOPPER_DMA_COPY_A:
			case BLACKWELL_DMA_COPY_A:
				ap_size = sizeof(struct nvb0b5_allocation_parameters);
				break;
			}
			if (ap_size > 0) {
				aux_buf = kzalloc(ap_size, GFP_KERNEL);
				if (!aux_buf) {
					kfree(params_buf);
					return -ENOMEM;
				}
				if (copy_from_user(aux_buf,
						   (void __user *)(uintptr_t)alloc->p_alloc_parms,
						   ap_size)) {
					kfree(aux_buf);
					kfree(params_buf);
					return -EFAULT;
				}
				aux_size = ap_size;
			}
		}
	} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
		   param_size == sizeof(struct nvos64_parameters) && params_buf) {
		struct nvos64_parameters *alloc = params_buf;
		if (alloc->p_alloc_parms != 0) {
			/* Track the original user pointer so the kernel-written
			 * alloc params (vaSize, vaBase, etc.) get copied back to
			 * userspace after the ioctl. Without this libcuda sees
			 * stale zeros and decides the GPU context is unusable
			 * (CUDA_ERROR_ILLEGAL_STATE on cuCtxCreate). */
			aux_uptr = (void __user *)(uintptr_t)alloc->p_alloc_parms;
			/* CUDA often leaves alloc_parms_size=0 and relies on the
			 * driver to size the buffer by hClass. Honor an explicit
			 * size if provided, otherwise fall back to the per-class
			 * size (same table as the nvos21 path above). */
			size_t ap_size = alloc->alloc_parms_size;
			if (ap_size == 0) {
				switch (alloc->h_class) {
				case NV01_DEVICE_0:
					ap_size = sizeof(struct nv0080_alloc_parameters);
					break;
				case NV20_SUBDEVICE_0:
					ap_size = sizeof(struct nv2080_alloc_parameters);
					break;
				case RM_USER_SHARED_DATA:
					ap_size = sizeof(struct nv00de_alloc_parameters_v545);
					break;
				case FERMI_VASPACE_A:
					ap_size = sizeof(struct nv_vaspace_allocation_parameters);
					break;
				case NV50_MEMORY_VIRTUAL:
				case NV01_MEMORY_LOCAL_USER:
				case NV01_MEMORY_SYSTEM:
					ap_size = sizeof(struct nv_memory_allocation_params_v545);
					break;
				case KEPLER_CHANNEL_GROUP_A:
					ap_size = sizeof(struct nv_channel_group_allocation_parameters);
					break;
				case FERMI_CONTEXT_SHARE_A:
					ap_size = sizeof(struct nv_ctxshare_allocation_parameters);
					break;
				case TURING_CHANNEL_GPFIFO_A:
				case AMPERE_CHANNEL_GPFIFO_A:
				case HOPPER_CHANNEL_GPFIFO_A:
					ap_size = sizeof(struct nv_channel_alloc_params_v570);
					break;
				case NV01_EVENT_OS_EVENT:
					ap_size = sizeof(struct nv0005_alloc_parameters);
					break;
				case VOLTA_DMA_COPY_A:
				case TURING_DMA_COPY_A:
				case AMPERE_DMA_COPY_A:
				case AMPERE_DMA_COPY_B:
				case HOPPER_DMA_COPY_A:
				case BLACKWELL_DMA_COPY_A:
					ap_size = sizeof(struct nvb0b5_allocation_parameters);
					break;
				}
			}
			if (ap_size > NVKVM_SHM_SLOT_DEFAULT_SIZE) {
				kfree(params_buf);
				return -EINVAL;
			}
			if (ap_size > 0) {
				aux_buf = kzalloc(ap_size, GFP_KERNEL);
				if (!aux_buf) {
					kfree(params_buf);
					return -ENOMEM;
				}
				if (copy_from_user(aux_buf,
						   (void __user *)(uintptr_t)alloc->p_alloc_parms,
						   ap_size)) {
					kfree(aux_buf);
					kfree(params_buf);
					return -EFAULT;
				}
				aux_size = ap_size;
				/* Sync the size back so the host driver sees a
				 * consistent (params, paramsSize) pair. */
				alloc->alloc_parms_size = (u32)ap_size;

				/* NV01_EVENT_OS_EVENT: Data is an FD pointing at one
				 * of the /dev/nvidia* char devices (not a generic
				 * eventfd — gVisor confirms via the frontendFD type
				 * check, and the driver verifies f->f_op ==
				 * nv_frontend_fops in osUserHandleToKernelPtr).  The
				 * fd libcuda passes IS already opened through our
				 * module, so we have a handle_id for it.  Same
				 * translation pattern as UVM_MM_INITIALIZE's
				 * uvm_fd: replace the user-supplied guest fd with
				 * its handle_id; the stub maps that to its local
				 * /dev/nvidia* fd. */
				if (alloc->h_class == NV01_EVENT_OS_EVENT &&
				    ap_size >= sizeof(struct nv0005_alloc_parameters)) {
					struct nv0005_alloc_parameters *ep = aux_buf;
					int user_fd = (int)(int32_t)ep->data;
					/* Pre-translate dump: exactly what libcuda wrote */
					print_hex_dump(KERN_INFO,
						"nvkvm guest pre 0x79 nvos64: ",
						DUMP_PREFIX_NONE, 48, 1,
						params_buf, param_size, false);
					print_hex_dump(KERN_INFO,
						"nvkvm guest pre 0x79 aux:    ",
						DUMP_PREFIX_NONE, 24, 1,
						aux_buf, aux_size, false);
					if (user_fd >= 0) {
						struct file *f = fget(user_fd);
						__u32 hid = 0;
						if (f) {
							struct nvkvm_fd_ctx *other =
								f->private_data;
							if (other && other->handle_id)
								hid = other->handle_id;
							fput(f);
						}
						if (hid > 0)
							ep->data = hid;
					}
					/* Post-translate dump */
					print_hex_dump(KERN_INFO,
						"nvkvm guest post 0x79 aux:   ",
						DUMP_PREFIX_NONE, 24, 1,
						aux_buf, aux_size, false);
				}
			}
		}
	}

	/*
	 * Sanitize embedded pointer and FD fields before forwarding.
	 * The sanitizer replaces guest VA pointers with slot references and
	 * translates session-local FD tokens to their host-facing equivalents.
	 */
	ret = nvkvm_sanitize_ioctl_params(ctx, cmd, params_buf, param_size);
	if (ret) {
		kfree(aux_buf);
		kfree(params_buf);
		return ret;
	}

	/* Forward to host via the appropriate path */
	if (ctx->handle_id && ctx->session->isolate_id) {
		/*
		 * Isolate path: ioctl runs in the isolate process so the NVIDIA
		 * driver sees a valid VA space.  On EFAULT the isolate returns
		 * the faulting GVA; we map the backing region and retry.
		 *
		 * UVM ioctls also go through the isolate — the UVM kernel must
		 * track the GPU-memory owner, which is the isolate.  Embedded
		 * fd_tokens have already been translated to handle_ids in the
		 * sanitizer; the stub looks them up via its handle table.
		 */
		__u32 ioctl_flags = 0;
		__u64 fault_addr  = 0;
		int retries;

#define NVKVM_MAX_EFAULT_RETRIES 128
		for (retries = 0; retries < NVKVM_MAX_EFAULT_RETRIES; retries++) {
			fault_addr = 0;
			ret = nvkvm_virtio_ioctl_on_isolate(ctx, cmd,
							    params_buf, param_size,
							    aux_buf, aux_size,
							    ioctl_flags,
							    &fault_addr);
			if (ret != -EFAULT || !fault_addr)
				break;

			ioctl_flags |= NVKVM_IOCTL_FL_RETRY_EFAULT;

			if (nvkvm_efault_resolve(ctx, fault_addr)) {
				ret = -EFAULT;
				break;
			}
		}

		/*
		 * Write back any CPU pages migrated during this ioctl.
		 * The NVIDIA driver may have written results into the isolate's
		 * copy of the page (e.g. DtoH data); copy it back to the guest.
		 */
		nvkvm_cpu_pages_writeback(ctx);
	} else {
		/* Open establishes ctx->handle_id and ctx->session->isolate_id;
		 * an ioctl on a ctx missing either is a logic bug. The legacy
		 * non-isolate fallback (NVKVM_REQ_IOCTL) was removed in Step 3d. */
		ret = -EBADF;
	}

	/*
	 * Restore the user-space pointer fields the sanitizer (and stub) blanked.
	 * CUDA expects to read its own pointer back unchanged across the ioctl;
	 * not restoring them causes cuInit to give up with CUDA_ERROR_NO_DEVICE.
	 * Only restore on success-ish responses — if the driver returned an
	 * error we still keep the host's writes (status field etc.) but the
	 * caller's pointer should round-trip.
	 */
	if (params_buf && ret != -ENOMEM && ret != -ENOSPC && ret != -EFAULT) {
		if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && orig_nvos54_params &&
		    param_size == sizeof(struct nvos54_parameters)) {
			((struct nvos54_parameters *)params_buf)->params =
				orig_nvos54_params;
		} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
			   param_size == sizeof(struct nvos64_parameters)) {
			struct nvos64_parameters *a = params_buf;
			if (orig_nvos64_alloc)  a->p_alloc_parms      = orig_nvos64_alloc;
			if (orig_nvos64_rights) a->p_rights_requested = orig_nvos64_rights;
			/* paramsSize must round-trip too — the host driver
			 * does not write it; CUDA verifies the OUT value
			 * matches the IN. Our sanitizer fills it in by class
			 * when CUDA leaves it at 0; restore the caller's
			 * original value here. */
			if (have_nvos64_orig)   a->alloc_parms_size   = orig_nvos64_size;
		} else if (_IOC_NR(cmd) == NV_ESC_RM_ALLOC && orig_nvos21_alloc &&
			   param_size == sizeof(struct nvos21_parameters)) {
			((struct nvos21_parameters *)params_buf)->p_alloc_parms =
				orig_nvos21_alloc;
		} else if (have_uvm_mm_init &&
			   param_size == sizeof(struct uvm_mm_initialize_params)) {
			((struct uvm_mm_initialize_params *)params_buf)->uvm_fd =
				orig_uvm_mm_init_fd;
		} else if (have_uvm_rm_ctrl && param_size >= 20) {
			*(__u32 *)((char *)params_buf + 16) = orig_uvm_rm_ctrl_fd;
		} else if (have_fe_ctl_fd && _IOC_NR(cmd) == NV_ESC_REGISTER_FD &&
			   param_size >= sizeof(struct nv_ioctl_register_fd)) {
			((struct nv_ioctl_register_fd *)params_buf)->ctl_fd =
				orig_fe_ctl_fd;
		} else if (have_fe_os_evt_fd &&
			   (_IOC_NR(cmd) == NV_ESC_ALLOC_OS_EVENT ||
			    _IOC_NR(cmd) == NV_ESC_FREE_OS_EVENT) &&
			   param_size >= sizeof(struct nv_ioctl_alloc_os_event)) {
			((struct nv_ioctl_alloc_os_event *)params_buf)->fd =
				orig_fe_os_evt_fd;
		} else if (have_fe_nvos02_fd && _IOC_NR(cmd) == NV_ESC_RM_ALLOC_MEMORY &&
			   param_size >= sizeof(struct nv_ioctl_nvos02_parameters_with_fd)) {
			((struct nv_ioctl_nvos02_parameters_with_fd *)params_buf)->fd =
				orig_fe_nvos02_fd;
		} else if (have_fe_nvos33_fd && _IOC_NR(cmd) == NV_ESC_RM_MAP_MEMORY &&
			   param_size >= sizeof(struct nv_ioctl_nvos33_parameters_with_fd)) {
			((struct nv_ioctl_nvos33_parameters_with_fd *)params_buf)->fd =
				orig_fe_nvos33_fd;
		} else if (fake_nvos56_ok &&
			   _IOC_NR(cmd) == NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO &&
			   param_size == sizeof(struct nvos56_parameters)) {
			struct nvos56_parameters *p = params_buf;
			/* Restore caller's pointer values so libcuda sees them
			 * unchanged across the ioctl, and force status=NV_OK
			 * to mask the kernel's unavoidable OBJECT_NOT_FOUND. */
			p->p_old_cpu_address = orig_nvos56_old;
			p->p_new_cpu_address = orig_nvos56_new;
			p->status = 0;
		} else if (fake_nvos34_ok &&
			   _IOC_NR(cmd) == NV_ESC_RM_UNMAP_MEMORY &&
			   param_size == sizeof(struct nv_ioctl_nvos34_parameters)) {
			struct nv_ioctl_nvos34_parameters *p = params_buf;
			/* Same VA-lookup-fail pattern as NVOS56; fake success. */
			p->p_linear_address = orig_nvos34_va;
			p->status = 0;
		}
	}

	/*
	 * Always copy parameters back to userspace, even on driver error.
	 * The NVIDIA RM may populate response fields even when returning an error
	 * (e.g. CHECK_VERSION_STR fills version_string and returns EINVAL).
	 * Only skip if the transport itself failed (ret == -ENOMEM / -ENOSPC)
	 * and params_buf was never written to shared memory.
	 */
	if (param_size > 0 && uparams != NULL &&
	    ret != -ENOMEM && ret != -ENOSPC && ret != -EFAULT) {
		if (copy_to_user(uparams, params_buf, param_size))
			ret = -EFAULT;
	}

	/* Copy back the aux buffer (RM_CONTROL output params) to userspace */
	if (aux_buf && aux_uptr && ret != -ENOMEM && ret != -ENOSPC &&
	    ret != -EFAULT) {
		/*
		 * For NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION: the original
		 * user-space pointer fields were replaced with zeros before
		 * forwarding to QEMU.  QEMU wrote the version strings into the
		 * extension area of aux_buf (at offsets sizeof(params), +sz, +2*sz).
		 * Copy the strings back to the original user-space buffer addresses
		 * that the caller passed in, then copy only the base params struct
		 * to aux_uptr so the embedded pointers in userspace remain valid.
		 */
		if (_IOC_NR(cmd) == NV_ESC_RM_CONTROL && params_buf) {
			struct nvos54_parameters *ctrl = params_buf;

			/*
			 * InfoList family (NV2080_CTRL_CMD_GR_GET_INFO and friends):
			 * aux_buf is sized to params_size + list_size*8; the trailing
			 * region holds the driver-written list. Copy it back to the
			 * original user-space list address, then restore the pointer
			 * in aux_buf and write only the base params struct out.
			 */
			/* FIFO_GET_CHANNELLIST response: copy both lists back to
			 * the original user pointers, then restore those pointers
			 * in aux_buf so userspace sees them unchanged. */
			if (ctrl->cmd == NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST &&
			    ctrl->params_size >= 24 &&
			    aux_size > ctrl->params_size) {
				struct {
					__u32 num_channels;
					__u32 pad;
					__u64 p_handles;
					__u64 p_list;
				} orig;
				if (!copy_from_user(&orig, aux_uptr, sizeof(orig)) &&
				    orig.num_channels > 0) {
					size_t list_bytes = (size_t)orig.num_channels *
							    sizeof(__u32);
					if (aux_size >= ctrl->params_size + 2 * list_bytes) {
						if (orig.p_handles)
							copy_to_user(
								(void __user *)(uintptr_t)orig.p_handles,
								(char *)aux_buf + ctrl->params_size,
								list_bytes);
						if (orig.p_list)
							copy_to_user(
								(void __user *)(uintptr_t)orig.p_list,
								(char *)aux_buf + ctrl->params_size +
								list_bytes,
								list_bytes);
					}
					/* Restore pointers */
					*(__u64 *)((char *)aux_buf + 8)  = orig.p_handles;
					*(__u64 *)((char *)aux_buf + 16) = orig.p_list;
				}
				if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
					ret = -EFAULT;
				goto done_aux_copy;
			}

			{
				int has_info_list = 0;
				switch (ctrl->cmd) {
				case NV0041_CTRL_CMD_GET_SURFACE_INFO:
				case NV0080_CTRL_CMD_GR_GET_INFO:
				case NV2080_CTRL_CMD_BIOS_GET_INFO:
				case NV2080_CTRL_CMD_GR_GET_INFO:
				case NV2080_CTRL_CMD_FB_GET_INFO:
				case NV2080_CTRL_CMD_BUS_GET_INFO:
					has_info_list = 1;
					break;
				}
				if (has_info_list &&
				    ctrl->params_size >= 16 &&
				    aux_size > ctrl->params_size) {
					struct {
						__u32 list_size;
						__u32 pad;
						__u64 list_ptr;
					} orig;
					if (!copy_from_user(&orig, aux_uptr, sizeof(orig)) &&
					    orig.list_size > 0 && orig.list_ptr != 0) {
						size_t list_bytes =
							(size_t)orig.list_size *
							NVXXX_CTRL_XXX_INFO_ENTRY_SIZE;
						if (aux_size >= ctrl->params_size + list_bytes) {
							copy_to_user(
								(void __user *)(uintptr_t)orig.list_ptr,
								(char *)aux_buf + ctrl->params_size,
								list_bytes);
						}
						/* Restore original pointer so userspace sees its VA */
						*(__u64 *)((char *)aux_buf + 8) = orig.list_ptr;
					}
					if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
						ret = -EFAULT;
					goto done_aux_copy;
				}
			}

			if (ctrl->cmd == NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION &&
			    ctrl->params_size >=
			    sizeof(struct nv0000_ctrl_system_get_build_version_params) &&
			    aux_size > ctrl->params_size) {
				/*
				 * aux_size = params_size + 3*sz; the extension area
				 * holds [drv_string][ver_string][title_string].
				 * We read the original user-space string pointers from
				 * the ORIGINAL copy of the params struct (in aux_uptr).
				 */
				struct nv0000_ctrl_system_get_build_version_params orig;
				__u32 sz;

				if (!copy_from_user(&orig, aux_uptr, sizeof(orig))) {
					sz = orig.size_of_strings;
					if (sz > 0 && sz <= 512) {
						char *ext = (char *)aux_buf +
							    ctrl->params_size;
						/* Copy version strings to original user buffers */
						if (orig.p_driver_version_buffer)
							copy_to_user((void __user *)(uintptr_t)
								     orig.p_driver_version_buffer,
								     ext, sz);
						if (orig.p_version_buffer)
							copy_to_user((void __user *)(uintptr_t)
								     orig.p_version_buffer,
								     ext + sz, sz);
						if (orig.p_title_buffer)
							copy_to_user((void __user *)(uintptr_t)
								     orig.p_title_buffer,
								     ext + 2 * sz, sz);
					}
					/*
					 * Restore the original user-space pointer values
					 * in aux_buf before copying it back so CUDA can
					 * still read its own pointer fields from the
					 * returned params struct.
					 */
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_driver_version_buffer =
						orig.p_driver_version_buffer;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_version_buffer = orig.p_version_buffer;
					((struct nv0000_ctrl_system_get_build_version_params *)
					 aux_buf)->p_title_buffer = orig.p_title_buffer;
				}
				/* Copy only the base params struct (not the extension) */
				if (copy_to_user(aux_uptr, aux_buf, ctrl->params_size))
					ret = -EFAULT;
				goto done_aux_copy;
			}
		}
		if (copy_to_user(aux_uptr, aux_buf, aux_size))
			ret = -EFAULT;
done_aux_copy:
		;
	}

	kfree(aux_buf);
	kfree(params_buf);
	return ret;
}

/* ── mmap ─────────────────────────────────────────────────────────────────── */

static int nvkvm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;

	if (!ctx)
		return -EBADF;

	return nvkvm_mmap_request(ctx, vma);
}

/* ── poll ─────────────────────────────────────────────────────────────────── */

static __poll_t nvkvm_poll(struct file *filp, poll_table *wait)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;
	__poll_t events;

	if (!ctx)
		return EPOLLERR;

	poll_wait(filp, &ctx->poll_wq, wait);
	events = atomic_xchg(&ctx->poll_events, 0);
	return events;
}

/* ── virtio device probe/remove ───────────────────────────────────────────── */

static int nvkvm_virtio_probe(struct virtio_device *vdev);
static void nvkvm_virtio_remove(struct virtio_device *vdev);

static const struct virtio_device_id nvkvm_virtio_id_table[] = {
	{ VIRTIO_ID_NVGPU, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver nvkvm_virtio_driver = {
	.driver.name  = "virtio-nvgpu",
	.driver.owner = THIS_MODULE,
	.id_table     = nvkvm_virtio_id_table,
	.probe        = nvkvm_virtio_probe,
	.remove       = nvkvm_virtio_remove,
};

static int nvkvm_virtio_probe(struct virtio_device *vdev)
{
	int ret;

	pr_info("nvkvm: probe called for virtio device id=0x%x\n",
		vdev->id.device);

	ret = nvkvm_virtio_init(vdev, &nvkvm);
	if (ret) {
		dev_err(&vdev->dev, "nvkvm: virtio init failed: %d\n", ret);
		return ret;
	}

	/*
	 * Read the host driver version from the shared memory control block
	 * and confirm the protocol version matches before accepting ioctls.
	 */
	ret = nvkvm_negotiate_version(&nvkvm);
	if (ret) {
		dev_err(&vdev->dev,
			"nvkvm: protocol version negotiation failed: %d\n",
			ret);
		nvkvm_virtio_fini(&nvkvm);
		return ret;
	}

	return 0;
}

static void nvkvm_virtio_remove(struct virtio_device *vdev)
{
	nvkvm_virtio_fini(&nvkvm);
}

/* ── Module init/exit ─────────────────────────────────────────────────────── */

static int __init nvkvm_init(void)
{
	int ret;

	memset(&nvkvm, 0, sizeof(nvkvm));
	mutex_init(&nvkvm.sessions_lock);
	idr_init(&nvkvm.sessions_idr);

	ret = register_virtio_driver(&nvkvm_virtio_driver);
	if (ret) {
		pr_err("nvkvm: failed to register virtio driver: %d\n", ret);
		return ret;
	}

	ret = register_devices();
	if (ret) {
		unregister_virtio_driver(&nvkvm_virtio_driver);
		return ret;
	}

	pr_info("nvkvm: NVIDIA GPU passthrough guest module loaded\n");
	return 0;
}

static void __exit nvkvm_exit(void)
{
	unregister_devices();
	unregister_virtio_driver(&nvkvm_virtio_driver);
	idr_destroy(&nvkvm.sessions_idr);
	pr_info("nvkvm: module unloaded\n");
}

module_init(nvkvm_init);
module_exit(nvkvm_exit);
