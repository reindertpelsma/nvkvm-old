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

/* virtio-nvgpu device ID — allocated from the vendor-specific range */
#define VIRTIO_ID_NVGPU   0x105d

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
	int     id;             /* IDR key                             */
	int     refcount;       /* protected by nvkvm_state.sessions_lock */
};

/* ── Per-FD context (one per open(/dev/nvidia*)) ──────────────────────────── */

struct nvkvm_mmap_region {
	struct list_head list;
	__u32    mmap_token;       /* opaque token from host                */
	unsigned long gpa_base;    /* guest physical address                */
	unsigned long length;
	struct vm_area_struct *vma; /* NULL after munmap                    */
};

struct nvkvm_fd_ctx {
	__u32                  fd_token;    /* opaque host-side FD reference       */
	int                    dev_id;      /* NVKVM_DEV_*                         */
	struct nvkvm_session  *session;

	/* poll support */
	wait_queue_head_t      poll_wq;
	atomic_t               poll_events; /* cached POLL* bits from host         */

	/* mmap regions owned by this FD */
	spinlock_t             mmap_lock;
	struct list_head       mmap_regions;
};

/* ── In-flight request tracking ───────────────────────────────────────────── */

struct nvkvm_inflight {
	struct list_head    list;
	__u32               req_id;
	struct completion   done;
	/* response fields filled by virtio RX callback */
	__u64               retval;
	int                 status;   /* 0 = success, errno otherwise          */
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

	/* In-flight request tracking */
	spinlock_t              inflight_lock;
	struct list_head        inflight_list;
	atomic_t                next_req_id;

	/* Slot allocator for shared memory */
	spinlock_t              slot_lock;
	unsigned long           slot_bitmap[NVKVM_SHM_NSLOTS / BITS_PER_LONG];

	/* Host driver version (from shared memory ctrl block) */
	char                    driver_version[64];

	/* GPU mmap window — GPA range reserved for nvkvm_mmap_request() */
	unsigned long           mmap_window_gpa_base;
	unsigned long           mmap_window_len;
};

/* ── Function declarations ─────────────────────────────────────────────────── */

/* nvkvm_virtio.c */
int  nvkvm_virtio_init(struct virtio_device *vdev, struct nvkvm_state *state);
void nvkvm_virtio_fini(struct nvkvm_state *state);
int  nvkvm_negotiate_version(struct nvkvm_state *state);
int  nvkvm_virtio_open(int dev_id, unsigned int flags,
		       struct nvkvm_resp_open *resp_out);
int  nvkvm_virtio_close(__u32 fd_token, struct nvkvm_resp_close *resp_out);
long nvkvm_virtio_ioctl(struct nvkvm_fd_ctx *ctx, unsigned int cmd,
			void *params_buf, size_t param_size);

/* nvkvm_ioctl.c */
size_t nvkvm_ioctl_param_size(unsigned int cmd);
int    nvkvm_sanitize_ioctl_params(struct nvkvm_fd_ctx *ctx,
				   unsigned int cmd,
				   void *params_buf, size_t param_size);

/* nvkvm_mmap.c */
int  nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma);
void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx);

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
