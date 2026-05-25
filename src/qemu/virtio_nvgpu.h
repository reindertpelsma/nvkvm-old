/*
 * virtio_nvgpu.h — QEMU virtio-nvgpu device backend header
 *
 * The QEMU backend is the host-side counterpart of the guest nvkvm-guest.ko
 * module. It sits at the hypervisor/VM boundary and:
 *
 *  1. Validates every request from the guest before forwarding.
 *  2. Translates guest fd_tokens to real host fds.
 *  3. Translates guest physical addresses (GPAs) to host virtual addresses
 *     (HVAs) for mmap / OS descriptor operations.
 *  4. Calls the real NVIDIA ioctls and returns results.
 *  5. Manages the host-side RM object graph for isolation and lifecycle.
 *
 * Security model
 * ==============
 * The guest kernel module is not trusted. Anything it sends may be malformed,
 * out-of-range, or designed to exploit the host NVIDIA driver or QEMU.
 *
 *  - param_size is validated against the expected size for cmd before any
 *    pointer into shared memory is dereferenced.
 *  - shm_slot must be in [1, NVKVM_SHM_NSLOTS) and owned by this request.
 *  - fd_token must map to an open host fd in the per-session table.
 *  - session_id must match the fd_token's owning session.
 *  - RM handles in alloc/control/free are checked against the per-client
 *    object graph before the real ioctl is issued.
 *  - GPA ranges for mmap are allocated from a dedicated pool managed by QEMU;
 *    the guest cannot influence which HPA a GPA maps to.
 */

#ifndef VIRTIO_NVGPU_H
#define VIRTIO_NVGPU_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/queue.h>

/* Bring in QEMU's virtio helpers */
#include "hw/virtio/virtio.h"
#include "hw/pci/pci.h"
#include "qemu/iov.h"
/* exec/memory.h (for MemoryRegion) is transitively included via virtio.h */

#include "../../src/common/nvkvm_proto.h"
#include "../../src/common/nvkvm_isolate_proto.h"
#include "../../src/abi/nvgpu.h"
#include "../../src/abi/uvm.h"
#include "nvkvm_handle.h"
#include "nvkvm_isolate.h"

/* ── Device constants ────────────────────────────────────────────────────── */

/* Virtio device type for NVIDIA GPU ioctl passthrough.
 * 50 is unassigned by the virtio spec; PCI device ID = 0x1040 + 50 = 0x1072.
 * Must be in range 0–63 so the PCI ID stays within 0x1040–0x107f (the valid
 * range the Linux virtio-pci driver recognizes).
 * Must match VIRTIO_ID_NVGPU in src/guest/nvkvm.h. */
#define VIRTIO_ID_NVGPU             50

/*
 * Guest-physical address layout for shared memory and mmap window.
 * These live above any realistic guest RAM ceiling (1 TB and 1.5 TB),
 * so they never alias guest RAM regardless of VM size.
 */
#define NVKVM_SHM_GPA_BASE          0x10000000000ULL  /* 1 TB  */
#define NVKVM_MMAP_WIN_GPA_BASE     0x18000000000ULL  /* 1.5 TB */
#define NVKVM_MMAP_WIN_SIZE         (16ULL << 30)     /* 16 GB window */

/* ── Object graph (mirrors gVisor nvproxy object.go) ────────────────────── */

struct nvkvm_object;

struct nvkvm_object_impl {
	void (*release)(struct nvkvm_object *obj);
};

struct nvkvm_object {
	uint32_t  handle;
	uint32_t  class_id;
	uint32_t  parent_handle;

	/* dependency graph */
	struct nvkvm_object **deps;   /* objects this depends on      */
	int                  ndeps;
	struct nvkvm_object **rdeps;  /* objects that depend on this  */
	int                 nrdeps;

	const struct nvkvm_object_impl *impl;

	/* for free-list traversal */
	struct nvkvm_object *free_next;
};

/* ── Root client (NV01_ROOT_CLIENT) ──────────────────────────────────────── */

#define NVKVM_MAX_OBJECTS_PER_CLIENT  4096

struct nvkvm_client {
	uint32_t           handle;

	pthread_mutex_t    lock;
	struct nvkvm_object *resources[NVKVM_MAX_OBJECTS_PER_CLIENT];
	int                 nresources;
	bool                released;

	/* captured params for checkpoint/restore (future) */
};

/* ── Per-FD state (one per host fd opened on behalf of guest) ────────────── */

struct nvkvm_host_fd {
	int       fd;           /* real host fd                          */
	int       dev_id;       /* NVKVM_DEV_*                           */
	uint32_t  token;        /* fd_token as seen by guest             */
	uint32_t  session_id;   /* owning session                        */

	/* clients owned by this fd (NV01_ROOT_CLIENT allocations) */
	struct nvkvm_client **clients;
	int                   nclients;
	pthread_mutex_t       clients_lock;

	TAILQ_ENTRY(nvkvm_host_fd) link;
};

/* ── Mmap region (one per active GPU memory mapping) ─────────────────────── */

struct nvkvm_mmap_region {
	uint32_t  token;
	void     *host_va;      /* address returned by host mmap()       */
	size_t    length;
	uint64_t  guest_pa;     /* GPA we assigned in the guest          */
	int       kvm_slot;     /* KVM memory slot (-1 if not mapped)    */

	TAILQ_ENTRY(nvkvm_mmap_region) link;
};

/* ── Per-session state ────────────────────────────────────────────────────── */

#define NVKVM_MAX_FDS_PER_SESSION  256

struct nvkvm_session {
	uint32_t  id;
	pid_t     guest_tgid;

	pthread_mutex_t lock;

	/* fd table: fd_token → nvkvm_host_fd (legacy path) */
	TAILQ_HEAD(, nvkvm_host_fd)    fds;
	uint32_t                        next_fd_token;

	/* mmap region table */
	TAILQ_HEAD(, nvkvm_mmap_region) mmaps;
	uint32_t                         next_mmap_token;

	/* global client map: handle → nvkvm_client (RM object graph) */
	struct nvkvm_client   *clients[NVKVM_MAX_OBJECTS_PER_CLIENT];
	int                    nclients;
	pthread_mutex_t        clients_lock;

	/* isolate IDs active in this session (one per guest mm) */
	uint32_t isolate_ids[256];
	int      nisolates;

	TAILQ_ENTRY(nvkvm_session) link;
};

/* ── Virtio device state ─────────────────────────────────────────────────── */

typedef struct VirtIONvgpu {
	VirtIODevice        parent_obj;

	/* Virtqueues */
	VirtQueue          *vq_tx;
	VirtQueue          *vq_rx;
	VirtQueue          *vq_evt;

	/* Shared memory region */
	void               *shm_base;
	size_t              shm_size;
	size_t              slot_size;
	uint64_t            shm_gpa;    /* GPA where guest sees shared mem  */
	MemoryRegion        shm_mr;     /* QEMU memory region for shm_base  */
	bool                shm_mr_registered;

	/* Virtio config space (little-endian, copied out by get_config) */
	struct nvkvm_virtio_config config_space;

	/* Mmap window: GPA range for GPU memory mappings */
	uint64_t            mmap_win_gpa;
	size_t              mmap_win_size;
	uint64_t            mmap_win_cur;   /* next available GPA offset    */
	pthread_mutex_t     mmap_win_lock;

	/* Session table */
	TAILQ_HEAD(, nvkvm_session) sessions;
	pthread_mutex_t             sessions_lock;
	uint32_t                    next_session_id;

	/* Handle and isolate managers (isolate architecture) */
	struct nvkvm_handle_table   handles;
	struct nvkvm_isolate_table  isolates;

	/* Host NVIDIA driver version (read at init) */
	char                driver_version[64];
} VirtIONvgpu;

#define TYPE_VIRTIO_NVGPU  "virtio-nvgpu-device"
#define VIRTIO_NVGPU(obj)  OBJECT_CHECK(VirtIONvgpu, (obj), TYPE_VIRTIO_NVGPU)

/* ── Handler context (per in-flight request) ─────────────────────────────── */

struct nvkvm_req_ctx {
	VirtIONvgpu       *nv;
	VirtQueue         *vq;
	VirtQueueElement  *elem;

	struct nvkvm_session  *session;
	struct nvkvm_host_fd  *hfd;

	/* Pointers into shared memory (already validated for bounds) */
	void              *params_buf;
	size_t             param_size;
	void              *aux_buf;
	size_t             aux_size;

	/* For mmap requests */
	struct nvkvm_mmap_region *mmap_region;
};

/* ── Function declarations ───────────────────────────────────────────────── */

/* virtio_nvgpu.c */
void virtio_nvgpu_init(VirtIONvgpu *nv);
void virtio_nvgpu_fini(VirtIONvgpu *nv);

/* nvkvm_dispatch.c */
int  nvkvm_dispatch_ioctl(struct nvkvm_req_ctx *ctx,
			  unsigned int cmd);
size_t nvkvm_ioctl_expected_param_size(unsigned int cmd);

/* nvkvm_frontend.c */
int nvkvm_handle_rm_alloc(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_free(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_control(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_rm_dup_object(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_register_fd(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_alloc_os_event(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_free_os_event(struct nvkvm_req_ctx *ctx);
int nvkvm_handle_simple_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd);

/* nvkvm_isolate_handlers.c — new isolate/handle virtio request handlers */
int nvkvm_req_list_nvidia_devices(VirtIONvgpu *nv,
				   struct nvkvm_req_list_nvidia_devices *req,
				   struct nvkvm_resp_list_nvidia_devices *resp);
int nvkvm_req_open_nvidia_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_nvidia_handle *req,
				  struct nvkvm_resp_open_nvidia_handle *resp);
int nvkvm_req_open_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_memory_handle *req,
				  struct nvkvm_resp_open_memory_handle *resp);
int nvkvm_req_close_handle(VirtIONvgpu *nv,
			    struct nvkvm_req_close_handle *req,
			    struct nvkvm_resp_close_handle *resp);
int nvkvm_req_create_isolate(VirtIONvgpu *nv,
			      struct nvkvm_req_create_isolate *req,
			      struct nvkvm_resp_create_isolate *resp);
int nvkvm_req_kill_isolate(VirtIONvgpu *nv,
			    struct nvkvm_req_kill_isolate *req,
			    struct nvkvm_resp_kill_isolate *resp);
int nvkvm_req_copy_handle_to_isolate(VirtIONvgpu *nv,
				      struct nvkvm_req_copy_handle_to_isolate *req,
				      struct nvkvm_resp_copy_handle_to_isolate *resp);
int nvkvm_req_close_handle_on_isolate(VirtIONvgpu *nv,
				       struct nvkvm_req_close_handle_on_isolate *req,
				       struct nvkvm_resp_close_handle_on_isolate *resp);
int nvkvm_req_ioctl_on_isolate(VirtIONvgpu *nv,
				struct nvkvm_req_ioctl_on_isolate *req,
				struct nvkvm_resp_ioctl_on_isolate *resp,
				void *param_buf, void *aux_buf);
int nvkvm_req_mmap_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_mmap_on_isolate *req,
			       struct nvkvm_resp_mmap_on_isolate *resp);
int nvkvm_req_munmap_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_munmap_on_isolate *req,
				 struct nvkvm_resp_munmap_on_isolate *resp);
int nvkvm_req_poll_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_poll_on_isolate *req,
			       struct nvkvm_resp_poll_on_isolate *resp);
int nvkvm_req_unpoll_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_unpoll_on_isolate *req,
				 struct nvkvm_resp_unpoll_on_isolate *resp);
int nvkvm_req_write_memory_handle(VirtIONvgpu *nv,
				   struct nvkvm_req_write_memory_handle *req,
				   struct nvkvm_resp_write_memory_handle *resp,
				   void *data_buf);
int nvkvm_req_read_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_read_memory_handle *req,
				  struct nvkvm_resp_read_memory_handle *resp,
				  void *data_buf);

/* nvkvm_objects.c */
struct nvkvm_client *nvkvm_client_alloc(uint32_t handle);
void                 nvkvm_client_free(struct nvkvm_session *session,
				       struct nvkvm_client *client);
struct nvkvm_object *nvkvm_obj_lookup(struct nvkvm_client *client,
				      uint32_t handle);
int  nvkvm_obj_add(struct nvkvm_client *client, uint32_t handle,
		   uint32_t class_id, uint32_t parent_handle,
		   const struct nvkvm_object_impl *impl);
void nvkvm_obj_free(struct nvkvm_client *client, uint32_t handle);
void nvkvm_obj_add_dep(struct nvkvm_client *client,
		       uint32_t h1, uint32_t h2);

/* nvkvm_mmap_host.c */
void nvkvm_set_kvm_vm_fd(int fd);
void nvkvm_mmap_win_alloc(VirtIONvgpu *nv, size_t length, uint64_t *gpa_out);
int  nvkvm_mmap_create(VirtIONvgpu *nv, struct nvkvm_host_fd *hfd,
		       uint64_t offset, size_t length,
		       int prot, int flags,
		       struct nvkvm_mmap_region **region_out);
void nvkvm_mmap_destroy(VirtIONvgpu *nv,
			struct nvkvm_mmap_region *region);
int  nvkvm_mmap_map_to_guest(VirtIONvgpu *nv,
			     struct nvkvm_mmap_region *region);
void nvkvm_mmap_unmap_from_guest(VirtIONvgpu *nv,
				 struct nvkvm_mmap_region *region);

/* nvkvm_ptr.c — pointer translation for ioctl secondary buffers */
int nvkvm_translate_ptr_to_host(struct nvkvm_req_ctx *ctx,
				uint64_t guest_p64, size_t size,
				void **host_ptr_out);

/* Session helpers */
struct nvkvm_session *nvkvm_session_find(VirtIONvgpu *nv, uint32_t session_id);
struct nvkvm_session *nvkvm_session_create(VirtIONvgpu *nv, uint32_t guest_tgid);
struct nvkvm_host_fd *nvkvm_fd_lookup(struct nvkvm_session *session,
				      uint32_t fd_token);
uint32_t             nvkvm_fd_alloc_token(struct nvkvm_session *session,
					  struct nvkvm_host_fd *hfd);
void                 nvkvm_fd_remove(struct nvkvm_session *session,
				     uint32_t fd_token);

#endif /* VIRTIO_NVGPU_H */
