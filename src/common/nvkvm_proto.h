/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_proto.h — virtio-nvgpu wire protocol
 *
 * Shared between the guest kernel module (nvkvm-guest.ko) and the QEMU virtio
 * device backend. Both sides must be compiled with the same version of this
 * header.
 *
 * Transport: a single virtio device with three virtqueues:
 *   VQ 0 (TX): guest → host requests
 *   VQ 1 (RX): host → guest responses
 *   VQ 2 (EVT): host → guest async events (poll/epoll notifications)
 *
 * Shared memory region (BAR 0): used for ioctl parameter blobs and for
 * GPU mmap regions that need zero-copy guest access. The region is divided
 * into fixed-size slots allocated by the guest and confirmed by the host.
 *
 * Security model
 * ==============
 * QEMU validates every field in every request. The guest kernel module
 * validates every field from guest userspace before forwarding. Neither side
 * trusts the other beyond what is proven by the protocol invariants below.
 *
 * Notable invariants:
 *  - param_size is validated against the known size for the ioctl command
 *    before any pointer dereference on the host side.
 *  - shm_offset + param_size must fit within the negotiated shared region.
 *  - fd_token values are checked against the per-session open-FD table on the
 *    host side; unknown tokens are rejected with EBADF.
 *  - All handles in RM alloc/control/free requests are validated against the
 *    per-client object graph before the real ioctl is issued.
 */

#ifndef NVKVM_PROTO_H
#define NVKVM_PROTO_H

#include <linux/types.h>

/* ── Protocol version ────────────────────────────────────────────────────── */

#define NVKVM_PROTO_VERSION     1

/* ── Virtqueue indices ───────────────────────────────────────────────────── */

#define NVKVM_VQ_TX     0  /* guest → host requests        */
#define NVKVM_VQ_RX     1  /* host → guest responses        */
#define NVKVM_VQ_EVT    2  /* host → guest async events     */
#define NVKVM_NUM_VQS   3

/* ── Shared memory layout ────────────────────────────────────────────────── */

/*
 * The shared memory BAR is divided into NVKVM_SHM_NSLOTS fixed-size slots.
 * Slot 0 is reserved for control (feature negotiation, etc.).
 * Slots 1..N-1 are used for ioctl parameter blobs and mmap regions.
 *
 * Slot size is negotiated during feature exchange and must be a power of two
 * >= NVKVM_SHM_SLOT_MIN_SIZE.
 */
#define NVKVM_SHM_SLOT_MIN_SIZE     (4096)          /* 4 KiB minimum        */
#define NVKVM_SHM_SLOT_DEFAULT_SIZE (64 * 1024)     /* 64 KiB default       */
#define NVKVM_SHM_NSLOTS            256             /* 256 slots = 16 MiB   */
#define NVKVM_SHM_CTRL_SLOT         0

/* Shared memory control block (slot 0) */
struct nvkvm_shm_ctrl {
	__le32 proto_version;       /* NVKVM_PROTO_VERSION              */
	__le32 slot_size;           /* negotiated slot size in bytes     */
	__le32 nslots;              /* total number of slots             */
	__le32 reserved;
	__u8   driver_version[64];  /* host NVIDIA driver version string */
};

/* ── Request types ───────────────────────────────────────────────────────── */

#define NVKVM_REQ_OPEN       1  /* open /dev/nvidia* device           */
#define NVKVM_REQ_CLOSE      2  /* close a previously opened device   */
#define NVKVM_REQ_IOCTL      3  /* forward an ioctl                   */
#define NVKVM_REQ_MMAP       4  /* create a GPU memory mapping        */
#define NVKVM_REQ_MUNMAP     5  /* tear down a GPU memory mapping     */

/* ── Device identifiers ──────────────────────────────────────────────────── */

#define NVKVM_DEV_CTL        0          /* /dev/nvidiactl                */
#define NVKVM_DEV_UVM        1          /* /dev/nvidia-uvm               */
#define NVKVM_DEV_GPU(n)     (16 + (n)) /* /dev/nvidia0 → /dev/nvidia15  */

/* ── Open request ────────────────────────────────────────────────────────── */

struct nvkvm_req_open {
	__le32 dev_id;      /* NVKVM_DEV_* identifying which device to open */
	__le32 flags;       /* O_RDWR, O_RDONLY, etc.                       */
};

struct nvkvm_resp_open {
	__le32 fd_token;    /* opaque handle used in subsequent requests    */
	__le32 status;      /* 0 = success, errno otherwise                 */
};

/* ── Close request ───────────────────────────────────────────────────────── */

struct nvkvm_req_close {
	__le32 fd_token;
	__le32 reserved;
};

struct nvkvm_resp_close {
	__le32 status;
	__le32 reserved;
};

/* ── Ioctl request ───────────────────────────────────────────────────────── */

/*
 * The parameter blob for the ioctl is written by the guest into the shared
 * memory slot identified by shm_slot before submitting this request.
 * The host reads the blob, translates any embedded pointers, calls the real
 * ioctl, writes the updated blob back into the same slot, then posts the
 * response.
 *
 * param_size must equal the known fixed size for cmd; the host rejects any
 * request where param_size doesn't match.
 *
 * For ioctls that contain embedded pointers to secondary buffers (e.g.
 * NV_ESC_RM_CONTROL with a param buffer, or NV_ESC_CARD_INFO with an array),
 * the guest places the secondary buffer in shm_aux_slot and sets aux_size.
 * The host's handler is responsible for knowing which ioctls require this.
 */
struct nvkvm_req_ioctl {
	__le32 fd_token;        /* which open device FD                  */
	__le32 cmd;             /* full ioctl command (including type/nr) */
	__le32 param_size;      /* size of parameter blob in shm_slot    */
	__le32 shm_slot;        /* slot index for parameter blob         */
	__le32 aux_size;        /* size of auxiliary buffer (0 if none)  */
	__le32 shm_aux_slot;    /* slot index for auxiliary buffer       */
	__le32 session_id;      /* per-guest-process session identifier  */
	__le32 reserved;
};

struct nvkvm_resp_ioctl {
	__le64 retval;          /* raw ioctl return value (typically 0)  */
	__le32 status;          /* 0 = success, errno otherwise          */
	__le32 reserved;
	/* updated parameter blob is already in shm_slot */
};

/* ── Mmap request ────────────────────────────────────────────────────────── */

/*
 * The guest kernel module calls mmap on a nvidia device fd. Instead of
 * directly mapping hardware pages, it asks the host to:
 *   1. Call mmap on the real host fd.
 *   2. Map the resulting host pages into a contiguous GPA range (via
 *      KVM_SET_USER_MEMORY_REGION or equivalent).
 *   3. Return the GPA base to the guest so it can establish process mappings.
 *
 * The host allocates a GPA range from the device's mmio window (BAR 1) and
 * returns gpa_base. The guest kernel module then maps [gpa_base, gpa_base+len)
 * into the requesting process's VMA.
 *
 * prot and flags mirror the mmap(2) arguments as seen from guest userspace.
 */
struct nvkvm_req_mmap {
	__le32 fd_token;
	__le32 prot;        /* PROT_READ | PROT_WRITE | ...           */
	__le32 flags;       /* MAP_SHARED, etc.                       */
	__le32 reserved;
	__le64 offset;      /* file offset passed to mmap             */
	__le64 length;      /* mapping length in bytes                */
};

struct nvkvm_resp_mmap {
	__le64 gpa_base;    /* guest physical address of mapping      */
	__le64 length;      /* actual mapped length (page-aligned)    */
	__le32 mmap_token;  /* opaque handle for munmap               */
	__le32 status;      /* 0 = success, errno otherwise           */
};

/* ── Munmap request ──────────────────────────────────────────────────────── */

struct nvkvm_req_munmap {
	__le32 mmap_token;
	__le32 reserved;
};

struct nvkvm_resp_munmap {
	__le32 status;
	__le32 reserved;
};

/* ── Async event notification (VQ_EVT) ──────────────────────────────────── */

/*
 * When a guest process is polling on a /dev/nvidia* fd and the host fd
 * becomes readable/writable/exceptional, the host posts an event notification
 * on VQ_EVT so the guest kernel module can wake up waiting processes.
 */
struct nvkvm_evt_poll {
	__le32 fd_token;
	__le32 events;      /* POLLIN | POLLOUT | POLLERR | ...       */
};

/* ── Generic virtio descriptor header ───────────────────────────────────── */

/*
 * Every descriptor placed on VQ_TX starts with this header so the host can
 * dispatch to the right handler before reading the type-specific payload.
 * Every descriptor on VQ_RX and VQ_EVT also starts with a matching header
 * (with the same req_id echoed back) for correlation.
 */
struct nvkvm_hdr {
	__le32 type;        /* NVKVM_REQ_*                            */
	__le32 req_id;      /* guest-assigned, echoed in response     */
};

/* Maximum size of the type-specific payload following nvkvm_hdr on VQ_TX */
#define NVKVM_MAX_REQ_PAYLOAD  sizeof(struct nvkvm_req_ioctl)

#endif /* NVKVM_PROTO_H */
