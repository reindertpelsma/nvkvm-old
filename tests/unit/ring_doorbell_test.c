/* ring_doorbell_test.c — Phase 3 doorbell of the command buffer.
 *
 * Proves the adaptive spin → FUTEX sleep → wake protocol that lets the isolate
 * consumer block when idle (user constraint: NOT a 100%-CPU spin loop) while
 * never dropping a wakeup.  Two threads in one process exercise the exact
 * production logic; the only thing the real system adds is routing the wake
 * through QEMU (guest kick → QEMU FUTEX_WAKE) — the futex semantics are
 * identical, so the lost-wakeup proof carries over.
 *
 *   producer : reserve+commit a record, then take_wake() + FUTEX_WAKE if the
 *              consumer was asleep.  Two modes: "gappy" (idle gaps that force
 *              real sleeps) and "hammer" (back-to-back, exercises the
 *              presleep/commit race and the spin path).
 *   consumer : spin a budget; on an empty ring, presleep() then FUTEX_WAIT;
 *              drain everything, in order, no loss.
 *
 * A watchdog alarm fails loudly instead of hanging if a wakeup is ever lost.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#include "nvkvm_ring.h"

#define N (1u << 16)

static int futex_wait(uint32_t *w, uint32_t expect)
{
	return (int)syscall(SYS_futex, w, FUTEX_WAIT, expect, NULL, NULL, 0);
}
static int futex_wake(uint32_t *w)
{
	return (int)syscall(SYS_futex, w, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* Tight spin hint — the consumer busy-waits a budget before sleeping.  Must be
 * sub-microsecond per iteration (NOT sched_yield, which can context-switch for
 * tens of µs and span an idle gap, hiding the sleep path). */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static struct nvkvm_ring *ring_new(void)
{
	struct nvkvm_ring *r = aligned_alloc(64, sizeof(*r) + N);
	memset(r, 0, sizeof(*r) + N);
	r->size = N;
	return r;
}

struct ctx {
	struct nvkvm_ring *r;
	uint32_t           total;     /* records to send */
	int                gappy;     /* producer inserts idle gaps */
	/* results */
	uint64_t           processed;
	uint64_t           blocking_sleeps;  /* consumer FUTEX_WAITs that blocked */
	int                order_ok;
	int                wake_calls;        /* producer FUTEX_WAKEs issued */
};

static void tiny_pause(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };  /* 2 ms */
	nanosleep(&ts, NULL);
}

static void *producer_fn(void *arg)
{
	struct ctx *c = arg;
	for (uint32_t i = 0; i < c->total; i++) {
		uint64_t total; void *p;
		while (!(p = nvkvm_ring_reserve(c->r, sizeof(uint32_t), &total)))
			sched_yield();
		memcpy(p, &i, sizeof(i));
		nvkvm_ring_commit(c->r, total);

		if (nvkvm_ring_producer_take_wake(c->r)) {
			c->wake_calls++;
			futex_wake(&c->r->consumer_sleeping);
		}
		/* Gaps force the consumer to actually reach FUTEX_WAIT.  Sprinkle,
		 * not every record, so we also hit the no-sleep fast path. */
		if (c->gappy && (i % 8) == 0)
			tiny_pause();
	}
	return NULL;
}

static void *consumer_fn(void *arg)
{
	struct ctx *c = arg;
	uint32_t expect = 0;
	c->order_ok = 1;

	while (c->processed < c->total) {
		uint8_t *pay; uint32_t len; uint64_t total;
		int spins = 2000;         /* tight busy-wait budget before sleeping */
		int rc;

		/* spin budget (tight relax-spin, sub-µs/iter) */
		while ((rc = nvkvm_ring_peek(c->r, &pay, &len, &total))
		       == NVKVM_RING_EMPTY && spins-- > 0)
			cpu_relax();

		if (rc == NVKVM_RING_OK) {
			uint32_t seq;
			memcpy(&seq, pay, sizeof(seq));
			if (seq != expect) c->order_ok = 0;
			expect++;
			nvkvm_ring_pop(c->r, total);
			c->processed++;
			continue;
		}
		if (rc == NVKVM_RING_BAD) {          /* never expected here */
			fprintf(stderr, "doorbell: peek BAD\n");
			return NULL;
		}
		/* empty after the spin budget → try to sleep */
		if (nvkvm_ring_consumer_presleep(c->r)) {
			int w = futex_wait(&c->r->consumer_sleeping,
					   NVKVM_RING_SLEEPING);
			if (w == 0)
				c->blocking_sleeps++;   /* genuinely blocked+woken */
			/* EAGAIN (-1/errno) = value already AWAKE = no lost wakeup */
			nvkvm_ring_consumer_wake_self(c->r);
		}
	}
	return NULL;
}

static int run(const char *name, uint32_t total, int gappy, int want_sleeps)
{
	struct ctx c;
	memset(&c, 0, sizeof(c));
	c.r = ring_new();
	c.total = total;
	c.gappy = gappy;

	pthread_t pt, ct;
	pthread_create(&ct, NULL, consumer_fn, &c);
	pthread_create(&pt, NULL, producer_fn, &c);
	pthread_join(pt, NULL);
	pthread_join(ct, NULL);

	int fail = 0;
	if (c.processed != total) {
		printf("%-9s: FAIL — processed %llu/%u\n", name,
		       (unsigned long long)c.processed, total); fail = 1;
	}
	if (!c.order_ok) {
		printf("%-9s: FAIL — out-of-order delivery\n", name); fail = 1;
	}
	if (want_sleeps && c.blocking_sleeps == 0) {
		printf("%-9s: FAIL — consumer never blocked (would be 100%% spin)\n",
		       name); fail = 1;
	}
	if (!fail)
		printf("%-9s: %u recs OK, %llu real sleeps, %d wakes (order OK)\n",
		       name, total,
		       (unsigned long long)c.blocking_sleeps, c.wake_calls);
	free(c.r);
	return fail;
}

static void on_alarm(int sig)
{
	(void)sig;
	const char *m = "ring doorbell: WATCHDOG — wakeup lost (hang)\nRING DOORBELL TEST: FAIL\n";
	if (write(2, m, strlen(m)) < 0) { /* ignore */ }
	_exit(2);
}

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(60);                         /* fail loudly instead of hanging */

	int fail = 0;
	/* gappy: forces real FUTEX sleeps and tests the wake path */
	fail |= run("gappy",    2000, 1, /*want_sleeps=*/1);
	/* hammer: back-to-back, exercises the presleep/commit race + spin path */
	fail |= run("hammer", 2000000, 0, /*want_sleeps=*/0);
	/* tiny: ring much larger than traffic, mostly-idle consumer */
	fail |= run("idle",       50, 1, /*want_sleeps=*/1);

	printf(fail ? "RING DOORBELL TEST: FAIL\n" : "RING DOORBELL TEST: PASS\n");
	return fail;
}
