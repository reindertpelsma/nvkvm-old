/*
 * nvkvm_isolate_handlers.c — virtio request handlers for isolate/handle commands
 *
 * These handlers are invoked from the virtio TX queue dispatch when the guest
 * sends one of the NVKVM_REQ_* isolate/handle request types.
 *
 * Security: every handle_id and isolate_id is validated before use. Unknown
 * IDs cause the handler to return an error status; the caller in virtio_nvgpu.c
 * will panic the VM if these fields are structurally invalid (e.g., non-existent
 * session_id), but per-operation errors (ENOENT, EBUSY) are propagated normally.
 */

#include "qemu/osdep.h"
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "virtio_nvgpu.h"

/* ── Isolate mmap token table ────────────────────────────────────────────── */
/*
 * Each MMAP_ON_ISOLATE allocates one entry.  The token (index into this table)
 * is returned to the guest and used later for MUNMAP_ON_ISOLATE cleanup.
 *
 * Slot 0 is reserved (invalid token).  Tokens wrap in [1, MAX).
 */
#define NVKVM_ISO_MMAP_MAX  8192

struct nvkvm_iso_mmap_entry {
	bool     used;
	uint32_t isolate_id;
	uint64_t gva;        /* GVA mapped in the isolate */
	void    *qva;        /* QEMU host VA from mmap()  */
	size_t   len;
	int      kvm_slot;   /* KVM memory slot (-1 if none) */
	uint64_t gpa;
};

static struct nvkvm_iso_mmap_entry iso_mmap_tbl[NVKVM_ISO_MMAP_MAX];
static uint32_t iso_mmap_seq = 1;
static pthread_mutex_t iso_mmap_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t iso_mmap_alloc(uint32_t isolate_id, uint64_t gva, void *qva,
				size_t len, int kvm_slot, uint64_t gpa)
{
	pthread_mutex_lock(&iso_mmap_lock);
	for (uint32_t i = 0; i < NVKVM_ISO_MMAP_MAX - 1; i++) {
		uint32_t tok = iso_mmap_seq;
		iso_mmap_seq = (iso_mmap_seq % (NVKVM_ISO_MMAP_MAX - 1)) + 1;
		if (!iso_mmap_tbl[tok].used) {
			iso_mmap_tbl[tok].used       = true;
			iso_mmap_tbl[tok].isolate_id = isolate_id;
			iso_mmap_tbl[tok].gva        = gva;
			iso_mmap_tbl[tok].qva        = qva;
			iso_mmap_tbl[tok].len        = len;
			iso_mmap_tbl[tok].kvm_slot   = kvm_slot;
			iso_mmap_tbl[tok].gpa        = gpa;
			pthread_mutex_unlock(&iso_mmap_lock);
			return tok;
		}
	}
	pthread_mutex_unlock(&iso_mmap_lock);
	return 0; /* table full */
}

static bool iso_mmap_free(uint32_t token, struct nvkvm_iso_mmap_entry *out)
{
	if (token == 0 || token >= NVKVM_ISO_MMAP_MAX)
		return false;
	pthread_mutex_lock(&iso_mmap_lock);
	if (!iso_mmap_tbl[token].used) {
		pthread_mutex_unlock(&iso_mmap_lock);
		return false;
	}
	*out = iso_mmap_tbl[token];
	iso_mmap_tbl[token].used = false;
	pthread_mutex_unlock(&iso_mmap_lock);
	return true;
}

/* ── Device enumeration ──────────────────────────────────────────────────── */

int nvkvm_req_list_nvidia_devices(VirtIONvgpu *nv,
				   struct nvkvm_req_list_nvidia_devices *req,
				   struct nvkvm_resp_list_nvidia_devices *resp)
{
	(void)nv;
	(void)req;

	memset(resp, 0, sizeof(*resp));

	/* Always include nvidiactl and nvidia-uvm */
	int n = 0;

	if (access("/dev/nvidiactl", F_OK) == 0) {
		resp->devices[n].dev_id = NVKVM_DEV_CTL;
		n++;
	}
	if (access("/dev/nvidia-uvm", F_OK) == 0) {
		resp->devices[n].dev_id = NVKVM_DEV_UVM;
		n++;
	}

	/* Scan /dev/nvidia0..15 */
	for (int i = 0; i < 16 && n < NVKVM_MAX_DEVICES; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/nvidia%d", i);
		if (access(path, F_OK) == 0) {
			resp->devices[n].dev_id = NVKVM_DEV_GPU(i);
			n++;
		}
	}

	resp->ndevices = (uint32_t)n;
	resp->status   = 0;
	return 0;
}

/* ── Handle open ─────────────────────────────────────────────────────────── */

int nvkvm_req_open_nvidia_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_nvidia_handle *req,
				  struct nvkvm_resp_open_nvidia_handle *resp)
{
	uint32_t handle_id = 0;
	int ret = nvkvm_handle_open_nvidia(&nv->handles,
					   req->session_id,
					   (int)req->dev_id,
					   (int)req->flags,
					   &handle_id);
	if (ret < 0) {
		resp->handle_id = 0;
		resp->status    = (uint32_t)-ret;
	} else {
		resp->handle_id = handle_id;
		resp->status    = 0;
	}
	return 0;
}

int nvkvm_req_open_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_memory_handle *req,
				  struct nvkvm_resp_open_memory_handle *resp)
{
	uint32_t handle_id = 0;
	int ret = nvkvm_handle_open_memory(&nv->handles,
					   req->session_id,
					   req->size,
					   &handle_id);
	if (ret < 0) {
		resp->handle_id = 0;
		resp->status    = (uint32_t)-ret;
	} else {
		resp->handle_id = handle_id;
		resp->status    = 0;
	}
	return 0;
}

int nvkvm_req_close_handle(VirtIONvgpu *nv,
			    struct nvkvm_req_close_handle *req,
			    struct nvkvm_resp_close_handle *resp)
{
	int ret = nvkvm_handle_close(&nv->handles, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Isolate lifecycle ───────────────────────────────────────────────────── */

int nvkvm_req_create_isolate(VirtIONvgpu *nv,
			      struct nvkvm_req_create_isolate *req,
			      struct nvkvm_resp_create_isolate *resp)
{
	uint32_t isolate_id = 0;
	int ret = nvkvm_isolate_create(&nv->isolates, req->session_id, &isolate_id);
	if (ret < 0) {
		resp->isolate_id = 0;
		resp->status     = (uint32_t)-ret;
		return 0;
	}

	/* Record isolate_id in session */
	struct nvkvm_session *session = nvkvm_session_find(nv, req->session_id);
	if (session) {
		pthread_mutex_lock(&session->lock);
		if (session->nisolates < 256)
			session->isolate_ids[session->nisolates++] = isolate_id;
		pthread_mutex_unlock(&session->lock);
	}

	resp->isolate_id = isolate_id;
	resp->status     = 0;
	return 0;
}

int nvkvm_req_kill_isolate(VirtIONvgpu *nv,
			    struct nvkvm_req_kill_isolate *req,
			    struct nvkvm_resp_kill_isolate *resp)
{
	int ret = nvkvm_isolate_kill(&nv->isolates, req->isolate_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Handle distribution ────────────────────────────────────────────────── */

int nvkvm_req_copy_handle_to_isolate(VirtIONvgpu *nv,
				      struct nvkvm_req_copy_handle_to_isolate *req,
				      struct nvkvm_resp_copy_handle_to_isolate *resp)
{
	int ret = nvkvm_isolate_send_handle(&nv->isolates, &nv->handles,
					    req->isolate_id, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

int nvkvm_req_close_handle_on_isolate(VirtIONvgpu *nv,
				       struct nvkvm_req_close_handle_on_isolate *req,
				       struct nvkvm_resp_close_handle_on_isolate *resp)
{
	int ret = nvkvm_isolate_close_handle(&nv->isolates, &nv->handles,
					     req->isolate_id, req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Ioctl on isolate ────────────────────────────────────────────────────── */

int nvkvm_req_ioctl_on_isolate(VirtIONvgpu *nv,
				struct nvkvm_req_ioctl_on_isolate *req,
				struct nvkvm_resp_ioctl_on_isolate *resp,
				void *param_buf, void *aux_buf)
{
	/*
	 * REGISTER_FD must be handled directly by QEMU: the stub holds the
	 * real host fds (same kernel file descriptions via SCM_RIGHTS), but
	 * the param carries a guest fd_token rather than a host fd number.
	 * We translate token → host fd here and call ioctl() ourselves, so
	 * the kernel associates the correct underlying file descriptions.
	 */
	if (_IOC_NR(req->cmd) == NV_ESC_REGISTER_FD && param_buf) {
		struct nv_ioctl_register_fd *p = param_buf;
		struct nvkvm_session *session;
		struct nvkvm_host_fd *ctl_hfd;
		struct nvkvm_handle  *gpu_h;
		long ret;

		pthread_mutex_lock(&nv->sessions_lock);
		session = nvkvm_session_find(nv, req->session_id);
		pthread_mutex_unlock(&nv->sessions_lock);
		if (!session) { resp->retval = (uint64_t)(int64_t)-EBADF; return 0; }

		pthread_mutex_lock(&session->lock);
		ctl_hfd = nvkvm_fd_lookup(session, (uint32_t)p->ctl_fd);
		pthread_mutex_unlock(&session->lock);
		if (!ctl_hfd) { resp->retval = (uint64_t)(int64_t)-EBADF; return 0; }

		gpu_h = nvkvm_handle_get(&nv->handles, req->handle_id);
		if (!gpu_h || gpu_h->fd < 0) {
			resp->retval = (uint64_t)(int64_t)-EBADF;
			return 0;
		}

		int32_t saved_ctl = p->ctl_fd;
		p->ctl_fd = (int32_t)ctl_hfd->fd;
		ret = ioctl(gpu_h->fd, req->cmd, p);
		if (ret < 0) ret = -errno;
		p->ctl_fd = saved_ctl;

		resp->retval = (ret < 0) ? (uint64_t)(int64_t)ret : (uint64_t)ret;
		fprintf(stderr,
			"nvkvm: register_fd (isolate path): isolate=%u handle=%u "
			"gpu_fd=%d ctl_fd=%d ret=%lld\n",
			req->isolate_id, req->handle_id,
			gpu_h->fd, ctl_hfd->fd, (long long)ret);
		return 0;
	}

	uint32_t nvstatus  = 0;
	uint64_t fault_addr = 0;
	int ret = nvkvm_isolate_ioctl(&nv->isolates,
				      req->isolate_id,
				      req->handle_id,
				      req->cmd,
				      param_buf, req->param_size,
				      aux_buf,   req->aux_size,
				      req->flags,
				      &nvstatus,
				      &fault_addr);

	resp->retval     = (ret < 0) ? (uint64_t)(int64_t)ret : (uint64_t)ret;
	resp->status     = (ret == -EFAULT && fault_addr) ? EFAULT : 0;
	resp->nvstatus   = nvstatus;
	resp->fault_addr = fault_addr;

	/* For RM_CONTROL, also extract the inner cmd at param offset 8 so we
	 * can see which control specifically returned a non-zero nvstatus. */
	uint32_t inner_cmd = 0;
	if (_IOC_NR(req->cmd) == NV_ESC_RM_CONTROL && param_buf &&
	    req->param_size >= 12) {
		memcpy(&inner_cmd, (char *)param_buf + 8, sizeof(uint32_t));
	}

	/* DIAG: for RM_MAP_MEMORY, dump all params so we can see what the
	 * kernel saw and what it returned. */
	if (_IOC_NR(req->cmd) == NV_ESC_RM_MAP_MEMORY && param_buf &&
	    req->param_size >= 48) {
		uint32_t h_client = 0, h_device = 0, h_memory = 0;
		uint64_t offset = 0, length = 0, plinear = 0;
		uint32_t mm_status = 0, flags = 0;
		int32_t fd = 0;
		memcpy(&h_client, (char *)param_buf + 0,  sizeof(uint32_t));
		memcpy(&h_device, (char *)param_buf + 4,  sizeof(uint32_t));
		memcpy(&h_memory, (char *)param_buf + 8,  sizeof(uint32_t));
		memcpy(&offset,   (char *)param_buf + 16, sizeof(uint64_t));
		memcpy(&length,   (char *)param_buf + 24, sizeof(uint64_t));
		memcpy(&plinear,  (char *)param_buf + 32, sizeof(uint64_t));
		memcpy(&mm_status,(char *)param_buf + 40, sizeof(uint32_t));
		memcpy(&flags,    (char *)param_buf + 44, sizeof(uint32_t));
		memcpy(&fd,       (char *)param_buf + 48, sizeof(int32_t));
		fprintf(stderr,
			"nvkvm: RM_MAP_MEMORY: h_client=0x%x h_device=0x%x "
			"h_memory=0x%x offset=0x%llx length=0x%llx flags=0x%x "
			"fd=%d -> pLinear=0x%llx status=0x%x\n",
			h_client, h_device, h_memory,
			(unsigned long long)offset, (unsigned long long)length,
			flags, fd, (unsigned long long)plinear, mm_status);
	}

	/* DIAG: for RM_ALLOC, dump hClient/hParent/hObjNew/hClass when
	 * nvstatus is non-zero, so we can see which class the driver
	 * rejected.  nvos21 has hClient, hParent, hObjNew, hClass at the
	 * start; nvos64 has the same layout for the first 16 bytes. */
	if (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC && nvstatus &&
	    param_buf && req->param_size >= 16) {
		uint32_t hClient = 0, hParent = 0, hObjNew = 0, hClass = 0;
		memcpy(&hClient, (char *)param_buf + 0,  sizeof(uint32_t));
		memcpy(&hParent, (char *)param_buf + 4,  sizeof(uint32_t));
		memcpy(&hObjNew, (char *)param_buf + 8,  sizeof(uint32_t));
		memcpy(&hClass,  (char *)param_buf + 12, sizeof(uint32_t));
		uint32_t aps = 0;
		if (req->param_size == sizeof(struct nvos64_parameters))
			memcpy(&aps, (char *)param_buf + 32, sizeof(uint32_t));
		fprintf(stderr,
			"nvkvm: RM_ALLOC failed: hClient=0x%x hParent=0x%x "
			"hObjNew=0x%x hClass=0x%x alloc_parms_size=%u aux_size=%u "
			"nvstatus=0x%x\n",
			hClient, hParent, hObjNew, hClass, aps,
			req->aux_size, nvstatus);
		/* hex dump first 64 bytes of aux_buf (the alloc params themselves) */
		if (aux_buf && req->aux_size > 0) {
			const uint8_t *b = aux_buf;
			uint32_t n = req->aux_size < 64 ? req->aux_size : 64;
			char hex[256] = {0};
			for (uint32_t i = 0; i < n; i++)
				snprintf(hex + i*3, sizeof(hex)-i*3, "%02x ", b[i]);
			fprintf(stderr, "nvkvm: RM_ALLOC failed aux[%u]: %s\n",
				n, hex);
		}
	}

	if (inner_cmd) {
		fprintf(stderr,
			"nvkvm: ioctl_on_isolate: isolate=%u handle=%u cmd=0x%x "
			"inner=0x%x ret=%lld nvstatus=0x%x fault=0x%llx\n",
			req->isolate_id, req->handle_id, req->cmd, inner_cmd,
			(long long)ret, nvstatus, (unsigned long long)fault_addr);
	} else {
		fprintf(stderr,
			"nvkvm: ioctl_on_isolate: isolate=%u handle=%u cmd=0x%x "
			"ret=%lld nvstatus=0x%x fault=0x%llx\n",
			req->isolate_id, req->handle_id, req->cmd,
			(long long)ret, nvstatus, (unsigned long long)fault_addr);
	}

	/* Trace UVM ioctls' rm_status field so we can see what the driver
	 * actually wrote back through the isolate path. */
	if (param_buf && req->param_size >= 8) {
		uint32_t rm_status_off = (uint32_t)-1;
		switch (req->cmd) {
		case 0x30000001: /* UVM_INITIALIZE: { __u64 flags; __u32 rm_status; ... } */
			rm_status_off = 8;
			break;
		case 0x30000002: /* UVM_DEINITIALIZE: { __u32 rm_status; } */
			rm_status_off = 0;
			break;
		case 75:         /* UVM_MM_INITIALIZE: { __s32 uvm_fd; __u32 rm_status; } */
			rm_status_off = 4;
			break;
		case 39:         /* UVM_PAGEABLE_MEM_ACCESS: { __u8 pageable_mem_access; __u32 rm_status; } */
			rm_status_off = 4;
			break;
		}
		if (rm_status_off != (uint32_t)-1 &&
		    req->param_size >= rm_status_off + 4) {
			uint32_t rmst = 0;
			memcpy(&rmst, (char *)param_buf + rm_status_off, 4);
			fprintf(stderr,
				"nvkvm: ioctl_on_isolate UVM: cmd=0x%x rm_status=0x%x\n",
				req->cmd, rmst);
		}
	}
	return 0;
}

/* ── Mmap on isolate ─────────────────────────────────────────────────────── */

/*
 * Double-mmap implementation:
 *   1. QEMU mmaps the handle fd at any QVA (mmap(NULL)).
 *   2. QEMU registers GPA→QVA in KVM (KVM_SET_USER_MEMORY_REGION).
 *   3. QEMU sends MMAP command to isolate: map same fd at gva (MAP_FIXED).
 */


extern int nvkvm_kvm_vm_fd;

#ifndef KVM_SET_USER_MEMORY_REGION
#define NVKVM_KVMIO 0xAE
struct nvkvm_kvm_mem_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#define KVM_SET_USER_MEMORY_REGION _IOW(NVKVM_KVMIO, 0x46, struct nvkvm_kvm_mem_region)
#endif

/* KVM slot counter for double-mmap registrations (starts at 100 to avoid
 * conflicts with QEMU's own slots which start at 0). */
static uint32_t iso_kvm_slot_counter = 100;

int nvkvm_req_mmap_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_mmap_on_isolate *req,
			       struct nvkvm_resp_mmap_on_isolate *resp)
{
	memset(resp, 0, sizeof(*resp));

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}

	size_t len = (size_t)req->length;
	len = (len + 4095UL) & ~4095UL;  /* page-align */

	/* Step 1: mmap in QEMU at any address */
	void *qva = mmap(NULL, len, req->prot,
			 MAP_SHARED, h->fd, (off_t)req->offset);
	if (qva == MAP_FAILED) {
		resp->status = (uint32_t)errno;
		return 0;
	}

	/* Step 2: allocate GPA from mmap window and register KVM slot */
	uint64_t gpa = 0;
	nvkvm_mmap_win_alloc(nv, len, &gpa);
	if (gpa == 0) {
		munmap(qva, len);
		resp->status = ENOMEM;
		return 0;
	}

	int kvm_slot = -1;
	if (nvkvm_kvm_vm_fd >= 0) {
		kvm_slot = (int)iso_kvm_slot_counter;
		struct nvkvm_kvm_mem_region mr = {
			.slot            = iso_kvm_slot_counter++,
			.flags           = 0,
			.guest_phys_addr = gpa,
			.memory_size     = len,
			.userspace_addr  = (uint64_t)(uintptr_t)qva,
		};
		if (ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr) < 0) {
			fprintf(stderr, "nvkvm: KVM_SET_USER_MEMORY_REGION: %s\n",
				strerror(errno));
			kvm_slot = -1; /* non-fatal */
		}
	}

	/* Step 3: send MMAP command to isolate */
	int ret = nvkvm_isolate_mmap(&nv->isolates,
				     req->isolate_id,
				     req->handle_id,
				     req->gva, len, req->offset,
				     (int)req->prot,
				     (int)req->map_flags);

	if (ret < 0) {
		if (kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
			struct nvkvm_kvm_mem_region mr = { .slot = (uint32_t)kvm_slot,
							   .memory_size = 0 };
			ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
		}
		munmap(qva, len);
		resp->status = (uint32_t)-ret;
		return 0;
	}

	/* Record for future MUNMAP_ON_ISOLATE */
	uint32_t token = iso_mmap_alloc(req->isolate_id, req->gva, qva,
					len, kvm_slot, gpa);
	if (token == 0) {
		fprintf(stderr, "nvkvm: iso_mmap_tbl full\n");
		token = 0xdeadbeef; /* non-zero; munmap will fail gracefully */
	}

	resp->mmap_token = token;
	resp->gpa_base   = gpa;
	resp->length     = (uint64_t)len;
	resp->status     = 0;
	return 0;
}

int nvkvm_req_munmap_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_munmap_on_isolate *req,
				 struct nvkvm_resp_munmap_on_isolate *resp)
{
	struct nvkvm_iso_mmap_entry e;

	if (!iso_mmap_free(req->mmap_token, &e)) {
		resp->status = ENOENT;
		return 0;
	}

	/* Tell the isolate to unmap the GVA range */
	nvkvm_isolate_munmap(&nv->isolates, e.isolate_id, e.gva, (uint64_t)e.len);

	/* Remove the KVM memory slot */
	if (e.kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
		struct nvkvm_kvm_mem_region mr = {
			.slot        = (uint32_t)e.kvm_slot,
			.memory_size = 0,
		};
		ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
	}

	/* Unmap the QEMU host VA */
	if (e.qva)
		munmap(e.qva, e.len);

	resp->status = 0;
	return 0;
}

/* ── Poll on isolate ─────────────────────────────────────────────────────── */

int nvkvm_req_poll_on_isolate(VirtIONvgpu *nv,
			       struct nvkvm_req_poll_on_isolate *req,
			       struct nvkvm_resp_poll_on_isolate *resp)
{
	int ret = nvkvm_isolate_poll(&nv->isolates,
				     req->isolate_id,
				     req->handle_id,
				     req->events);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

int nvkvm_req_unpoll_on_isolate(VirtIONvgpu *nv,
				 struct nvkvm_req_unpoll_on_isolate *req,
				 struct nvkvm_resp_unpoll_on_isolate *resp)
{
	int ret = nvkvm_isolate_unpoll(&nv->isolates,
				       req->isolate_id,
				       req->handle_id);
	resp->status = (ret < 0) ? (uint32_t)-ret : 0;
	return 0;
}

/* ── Memory handle I/O (CPU page migration) ──────────────────────────────── */

int nvkvm_req_write_memory_handle(VirtIONvgpu *nv,
				   struct nvkvm_req_write_memory_handle *req,
				   struct nvkvm_resp_write_memory_handle *resp,
				   void *data_buf)
{
	resp->status = 0;
	resp->reserved = 0;

	if (!data_buf || req->size == 0) {
		resp->status = EINVAL;
		return 0;
	}

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}

	ssize_t n = pwrite(h->fd, data_buf, req->size, (off_t)req->offset);
	if (n < 0) {
		resp->status = (uint32_t)errno;
	} else if ((uint32_t)n != req->size) {
		resp->status = EIO;
	}
	return 0;
}

int nvkvm_req_read_memory_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_read_memory_handle *req,
				  struct nvkvm_resp_read_memory_handle *resp,
				  void *data_buf)
{
	resp->status = 0;
	resp->reserved = 0;

	if (!data_buf || req->size == 0) {
		resp->status = EINVAL;
		return 0;
	}

	struct nvkvm_handle *h = nvkvm_handle_get(&nv->handles, req->handle_id);
	if (!h || h->fd < 0) {
		resp->status = EBADF;
		return 0;
	}

	ssize_t n = pread(h->fd, data_buf, req->size, (off_t)req->offset);
	if (n < 0) {
		resp->status = (uint32_t)errno;
	} else if ((uint32_t)n != req->size) {
		resp->status = EIO;
	}
	return 0;
}
