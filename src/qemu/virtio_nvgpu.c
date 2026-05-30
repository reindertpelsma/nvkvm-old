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
#include "hw/boards.h"   /* current_machine->ram_size (#55 GPA-overlap guard) */
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "block/thread-pool.h"
#include "block/aio.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "virtio_nvgpu.h"
#include <dirent.h>

/*
 * host_dev_path was used by the legacy handle_open() that opened
 * /dev/nvidia* in QEMU's process. Step 3 moved opens to the stub.
 * nvkvm_handle_open_nvidia() in nvkvm_handle.c has its own path table.
 * Kept under #if 0 with the rest of the legacy handlers as a tombstone.
 */
#if 0
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
#endif

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

/*
 * #80 (audit H-2/H-3): destroy a session whose last isolate has been killed.
 * Unlinks it from the table first (so no later lookup races a free), then
 * reclaims everything the guest is no longer able to close itself:
 *   - all host /dev/nvidia* + memfd fds for the session (force, ignoring
 *     isolate_refcount — the isolates are gone), which releases the kernel RM
 *     objects + GPU memory;
 *   - the QEMU-side RM object graph (clients[]);
 *   - the legacy fd / mmap lists (empty in the handle-based model, drained
 *     defensively);
 *   - the session struct + its mutexes.
 *
 * Safe to free here: every control request runs serialised on the single TX
 * virtqueue thread, and we are only called once nisolates == 0.  A pooled
 * IOCTL worker (NVKVM_REQ_IOCTL_ON_ISOLATE) may still be unwinding after
 * nvkvm_isolate_kill (which joins the isolate's reader thread, not the pool
 * workers), but the worker only touches the static handle/isolate tables and
 * its own stack — it never dereferences a nvkvm_session — so freeing the
 * session struct here cannot UAF it.
 */
void nvkvm_session_destroy(VirtIONvgpu *nv, struct nvkvm_session *session)
{
	if (!session)
		return;

	pthread_mutex_lock(&nv->sessions_lock);
	TAILQ_REMOVE(&nv->sessions, session, link);
	pthread_mutex_unlock(&nv->sessions_lock);

	/* Force-close host fds → releases kernel RM objects + GPU memory. */
	nvkvm_handle_close_session(&nv->handles, session->id);

	/* Free the QEMU-side RM object graph. */
	pthread_mutex_lock(&session->clients_lock);
	for (int i = 0; i < session->nclients; i++) {
		if (session->clients[i]) {
			nvkvm_client_free(session, session->clients[i]);
			session->clients[i] = NULL;
		}
	}
	session->nclients = 0;
	pthread_mutex_unlock(&session->clients_lock);

	/* Drain the legacy fd list (dead in the handle model; defensive). */
	while (!TAILQ_EMPTY(&session->fds)) {
		struct nvkvm_host_fd *hfd = TAILQ_FIRST(&session->fds);
		TAILQ_REMOVE(&session->fds, hfd, link);
		if (hfd->fd >= 0)
			close(hfd->fd);
		g_free(hfd->clients);
		pthread_mutex_destroy(&hfd->clients_lock);
		g_free(hfd);
	}
	while (!TAILQ_EMPTY(&session->mmaps)) {
		struct nvkvm_mmap_region *mr = TAILQ_FIRST(&session->mmaps);
		TAILQ_REMOVE(&session->mmaps, mr, link);
		g_free(mr);
	}

	pthread_mutex_destroy(&session->lock);
	pthread_mutex_destroy(&session->clients_lock);
	g_free(session);
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

/*
 * Audit C-1: a guest-controlled `size` (param_size/aux_size/req->size, up to the
 * stub's 256 KiB cap) must NEVER exceed the 64 KiB slot it indexes, or the
 * subsequent send/recv/pread on slot_ptr() over-reads/over-writes past the slot
 * and past the 16 MiB shm region — a guest-driven OOB R/W in the privileged VMM.
 * Every live handler must obtain its shm pointer through this bounded helper,
 * which returns NULL unless the slot is valid AND [slot, slot+size) fits inside
 * both the slot and the whole shm region.  (The legacy path had this check; the
 * thread-pool/memory/realize paths lost it.)
 */
static void *slot_blob(VirtIONvgpu *nv, uint32_t slot, uint64_t size)
{
	uint64_t base;
	if (!slot_valid(nv, slot))
		return NULL;
	if (size > nv->slot_size)
		return NULL;
	base = (uint64_t)slot * nv->slot_size;
	if (base + size > nv->shm_size)        /* defense-in-depth vs the last slot */
		return NULL;
	return slot_ptr(nv, slot);
}

/*
 * Legacy NVKVM_REQ_OPEN / _CLOSE / _IOCTL / _MMAP / _MUNMAP request handlers.
 * These are dead code as of Step 3d.1 (guest module no longer sends these
 * request types). Kept under #if 0 as a tombstone until Step 3d.3 deletes
 * them along with the underlying nvkvm_host_fd / session->fds machinery.
 */
#if 0

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
		.hdr.txn_id = hdr->txn_id,
	};
	uint32_t dev_id    = le32_to_cpu(req->dev_id);
	uint32_t flags     = le32_to_cpu(req->flags);
	uint32_t session_id = le32_to_cpu(req->session_id);
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

	/* Find or create the session for the requesting guest process. */
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
	NVKVM_DBG( "nvkvm: open: dev_id=%u session=%u token=%u host_fd=%d\n",
		dev_id, session->id, hfd->token, hfd->fd);

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
		.hdr.txn_id = hdr->txn_id,
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
		.hdr.txn_id = hdr->txn_id,
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
		.hdr.txn_id = hdr->txn_id,
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

	/* Validate length: must be non-zero, ≤1 GiB, page-aligned */
	if (!length || length > (1UL << 30) || (length & (4096UL - 1))) {
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
		.hdr.txn_id = hdr->txn_id,
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
#endif /* legacy NVKVM_REQ_OPEN/CLOSE/IOCTL/MMAP/MUNMAP handlers */

/* ── VQ_TX callback ──────────────────────────────────────────────────────── */

/*
 * Asynchronous IOCTL_ON_ISOLATE dispatch.
 *
 * IOCTL_ON_ISOLATE is the hot path: a CUDA process issues thousands of them,
 * and each blocks until the per-isolate stub round-trip (or a UVM ioctl in
 * QEMU's own process) completes.  Running it inline in nvkvm_tx_handler would
 * block the single virtio TX thread, so a second concurrent guest process
 * whose request sits behind it in the ring is starved — and if one isolate's
 * stub wedges, every other guest hangs forever in wait_for_completion.
 *
 * Instead we offload each IOCTL_ON_ISOLATE to QEMU's thread pool: the worker
 * runs the (blocking) handler off the main loop, and the completion callback
 * — which runs back on the device AioContext, BQL held — pushes the response
 * onto the virtqueue.  The TX handler returns immediately to pop the next
 * request, so independent isolates run truly in parallel and a wedged isolate
 * no longer starves the others.  Responses may complete out of order; the
 * guest demuxes by txn_id (see nvkvm_tx_done_callback), so that is fine.
 *
 * Only IOCTL_ON_ISOLATE is offloaded.  MMAP/REALIZE stay synchronous: they
 * touch KVM memslots (KVM_SET_USER_MEMORY_REGION), which we keep on the main
 * loop, and they are infrequent (device bring-up, not the compute hot path).
 */
struct nvkvm_ioctl_work {
	VirtIONvgpu                       *nv;
	VirtQueue                         *vq;
	VirtQueueElement                  *elem;
	struct nvkvm_hdr                   hdr;
	struct nvkvm_req_ioctl_on_isolate  req;
	void                              *param_buf;   /* shm — stable */
	void                              *aux_buf;     /* shm — stable */
	struct nvkvm_resp_ioctl_on_isolate resp;
};

/* Worker thread: run the blocking ioctl handler off the main loop. */
static int nvkvm_ioctl_work_fn(void *opaque)
{
	struct nvkvm_ioctl_work *w = opaque;
	nvkvm_req_ioctl_on_isolate(w->nv, &w->req, &w->resp,
				   w->param_buf, w->aux_buf);
	return 0;
}

/* Completion: runs on the device AioContext (BQL held) — ring ops are safe. */
static void nvkvm_ioctl_work_done(void *opaque, int ret)
{
	struct nvkvm_ioctl_work *w = opaque;
	struct { struct nvkvm_hdr h;
		 struct nvkvm_resp_ioctl_on_isolate r; } out;
	out.h = w->hdr;
	out.r = w->resp;
	iov_from_buf(w->elem->in_sg, w->elem->in_num, 0, &out, sizeof(out));
	virtqueue_push(w->vq, w->elem, sizeof(out));
	virtio_notify(VIRTIO_DEVICE(w->nv), w->vq);
	g_free(w->elem);
	g_free(w);
}

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

		/* ── Isolate/handle request types ────────────────────────────────── */

#define ISOLATE_REQ(TYPE, req_t, resp_t, handler) \
		case TYPE: { \
			struct req_t  req  = {0}; \
			struct resp_t resp = {0}; \
			struct { struct nvkvm_hdr h; struct resp_t r; } out; \
			iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), \
				   &req, sizeof(req)); \
			handler(nv, &req, &resp); \
			out.h = hdr; out.r = resp; \
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out)); \
			virtqueue_push(vq, elem, sizeof(out)); \
			virtio_notify(VIRTIO_DEVICE(nv), vq); \
			break; \
		}

		ISOLATE_REQ(NVKVM_REQ_LIST_NVIDIA_DEVICES,
			    nvkvm_req_list_nvidia_devices,
			    nvkvm_resp_list_nvidia_devices,
			    nvkvm_req_list_nvidia_devices)
		ISOLATE_REQ(NVKVM_REQ_OPEN_NVIDIA_HANDLE,
			    nvkvm_req_open_nvidia_handle,
			    nvkvm_resp_open_nvidia_handle,
			    nvkvm_req_open_nvidia_handle)
		ISOLATE_REQ(NVKVM_REQ_OPEN_MEMORY_HANDLE,
			    nvkvm_req_open_memory_handle,
			    nvkvm_resp_open_memory_handle,
			    nvkvm_req_open_memory_handle)
		ISOLATE_REQ(NVKVM_REQ_CLOSE_HANDLE,
			    nvkvm_req_close_handle,
			    nvkvm_resp_close_handle,
			    nvkvm_req_close_handle)
		ISOLATE_REQ(NVKVM_REQ_CREATE_ISOLATE,
			    nvkvm_req_create_isolate,
			    nvkvm_resp_create_isolate,
			    nvkvm_req_create_isolate)
		ISOLATE_REQ(NVKVM_REQ_KILL_ISOLATE,
			    nvkvm_req_kill_isolate,
			    nvkvm_resp_kill_isolate,
			    nvkvm_req_kill_isolate)
		ISOLATE_REQ(NVKVM_REQ_INTERRUPT,
			    nvkvm_req_interrupt,
			    nvkvm_resp_interrupt,
			    nvkvm_req_interrupt)
		ISOLATE_REQ(NVKVM_REQ_COPY_HANDLE_TO_ISOLATE,
			    nvkvm_req_copy_handle_to_isolate,
			    nvkvm_resp_copy_handle_to_isolate,
			    nvkvm_req_copy_handle_to_isolate)
		ISOLATE_REQ(NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE,
			    nvkvm_req_close_handle_on_isolate,
			    nvkvm_resp_close_handle_on_isolate,
			    nvkvm_req_close_handle_on_isolate)
		ISOLATE_REQ(NVKVM_REQ_POLL_ON_ISOLATE,
			    nvkvm_req_poll_on_isolate,
			    nvkvm_resp_poll_on_isolate,
			    nvkvm_req_poll_on_isolate)
		ISOLATE_REQ(NVKVM_REQ_UNPOLL_ON_ISOLATE,
			    nvkvm_req_unpoll_on_isolate,
			    nvkvm_resp_unpoll_on_isolate,
			    nvkvm_req_unpoll_on_isolate)

		case NVKVM_REQ_IOCTL_ON_ISOLATE: {
			/*
			 * Offload to the thread pool so a blocking stub
			 * round-trip does not stall the single TX thread and
			 * starve other guests.  The completion callback pushes
			 * the response.  Ownership of `elem` transfers to the
			 * work item — do NOT g_free it here (we `continue`).
			 */
			struct nvkvm_ioctl_work *w =
				g_new0(struct nvkvm_ioctl_work, 1);
			w->nv   = nv;
			w->vq   = vq;
			w->elem = elem;
			w->hdr  = hdr;
			iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr),
				   &w->req, sizeof(w->req));
			/* param/aux buffers live in shared memory; the guest
			 * keeps the slot reserved until it sees the response,
			 * so these pointers stay valid for the whole job.
			 * Audit C-1: bound size<=slot_size via slot_blob; on a
			 * malformed/oversize request clamp the size to 0 so the
			 * worker's send/recv touches nothing (no OOB, no NULL+len
			 * deref). */
			w->param_buf = NULL;
			w->aux_buf   = NULL;
			if (w->req.param_size > 0) {
				w->param_buf = slot_blob(nv, w->req.shm_slot,
							 w->req.param_size);
				if (!w->param_buf)
					w->req.param_size = 0;
			}
			if (w->req.aux_size > 0) {
				w->aux_buf = slot_blob(nv, w->req.shm_aux_slot,
						       w->req.aux_size);
				if (!w->aux_buf)
					w->req.aux_size = 0;
			}
			thread_pool_submit_aio(nvkvm_ioctl_work_fn, w,
					       nvkvm_ioctl_work_done, w);
			continue;  /* elem now owned by the work item */
		}

		case NVKVM_REQ_MMAP_ON_ISOLATE: {
			struct nvkvm_req_mmap_on_isolate  req  = {0};
			struct nvkvm_resp_mmap_on_isolate resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			nvkvm_req_mmap_on_isolate(nv, &req, &resp);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_mmap_on_isolate r; } out;
			out.h = hdr; out.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out));
			virtqueue_push(vq, elem, sizeof(out));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_MUNMAP_ON_ISOLATE: {
			struct nvkvm_req_munmap_on_isolate  req  = {0};
			struct nvkvm_resp_munmap_on_isolate resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			nvkvm_req_munmap_on_isolate(nv, &req, &resp);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_munmap_on_isolate r; } out;
			out.h = hdr; out.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &out, sizeof(out));
			virtqueue_push(vq, elem, sizeof(out));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_WRITE_MEMORY_HANDLE: {
			struct nvkvm_req_write_memory_handle  req  = {0};
			struct nvkvm_resp_write_memory_handle resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = (req.size > 0) ?
				   slot_blob(nv, req.shm_slot, req.size) : NULL; /* C-1 */
			nvkvm_req_write_memory_handle(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_write_memory_handle r; } wout;
			wout.h = hdr; wout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &wout, sizeof(wout));
			virtqueue_push(vq, elem, sizeof(wout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_READ_MEMORY_HANDLE: {
			struct nvkvm_req_read_memory_handle  req  = {0};
			struct nvkvm_resp_read_memory_handle resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = (req.size > 0) ?
				   slot_blob(nv, req.shm_slot, req.size) : NULL; /* C-1 */
			nvkvm_req_read_memory_handle(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_read_memory_handle r; } rout;
			rout.h = hdr; rout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &rout, sizeof(rout));
			virtqueue_push(vq, elem, sizeof(rout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_READ_HOST_FILE: {
			struct nvkvm_req_read_host_file  req  = {0};
			struct nvkvm_resp_read_host_file resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			void *db = slot_blob(nv, req.shm_slot, req.max_len); /* C-1 */
			nvkvm_req_read_host_file(nv, &req, &resp, db);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_read_host_file r; } hout;
			hout.h = hdr; hout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &hout, sizeof(hout));
			virtqueue_push(vq, elem, sizeof(hout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

		case NVKVM_REQ_REALIZE_UVM_MAPPING: {
			struct nvkvm_req_realize_uvm_mapping  req  = {0};
			struct nvkvm_resp_realize_uvm_mapping resp = {0};
			iov_to_buf(elem->out_sg, elem->out_num,
				   sizeof(hdr), &req, sizeof(req));
			/* C-1: bound state slot to the whole slot (handler validates
			 * the snapshot size internally) and intent to intent_size. */
			void *sb = slot_blob(nv, req.state_shm_slot, nv->slot_size);
			void *ib = (req.intent_size > 0) ?
				   slot_blob(nv, req.intent_shm_slot, req.intent_size) : NULL;
			nvkvm_req_realize_uvm_mapping(nv, &req, &resp, sb, ib);
			struct { struct nvkvm_hdr h;
				 struct nvkvm_resp_realize_uvm_mapping r; } rout;
			rout.h = hdr; rout.r = resp;
			iov_from_buf(elem->in_sg, elem->in_num, 0, &rout, sizeof(rout));
			virtqueue_push(vq, elem, sizeof(rout));
			virtio_notify(VIRTIO_DEVICE(nv), vq);
			break;
		}

#undef ISOLATE_REQ

		default:
			error_report("nvkvm: unknown request type %u",
				     le32_to_cpu(hdr.type));
			virtqueue_push(vq, elem, 0);
			break;
		}
		g_free(elem);
	}
}

/* ── Virtio config space ─────────────────────────────────────────────────── */

static void nvkvm_get_config(VirtIODevice *vdev, uint8_t *config)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(vdev);
	/*
	 * #55: resolve the sparse window to the firmware-assigned reservation-BAR
	 * GPA (and install the raw memslot there) the first time the guest reads
	 * config — which happens during the guest's nvkvm probe, after PCI
	 * enumeration has programmed the BAR.  Advertise that base/len as the
	 * window the guest validates returned GPAs against.  Falls back to the
	 * fixed base if there's no BAR (nvkvm_sparse_ensure handles both).
	 */
	uint64_t base = nvkvm_sparse_ensure(nv);
	if (base) {
		nv->config_space.mmap_win_gpa = cpu_to_le64(base);
		nv->config_space.mmap_win_len = cpu_to_le64((uint64_t)nv->sparse_size);
	}
	memcpy(config, &nv->config_space, sizeof(nv->config_space));
}

static uint64_t nvkvm_get_features(VirtIODevice *vdev, uint64_t features,
				   Error **errp)
{
	/*
	 * Disable VIRTIO_RING_F_EVENT_IDX.  With async out-of-order completions
	 * (IOCTL_ON_ISOLATE offloaded to the thread pool), EVENT_IDX interrupt
	 * suppression can strand the last used-ring entry: the guest's TX
	 * callback (nvkvm_tx_done_callback) drains with virtqueue_get_buf but
	 * does not use the disable_cb/enable_cb re-check pattern, so a buffer
	 * pushed in the suppression window never raises an IRQ and the guest
	 * hangs forever in wait_for_completion.  Without EVENT_IDX the device
	 * notifies on every push (unless the guest explicitly set NO_INTERRUPT,
	 * which it does not), so no completion can be lost.
	 */
	features &= ~(1ULL << VIRTIO_RING_F_EVENT_IDX);
	return features;
}

/* ── Device realize / unrealize ──────────────────────────────────────────── */

/*
 * The supervisor thread and install_mapping RPC handlers need a way to
 * reach the device. We only ever realize one virtio-nvgpu device per QEMU
 * process; record the pointer at realize time.
 */
static VirtIONvgpu *g_nvkvm_device;

VirtIONvgpu *nvkvm_get_global_device(void)
{
	return g_nvkvm_device;
}

/* Verbose per-operation tracing gate (nvkvm_log.h). Off unless NVKVM_DEBUG
 * is set in the environment; errors and security DENY logs are unconditional. */
int nvkvm_debug_enabled;

static void virtio_nvgpu_device_realize(DeviceState *dev, Error **errp)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIONvgpu  *nv   = VIRTIO_NVGPU(dev);

	nvkvm_debug_enabled = (getenv("NVKVM_DEBUG") != NULL);

	g_nvkvm_device = nv;

	/*
	 * Find QEMU's KVM VM fd by scanning our own /proc/self/fd for the
	 * "anon_inode:kvm-vm" entry, so the nvidia/UVM mmap path in
	 * nvkvm_isolate_handlers.c can call KVM_SET_USER_MEMORY_REGION
	 * on it.  (We can't include sysemu/kvm_int.h here — kvm_int.h is
	 * target-only.)
	 */
	{
		DIR *d = opendir("/proc/self/fd");
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
				char path[300], link[64];
				snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
				ssize_t n = readlink(path, link, sizeof(link) - 1);
				if (n <= 0) continue;
				link[n] = 0;
				if (strcmp(link, "anon_inode:kvm-vm") == 0) {
					int fd = atoi(de->d_name);
					nvkvm_set_kvm_vm_fd(fd);
					NVKVM_DBG(
						"nvkvm: registered KVM vm fd %d\n", fd);
					break;
				}
			}
			closedir(d);
		}
	}
	int fd;

	virtio_init(vdev, VIRTIO_ID_NVGPU, sizeof(struct nvkvm_virtio_config));

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
		/* '2' == NV_RM_API_VERSION_CMD_QUERY (driver returns its
		 * version without enforcing a string compare). cmd=0 is
		 * STRICT — open-source nvidia.ko enforces it. */
		struct nv_ioctl_rm_api_version ver = {.cmd = '2'};
		ioctl(fd, /* NV_ESC_CHECK_VERSION_STR */ _IOWR('F',
		      NV_ESC_CHECK_VERSION_STR, struct nv_ioctl_rm_api_version),
		      &ver);
		memcpy(nv->driver_version, ver.version_string,
		       sizeof(nv->driver_version));
	}
	close(fd);

	/* #81: select the per-version ABI profile from the host driver version.
	 * The guest independently selects the same profile from the version
	 * string we forward; QEMU also stamps the profile id into each
	 * ISOLATE_CMD_IOCTL so the stub uses matching offsets. */
	nv->abi = nvkvm_abi_for_version(nv->driver_version);
	fprintf(stderr, "nvkvm: host driver %s → ABI profile %u\n",
		nv->driver_version, nv->abi ? nv->abi->id : 0);

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
	pthread_mutex_init(&nv->client_allow_lock, NULL);
	nv->client_allow_n = 0;
	TAILQ_INIT(&nv->sessions);

	/* Initialize isolate/handle managers */
	nvkvm_handle_table_init(&nv->handles);
	nvkvm_isolate_table_init(&nv->isolates);
	/* #81: stamp every forwarded IOCTL with the host driver's ABI id so the
	 * stub uses matching version-variant offsets. */
	nv->isolates.abi_profile = nv->abi ? nv->abi->id : NVKVM_ABI_570;

	/* #66 admin subdevice (lazy; for GET_PID_INFO per-process VRAM) */
	pthread_mutex_init(&nv->admin_lock, NULL);
	nv->admin_ctl_fd = -1;
	nv->admin_gpu_fd = -1;
	nv->admin_state  = 0;

	/* Register shared memory as a KVM memory region at NVKVM_SHM_GPA_BASE.
	 * The guest reads shm_base/shm_len from the virtio config space and maps
	 * this region to access ioctl parameter slots with zero virtio copies. */
	memory_region_init_ram_ptr(&nv->shm_mr, OBJECT(dev), "virtio-nvgpu-shm",
				   nv->shm_size, nv->shm_base);
	memory_region_add_subregion(get_system_memory(),
				    NVKVM_SHM_GPA_BASE, &nv->shm_mr);
	nv->shm_mr_registered = true;
	nv->shm_gpa = NVKVM_SHM_GPA_BASE;

	/* Mmap window: 16 GB GPA range for GPU memory mappings */
	nv->mmap_win_gpa  = NVKVM_MMAP_WIN_GPA_BASE;
	nv->mmap_win_size = NVKVM_MMAP_WIN_SIZE;
	nv->mmap_win_cur  = 0;

	/* Sparse GPA window — used by the memory-ioctl path for guest-VA
	 * regions that don't yet have backing.  Lazy via MAP_NORESERVE +
	 * a single big KVM region. */
	nv->sparse_kvm_slot = -1;
	/*
	 * #55 interim safety: the GPA windows (shm @1TB, mmap @1.5TB, sparse
	 * @2TB) squat on fixed GPAs above guest RAM.  That holds for any normal
	 * config, but a guest configured with >=1 TB RAM would overlap the shm
	 * window and silently corrupt — fail loudly instead.  The real fix is to
	 * expose the window as a 64-bit PCI BAR so guest firmware assigns/reserves
	 * the range (docs/design/gpa_window_pci_bar.md).
	 */
	if (current_machine && current_machine->ram_size >= NVKVM_SHM_GPA_BASE) {
		error_setg(errp,
			"nvkvm: guest RAM (0x%" PRIx64 ") overlaps the fixed GPA "
			"windows at 0x%llx; reduce RAM or migrate to the PCI-BAR "
			"window (#55)", (uint64_t)current_machine->ram_size,
			(unsigned long long)NVKVM_SHM_GPA_BASE);
		return;
	}
	if (nvkvm_sparse_init(nv) < 0)
		fprintf(stderr, "nvkvm: sparse window unavailable; "
			"memory-ioctl path will degrade\n");

	/* Populate virtio config space so the guest can locate both regions */
	nv->config_space.shm_base     = cpu_to_le64(NVKVM_SHM_GPA_BASE);
	nv->config_space.shm_len      = cpu_to_le64(nv->shm_size);
	/* The guest validates every host-returned GPA against this window.
	 * Two GPA windows are in play:
	 *   - sparse window (128 GiB @ 2 TB): single pre-installed memslot,
	 *     backs all the bulk BAR/sysmem mmaps as MAP_FIXED slices.
	 *   - mmap_win (16 GiB @ 1.5 TB): legacy per-mmap memslots, still used
	 *     by /dev/nvidia-uvm mappings (which cannot be MAP_FIXED into the
	 *     sparse window — the UVM kernel requires vm_start==pgoff<<SHIFT).
	 * Advertise one window spanning both [mmap_win_base, sparse_end).  The
	 * guest only ever validates GPAs QEMU actually returns (always inside
	 * one of the two sub-windows), so accepting the superset — including the
	 * unbacked gap between them — is safe. */
	nv->config_space.mmap_win_gpa = cpu_to_le64(NVKVM_MMAP_WIN_GPA_BASE);
	nv->config_space.mmap_win_len = cpu_to_le64(
		(NVKVM_SPARSE_GPA_BASE + NVKVM_SPARSE_GPA_SIZE) -
		NVKVM_MMAP_WIN_GPA_BASE);
}

static void virtio_nvgpu_device_unrealize(DeviceState *dev)
{
	VirtIONvgpu *nv = VIRTIO_NVGPU(dev);

	/* Tear down isolates and handles before shared memory */
	nvkvm_isolate_table_fini(&nv->isolates);
	nvkvm_handle_table_fini(&nv->handles);

	/* #66 admin subdevice: closing the fds frees its RM objects. */
	if (nv->admin_ctl_fd >= 0) close(nv->admin_ctl_fd);
	if (nv->admin_gpu_fd >= 0) close(nv->admin_gpu_fd);
	nv->admin_ctl_fd = nv->admin_gpu_fd = -1;
	nv->admin_state = -1;

	if (nv->shm_mr_registered) {
		memory_region_del_subregion(get_system_memory(), &nv->shm_mr);
		nv->shm_mr_registered = false;
	}

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
	VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

	vdc->realize     = virtio_nvgpu_device_realize;
	vdc->unrealize   = virtio_nvgpu_device_unrealize;
	vdc->get_features = nvkvm_get_features;
	vdc->get_config   = nvkvm_get_config;
}

static void virtio_nvgpu_register_types(void)
{
	TypeInfo info = virtio_nvgpu_info;
	info.class_init = virtio_nvgpu_class_init;
	type_register_static(&info);
}

type_init(virtio_nvgpu_register_types);
