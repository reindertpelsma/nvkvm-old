/*
 * nvkvm_stub.c — nvkvm isolate stub process (multi-threaded)
 *
 * Minimal static binary: no libc, Linux syscalls via syscall(3) (from
 * sys/syscall.h only — no libc linked), seccomp allowlist, SIGSEGV handler.
 *
 * Threading model
 * ===============
 * One reader thread reads framed commands from the QEMU socket (fd 0).
 * Non-IOCTL commands (RECEIVE_FD, CLOSE_FD, MMAP, MUNMAP, POLL, UNPOLL, EXIT)
 * are dispatched inline on the reader thread — they are fast and non-blocking.
 * IOCTL commands are queued to a pool of NVKVM_STUB_WORKERS worker threads.
 * Each worker executes the ioctl and writes the response back with the echoed
 * req_id so QEMU can match it to the waiting caller.
 *
 * All socket writes are protected by write_mutex.
 * The fd_table is protected by fd_mutex (workers and the reader share it).
 *
 * Self-relocation
 * ===============
 * Applies R_X86_64_RELATIVE entries from the ELF dynamic section before any
 * code that touches global data runs (constructor priority 101).
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <elf.h>
#include <stdio.h>

#include "../common/nvkvm_isolate_proto.h"

/*
 * KVM ioctl number for KVM_SET_USER_MEMORY_REGION on x86_64.
 * From <linux/kvm.h>:
 *   _IOW(KVMIO=0xAE, 0x46, struct kvm_userspace_memory_region)
 *   struct is 32 bytes (slot+flags+gpa+size+userspace_addr).
 */
#define NVKVM_KVM_SET_USER_MEMORY_REGION  0x4020ae46UL

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER  (1UL << 3)
#endif

/* ── Syscall wrappers ────────────────────────────────────────────────────── */

static __attribute__((noreturn)) void stub_exit(int code)
{
	syscall(SYS_exit_group, code);
	__builtin_unreachable();
}

static long stub_read(int fd, void *buf, size_t n)
{
	return syscall(SYS_read, fd, buf, n);
}

static long stub_write(int fd, const void *buf, size_t n)
{
	return syscall(SYS_write, fd, buf, n);
}

static long stub_recvmsg(int fd, struct msghdr *m, int fl)
{
	return syscall(SYS_recvmsg, fd, m, fl);
}

static long stub_ioctl(int fd, unsigned long req, void *arg)
{
	return syscall(SYS_ioctl, fd, req, arg);
}

static void *stub_mmap(void *a, size_t l, int p, int f, int fd, off_t o)
{
	return (void *)syscall(SYS_mmap, a, l, p, f, fd, o);
}

static long stub_munmap(void *a, size_t l)
{
	return syscall(SYS_munmap, a, l);
}

static long stub_close(int fd) { return syscall(SYS_close, fd); }

static long stub_prctl(int op, unsigned long a2, unsigned long a3,
		       unsigned long a4, unsigned long a5)
{
	return syscall(SYS_prctl, op, a2, a3, a4, a5);
}

static long stub_seccomp(unsigned int op, unsigned int fl, const void *arg)
{
	return syscall(SYS_seccomp, op, fl, arg);
}

static long stub_sigaction(int sig, const struct sigaction *act,
			   struct sigaction *oact)
{
	return syscall(SYS_rt_sigaction, sig, act, oact, sizeof(sigset_t));
}

/* ── Constants ────────────────────────────────────────────────────────────── */

#define SOCK_FD          STDIN_FILENO
#define NVKVM_STUB_WORKERS   16
#define MAX_HANDLES      4096
#define MAX_PARAM_SIZE   (256 * 1024)
#define MAX_INFLIGHT     64   /* max concurrent IOCTL jobs */

/* ── Mutex helpers (use pthread, which is available statically) ──────────── */

static pthread_mutex_t write_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fd_mutex     = PTHREAD_MUTEX_INITIALIZER;

/* ── Globals for install_isolate_mapping ─────────────────────────────────── */

/* The KVM VM fd that QEMU SCM_RIGHTS'd at spawn. The stub uses this for
 * KVM_SET_USER_MEMORY_REGION; seccomp filter is configured to USER_NOTIF
 * exactly that fd+ioctl combination so QEMU's supervisor revalidates
 * before the kernel commits the call. */
static int g_kvm_fd      = -1;

/* /proc/self/maps held open before seccomp so the stub can answer
 * provenance questions later without needing openat. */
static int g_proc_maps_fd = -1;

/* ── Handle fd table ─────────────────────────────────────────────────────── */

static int handle_fds[MAX_HANDLES];

static void handle_table_init(void)
{
	for (int i = 0; i < MAX_HANDLES; i++)
		handle_fds[i] = -1;
}

static int handle_lookup(uint32_t id)
{
	if (id >= MAX_HANDLES) return -1;
	return handle_fds[id];
}

static void handle_store(uint32_t id, int fd)
{
	if (id < MAX_HANDLES) handle_fds[id] = fd;
}

static void handle_remove(uint32_t id)
{
	if (id < MAX_HANDLES) {
		if (handle_fds[id] >= 0) stub_close(handle_fds[id]);
		handle_fds[id] = -1;
	}
}

/* ── Socket I/O ───────────────────────────────────────────────────────────── */

static int send_full(const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		long n = stub_write(SOCK_FD, p, len);
		if (n <= 0) return -1;
		p += n; len -= (size_t)n;
	}
	return 0;
}

static int recv_full(void *buf, size_t len)
{
	char *p = buf;
	while (len > 0) {
		long n = stub_read(SOCK_FD, p, len);
		if (n <= 0) return -1;
		p += n; len -= (size_t)n;
	}
	return 0;
}

static int locked_send(const void *buf, size_t len)
{
	pthread_mutex_lock(&write_mutex);
	int r = send_full(buf, len);
	pthread_mutex_unlock(&write_mutex);
	return r;
}

static int send_ok(void)
{
	struct isolate_resp_ok r = { .type = ISOLATE_RESP_OK };
	return locked_send(&r, sizeof(r));
}

static int send_error(int err)
{
	struct isolate_resp_error r = {
		.type = ISOLATE_RESP_ERROR, .err = err < 0 ? -err : err };
	return locked_send(&r, sizeof(r));
}

/* ── SIGSEGV handler ──────────────────────────────────────────────────────── */

/* Per-thread fault address — pthread TLS key */
static pthread_key_t  fault_addr_key;
static pthread_once_t fault_key_once = PTHREAD_ONCE_INIT;

static void init_fault_key(void)
{
	pthread_key_create(&fault_addr_key, NULL);
}

static void sigsegv_handler(int sig, siginfo_t *info, void *ctx)
{
	(void)sig; (void)ctx;
	pthread_once(&fault_key_once, init_fault_key);
	/* Store fault address in TLS so each worker thread has its own */
	pthread_setspecific(fault_addr_key,
			    (void *)(uintptr_t)info->si_addr);
}

static uint64_t get_fault_addr(void)
{
	return (uint64_t)(uintptr_t)pthread_getspecific(fault_addr_key);
}

static void clear_fault_addr(void)
{
	pthread_setspecific(fault_addr_key, NULL);
}

/* ── Thread pool ──────────────────────────────────────────────────────────── */

struct ioctl_job {
	uint32_t req_id;
	uint32_t handle_id;
	uint32_t cmd;
	uint32_t flags;
	/* param and aux blobs are malloc'd; worker frees them */
	void    *param_buf;
	uint32_t param_size;
	void    *aux_buf;
	uint32_t aux_size;
	int      valid;   /* 1 = slot occupied */
};

static void *blob_alloc(size_t size);

static struct ioctl_job     job_queue[MAX_INFLIGHT];
static pthread_mutex_t      queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t       queue_cond  = PTHREAD_COND_INITIALIZER;
static volatile int         stub_exiting = 0;

static void job_queue_init(void)
{
	for (int i = 0; i < MAX_INFLIGHT; i++)
		job_queue[i].valid = 0;
}

static void enqueue_job(const struct ioctl_job *job)
{
	pthread_mutex_lock(&queue_mutex);
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (!job_queue[i].valid) {
			job_queue[i] = *job;
			job_queue[i].valid = 1;
			pthread_cond_signal(&queue_cond);
			break;
		}
	}
	pthread_mutex_unlock(&queue_mutex);
}

static int dequeue_job(struct ioctl_job *out)
{
	pthread_mutex_lock(&queue_mutex);
	while (!stub_exiting) {
		for (int i = 0; i < MAX_INFLIGHT; i++) {
			if (job_queue[i].valid) {
				*out = job_queue[i];
				job_queue[i].valid = 0;
				pthread_mutex_unlock(&queue_mutex);
				return 1;
			}
		}
		pthread_cond_wait(&queue_cond, &queue_mutex);
	}
	pthread_mutex_unlock(&queue_mutex);
	return 0;
}

static void *worker_thread(void *arg)
{
	(void)arg;
	struct ioctl_job job;

	while (dequeue_job(&job)) {
		pthread_mutex_lock(&fd_mutex);
		int fd = handle_lookup(job.handle_id);
		pthread_mutex_unlock(&fd_mutex);

		struct isolate_resp_ioctl resp = {
			.type     = ISOLATE_RESP_IOCTL,
			.req_id   = job.req_id,
		};

		if (fd < 0) {
			resp.retval = -EBADF;
			goto send_resp;
		}

		/*
		 * Wire aux buffer pointer into param blob.
		 *
		 * RM_CONTROL (nvos54, 32 bytes) and RM_ALLOC (nvos21 32 bytes,
		 * nvos64 48 bytes) both have their embedded pointer field at
		 * offset 16 (after four 4-byte handles/integers).  The guest
		 * zeroes this field and puts the secondary buffer in the aux
		 * slot; we restore the host-accessible address here so the
		 * driver can dereference it.
		 */
		if (job.aux_size > 0 && job.param_size >= 24) {
			uint64_t aux_ptr = (uint64_t)(uintptr_t)job.aux_buf;
			__builtin_memcpy((char *)job.param_buf + 16, &aux_ptr,
					 sizeof(uint64_t));
		}

		/*
		 * GET_BUILD_VERSION (inner cmd=0x101) has three embedded string
		 * pointer fields (p_driver_version_buffer, p_version_buffer,
		 * p_title_buffer) at aux_buf offsets 8, 16, 24.  The guest fills
		 * these with guest-VA buffers that are not valid in the stub's
		 * address space; the host driver calls copy_to_user with them,
		 * fails silently, and leaves changelist_number=0, which causes
		 * cuInit to return CUDA_ERROR_SYSTEM_NOT_READY.
		 *
		 * Fix: allocate stub-local string buffers and redirect the
		 * pointer fields.  After the ioctl, zero them before sending the
		 * aux_buf back so the guest cannot see stub VAs.
		 */
		uint32_t str_sz = 0;
		uint32_t info_list_size = 0; /* if non-zero, info_list pointer must be re-zeroed after the ioctl */
		uint32_t info_list_base = 0; /* base offset of info_list area in aux_buf */
		if ((job.cmd & 0xff) == 0x2a &&        /* NV_ESC_RM_CONTROL */
		    job.aux_size > 0 && job.param_size >= 12) {
			uint32_t inner_cmd;
			__builtin_memcpy(&inner_cmd,
					 (char *)job.param_buf + 8,
					 sizeof(uint32_t));
			/* InfoList family (NV2080_CTRL_CMD_GR_GET_INFO etc.):
			 * aux_buf layout is [base_params][list_size*8 bytes of list].
			 * The base params has list_size at offset 0 and the info_list
			 * pointer at offset 8 (currently zero — guest cleared it). We
			 * point info_list at the extension area so the host driver
			 * writes into our own memory. After the ioctl we zero the
			 * pointer again so we don't leak a host VA back to the guest. */
			if (inner_cmd == 0x00410110U || /* NV0041_CTRL_CMD_GET_SURFACE_INFO */
			    inner_cmd == 0x00801104U || /* NV0080_CTRL_CMD_GR_GET_INFO */
			    inner_cmd == 0x20800802U || /* NV2080_CTRL_CMD_BIOS_GET_INFO */
			    inner_cmd == 0x20801201U || /* NV2080_CTRL_CMD_GR_GET_INFO */
			    inner_cmd == 0x20801301U || /* NV2080_CTRL_CMD_FB_GET_INFO */
			    inner_cmd == 0x20801802U) { /* NV2080_CTRL_CMD_BUS_GET_INFO */
				uint32_t ls = 0;
				__builtin_memcpy(&ls, job.aux_buf, sizeof(uint32_t));
				/* base_size is whatever the guest sent before the
				 * extension; we recover it as aux_size - ls*8. */
				if (ls > 0 && (size_t)ls * 8 < job.aux_size) {
					uint32_t base = (uint32_t)(job.aux_size - (size_t)ls * 8);
					if (base >= 16) {
						uint64_t list_va =
							(uint64_t)(uintptr_t)
							((char *)job.aux_buf + base);
						__builtin_memcpy((char *)job.aux_buf + 8,
								 &list_va, 8);
						info_list_size = ls;
						info_list_base = base;
					}
				}
			}
			if (inner_cmd == 0x00000101U) {    /* GET_BUILD_VERSION */
				/*
				 * nv0000_ctrl_system_get_build_version_params is
				 * 40 bytes; the guest extends aux_buf to
				 * 40 + 3*sz so the host driver can write strings
				 * into the extension area without accessing guest VAs.
				 * Point the embedded string pointer fields at the
				 * extension region already present in job.aux_buf.
				 */
				uint32_t sz = 0;
				if (job.aux_size >= 4)
					__builtin_memcpy(&sz, job.aux_buf, sizeof(uint32_t));
				if (sz > 0 && sz <= 512 &&
				    job.aux_size >= 40 + (size_t)sz * 3) {
					str_sz = sz; /* flag: zero pointers after ioctl */
					uint64_t p1, p2, p3;
					p1 = (uint64_t)(uintptr_t)((char *)job.aux_buf + 40);
					p2 = p1 + sz;
					p3 = p1 + 2 * sz;
					__builtin_memcpy((char *)job.aux_buf +  8, &p1, 8);
					__builtin_memcpy((char *)job.aux_buf + 16, &p2, 8);
					__builtin_memcpy((char *)job.aux_buf + 24, &p3, 8);
				}
			}
		}

		/* NV_ESC_CARD_INFO: log how many valid entries the driver returned */
		int is_card_info = ((job.cmd & 0xff) == 0xc8 &&
				    job.param_size > 0 && job.aux_size == 0);

		clear_fault_addr();
		long ret  = stub_ioctl(fd, job.cmd, job.param_buf);
		int  err  = (ret < 0) ? errno : 0;

		if (is_card_info) {
			int n = 0;
			size_t entry_sz = 80; /* sizeof(nv_ioctl_card_info) on x86-64 */
			size_t count = job.param_size / entry_sz;
			for (size_t i = 0; i < count && i < 32; i++) {
				uint8_t valid = *((uint8_t *)job.param_buf + i * entry_sz);
				if (valid) n++;
			}
			dprintf(2, "nvkvm_stub: CARD_INFO ret=%ld err=%d param_size=%u valid_entries=%d\n",
				ret, err, job.param_size, n);
		}

		/* Zero the embedded pointer field in nvos54 (don't leak host VA) */
		if (job.aux_size > 0 && job.param_size >= 24) {
			uint64_t zero = 0;
			__builtin_memcpy((char *)job.param_buf + 16, &zero,
					 sizeof(uint64_t));
		}

		/* Zero GET_BUILD_VERSION embedded string pointer fields (host VAs) */
		if (str_sz > 0) {
			uint64_t z = 0;
			__builtin_memcpy((char *)job.aux_buf +  8, &z, 8);
			__builtin_memcpy((char *)job.aux_buf + 16, &z, 8);
			__builtin_memcpy((char *)job.aux_buf + 24, &z, 8);
		}

		/* Zero the InfoList pointer we set above (don't leak host VA). The
		 * driver-written list contents in [info_list_base ..] are preserved
		 * — the guest module copies them out to the original user buffer. */
		if (info_list_size > 0) {
			uint64_t z = 0;
			__builtin_memcpy((char *)job.aux_buf + 8, &z, 8);
		}
		(void)info_list_base;

		/*
		 * Extract NvStatus from the response struct.
		 *   nvos54 (RM_CONTROL, 32 bytes): status at offset 28
		 *   nvos21 (RM_ALLOC,  32 bytes): status at offset 28
		 *   nvos64 (RM_ALLOC,  48 bytes): status at offset 40
		 *     (after hRoot/parent/new/class, pAllocParms, pRightsRequested,
		 *      paramsSize, flags — then status; see gVisor frontend.go).
		 */
		uint32_t nvstatus = 0;
		if (job.param_size == 48)
			__builtin_memcpy(&nvstatus,
					 (char *)job.param_buf + 40,
					 sizeof(uint32_t));
		else if (job.param_size >= 32)
			__builtin_memcpy(&nvstatus,
					 (char *)job.param_buf + 28,
					 sizeof(uint32_t));

		resp.retval     = err ? -err : (int32_t)ret;
		resp.nvstatus   = nvstatus;
		resp.fault_addr = get_fault_addr();
		resp.param_size = job.param_size;
		resp.aux_size   = job.aux_size;

	send_resp:
		pthread_mutex_lock(&write_mutex);
		send_full(&resp, sizeof(resp));
		if (resp.param_size > 0 && job.param_buf)
			send_full(job.param_buf, resp.param_size);
		if (resp.aux_size > 0 && job.aux_buf)
			send_full(job.aux_buf, resp.aux_size);
		pthread_mutex_unlock(&write_mutex);

		/* Use stub_munmap to free blobs (allocated from anonymous mmap) */
		if (job.param_buf)
			stub_munmap(job.param_buf,
				    (job.param_size + 4095) & ~4095UL);
		if (job.aux_buf)
			stub_munmap(job.aux_buf,
				    (job.aux_size + 4095) & ~4095UL);
	}
	return NULL;
}

/* Allocate a blob buffer via anonymous mmap (no heap/libc needed). */
static void *blob_alloc(size_t size)
{
	if (!size) return NULL;
	size_t aligned = (size + 4095) & ~4095UL;
	void *p = stub_mmap(NULL, aligned, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return (p == MAP_FAILED) ? NULL : p;
}

/* ── Command handlers (reader thread) ───────────────────────────────────── */

static void handle_close_fd(uint32_t handle_id)
{
	pthread_mutex_lock(&fd_mutex);
	handle_remove(handle_id);
	pthread_mutex_unlock(&fd_mutex);
	send_ok();
}

static void handle_ioctl_cmd(struct isolate_cmd_ioctl *cmd)
{
	if (cmd->param_size > MAX_PARAM_SIZE || cmd->aux_size > MAX_PARAM_SIZE) {
		/* Can't send error with req_id here in the old format;
		 * send a minimal error response. */
		struct isolate_resp_ioctl resp = {
			.type   = ISOLATE_RESP_IOCTL,
			.req_id = cmd->req_id,
			.retval = -EINVAL,
		};
		locked_send(&resp, sizeof(resp));
		/* drain the data that was supposed to follow */
		return;
	}

	struct ioctl_job job = {
		.req_id     = cmd->req_id,
		.handle_id  = cmd->handle_id,
		.cmd        = cmd->cmd,
		.flags      = cmd->flags,
		.param_size = cmd->param_size,
		.aux_size   = cmd->aux_size,
	};

	/* Read param+aux blobs into per-job buffers */
	if (cmd->param_size > 0) {
		job.param_buf = blob_alloc(cmd->param_size);
		if (!job.param_buf || recv_full(job.param_buf, cmd->param_size) < 0) {
			stub_munmap(job.param_buf,
				    (cmd->param_size + 4095) & ~4095UL);
			struct isolate_resp_ioctl resp = {
				.type = ISOLATE_RESP_IOCTL, .req_id = cmd->req_id,
				.retval = -ENOMEM };
			locked_send(&resp, sizeof(resp));
			return;
		}
	}
	if (cmd->aux_size > 0) {
		job.aux_buf = blob_alloc(cmd->aux_size);
		if (!job.aux_buf || recv_full(job.aux_buf, cmd->aux_size) < 0) {
			if (job.param_buf)
				stub_munmap(job.param_buf,
					    (cmd->param_size + 4095) & ~4095UL);
			stub_munmap(job.aux_buf,
				    (cmd->aux_size + 4095) & ~4095UL);
			struct isolate_resp_ioctl resp = {
				.type = ISOLATE_RESP_IOCTL, .req_id = cmd->req_id,
				.retval = -ENOMEM };
			locked_send(&resp, sizeof(resp));
			return;
		}
	}

	enqueue_job(&job);
}

static void handle_mmap(struct isolate_cmd_mmap *cmd)
{
	pthread_mutex_lock(&fd_mutex);
	int fd = handle_lookup(cmd->handle_id);
	pthread_mutex_unlock(&fd_mutex);

	struct isolate_resp_mmap r = { .type = ISOLATE_RESP_MMAP };
	if (fd < 0) { r.retval = -EBADF; locked_send(&r, sizeof(r)); return; }

	uint32_t flags = cmd->map_flags | MAP_FIXED;
	void *addr = stub_mmap((void *)(uintptr_t)cmd->gva, (size_t)cmd->length,
			       (int)cmd->prot, (int)flags, fd, (off_t)cmd->offset);
	r.retval = ((uintptr_t)addr == (uintptr_t)MAP_FAILED) ? -ENOMEM : 0;
	locked_send(&r, sizeof(r));
}

static void handle_munmap_cmd(struct isolate_cmd_munmap *cmd)
{
	stub_munmap((void *)(uintptr_t)cmd->gva, (size_t)cmd->length);
	send_ok();
}

/* ── install_mapping handlers ────────────────────────────────────────────── */

/*
 * Call KVM_SET_USER_MEMORY_REGION on the kvm fd we received at spawn.
 * The seccomp filter routes this to USER_NOTIF so QEMU's supervisor
 * revalidates the args before the kernel commits the call.
 *
 * For install: gva is the stub-side userspace_addr, size > 0, gpa is
 * where the region appears in the guest.  For uninstall: same struct,
 * size = 0 (kernel semantics — slot is removed when size=0).
 */
struct nvkvm_kvm_userspace_memory_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};

#ifndef KVM_MEM_READONLY
#define KVM_MEM_READONLY (1UL << 1)
#endif

static void handle_install_mapping(struct isolate_cmd_install_mapping *cmd,
				   int is_uninstall)
{
	struct isolate_resp_mapping resp = { .type = ISOLATE_RESP_MAPPING };

	if (g_kvm_fd < 0) {
		resp.status = -EBADF;
		locked_send(&resp, sizeof(resp));
		return;
	}

	struct nvkvm_kvm_userspace_memory_region region = {
		.slot            = cmd->slot,
		.flags           = (cmd->prot & 2 /*PROT_WRITE*/) ? 0 : KVM_MEM_READONLY,
		.guest_phys_addr = cmd->gpa,
		.memory_size     = is_uninstall ? 0 : cmd->size,
		.userspace_addr  = cmd->gva,
	};
	long ret = stub_ioctl(g_kvm_fd, NVKVM_KVM_SET_USER_MEMORY_REGION, &region);
	resp.status = (ret < 0) ? -errno : 0;
	locked_send(&resp, sizeof(resp));
}

/* ── Seccomp ─────────────────────────────────────────────────────────────── */

/*
 * Apply the seccomp filter. When `kvm_fd >= 0`, the filter routes
 *   ioctl(kvm_fd, KVM_SET_USER_MEMORY_REGION, *)
 * to SECCOMP_RET_USER_NOTIF and returns the listener fd from
 * seccomp(SECCOMP_SET_MODE_FILTER, NEW_LISTENER, ...).  When `kvm_fd < 0`
 * (fallback path used if QEMU did not pass a kvm fd), the filter just
 * permits the previously-allowed syscalls.
 *
 * Returns the listener fd (>=0) when NEW_LISTENER was set, 0 when not,
 * -errno on failure.
 */
static long apply_seccomp(int kvm_fd)
{
	struct sock_filter filter[64];
	int n = 0;

#define EMIT(...) do { \
	struct sock_filter _f = __VA_ARGS__; \
	filter[n++] = _f; \
} while (0)
#define ALLOW_IF(nr_val) do { \
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr_val), 0, 1)); \
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)); \
} while (0)

	/* Arch check */
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)));
	EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0));
	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS));

	/* Load nr */
	EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));

	/* KVM_SET_USER_MEMORY_REGION on kvm_fd → USER_NOTIF (before generic ioctl allow) */
	if (kvm_fd >= 0) {
		/* JT to first stmt of kvm-block; JF skips it.
		 * Block length: load args[0], cmp fd, load args[1], cmp cmd,
		 *  ret USER_NOTIF, reload nr  = 6 stmts. */
		EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_ioctl, 0, 6));
		EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
			      offsetof(struct seccomp_data, args[0])));
		/* if fd != kvm_fd, skip USER_NOTIF block */
		EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (uint32_t)kvm_fd, 0, 3));
		EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
			      offsetof(struct seccomp_data, args[1])));
		EMIT(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
			      (uint32_t)NVKVM_KVM_SET_USER_MEMORY_REGION, 0, 1));
		EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF));
		/* Common landing pad — reload nr for the generic allowlist below */
		EMIT(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));
	}

	ALLOW_IF(SYS_read);
	ALLOW_IF(SYS_write);
	ALLOW_IF(SYS_recvmsg);
	ALLOW_IF(SYS_sendmsg);
	ALLOW_IF(SYS_ioctl);
	ALLOW_IF(SYS_mmap);
	ALLOW_IF(SYS_mprotect);
	ALLOW_IF(SYS_munmap);
	ALLOW_IF(SYS_ppoll);
	ALLOW_IF(SYS_close);
	ALLOW_IF(SYS_exit_group);
	ALLOW_IF(SYS_rt_sigaction);
	ALLOW_IF(SYS_rt_sigreturn);
	ALLOW_IF(SYS_futex);
	ALLOW_IF(SYS_clone);
	ALLOW_IF(SYS_set_robust_list);
	ALLOW_IF(SYS_madvise);
	ALLOW_IF(SYS_lseek);
	ALLOW_IF(SYS_pread64);

	EMIT(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM));

#undef ALLOW_IF
#undef EMIT

	struct sock_fprog prog = {
		.len    = (unsigned short)n,
		.filter = filter,
	};
	stub_prctl(38 /* PR_SET_NO_NEW_PRIVS */, 1, 0, 0, 0);
	if (kvm_fd >= 0) {
		long r = stub_seccomp(SECCOMP_SET_MODE_FILTER,
				      SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
		if (r < 0) return -errno;
		return r; /* listener fd */
	}
	long r = stub_seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog);
	if (r < 0) return -errno;
	return 0;
}

/* ── Self-relocation ─────────────────────────────────────────────────────── */

/*
 * apply_relocations() is only needed when the stub binary is loaded via
 * fexecve() from a memfd (embedded/NVKVM_STUB_EMBEDDED build).  In that
 * scenario there is no dynamic linker to process RELA entries, so we do it
 * ourselves in a constructor.  When the stub is executed normally from disk
 * the kernel dynamic linker already handles all relocations before constructors
 * run, so applying them again would corrupt global-pointer state.
 */
#ifdef NVKVM_STUB_EMBEDDED
extern char __ehdr_start[];
extern char _DYNAMIC[];

__attribute__((constructor(101)))
static void apply_relocations(void)
{
	unsigned long base = (unsigned long)__ehdr_start;
	Elf64_Dyn *dyn = (Elf64_Dyn *)_DYNAMIC;
	Elf64_Rela *rela = NULL;
	size_t rela_sz = 0, rela_ent = sizeof(Elf64_Rela);

	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:    rela     = (Elf64_Rela *)(base + dyn->d_un.d_ptr); break;
		case DT_RELASZ:  rela_sz  = dyn->d_un.d_val; break;
		case DT_RELAENT: rela_ent = dyn->d_un.d_val; break;
		}
	}
	if (!rela) return;
	for (size_t i = 0; i < rela_sz / rela_ent; i++) {
		Elf64_Rela *r = (Elf64_Rela *)((char *)rela + i * rela_ent);
		if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
			uint64_t *target = (uint64_t *)(base + r->r_offset);
			*target = base + r->r_addend;
		}
	}
}
#endif /* NVKVM_STUB_EMBEDDED */

/* ── main ────────────────────────────────────────────────────────────────── */

/*
 * Spawn handshake: before applying seccomp, expect QEMU to send the KVM VM
 * fd via SCM_RIGHTS in a single one-shot message. If no such message
 * arrives (e.g. unit-test harness), fall back to the no-kvm-fd path and
 * apply seccomp without USER_NOTIF.
 *
 * On success, sets g_kvm_fd and returns the seccomp listener fd
 * (which we send back to QEMU). Returns -1 if we couldn't get a kvm fd
 * (degraded mode: install_mapping requests will fail with -EBADF).
 */
/* Debug write — bypasses libc, writes raw to fd 2 (inherited from QEMU). */
static void hsdbg(const char *s)
{
	size_t n = 0; while (s[n]) n++;
	stub_write(2, s, n);
}

static int do_spawn_handshake(void)
{
	hsdbg("nvkvm_stub: handshake start\n");
	/* Open /proc/self/maps before lockdown so we can read it later. */
	g_proc_maps_fd = (int)syscall(SYS_openat, AT_FDCWD,
				      "/proc/self/maps", O_RDONLY | O_CLOEXEC);
	hsdbg("nvkvm_stub: opened /proc/self/maps\n");

	/* Receive the KVM VM fd via SCM_RIGHTS. */
	struct isolate_cmd_receive_fd recv;
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &recv, sizeof(recv) };
	struct msghdr msg_hdr = {
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = cmsg_buf,
		.msg_controllen = sizeof(cmsg_buf),
	};
	long n = stub_recvmsg(SOCK_FD, &msg_hdr, 0);
	hsdbg("nvkvm_stub: recvmsg returned\n");
	{
		/* Print actual received bytes hex for debugging. */
		char buf[80];
		const char *hex = "0123456789abcdef";
		int p = 0;
		buf[p++] = 'n'; buf[p++] = '='; buf[p++] = '0'+ (int)n%10; buf[p++] = ' ';
		buf[p++] = 't'; buf[p++] = '=';
		for (int i = 3; i >= 0; i--) {
			unsigned b = ((unsigned char *)&recv.type)[i];
			buf[p++] = hex[(b>>4)&0xf]; buf[p++] = hex[b&0xf];
		}
		buf[p++] = ' '; buf[p++] = 'h'; buf[p++] = '=';
		for (int i = 3; i >= 0; i--) {
			unsigned b = ((unsigned char *)&recv.handle_id)[i];
			buf[p++] = hex[(b>>4)&0xf]; buf[p++] = hex[b&0xf];
		}
		buf[p++] = '\n';
		stub_write(2, buf, p);
	}
	if (n != (long)sizeof(recv) ||
	    recv.type != ISOLATE_CMD_RECEIVE_FD ||
	    recv.handle_id != 0xFFFFFFFFu) {
		hsdbg("nvkvm_stub: handshake: not RECEIVE_FD/0xFFFFFFFF — degrading\n");
		return -1;
	}
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg_hdr);
	if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
		hsdbg("nvkvm_stub: handshake: no SCM_RIGHTS — degrading\n");
		return -1;
	}
	__builtin_memcpy(&g_kvm_fd, CMSG_DATA(cm), sizeof(int));
	hsdbg("nvkvm_stub: got kvm fd\n");

	/* Apply seccomp with NEW_LISTENER. */
	long listener_fd = apply_seccomp(g_kvm_fd);
	hsdbg("nvkvm_stub: apply_seccomp returned\n");
	if (listener_fd <= 0) {
		hsdbg("nvkvm_stub: apply_seccomp failed/no listener\n");
		return -1;
	}

	/* Send listener fd back. */
	struct isolate_resp_ok rok = { .type = ISOLATE_RESP_OK };
	char cm_out[CMSG_SPACE(sizeof(int))];
	struct iovec iov_out = { &rok, sizeof(rok) };
	struct msghdr msg_out = {
		.msg_iov        = &iov_out,
		.msg_iovlen     = 1,
		.msg_control    = cm_out,
		.msg_controllen = sizeof(cm_out),
	};
	struct cmsghdr *cmo = CMSG_FIRSTHDR(&msg_out);
	cmo->cmsg_level = SOL_SOCKET;
	cmo->cmsg_type  = SCM_RIGHTS;
	cmo->cmsg_len   = CMSG_LEN(sizeof(int));
	int lfd = (int)listener_fd;
	__builtin_memcpy(CMSG_DATA(cmo), &lfd, sizeof(int));
	long sn = syscall(SYS_sendmsg, SOCK_FD, &msg_out, 0);
	hsdbg("nvkvm_stub: sent listener fd back\n");
	if (sn < 0) return -1;

	/* Close our reference to the listener — QEMU's supervisor is the
	 * only reader now. Keeping our own reference open is harmless but
	 * unnecessary; closing it keeps the stub's fdtable tidy. */
	stub_close((int)listener_fd);
	hsdbg("nvkvm_stub: closed local listener fd\n");
	return 0;
}

int main(void)
{
	handle_table_init();
	job_queue_init();
	pthread_once(&fault_key_once, init_fault_key);

	/* SIGSEGV handler — per-thread fault address via TLS */
	struct sigaction sa = { .sa_sigaction = sigsegv_handler,
				.sa_flags = SA_SIGINFO };
	stub_sigaction(SIGSEGV, &sa, NULL);

	/* Spawn worker threads */
	pthread_t workers[NVKVM_STUB_WORKERS];
	for (int i = 0; i < NVKVM_STUB_WORKERS; i++)
		pthread_create(&workers[i], NULL, worker_thread, NULL);

	/* QEMU sends kvm fd via SCM_RIGHTS first; we send listener fd back.
	 * On failure (e.g. older QEMU), apply seccomp without NEW_LISTENER. */
	if (do_spawn_handshake() < 0)
		apply_seccomp(-1);

	/*
	 * Reader loop — reads ONE complete SEQPACKET message per iteration.
	 *
	 * SOCK_SEQPACKET preserves message boundaries: a single read() consumes
	 * exactly one send() and discards any excess bytes if the buffer is
	 * smaller than the message.  The old split-read approach (read type,
	 * then read the rest) therefore discarded the body of every message.
	 *
	 * Fix: use recvmsg() with a union buffer large enough for any command
	 * struct, so the entire header arrives in one call.  Ancillary data
	 * (SCM_RIGHTS for RECEIVE_FD) is handled inline.
	 *
	 * Variable-length param/aux blobs for IOCTL are sent as separate
	 * SEQPACKET messages by QEMU and are still read individually below.
	 */
	for (;;) {
		union {
			uint32_t                            type;
			struct isolate_cmd_receive_fd       recv_fd;
			struct isolate_cmd_close_fd         close_fd;
			struct isolate_cmd_ioctl            ioctl_cmd;
			struct isolate_cmd_mmap             mmap_cmd;
			struct isolate_cmd_munmap           munmap_cmd;
			struct isolate_cmd_poll             poll_cmd;
			struct isolate_cmd_unpoll           unpoll_cmd;
			struct isolate_cmd_install_mapping  install_mapping_cmd;
		} cmd;
		char cmsg_buf[CMSG_SPACE(sizeof(int))];

		struct iovec iov = { &cmd, sizeof(cmd) };
		struct msghdr msg_hdr = {
			.msg_iov        = &iov,
			.msg_iovlen     = 1,
			.msg_control    = cmsg_buf,
			.msg_controllen = sizeof(cmsg_buf),
		};

		long n = stub_recvmsg(SOCK_FD, &msg_hdr, 0);
		if (n <= 0)
			break;
		if (n < (long)sizeof(uint32_t))
			goto done;

		switch (cmd.type) {
		case ISOLATE_CMD_RECEIVE_FD: {
			struct cmsghdr *cm = CMSG_FIRSTHDR(&msg_hdr);
			if (!cm || cm->cmsg_level != SOL_SOCKET ||
			    cm->cmsg_type != SCM_RIGHTS) {
				send_error(EINVAL);
				break;
			}
			int fd;
			__builtin_memcpy(&fd, CMSG_DATA(cm), sizeof(int));
			pthread_mutex_lock(&fd_mutex);
			if (cmd.recv_fd.handle_id < MAX_HANDLES &&
			    handle_fds[cmd.recv_fd.handle_id] >= 0)
				stub_close(handle_fds[cmd.recv_fd.handle_id]);
			handle_store(cmd.recv_fd.handle_id, fd);
			pthread_mutex_unlock(&fd_mutex);
			send_ok();
			break;
		}
		case ISOLATE_CMD_CLOSE_FD:
			handle_close_fd(cmd.close_fd.handle_id);
			break;
		case ISOLATE_CMD_IOCTL:
			handle_ioctl_cmd(&cmd.ioctl_cmd);
			break;
		case ISOLATE_CMD_MMAP:
			handle_mmap(&cmd.mmap_cmd);
			break;
		case ISOLATE_CMD_MUNMAP:
			handle_munmap_cmd(&cmd.munmap_cmd);
			break;
		case ISOLATE_CMD_POLL:
			(void)cmd.poll_cmd;
			send_ok();  /* TODO: background poll */
			break;
		case ISOLATE_CMD_UNPOLL:
			(void)cmd.unpoll_cmd;
			send_ok();
			break;
		case ISOLATE_CMD_INSTALL_MAPPING:
			handle_install_mapping(&cmd.install_mapping_cmd, 0);
			break;
		case ISOLATE_CMD_UNINSTALL_MAPPING:
			handle_install_mapping(&cmd.install_mapping_cmd, 1);
			break;
		case ISOLATE_CMD_EXIT:
			goto done;
		default:
			goto done;
		}
	}

done:
	stub_exiting = 1;
	pthread_cond_broadcast(&queue_cond);
	for (int i = 0; i < NVKVM_STUB_WORKERS; i++)
		pthread_join(workers[i], NULL);
	stub_exit(0);
}
