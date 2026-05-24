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
 *   - Pointer fields inside ioctl structs are checked or zeroed before the
 *     blob is placed in shared memory — the host backend never receives raw
 *     guest userspace virtual addresses.
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

static struct nvkvm_state nvkvm;

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

static int __init register_devices(void)
{
	int ret, i;
	dev_t devno;

	nvkvm.class = class_create("nvkvm");
	if (IS_ERR(nvkvm.class))
		return PTR_ERR(nvkvm.class);

	/* /dev/nvidiactl — control device, minor 255 */
	ret = alloc_chrdev_region(&devno, NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE,
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

	/* /dev/nvidia0 .. /dev/nvidia{N-1} */
	nvkvm.num_gpus = min(num_gpus, NV_MINOR_DEVICE_NUMBER_REGULAR_MAX + 1);
	ret = alloc_chrdev_region(&nvkvm.gpu_devno_base, 0, nvkvm.num_gpus,
				  "nvidia");
	if (ret)
		goto err_ctl;
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

static int nvkvm_open(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx;
	struct nvkvm_resp_open resp;
	int dev_id;
	int ret;

	/* Determine which device is being opened from the minor number */
	if (imajor(inode) == nvkvm.ctl_major)
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

	/* Ask the host to open the corresponding real device */
	ret = nvkvm_virtio_open(dev_id, filp->f_flags, &resp);
	if (ret) {
		nvkvm_session_put(ctx->session);
		kfree(ctx);
		return ret;
	}
	if (resp.status) {
		nvkvm_session_put(ctx->session);
		kfree(ctx);
		return -resp.status;
	}

	ctx->fd_token = resp.fd_token;
	filp->private_data = ctx;

	pr_debug("nvkvm: opened dev_id=%d fd_token=%u tgid=%d\n",
		 dev_id, ctx->fd_token, current->tgid);
	return 0;
}

static int nvkvm_release(struct inode *inode, struct file *filp)
{
	struct nvkvm_fd_ctx *ctx = filp->private_data;
	struct nvkvm_resp_close resp;

	if (!ctx)
		return 0;

	nvkvm_virtio_close(ctx->fd_token, &resp);

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

		if (copy_from_user(params_buf, uparams, param_size)) {
			kfree(params_buf);
			return -EFAULT;
		}
	}

	/*
	 * Sanitize embedded pointer and FD fields before forwarding.
	 * The sanitizer replaces guest VA pointers with slot references and
	 * translates session-local FD tokens to their host-facing equivalents.
	 */
	ret = nvkvm_sanitize_ioctl_params(ctx, cmd, params_buf, param_size);
	if (ret) {
		kfree(params_buf);
		return ret;
	}

	/* Forward to host and wait for response */
	ret = nvkvm_virtio_ioctl(ctx, cmd, params_buf, param_size);

	/* Copy updated parameters back to userspace */
	if (ret >= 0 && param_size > 0) {
		if (copy_to_user(uparams, params_buf, param_size))
			ret = -EFAULT;
	}

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
