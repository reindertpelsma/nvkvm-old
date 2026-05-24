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

static struct nvkvm_inflight *inflight_find(struct nvkvm_state *state,
					    __u32 req_id)
{
	struct nvkvm_inflight *inf;
	list_for_each_entry(inf, &state->inflight_list, list) {
		if (inf->req_id == req_id)
			return inf;
	}
	return NULL;
}

static void inflight_dequeue(struct nvkvm_state *state,
			     struct nvkvm_inflight *inf)
{
	unsigned long flags;
	spin_lock_irqsave(&state->inflight_lock, flags);
	list_del(&inf->list);
	spin_unlock_irqrestore(&state->inflight_lock, flags);
}

/* ── VQ_RX callback — called from softirq context ────────────────────────── */

static void nvkvm_rx_callback(struct virtqueue *vq)
{
	struct nvkvm_state *state = vq->vdev->priv;
	void *buf;
	unsigned int len;

	while ((buf = virtqueue_get_buf(vq, &len)) != NULL) {
		struct nvkvm_hdr *hdr = buf;
		unsigned long flags;
		struct nvkvm_inflight *inf;

		spin_lock_irqsave(&state->inflight_lock, flags);
		inf = inflight_find(state, le32_to_cpu(hdr->req_id));
		spin_unlock_irqrestore(&state->inflight_lock, flags);

		if (!inf) {
			pr_warn("nvkvm: rx: no inflight for req_id %u\n",
				le32_to_cpu(hdr->req_id));
			kfree(buf);
			continue;
		}

		switch (le32_to_cpu(hdr->type)) {
		case NVKVM_REQ_OPEN: {
			struct nvkvm_resp_open *resp =
				(void *)((char *)buf + sizeof(*hdr));
			inf->status = le32_to_cpu(resp->status);
			/* fd_token is stored via the req wrapper, see below */
			inf->retval = le32_to_cpu(resp->fd_token);
			break;
		}
		case NVKVM_REQ_CLOSE: {
			struct nvkvm_resp_close *resp =
				(void *)((char *)buf + sizeof(*hdr));
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_IOCTL: {
			struct nvkvm_resp_ioctl *resp =
				(void *)((char *)buf + sizeof(*hdr));
			inf->retval = le64_to_cpu(resp->retval);
			inf->status = le32_to_cpu(resp->status);
			/* updated params already in shared mem slot */
			break;
		}
		case NVKVM_REQ_MMAP: {
			struct nvkvm_resp_mmap *resp =
				(void *)((char *)buf + sizeof(*hdr));
			inf->retval = le64_to_cpu(resp->gpa_base);
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		case NVKVM_REQ_MUNMAP: {
			struct nvkvm_resp_munmap *resp =
				(void *)((char *)buf + sizeof(*hdr));
			inf->status = le32_to_cpu(resp->status);
			break;
		}
		default:
			pr_warn("nvkvm: rx: unknown type %u\n",
				le32_to_cpu(hdr->type));
			inf->status = EINVAL;
			break;
		}

		complete(&inf->done);
		kfree(buf);
	}
}

/* ── VQ_EVT callback — async poll events from host ───────────────────────── */

static void nvkvm_evt_callback(struct virtqueue *vq)
{
	struct nvkvm_state *state = vq->vdev->priv;
	struct nvkvm_evt_poll *evt;
	unsigned int len;

	while ((evt = virtqueue_get_buf(vq, &len)) != NULL) {
		/* TODO: look up fd_token in session table, wake poll queue */
		(void)evt;
		kfree(evt);
	}
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
static int nvkvm_send_sync(struct nvkvm_state *state,
			   void *req_buf, size_t req_len,
			   struct nvkvm_inflight *inf)
{
	struct scatterlist sg;
	int ret;

	sg_init_one(&sg, req_buf, req_len);
	inflight_enqueue(state, inf);

	ret = virtqueue_add_outbuf(state->vq_tx, &sg, 1, req_buf, GFP_KERNEL);
	if (ret) {
		inflight_dequeue(state, inf);
		return ret;
	}
	virtqueue_kick(state->vq_tx);

	wait_for_completion(&inf->done);
	inflight_dequeue(state, inf);
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int nvkvm_virtio_open(int dev_id, unsigned int flags,
		      struct nvkvm_resp_open *resp_out)
{
	extern struct nvkvm_state nvkvm;
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

	msg->hdr.type   = cpu_to_le32(NVKVM_REQ_OPEN);
	msg->hdr.req_id = cpu_to_le32(req_id);
	msg->req.dev_id = cpu_to_le32(dev_id);
	msg->req.flags  = cpu_to_le32(flags);

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
	extern struct nvkvm_state nvkvm;
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
			void *params_buf, size_t param_size)
{
	extern struct nvkvm_state nvkvm;
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_req_ioctl req;
	} *msg;
	struct nvkvm_inflight *inf;
	__u32 req_id = atomic_inc_return(&nvkvm.next_req_id);
	int shm_slot = -1;
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
		/*
		 * Ensure the write is visible to the host before the
		 * virtqueue kick.
		 */
		wmb();
	}

	msg->hdr.type          = cpu_to_le32(NVKVM_REQ_IOCTL);
	msg->hdr.req_id        = cpu_to_le32(req_id);
	msg->req.fd_token      = cpu_to_le32(ctx->fd_token);
	msg->req.cmd           = cpu_to_le32(cmd);
	msg->req.param_size    = cpu_to_le32((__u32)param_size);
	msg->req.shm_slot      = cpu_to_le32((__u32)shm_slot);
	msg->req.session_id    = cpu_to_le32((__u32)ctx->session->id);

	ret = nvkvm_send_sync(&nvkvm, msg, sizeof(*msg), inf);
	if (ret < 0)
		goto out_slot;

	if (inf->status) {
		ret = -inf->status;
		goto out_slot;
	}

	/* Copy updated params back from shared memory slot */
	if (param_size > 0) {
		void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
		rmb();   /* ensure host writes are visible */
		memcpy(params_buf, slot_ptr, param_size);
	}
	ret = (long)inf->retval;

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
		NULL,                /* TX: we kick, no callback needed      */
		nvkvm_rx_callback,   /* RX                                    */
		nvkvm_evt_callback,  /* EVT                                   */
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
	shm_addr = virtio_cread64(vdev, offsetof(struct virtio_device_config,
						 shm_base));
	shm_len  = virtio_cread64(vdev, offsetof(struct virtio_device_config,
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
