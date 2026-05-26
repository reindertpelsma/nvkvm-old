/*
 * nvkvm_isolate.c — QEMU-side isolate process manager (multi-inflight)
 *
 * Each isolate has a dedicated reader thread that multiplexes IOCTL responses
 * by req_id onto per-caller condvars allocated on the callers' stacks.
 * Non-IOCTL commands serialize via sync_lock + sync_cond (one at a time).
 * All socket writes go through write_lock (prevents partial-send interleaving).
 *
 * The stub binary is embedded as nvkvm_stub_elf[] generated at build time
 * from src/stub/nvkvm_stub; loaded via memfd_create + fexecve (no disk file).
 *
 * Lock order: sync_lock > write_lock > lock
 */

#include "qemu/osdep.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <signal.h>

/* memfd_create may not be in older glibc headers; use syscall directly. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static inline int nvkvm_memfd_create(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, (unsigned long)flags);
}

#include "nvkvm_isolate.h"
#include "virtio_nvgpu.h"

#include "../../src/common/nvkvm_isolate_proto.h"

#ifdef NVKVM_STUB_EMBEDDED
#include "nvkvm_stub_bin.h"
static const unsigned char *stub_elf     = nvkvm_stub;
static unsigned int         stub_elf_len = nvkvm_stub_len;
#else
static const unsigned char *stub_elf     = NULL;
static unsigned int         stub_elf_len = 0;
#endif

/* ── In-flight IOCTL request (lives on the caller's stack) ──────────────── */

struct nvkvm_pending_ioctl {
	uint32_t        req_id;
	bool            done;       /* set by reader thread */
	pthread_cond_t  cond;       /* signaled by reader, waited under iso->lock */

	/* Caller's output buffers — written by reader before done=true */
	void           *param_buf;
	size_t          param_cap;
	void           *aux_buf;
	size_t          aux_cap;

	/* Response fields (written by reader before done=true) */
	int             error;      /* transport error (-errno), 0 on success */
	int32_t         retval;
	uint32_t        nvstatus;
	uint64_t        fault_addr;

	struct nvkvm_pending_ioctl *next; /* intrusive list, protected by iso->lock */
};

/* ── Socket I/O helpers ─────────────────────────────────────────────────── */

static ssize_t sock_send_full(int fd, const void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = send(fd, (const char *)buf + done, len - done,
				 MSG_NOSIGNAL);
		if (n <= 0)
			return n < 0 ? -errno : -ECONNRESET;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

/* Send fd via SCM_RIGHTS together with a RECEIVE_FD header. */
static ssize_t sock_sendmsg_fd(int sock, struct msghdr *msg)
{
	ssize_t n = sendmsg(sock, msg, MSG_NOSIGNAL);
	return (n < 0) ? -errno : n;
}

/* ── Reader thread ──────────────────────────────────────────────────────── */

/*
 * Maximum payload size for IOCTL param/aux blobs.  If the isolate sends
 * a blob larger than this, it is truncated/discarded (protocol violation).
 */
#define MAX_IOCTL_PAYLOAD  (64 * 1024)

/* Drain one SEQPACKET message; for SEQPACKET a single recv consumes one msg. */
static void drain_message(int fd)
{
	char buf[MAX_IOCTL_PAYLOAD];
	recv(fd, buf, sizeof(buf), 0);
}

static void reader_signal_sync(struct nvkvm_isolate *iso, int err,
				int mmap_retval)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_error       = err;
	iso->sync_mmap_retval = mmap_retval;
	iso->sync_done        = true;
	pthread_cond_signal(&iso->sync_cond);
	pthread_mutex_unlock(&iso->sync_lock);
}

static void *isolate_reader_fn(void *arg)
{
	struct nvkvm_isolate *iso = arg;

	union {
		uint32_t                       type;
		struct isolate_resp_ok         ok;
		struct isolate_resp_error      err;
		struct isolate_resp_ioctl      ioctl;
		struct isolate_resp_mmap       mmap;
		struct isolate_resp_poll_event poll_event;
	} u;

	for (;;) {
		/*
		 * For SOCK_SEQPACKET one recv() reads exactly one message.
		 * A buffer larger than the message is fine; excess bytes are
		 * discarded. A buffer smaller would truncate — our union is
		 * sized to the largest response struct, so we're safe.
		 */
		ssize_t n = recv(iso->sock_fd, &u, sizeof(u), 0);
		if (n <= 0)
			break;

		switch (u.type) {
		case ISOLATE_RESP_OK:
			reader_signal_sync(iso, 0, 0);
			break;

		case ISOLATE_RESP_ERROR:
			reader_signal_sync(iso, -(int)u.err.err, 0);
			break;

		case ISOLATE_RESP_MMAP:
			reader_signal_sync(iso, 0, u.mmap.retval);
			break;

		case ISOLATE_RESP_IOCTL: {
			uint32_t req_id     = u.ioctl.req_id;
			int32_t  retval     = u.ioctl.retval;
			uint32_t nvstatus   = u.ioctl.nvstatus;
			uint64_t fault_addr = u.ioctl.fault_addr;
			uint32_t param_size = u.ioctl.param_size;
			uint32_t aux_size   = u.ioctl.aux_size;

			/* Locate the pending caller (brief lock). */
			pthread_mutex_lock(&iso->lock);
			struct nvkvm_pending_ioctl *p = iso->pending_head;
			while (p && p->req_id != req_id)
				p = p->next;
			pthread_mutex_unlock(&iso->lock);

			/*
			 * Read param blob.  We're the only reader on this
			 * socket so we can do this without the lock.
			 */
			if (param_size > 0) {
				if (p && p->param_buf &&
				    param_size <= (uint32_t)p->param_cap) {
					n = recv(iso->sock_fd, p->param_buf,
						 p->param_cap, 0);
					if (n <= 0)
						goto reader_exit;
				} else {
					drain_message(iso->sock_fd);
					if (p)
						p->param_buf = NULL;
				}
			}

			/* Read aux blob. */
			if (aux_size > 0) {
				if (p && p->aux_buf &&
				    aux_size <= (uint32_t)p->aux_cap) {
					n = recv(iso->sock_fd, p->aux_buf,
						 p->aux_cap, 0);
					if (n <= 0)
						goto reader_exit;
				} else {
					drain_message(iso->sock_fd);
					if (p)
						p->aux_buf = NULL;
				}
			}

			if (p) {
				pthread_mutex_lock(&iso->lock);
				p->retval     = retval;
				p->nvstatus   = nvstatus;
				p->fault_addr = fault_addr;
				p->error      = 0;
				p->done       = true;
				pthread_cond_signal(&p->cond);
				pthread_mutex_unlock(&iso->lock);
			}
			break;
		}

		case ISOLATE_RESP_POLL_EVENT:
			/* TODO: forward to virtio EVT queue */
			break;

		default:
			fprintf(stderr,
				"nvkvm_isolate: unknown response type 0x%x\n",
				u.type);
			break;
		}
	}

reader_exit:
	/* Wake every pending IOCTL caller with a transport error. */
	pthread_mutex_lock(&iso->lock);
	iso->alive = false;
	for (struct nvkvm_pending_ioctl *p = iso->pending_head; p; p = p->next) {
		p->error = -ECONNRESET;
		p->done  = true;
		pthread_cond_signal(&p->cond);
	}
	iso->pending_head = NULL;
	pthread_mutex_unlock(&iso->lock);

	/* Wake any pending sync command too. */
	reader_signal_sync(iso, -ECONNRESET, 0);

	return NULL;
}

/* ── Table management ───────────────────────────────────────────────────── */

void nvkvm_isolate_table_init(struct nvkvm_isolate_table *t)
{
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->lock, NULL);
	t->next_id = 1;
	for (int i = 0; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		iso->sock_fd = -1;
		pthread_mutex_init(&iso->lock,       NULL);
		pthread_mutex_init(&iso->write_lock, NULL);
		pthread_mutex_init(&iso->sync_lock,  NULL);
		pthread_cond_init(&iso->sync_cond,   NULL);
	}
}

void nvkvm_isolate_table_fini(struct nvkvm_isolate_table *t)
{
	for (int i = 1; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		if (iso->in_use)
			nvkvm_isolate_kill(t, iso->id);
		pthread_mutex_destroy(&iso->lock);
		pthread_mutex_destroy(&iso->write_lock);
		pthread_mutex_destroy(&iso->sync_lock);
		pthread_cond_destroy(&iso->sync_cond);
	}
	pthread_mutex_destroy(&t->lock);
}

static struct nvkvm_isolate *alloc_isolate_slot(struct nvkvm_isolate_table *t,
						uint32_t *id_out)
{
	for (int attempt = 0; attempt < NVKVM_ISOLATE_MAX; attempt++) {
		uint32_t id = t->next_id++;
		if (t->next_id >= NVKVM_ISOLATE_MAX)
			t->next_id = 1;
		if (id == 0)
			continue;
		struct nvkvm_isolate *iso = &t->isolates[id % NVKVM_ISOLATE_MAX];
		if (!iso->in_use) {
			iso->id           = id;
			iso->in_use       = true;
			iso->alive        = false;
			iso->sock_fd      = -1;
			iso->pending_head = NULL;
			iso->next_req_id  = 1;
			iso->sync_done    = false;
			iso->reader_started = false;
			*id_out = id;
			return iso;
		}
	}
	return NULL;
}

/* ── Spawn isolate ──────────────────────────────────────────────────────── */

int nvkvm_isolate_create(struct nvkvm_isolate_table *t,
			 uint32_t session_id,
			 uint32_t *isolate_id_out)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
		return -errno;

	pthread_mutex_lock(&t->lock);
	uint32_t id;
	struct nvkvm_isolate *iso = alloc_isolate_slot(t, &id);
	if (!iso) {
		pthread_mutex_unlock(&t->lock);
		close(sv[0]);
		close(sv[1]);
		return -EMFILE;
	}
	iso->session_id = session_id;
	pthread_mutex_unlock(&t->lock);

	pid_t pid;

	if (stub_elf && stub_elf_len > 0) {
		int mfd = nvkvm_memfd_create("nvkvm_stub", MFD_CLOEXEC);
		if (mfd < 0) {
			close(sv[0]);
			close(sv[1]);
			iso->in_use = false;
			return -errno;
		}
		if (write(mfd, stub_elf, stub_elf_len) != (ssize_t)stub_elf_len) {
			close(mfd);
			close(sv[0]);
			close(sv[1]);
			iso->in_use = false;
			return -EIO;
		}
		lseek(mfd, 0, SEEK_SET);

		pid = fork();
		if (pid == 0) {
			dup2(sv[1], STDIN_FILENO);
			close(sv[0]);
			close(sv[1]);
			const char *argv[] = { "nvkvm_stub", NULL };
			const char *envp[] = { NULL };
			fexecve(mfd, (char *const *)argv, (char *const *)envp);
			_exit(127);
		}
		close(mfd);
	} else {
		const char *stub_path = getenv("NVKVM_STUB_PATH");
		if (!stub_path)
			stub_path = "/usr/lib/nvkvm/nvkvm_stub";

		pid = fork();
		if (pid == 0) {
			dup2(sv[1], STDIN_FILENO);
			close(sv[0]);
			close(sv[1]);
			/* DEBUG: inject ioctl-dump LD_PRELOAD if requested */
			const char *dbg = getenv("NVKVM_STUB_LD_PRELOAD");
			if (dbg && *dbg) {
				setenv("LD_PRELOAD", dbg, 1);
				/* If TRACE_FILE env vars exist for stub, override
				 * the inherited QEMU ones so the stub writes to
				 * its own file. */
				const char *stf = getenv("NVKVM_STUB_TRACE_FILE");
				const char *stt = getenv("NVKVM_STUB_TRACE_TAG");
				if (stf) setenv("TRACE_FILE", stf, 1);
				if (stt) setenv("TRACE_TAG", stt, 1);
			}
			execl(stub_path, "nvkvm_stub", NULL);
			_exit(127);
		}
	}

	if (pid < 0) {
		int e = errno;
		close(sv[0]);
		close(sv[1]);
		iso->in_use = false;
		return -e;
	}

	close(sv[1]);

	iso->pid     = pid;
	iso->sock_fd = sv[0];
	iso->alive   = true;

	/* Start the reader thread before announcing success. */
	if (pthread_create(&iso->reader_tid, NULL, isolate_reader_fn, iso)) {
		int e = errno;
		close(iso->sock_fd);
		iso->sock_fd = -1;
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		iso->in_use = false;
		return -e;
	}
	iso->reader_started = true;

	*isolate_id_out = id;

	fprintf(stderr,
		"nvkvm_isolate: created isolate %u pid=%d sock=%d\n",
		id, pid, sv[0]);
	return 0;
}

/* ── Kill isolate ───────────────────────────────────────────────────────── */

int nvkvm_isolate_kill(struct nvkvm_isolate_table *t, uint32_t isolate_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;

	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	if (!iso->in_use || iso->id != isolate_id) {
		pthread_mutex_unlock(&iso->lock);
		return -ENOENT;
	}

	if (iso->alive && iso->sock_fd >= 0) {
		struct isolate_cmd_exit cmd = { .type = ISOLATE_CMD_EXIT };
		/* Best-effort; ignore error — we're shutting down anyway. */
		send(iso->sock_fd, &cmd, sizeof(cmd), MSG_NOSIGNAL);
	}

	iso->alive = false;
	int sock_fd = iso->sock_fd;
	iso->sock_fd = -1;
	pthread_mutex_unlock(&iso->lock);

	/* Closing the socket makes the reader thread's recv() return 0/error. */
	if (sock_fd >= 0)
		close(sock_fd);

	/*
	 * Join the reader thread: it has already signaled all pending IOCTL
	 * callers and the sync waiter (if any).  After join, no thread accesses
	 * iso's internals through the reader path.
	 */
	if (iso->reader_started) {
		pthread_join(iso->reader_tid, NULL);
		iso->reader_started = false;
	}

	pid_t pid = iso->pid;
	if (pid > 0) {
		int status;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
		nanosleep(&ts, NULL);
		if (waitpid(pid, &status, WNOHANG) == 0) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
		}
	}

	pthread_mutex_lock(&iso->lock);
	iso->pid    = 0;
	iso->in_use = false;
	pthread_mutex_unlock(&iso->lock);

	fprintf(stderr, "nvkvm_isolate: killed isolate %u\n", isolate_id);
	return 0;
}

void nvkvm_isolate_kill_session(struct nvkvm_isolate_table *t,
				uint32_t session_id)
{
	for (int i = 1; i < NVKVM_ISOLATE_MAX; i++) {
		struct nvkvm_isolate *iso = &t->isolates[i];
		if (iso->in_use && iso->session_id == session_id)
			nvkvm_isolate_kill(t, iso->id);
	}
}

/* ── Sync command helpers ───────────────────────────────────────────────── */

/*
 * Send a fixed-size command and wait for an OK/ERROR response.
 * The reader thread delivers the response via sync_cond.
 * Caller must NOT hold iso->lock.
 */
static int sync_send_recv(struct nvkvm_isolate *iso,
			  const void *cmd_buf, size_t cmd_size)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/*
 * Like sync_send_recv but via sendmsg (for SCM_RIGHTS); returns sync_error.
 */
static int sync_sendmsg_recv(struct nvkvm_isolate *iso, struct msghdr *msg)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done  = false;
	iso->sync_error = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_sendmsg_fd(iso->sock_fd, msg);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/*
 * Like sync_send_recv but returns the MMAP retval on success.
 */
static int sync_send_recv_mmap(struct nvkvm_isolate *iso,
				const void *cmd_buf, size_t cmd_size)
{
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done        = false;
	iso->sync_error       = 0;
	iso->sync_mmap_retval = 0;

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, cmd_buf, cmd_size);
	pthread_mutex_unlock(&iso->write_lock);

	if (sr < 0) {
		pthread_mutex_unlock(&iso->sync_lock);
		return (int)sr;
	}

	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	int result = iso->sync_error ? iso->sync_error : iso->sync_mmap_retval;
	pthread_mutex_unlock(&iso->sync_lock);
	return result;
}

/* ── Handle distribution ────────────────────────────────────────────────── */

int nvkvm_isolate_send_handle(struct nvkvm_isolate_table *t,
			      struct nvkvm_handle_table *ht,
			      uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	struct nvkvm_handle *h = valid ? nvkvm_handle_get(ht, handle_id) : NULL;
	int fd        = (h && h->fd >= 0) ? h->fd : -1;
	int h_dev_id  = h ? h->dev_id : 0;
	pthread_mutex_unlock(&iso->lock);

	if (!valid)
		return -ENOENT;
	if (fd < 0)
		return -EBADF;

	struct isolate_cmd_receive_fd hdr = {
		.type      = ISOLATE_CMD_RECEIVE_FD,
		.handle_id = handle_id,
		.dev_id    = (uint32_t)h_dev_id,
	};
	struct msghdr   msg  = { 0 };
	struct iovec    iov  = { .iov_base = &hdr, .iov_len = sizeof(hdr) };
	char            cbuf[CMSG_SPACE(sizeof(int))];

	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type  = SCM_RIGHTS;
	cm->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));

	int ret = sync_sendmsg_recv(iso, &msg);
	if (ret == 0)
		nvkvm_handle_ref_isolate(ht, handle_id);
	return ret;
}

int nvkvm_isolate_close_handle(struct nvkvm_isolate_table *t,
				struct nvkvm_handle_table *ht,
				uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_close_fd cmd = {
		.type      = ISOLATE_CMD_CLOSE_FD,
		.handle_id = handle_id,
	};
	int ret = sync_send_recv(iso, &cmd, sizeof(cmd));
	if (ret == 0)
		nvkvm_handle_unref_isolate(ht, handle_id);
	return ret;
}

/* ── Ioctl forwarding (async, multi-inflight) ───────────────────────────── */

int nvkvm_isolate_ioctl(struct nvkvm_isolate_table *t,
			uint32_t isolate_id, uint32_t handle_id,
			unsigned int cmd,
			void *param_buf, size_t param_size,
			void *aux_buf, size_t aux_size,
			uint32_t flags,
			uint32_t *nvstatus_out,
			uint64_t *fault_addr_out)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	/* Build a pending slot on the caller's stack. */
	struct nvkvm_pending_ioctl pending = {
		.done      = false,
		.error     = 0,
		.param_buf = param_buf,
		.param_cap = param_size,
		.aux_buf   = aux_buf,
		.aux_cap   = aux_size,
	};
	pthread_cond_init(&pending.cond, NULL);

	/* Validate and register. */
	pthread_mutex_lock(&iso->lock);
	if (!iso->in_use || iso->id != isolate_id || !iso->alive) {
		pthread_mutex_unlock(&iso->lock);
		pthread_cond_destroy(&pending.cond);
		return -ENOENT;
	}
	pending.req_id = iso->next_req_id++;
	if (iso->next_req_id == 0)
		iso->next_req_id = 1;
	pending.next      = iso->pending_head;
	iso->pending_head = &pending;
	pthread_mutex_unlock(&iso->lock);

	/* Send command under write_lock. */
	struct isolate_cmd_ioctl hdr = {
		.type       = ISOLATE_CMD_IOCTL,
		.handle_id  = handle_id,
		.cmd        = (uint32_t)cmd,
		.param_size = (uint32_t)param_size,
		.aux_size   = (uint32_t)aux_size,
		.flags      = flags,
		.req_id     = pending.req_id,
	};

	pthread_mutex_lock(&iso->write_lock);
	ssize_t sr = sock_send_full(iso->sock_fd, &hdr, sizeof(hdr));
	if (sr >= 0 && param_size > 0)
		sr = sock_send_full(iso->sock_fd, param_buf, param_size);
	if (sr >= 0 && aux_size > 0)
		sr = sock_send_full(iso->sock_fd, aux_buf, aux_size);
	pthread_mutex_unlock(&iso->write_lock);

	/*
	 * Wait for the reader thread to deliver the response.
	 * If the send failed, the reader will notice the dead socket and
	 * signal us with -ECONNRESET.  Either way we always wait.
	 */
	pthread_mutex_lock(&iso->lock);
	while (!pending.done)
		pthread_cond_wait(&pending.cond, &iso->lock);
	/* Remove from pending list. */
	struct nvkvm_pending_ioctl **pp = &iso->pending_head;
	while (*pp && *pp != &pending)
		pp = &(*pp)->next;
	if (*pp)
		*pp = pending.next;
	pthread_mutex_unlock(&iso->lock);
	pthread_cond_destroy(&pending.cond);

	/* Prefer the transport error from the send over the reader's error. */
	if (sr < 0 && !pending.error)
		return (int)sr;
	if (pending.error)
		return pending.error;

	if (nvstatus_out)
		*nvstatus_out = pending.nvstatus;
	if (fault_addr_out)
		*fault_addr_out = pending.fault_addr;
	return pending.retval;
}

/* ── Mmap / munmap ──────────────────────────────────────────────────────── */

int nvkvm_isolate_mmap(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint64_t gva, uint64_t length, uint64_t offset,
		       int prot, int map_flags)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_mmap cmd = {
		.type      = ISOLATE_CMD_MMAP,
		.handle_id = handle_id,
		.gva       = gva,
		.length    = length,
		.offset    = offset,
		.prot      = (uint32_t)prot,
		.map_flags = (uint32_t)map_flags,
	};
	return sync_send_recv_mmap(iso, &cmd, sizeof(cmd));
}

int nvkvm_isolate_munmap(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint64_t gva, uint64_t length)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_munmap cmd = {
		.type   = ISOLATE_CMD_MUNMAP,
		.gva    = gva,
		.length = length,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}

/* ── Poll / unpoll ──────────────────────────────────────────────────────── */

int nvkvm_isolate_poll(struct nvkvm_isolate_table *t,
		       uint32_t isolate_id, uint32_t handle_id,
		       uint32_t events)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_poll cmd = {
		.type      = ISOLATE_CMD_POLL,
		.handle_id = handle_id,
		.events    = events,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}

int nvkvm_isolate_unpoll(struct nvkvm_isolate_table *t,
			 uint32_t isolate_id, uint32_t handle_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return -ENOENT;
	struct nvkvm_isolate *iso = &t->isolates[isolate_id % NVKVM_ISOLATE_MAX];

	pthread_mutex_lock(&iso->lock);
	bool valid = iso->in_use && iso->id == isolate_id && iso->alive;
	pthread_mutex_unlock(&iso->lock);
	if (!valid)
		return -ENOENT;

	struct isolate_cmd_unpoll cmd = {
		.type      = ISOLATE_CMD_UNPOLL,
		.handle_id = handle_id,
	};
	return sync_send_recv(iso, &cmd, sizeof(cmd));
}
