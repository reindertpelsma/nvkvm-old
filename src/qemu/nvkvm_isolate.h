/*
 * nvkvm_isolate.h — QEMU-side isolate process manager
 *
 * One isolate per guest userspace mm. The isolate is a minimal static
 * process (nvkvm_stub) that mirrors the guest's GPU virtual address layout
 * so the NVIDIA kernel driver sees valid mappings in current->mm.
 *
 * Multi-inflight design
 * =====================
 * Each isolate has a dedicated reader thread that demultiplexes IOCTL
 * responses by txn_id onto per-caller condvars (stack-allocated by callers).
 * Non-IOCTL (sync) commands serialize via sync_lock + sync_cond.
 * All socket writes are serialized by write_lock.
 *
 * Lock order: sync_lock > write_lock > lock
 * (Never hold a later lock while trying to acquire an earlier one.)
 */

#ifndef NVKVM_ISOLATE_H
#define NVKVM_ISOLATE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>

#include "nvkvm_handle.h"

#define NVKVM_ISOLATE_MAX  4096

/*
 * Forward declaration — fully defined in nvkvm_isolate.c.
 * Callers of nvkvm_isolate_ioctl never touch this directly; it lives on
 * the caller's stack and is registered/deregistered internally.
 */
struct nvkvm_pending_ioctl;

struct nvkvm_isolate {
	uint32_t    id;
	uint32_t    session_id;
	pid_t       pid;
	int         sock_fd;
	bool        alive;
	bool        in_use;

	/*
	 * lock: protects alive, in_use, pending_head, next_txn_id.
	 * Held briefly; never during blocking I/O.
	 */
	pthread_mutex_t lock;

	/* Serializes all multi-part socket writes. */
	pthread_mutex_t write_lock;

	/* Reader thread — one per isolate, started at create time. */
	pthread_t   reader_tid;
	bool        reader_started;

	/* In-flight async IOCTL list (intrusive linked list on callers' stacks). */
	struct nvkvm_pending_ioctl *pending_head;
	uint32_t    next_txn_id;    /* monotonic counter, never 0 */

	/*
	 * Sync command slot — one non-IOCTL command at a time.
	 * sync_lock serializes senders; reader signals sync_cond.
	 */
	pthread_mutex_t sync_lock;
	pthread_cond_t  sync_cond;
	bool        sync_done;
	int         sync_error;     /* -errno or 0 */
	int         sync_mmap_retval;
	int         sync_open_fd;   /* fd received via SCM_RIGHTS for OPEN_DEVICE;
	                             * -1 if none. Reader fills before signaling. */
	/* REALIZE_UVM_FD response slots — reader fills before signaling. */
	uint64_t    sync_realize_host_va;
	uint64_t    sync_realize_length;
	uint64_t    sync_realize_token;
	uint32_t    sync_realize_rm_status;
};

struct nvkvm_isolate_table {
	pthread_mutex_t      lock;
	struct nvkvm_isolate isolates[NVKVM_ISOLATE_MAX];
	uint32_t             next_id;
};

void nvkvm_isolate_table_init(struct nvkvm_isolate_table *t);
void nvkvm_isolate_table_fini(struct nvkvm_isolate_table *t);

/*
 * Spawn a new isolate process. The stub binary is loaded from
 * nvkvm_stub_elf[] (embedded at build time) via memfd_create + fexecve.
 * Returns 0 and fills *isolate_id_out on success.
 */
int nvkvm_isolate_create(struct nvkvm_isolate_table *t,
			 uint32_t session_id,
			 uint32_t *isolate_id_out);

/*
 * Send EXIT command, join the reader thread, wait for the isolate to exit,
 * and free the slot. All in-flight IOCTL callers receive -ECONNRESET.
 */
int nvkvm_isolate_kill(struct nvkvm_isolate_table *t, uint32_t isolate_id);

/*
 * Send a handle's fd to the isolate via SCM_RIGHTS.
 * Also bumps the handle's isolate_refcount.
 */
int nvkvm_isolate_send_handle(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *ht,
			      uint32_t isolate_id, uint32_t handle_id);

/*
 * Ask the isolate to open /dev/nvidia* (or eventfd) on QEMU's behalf so the
 * file's nvfp/mm lineage is the stub process. On success the stub stores
 * the fd under handle_id and replies with a SCM_RIGHTS-attached copy of
 * the same fd; that copy is returned via *fd_out — caller owns it and
 * typically attaches it as the qemu_fd in the handle table.
 *
 * dev_id values match the NVKVM_DEV_* constants from nvkvm_proto.h. UVM
 * and memfd are NOT supported here; both stay opened in QEMU directly.
 */
int nvkvm_isolate_open_device(struct nvkvm_isolate_table *t,
			      uint32_t isolate_id, uint32_t handle_id,
			      uint32_t dev_id, uint32_t flags,
			      int *fd_out);

/*
 * Tell the isolate to close a handle's fd.
 * Also decrements the handle's isolate_refcount.
 */
int nvkvm_isolate_close_handle(struct nvkvm_isolate_table *t,
				struct nvkvm_handle_table *ht,
				uint32_t isolate_id, uint32_t handle_id);

/*
 * Forward an ioctl to the isolate asynchronously.
 * Multiple concurrent callers are supported; responses are matched by txn_id.
 * param_buf and aux_buf are updated in-place with the isolate's response data.
 * fault_addr_out receives the GVA that triggered SIGSEGV (0 if none).
 */
int nvkvm_isolate_ioctl(struct nvkvm_isolate_table *t,
			uint32_t isolate_id, uint32_t handle_id,
			unsigned int cmd,
			void *param_buf, size_t param_size,
			void *aux_buf, size_t aux_size,
			uint32_t flags,
			uint32_t *nvstatus_out,
			uint64_t *fault_addr_out);

/*
 * Tell the isolate to mmap handle_id's fd at gva (MAP_FIXED).
 */
int nvkvm_isolate_mmap(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint64_t gva, uint64_t length, uint64_t offset,
		       int prot, int map_flags);

/*
 * Tell the isolate to munmap [gva, gva+length).
 */
int nvkvm_isolate_munmap(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint64_t gva, uint64_t length);

/*
 * Send REALIZE_UVM_FD: stub opens /dev/nvidia-uvm, replays the recorded
 * state, runs the mode-specific intent ioctl, and mmaps the result.
 *
 * The cmd header is sent first; then `state` (state_size bytes); then
 * `intent` (intent_size bytes), all on the same SEQPACKET socket.
 *
 * On success returns 0 and fills *host_va_out / *length_out / *token_out /
 * *rm_status_out.  rm_status != 0 means the kernel rejected the intent
 * but the transport succeeded.
 */
int nvkvm_isolate_realize_uvm_fd(struct nvkvm_isolate_table *t,
				 uint32_t isolate_id,
				 uint32_t mode,
				 const void *state, uint32_t state_size,
				 const void *intent, uint32_t intent_size,
				 uint32_t prot, uint32_t map_flags,
				 uint64_t length, uint64_t host_va_hint,
				 uint64_t offset,
				 uint64_t *host_va_out, uint64_t *length_out,
				 uint64_t *token_out, uint32_t *rm_status_out);

/*
 * Register/deregister poll on a handle in the isolate.
 */
int nvkvm_isolate_poll(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint32_t events);
int nvkvm_isolate_unpoll(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint32_t handle_id);

/* Kill all isolates belonging to a session. */
void nvkvm_isolate_kill_session(struct nvkvm_isolate_table *t,
				uint32_t session_id);

#endif /* NVKVM_ISOLATE_H */
