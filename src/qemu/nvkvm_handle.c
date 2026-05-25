/*
 * nvkvm_handle.c — global handle table for nvidia and memory handles
 *
 * All operations are serialized under handle_table.lock.
 * The handle ID 0 is reserved (invalid).
 */

#include "qemu/osdep.h"
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef SYS_memfd_create
/* x86-64 */
#define SYS_memfd_create 319
#endif

static inline int memfd_create_compat(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, flags);
}
#undef memfd_create
#define memfd_create memfd_create_compat

#include "nvkvm_handle.h"
#include "../../src/common/nvkvm_proto.h"

/* Device path table indexed by NVKVM_DEV_* */
static const char *nvidia_dev_path(int dev_id)
{
	static char gpu_path[32];
	if (dev_id == NVKVM_DEV_CTL)
		return "/dev/nvidiactl";
	if (dev_id == NVKVM_DEV_UVM)
		return "/dev/nvidia-uvm";
	int n = dev_id - 16;
	if (n >= 0 && n < 16) {
		snprintf(gpu_path, sizeof(gpu_path), "/dev/nvidia%d", n);
		return gpu_path;
	}
	return NULL;
}

void nvkvm_handle_table_init(struct nvkvm_handle_table *t)
{
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->lock, NULL);
	t->next_id = 1;
	for (int i = 0; i < NVKVM_HANDLE_MAX; i++)
		t->handles[i].fd = -1;
}

void nvkvm_handle_table_fini(struct nvkvm_handle_table *t)
{
	pthread_mutex_lock(&t->lock);
	for (int i = 0; i < NVKVM_HANDLE_MAX; i++) {
		if (t->handles[i].in_use && t->handles[i].fd >= 0) {
			close(t->handles[i].fd);
			t->handles[i].fd = -1;
		}
	}
	pthread_mutex_unlock(&t->lock);
	pthread_mutex_destroy(&t->lock);
}

/* Allocate next available handle slot. Called with lock held. */
static struct nvkvm_handle *alloc_slot(struct nvkvm_handle_table *t,
				       uint32_t *id_out)
{
	/* linear scan from next_id (IDs are rare, table is small) */
	for (int attempt = 0; attempt < NVKVM_HANDLE_MAX; attempt++) {
		uint32_t id = t->next_id;
		t->next_id++;
		if (t->next_id >= NVKVM_HANDLE_MAX)
			t->next_id = 1;
		if (id == 0)
			continue;
		struct nvkvm_handle *h = &t->handles[id % NVKVM_HANDLE_MAX];
		if (!h->in_use) {
			memset(h, 0, sizeof(*h));
			h->id     = id;
			h->fd     = -1;
			h->in_use = true;
			*id_out   = id;
			return h;
		}
	}
	return NULL;
}

int nvkvm_handle_open_nvidia(struct nvkvm_handle_table *t,
			     uint32_t session_id, int dev_id, int flags,
			     uint32_t *handle_id_out)
{
	const char *path = nvidia_dev_path(dev_id);
	if (!path)
		return -EINVAL;

	int fd = open(path, flags | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_handle *h = alloc_slot(t, &id);
	if (!h) {
		pthread_mutex_unlock(&t->lock);
		close(fd);
		return -EMFILE;
	}
	h->type       = NVKVM_HANDLE_TYPE_NVIDIA;
	h->fd         = fd;
	h->session_id = session_id;
	h->dev_id     = dev_id;
	*handle_id_out = id;
	pthread_mutex_unlock(&t->lock);

	fprintf(stderr, "nvkvm_handle: opened nvidia handle %u dev_id=%d fd=%d\n",
		id, dev_id, fd);
	return 0;
}

int nvkvm_handle_open_memory(struct nvkvm_handle_table *t,
			     uint32_t session_id, uint64_t size,
			     uint32_t *handle_id_out)
{
	int fd = memfd_create("nvkvm_mem", MFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	if (size > 0 && ftruncate(fd, (off_t)size) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_handle *h = alloc_slot(t, &id);
	if (!h) {
		pthread_mutex_unlock(&t->lock);
		close(fd);
		return -EMFILE;
	}
	h->type       = NVKVM_HANDLE_TYPE_MEMORY;
	h->fd         = fd;
	h->session_id = session_id;
	h->dev_id     = 0;
	*handle_id_out = id;
	pthread_mutex_unlock(&t->lock);

	fprintf(stderr, "nvkvm_handle: opened memory handle %u size=%llu fd=%d\n",
		id, (unsigned long long)size, fd);
	return 0;
}

struct nvkvm_handle *nvkvm_handle_get(struct nvkvm_handle_table *t,
				      uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return NULL;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return NULL;
	}
	pthread_mutex_unlock(&t->lock);
	return h;
}

int nvkvm_handle_ref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	h->isolate_refcount++;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

int nvkvm_handle_unref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	if (h->isolate_refcount > 0)
		h->isolate_refcount--;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

int nvkvm_handle_close(struct nvkvm_handle_table *t, uint32_t handle_id)
{
	if (handle_id == 0 || handle_id >= NVKVM_HANDLE_MAX)
		return -EBADF;
	pthread_mutex_lock(&t->lock);
	struct nvkvm_handle *h = &t->handles[handle_id % NVKVM_HANDLE_MAX];
	if (!h->in_use || h->id != handle_id) {
		pthread_mutex_unlock(&t->lock);
		return -EBADF;
	}
	if (h->isolate_refcount > 0) {
		pthread_mutex_unlock(&t->lock);
		return -EBUSY;
	}
	if (h->fd >= 0) {
		close(h->fd);
		h->fd = -1;
	}
	h->in_use = false;
	pthread_mutex_unlock(&t->lock);
	return 0;
}

void nvkvm_handle_close_session(struct nvkvm_handle_table *t, uint32_t session_id)
{
	pthread_mutex_lock(&t->lock);
	for (int i = 1; i < NVKVM_HANDLE_MAX; i++) {
		struct nvkvm_handle *h = &t->handles[i];
		if (!h->in_use || h->session_id != session_id)
			continue;
		if (h->fd >= 0) {
			close(h->fd);
			h->fd = -1;
		}
		h->in_use = false;
	}
	pthread_mutex_unlock(&t->lock);
}
