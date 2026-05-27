/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvkvm.h — internal header for the nvkvm-guest kernel module
 */

#ifndef NVKVM_H
#define NVKVM_H

#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/list.h>

#include "../../src/common/nvkvm_proto.h"
#include "../../src/abi/nvgpu.h"

/* virtio-nvgpu device ID.
 * Must be in the range 0–63 so the PCI device ID (0x1040 + type) falls in
 * the valid modern virtio PCI range 0x1040–0x107f.  50 is unassigned by the
 * virtio spec; PCI device ID = 0x1072.
 * Must match VIRTIO_ID_NVGPU in src/qemu/virtio_nvgpu.h. */
#define VIRTIO_ID_NVGPU   50

/* ── Per-session state (one per guest process that has opened a device) ───── */

/*
 * A session groups all FDs opened by a single guest process (tgid). When the
 * process exits, all host FDs belonging to its session are closed and all RM
 * objects belonging to its clients are freed.
 *
 * This mirrors gVisor's per-container nvproxy state, providing isolation:
 * each guest process sees only its own GPU contexts.
 */
struct nvkvm_session {
	pid_t   tgid;
	int     id;             /* IDR key                                */
	int     refcount;       /* protected by nvkvm_state.sessions_lock */
	__u32   isolate_id;     /* QEMU isolate process ID (0 = not yet created) */
	struct mutex isolate_lock; /* protects isolate_id creation       */
};

/* ── Per-FD context (one per open(/dev/nvidia*)) ──────────────────────────── */

struct nvkvm_mmap_region {
	struct list_head list;
	__u32    mmap_token;       /* opaque token from host                */
	__u32    handle_id;        /* QEMU-side handle (for isolate re-map) */
	unsigned long gpa_base;    /* guest physical address                */
	unsigned long length;
	__u64    offset;           /* fd offset at which we were mapped     */
	struct vm_area_struct *vma; /* NULL after munmap                   */
};

/*
 * CPU page migration tracking — one entry per page pinned and uploaded to a
 * memfd for the isolate's CPU-userptr access.
 */
struct nvkvm_cpu_page {
	struct page    *page;      /* pinned guest physical page                */
	unsigned long   gva;       /* page-aligned GVA mapped in the isolate    */
	__u32           handle_id; /* QEMU memory handle wrapping the memfd     */
	__u32           mmap_token;/* token for MUNMAP_ON_ISOLATE cleanup       */
	__u32           prot;      /* PROT_* flags (read/write)                 */
	struct list_head list;
};

struct nvkvm_fd_ctx {
	__u32                  handle_id;   /* QEMU-side nvidia handle ID */
	int                    dev_id;      /* NVKVM_DEV_*                */
	struct nvkvm_session  *session;

	/* poll support */
	wait_queue_head_t      poll_wq;
	atomic_t               poll_events; /* cached POLL* bits from host         */

	/* mmap regions owned by this FD */
	spinlock_t             mmap_lock;
	struct list_head       mmap_regions;

	/* CPU pages migrated to the isolate for userptr access */
	struct mutex           cpu_pages_lock;
	struct list_head       cpu_pages;
};

/* ── In-flight request tracking ───────────────────────────────────────────── */

struct nvkvm_inflight {
	struct list_head    list;
	__u32               txn_id;
	struct completion   done;
	/* response fields filled by virtio TX completion callback */
	__u64               retval;
	int                 status;   /* 0 = success, errno otherwise          */
	void               *resp_buf; /* IN-sg buffer holding the host response */
	/* extended fields for isolate-path responses */
	__u64               fault_addr; /* GVA of SIGSEGV in isolate (IOCTL_ON_ISOLATE) */
	__u32               nvstatus;   /* NvStatus from NVIDIA params              */
};

/* ── Global module state ──────────────────────────────────────────────────── */

#define NVKVM_MAX_GPUS  (NV_MINOR_DEVICE_NUMBER_REGULAR_MAX + 1)

struct nvkvm_state {
	/* Device registration */
	struct class  *class;
	unsigned int   ctl_major;
	struct cdev    ctl_cdev;
	unsigned int   gpu_major;
	dev_t          gpu_devno_base;
	struct cdev    gpu_cdevs[NVKVM_MAX_GPUS];
	int            num_gpus;
	unsigned int   uvm_major;
	dev_t          uvm_devno;
	struct cdev    uvm_cdev;

	/* Session management */
	struct mutex   sessions_lock;
	struct idr     sessions_idr;

	/* Virtio transport */
	struct virtio_device   *vdev;
	struct virtqueue       *vq_tx;
	struct virtqueue       *vq_rx;
	struct virtqueue       *vq_evt;

	/* Shared memory region (BAR 0) */
	void __iomem           *shm_base;
	size_t                  shm_size;
	size_t                  slot_size;

	/* In-flight request tracking.
	 *
	 * txn_id is u32; the guest module assigns one per request and never
	 * reuses an outstanding one. The bitmap tracks the set of currently
	 * outstanding txn_ids so that even at the (impossibly high) rate
	 * required for the u32 counter to wrap we never collide.
	 *
	 * Bitmap size is bounded to NVKVM_MAX_INFLIGHT; at 4096 in-flight
	 * requests this is 512 bytes. Real workloads stay well under 256.
	 */
#define NVKVM_MAX_INFLIGHT 4096
	spinlock_t              inflight_lock;
	struct list_head        inflight_list;
	atomic_t                next_txn_id;        /* monotonic seed */
	unsigned long           txn_inflight_bm[NVKVM_MAX_INFLIGHT / BITS_PER_LONG];

	/* Slot allocator for shared memory */
	spinlock_t              slot_lock;
	unsigned long           slot_bitmap[NVKVM_SHM_NSLOTS / BITS_PER_LONG];

	/* Host driver version (from shared memory ctrl block) */
	char                    driver_version[64];

	/* GPU mmap window — GPA range reserved for nvkvm_mmap_request() */
	unsigned long           mmap_window_gpa_base;
	unsigned long           mmap_window_len;
};

/* ── Global module state (defined in nvkvm_main.c) ────────────────────────── */

extern struct nvkvm_state nvkvm;

/* ── Function declarations ─────────────────────────────────────────────────── */

/* nvkvm_virtio.c — transport layer */
int  nvkvm_virtio_init(struct virtio_device *vdev, struct nvkvm_state *state);
void nvkvm_virtio_fini(struct nvkvm_state *state);
int  nvkvm_negotiate_version(struct nvkvm_state *state);

/* Isolate-aware API */
int  nvkvm_virtio_open_nvidia_handle(int dev_id, unsigned int flags,
				     unsigned int session_id,
				     __u32 *handle_id_out);
int  nvkvm_virtio_create_isolate(unsigned int session_id,
				 __u32 *isolate_id_out);
int  nvkvm_virtio_copy_handle_to_isolate(__u32 handle_id, __u32 isolate_id);
int  nvkvm_virtio_close_handle_on_isolate(__u32 handle_id, __u32 isolate_id);
int  nvkvm_virtio_close_handle(__u32 handle_id);
int  nvkvm_virtio_kill_isolate(__u32 isolate_id);
long nvkvm_virtio_ioctl_on_isolate(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size,
				   void *aux_buf, size_t aux_size,
				   __u32 flags,
				   __u64 *fault_addr_out);
int  nvkvm_virtio_mmap_on_isolate(__u32 isolate_id, __u32 handle_id,
				  __u64 gva, __u64 offset, __u64 length,
				  __u32 prot, __u32 map_flags,
				  unsigned int session_id,
				  __u64 *gpa_base_out,
				  __u32 *mmap_token_out);
int  nvkvm_virtio_munmap_on_isolate(__u32 isolate_id, __u32 mmap_token);
int  nvkvm_virtio_open_memory_handle(unsigned int session_id, __u64 size,
				     __u32 *handle_id_out);
int  nvkvm_virtio_write_memory_handle(__u32 handle_id, __u64 offset,
				      int shm_slot, __u32 size);
int  nvkvm_virtio_read_memory_handle(__u32 handle_id, __u64 offset,
				     int shm_slot, __u32 size);

/* nvkvm_ioctl.c */
size_t nvkvm_ioctl_param_size(unsigned int cmd);
int    nvkvm_sanitize_ioctl_params(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size);

/* nvkvm_mmap.c */
extern const struct vm_operations_struct nvkvm_vm_ops;
int  nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma);
void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx);
int  nvkvm_efault_resolve(struct nvkvm_fd_ctx *ctx, __u64 fault_addr);
void nvkvm_cpu_pages_writeback(struct nvkvm_fd_ctx *ctx);
void nvkvm_cpu_pages_free(struct nvkvm_fd_ctx *ctx);

/* nvkvm_session.c */
struct nvkvm_session *nvkvm_session_get_or_create(pid_t tgid);
void                  nvkvm_session_put(struct nvkvm_session *session);

/* Shared memory slot helpers (nvkvm_virtio.c) */
int   nvkvm_slot_alloc(struct nvkvm_state *state);
void  nvkvm_slot_free(struct nvkvm_state *state, int slot);
void *nvkvm_slot_addr(struct nvkvm_state *state, int slot);

/* Internal transport send (nvkvm_virtio.c) */
int   nvkvm_send_sync(struct nvkvm_state *state,
		      void *req_buf, size_t req_len,
		      struct nvkvm_inflight *inf);

#endif /* NVKVM_H */
