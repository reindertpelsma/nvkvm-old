/*
 * nvkvm_handle.h — global handle table for nvidia and memory handles
 *
 * Handle IDs are 32-bit integers, globally unique, persistent across
 * isolate lifetime. The underlying fd lives in QEMU and is distributed
 * to isolates via SCM_RIGHTS on demand.
 */

#ifndef NVKVM_HANDLE_H
#define NVKVM_HANDLE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define NVKVM_HANDLE_TYPE_NVIDIA  1   /* open /dev/nvidia* fd     */
#define NVKVM_HANDLE_TYPE_MEMORY  2   /* memfd                    */

#define NVKVM_HANDLE_MAX  65536

struct nvkvm_handle {
	uint32_t    id;
	int         type;         /* NVKVM_HANDLE_TYPE_* */
	int         fd;           /* underlying fd in QEMU (-1 if closed) */
	uint32_t    session_id;   /* owning session */
	int         dev_id;       /* for TYPE_NVIDIA: NVKVM_DEV_*         */
	uint32_t    isolate_refcount;  /* # isolates that hold this handle */
	bool        poll_active;  /* handle is registered for poll        */
	bool        in_use;
};

struct nvkvm_handle_table {
	pthread_mutex_t  lock;
	struct nvkvm_handle handles[NVKVM_HANDLE_MAX];
	uint32_t         next_id;   /* monotonic counter, wraps with gap-fill */
};

void nvkvm_handle_table_init(struct nvkvm_handle_table *t);
void nvkvm_handle_table_fini(struct nvkvm_handle_table *t);

/* Allocate a new nvidia handle (opens fd in QEMU). */
int nvkvm_handle_open_nvidia(struct nvkvm_handle_table *t,
			     uint32_t session_id, int dev_id, int flags,
			     uint32_t *handle_id_out);

/* Allocate a new memory handle (creates memfd in QEMU). */
int nvkvm_handle_open_memory(struct nvkvm_handle_table *t,
			     uint32_t session_id, uint64_t size,
			     uint32_t *handle_id_out);

/* Look up a handle (caller must hold no lock; returns pointer under table lock). */
struct nvkvm_handle *nvkvm_handle_get(struct nvkvm_handle_table *t,
				      uint32_t handle_id);

/* Bump isolate refcount (called when sending fd to an isolate). */
int nvkvm_handle_ref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Decrement isolate refcount (called when isolate closes fd). */
int nvkvm_handle_unref_isolate(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Close the underlying fd. Fails (returns -EBUSY) if isolate_refcount > 0. */
int nvkvm_handle_close(struct nvkvm_handle_table *t, uint32_t handle_id);

/* Close all handles belonging to a session (called on session teardown). */
void nvkvm_handle_close_session(struct nvkvm_handle_table *t, uint32_t session_id);

#endif /* NVKVM_HANDLE_H */
