/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stub_freestanding.h — no-libc runtime primitives for the nvkvm stub.
 *
 * The stub builds with -nostdlib -static -fPIE so its address-space layout
 * is a tiny, predictable blob that can be fexecve'd into a fresh process
 * with nothing inherited from QEMU.  This header replaces libc bits the
 * stub needs:
 *
 *   - syscall macros (no errno; negative return = -errno, kernel convention)
 *   - futex-based mutex / cond / once
 *   - raw clone3 thread spawn (no pthread)
 *   - tiny dprintf-style writer (for diagnostics)
 *
 * No TLS.  fault_addr tracking lives in a per-worker slot indexed by the
 * worker's own thread id stash, not glibc TLS.
 *
 * x86_64 only — aarch64 can be added later.
 */
#ifndef NVKVM_STUB_FREESTANDING_H
#define NVKVM_STUB_FREESTANDING_H

#include <stdint.h>
#include <stddef.h>
#include <linux/futex.h>

/* ── Inline syscalls — kernel convention: negative return = -errno ──────── */

static inline long sc0(long n)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n) : "rcx", "r11", "memory");
	return r;
}
static inline long sc1(long n, long a)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}
static inline long sc2(long n, long a, long b)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
	return r;
}
static inline long sc3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc4(long n, long a, long b, long c, long d)
{
	long r;
	register long r10 __asm__("r10") = d;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc5(long n, long a, long b, long c, long d, long e)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
		: "rcx", "r11", "memory");
	return r;
}
static inline long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile ("syscall"
		: "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return r;
}

/* ── Futex-based mutex / cond ───────────────────────────────────────────── */

/* States: 0 = unlocked, 1 = locked uncontended, 2 = locked with waiters. */
struct fs_mutex { int v; };
#define FS_MUTEX_INIT { 0 }

static inline void fs_mutex_lock(struct fs_mutex *m)
{
	int c;
	c = __atomic_load_n(&m->v, __ATOMIC_RELAXED);
	if (c == 0 &&
	    __atomic_compare_exchange_n(&m->v, &c, 1, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	while (1) {
		int prev = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
		if (prev == 0)
			return;
		sc6(202 /*SYS_futex*/, (long)&m->v, FUTEX_WAIT_PRIVATE,
		    2, 0, 0, 0);
	}
}

static inline void fs_mutex_unlock(struct fs_mutex *m)
{
	int prev = __atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE);
	if (prev == 2)
		sc6(202 /*SYS_futex*/, (long)&m->v, FUTEX_WAKE_PRIVATE,
		    1, 0, 0, 0);
}

/* Generation-counter condvar.  Waiter samples gen, drops mutex, futex_waits
 * for gen change, reacquires mutex.  Broadcast: increment gen, wake all. */
struct fs_cond { uint32_t gen; };
#define FS_COND_INIT { 0 }

static inline void fs_cond_wait(struct fs_cond *c, struct fs_mutex *m)
{
	uint32_t gen = __atomic_load_n(&c->gen, __ATOMIC_RELAXED);
	fs_mutex_unlock(m);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAIT_PRIVATE,
	    (long)gen, 0, 0, 0);
	fs_mutex_lock(m);
}

static inline void fs_cond_signal(struct fs_cond *c)
{
	__atomic_fetch_add(&c->gen, 1, __ATOMIC_RELEASE);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAKE_PRIVATE,
	    1, 0, 0, 0);
}

static inline void fs_cond_broadcast(struct fs_cond *c)
{
	__atomic_fetch_add(&c->gen, 1, __ATOMIC_RELEASE);
	sc6(202 /*SYS_futex*/, (long)&c->gen, FUTEX_WAKE_PRIVATE,
	    0x7fffffff, 0, 0, 0);
}

/* One-shot init via atomic state machine. */
struct fs_once { int v; };
#define FS_ONCE_INIT { 0 }

static inline void fs_once(struct fs_once *o, void (*fn)(void))
{
	int expected = 0;
	if (__atomic_compare_exchange_n(&o->v, &expected, 1, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
		fn();
		__atomic_store_n(&o->v, 2, __ATOMIC_RELEASE);
		sc6(202 /*SYS_futex*/, (long)&o->v, FUTEX_WAKE_PRIVATE,
		    0x7fffffff, 0, 0, 0);
		return;
	}
	while (__atomic_load_n(&o->v, __ATOMIC_ACQUIRE) != 2)
		sc6(202 /*SYS_futex*/, (long)&o->v, FUTEX_WAIT_PRIVATE,
		    1, 0, 0, 0);
}

/* ── Worker thread spawn via clone3 ─────────────────────────────────────── */

struct clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
};

/* SYS_clone3 = 435 on x86_64; stack provided by caller. */
#define FS_CLONE3_NR 435

#endif /* NVKVM_STUB_FREESTANDING_H */
