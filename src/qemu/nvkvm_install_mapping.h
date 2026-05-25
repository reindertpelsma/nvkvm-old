/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_install_mapping.h — guest-driven KVM memory region installs.
 *
 * The guest kernel intercepts UVM/RM ioctls that create mappings in the
 * isolate's mm and sends install_isolate_mapping RPCs. QEMU validates,
 * picks a slot+GPA, records the call in a per-isolate whitelist, and
 * tells the stub to call KVM_SET_USER_MEMORY_REGION. The stub's seccomp
 * filter traps that ioctl and routes it back through this file's
 * supervisor thread for a final re-validation before the kernel commits
 * the call.
 *
 * See docs/INSTALL_ISOLATE_MAPPING.md for the full design.
 */

#ifndef NVKVM_INSTALL_MAPPING_H
#define NVKVM_INSTALL_MAPPING_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

struct VirtIONvgpu;
struct nvkvm_isolate;

/*
 * One pre-authorised KVM_SET_USER_MEMORY_REGION call. The supervisor
 * thread for an isolate matches the trapped syscall's args against the
 * list and decides whether to CONTINUE or reject.
 *
 * Lives on the per-isolate install_head linked list (see
 * struct nvkvm_isolate::install_head).
 */
struct nvkvm_install_entry {
	uint32_t slot;        /* KVM memory slot number */
	uint32_t flags;       /* KVM_MEM_READONLY etc */
	uint64_t gva;         /* userspace_addr in the stub's mm */
	uint64_t size;        /* 0 = uninstall (slot removal) */
	uint64_t gpa;         /* guest physical address */
	uint32_t prot;        /* PROT_READ|PROT_WRITE */
	uint32_t reserved;

	/* Inflight bookkeeping — set when the stub's syscall is being
	 * supervised; cleared when the syscall completes. */
	bool     pending;
	bool     consumed;    /* set after the syscall completes; safe to free */

	struct nvkvm_install_entry *next;
};

/* Spawn the per-isolate seccomp USER_NOTIF supervisor thread.
 * Called once notify_fd has been received via SCM_RIGHTS from the stub. */
int nvkvm_install_supervisor_start(struct VirtIONvgpu *nv,
				    struct nvkvm_isolate *iso);

/* Stop the supervisor (closes notify_fd, joins thread). */
void nvkvm_install_supervisor_stop(struct nvkvm_isolate *iso);

/*
 * Install handler — called from the virtio backend on
 * NVKVM_REQ_INSTALL_ISOLATE_MAPPING. Picks a slot+GPA, records the
 * whitelist entry, sends ISOLATE_CMD_INSTALL_MAPPING to the stub,
 * waits for the stub's ISOLATE_RESP_MAPPING, returns 0 on success
 * or -errno on failure. On success, *gpa_out = chosen GPA.
 *
 * Idempotent on (isolate_id, gva, size). Re-sends return the same GPA
 * without double-installing.
 */
int nvkvm_install_isolate_mapping(struct VirtIONvgpu *nv,
				   uint32_t session_id,
				   uint32_t isolate_id,
				   uint64_t gva,
				   uint64_t size,
				   uint64_t gpa_target,
				   uint32_t prot,
				   uint64_t *gpa_out);

int nvkvm_uninstall_isolate_mapping(struct VirtIONvgpu *nv,
				     uint32_t session_id,
				     uint32_t isolate_id,
				     uint64_t gva,
				     uint64_t size);

#endif /* NVKVM_INSTALL_MAPPING_H */
