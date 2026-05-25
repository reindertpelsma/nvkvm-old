// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_session.c — per-process session management
 *
 * A session groups all /dev/nvidia* file descriptors belonging to a single
 * guest process (identified by tgid). This mirrors gVisor's model where each
 * container has its own nvproxy instance with isolated client/object tables.
 *
 * Sessions are reference-counted. The last fd close drops the session, which
 * triggers host-side cleanup of all RM objects associated with the session's
 * clients.
 *
 * Isolation: each session gets a unique session_id sent with every ioctl
 * request. The QEMU backend keeps per-session fd and object tables, ensuring
 * that RM handles from one session cannot be used by another (the host
 * validates session_id against fd_token ownership on every request).
 */

#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>

#include "nvkvm.h"

struct nvkvm_session *nvkvm_session_get_or_create(pid_t tgid)
{
	struct nvkvm_session *session = NULL;
	int id;

	mutex_lock(&nvkvm.sessions_lock);

	/* Search existing sessions for this tgid */
	idr_for_each_entry(&nvkvm.sessions_idr, session, id) {
		if (session->tgid == tgid) {
			session->refcount++;
			mutex_unlock(&nvkvm.sessions_lock);
			return session;
		}
	}

	/* Allocate a new session */
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		mutex_unlock(&nvkvm.sessions_lock);
		return ERR_PTR(-ENOMEM);
	}
	session->tgid       = tgid;
	session->refcount   = 1;
	session->isolate_id = 0;
	mutex_init(&session->isolate_lock);

	id = idr_alloc(&nvkvm.sessions_idr, session, 1, 0, GFP_KERNEL);
	if (id < 0) {
		kfree(session);
		mutex_unlock(&nvkvm.sessions_lock);
		return ERR_PTR(id);
	}
	session->id = id;

	mutex_unlock(&nvkvm.sessions_lock);
	return session;
}

void nvkvm_session_put(struct nvkvm_session *session)
{
	bool last;
	__u32 isolate_id = 0;

	mutex_lock(&nvkvm.sessions_lock);
	last = --session->refcount == 0;
	if (last) {
		idr_remove(&nvkvm.sessions_idr, session->id);
		isolate_id = session->isolate_id;
		session->isolate_id = 0;
	}
	mutex_unlock(&nvkvm.sessions_lock);

	if (last) {
		/* Kill the isolate process before freeing the session struct. */
		if (isolate_id)
			nvkvm_virtio_kill_isolate(isolate_id);
		mutex_destroy(&session->isolate_lock);
		kfree(session);
	}
}
