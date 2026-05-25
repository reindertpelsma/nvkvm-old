/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_install_mapping.c — guest-driven KVM memory region installs.
 *
 * See nvkvm_install_mapping.h and docs/INSTALL_ISOLATE_MAPPING.md.
 */

#include "qemu/osdep.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include "virtio_nvgpu.h"
#include "nvkvm_isolate.h"
#include "nvkvm_install_mapping.h"
#include "../../src/common/nvkvm_isolate_proto.h"

/* KVM ioctl/struct (avoid <linux/kvm.h> include due to QEMU header chain conflicts). */
#define NVKVM_KVMIO 0xAE
#ifndef KVM_MEM_READONLY
#define KVM_MEM_READONLY (1UL << 1)
#endif
#ifndef KVM_SET_USER_MEMORY_REGION
#define KVM_SET_USER_MEMORY_REGION  _IOW(NVKVM_KVMIO, 0x46, \
		struct kvm_userspace_memory_region_compat)
struct kvm_userspace_memory_region_compat {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#endif

/* seccomp_notif structs (Linux 5.0+; redefine to avoid version skew). */
#ifndef SECCOMP_IOCTL_NOTIF_RECV
struct nvkvm_seccomp_notif {
	uint64_t id;
	uint32_t pid;
	uint32_t flags;
	struct {
		int      nr;
		uint32_t arch;
		uint64_t instruction_pointer;
		uint64_t args[6];
	} data;
};
struct nvkvm_seccomp_notif_resp {
	uint64_t id;
	int64_t  val;
	int32_t  error;
	uint32_t flags;
};
#define SECCOMP_IOCTL_NOTIF_RECV   _IOWR('!', 0, struct nvkvm_seccomp_notif)
#define SECCOMP_IOCTL_NOTIF_SEND   _IOWR('!', 1, struct nvkvm_seccomp_notif_resp)
#define SECCOMP_IOCTL_NOTIF_ID_VALID  _IOW('!', 2, uint64_t)
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE  (1UL << 0)
#else
#define nvkvm_seccomp_notif      seccomp_notif
#define nvkvm_seccomp_notif_resp seccomp_notif_resp
#endif

/* Slot numbering: start above the static base used by nvkvm_mmap_host.c
 * for handle-based mmaps. Per-isolate counter so multiple isolates don't
 * collide. */
#define NVKVM_INSTALL_SLOT_BASE   1024

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static struct nvkvm_isolate *find_isolate(struct VirtIONvgpu *nv,
					   uint32_t isolate_id)
{
	if (isolate_id == 0 || isolate_id >= NVKVM_ISOLATE_MAX)
		return NULL;
	struct nvkvm_isolate *iso = &nv->isolates.isolates[isolate_id % NVKVM_ISOLATE_MAX];
	if (!iso->in_use || iso->id != isolate_id || !iso->alive) return NULL;
	return iso;
}

static struct nvkvm_install_entry *
find_install_entry(struct nvkvm_isolate *iso, uint64_t gva, uint64_t size)
{
	for (struct nvkvm_install_entry *e = iso->install_head; e; e = e->next) {
		if (e->gva == gva && e->size == size && !e->consumed)
			return e;
	}
	return NULL;
}

static void free_install_entry(struct nvkvm_isolate *iso,
			        struct nvkvm_install_entry *target)
{
	struct nvkvm_install_entry **pp = &iso->install_head;
	while (*pp) {
		if (*pp == target) {
			*pp = target->next;
			free(target);
			return;
		}
		pp = &(*pp)->next;
	}
}

/* ── Supervisor thread ───────────────────────────────────────────────────── */

/*
 * Reads seccomp USER_NOTIF events from notify_fd; for each, validates the
 * trapped KVM_SET_USER_MEMORY_REGION call against the install whitelist
 * and responds CONTINUE or EPERM.
 *
 * We peek the kvm_userspace_memory_region from the stub's mm at
 * args[2] using /proc/<stub-pid>/mem to confirm slot/gpa/size/userspace_addr
 * match the whitelist. This is the *real* validation; the whitelist is
 * QEMU's record of which calls it pre-authorised.
 */
static int peek_remote(pid_t pid, uint64_t addr, void *out, size_t len)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/mem", pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -errno;
	ssize_t n = pread(fd, out, len, (off_t)addr);
	close(fd);
	return (n < 0) ? -errno : (int)n;
}

static void *supervisor_thread(void *arg)
{
	struct nvkvm_isolate *iso = (struct nvkvm_isolate *)arg;

	/* Block all signals in this thread.  QEMU directs SIGALRM and
	 * other timer signals to the I/O thread; if the supervisor thread
	 * happens to absorb them, QEMU's main loop stalls. */
	sigset_t allsigs;
	sigfillset(&allsigs);
	pthread_sigmask(SIG_SETMASK, &allsigs, NULL);

	fprintf(stderr,
		"nvkvm_install: supervisor thread started isolate=%u notify_fd=%d\n",
		iso->id, iso->notify_fd);
	while (iso->alive && iso->notify_fd >= 0) {
		struct nvkvm_seccomp_notif notif = {0};
		int rc = ioctl(iso->notify_fd, SECCOMP_IOCTL_NOTIF_RECV, &notif);
		if (rc < 0) {
			int e = errno;
			if (e == EINTR) continue;
			if (e == ENOENT) continue; /* tracee gone */
			fprintf(stderr,
				"nvkvm_install: supervisor RECV failed errno=%d (%s) — "
				"exiting thread\n", e, strerror(e));
			break;
		}
		fprintf(stderr,
			"nvkvm_install: supervisor got notif id=%llu pid=%u nr=%d "
			"args[0]=0x%llx args[1]=0x%llx args[2]=0x%llx\n",
			(unsigned long long)notif.id,
			(unsigned int)notif.pid,
			notif.data.nr,
			(unsigned long long)notif.data.args[0],
			(unsigned long long)notif.data.args[1],
			(unsigned long long)notif.data.args[2]);
		int xrc;

		/* The trapped syscall is ioctl(kvm_fd, KVM_SET_USER_MEMORY_REGION,
		 * &region). region is in the stub's mm at notif.data.args[2]. */
		uint64_t region_ptr = notif.data.args[2];
		struct kvm_userspace_memory_region_compat region;
		xrc = peek_remote(notif.pid, region_ptr,
				     &region, sizeof(region));

		struct nvkvm_seccomp_notif_resp resp = {
			.id    = notif.id,
			.val   = 0,
			.error = -EPERM,
			.flags = 0,
		};

		if (xrc != (int)sizeof(region)) {
			/* Could not validate; reject. */
			resp.error = -EFAULT;
			goto send_resp;
		}

		/* Look up the whitelist entry. Match on slot first (we picked
		 * it ourselves), then verify everything else. */
		pthread_mutex_lock(&iso->install_lock);
		struct nvkvm_install_entry *match = NULL;
		for (struct nvkvm_install_entry *e = iso->install_head; e; e = e->next) {
			if (!e->pending || e->consumed) continue;
			if (e->slot != region.slot) continue;
			if (e->gpa != region.guest_phys_addr) continue;
			if (e->size != region.memory_size)    continue;
			if (e->gva != region.userspace_addr)  continue;
			match = e;
			break;
		}
		if (match) match->consumed = true; /* the syscall is about to run */
		pthread_mutex_unlock(&iso->install_lock);

		if (match) {
			resp.error = 0;
			resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
		} else {
			fprintf(stderr,
				"nvkvm: USER_NOTIF: unauthorised KVM_SET_USER_MEMORY_REGION "
				"slot=%u gpa=0x%llx size=0x%llx va=0x%llx — denying\n",
				region.slot,
				(unsigned long long)region.guest_phys_addr,
				(unsigned long long)region.memory_size,
				(unsigned long long)region.userspace_addr);
		}

send_resp:
		if (ioctl(iso->notify_fd, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0) {
			if (errno == ENOENT) continue;
		}
	}
	return NULL;
}

int nvkvm_install_supervisor_start(struct VirtIONvgpu *nv,
				    struct nvkvm_isolate *iso)
{
	(void)nv;
	if (iso->notify_fd < 0) return -EBADF;
	pthread_mutex_init(&iso->install_lock, NULL);
	iso->install_head = NULL;
	iso->next_slot    = NVKVM_INSTALL_SLOT_BASE;
	/* Block all signals while spawning so the new thread inherits a
	 * fully-blocked mask.  This is essential because QEMU directs timer
	 * and IPI signals to specific threads; any thread that absorbs them
	 * causes the I/O thread to miss timer events. */
	sigset_t allsigs, oldmask;
	sigfillset(&allsigs);
	pthread_sigmask(SIG_SETMASK, &allsigs, &oldmask);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&iso->notify_tid, &attr, supervisor_thread, iso);
	pthread_attr_destroy(&attr);
	pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
	if (rc) return -rc;
	iso->notify_started = true;
	return 0;
}

void nvkvm_install_supervisor_stop(struct nvkvm_isolate *iso)
{
	if (!iso->notify_started) return;
	int fd = iso->notify_fd;
	iso->notify_fd = -1;
	if (fd >= 0) close(fd); /* wakes the RECV ioctl with -1/EBADF */
	/* Thread is detached; it cleans itself up after returning. */
	iso->notify_started = false;
	pthread_mutex_lock(&iso->install_lock);
	while (iso->install_head) {
		struct nvkvm_install_entry *e = iso->install_head;
		iso->install_head = e->next;
		free(e);
	}
	pthread_mutex_unlock(&iso->install_lock);
	pthread_mutex_destroy(&iso->install_lock);
}

/* ── Install / uninstall RPC handlers ────────────────────────────────────── */

/* Send ISOLATE_CMD_INSTALL_MAPPING and wait for ISOLATE_RESP_MAPPING.
 * Uses the isolate's sync slot like other non-IOCTL commands. */
static int send_install_cmd(struct nvkvm_isolate *iso,
			     uint32_t type,
			     const struct nvkvm_install_entry *e,
			     int *status_out)
{
	struct isolate_cmd_install_mapping cmd = {
		.type  = type,
		.slot  = e->slot,
		.gva   = e->gva,
		.size  = e->size,
		.gpa   = e->gpa,
		.prot  = e->prot,
	};
	pthread_mutex_lock(&iso->sync_lock);
	iso->sync_done = false;
	pthread_mutex_lock(&iso->write_lock);
	ssize_t n = send(iso->sock_fd, &cmd, sizeof(cmd), 0);
	pthread_mutex_unlock(&iso->write_lock);
	if (n != (ssize_t)sizeof(cmd)) {
		pthread_mutex_unlock(&iso->sync_lock);
		return -EIO;
	}
	while (!iso->sync_done)
		pthread_cond_wait(&iso->sync_cond, &iso->sync_lock);
	*status_out = iso->sync_error;
	pthread_mutex_unlock(&iso->sync_lock);
	return 0;
}

int nvkvm_install_isolate_mapping(struct VirtIONvgpu *nv,
				   uint32_t session_id,
				   uint32_t isolate_id,
				   uint64_t gva,
				   uint64_t size,
				   uint64_t gpa_target,
				   uint32_t prot,
				   uint64_t *gpa_out)
{
	(void)session_id;
	*gpa_out = 0;

	struct nvkvm_isolate *iso = find_isolate(nv, isolate_id);
	if (!iso) return -ENOENT;
	if (iso->notify_fd < 0) return -ENOTSUP;

	if (!size || (size & 0xFFFULL)) return -EINVAL;
	if (gva & 0xFFFULL)             return -EINVAL;

	/* Idempotency check */
	pthread_mutex_lock(&iso->install_lock);
	struct nvkvm_install_entry *existing = find_install_entry(iso, gva, size);
	if (existing) {
		*gpa_out = existing->gpa;
		pthread_mutex_unlock(&iso->install_lock);
		return 0;
	}

	/* Allocate GPA */
	uint64_t gpa = gpa_target;
	if (!gpa) {
		nvkvm_mmap_win_alloc(nv, (size_t)size, &gpa);
		if (!gpa) {
			pthread_mutex_unlock(&iso->install_lock);
			return -ENOSPC;
		}
	} else {
		if (gpa < nv->mmap_win_gpa ||
		    gpa + size > nv->mmap_win_gpa + nv->mmap_win_size) {
			pthread_mutex_unlock(&iso->install_lock);
			return -EINVAL;
		}
	}

	struct nvkvm_install_entry *e = calloc(1, sizeof(*e));
	if (!e) {
		pthread_mutex_unlock(&iso->install_lock);
		return -ENOMEM;
	}
	e->slot    = iso->next_slot++;
	e->gva     = gva;
	e->size    = size;
	e->gpa     = gpa;
	e->prot    = prot;
	e->pending = true;
	e->next    = iso->install_head;
	iso->install_head = e;
	pthread_mutex_unlock(&iso->install_lock);

	/* Tell the stub to call KVM_SET_USER_MEMORY_REGION. The seccomp
	 * supervisor will revalidate before the kernel commits. */
	int status = 0;
	int rc = send_install_cmd(iso, ISOLATE_CMD_INSTALL_MAPPING, e, &status);
	if (rc || status) {
		pthread_mutex_lock(&iso->install_lock);
		free_install_entry(iso, e);
		pthread_mutex_unlock(&iso->install_lock);
		return rc ? rc : status;
	}

	*gpa_out = gpa;
	return 0;
}

int nvkvm_uninstall_isolate_mapping(struct VirtIONvgpu *nv,
				     uint32_t session_id,
				     uint32_t isolate_id,
				     uint64_t gva,
				     uint64_t size)
{
	(void)session_id;
	struct nvkvm_isolate *iso = find_isolate(nv, isolate_id);
	if (!iso) return -ENOENT;
	if (iso->notify_fd < 0) return -ENOTSUP;

	pthread_mutex_lock(&iso->install_lock);
	struct nvkvm_install_entry *e = find_install_entry(iso, gva, size);
	if (!e) {
		pthread_mutex_unlock(&iso->install_lock);
		return -ENOENT;
	}
	/* Mark it pending again with size=0 — seccomp re-validates against
	 * the same slot/gva with zero size (slot-removal semantics). */
	uint32_t old_size = e->size;
	e->size = 0;
	e->pending = true;
	e->consumed = false;
	pthread_mutex_unlock(&iso->install_lock);

	int status = 0;
	int rc = send_install_cmd(iso, ISOLATE_CMD_UNINSTALL_MAPPING, e, &status);

	pthread_mutex_lock(&iso->install_lock);
	if (rc == 0 && status == 0) {
		free_install_entry(iso, e);
	} else {
		/* Roll back the size change. */
		e->size = old_size;
		e->pending = false;
		e->consumed = true;
	}
	pthread_mutex_unlock(&iso->install_lock);
	return (rc || status) ? (rc ? rc : status) : 0;
}
