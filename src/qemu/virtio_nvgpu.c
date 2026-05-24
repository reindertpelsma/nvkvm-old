/*
 * virtio_nvgpu.c — QEMU virtio-nvgpu device backend
 *
 * Implements the VirtIONvgpu QEMU device which processes requests from the
 * guest nvkvm-guest.ko kernel module and forwards them to the real NVIDIA
 * driver on the host.
 *
 * Request lifecycle:
 *   1. Guest writes request to VQ_TX and kicks the VQ.
 *   2. nvkvm_tx_handler() is called (QEMU IOThread or BH).
 *   3. Request is validated, dispatched, and the real ioctl is called.
 *   4. Response is posted to VQ_RX.
 *   5. Guest RX callback wakes the waiting task.
 *
 * Threading: all VQ callbacks run in QEMU's IOThread. Per-session and per-FD
 * structures are protected by their own mutexes for future multi-threaded
 * dispatch. Currently we hold the QEMU BQL across dispatch for simplicity
 * and will relax this later.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "virtio_nvgpu.h"

/* ── Device node paths on the host ──────────────────────────────────────── */

static const char *host_dev_path(int dev_id)
{
	static char buf[32];
	if (dev_id == NVKVM_DEV_CTL)
		return "/dev/nvidiactl";
	if (dev_id == NVKVM_DEV_UVM)
		return "/dev/nvidia-uvm";
	snprintf(buf, sizeof(buf), "/dev/nvidia%d", dev_id - 16);
	return buf;
}

/* ── Session management ──────────────────────────────────────────────────── */

struct nvkvm_session *nvkvm_session_find(VirtIONvgpu *nv, uint32_t session_id)
{
	struct nvkvm_session *s;
	TAILQ_FOREACH(s, &nv->sessions, link) {
		if (s->id == session_id)
			return s;
	}
	return NULL;
}

struct nvkvm_session *nvkvm_session_create(VirtIONvgpu *nv,
					   uint32_t session_id)
{
	struct nvkvm_session *s;

	s = g_new0(struct nvkvm_session, 1);
	s->id = session_id;
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->clients_lock, NULL);
	TAILQ_INIT(&s->fds);
	TAILQ_INIT(&s->mmaps);
	s->next_fd_token   = 1;
	s->next_mmap_token = 1;

	pthread_mutex_lock(&nv->sessions_lock);
	TAILQ_INSERT_TAIL(&nv->sessions, s, link);
	pthread_mutex_unlock(&nv->sessions_lock);
	return s;
}

struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
				      uint32_t fd_token)
{
	struct nvkvm_host_fd *hfd;
	TAILQ_FOREACH(hfd, &session->fds, link) {
		if (hfd->token == fd_token)
			return hfd;
	}
	return NULL;
}

uint32_t nvkvm_fd_alloc_token(struct nvkvm_session *session,
			      struct nvkvm_host_fd *hfd)
{
	uint32_t token = session->next_fd_token++;
	hfd->token = token;
	TAILQ_INSERT_TAIL(&session->fds, hfd, link);
	return token;
}

void nvkvm_fd_remove(struct nvkvm_session *session, uint32_t fd_token)
{
	struct nvkvm_host_fd *hfd = nvkvm_fd_lookup(session, fd_token);
	if (hfd) {
		TAILQ_REMOVE(&session->fds, hfd, link);
		close(hfd->fd);
		pthread_mutex_destroy(&hfd->clients_lock);
		g_free(hfd->clients);
		g_free(hfd);
	}
}

/* ── Shared memory slot validation ──────────────────────────────────────── */

static bool slot_valid(VirtIONvgpu *nv, uint32_t slot)
{
	return slot >= 1 && slot < NVKVM_SHM_NSLOTS;
}

static void *slot_ptr(VirtIONvgpu *nv, uint32_t slot)
{
	return (char *)nv->shm_base + (size_t)slot * nv->slot_size;
}

/* ── OPEN request handler ─────────────────────────────────────────────────── */

static void handle_open(VirtIONvgpu *nv, VirtQueue *vq,
			VirtQueueElement *elem,
			const struct nvkvm_hdr *hdr,
			const struct nvkvm_req_open *req)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_resp_open resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.req_id = hdr->req_id,
	};
	uint32_t dev_id    = le32_to_cpu(req->dev_id);
	uint32_t flags     = le32_to_cpu(req->flags);
	uint32_t session_id;
	struct nvkvm_session *session;
	struct nvkvm_host_fd *hfd;
	int fd;
	const char *path;

	/* Sanitize dev_id */
	if (dev_id != NVKVM_DEV_CTL && dev_id != NVKVM_DEV_UVM &&
	    !(dev_id >= NVKVM_DEV_GPU(0) &&
	      dev_id <= NVKVM_DEV_GPU(NV_MINOR_DEVICE_NUMBER_REGULAR_MAX))) {
		resp_msg.resp.status = cpu_to_le32(EINVAL);
		goto send;
	}

	/* Sanitize flags: only allow O_RDONLY / O_RDWR / O_CLOEXEC */
	flags &= O_RDONLY | O_RDWR | O_CLOEXEC;

	path = host_dev_path(dev_id);
	fd = open(path, (int)flags | O_CLOEXEC);
	if (fd < 0) {
		resp_msg.resp.status = cpu_to_le32(errno);
		goto send;
	}

	/*
	 * Find or create the session for the requesting guest process.
	 * The session_id in OPEN requests is the guest tgid (process ID).
	 * We use the tgid as a hint but assign our own stable session_id.
	 */
	session_id = le32_to_cpu(req->flags) >> 16; /* upper 16 bits = tgid hint */
	pthread_mutex_lock(&nv->sessions_lock);
	session = nvkvm_session_find(nv, session_id);
	if (!session) {
		pthread_mutex_unlock(&nv->sessions_lock);
		session = nvkvm_session_create(nv, session_id);
	} else {
		pthread_mutex_unlock(&nv->sessions_lock);
	}

	hfd = g_new0(struct nvkvm_host_fd, 1);
	hfd->fd         = fd;
	hfd->dev_id     = dev_id;
	hfd->session_id = session_id;
	pthread_mutex_init(&hfd->clients_lock, NULL);

	pthread_mutex_lock(&session->lock);
	nvkvm_fd_alloc_token(session, hfd);
	pthread_mutex_unlock(&session->lock);

	resp_msg.resp.fd_token = cpu_to_le32(hfd->token);
	resp_msg.resp.status   = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── CLOSE request handler ───────────────────────────────────────────────── */

static void handle_close(VirtIONvgpu *nv, VirtQueue *vq,
			 VirtQueueElement *elem,
			 const struct nvkvm_hdr *hdr,
			 const struct nvkvm_req_close *req)
{
	struct {
		struct nvkvm_hdr        hdr;
		struct nvkvm_resp_close resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.req_id = hdr->req_id,
	};
	uint32_t fd_token = le32_to_cpu(req->fd_token);
	struct nvkvm_session *session = NULL;

	/* Find the session owning this fd_token */
	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			struct nvkvm_host_fd *hfd = nvkvm_fd_lookup(s, fd_token);
			if (hfd) {
				session = s;
				break;
			}
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);

	if (!session) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	pthread_mutex_lock(&session->lock);
	nvkvm_fd_remove(session, fd_token);
	pthread_mutex_unlock(&session->lock);
	resp_msg.resp.status = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── IOCTL request handler ───────────────────────────────────────────────── */

static void handle_ioctl(VirtIONvgpu *nv, VirtQueue *vq,
			 VirtQueueElement *elem,
			 const struct nvkvm_hdr *hdr,
			 const struct nvkvm_req_ioctl *req)
{
	struct {
		struct nvkvm_hdr        hdr;
		struct nvkvm_resp_ioctl resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.req_id = hdr->req_id,
	};
	uint32_t fd_token    = le32_to_cpu(req->fd_token);
	uint32_t cmd         = le32_to_cpu(req->cmd);
	uint32_t param_size  = le32_to_cpu(req->param_size);
	uint32_t shm_slot    = le32_to_cpu(req->shm_slot);
	uint32_t session_id  = le32_to_cpu(req->session_id);
	struct nvkvm_session  *session;
	struct nvkvm_host_fd  *hfd;
	size_t expected_size;
	struct nvkvm_req_ctx  ctx = {0};
	long ret;

	/* Validate session */
	pthread_mutex_lock(&nv->sessions_lock);
	session = nvkvm_session_find(nv, session_id);
	pthread_mutex_unlock(&nv->sessions_lock);
	if (!session) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	/* Validate fd_token belongs to this session */
	pthread_mutex_lock(&session->lock);
	hfd = nvkvm_fd_lookup(session, fd_token);
	pthread_mutex_unlock(&session->lock);
	if (!hfd) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	/* Validate param_size against the known ABI size for this command */
	expected_size = nvkvm_ioctl_expected_param_size(cmd);
	if (expected_size == (size_t)-1) {
		resp_msg.resp.status = cpu_to_le32(ENOTTY);
		goto send;
	}
	if (param_size != expected_size) {
		error_report("nvkvm: ioctl cmd=0x%x: param_size=%u expected=%zu",
			     cmd, param_size, expected_size);
		resp_msg.resp.status = cpu_to_le32(EINVAL);
		goto send;
	}

	/* Validate shm_slot */
	if (param_size > 0) {
		if (!slot_valid(nv, shm_slot) ||
		    param_size > nv->slot_size) {
			resp_msg.resp.status = cpu_to_le32(EINVAL);
			goto send;
		}
	}

	ctx.nv         = nv;
	ctx.vq         = vq;
	ctx.elem       = elem;
	ctx.session    = session;
	ctx.hfd        = hfd;
	ctx.param_size = param_size;
	ctx.params_buf = param_size > 0 ? slot_ptr(nv, shm_slot) : NULL;

	/* Handle aux slot */
	if (le32_to_cpu(req->aux_size) > 0) {
		uint32_t aux_slot = le32_to_cpu(req->shm_aux_slot);
		uint32_t aux_size = le32_to_cpu(req->aux_size);
		if (!slot_valid(nv, aux_slot) || aux_size > nv->slot_size) {
			resp_msg.resp.status = cpu_to_le32(EINVAL);
			goto send;
		}
		ctx.aux_buf  = slot_ptr(nv, aux_slot);
		ctx.aux_size = aux_size;
	}

	/* Dispatch to the appropriate handler */
	ret = nvkvm_dispatch_ioctl(&ctx, cmd);
	if (ret < 0) {
		resp_msg.resp.status = cpu_to_le32((uint32_t)-ret);
		resp_msg.resp.retval = 0;
	} else {
		resp_msg.resp.status = 0;
		resp_msg.resp.retval = cpu_to_le64((uint64_t)ret);
	}
	/* params_buf was modified in-place in shared memory; no copy needed */

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── MMAP request handler ────────────────────────────────────────────────── */

static void handle_mmap(VirtIONvgpu *nv, VirtQueue *vq,
			VirtQueueElement *elem,
			const struct nvkvm_hdr *hdr,
			const struct nvkvm_req_mmap *req)
{
	struct {
		struct nvkvm_hdr       hdr;
		struct nvkvm_resp_mmap resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.req_id = hdr->req_id,
	};
	uint32_t fd_token = le32_to_cpu(req->fd_token);
	uint64_t offset   = le64_to_cpu(req->offset);
	uint64_t length   = le64_to_cpu(req->length);
	int      prot     = (int)le32_to_cpu(req->prot);
	int      flags    = MAP_SHARED;
	struct nvkvm_session *session = NULL;
	struct nvkvm_host_fd *hfd;
	struct nvkvm_mmap_region *region = NULL;
	int ret;

	/* Validate length */
	if (!length || length > SZ_1G || (length & ~PAGE_MASK)) {
		resp_msg.resp.status = cpu_to_le32(EINVAL);
		goto send;
	}

	/* Sanitize prot: only read/write allowed */
	prot &= PROT_READ | PROT_WRITE;

	/* Find session+fd */
	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			hfd = nvkvm_fd_lookup(s, fd_token);
			if (hfd) { session = s; break; }
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);
	if (!session || !hfd) {
		resp_msg.resp.status = cpu_to_le32(EBADF);
		goto send;
	}

	ret = nvkvm_mmap_create(nv, hfd, offset, length, prot, flags, &region);
	if (ret) {
		resp_msg.resp.status = cpu_to_le32((uint32_t)-ret);
		goto send;
	}

	ret = nvkvm_mmap_map_to_guest(nv, region);
	if (ret) {
		nvkvm_mmap_destroy(nv, region);
		resp_msg.resp.status = cpu_to_le32((uint32_t)-ret);
		goto send;
	}

	pthread_mutex_lock(&session->lock);
	region->token = session->next_mmap_token++;
	TAILQ_INSERT_TAIL(&session->mmaps, region, link);
	pthread_mutex_unlock(&session->lock);

	resp_msg.resp.gpa_base    = cpu_to_le64(region->guest_pa);
	resp_msg.resp.length      = cpu_to_le64(region->length);
	resp_msg.resp.mmap_token  = cpu_to_le32(region->token);
	resp_msg.resp.status      = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── MUNMAP request handler ──────────────────────────────────────────────── */

static void handle_munmap(VirtIONvgpu *nv, VirtQueue *vq,
			  VirtQueueElement *elem,
			  const struct nvkvm_hdr *hdr,
			  const struct nvkvm_req_munmap *req)
{
	struct {
		struct nvkvm_hdr         hdr;
		struct nvkvm_resp_munmap resp;
	} resp_msg = {
		.hdr.type   = hdr->type,
		.hdr.req_id = hdr->req_id,
	};
	uint32_t mmap_token = le32_to_cpu(req->mmap_token);
	struct nvkvm_mmap_region *region = NULL;

	pthread_mutex_lock(&nv->sessions_lock);
	{
		struct nvkvm_session *s;
		TAILQ_FOREACH(s, &nv->sessions, link) {
			struct nvkvm_mmap_region *r;
			TAILQ_FOREACH(r, &s->mmaps, link) {
				if (r->token == mmap_token) {
					region = r;
					TAILQ_REMOVE(&s->mmaps, r, link);
					break;
				}
			}
			if (region) break;
		}
	}
	pthread_mutex_unlock(&nv->sessions_lock);

	if (!region) {
		resp_msg.resp.status = cpu_to_le32(ENOENT);
		goto send;
	}

	nvkvm_mmap_unmap_from_guest(nv, region);
	nvkvm_mmap_destroy(nv, region);
	resp_msg.resp.status = 0;

send:
	iov_from_buf(elem->in_sg, elem->in_num, 0,
		     &resp_msg, sizeof(resp_msg));
	virtqueue_push(vq, elem, sizeof(resp_msg));
	virtio_notify(VIRTIO_DEVICE(nv), vq);
}

/* ── VQ_TX callback ──────────────────────────────────────────────────────── */

static void nvkvm_tx_handler(VirtIODevice *vdev, VirtQueue *vq)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(vdev);
	VirtQueueElement *elem;

	while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) != NULL) {
		struct nvkvm_hdr hdr;
		size_t hdr_len;

		hdr_len = iov_to_buf(elem->out_sg, elem->out_num,
				     0, &hdr, sizeof(hdr));
		if (hdr_len < sizeof(hdr)) {
			error_report("nvkvm: short request header");
			virtqueue_push(vq, elem, 0);
			g_free(elem);
			continue;
		}

		switch (le32_to_cpu(hdr.type)) {
		case NVKVM_REQ_OPEN: {
			struct nvkvm_req_open req;
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			handle_open(nv, vq, elem, &hdr, &req);
			break;
		}
		case NVKVM_REQ_CLOSE: {
			struct nvkvm_req_close req;
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			handle_close(nv, vq, elem, &hdr, &req);
			break;
		}
		case NVKVM_REQ_IOCTL: {
			struct nvkvm_req_ioctl req;
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			handle_ioctl(nv, vq, elem, &hdr, &req);
			break;
		}
		case NVKVM_REQ_MMAP: {
			struct nvkvm_req_mmap req;
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			handle_mmap(nv, vq, elem, &hdr, &req);
			break;
		}
		case NVKVM_REQ_MUNMAP: {
			struct nvkvm_req_munmap req;
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			handle_munmap(nv, vq, elem, &hdr, &req);
			break;
		}
		default:
			error_report("nvkvm: unknown request type %u",
				     le32_to_cpu(hdr.type));
			virtqueue_push(vq, elem, 0);
			break;
		}
		g_free(elem);
	}
}

/* ── Device realize / unrealize ──────────────────────────────────────────── */

static void virtio_nvgpu_device_realize(DeviceState *dev, Error **errp)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIONvgpu  *nv   = VIRTIO_NVGPU(dev);
	int fd;

	virtio_init(vdev, VIRTIO_ID_NVGPU, 0);

	nv->vq_tx  = virtio_add_queue(vdev, 256, nvkvm_tx_handler);
	nv->vq_rx  = virtio_add_queue(vdev, 256, NULL);
	nv->vq_evt = virtio_add_queue(vdev, 64,  NULL);

	/* Probe host NVIDIA driver version */
	fd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error_setg(errp,
			   "nvkvm: cannot open /dev/nvidiactl on host: %s",
			   strerror(errno));
		return;
	}
	{
		struct nv_ioctl_rm_api_version ver = {.cmd = 0};
		ioctl(fd, /* NV_ESC_CHECK_VERSION_STR */ _IOWR('F',
		      NV_ESC_CHECK_VERSION_STR, struct nv_ioctl_rm_api_version),
		      &ver);
		memcpy(nv->driver_version, ver.version_string,
		       sizeof(nv->driver_version));
	}
	close(fd);

	/* Allocate shared memory region */
	nv->slot_size = NVKVM_SHM_SLOT_DEFAULT_SIZE;
	nv->shm_size  = (size_t)NVKVM_SHM_NSLOTS * nv->slot_size;
	nv->shm_base  = mmap(NULL, nv->shm_size,
			     PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (nv->shm_base == MAP_FAILED) {
		error_setg(errp, "nvkvm: failed to allocate shared memory");
		return;
	}

	/* Fill in the control block */
	{
		struct nvkvm_shm_ctrl *ctrl = nv->shm_base;
		ctrl->proto_version = cpu_to_le32(NVKVM_PROTO_VERSION);
		ctrl->slot_size     = cpu_to_le32((uint32_t)nv->slot_size);
		ctrl->nslots        = cpu_to_le32(NVKVM_SHM_NSLOTS);
		memcpy(ctrl->driver_version, nv->driver_version,
		       sizeof(ctrl->driver_version));
	}

	pthread_mutex_init(&nv->sessions_lock, NULL);
	pthread_mutex_init(&nv->mmap_win_lock, NULL);
	TAILQ_INIT(&nv->sessions);

	/* TODO: register shm_base as a guest memory region via
	 * memory_region_init_ram_ptr() and expose via BAR.
	 * For now, shared memory is accessed via virtio descriptors pointing
	 * to guest RAM that both sides agree on. */
}

static void virtio_nvgpu_device_unrealize(DeviceState *dev)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(dev);

	if (nv->shm_base && nv->shm_base != MAP_FAILED)
		munmap(nv->shm_base, nv->shm_size);

	virtio_cleanup(VIRTIO_DEVICE(dev));
}

/* ── QEMU type registration ──────────────────────────────────────────────── */

static const TypeInfo virtio_nvgpu_info = {
	.name          = TYPE_VIRTIO_NVGPU,
	.parent        = TYPE_VIRTIO_DEVICE,
	.instance_size = sizeof(VirtIONvgpu),
	.class_init    = NULL,     /* filled below */
};

static void virtio_nvgpu_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize   = virtio_nvgpu_device_realize;
	dc->unrealize = virtio_nvgpu_device_unrealize;
}

static void virtio_nvgpu_register_types(void)
{
	TypeInfo info = virtio_nvgpu_info;
	info.class_init = virtio_nvgpu_class_init;
	type_register_static(&info);
}

type_init(virtio_nvgpu_register_types);
