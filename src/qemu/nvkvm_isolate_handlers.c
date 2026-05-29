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
	bool     stub_mirrored; /* true if isolate-side mmap was also installed */
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
				size_t len, int kvm_slot, uint64_t gpa,
				bool stub_mirrored)
{
	pthread_mutex_lock(&iso_mmap_lock);
	for (uint32_t i = 0; i < NVKVM_ISO_MMAP_MAX - 1; i++) {
		uint32_t tok = iso_mmap_seq;
		iso_mmap_seq = (iso_mmap_seq % (NVKVM_ISO_MMAP_MAX - 1)) + 1;
		if (!iso_mmap_tbl[tok].used) {
			iso_mmap_tbl[tok].used          = true;
			iso_mmap_tbl[tok].stub_mirrored = stub_mirrored;
			iso_mmap_tbl[tok].isolate_id    = isolate_id;
			iso_mmap_tbl[tok].gva           = gva;
			iso_mmap_tbl[tok].qva           = qva;
			iso_mmap_tbl[tok].len           = len;
			iso_mmap_tbl[tok].kvm_slot      = kvm_slot;
			iso_mmap_tbl[tok].gpa           = gpa;
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

/*
 * Look up the (first) isolate for a session. Sessions may eventually carry
 * multiple isolates (post-fork); Step 6 handles that lazily — for now the
 * guest opens one isolate per session before any /dev/nvidia* open and the
 * first slot is the active one.
 */
static uint32_t session_first_isolate(VirtIONvgpu *nv, uint32_t session_id)
{
	uint32_t iso_id = 0;
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *s = nvkvm_session_find(nv, session_id);
	if (s) {
		pthread_mutex_lock(&s->lock);
		if (s->nisolates > 0)
			iso_id = s->isolate_ids[0];
		pthread_mutex_unlock(&s->lock);
	}
	pthread_mutex_unlock(&nv->sessions_lock);
	return iso_id;
}

int nvkvm_req_open_nvidia_handle(VirtIONvgpu *nv,
				  struct nvkvm_req_open_nvidia_handle *req,
				  struct nvkvm_resp_open_nvidia_handle *resp)
{
	uint32_t handle_id = 0;
	int ret;

	/*
	 * UVM stays opened in QEMU (driver enforces opener-does-mmap, and
	 * mmap is done in QEMU for KVM region installation). The other
	 * devices — /dev/nvidiactl, /dev/nvidia0..N, and the eventfd that
	 * stands in for the guest's libcuda eventfd — open inside the
	 * isolate so nvfp/mm lineage matches the process that runs RM
	 * ioctls. See docs/REFACTOR_PLAN.md §1 open-ownership table.
	 */
	if ((int)req->dev_id == NVKVM_DEV_UVM) {
		ret = nvkvm_handle_open_nvidia(&nv->handles,
					       req->session_id,
					       (int)req->dev_id,
					       (int)req->flags,
					       &handle_id);
		if (ret < 0)
			goto out;
		/*
		 * Stub swaps the SCM_RIGHTS-received UVM fd for one of its
		 * own pre-opened local UVM fds (file-owner-mm match for
		 * UVM_MM_INITIALIZE). We still need to send a RECEIVE_FD so
		 * the stub knows about the handle_id → local-fd mapping.
		 * If the session has no isolate yet, this is deferred until
		 * the guest creates one and re-issues COPY_HANDLE_TO_ISOLATE
		 * (legacy compat — Step 3d removes that fallback).
		 */
		{
			uint32_t iso = session_first_isolate(nv, req->session_id);
			if (iso != 0)
				nvkvm_isolate_send_handle(&nv->isolates,
							   &nv->handles,
							   iso, handle_id);
		}
		goto out;
	}

	uint32_t iso_id = session_first_isolate(nv, req->session_id);
	if (iso_id == 0) {
		/*
		 * No isolate yet. Guest must call CREATE_ISOLATE before the
		 * first non-UVM open. Returned to the guest so it can either
		 * reorder or fail the open syscall.
		 */
		ret = -ENOENT;
		goto out;
	}

	ret = nvkvm_handle_alloc_pending(&nv->handles, req->session_id,
					 (int)req->dev_id, &handle_id);
	if (ret < 0)
		goto out;

	int fd_from_scm = -1;
	ret = nvkvm_isolate_open_device(&nv->isolates, iso_id, handle_id,
					req->dev_id, req->flags,
					&fd_from_scm);
	if (ret < 0) {
		nvkvm_handle_abort_open(&nv->handles, handle_id);
		handle_id = 0;
		goto out;
	}

	ret = nvkvm_handle_attach_fd(&nv->handles, handle_id, fd_from_scm);
	if (ret < 0) {
		/* Shouldn't happen on a fresh slot; clean up if it does. */
		close(fd_from_scm);
		nvkvm_handle_abort_open(&nv->handles, handle_id);
		handle_id = 0;
		goto out;
	}

	/*
	 * Bump the isolate refcount to mirror what nvkvm_isolate_send_handle
	 * did in the legacy COPY_HANDLE_TO_ISOLATE flow: the stub now holds
	 * one copy of this fd (the original); QEMU holds the SCM_RIGHTS copy
	 * as qemu_fd. Close-handle must refuse until the isolate releases.
	 */
	nvkvm_handle_ref_isolate(&nv->handles, handle_id);
	ret = 0;

out:
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

	/*
	 * Find-or-create the QEMU-side session. The legacy NVKVM_REQ_OPEN
	 * used to create it as a side effect of the first device open; in
	 * the new flow CREATE_ISOLATE is the first request the guest sends
	 * for a fresh session, so we own the creation here.
	 */
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *session = nvkvm_session_find(nv, req->session_id);
	pthread_mutex_unlock(&nv->sessions_lock);
	if (!session)
		session = nvkvm_session_create(nv, req->session_id);

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

	/*
	 * Walk every session and prune the killed isolate from its
	 * isolate_ids[] list. Without this, session_first_isolate
	 * later returns a stale (dead) isolate_id and the OPEN_DEVICE
	 * round-trip fails — the session can outlive its isolate in
	 * the test-cycle case (session_id is reused after the guest
	 * idr_remove + new alloc lands the same id).
	 */
	pthread_mutex_lock(&nv->sessions_lock);
	struct nvkvm_session *s;
	TAILQ_FOREACH(s, &nv->sessions, link) {
		pthread_mutex_lock(&s->lock);
		int dst = 0;
		for (int i = 0; i < s->nisolates; i++) {
			if (s->isolate_ids[i] != req->isolate_id) {
				s->isolate_ids[dst++] = s->isolate_ids[i];
			}
		}
		s->nisolates = dst;
		pthread_mutex_unlock(&s->lock);
	}
	pthread_mutex_unlock(&nv->sessions_lock);

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

/* ── Phase 3: UVM ioctl field schema + default-deny ──────────────────────────
 *
 * UVM ioctls execute in QEMU's (privileged) process, so we MUST NOT blindly
 * forward an arbitrary cmd the guest names.  This table is the allowlist: each
 * UVM cmd we will forward is listed with (a) a minimum param_size and (b) the
 * offsets of any embedded *frontend fd* fields (RMCtrlFD / UvmFD) that the
 * guest sanitizer rewrote into a handle_id and that the kernel will dereference
 * as an fd — those are translated to QEMU's local fd and restored on response.
 *
 * Field offsets/sizes are taken from the open kernel module / gVisor nvproxy
 * (pkg/abi/nvgpu/uvm.go): NvUUID is 16 bytes, Handle/NvU32 is 4 bytes, the
 * frontend fd is an NvS32 at a fixed offset in the struct header (before any
 * variable-length PerGPUAttributes array, so the offset is version-stable).
 *
 * DEFAULT-DENY: any UVM cmd absent from this table is refused, never forwarded.
 * Notably this denies UVM_TOOLS_READ/WRITE_PROCESS_MEMORY (62/63) — a
 * cross-process memory peek/poke primitive that has no place in our model.
 * min_size uses the base (pre-V550) struct size as a conservative floor; the
 * fd-field read is additionally guarded against the actual param_size.
 */
enum { NVKVM_UVM_FD_FIELD = 1 };
struct nvkvm_uvm_desc {
	uint32_t cmd;
	uint16_t min_size;
	uint16_t fd_off[2];   /* frontend-fd field byte offsets; 0xffff = none */
};
/*
 * min_size is the EXACT struct size from our ABI (src/abi/uvm.h, driver
 * 575.51.03) — verified by sizeof, NOT copied from gVisor's newer layouts
 * (several differ: e.g. REGISTER_GPU is 32B here, not gVisor's 40B-with-NUMA;
 * REGISTER_CHANNEL 48 not 56; MIGRATE 48 not 56).  The guest always sends
 * exactly this size, so "param_size < min_size" rejects only malformed calls.
 * fd-field translation is limited to the two cmds the prior code translated
 * (MM_INITIALIZE@0, REGISTER_GPU_VASPACE@16); every other cmd forwarded with
 * its fd field untouched, exactly as before — generalizing it was speculative.
 */
static const struct nvkvm_uvm_desc nvkvm_uvm_schema[] = {
	/* The full UVM command set (open kernel module / gVisor nvproxy
	 * uvm.go).  min_size: cmds whose struct is defined in our ABI
	 * (src/abi/uvm.h, driver 575.51.03) carry the exact sizeof, verified
	 * by measurement — these are the layouts the guest actually sends, so
	 * "param_size < min" rejects only malformed calls.  The five cmds NOT
	 * in our ABI (44/45/53/65/66) carry min_size 0 (allow any size): we
	 * have no driver-verified layout for them and an over-strict guess
	 * already mis-denied REGISTER_GPU once; the kernel validates its own
	 * struct against the fixed shm slot regardless.  fd-field translation
	 * stays limited to the two cmds the pre-schema code translated. */
	{ 0x30000001 /* UVM_INITIALIZE          */,  16, { 0xffff, 0xffff } },
	{ 0x30000002 /* UVM_DEINITIALIZE        */,   8, { 0xffff, 0xffff } },
	{ 23 /* UVM_CREATE_RANGE_GROUP          */,  16, { 0xffff, 0xffff } },
	{ 24 /* UVM_DESTROY_RANGE_GROUP         */,  16, { 0xffff, 0xffff } },
	{ 25 /* UVM_REGISTER_GPU_VASPACE        */,  32, { 16,     0xffff } },
	{ 26 /* UVM_UNREGISTER_GPU_VASPACE      */,  20, { 0xffff, 0xffff } },
	{ 27 /* UVM_REGISTER_CHANNEL            */,  48, { 0xffff, 0xffff } },
	{ 28 /* UVM_UNREGISTER_CHANNEL          */,  28, { 0xffff, 0xffff } },
	{ 29 /* UVM_ENABLE_PEER_ACCESS          */,  40, { 0xffff, 0xffff } },
	{ 30 /* UVM_DISABLE_PEER_ACCESS         */,  40, { 0xffff, 0xffff } },
	{ 31 /* UVM_SET_RANGE_GROUP             */,  32, { 0xffff, 0xffff } },
	{ 33 /* UVM_MAP_EXTERNAL_ALLOCATION     */, 9264, { 0xffff, 0xffff } },
	{ 34 /* UVM_FREE                        */,  24, { 0xffff, 0xffff } },
	{ 37 /* UVM_REGISTER_GPU                */,  32, { 0xffff, 0xffff } },
	{ 38 /* UVM_UNREGISTER_GPU              */,  24, { 0xffff, 0xffff } },
	{ 39 /* UVM_PAGEABLE_MEM_ACCESS         */,   8, { 0xffff, 0xffff } },
	{ 42 /* UVM_SET_PREFERRED_LOCATION      */,  40, { 0xffff, 0xffff } },
	{ 43 /* UVM_UNSET_PREFERRED_LOCATION    */,  24, { 0xffff, 0xffff } },
	{ 44 /* UVM_ENABLE_READ_DUPLICATION     */,   0, { 0xffff, 0xffff } },
	{ 45 /* UVM_DISABLE_READ_DUPLICATION    */,   0, { 0xffff, 0xffff } },
	{ 46 /* UVM_SET_ACCESSED_BY             */,  40, { 0xffff, 0xffff } },
	{ 47 /* UVM_UNSET_ACCESSED_BY           */,  40, { 0xffff, 0xffff } },
	{ 51 /* UVM_MIGRATE                     */,  48, { 0xffff, 0xffff } },
	{ 53 /* UVM_MIGRATE_RANGE_GROUP         */,   0, { 0xffff, 0xffff } },
	{ 65 /* UVM_MAP_DYNAMIC_PARALLELISM_REGION */, 0, { 0xffff, 0xffff } },
	{ 66 /* UVM_UNMAP_EXTERNAL              */,   0, { 0xffff, 0xffff } },
	{ 68 /* UVM_ALLOC_SEMAPHORE_POOL        */, 9248, { 0xffff, 0xffff } },
	{ 70 /* UVM_PAGEABLE_MEM_ACCESS_ON_GPU  */,  24, { 0xffff, 0xffff } },
	{ 72 /* UVM_VALIDATE_VA_RANGE           */,  24, { 0xffff, 0xffff } },
	{ 73 /* UVM_CREATE_EXTERNAL_RANGE       */,  24, { 0xffff, 0xffff } },
	{ 75 /* UVM_MM_INITIALIZE               */,   8, { 0,      0xffff } },
	/* Default-denied by omission: UVM_TOOLS_READ_PROCESS_MEMORY (62) and
	 * UVM_TOOLS_WRITE_PROCESS_MEMORY (63) — a cross-process memory
	 * peek/poke primitive with no place in our isolation model — plus any
	 * unknown/garbage cmd a malicious guest might name. */
};

static const struct nvkvm_uvm_desc *nvkvm_uvm_lookup(uint32_t cmd)
{
	for (size_t i = 0;
	     i < sizeof(nvkvm_uvm_schema) / sizeof(nvkvm_uvm_schema[0]); i++) {
		if (nvkvm_uvm_schema[i].cmd == cmd)
			return &nvkvm_uvm_schema[i];
	}
	return NULL;
}

/* ── Phase 4: per-VM RM client-handle allowlist ─────────────────────────────
 * Record every hClient this VM's isolates successfully use, and vet foreign
 * hClient references (e.g. DUP_OBJECT h_client_src) against the set.  See the
 * VirtIONvgpu.client_allow comment for why fds are not the boundary. */
static void nvkvm_client_allow_add(VirtIONvgpu *nv, uint32_t hc)
{
	if (hc == 0 || hc == (uint32_t)-1)
		return;
	pthread_mutex_lock(&nv->client_allow_lock);
	for (uint32_t i = 0; i < nv->client_allow_n; i++) {
		if (nv->client_allow[i] == hc) {
			pthread_mutex_unlock(&nv->client_allow_lock);
			return;
		}
	}
	if (nv->client_allow_n < NVKVM_CLIENT_ALLOWLIST_MAX)
		nv->client_allow[nv->client_allow_n++] = hc;
	pthread_mutex_unlock(&nv->client_allow_lock);
}

static bool nvkvm_client_allow_has(VirtIONvgpu *nv, uint32_t hc)
{
	bool found = false;
	pthread_mutex_lock(&nv->client_allow_lock);
	for (uint32_t i = 0; i < nv->client_allow_n; i++) {
		if (nv->client_allow[i] == hc) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&nv->client_allow_lock);
	return found;
}

int nvkvm_req_ioctl_on_isolate(VirtIONvgpu *nv,
				struct nvkvm_req_ioctl_on_isolate *req,
				struct nvkvm_resp_ioctl_on_isolate *resp,
				void *param_buf, void *aux_buf)
{
	/*
	 * UVM ioctls run in QEMU's process, not the stub.  UVM binds its
	 * file's nvfp to the calling task's mm during UVM_INITIALIZE, and
	 * the matching mmap of /dev/nvidia-uvm must come from the same mm
	 * — which is QEMU (we install a KVM memory region at the resulting
	 * host VA so the guest sees the GPU memory at the right GPA).
	 *
	 * Command-buffer data is already explicitly copied via shm slots,
	 * so the kernel's copy_from_user reading param_buf from QEMU's
	 * address space gives the right bytes regardless of which process
	 * issues the ioctl.
	 *
	 * NOTE on access rights (intentionally NOT enforced here): intra-VM,
	 * per-guest-process access control (which process may touch which
	 * object) is emulated entirely by the guest kernel module — it owns the
	 * guest's pids/uids/namespaces/fds and is the authority.  QEMU must NOT
	 * second-guess it with a session-ownership check: doing so would wrongly
	 * reject a handle that the guest LEGITIMATELY shared into another isolate
	 * via a guest-commanded COPY_HANDLE_TO_ISOLATE (e.g. CUDA IPC), and it
	 * adds no security (malicious guest userspace is blocked by the guest
	 * module; a malicious guest kernel would just forge session_id).  QEMU's
	 * boundary is cross-VM / host-process (the per-VM handle table + hClient
	 * allowlist + no host-wide TYPE_ALL), not intra-VM.
	 */
	{
		struct nvkvm_handle *h =
			nvkvm_handle_get(&nv->handles, req->handle_id);
		if (h && h->dev_id == NVKVM_DEV_UVM && h->fd >= 0) {
			/* Phase 3: schema-gated forwarding.  Look the cmd up in
			 * the UVM allowlist; refuse anything not described. */
			const struct nvkvm_uvm_desc *d = nvkvm_uvm_lookup(req->cmd);
			if (!d) {
				fprintf(stderr,
					"nvkvm: DENY unschemaed UVM ioctl cmd=0x%x "
					"(default-deny)\n", req->cmd);
				resp->retval     = (uint64_t)(int64_t)(-EPERM);
				resp->status     = 0;
				resp->nvstatus   = 0x57; /* NV_ERR_NOT_SUPPORTED */
				resp->fault_addr = 0;
				return 0;
			}
			if (req->param_size < d->min_size ||
			    (d->min_size > 0 && !param_buf)) {
				fprintf(stderr,
					"nvkvm: DENY UVM cmd=0x%x short param_size=%u "
					"(<%u)\n", req->cmd, req->param_size,
					d->min_size);
				resp->retval     = (uint64_t)(int64_t)(-EINVAL);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
			/* Translate each embedded frontend-fd field: the guest
			 * sanitizer rewrote the fd into a handle_id; swap to
			 * QEMU's local fd for the kernel, then restore the
			 * handle_id on response so libcuda sees what it sent. */
			uint32_t saved_val[2];
			int      saved_off[2];
			int      nsaved = 0;
			for (int k = 0; k < 2 && d->fd_off[k] != 0xffff; k++) {
				uint32_t off = d->fd_off[k];
				if (!param_buf || req->param_size < off + 4)
					continue;
				uint32_t hid;
				memcpy(&hid, (char *)param_buf + off, 4);
				if (hid == 0 || hid == (uint32_t)-1)
					continue;
				struct nvkvm_handle *hh =
					nvkvm_handle_get(&nv->handles, hid);
				if (!hh || hh->fd < 0)
					continue;
				saved_val[nsaved] = hid;
				saved_off[nsaved] = (int)off;
				nsaved++;
				uint32_t fd32 = (uint32_t)hh->fd;
				memcpy((char *)param_buf + off, &fd32, 4);
			}
			/* C-2: dup the target fd under the table lock so a
			 * concurrent CLOSE_HANDLE on the TX thread cannot
			 * close()+recycle this fd while we're mid-ioctl on the
			 * pool worker.  The dup keeps the struct file alive for
			 * the whole call; we close it immediately after. */
			int tfd = nvkvm_handle_acquire_fd(&nv->handles,
							  req->handle_id, NULL);
			if (tfd < 0) {
				resp->retval     = (uint64_t)(int64_t)(-EBADF);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
			int r = ioctl(tfd, (unsigned long)req->cmd, param_buf);
			int saved_errno = errno;
			close(tfd);
			for (int k = 0; k < nsaved; k++)
				memcpy((char *)param_buf + saved_off[k],
				       &saved_val[k], 4);
			uint32_t st = 0;
			/* UVM_*_PARAMS conventionally ends with rmStatus (u32).
			 * Read the last 4 bytes of the params struct. */
			if (param_buf && req->param_size >= 4) {
				memcpy(&st, (char *)param_buf + req->param_size - 4,
				       sizeof(st));
			}
			resp->retval     = (r < 0) ? (uint64_t)(int64_t)(-saved_errno) : 0;
			resp->status     = 0;
			resp->nvstatus   = st;
			resp->fault_addr = 0;
			return 0;
		}
	}
	/*
	 * REGISTER_FD now runs inside the isolate (stub) along with every
	 * other RM ioctl: the stub allocated the pClient (NV01_ROOT_CLIENT)
	 * when the gpu fd was opened, and rmclientValidate on the open
	 * driver compares pClient->pOSInfo with the calling task's nvfp.
	 * Running the ioctl from QEMU would fail strict validation. The
	 * stub translates ctl_fd from handle_id → its local fd before the
	 * kernel sees the ioctl (see fe_embedded_fd_off case 0xc9).
	 */

	/* DEBUG: dump structs at the QEMU layer right before forwarding
	 * to the isolate.  Only fd field should differ vs. guest's
	 * post-translate dump.  Same applies to NV01_EVENT_OS_EVENT. */
	if (_IOC_TYPE(req->cmd) == 'F' &&
	    (_IOC_NR(req->cmd) == 0xce || _IOC_NR(req->cmd) == 0xcf) &&
	    param_buf && req->param_size >= 16) {
		const uint8_t *p = param_buf;
		fprintf(stderr,
			"nvkvm qemu pre 0x%x param[16]= "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			_IOC_NR(req->cmd),
			p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
			p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
	}
	if (_IOC_TYPE(req->cmd) == 'F' && _IOC_NR(req->cmd) == 0x2b &&
	    param_buf && aux_buf &&
	    req->param_size >= 16 && req->aux_size >= 24) {
		uint32_t hclass;
		memcpy(&hclass, (char *)param_buf + 12, 4);
		if (hclass == 0x79) {
			const uint8_t *a = aux_buf;
			fprintf(stderr,
				"nvkvm qemu pre 0x79 aux[24]= "
				"%02x %02x %02x %02x  %02x %02x %02x %02x "
				"%02x %02x %02x %02x  %02x %02x %02x %02x "
				"%02x %02x %02x %02x  %02x %02x %02x %02x\n",
				a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
				a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],
				a[16],a[17],a[18],a[19],a[20],a[21],a[22],a[23]);
		}
	}

	/*
	 * Phase 4 gate — DUP_OBJECT cross-VM defense.  NVOS55 (verified 28-byte
	 * layout): h_client@0, h_parent@4, h_object@8, h_client_src@12,
	 * h_src_object@16.  The source client must be one THIS VM allocated;
	 * otherwise a guest (whose objects carry the Path-α TYPE_ALL DUP grant)
	 * could dup another VM's object by naming its (h_client_src,
	 * h_src_object).  h_client itself is the caller's own client (recorded
	 * post-success below), so we only need to vet the src.
	 */
	if (_IOC_TYPE(req->cmd) == 'F' &&
	    _IOC_NR(req->cmd) == NV_ESC_RM_DUP_OBJECT &&
	    param_buf && req->param_size >= 16) {
		uint32_t h_client_src = 0;
		memcpy(&h_client_src, (char *)param_buf + 12, 4);
		if (h_client_src != 0 && h_client_src != (uint32_t)-1 &&
		    !nvkvm_client_allow_has(nv, h_client_src)) {
			fprintf(stderr,
				"nvkvm: DENY DUP_OBJECT foreign h_client_src=0x%x "
				"(not a client of this VM)\n", h_client_src);
			resp->retval     = (uint64_t)(int64_t)(-EACCES);
			resp->status     = 0;
			resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
			resp->fault_addr = 0;
			return 0;
		}
	}

	/*
	 * Audit H-3: make the per-VM hClient allowlist authoritative for EVERY
	 * forwarded RM 'F' ioctl that carries an hClient at param offset 0
	 * (ALLOC/ALLOC_MEMORY/CONTROL/FREE/DUP/SHARE/MAP[_DMA]/UNMAP[_DMA]/
	 * VID_HEAP_CONTROL), not just DUP_OBJECT's source.  A guest must not be
	 * able to make the privileged stub name another VM's RM client.  Every
	 * client this VM creates is recorded at its root-client alloc (above), so
	 * a legitimate reference is always in the set; the only exemption is that
	 * client-creating alloc itself (param[0]=h_root=0, new client in hObjNew).
	 * Layered defense-in-depth on top of eliminating host-wide TYPE_ALL (H-2).
	 */
	if (_IOC_TYPE(req->cmd) == 'F' && param_buf && req->param_size >= 4 &&
	    nv->client_allow_n > 0) {
		unsigned nr = _IOC_NR(req->cmd);
		int hclient_at_0 =
			nr == NV_ESC_RM_ALLOC || nr == NV_ESC_RM_ALLOC_MEMORY ||
			nr == NV_ESC_RM_CONTROL || nr == NV_ESC_RM_FREE ||
			nr == NV_ESC_RM_DUP_OBJECT || nr == 0x35 /* SHARE */ ||
			nr == 0x4e /* MAP_MEMORY */ || nr == 0x4f /* UNMAP_MEMORY */ ||
			nr == 0x57 /* MAP_MEMORY_DMA */ ||
			nr == 0x58 /* UNMAP_MEMORY_DMA */ ||
			nr == 0x4a /* VID_HEAP_CONTROL */;
		if (hclient_at_0) {
			uint32_t hc = 0;
			memcpy(&hc, (char *)param_buf, 4);
			int is_root_alloc = 0;
			if (nr == NV_ESC_RM_ALLOC && req->param_size >= 16) {
				uint32_t hObjNew = 0, hClass = 0;
				memcpy(&hObjNew, (char *)param_buf + 8, 4);
				memcpy(&hClass,  (char *)param_buf + 12, 4);
				is_root_alloc = (hObjNew == hc &&
						 (hClass == 0 || hClass == 0x41)) ||
						(hc == 0 &&
						 (hClass == 0 || hClass == 0x41));
			}
			if (!is_root_alloc && hc != 0 && hc != (uint32_t)-1 &&
			    !nvkvm_client_allow_has(nv, hc)) {
				fprintf(stderr,
					"nvkvm: DENY ioctl NR=0x%x foreign hClient=0x%x "
					"(not a client of this VM)\n", nr, hc);
				resp->retval     = (uint64_t)(int64_t)(-EACCES);
				resp->status     = 0;
				resp->nvstatus   = 0x1f; /* NV_ERR_INVALID_ARGUMENT */
				resp->fault_addr = 0;
				return 0;
			}
		}
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

	/*
	 * Phase 4 — record this VM's RM client handles.  Every successful 'F'
	 * RM ioctl carries the owning hClient at param offset 0; recording it
	 * builds the per-VM allowlist the DUP_OBJECT gate above consults.  (A
	 * client this VM uses successfully is, by definition, this VM's.)
	 */
	if (ret == 0 && nvstatus == 0 &&
	    _IOC_TYPE(req->cmd) == 'F' && param_buf && req->param_size >= 4) {
		/* Only the NVOSxx structs whose first field is hClient: ALLOC,
		 * ALLOC_MEMORY, CONTROL, FREE, DUP_OBJECT, SHARE.  Every RM
		 * client performs at least an ALLOC, so this captures them all
		 * without recording stray words from header-less ioctls. */
		unsigned nr = _IOC_NR(req->cmd);
		if (nr == NV_ESC_RM_ALLOC || nr == NV_ESC_RM_ALLOC_MEMORY ||
		    nr == NV_ESC_RM_CONTROL || nr == NV_ESC_RM_FREE ||
		    nr == NV_ESC_RM_DUP_OBJECT || nr == 0x35 /* RM_SHARE */) {
			uint32_t hc = 0;
			memcpy(&hc, (char *)param_buf, 4);
			nvkvm_client_allow_add(nv, hc);   /* owning client (param[0]) */
			/* A root-client alloc (NV01_ROOT class 0, or 0x41
			 * NV01_ROOT_CLIENT) creates a NEW client whose
			 * kernel-assigned handle is written back to hObjNew
			 * (param[8]); h_root (param[0]) is 0 for that alloc.
			 * Record the new client now so the H-3 gate accepts the
			 * client's very first subsequent ioctl (which references
			 * it at param[0]). */
			if (nr == NV_ESC_RM_ALLOC && req->param_size >= 16) {
				uint32_t hObjNew = 0, hClass = 0;
				memcpy(&hObjNew, (char *)param_buf + 8, 4);
				memcpy(&hClass,  (char *)param_buf + 12, 4);
				if (hClass == 0 || hClass == 0x41)
					nvkvm_client_allow_add(nv, hObjNew);
			}
		}
	}

	/*
	 * Path α — explicit DUP_OBJECT grant for cross-process duplication.
	 *
	 * The kernel's default share policy is `RS_SHARE_TYPE_PID` which
	 * grants DUP_OBJECT only when the calling task's PID matches the
	 * resource owner's ProcID.  In our split-process model (libcuda's
	 * RM client allocated by the stub, UVM ioctls called from QEMU)
	 * the PIDs don't match, so UVM's kernel-internal client can't dup
	 * libcuda's VA space → NV_ERR_INSUFFICIENT_PERMISSIONS.
	 *
	 * Fix: right after the stub successfully allocates a class that we
	 * know UVM will need to dup, issue an NV_ESC_RM_SHARE on the new
	 * handle granting DUP_OBJECT scoped to QEMU's pid (RS_SHARE_TYPE_PID,
	 * see the share initializer below — was TYPE_ALL, host-wide, which let
	 * any host process dup a guessed handle).  The share runs on the stub
	 * fd so the owner check inside _serverShareResource matches (caller
	 * process == resource owner).
	 *
	 * Classes we share: FERMI_VASPACE_A (0x90f1) for now; add others as
	 * we hit further duplications.
	 */
	if (ret == 0 && nvstatus == 0 &&
	    _IOC_TYPE(req->cmd) == 'F' &&
	    (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC ||
	     _IOC_NR(req->cmd) == NV_ESC_RM_ALLOC_MEMORY) &&
	    param_buf && req->param_size >= 16) {
		uint32_t hClient = 0, hObjNew = 0, hClass = 0;
		memcpy(&hClient, (char *)param_buf +  0, sizeof(uint32_t));
		memcpy(&hObjNew, (char *)param_buf +  8, sizeof(uint32_t));
		if (_IOC_NR(req->cmd) == NV_ESC_RM_ALLOC) {
			memcpy(&hClass,  (char *)param_buf + 12, sizeof(uint32_t));
		} else {
			/* RM_ALLOC_MEMORY always allocates NV01_MEMORY_LOCAL_USER */
			hClass = 0x40;
		}

		/* Grant DUP_OBJECT on every successful RM_ALLOC.  UVM duplicates
		 * VA spaces (0x90f1), memory objects (0x40 = NV01_MEMORY_LOCAL_-
		 * USER), channels, and more — granting universally is simpler
		 * than maintaining a class allowlist, and harmless: the share
		 * only adds DUP_OBJECT, which RM-allocated resources have for
		 * their owner anyway.  Skip RM client objects (hObjNew == hClient
		 * AND hClass == NV01_ROOT_CLIENT) since rmapiAllocClient already
		 * REVOKEs DUP from TYPE_ALL on those for security. */
		int is_client_obj = (hObjNew == hClient && hClass == 0x0);
		int needs_share = !is_client_obj && hClass != 0;

		if (needs_share && hClient && hObjNew) {
			/* NVOS57_PARAMETERS layout (24 bytes):
			 *   u32 hClient
			 *   u32 hObject
			 *   u32 sharePolicy.target
			 *   u32 sharePolicy.accessMask (1 limb)
			 *   u16 sharePolicy.type
			 *   u8  sharePolicy.action
			 *   u8  pad
			 *   u32 status
			 * cmd = _IOWR('F', NV_ESC_RM_SHARE=0x35, 24) = 0xc0184635.
			 */
			struct {
				uint32_t hClient;
				uint32_t hObject;
				uint32_t target;
				uint32_t accessMask;
				uint16_t type;
				uint8_t  action;
				uint8_t  _pad;
				uint32_t status;
			} share = {
				.hClient    = hClient,
				.hObject    = hObjNew,
				/* Phase 4 step 3 — STILL TYPE_ALL pending a proper
				 * narrowing.  EXPERIMENT (reverted): TYPE_PID
				 * targeted at QEMU's getpid() broke cuCtxCreate
				 * (=800), proving the dup consumer is NOT QEMU's
				 * task pid but the UVM KERNEL-internal client.  The
				 * correct narrowing is TYPE_CLIENT targeted at that
				 * UVM kernel client's handle, which must first be
				 * discovered (kernel-internal; ~0xc1d00001 but it
				 * drifts) — e.g. via a printk in the open driver's
				 * dup access path.  Until then TYPE_ALL remains the
				 * known host-process exposure; the per-VM hClient
				 * gate (above) still blocks the guest-driven path.
				 * TODO(phase4-step3): TYPE_CLIENT(uvm_kernel_client). */
				.target     = 0,
				.accessMask = 0x1,   /* RS_ACCESS_DUP_OBJECT */
				.type       = 1,     /* RS_SHARE_TYPE_ALL */
				.action     = 0,     /* grant (no REVOKE/REQUIRE/COMPOSE) */
				.status     = 0,
			};
			uint32_t share_nvstatus = 0;
			uint64_t share_fault    = 0;
			int sret = nvkvm_isolate_ioctl(&nv->isolates,
						       req->isolate_id,
						       req->handle_id,
						       0xc0184635u,
						       &share, sizeof(share),
						       NULL, 0,
						       0,
						       &share_nvstatus,
						       &share_fault);
			fprintf(stderr,
				"nvkvm: post-alloc SHARE hClient=0x%x hClass=0x%x "
				"hObj=0x%x ret=%d nvstatus=0x%x status=0x%x\n",
				hClient, hClass, hObjNew, sret, share_nvstatus,
				share.status);
		}
	}

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

/*
 * Sentinel kvm_slot value meaning "this mapping lives inside the single
 * pre-installed 128 GiB sparse window — there is NO per-mmap KVM memslot to
 * remove on teardown; instead the device backing is restored to anonymous
 * pages so the window stays fully mapped for KVM."  Distinct from -1, which
 * means "legacy path, memslot install was attempted but failed/absent."
 */
#define NVKVM_IN_WINDOW_SLOT  (-2)

/*
 * KVM slot allocator is centralised in nvkvm_mmap_host.c via the
 * nvkvm_kvm_slot_alloc/release prototypes in virtio_nvgpu.h, shared with
 * nvkvm_mmap_create().  The stale `iso_kvm_slot_counter` monotonic
 * counter that used to live here — which overlapped with the mmap_host
 * counter's range past 100 and never recycled — has been removed.
 * Audit L5 follow-up.
 */

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

	/*
	 * Audit M-1: this mmap runs in the privileged QEMU process against the
	 * real GPU device fd with a guest-controlled prot/len.  Mask prot to
	 * R/W only (never PROT_EXEC, matching the REALIZE path's
	 * NVKVM_REALIZE_PROT_MASK) and reject absurd lengths before we touch the
	 * device fd or the sparse window.  (Window allocation also rejects
	 * oversize, but bound here so we never hand a wild len to mmap().)
	 */
	req->prot &= (uint32_t)(PROT_READ | PROT_WRITE);
	if (len == 0 || len > nv->sparse_size) {
		resp->status = EINVAL;
		return 0;
	}

	/*
	 * Place the mapping inside the single pre-installed 128 GiB sparse
	 * window instead of allocating a fresh KVM memslot per mmap.  A single
	 * cuCtxCreate issues >1500 tiny (4 KB) device mmaps; one memslot each
	 * blows past both our pool and any sane slot count.  By MAP_FIXED'ing
	 * the device fd into the sparse window's VA range we reuse the one
	 * memslot nvkvm_sparse_init() already installed — zero per-mmap KVM
	 * ioctls.  Sparse/holey device regions inside one big prereserved KVM
	 * memory region are fully supported by KVM (per-page gup on fault).
	 *
	 * /dev/nvidia-uvm is the exception: its kernel mmap handler requires
	 *   vm_start == (vm_pgoff << PAGE_SHIFT)
	 * so QEMU must map it MAP_FIXED at req->offset, not at an arbitrary
	 * window VA.  UVM mappings are few, so the legacy per-mmap memslot is
	 * acceptable for them.
	 */
	void    *qva       = MAP_FAILED;
	uint64_t gpa       = 0;
	int      kvm_slot  = -1;
	bool     in_window = (h->dev_id != NVKVM_DEV_UVM);

	if (in_window) {
		gpa = nvkvm_sparse_gpa_alloc(nv, len);
		void *target = gpa ? nvkvm_gpa_to_vmm_va(nv, gpa, len) : NULL;
		if (!target) {
			fprintf(stderr,
				"nvkvm: mmap_on_isolate: sparse window full "
				"(handle=%u len=%lu)\n",
				req->handle_id, (unsigned long)len);
			resp->status = ENOMEM;
			return 0;
		}
		qva = mmap(target, len, req->prot,
			   MAP_SHARED | MAP_FIXED, h->fd, (off_t)req->offset);
		if (qva == MAP_FAILED) {
			int se = errno;
			fprintf(stderr,
				"nvkvm: mmap_on_isolate(window) FAIL fd=%d "
				"dev_id=%d prot=0x%x len=%lu off=0x%lx "
				"gpa=0x%llx errno=%d (%s)\n",
				h->fd, h->dev_id, req->prot, (unsigned long)len,
				(unsigned long)req->offset,
				(unsigned long long)gpa, se, strerror(se));
			/* Restore the anonymous backing we just clobbered so the
			 * window stays fully mapped for KVM. */
			mmap(target, len, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE |
			     MAP_FIXED, -1, 0);
			resp->status = (uint32_t)se;
			return 0;
		}
		/* No KVM ioctl: the sparse window's single memslot already maps
		 * [gpa, gpa+len) → this VA range. */
		kvm_slot = NVKVM_IN_WINDOW_SLOT;
	} else {
		/*
		 * /dev/nvidia-uvm.  The old approach mmap'd the UVM fd MAP_FIXED at
		 * req->offset in QEMU's address space and installed a per-mmap
		 * memslot.  That collides across concurrent processes: libcuda
		 * picks the same UVM VA in every process, and QEMU's single address
		 * space can only hold one mapping there, so the second process hits
		 * MAP_FIXED_NOREPLACE → EEXIST → cuCtxCreate fails (304).  (UVM also
		 * cannot be MAP_FIXED into the sparse window: its kernel mmap
		 * requires vm_start == (vm_pgoff<<PAGE_SHIFT).)
		 *
		 * Instead allocate the GPA from the sparse window and let it ride
		 * the window's anonymous backing — no QEMU-side device mmap, no
		 * per-mmap memslot, no cross-process VA collision (same model as the
		 * realize path).  The stub owns the real UVM mapping in its own
		 * per-process address space; the GPU reaches the memory via DMA, not
		 * a QEMU CPU memslot.
		 */
		gpa = nvkvm_sparse_gpa_alloc(nv, len);
		void *target = gpa ? nvkvm_gpa_to_vmm_va(nv, gpa, len) : NULL;
		if (!target) {
			fprintf(stderr,
				"nvkvm: mmap_on_isolate(uvm): sparse window full "
				"(handle=%u len=%lu)\n",
				req->handle_id, (unsigned long)len);
			resp->status = ENOMEM;
			return 0;
		}
		qva      = target;             /* sparse-window VA (anon-backed) */
		kvm_slot = NVKVM_IN_WINDOW_SLOT;
	}

	/* Step 3: optionally mirror the mapping into the isolate's mm.
	 *
	 * The isolate-side mmap is only useful for the case where an NVIDIA
	 * ioctl dereferences a user VA pointing into this region while
	 * executing in the stub's process context.  All command-buffer
	 * traffic (NVOS54 params, alloc structs, etc) is already explicitly
	 * copied via shm slots, and the GPU itself reaches the memory via
	 * the KVM-installed GPA↔hostVA mapping — not through the stub's mm.
	 *
	 * For /dev/nvidia-uvm this mmap actively fails (EBADFD): the stub's
	 * UVM fd is per-process state in the kernel and may not be in the
	 * right uvm_fd_type when libcuda issues the mmap.  Skipping the
	 * mirror unblocks cuCtxCreate; if we ever discover an ioctl that
	 * does require the stub mm to back the VA, we'll mirror it then.
	 */
	int ret = 0;
	struct nvkvm_handle *hd = nvkvm_handle_get(&nv->handles, req->handle_id);
	int do_stub_mirror = !hd || hd->dev_id != 1 /* NVKVM_DEV_UVM */;
	if (do_stub_mirror) {
		ret = nvkvm_isolate_mmap(&nv->isolates,
					 req->isolate_id,
					 req->handle_id,
					 req->gva, len, req->offset,
					 (int)req->prot,
					 (int)req->map_flags);
	}

	if (ret < 0) {
		if (kvm_slot == NVKVM_IN_WINDOW_SLOT) {
			/* Restore anon backing inside the window (no munmap — that
			 * would punch a hole in the sparse VMA). */
			mmap(qva, len, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE |
			     MAP_FIXED, -1, 0);
		} else {
			if (kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
				struct nvkvm_kvm_mem_region mr = {
					.slot = (uint32_t)kvm_slot,
					.memory_size = 0 };
				ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
				nvkvm_kvm_slot_release(kvm_slot);
			}
			munmap(qva, len);
		}
		resp->status = (uint32_t)-ret;
		return 0;
	}

	/* Record for future MUNMAP_ON_ISOLATE */
	uint32_t token = iso_mmap_alloc(req->isolate_id, req->gva, qva,
					len, kvm_slot, gpa,
					do_stub_mirror);
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

	/* Tell the isolate to unmap the GVA range, but only if we mirrored
	 * the mapping there in the first place. */
	if (e.stub_mirrored)
		nvkvm_isolate_munmap(&nv->isolates, e.isolate_id, e.gva,
				     (uint64_t)e.len);

	if (e.kvm_slot == NVKVM_IN_WINDOW_SLOT) {
		/* In-window mapping: restore anonymous backing so the sparse
		 * window stays fully mapped (the single memslot covers it).
		 * Do NOT munmap — that would punch a hole in the sparse VMA. */
		if (e.qva)
			mmap(e.qva, e.len, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE |
			     MAP_FIXED, -1, 0);
	} else {
		/* Legacy path: remove the per-mmap KVM memslot, return it to the
		 * pool, and unmap the standalone QEMU host VA. */
		if (e.kvm_slot >= 0 && nvkvm_kvm_vm_fd >= 0) {
			struct nvkvm_kvm_mem_region mr = {
				.slot        = (uint32_t)e.kvm_slot,
				.memory_size = 0,
			};
			ioctl(nvkvm_kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &mr);
			nvkvm_kvm_slot_release(e.kvm_slot);
		}
		if (e.qva)
			munmap(e.qva, e.len);
	}

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

/* ── READ_HOST_FILE ──────────────────────────────────────────────────────────
 *
 * Live read of a host-side proc/sys file the guest doesn't have because
 * the real nvidia.ko isn't loaded in the VM.  File selection is by enum;
 * QEMU never trusts a guest-supplied path.
 *
 * The path table is the security boundary.  Files are read fresh on every
 * call so callers see live state.
 */
static const char *nvkvm_hfile_path(uint32_t id)
{
	switch (id) {
	case NVKVM_HFILE_NVIDIA_PARAMS:
		return "/proc/driver/nvidia/params";
	case NVKVM_HFILE_NVIDIA_INITSTATE:
		return "/sys/module/nvidia/initstate";
	case NVKVM_HFILE_NVIDIA_UVM_INITSTATE:
		return "/sys/module/nvidia_uvm/initstate";
	case NVKVM_HFILE_NVIDIA_NUMA_STATUS:
		return "/proc/driver/nvidia/gpus/0000:00:07.0/numa_status";
	case NVKVM_HFILE_NVIDIA_INFORMATION:
		return "/proc/driver/nvidia/gpus/0000:00:07.0/information";
	case NVKVM_HFILE_NVIDIA_REG_BASE:
		return "/proc/driver/nvidia/gpus/0000:00:07.0/registry";
	default:
		return NULL;
	}
}

int nvkvm_req_read_host_file(VirtIONvgpu *nv,
			      struct nvkvm_req_read_host_file *req,
			      struct nvkvm_resp_read_host_file *resp,
			      void *shm_buf)
{
	(void)nv;
	memset(resp, 0, sizeof(*resp));

	if (!shm_buf || req->max_len == 0 ||
	    req->max_len > NVKVM_HFILE_MAX_SIZE) {
		resp->status = EINVAL;
		return 0;
	}

	const char *path = nvkvm_hfile_path(req->file_id);
	if (!path) {
		resp->status = EINVAL;
		return 0;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		resp->status = (uint32_t)errno;
		return 0;
	}

	uint32_t total = 0;
	while (total < req->max_len) {
		ssize_t n = read(fd, (char *)shm_buf + total,
				 (size_t)(req->max_len - total));
		if (n < 0) {
			if (errno == EINTR) continue;
			resp->status = (uint32_t)errno;
			close(fd);
			return 0;
		}
		if (n == 0) break;
		total += (uint32_t)n;
	}
	close(fd);

	resp->status = 0;
	resp->nbytes = total;
	return 0;
}

/* ── REALIZE_UVM_MAPPING ─────────────────────────────────────────────────────
 *
 * STATE_MACHINE_PLAN §8a — strict validation.  This handler runs in QEMU
 * (privileged) on behalf of a guest that we treat as adversarial.
 *
 * Threat model: any field can be attacker-controlled.  We must:
 *   1. Bound every count / size against caps defined in nvkvm_proto.h.
 *   2. Sanitize flags — strip everything outside the allowlist.
 *   3. Sanitize prot — strip everything outside R/W (no exec on GPU mmaps).
 *   4. For mode SEM_POOL: validate intent_size == sizeof(SEM_POOL_PARAMS).
 *   5. Validate the intent struct's base/length match req.length so a
 *      malicious guest can't trick the kernel into mapping the wrong VA.
 *   6. Allocate a fresh KVM GPA window — never trust offsets.
 *   7. Forward exact validated state+intent to the stub.
 */
struct nvkvm_kvm_mem_region_rl {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#define NVKVM_KVMIO_RL  0xAE
#define KVM_SET_USER_MEMORY_REGION_RL \
	_IOW(NVKVM_KVMIO_RL, 0x46, struct nvkvm_kvm_mem_region_rl)

#define NVKVM_REALIZE_PROT_MASK    (PROT_READ | PROT_WRITE)
#define NVKVM_REALIZE_FLAGS_MASK   (MAP_SHARED | MAP_PRIVATE)
/* Cap intent blob: SEM_POOL is 9248 bytes; allow a small margin. */
#define NVKVM_REALIZE_INTENT_MAX   (64u * 1024u)

/* realize_kvm_slot_counter superseded by nvkvm_kvm_slot_alloc().  Audit L5. */

int nvkvm_req_realize_uvm_mapping(VirtIONvgpu *nv,
				   struct nvkvm_req_realize_uvm_mapping *req,
				   struct nvkvm_resp_realize_uvm_mapping *resp,
				   void *state_buf, void *intent_buf)
{
	memset(resp, 0, sizeof(*resp));

	/* §8a.1 — pointer presence. */
	if (!state_buf || !intent_buf) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.1 — state size is a fixed cap-bound struct. */
	const size_t state_size =
		sizeof(struct nvkvm_uvm_state_snapshot);

	/* §8a.4 — intent size bound + mode-specific exact match. */
	if (req->intent_size == 0 ||
	    req->intent_size > NVKVM_REALIZE_INTENT_MAX) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.5 — validate per-mode intent shape. */
	struct nvkvm_uvm_state_snapshot *snap = state_buf;
	if (snap->n_gpus > NVKVM_UVM_MAX_REG_GPUS ||
	    snap->n_va_spaces > NVKVM_UVM_MAX_VA_SPACES ||
	    snap->n_range_groups > NVKVM_UVM_MAX_RANGE_GROUPS) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	switch (req->mode) {
	case NVKVM_UVM_REALIZE_MODE_SEM_POOL: {
		if (req->intent_size !=
		    sizeof(struct uvm_alloc_semaphore_pool_params)) {
			resp->status = (uint32_t)-EINVAL;
			return 0;
		}
		struct uvm_alloc_semaphore_pool_params *p = intent_buf;
		if (p->length != req->length || p->base != req->gva) {
			resp->status = (uint32_t)-EINVAL;
			return 0;
		}
		p->rm_status = 0;
		break;
	}
	default:
		resp->status = (uint32_t)-ENOTSUP;
		return 0;
	}

	/* §8a.2/3 — sanitize prot+flags.  Strip anything outside allowlist. */
	uint32_t prot      = req->prot      & (uint32_t)NVKVM_REALIZE_PROT_MASK;
	uint32_t map_flags = req->map_flags & (uint32_t)NVKVM_REALIZE_FLAGS_MASK;
	if (prot == 0)
		prot = PROT_READ | PROT_WRITE;
	if ((map_flags & (MAP_SHARED | MAP_PRIVATE)) == 0)
		map_flags |= MAP_SHARED;

	/* Length must be page-aligned and within sane bounds. */
	uint64_t len = req->length;
	if (len == 0 || len > (1ULL << 40) || (len & 4095ULL)) {
		resp->status = (uint32_t)-EINVAL;
		return 0;
	}

	/* §8a.6 — allocate a fresh GPA from the single sparse window so the
	 * guest (which validates every returned GPA against that window)
	 * accepts it.  No per-mmap memslot is installed for realize — see the
	 * note below; the GPA rides the sparse window's pre-installed memslot
	 * (anonymous backing), matching the proven v0.1 behaviour. */
	uint64_t gpa = nvkvm_sparse_gpa_alloc(nv, (size_t)len);
	if (gpa == 0) {
		resp->status = (uint32_t)-ENOMEM;
		return 0;
	}

	/* §8a.7 — send to stub.  Stub does the actual /dev/nvidia-uvm work
	 * inside the isolate's mm. */
	uint64_t host_va = 0, out_len = 0, token = 0;
	uint32_t rm_status = 0;
	int ret = nvkvm_isolate_realize_uvm_fd(&nv->isolates,
					       req->isolate_id,
					       req->mode,
					       state_buf, (uint32_t)state_size,
					       intent_buf, req->intent_size,
					       prot, map_flags,
					       len, /*host_va_hint=*/0,
					       /*offset=*/0,
					       &host_va, &out_len,
					       &token, &rm_status);
	if (ret < 0 || host_va == 0) {
		resp->status    = (uint32_t)-ret;
		resp->rm_status = rm_status;
		return 0;
	}
	if (rm_status != 0) {
		/* Kernel rejected the intent — host_va may still be set if the
		 * mmap succeeded but a later step failed.  Treat as failure. */
		resp->rm_status = rm_status;
		resp->status    = (uint32_t)-EIO;
		return 0;
	}

	/* §8a.6 — NO per-mmap KVM memslot: host_va is a stub-process VA,
	 * invalid as a QEMU KVM userspace_addr.  See security-fixes commit.
	 * Master masked this via slot=1100 > KVM cap (install failed). */
	(void)host_va;

	resp->gpa_base      = gpa;
	resp->length        = len;
	resp->realize_token = token;
	resp->rm_status     = 0;
	resp->status        = 0;
	return 0;
}
