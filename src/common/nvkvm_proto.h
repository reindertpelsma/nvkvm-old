/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_proto.h — virtio-nvgpu wire protocol (isolate architecture)
 *
 * Shared between the guest kernel module (nvkvm-guest.ko) and the QEMU virtio
 * device backend. Both sides must be compiled with the same version of this
 * header.
 *
 * Transport: a single virtio device with three virtqueues:
 *   VQ 0 (TX): guest → host requests
 *   VQ 1 (RX): host → guest responses
 *   VQ 2 (EVT): host → guest async events (poll notifications)
 *
 * Security model
 * ==============
 * QEMU validates every field in every request. Unknown handle IDs or isolate IDs
 * cause QEMU to immediately panic the VM — the guest kernel should never send
 * invalid values; if it does, it is compromised.
 *
 * The guest kernel module validates every field from guest userspace before
 * forwarding over virtio. Neither side trusts the other beyond what is proven
 * by the protocol invariants below.
 *
 * Isolate architecture
 * ====================
 * Each guest userspace process (identified by mm_struct) has exactly one
 * "isolate" host process that mirrors its virtual address space. The isolate
 * has GPU device fds dup'd in via SCM_RIGHTS and replays every mmap at the
 * same GVA (MAP_FIXED), so the NVIDIA driver sees valid mappings in current->mm
 * when processing ioctls. QEMU routes all ioctls through the correct isolate.
 *
 * Handle IDs
 * ==========
 * Two types, both 32-bit opaque integers, globally unique across all sessions:
 *   nvidia handle: wraps an open /dev/nvidia* fd kept in QEMU
 *   memory handle: wraps a memfd kept in QEMU
 * Handles may be distributed to isolates via SCM_RIGHTS. QEMU tracks which
 * isolates hold each handle. close_handle requires no isolates to hold the handle.
 */

#ifndef NVKVM_PROTO_H
#define NVKVM_PROTO_H

#include <linux/types.h>

/* ── Virtio device configuration space ──────────────────────────────────── */

struct nvkvm_virtio_config {
	__le64 shm_base;        /* host-physical base of shared memory      */
	__le64 shm_len;         /* size of shared memory region in bytes    */
	__le64 mmap_win_gpa;    /* guest-physical base of mmap window       */
	__le64 mmap_win_len;    /* size of mmap window in bytes             */
};

/* ── Protocol version ────────────────────────────────────────────────────── */

#define NVKVM_PROTO_VERSION     2

/* ── Virtqueue indices ───────────────────────────────────────────────────── */

#define NVKVM_VQ_TX     0  /* guest → host requests        */
#define NVKVM_VQ_RX     1  /* host → guest responses       */
#define NVKVM_VQ_EVT    2  /* host → guest async events    */
#define NVKVM_NUM_VQS   3

/* ── Shared memory layout ────────────────────────────────────────────────── */

#define NVKVM_SHM_SLOT_MIN_SIZE     (4096)
#define NVKVM_SHM_SLOT_DEFAULT_SIZE (64 * 1024)
#define NVKVM_SHM_NSLOTS            256
#define NVKVM_SHM_CTRL_SLOT         0

struct nvkvm_shm_ctrl {
	__le32 proto_version;
	__le32 slot_size;
	__le32 nslots;
	__le32 reserved;
	__u8   driver_version[64];
};

/* ── Device identifiers ──────────────────────────────────────────────────── */

#define NVKVM_DEV_CTL        0          /* /dev/nvidiactl                */
#define NVKVM_DEV_UVM        1          /* /dev/nvidia-uvm               */
#define NVKVM_DEV_GPU(n)     (16 + (n)) /* /dev/nvidia0 → /dev/nvidia15  */

/* ── Request types ───────────────────────────────────────────────────────── */

/* Legacy (compat, will be removed) */
#define NVKVM_REQ_OPEN                   1
#define NVKVM_REQ_CLOSE                  2

/* Isolate/handle architecture */
#define NVKVM_REQ_LIST_NVIDIA_DEVICES    10  /* enumerate host GPU devices     */
#define NVKVM_REQ_OPEN_NVIDIA_HANDLE     11  /* open /dev/nvidia* in QEMU      */
#define NVKVM_REQ_OPEN_MEMORY_HANDLE     12  /* memfd_create in QEMU           */
#define NVKVM_REQ_CLOSE_HANDLE           13  /* close when no isolate holds it */
#define NVKVM_REQ_CREATE_ISOLATE         14  /* spawn isolate process          */
#define NVKVM_REQ_KILL_ISOLATE           15  /* exit isolate process           */
#define NVKVM_REQ_COPY_HANDLE_TO_ISOLATE 16  /* SCM_RIGHTS send                */
#define NVKVM_REQ_CLOSE_HANDLE_ON_ISOLATE 17 /* CLOSE_FD cmd to isolate        */
#define NVKVM_REQ_IOCTL_ON_ISOLATE       18  /* IOCTL cmd via isolate          */
#define NVKVM_REQ_MMAP_ON_ISOLATE        19  /* MMAP cmd via isolate           */
#define NVKVM_REQ_MUNMAP_ON_ISOLATE      20  /* MUNMAP cmd via isolate         */
#define NVKVM_REQ_POLL_ON_ISOLATE        21  /* start polling fd in isolate    */
#define NVKVM_REQ_UNPOLL_ON_ISOLATE      22  /* stop polling fd in isolate     */
#define NVKVM_REQ_WRITE_MEMORY_HANDLE    23  /* shm_slot → memfd (page upload) */
#define NVKVM_REQ_READ_MEMORY_HANDLE     24  /* memfd → shm_slot (writeback)   */

/* ── Generic header ──────────────────────────────────────────────────────── */

struct nvkvm_hdr {
	__le32 type;        /* NVKVM_REQ_*                            */
	__le32 req_id;      /* guest-assigned, echoed in response     */
};

/* ── LIST_NVIDIA_DEVICES ─────────────────────────────────────────────────── */

struct nvkvm_req_list_nvidia_devices {
	/* no payload */
};

#define NVKVM_MAX_DEVICES  32

struct nvkvm_device_info {
	__le32 dev_id;       /* NVKVM_DEV_* */
	__le32 flags;        /* reserved    */
	__le64 reserved;
};

struct nvkvm_resp_list_nvidia_devices {
	__le32 ndevices;
	__le32 status;
	struct nvkvm_device_info devices[NVKVM_MAX_DEVICES];
};

/* ── OPEN_NVIDIA_HANDLE ──────────────────────────────────────────────────── */

struct nvkvm_req_open_nvidia_handle {
	__le32 dev_id;       /* NVKVM_DEV_*                          */
	__le32 flags;        /* O_RDWR etc.                          */
	__le32 session_id;   /* guest session                        */
	__le32 reserved;
};

struct nvkvm_resp_open_nvidia_handle {
	__le32 handle_id;    /* globally unique handle               */
	__le32 status;       /* 0 = success, errno otherwise         */
};

/* Kept for compat with existing open path */
struct nvkvm_req_open {
	__le32 dev_id;
	__le32 flags;
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_open {
	__le32 fd_token;
	__le32 status;
};

/* ── OPEN_MEMORY_HANDLE ──────────────────────────────────────────────────── */

struct nvkvm_req_open_memory_handle {
	__le64 size;         /* initial size (may be 0 for grow-on-demand) */
	__le32 session_id;
	__le32 flags;        /* reserved                               */
};

struct nvkvm_resp_open_memory_handle {
	__le32 handle_id;
	__le32 status;
};

/* ── CLOSE_HANDLE ────────────────────────────────────────────────────────── */

struct nvkvm_req_close_handle {
	__le32 handle_id;
	__le32 reserved;
};

struct nvkvm_resp_close_handle {
	__le32 status;
	__le32 reserved;
};

/* ── CLOSE (legacy) ──────────────────────────────────────────────────────── */

struct nvkvm_req_close {
	__le32 fd_token;
	__le32 reserved;
};

struct nvkvm_resp_close {
	__le32 status;
	__le32 reserved;
};

/* ── CREATE_ISOLATE ──────────────────────────────────────────────────────── */

struct nvkvm_req_create_isolate {
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_create_isolate {
	__le32 isolate_id;   /* opaque isolate identifier            */
	__le32 status;
};

/* ── KILL_ISOLATE ────────────────────────────────────────────────────────── */

struct nvkvm_req_kill_isolate {
	__le32 isolate_id;
	__le32 reserved;
};

struct nvkvm_resp_kill_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── COPY_HANDLE_TO_ISOLATE ──────────────────────────────────────────────── */

struct nvkvm_req_copy_handle_to_isolate {
	__le32 handle_id;
	__le32 isolate_id;
};

struct nvkvm_resp_copy_handle_to_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── CLOSE_HANDLE_ON_ISOLATE ─────────────────────────────────────────────── */

struct nvkvm_req_close_handle_on_isolate {
	__le32 handle_id;
	__le32 isolate_id;
};

struct nvkvm_resp_close_handle_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── IOCTL_ON_ISOLATE ────────────────────────────────────────────────────── */

/*
 * The parameter blob and aux buffer are placed in shared memory slots.
 * vma_whitelist_slot holds an array of nvkvm_vma_entry structs (see below)
 * used for demand-fault resolution. Set vma_whitelist_nentries = 0 to skip.
 *
 * flags:
 *   NVKVM_IOCTL_FL_RETRY_EFAULT — this is a retry after demand-fault mapping
 */
#define NVKVM_IOCTL_FL_RETRY_EFAULT  (1 << 0)

struct nvkvm_req_ioctl_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;            /* which nvidia handle (fd)              */
	__le32 cmd;                  /* full ioctl command                    */
	__le32 param_size;           /* size of param blob in shm_slot        */
	__le32 shm_slot;             /* slot index for param blob             */
	__le32 aux_size;             /* aux buffer size (0 if none)           */
	__le32 shm_aux_slot;         /* slot index for aux buffer             */
	__le32 vma_whitelist_nentries; /* # of VMA whitelist entries          */
	__le32 vma_whitelist_slot;   /* slot for nvkvm_vma_entry array        */
	__le32 flags;                /* NVKVM_IOCTL_FL_*                      */
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_ioctl_on_isolate {
	__le64 retval;               /* raw ioctl return value                */
	__le32 status;               /* 0 = ok, errno on transport error      */
	__le32 nvstatus;             /* NvStatus from params.status field     */
	__le64 fault_addr;           /* GVA that caused SIGSEGV (0 if none)  */
};

/* VMA entry for demand-fault whitelist */
struct nvkvm_vma_entry {
	__le64 start;   /* inclusive */
	__le64 end;     /* exclusive */
	__le32 prot;    /* PROT_READ | PROT_WRITE | PROT_EXEC */
	__le32 reserved;
};

/* Maximum whitelist entries per ioctl */
#define NVKVM_MAX_VMA_ENTRIES  1024

/* ── MMAP_ON_ISOLATE ─────────────────────────────────────────────────────── */

/*
 * Asks QEMU to:
 *   1. mmap the handle fd at any host VA (QVA), register GPA→QVA in KVM slot.
 *   2. Send MMAP command to isolate: map same fd at gva (MAP_FIXED).
 * Returns the GPA allocated for the guest to use in remap_pfn_range.
 */
struct nvkvm_req_mmap_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
	__le64 gva;              /* exact guest VA to map in the isolate   */
	__le64 offset;           /* fd offset                              */
	__le64 length;           /* mapping length                         */
	__le32 prot;             /* PROT_READ | PROT_WRITE | ...           */
	__le32 map_flags;        /* MAP_SHARED etc. (MAP_FIXED added by QEMU) */
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_mmap_on_isolate {
	__le64 gpa_base;         /* GPA for remap_pfn_range                */
	__le64 length;           /* actual mapped length (page-aligned)    */
	__le32 mmap_token;       /* opaque handle for munmap               */
	__le32 status;
};

/* ── MUNMAP_ON_ISOLATE ───────────────────────────────────────────────────── */

struct nvkvm_req_munmap_on_isolate {
	__le32 isolate_id;
	__le32 mmap_token;
};

struct nvkvm_resp_munmap_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── POLL_ON_ISOLATE ─────────────────────────────────────────────────────── */

struct nvkvm_req_poll_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
	__le32 events;           /* POLLIN | POLLOUT | ...                 */
	__le32 reserved;
};

struct nvkvm_resp_poll_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── UNPOLL_ON_ISOLATE ───────────────────────────────────────────────────── */

struct nvkvm_req_unpoll_on_isolate {
	__le32 isolate_id;
	__le32 handle_id;
};

struct nvkvm_resp_unpoll_on_isolate {
	__le32 status;
	__le32 reserved;
};

/* ── Async poll event (VQ_EVT) ───────────────────────────────────────────── */

struct nvkvm_evt_poll {
	__le32 isolate_id;
	__le32 handle_id;
	__le32 events;
	__le32 reserved;
};

/* ── WRITE_MEMORY_HANDLE ─────────────────────────────────────────────────── */
/* Copy shm_slot[0..size) into memfd at offset (CPU page upload). */

struct nvkvm_req_write_memory_handle {
	__le32 handle_id;
	__le32 shm_slot;
	__le64 offset;
	__le32 size;
	__le32 reserved;
};

struct nvkvm_resp_write_memory_handle {
	__le32 status;
	__le32 reserved;
};

/* ── READ_MEMORY_HANDLE ──────────────────────────────────────────────────── */
/* Copy memfd at offset into shm_slot[0..size) (CPU page writeback). */

struct nvkvm_req_read_memory_handle {
	__le32 handle_id;
	__le32 shm_slot;
	__le64 offset;
	__le32 size;
	__le32 reserved;
};

struct nvkvm_resp_read_memory_handle {
	__le32 status;
	__le32 reserved;
};

/* Legacy ioctl request (retained for dispatch.c compat) */
struct nvkvm_req_ioctl {
	__le32 fd_token;
	__le32 cmd;
	__le32 param_size;
	__le32 shm_slot;
	__le32 aux_size;
	__le32 shm_aux_slot;
	__le32 session_id;
	__le32 reserved;
};

struct nvkvm_resp_ioctl {
	__le64 retval;
	__le32 status;
	__le32 reserved;
};

/* Legacy mmap request */
struct nvkvm_req_mmap {
	__le32 fd_token;
	__le32 prot;
	__le32 flags;
	__le32 reserved;
	__le64 offset;
	__le64 length;
};

struct nvkvm_resp_mmap {
	__le64 gpa_base;
	__le64 length;
	__le32 mmap_token;
	__le32 status;
};

struct nvkvm_req_munmap {
	__le32 mmap_token;
	__le32 reserved;
};

struct nvkvm_resp_munmap {
	__le32 status;
	__le32 reserved;
};

#define NVKVM_MAX_REQ_PAYLOAD  sizeof(struct nvkvm_req_ioctl_on_isolate)

#endif /* NVKVM_PROTO_H */
