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

#include "../common/nvkvm_isolate_proto.h"

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

		clear_fault_addr();
		long ret  = stub_ioctl(fd, job.cmd, job.param_buf);
		int  err  = (ret < 0) ? errno : 0;

		/* Zero the pointer field before sending back (don't leak host VA) */
		if (job.aux_size > 0 && job.param_size >= 24) {
			uint64_t zero = 0;
			__builtin_memcpy((char *)job.param_buf + 16, &zero,
					 sizeof(uint64_t));
		}

		/*
		 * Extract NvStatus from the response struct.  For nvos54
		 * (RM_CONTROL) and nvos21/nvos64 (RM_ALLOC), the status field
		 * is at offset 28 in the 32-byte layout.  Larger structs (like
		 * nvos64 which is 48 bytes) have status at a different offset;
		 * read conservatively only for param_size >= 32.
		 */
		uint32_t nvstatus = 0;
		if (job.param_size >= 32)
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

/* ── Seccomp ─────────────────────────────────────────────────────────────── */

static void apply_seccomp(void)
{
#define JUMP_ALLOW(nr) \
	BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr), 0, 1), \
	BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)

	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
			 offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		JUMP_ALLOW(SYS_read),
		JUMP_ALLOW(SYS_write),
		JUMP_ALLOW(SYS_recvmsg),
		JUMP_ALLOW(SYS_sendmsg),
		JUMP_ALLOW(SYS_ioctl),
		JUMP_ALLOW(SYS_mmap),
		JUMP_ALLOW(SYS_mprotect),
		JUMP_ALLOW(SYS_munmap),
		JUMP_ALLOW(SYS_ppoll),
		JUMP_ALLOW(SYS_close),
		JUMP_ALLOW(SYS_exit_group),
		JUMP_ALLOW(SYS_rt_sigaction),
		JUMP_ALLOW(SYS_rt_sigreturn),
		JUMP_ALLOW(SYS_futex),     /* needed by pthreads */
		JUMP_ALLOW(SYS_clone),     /* needed for pthread_create */
		JUMP_ALLOW(SYS_set_robust_list),
		JUMP_ALLOW(SYS_madvise),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM),
	};
#undef JUMP_ALLOW

	struct sock_fprog prog = {
		.len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
		.filter = filter,
	};
	stub_prctl(38 /* PR_SET_NO_NEW_PRIVS */, 1, 0, 0, 0);
	stub_seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog);
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

	apply_seccomp();

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
			uint32_t                        type;
			struct isolate_cmd_receive_fd   recv_fd;
			struct isolate_cmd_close_fd     close_fd;
			struct isolate_cmd_ioctl        ioctl_cmd;
			struct isolate_cmd_mmap         mmap_cmd;
			struct isolate_cmd_munmap       munmap_cmd;
			struct isolate_cmd_poll         poll_cmd;
			struct isolate_cmd_unpoll       unpoll_cmd;
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
