// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_virtio.c — virtio transport frontend for the nvkvm guest module
 *
 * Manages:
 *  - Virtio device probe / VQ setup
 *  - Shared memory slot allocator
 *  - Synchronous request/response round-trips over VQ_TX / VQ_RX
 *  - Async event delivery from VQ_EVT to per-FD poll queues
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>

#include "nvkvm.h"

/* ── Slot allocator ───────────────────────────────────────────────────────── */

int nvkvm_slot_alloc(struct nvkvm_state *state)
{
	unsigned long flags;
	int slot;

	spin_lock_irqsave(&state->slot_lock, flags);
	slot = find_next_zero_bit(state->slot_bitmap, NVKVM_SHM_NSLOTS, 1);
	if (slot >= NVKVM_SHM_NSLOTS)
		slot = -ENOMEM;
	else
		set_bit(slot, state->slot_bitmap);
	spin_unlock_irqrestore(&state->slot_lock, flags);
	return slot;
}

void nvkvm_slot_free(struct nvkvm_state *state, int slot)
{
	unsigned long flags;
	spin_lock_irqsave(&state->slot_lock, flags);
	clear_bit(slot, state->slot_bitmap);
	spin_unlock_irqrestore(&state->slot_lock, flags);
}

void *nvkvm_slot_addr(struct nvkvm_state *state, int slot)
{
	if (!state->shm_base || slot < 0 || slot >= NVKVM_SHM_NSLOTS)
		return NULL;
	return (void __force *)state->shm_base + (size_t)slot * state->slot_size;
}

/* ── In-flight request helpers ───────────────────────────────────────────── */

static struct nvkvm_inflight *inflight_alloc(__u32 req_id)
{
	struct nvkvm_inflight *inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf)
		return NULL;
	inf->req_id = req_id;
	init_completion(&inf->done);
	return inf;
}

static void inflight_enqueue(struct nvkvm_state *state,
			     struct nvkvm_inflight *inf)
{
	unsigned long flags;
	spin_lock_irqsave(&state->inflight_lock, flags);
	list_add_tail(&inf->list, &state->inflight_list);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}


static void inflight_dequeue(struct nvkvm_state *state,
			     struct nvkvm_inflight *inf)
{
	unsigned long flags;
	spin_lock_irqsave(&state->inflight_lock, flags);
	list_del(&inf->list);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}

/*
 * Maximum size of a response message (nvkvm_hdr + largest resp struct).
 * nvkvm_resp_list_nvidia_devices is the largest: 8 + 32*16 = 520 bytes.
 * Round up to 512 to cover all current responses; LIST_NVIDIA_DEVICES uses
 * a dedicated larger buffer allocated at probe time.
 */
#define NVKVM_RESP_BUF_SIZE  512

/* ── VQ_TX completion callback — called from softirq context ─────────────── */
/*
 * QEMU writes the response into the IN sg of the TX element and pushes it
 * back.  The data pointer we passed to virtqueue_add_sgs() is the inflight
 * record, so virtqueue_get_buf() returns inf directly.
 */
static void nvkvm_tx_done_callback(struct virtqueue *vq)
{
	struct nvkvm_inflight *inf;
	unsigned int len;

	while ((inf = virtqueue_get_buf(vq, &len)) != NULL) {
		struct nvkvm_hdr *hdr = inf->resp_buf;

		if (!hdr) {
			pr_warn("nvkvm: tx_done: null resp_buf\n");
			complete(&inf->done);
			continue;
		}

		switch (le32_to_cpu(hdr->type)) {
		case NVKVM_REQ_OPEN: {
			struct nvkvm_resp_open *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->fd_token);
			break;
		}
		case NVKVM_REQ_CLOSE: {
			struct nvkvm_resp_close *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_IOCTL: {
			struct nvkvm_resp_ioctl *resp = (void *)(hdr + 1);
			inf->retval = le64_to_cpu(resp->retval);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_MMAP: {
			struct nvkvm_resp_mmap *resp = (void *)(hdr + 1);
			inf->retval = le64_to_cpu(resp->gpa_base);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_MUNMAP: {
			struct nvkvm_resp_munmap *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_OPEN_NVIDIA_HANDLE: {
			struct nvkvm_resp_open_nvidia_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->handle_id);
			break;
		}
		case NVKVM_REQ_OPEN_MEMORY_HANDLE: {
			struct nvkvm_resp_open_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->handle_id);
			break;
		}
		case NVKVM_REQ_CLOSE_HANDLE: {
			struct nvkvm_resp_close_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_CREATE_ISOLATE: {
			struct nvkvm_resp_create_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			inf->retval = le32_to_cpu(resp->isolate_id);
			break;
		}
		case NVKVM_REQ_KILL_ISOLATE: {
			struct nvkvm_resp_kill_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_COPY_HANDLE_TO_ISOLATE: {
			struct nvkvm_resp_copy_handle_to_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE: {
			struct nvkvm_resp_close_handle_on_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_IOCTL_ON_ISOLATE: {
			struct nvkvm_resp_ioctl_on_isolate *resp = (void *)(hdr + 1);
			inf->retval     = le64_to_cpu(resp->retval);
			inf->status     = le32_to_cpu(resp->status);
			inf->nvstatus   = le32_to_cpu(resp->nvstatus);
			inf->fault_addr = le64_to_cpu(resp->fault_addr);
			break;
		}
		case NVKVM_REQ_MMAP_ON_ISOLATE: {
			struct nvkvm_resp_mmap_on_isolate *resp = (void *)(hdr + 1);
			inf->retval = le64_to_cpu(resp->gpa_base);
			inf->status = le32_to_cpu(resp->status);
			/* store mmap_token in nvstatus field (repurposed) */
			inf->nvstatus = le32_to_cpu(resp->mmap_token);
			break;
		}
		case NVKVM_REQ_MUNMAP_ON_ISOLATE: {
			struct nvkvm_resp_munmap_on_isolate *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_WRITE_MEMORY_HANDLE: {
			struct nvkvm_resp_write_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_READ_MEMORY_HANDLE: {
			struct nvkvm_resp_read_memory_handle *resp = (void *)(hdr + 1);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		default:
			pr_warn("nvkvm: tx_done: unknown type %u\n",
				le32_to_cpu(hdr->type));
			inf->status = EINVAL;
			break;
		}
		complete(&inf->done);
	}
}

/* ── VQ_RX callback — unused (responses come back on VQ_TX) ─────────────── */

static void nvkvm_rx_callback(struct virtqueue *vq)
{
	/* vq_rx has no pre-posted buffers; this callback is never triggered. */
	(void)vq;
}

/* ── VQ_EVT callback — async poll events from host ───────────────────────── */

static void nvkvm_evt_callback(struct virtqueue *vq)
{
	struct nvkvm_evt_poll *evt;
	unsigned int len;

	/* TODO: look up fd_token in session table, wake poll queue */
	while ((evt = virtqueue_get_buf(vq, &len)) != NULL)
		kfree(evt);
}

/* ── Generic synchronous send ─────────────────────────────────────────────── */

/*
 * nvkvm_send_sync — place a request on VQ_TX and block for the response.
 *
 * @req_buf:  buffer holding nvkvm_hdr + request payload
 * @req_len:  total length
 * @inf:      pre-allocated inflight record (req_id already set)
 *
 * Returns 0 on transport success (inf->status holds the operation result).
 */
int nvkvm_send_sync(struct nvkvm_state *state,
		    void *req_buf, size_t req_len,
		    struct nvkvm_inflight *inf)
{
	struct scatterlist out_sg, in_sg;
	struct scatterlist *sgs[2] = { &out_sg, &in_sg };
	int ret;

	/* Allocate the IN buffer that QEMU will write the response into. */
	inf->resp_buf = kzalloc(NVKVM_RESP_BUF_SIZE, GFP_KERNEL);
	if (!inf->resp_buf)
		return -ENOMEM;

	sg_init_one(&out_sg, req_buf, req_len);
	sg_init_one(&in_sg, inf->resp_buf, NVKVM_RESP_BUF_SIZE);

	inflight_enqueue(state, inf);

	/*
	 * Pass inf as the data cookie so virtqueue_get_buf() in the TX-done
	 * callback returns the inflight record directly.
	 */
	ret = virtqueue_add_sgs(state->vq_tx, sgs, 1, 1, inf, GFP_KERNEL);
	if (ret) {
		inflight_dequeue(state, inf);
		kfree(inf->resp_buf);
		inf->resp_buf = NULL;
		return ret;
	}
	virtqueue_kick(state->vq_tx);

	wait_for_completion(&inf->done);
	inflight_dequeue(state, inf);

	kfree(inf->resp_buf);
	inf->resp_buf = NULL;
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int nvkvm_virtio_open(int dev_id, unsigned int flags, unsigned int session_id,
		      struct nvkvm_resp_open *resp_out)
{
	struct {
		struct nvkvm_hdr      hdr;
		struct nvkvm_req_open req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc(req_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type          = cpu_to_le32(NVKVM_REQ_OPEN);
	msg->hdr.req_id        = cpu_to_le32(req_id);
	msg->req.dev_id        = cpu_to_le32(dev_id);
	msg->req.flags         = cpu_to_le32(flags);
	msg->req.session_id    = cpu_to_le32(session_id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0) {
		resp_out->fd_token = (__u32)inf->retval;
		resp_out->status   = inf->status;
	}
	kfree(inf);
	kfree(msg);
	return ret;
}

int nvkvm_virtio_close(__u32 fd_token, struct nvkvm_resp_close *resp_out)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_req_close req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc(req_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type    = cpu_to_le32(NVKVM_REQ_CLOSE);
	msg->hdr.req_id  = cpu_to_le32(req_id);
	msg->req.fd_token = cpu_to_le32(fd_token);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0)
		resp_out->status = inf->status;
	kfree(inf);
	kfree(msg);
	return ret;
}

long nvkvm_virtio_ioctl(struct nvkvm_fd_ctx *ctx,
			unsigned int cmd,
			void *params_buf, size_t param_size,
			void *aux_buf, size_t aux_size)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_req_ioctl req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int shm_slot = -1;
	int shm_aux_slot = -1;
	long ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc(req_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	/* Copy parameter blob into shared memory slot */
	if (param_size > 0) {
		void *slot_ptr;
		shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0) {
			ret = -ENOSPC;
			goto out;
		}
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		if (!slot_ptr) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(slot_ptr, params_buf, param_size);
	}

	/* Copy auxiliary buffer (secondary params) into a separate slot */
	if (aux_size > 0 && aux_buf) {
		void *slot_ptr;
		shm_aux_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_aux_slot < 0) {
			ret = -ENOSPC;
			goto out_slot;
		}
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		if (!slot_ptr) {
			ret = -ENOMEM;
			goto out_slot;
		}
		memcpy(slot_ptr, aux_buf, aux_size);
	}

	wmb();   /* ensure all slot writes visible before kick */

	msg->hdr.type          = cpu_to_le32(NVKVM_REQ_IOCTL);
	msg->hdr.req_id        = cpu_to_le32(req_id);
	msg->req.fd_token      = cpu_to_le32(ctx->fd_token);
	msg->req.cmd           = cpu_to_le32(cmd);
	msg->req.param_size    = cpu_to_le32((__u32)param_size);
	msg->req.shm_slot      = cpu_to_le32((__u32)shm_slot);
	msg->req.aux_size      = cpu_to_le32((__u32)aux_size);
	msg->req.shm_aux_slot  = cpu_to_le32((__u32)(shm_aux_slot >= 0 ?
						      shm_aux_slot : 0));
	msg->req.session_id    = cpu_to_le32((__u32)ctx->session->id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret < 0)
		goto out_aux_slot;

	/*
	 * Always copy back from shared memory, even when the driver returned an
	 * error.  The NVIDIA RM updates response fields in the params struct
	 * regardless of success/failure (e.g. NV_ESC_CHECK_VERSION_STR fills
	 * version_string and returns EINVAL; NV_ESC_RM_ALLOC writes h_object_new).
	 */
	if (param_size > 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		rmb();   /* ensure host writes are visible */
		memcpy(params_buf, slot_ptr, param_size);
	}
	if (aux_size > 0 && aux_buf && shm_aux_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		rmb();
		memcpy(aux_buf, slot_ptr, aux_size);
	}

	if (inf->status) {
		ret = -(long)inf->status;
		goto out_aux_slot;
	}
	ret = (long)inf->retval;

out_aux_slot:
	if (shm_aux_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_aux_slot);
out_slot:
	if (shm_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_slot);
out:
	kfree(inf);
	kfree(msg);
	return ret;
}

/* ── Version negotiation ──────────────────────────────────────────────────── */

int nvkvm_negotiate_version(struct nvkvm_state *state)
{
	struct nvkvm_shm_ctrl __iomem *ctrl;
	__u32 host_version;

	if (!state->shm_base)
		return -ENODEV;

	ctrl = (struct nvkvm_shm_ctrl __iomem *)state->shm_base;
	host_version = ioread32(&ctrl->proto_version);

	if (host_version != NVKVM_PROTO_VERSION) {
		pr_err("nvkvm: protocol version mismatch: host=%u guest=%u\n",
		       host_version, NVKVM_PROTO_VERSION);
		return -EPROTO;
	}

	state->slot_size = ioread32(&ctrl->slot_size);
	if (state->slot_size < NVKVM_SHM_SLOT_MIN_SIZE ||
	    !is_power_of_2(state->slot_size)) {
		pr_err("nvkvm: invalid slot_size %zu from host\n",
		       state->slot_size);
		return -EINVAL;
	}

	memcpy_fromio(state->driver_version, ctrl->driver_version,
		      sizeof(state->driver_version));
	state->driver_version[sizeof(state->driver_version) - 1] = '\0';

	pr_info("nvkvm: host NVIDIA driver %s, slot_size=%zu\n",
		state->driver_version, state->slot_size);
	return 0;
}

/* ── Virtio device init/fini ─────────────────────────────────────────────── */

int nvkvm_virtio_init(struct virtio_device *vdev, struct nvkvm_state *state)
{
	struct virtqueue *vqs[NVKVM_NUM_VQS];
	vq_callback_t *cbs[NVKVM_NUM_VQS] = {
		nvkvm_tx_done_callback, /* TX: called when QEMU posts response  */
		nvkvm_rx_callback,      /* RX: unused placeholder               */
		nvkvm_evt_callback,     /* EVT                                  */
	};
	static const char *names[NVKVM_NUM_VQS] = {
		"nvkvm-tx", "nvkvm-rx", "nvkvm-evt",
	};
	int ret;
	u64 shm_addr, shm_len;

	vdev->priv = state;
	state->vdev = vdev;

	spin_lock_init(&state->inflight_lock);
	INIT_LIST_HEAD(&state->inflight_list);
	atomic_set(&state->next_req_id, 0);
	spin_lock_init(&state->slot_lock);
	bitmap_zero(state->slot_bitmap, NVKVM_SHM_NSLOTS);
	/* Mark slot 0 (ctrl) as permanently allocated */
	set_bit(NVKVM_SHM_CTRL_SLOT, state->slot_bitmap);

	ret = virtio_find_vqs(vdev, NVKVM_NUM_VQS, vqs, cbs, names, NULL);
	if (ret) {
		dev_err(&vdev->dev, "nvkvm: find_vqs failed: %d\n", ret);
		return ret;
	}
	state->vq_tx  = vqs[NVKVM_VQ_TX];
	state->vq_rx  = vqs[NVKVM_VQ_RX];
	state->vq_evt = vqs[NVKVM_VQ_EVT];

	/* Map shared memory BAR 0 */
	shm_addr = virtio_cread64(vdev, offsetof(struct nvkvm_virtio_config,
						 shm_base));
	shm_len  = virtio_cread64(vdev, offsetof(struct nvkvm_virtio_config,
						  shm_len));

	if (shm_len < NVKVM_SHM_SLOT_MIN_SIZE) {
		dev_err(&vdev->dev,
			"nvkvm: shared memory region too small: %llu\n",
			shm_len);
		vdev->config->del_vqs(vdev);
		return -EINVAL;
	}

	state->shm_base = ioremap(shm_addr, shm_len);
	if (!state->shm_base) {
		dev_err(&vdev->dev, "nvkvm: failed to map shared memory\n");
		vdev->config->del_vqs(vdev);
		return -ENOMEM;
	}
	state->shm_size  = shm_len;
	state->slot_size = NVKVM_SHM_SLOT_DEFAULT_SIZE; /* overridden by negotiate */

	state->mmap_window_gpa_base = virtio_cread64(vdev,
		offsetof(struct nvkvm_virtio_config, mmap_win_gpa));
	state->mmap_window_len = virtio_cread64(vdev,
		offsetof(struct nvkvm_virtio_config, mmap_win_len));

	virtio_device_ready(vdev);
	return 0;
}

void nvkvm_virtio_fini(struct nvkvm_state *state)
{
	if (state->shm_base) {
		iounmap(state->shm_base);
		state->shm_base = NULL;
	}
	if (state->vdev) {
		state->vdev->config->reset(state->vdev);
		state->vdev->config->del_vqs(state->vdev);
		state->vdev = NULL;
	}
}

/* ── Isolate-aware API ────────────────────────────────────────────────────── */

/*
 * Simple helper: send a fixed-size request, wait for response.
 * Extracts inf->retval and inf->status for the caller.
 */
static int simple_req(__u32 req_type,
		      void *req_buf, size_t req_len,
		      __u64 *retval_out)
{
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int ret;

	/* The hdr is at the start of req_buf; caller has already filled it */
	inf = inflight_alloc(req_id);
	if (!inf)
		return -ENOMEM;

	((struct nvkvm_hdr *)req_buf)->type   = cpu_to_le32(req_type);
	((struct nvkvm_hdr *)req_buf)->req_id = cpu_to_le32(req_id);

	ret = nvkvm_send_sync(&nvkvm, req_buf, req_len, inf);
	if (ret == 0) {
		if (inf->status)
			ret = -(int)inf->status;
		else if (retval_out)
			*retval_out = inf->retval;
	}
	kfree(inf);
	return ret;
}

int nvkvm_virtio_open_nvidia_handle(int dev_id, unsigned int flags,
				    unsigned int session_id,
				    __u32 *handle_id_out)
{
	struct {
		struct nvkvm_hdr                   hdr;
		struct nvkvm_req_open_nvidia_handle req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.dev_id     = cpu_to_le32(dev_id);
	msg.req.flags      = cpu_to_le32(flags);
	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_OPEN_NVIDIA_HANDLE, &msg, sizeof(msg), &retval);
	if (ret == 0 && handle_id_out)
		*handle_id_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_create_isolate(unsigned int session_id, __u32 *isolate_id_out)
{
	struct {
		struct nvkvm_hdr              hdr;
		struct nvkvm_req_create_isolate req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_CREATE_ISOLATE, &msg, sizeof(msg), &retval);
	if (ret == 0 && isolate_id_out)
		*isolate_id_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_copy_handle_to_isolate(__u32 handle_id, __u32 isolate_id)
{
	struct {
		struct nvkvm_hdr                      hdr;
		struct nvkvm_req_copy_handle_to_isolate req;
	} msg = {};

	msg.req.handle_id  = cpu_to_le32(handle_id);
	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_COPY_HANDLE_TO_ISOLATE,
			  &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_close_handle_on_isolate(__u32 handle_id, __u32 isolate_id)
{
	struct {
		struct nvkvm_hdr                       hdr;
		struct nvkvm_req_close_handle_on_isolate req;
	} msg = {};

	msg.req.handle_id  = cpu_to_le32(handle_id);
	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE,
			  &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_close_handle(__u32 handle_id)
{
	struct {
		struct nvkvm_hdr          hdr;
		struct nvkvm_req_close_handle req;
	} msg = {};

	msg.req.handle_id = cpu_to_le32(handle_id);

	return simple_req(NVKVM_REQ_CLOSE_HANDLE, &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_kill_isolate(__u32 isolate_id)
{
	struct {
		struct nvkvm_hdr            hdr;
		struct nvkvm_req_kill_isolate req;
	} msg = {};

	msg.req.isolate_id = cpu_to_le32(isolate_id);

	return simple_req(NVKVM_REQ_KILL_ISOLATE, &msg, sizeof(msg), NULL);
}

/*
 * nvkvm_virtio_ioctl_on_isolate — forward ioctl through the isolate path.
 *
 * VMA whitelist (all VMAs in the current mm) is collected here and sent in a
 * third shm slot.  On -EFAULT, *fault_addr_out is set to the faulting GVA so
 * the caller can map the region and retry.
 */
long nvkvm_virtio_ioctl_on_isolate(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size,
				   void *aux_buf, size_t aux_size,
				   __u32 flags,
				   __u64 *fault_addr_out)
{
	struct {
		struct nvkvm_hdr                 hdr;
		struct nvkvm_req_ioctl_on_isolate req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int shm_slot = -1, shm_aux_slot = -1, shm_vma_slot = -1;
	struct nvkvm_vma_entry *vma_buf = NULL;
	size_t vma_count = 0;
	long ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc(req_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	/* Param slot */
	if (param_size > 0) {
		void *slot_ptr;
		shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0) { ret = -ENOSPC; goto out; }
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		if (!slot_ptr) { ret = -ENOMEM; goto out; }
		memcpy(slot_ptr, params_buf, param_size);
	}

	/* Aux slot */
	if (aux_size > 0 && aux_buf) {
		void *slot_ptr;
		shm_aux_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_aux_slot < 0) { ret = -ENOSPC; goto out; }
		slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		if (!slot_ptr) { ret = -ENOMEM; goto out; }
		memcpy(slot_ptr, aux_buf, aux_size);
	}

	/* VMA whitelist slot — all VMAs in current mm */
	vma_buf = kzalloc(sizeof(*vma_buf) * NVKVM_MAX_VMA_ENTRIES, GFP_KERNEL);
	if (vma_buf) {
		struct mm_struct *mm = current->mm;
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);

		mmap_read_lock(mm);
		for_each_vma(vmi, vma) {
			if (vma_count >= NVKVM_MAX_VMA_ENTRIES)
				break;
			vma_buf[vma_count].start    = cpu_to_le64(vma->vm_start);
			vma_buf[vma_count].end      = cpu_to_le64(vma->vm_end);
			vma_buf[vma_count].prot     = cpu_to_le32(
				(vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
				(vma->vm_flags & VM_WRITE ? PROT_WRITE : 0) |
				(vma->vm_flags & VM_EXEC  ? PROT_EXEC  : 0));
			vma_buf[vma_count].reserved = 0;
			vma_count++;
		}
		mmap_read_unlock(mm);

		if (vma_count > 0) {
			size_t vma_bytes = vma_count * sizeof(*vma_buf);
			void *slot_ptr;
			shm_vma_slot = nvkvm_slot_alloc(&nvkvm);
			if (shm_vma_slot >= 0) {
				slot_ptr = nvkvm_slot_addr(&nvkvm, shm_vma_slot);
				if (slot_ptr)
					memcpy(slot_ptr, vma_buf, vma_bytes);
				else {
					nvkvm_slot_free(&nvkvm, shm_vma_slot);
					shm_vma_slot = -1;
					vma_count = 0;
				}
			} else {
				vma_count = 0;
			}
		}
		kfree(vma_buf);
	}

	wmb();

	msg->hdr.type   = cpu_to_le32(NVKVM_REQ_IOCTL_ON_ISOLATE);
	msg->hdr.req_id = cpu_to_le32(req_id);
	msg->req.isolate_id              = cpu_to_le32(ctx->session->isolate_id);
	msg->req.handle_id               = cpu_to_le32(ctx->handle_id);
	msg->req.cmd                     = cpu_to_le32(cmd);
	msg->req.param_size              = cpu_to_le32((__u32)param_size);
	msg->req.shm_slot                = cpu_to_le32((__u32)(shm_slot >= 0 ? shm_slot : 0));
	msg->req.aux_size                = cpu_to_le32((__u32)aux_size);
	msg->req.shm_aux_slot            = cpu_to_le32((__u32)(shm_aux_slot >= 0 ? shm_aux_slot : 0));
	msg->req.vma_whitelist_nentries  = cpu_to_le32((__u32)vma_count);
	msg->req.vma_whitelist_slot      = cpu_to_le32((__u32)(shm_vma_slot >= 0 ? shm_vma_slot : 0));
	msg->req.flags                   = cpu_to_le32(flags);
	msg->req.session_id              = cpu_to_le32((__u32)ctx->session->id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret < 0)
		goto out;

	/* Copy back params and aux from shared memory */
	if (param_size > 0 && shm_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		rmb();
		if (slot_ptr)
			memcpy(params_buf, slot_ptr, param_size);
	}
	if (aux_size > 0 && aux_buf && shm_aux_slot >= 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_aux_slot);
		rmb();
		if (slot_ptr)
			memcpy(aux_buf, slot_ptr, aux_size);
	}

	if (fault_addr_out)
		*fault_addr_out = inf->fault_addr;

	if (inf->status == EFAULT && inf->fault_addr) {
		ret = -EFAULT;
		goto out;
	}
	if (inf->status) {
		ret = -(long)inf->status;
		goto out;
	}
	ret = (long)inf->retval;

out:
	if (shm_vma_slot >= 0) nvkvm_slot_free(&nvkvm, shm_vma_slot);
	if (shm_aux_slot >= 0) nvkvm_slot_free(&nvkvm, shm_aux_slot);
	if (shm_slot >= 0)     nvkvm_slot_free(&nvkvm, shm_slot);
	kfree(inf);
	kfree(msg);
	return ret;
}

int nvkvm_virtio_munmap_on_isolate(__u32 isolate_id, __u32 mmap_token)
{
	struct {
		struct nvkvm_hdr                   hdr;
		struct nvkvm_req_munmap_on_isolate req;
	} msg = {};

	msg.req.isolate_id  = cpu_to_le32(isolate_id);
	msg.req.mmap_token  = cpu_to_le32(mmap_token);

	return simple_req(NVKVM_REQ_MUNMAP_ON_ISOLATE, &msg, sizeof(msg), NULL);
}

int nvkvm_virtio_open_memory_handle(unsigned int session_id, __u64 size,
				    __u32 *handle_id_out)
{
	struct {
		struct nvkvm_hdr                    hdr;
		struct nvkvm_req_open_memory_handle req;
	} msg = {};
	__u64 retval = 0;
	int ret;

	msg.req.size       = cpu_to_le64(size);
	msg.req.session_id = cpu_to_le32(session_id);

	ret = simple_req(NVKVM_REQ_OPEN_MEMORY_HANDLE, &msg, sizeof(msg), &retval);
	if (ret == 0 && handle_id_out)
		*handle_id_out = (__u32)retval;
	return ret;
}

int nvkvm_virtio_write_memory_handle(__u32 handle_id, __u64 offset,
				     int shm_slot, __u32 size)
{
	struct {
		struct nvkvm_hdr                      hdr;
		struct nvkvm_req_write_memory_handle  req;
	} msg = {};
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	struct nvkvm_inflight *inf;
	int ret;

	inf = inflight_alloc(req_id);
	if (!inf)
		return -ENOMEM;

	msg.hdr.type      = cpu_to_le32(NVKVM_REQ_WRITE_MEMORY_HANDLE);
	msg.hdr.req_id    = cpu_to_le32(req_id);
	msg.req.handle_id = cpu_to_le32(handle_id);
	msg.req.shm_slot  = cpu_to_le32((__u32)shm_slot);
	msg.req.offset    = cpu_to_le64(offset);
	msg.req.size      = cpu_to_le32(size);

	ret = nvkvm_send_sync(&nvkvm, &msg, sizeof(msg), inf);
	if (ret == 0 && inf->status)
		ret = -(int)inf->status;
	kfree(inf);
	return ret;
}

int nvkvm_virtio_read_memory_handle(__u32 handle_id, __u64 offset,
				    int shm_slot, __u32 size)
{
	struct {
		struct nvkvm_hdr                     hdr;
		struct nvkvm_req_read_memory_handle  req;
	} msg = {};
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	struct nvkvm_inflight *inf;
	int ret;

	inf = inflight_alloc(req_id);
	if (!inf)
		return -ENOMEM;

	msg.hdr.type      = cpu_to_le32(NVKVM_REQ_READ_MEMORY_HANDLE);
	msg.hdr.req_id    = cpu_to_le32(req_id);
	msg.req.handle_id = cpu_to_le32(handle_id);
	msg.req.shm_slot  = cpu_to_le32((__u32)shm_slot);
	msg.req.offset    = cpu_to_le64(offset);
	msg.req.size      = cpu_to_le32(size);

	ret = nvkvm_send_sync(&nvkvm, &msg, sizeof(msg), inf);
	if (ret == 0 && inf->status)
		ret = -(int)inf->status;
	kfree(inf);
	return ret;
}

int nvkvm_virtio_mmap_on_isolate(__u32 isolate_id, __u32 handle_id,
				 __u64 gva, __u64 offset, __u64 length,
				 __u32 prot, __u32 map_flags,
				 unsigned int session_id,
				 __u64 *gpa_base_out,
				 __u32 *mmap_token_out)
{
	struct {
		struct nvkvm_hdr                hdr;
		struct nvkvm_req_mmap_on_isolate req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int ret;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	inf = inflight_alloc(req_id);
	if (!inf) {
		kfree(msg);
		return -ENOMEM;
	}

	msg->hdr.type       = cpu_to_le32(NVKVM_REQ_MMAP_ON_ISOLATE);
	msg->hdr.req_id     = cpu_to_le32(req_id);
	msg->req.isolate_id = cpu_to_le32(isolate_id);
	msg->req.handle_id  = cpu_to_le32(handle_id);
	msg->req.gva        = cpu_to_le64(gva);
	msg->req.offset     = cpu_to_le64(offset);
	msg->req.length     = cpu_to_le64(length);
	msg->req.prot       = cpu_to_le32(prot);
	msg->req.map_flags  = cpu_to_le32(map_flags);
	msg->req.session_id = cpu_to_le32(session_id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret == 0) {
		if (inf->status) {
			ret = -(int)inf->status;
		} else {
			if (gpa_base_out)   *gpa_base_out   = inf->retval;
			if (mmap_token_out) *mmap_token_out = inf->nvstatus;
		}
	}
	kfree(inf);
	kfree(msg);
	return ret;
}
