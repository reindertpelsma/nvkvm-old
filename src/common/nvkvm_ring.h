/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvkvm_ring.h — single-producer / single-consumer byte ring for the
 * command-buffer fast path (docs/design/command_buffer.md).
 *
 * Header-only; the SAME definitions are used by the guest kernel module, the
 * freestanding isolate stub, and the host unit test. Lock-free SPSC across a
 * shared mmap: the producer owns `tail`, the consumer owns `head`, and the
 * only cross-domain synchronisation is one release-store of `tail` on commit
 * paired with one acquire-load on peek (and symmetrically for `head`). No
 * mutex, no per-record atomics, no ABA — single producer, single consumer.
 *
 * TRUST: the consumer treats the ring as HOSTILE (the producer may be the
 * untrusted guest). Every record length is bounds-checked against the ring
 * extent; on ANY violation the consumer returns a negative code and the caller
 * tears the offending isolate down (per-guest, DoS-only — never OOB).
 *
 * The includer must provide uint32_t / uint64_t / uint8_t (stdint.h on the
 * host; linux/types.h in the kernel; the stub's freestanding header).
 */
#ifndef NVKVM_RING_H
#define NVKVM_RING_H

#define NVKVM_RING_ALIGN     8u
#define NVKVM_RING_REC_SKIP  0u   /* wrap padding; consumer advances past it   */
#define NVKVM_RING_REC_DATA  1u   /* a command/response record                 */

/* peek() result codes */
#define NVKVM_RING_EMPTY      0
#define NVKVM_RING_OK         1
#define NVKVM_RING_BAD      (-1)  /* malformed ring → caller must tear down     */

/* Record header; payload follows immediately. `len` = total record bytes
 * (header + payload) rounded up to NVKVM_RING_ALIGN. */
struct nvkvm_ring_rec {
	uint32_t len;
	uint32_t type;
	/* uint8_t payload[]; */
};

/*
 * Shared control block at the base of the mmap; the N-byte data region follows
 * immediately after (NVKVM_RING_DATA). Counters are on separate 64-byte lines
 * to avoid false sharing between producer and consumer.
 */
struct nvkvm_ring {
	uint64_t size;                 /* N: power of two, immutable after init   */
	uint64_t _pad0[7];
	uint64_t tail;                 /* producer-owned, free-running byte count  */
	uint64_t _pad1[7];
	uint64_t head;                 /* consumer-owned, free-running byte count  */
	uint64_t _pad2[7];
	uint32_t consumer_sleeping;    /* doorbell: set by consumer before block   */
	uint32_t _pad3[15];
	/* data region (size bytes) begins here */
};

#define NVKVM_RING_DATA(r) ((uint8_t *)((struct nvkvm_ring *)(r) + 1))

/* ── Ring-pair region layout (command-buffer transport) ───────────────────
 * QEMU mints one memfd holding two back-to-back [control + data] rings:
 *   request ring  (guest→isolate) at offset 0
 *   response ring (isolate→guest) at nvkvm_ring_resp_off(ring_bytes)
 * Each ring's data region immediately follows its own control block, so the
 * SAME producer/consumer helpers work on an nvkvm_ring* aimed at either
 * offset (NVKVM_RING_DATA does the right thing for both).
 */
#define NVKVM_RING_DEFAULT_BYTES (64u * 1024u)   /* per-ring data bytes */

static inline uint64_t nvkvm_ring_resp_off(uint32_t ring_bytes)
{
	return (uint64_t)sizeof(struct nvkvm_ring) + ring_bytes;
}

static inline uint64_t nvkvm_ring_region_size(uint32_t ring_bytes)
{
	return 2ull * ((uint64_t)sizeof(struct nvkvm_ring) + ring_bytes);
}

static inline uint32_t nvkvm_ring_roundup(uint32_t x)
{
	return (x + (NVKVM_RING_ALIGN - 1)) & ~(NVKVM_RING_ALIGN - 1);
}

/* True iff N is a non-zero power of two (required for the &(N-1) masking). */
static inline int nvkvm_ring_size_ok(uint64_t n)
{
	return n >= 64 && (n & (n - 1)) == 0;
}

/* ── Producer (single thread) ─────────────────────────────────────────────
 * reserve() returns a pointer to write `payload` bytes into, or NULL if the
 * ring is full (caller retries / spins / blocks). It writes record header(s)
 * but does NOT publish; *out_total receives the bytes this reservation will
 * consume (including any wrap-skip), which the matching commit() publishes.
 */
static inline void *nvkvm_ring_reserve(struct nvkvm_ring *r, uint32_t payload,
				       uint64_t *out_total)
{
	uint32_t rec = nvkvm_ring_roundup(
		(uint32_t)sizeof(struct nvkvm_ring_rec) + payload);
	uint64_t N    = r->size;
	uint64_t tail = r->tail;   /* producer-owned: plain read */
	uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
	uint64_t used = tail - head;
	uint64_t off  = tail & (N - 1);
	uint64_t to_end = N - off;
	uint8_t *data = NVKVM_RING_DATA(r);
	struct nvkvm_ring_rec *h;

	if (rec > N || payload > N)            /* never fits, or overflow guard */
		return NULL;

	if (rec <= to_end) {
		if (N - used < rec)            /* full */
			return NULL;
		h = (struct nvkvm_ring_rec *)(data + off);
		h->len = rec;
		h->type = NVKVM_RING_REC_DATA; /* finalised by commit's release */
		*out_total = rec;
		return (uint8_t *)h + sizeof(*h);
	}
	/* Record would straddle the wrap → pad [off, N) with a skip, place the
	 * real record at offset 0. to_end is a multiple of ALIGN and >= 8 (tail
	 * is always ALIGN-aligned and N is a power of two), so the skip header
	 * always fits. */
	if (N - used < to_end + rec)           /* full */
		return NULL;
	{
		struct nvkvm_ring_rec *skip = (struct nvkvm_ring_rec *)(data + off);
		skip->len  = (uint32_t)to_end;
		skip->type = NVKVM_RING_REC_SKIP;
	}
	h = (struct nvkvm_ring_rec *)(data + 0);
	h->len = rec;
	h->type = NVKVM_RING_REC_DATA;
	*out_total = to_end + rec;
	return (uint8_t *)h + sizeof(*h);
}

/* Publish the reservation. The single release-store of tail is the barrier
 * that makes ALL prior record writes visible to the consumer's acquire-load. */
static inline void nvkvm_ring_commit(struct nvkvm_ring *r, uint64_t total)
{
	__atomic_store_n(&r->tail, r->tail + total, __ATOMIC_RELEASE);
}

/* ── Consumer (single thread) ─────────────────────────────────────────────
 * peek() validates the next record against the ring extent and returns its
 * payload pointer + length, advancing past wrap-skips. Returns NVKVM_RING_OK
 * (record available), NVKVM_RING_EMPTY, or NVKVM_RING_BAD (malformed → tear
 * down the isolate). On OK, *out_pay / *out_len describe the payload and
 * *out_total is what pop() must consume. The consumer must COPY the payload
 * into private memory before acting on it (it lives in producer-writable mem).
 */
static inline int nvkvm_ring_peek(struct nvkvm_ring *r, uint8_t **out_pay,
				  uint32_t *out_len, uint64_t *out_total)
{
	uint64_t N = r->size;
	uint8_t *data = NVKVM_RING_DATA(r);

	for (;;) {
		uint64_t head = r->head;                       /* consumer-owned */
		uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
		uint64_t avail = tail - head;
		uint64_t off;
		struct nvkvm_ring_rec *h;
		uint32_t len, type;

		if (avail == 0)
			return NVKVM_RING_EMPTY;
		if (avail < sizeof(struct nvkvm_ring_rec) || avail > N)
			return NVKVM_RING_BAD;     /* partial / impossible */

		off = head & (N - 1);
		h   = (struct nvkvm_ring_rec *)(data + off);
		len  = h->len;
		type = h->type;

		/* Bounds: header+payload must lie wholly inside the data region,
		 * be aligned, fit the available bytes, and make forward progress. */
		if (len < sizeof(struct nvkvm_ring_rec) || (len & (NVKVM_RING_ALIGN - 1)) ||
		    len > avail || len > N || off + len > N)
			return NVKVM_RING_BAD;

		if (type == NVKVM_RING_REC_SKIP) {
			/* consume the wrap padding in place and look again */
			__atomic_store_n(&r->head, head + len, __ATOMIC_RELEASE);
			continue;
		}
		if (type != NVKVM_RING_REC_DATA)
			return NVKVM_RING_BAD;

		*out_pay   = (uint8_t *)h + sizeof(*h);
		*out_len   = len - (uint32_t)sizeof(*h);
		*out_total = len;              /* skips already advanced above */
		return NVKVM_RING_OK;
	}
}

/* Release the data record. Call AFTER copy-out. `total` is the value peek()
 * returned in *out_total (the data record's len; any wrap-skips were already
 * advanced inside peek). */
static inline void nvkvm_ring_pop(struct nvkvm_ring *r, uint64_t total)
{
	__atomic_store_n(&r->head, r->head + total, __ATOMIC_RELEASE);
}

/* ── Doorbell ─────────────────────────────────────────────────────────────
 * The consumer (isolate) spins a budget on an empty ring (the decode hot path:
 * no syscall, no VM exit), and only sleeps when idle.  Sleeping is published
 * by storing 1 to `consumer_sleeping`; the consumer FUTEX_WAITs on that word.
 * The wake is asymmetric: a guest producer cannot FUTEX_WAKE a host thread, so
 * it KICKS the virtqueue and QEMU does the FUTEX_WAKE on the shared word.
 *
 * The two helpers below are the PORTABLE half (plain atomics, no futex) so the
 * guest kernel, QEMU and the host test all share them.  The actual
 * FUTEX_WAIT/FUTEX_WAKE stay in the respective C files (raw syscall in the
 * freestanding stub, glibc/QEMU elsewhere) because the syscall surface differs.
 *
 * consumer_sleeping states: 0 = awake/spinning, 1 = asleep (wants a wake).
 */
#define NVKVM_RING_AWAKE    0u
#define NVKVM_RING_SLEEPING 1u

/*
 * Producer side (guest / QEMU): call AFTER commit().  Atomically clears the
 * sleeping flag and returns non-zero iff the consumer had published that it is
 * asleep — in which case the caller must wake it (kick the virtqueue → QEMU
 * FUTEX_WAKEs the shared word).
 *
 * Clearing the flag BEFORE the wake syscall is what makes the wake un-loseable:
 * the consumer's FUTEX_WAIT is issued with the expected value SLEEPING, so if
 * this exchange runs in the gap between the consumer's presleep store and its
 * FUTEX_WAIT, the kernel sees the word is now AWAKE and FUTEX_WAIT returns
 * immediately (EAGAIN) instead of blocking on a wake that already fired.
 */
static inline int nvkvm_ring_producer_take_wake(struct nvkvm_ring *r)
{
	return __atomic_exchange_n(&r->consumer_sleeping, NVKVM_RING_AWAKE,
				   __ATOMIC_SEQ_CST) == NVKVM_RING_SLEEPING;
}

/*
 * Consumer side: publish the intent to sleep, then RE-CHECK for work before
 * actually blocking.  Returns non-zero if the caller should FUTEX_WAIT on
 * `consumer_sleeping` with expected value NVKVM_RING_SLEEPING (ring still empty
 * after publishing); zero if a record raced in (caller keeps draining; the flag
 * is already cleared back to AWAKE).
 *
 * Ordering (lost-wakeup-free): the SEQ_CST store of SLEEPING is followed by a
 * SEQ_CST load of the producer's `tail`.  A producer that committed before our
 * tail-load is seen here (tail != head → we abort the sleep); a producer that
 * commits after our tail-load necessarily observes our SLEEPING store on its
 * take_wake exchange (→ it wakes us).  No interleaving drops the wakeup.
 */
static inline int nvkvm_ring_consumer_presleep(struct nvkvm_ring *r)
{
	__atomic_store_n(&r->consumer_sleeping, NVKVM_RING_SLEEPING,
			 __ATOMIC_SEQ_CST);
	uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_SEQ_CST);
	uint64_t head = r->head;                  /* consumer-owned */
	if (tail != head) {                       /* work raced in — don't sleep */
		__atomic_store_n(&r->consumer_sleeping, NVKVM_RING_AWAKE,
				 __ATOMIC_RELEASE);
		return 0;
	}
	return 1;
}

/* Consumer side: clear the sleeping flag (after a spurious FUTEX wake). */
static inline void nvkvm_ring_consumer_wake_self(struct nvkvm_ring *r)
{
	__atomic_store_n(&r->consumer_sleeping, NVKVM_RING_AWAKE, __ATOMIC_RELEASE);
}

#endif /* NVKVM_RING_H */
