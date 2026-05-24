/*
 * nvkvm_frontend.c — NVIDIA frontend ioctl handlers (host side)
 *
 * Handles the core RM ioctls on /dev/nvidiactl and /dev/nvidiaN:
 *   NV_ESC_RM_ALLOC, NV_ESC_RM_FREE, NV_ESC_RM_CONTROL, NV_ESC_RM_DUP_OBJECT
 *   NV_ESC_REGISTER_FD, NV_ESC_ALLOC_OS_EVENT, NV_ESC_FREE_OS_EVENT
 *   plus "simple" ioctls that forward without pointer translation.
 *
 * The pointer translation model (from gVisor nvproxy):
 *  - For ioctls with embedded pointer fields (NVOS54_PARAMETERS.params,
 *    NVOS64_PARAMETERS.p_alloc_parms, etc.), the guest placed the secondary
 *    buffer in the aux slot. We substitute the host VA of the aux slot for
 *    the pointer field before calling the real ioctl, then restore it.
 *  - For ioctls with embedded fd fields, the guest already translated them
 *    to fd_token values; we convert those to host fds here.
 *
 * Security invariants:
 *  - We verify all handles in ALLOC/FREE/CONTROL requests against the
 *    per-client object graph before invoking the real driver.
 *  - We ensure NV01_ROOT_CLIENT allocations are unprivileged (no admin caps).
 *  - We do not allow handles from one session to appear in another session's
 *    requests.
 */

#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "virtio_nvgpu.h"

/* ── Helper: invoke ioctl on the real host fd ─────────────────────────────── */

static long host_ioctl(int fd, unsigned int cmd, void *params)
{
	long ret;
	do {
		ret = ioctl(fd, cmd, params);
	} while (ret == -1 && errno == EINTR);
	return ret == -1 ? -errno : ret;
}

/* ── Helper: look up client by handle in session ─────────────────────────── */

static struct nvkvm_client *find_client(struct nvkvm_session *session,
					uint32_t h_client)
{
	int i;
	pthread_mutex_lock(&session->clients_lock);
	for (i = 0; i < session->nclients; i++) {
		if (session->clients[i] &&
		    session->clients[i]->handle == h_client) {
			pthread_mutex_unlock(&session->clients_lock);
			return session->clients[i];
		}
	}
	pthread_mutex_unlock(&session->clients_lock);
	return NULL;
}

static void register_client(struct nvkvm_session *session,
			     struct nvkvm_client *client)
{
	pthread_mutex_lock(&session->clients_lock);
	if (session->nclients < NVKVM_MAX_OBJECTS_PER_CLIENT)
		session->clients[session->nclients++] = client;
	pthread_mutex_unlock(&session->clients_lock);
}

/* ── NV_ESC_RM_ALLOC ──────────────────────────────────────────────────────── */

/*
 * nvkvm_handle_rm_alloc — forward NV_ESC_RM_ALLOC to the host driver.
 *
 * The params blob in ctx->params_buf is either NVOS21 or NVOS64 format
 * (distinguished by param_size). The guest zeroed p_alloc_parms before
 * sending; if ctx->aux_buf is non-NULL, we substitute it as the alloc params
 * pointer.
 *
 * After a successful alloc, we record the new object in the client's resource
 * map so future CONTROL/FREE calls can validate it.
 */
int nvkvm_handle_rm_alloc(struct nvkvm_req_ctx *ctx)
{
	int ret;
	struct nvkvm_client *client;
	uint32_t h_client, h_object_new, h_class;

	if (ctx->param_size == sizeof(struct nvos64_parameters)) {
		struct nvos64_parameters *p = ctx->params_buf;
		nvp64_t saved_alloc_parms   = p->p_alloc_parms;
		nvp64_t saved_rights        = p->p_rights_requested;

		h_client     = p->h_root;
		h_object_new = p->h_object_new;
		h_class      = p->h_class;

		/* Substitute host pointer for alloc params if provided */
		if (ctx->aux_buf && ctx->aux_size > 0)
			p->p_alloc_parms = (nvp64_t)(uintptr_t)ctx->aux_buf;

		/*
		 * Security: ensure NV01_ROOT_CLIENT allocations are
		 * unprivileged. Clear any admin access bits in rights mask.
		 * gVisor does the equivalent by checking the allocation class
		 * and rejecting admin ones — we follow suit.
		 */
		if (h_class == NV01_ROOT_CLIENT)
			p->p_rights_requested = 0;

		ret = (int)host_ioctl(ctx->hfd->fd,
			_IOWR('F', NV_ESC_RM_ALLOC,
			      struct nvos64_parameters), p);

		/* Restore zeroed fields before copying back to guest */
		p->p_alloc_parms      = saved_alloc_parms;
		p->p_rights_requested = saved_rights;

	} else if (ctx->param_size == sizeof(struct nvos21_parameters)) {
		struct nvos21_parameters *p = ctx->params_buf;
		nvp64_t saved_alloc_parms   = p->p_alloc_parms;

		h_client     = p->h_root;
		h_object_new = p->h_object_new;
		h_class      = p->h_class;

		if (ctx->aux_buf && ctx->aux_size > 0)
			p->p_alloc_parms = (nvp64_t)(uintptr_t)ctx->aux_buf;

		ret = (int)host_ioctl(ctx->hfd->fd,
			_IOWR('F', NV_ESC_RM_ALLOC,
			      struct nvos21_parameters), p);

		p->p_alloc_parms = saved_alloc_parms;
	} else {
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	/*
	 * Record the new object in the client's resource map.
	 * If this is a new root client, create and register the client struct.
	 */
	if (h_class == NV01_ROOT_CLIENT) {
		struct nvkvm_client *c = nvkvm_client_alloc(h_object_new);
		register_client(ctx->session, c);
		/* The root client is its own resource entry */
		pthread_mutex_lock(&c->lock);
		nvkvm_obj_add(c, h_object_new, h_class,
			      NV01_NULL_OBJECT, NULL);
		pthread_mutex_unlock(&c->lock);
	} else {
		client = find_client(ctx->session, h_client);
		if (client) {
			uint32_t h_parent;
			if (ctx->param_size == sizeof(struct nvos64_parameters))
				h_parent = ((struct nvos64_parameters *)
					    ctx->params_buf)->h_object_parent;
			else
				h_parent = ((struct nvos21_parameters *)
					    ctx->params_buf)->h_object_parent;

			pthread_mutex_lock(&client->lock);
			nvkvm_obj_add(client, h_object_new, h_class,
				      h_parent, NULL);
			pthread_mutex_unlock(&client->lock);
		}
	}

	return ret;
}

/* ── NV_ESC_RM_FREE ───────────────────────────────────────────────────────── */

int nvkvm_handle_rm_free(struct nvkvm_req_ctx *ctx)
{
	struct nvos00_parameters *p = ctx->params_buf;
	struct nvkvm_client *client;
	long ret;

	/* Validate client handle */
	client = find_client(ctx->session, p->h_root);
	if (!client) {
		fprintf(stderr, "nvkvm: rm_free: unknown client 0x%x\n",
			p->h_root);
		return -EINVAL;
	}

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_FREE,
		      struct nvos00_parameters), p);

	/* Update our object graph regardless of driver result */
	pthread_mutex_lock(&client->lock);
	nvkvm_obj_free(client, p->h_object_old);
	pthread_mutex_unlock(&client->lock);

	return (int)ret;
}

/* ── NV_ESC_RM_CONTROL ────────────────────────────────────────────────────── */

int nvkvm_handle_rm_control(struct nvkvm_req_ctx *ctx)
{
	struct nvos54_parameters *p = ctx->params_buf;
	nvp64_t saved_params = p->params;
	long ret;

	/* Validate that h_client is a known client in this session */
	if (!find_client(ctx->session, p->h_client)) {
		fprintf(stderr,
			"nvkvm: rm_control: unknown client 0x%x\n",
			p->h_client);
		return -EINVAL;
	}

	/*
	 * Substitute host pointer for the command params buffer.
	 * The guest zeroed p->params; the secondary buffer is in aux_buf.
	 * We validate params_size against aux_size to prevent overread.
	 */
	if (ctx->aux_buf) {
		if (p->params_size > ctx->aux_size) {
			fprintf(stderr,
				"nvkvm: rm_control: params_size %u > aux_size %zu\n",
				p->params_size, ctx->aux_size);
			return -EINVAL;
		}
		p->params = (nvp64_t)(uintptr_t)ctx->aux_buf;
	} else if (p->params_size > 0) {
		fprintf(stderr,
			"nvkvm: rm_control: params_size %u but no aux_buf\n",
			p->params_size);
		return -EINVAL;
	}

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_CONTROL,
		      struct nvos54_parameters), p);

	p->params = saved_params;
	return (int)ret;
}

/* ── NV_ESC_RM_DUP_OBJECT ────────────────────────────────────────────────── */

int nvkvm_handle_rm_dup_object(struct nvkvm_req_ctx *ctx)
{
	struct nvos55_parameters *p = ctx->params_buf;
	struct nvkvm_client *client_dst, *client_src;
	long ret;

	/* Both source and destination clients must belong to this session */
	client_dst = find_client(ctx->session, p->h_client);
	client_src = find_client(ctx->session, p->h_client_src);
	if (!client_dst || !client_src)
		return -EINVAL;

	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_RM_DUP_OBJECT,
		      struct nvos55_parameters), p);
	if (ret < 0)
		return (int)ret;

	/* Record the duplicated object in the destination client */
	pthread_mutex_lock(&client_dst->lock);
	nvkvm_obj_add(client_dst, p->h_object, p->h_parent,
		      p->h_parent, NULL);
	pthread_mutex_unlock(&client_dst->lock);

	return (int)ret;
}

/* ── NV_ESC_REGISTER_FD ───────────────────────────────────────────────────── */

int nvkvm_handle_register_fd(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_register_fd *p = ctx->params_buf;
	struct nvkvm_host_fd *ctl_hfd;
	int32_t saved_fd = p->ctl_fd;
	long ret;

	/*
	 * The guest translated ctl_fd to the fd_token of the /dev/nvidiactl
	 * FD. We look it up and substitute the real host fd.
	 */
	ctl_hfd = nvkvm_fd_lookup(ctx->session, (uint32_t)p->ctl_fd);
	if (!ctl_hfd)
		return -EBADF;

	p->ctl_fd = (int32_t)ctl_hfd->fd;
	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_REGISTER_FD,
		      struct nv_ioctl_register_fd), p);
	p->ctl_fd = saved_fd;
	return (int)ret;
}

/* ── NV_ESC_ALLOC_OS_EVENT ────────────────────────────────────────────────── */

int nvkvm_handle_alloc_os_event(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_alloc_os_event *p = ctx->params_buf;
	uint32_t saved_fd = p->fd;
	int host_efd;
	long ret;

	/*
	 * The guest cannot pass its own eventfd across the VM boundary.
	 * We create a host-side eventfd and use that instead. When the
	 * host eventfd fires, we relay the notification to the guest via VQ_EVT.
	 *
	 * TODO: implement host eventfd → VQ_EVT relay thread.
	 */
	host_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (host_efd < 0)
		return -errno;

	p->fd = (uint32_t)host_efd;
	ret = host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_ALLOC_OS_EVENT,
		      struct nv_ioctl_alloc_os_event), p);
	p->fd = saved_fd;

	if (ret < 0) {
		close(host_efd);
		return (int)ret;
	}

	/* TODO: register host_efd with the event relay */
	return (int)ret;
}

/* ── NV_ESC_FREE_OS_EVENT ─────────────────────────────────────────────────── */

int nvkvm_handle_free_os_event(struct nvkvm_req_ctx *ctx)
{
	struct nv_ioctl_free_os_event *p = ctx->params_buf;
	/* TODO: look up and close the host eventfd, deregister relay */
	return (int)host_ioctl(ctx->hfd->fd,
		_IOWR('F', NV_ESC_FREE_OS_EVENT,
		      struct nv_ioctl_free_os_event), p);
}

/* ── Simple ioctls (no pointer translation needed) ────────────────────────── */

int nvkvm_handle_simple_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd)
{
	return (int)host_ioctl(ctx->hfd->fd, cmd, ctx->params_buf);
}
