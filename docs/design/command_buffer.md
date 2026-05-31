# Command Buffer — SPSC-Ring Fast Path (build spec)

Status: SPEC (2026-05-31). Supersedes the linked-list sketch in
`perf_findings.md`. Rationale and measurements: `perf_findings.md`,
memory `decode_ioctl_profile`.

## Why

Decode is ~884 µs/ioctl, and 89% of that is the **four sleeping-thread wakeups**
in the QEMU↔isolate round-trip (stub receiver, stub worker, QEMU reader, QEMU
worker), NOT the wire (~30 µs) or the host ioctl (~7 µs). The fix is to replace
trap-and-defer with **poll-and-handle-inline**: a per-isolate shared-memory ring
the isolate spin-thread reads directly, runs the ioctl inline, and writes back —
no socketpair, no thread handoffs. `work` collapses toward the ~10 µs floor →
decode approaches host rate (~368 t/s; ~15× single-stream). Ceiling = host rate
(the isolate still runs the same ~36 ioctls/token).

## Core principle: the ring is a TRANSPORT, never a hold buffer

The isolate consumer reads a command and **immediately copies it into private
memory**, freeing the ring slot in µs. The (possibly long) ioctl then executes
from the private copy. Consequences:
- **No head-of-line blocking by slow ioctls** — a 3 s ioctl does not occupy a
  ring slot for 3 s; the slot is freed on copy-out.
- **No in-flight tampering** — the isolate never re-reads guest-writable memory
  mid-operation (this is audit P2-2 made structural).
So *every* isolate-serviceable ioctl rides the ring regardless of its duration.

## Model

Two **SPSC byte-rings per isolate** + the existing **virtqueue** as control
channel:
- **request ring**: producer = guest (untrusted), consumer = isolate (trusted)
- **response ring**: producer = isolate (trusted), consumer = guest (untrusted)
- **virtqueue** keeps: ring setup, **grow/resize** handshake, **sync/doorbell**,
  and the ioctls the isolate cannot service alone (see classification).

SPSC needs **no mutex, no double-guard, no ABA reasoning**: producer owns `tail`,
consumer owns `head`; each only acquire-reads the other's counter, and a stale
read is always conservatively safe.

### Ring mechanics
- Region size `N` (pow2). Two cache-line-separated **free-running** counters
  `head`/`tail` (empty: `head==tail`; full: `tail-head==N` — unambiguous).
- Records are length-prefixed and aligned: `[u32 len][u32 type][payload]`,
  payload = ioctl request/response struct + data + **txn_id**. txn tracking
  sits one level above blocks, exactly as before.
- **Pad-to-end on wrap**: if a record would straddle `N`, emit a skip record so
  every record is contiguous (no split reads).
- Producer: free-space check (acquire other's counter) → write payload →
  **release-store** own counter `+= len`. Full ⇒ adaptive spin then block.
- Consumer: acquire other's counter; while not empty: read `len` at `head%N`,
  **bounds-check** (`len ≤ available`, `≤ N`, aligned) → **copy record to
  private memory** → validate (fast-path allowlist) → execute → **release-store
  `head += len`**.

## Isolate consumer loop (the decode fast path)

```
spin:
  while ring not empty:
     copy record out (private); bounds-checked
     validate command (per-cmd allowlist)
     if fast-class:  run stub_ioctl INLINE, write response to response ring
     else (slow):    hand the private copy to a worker; worker runs it and
                     writes the response when done   # ring keeps draining
  adaptive: spin a budget; if still empty, sleep (see sync/doorbell)
```

- **Fast ⇒ inline** = no thread handoff = the win (decode's RM_CONTROLs, ~7 µs).
- **Slow ⇒ worker** = the (now-negligible relative to a long op) handoff, so a
  slow isolate-serviceable ioctl (e.g. a blocking event-wait) never stalls the
  ring reader. The fast/slow split lives HERE (inline vs worker), fed by the
  per-cmd latency measurement — it is NOT a ring-vs-virtqueue split.

## Classification — what rides the ring

Ring = ioctls the isolate fully services with **no QEMU/KVM/fd mediation** and
(for inline) **bounded-fast**:
- **Ring**: RM_CONTROL (no-embedded-fd subset), RM_FREE (0x29), simple
  (non-memory, non-event) RM_ALLOC (0x2b). [≈ the 70% RM_CONTROL + frees +
  simple allocs of decode traffic]
- **Virtqueue (must)**: anything needing QEMU/KVM — RM_MAP_MEMORY (0x4e),
  RM_ALLOC_MEMORY/OS_DESCRIPTOR (0x27), MAP_MEMORY_DMA, UNMAP_*, **all UVM**
  (mm-bound), OPEN_DEVICE, REGISTER_FD, GET_PID_INFO (init-ns answer),
  EXPORT_OBJECT_TO_FD, ALLOC_OS_EVENT (embedded fd), NVKMS/DRM (graphics),
  plus control/grow/sync.
- The exact RM_CONTROL `cmd` subset (and inline-vs-worker per cmd) is set by the
  **per-cmd latency measurement** (instrument the stub around `stub_ioctl`,
  record count + avg/max latency per NVOS54 `cmd`; any control with high max or
  an embedded fd → off-ring or worker; the rest → ring/inline). Tightens
  `nvkvm_ctrl_allowlist.h`.

## Sync / drain protocol (lost-wakeup-safe)

The guest must know the isolate has seen every command before it sleeps:
- A **sync** command (on the virtqueue) reports the **last-completed txn_id** and
  **blocks until ≥1 cmd completes OR the request ring is empty**.
- Guest flow: publish first command → issue sync **async** (don't wait) → enter
  the big loop: publish new requests + read responses off the response ring in
  real time (pure ring, no virtqueue, the fast path). When the guest wants to
  stop, it reads/awaits the sync response: if `last-completed == last-published`
  → drained, exit; else reloop. Sync blocking is the only place the guest sleeps
  — off the hot path, so its virtqueue latency is irrelevant.
- This is the futex-compare-value pattern: compare last-completed vs
  last-published to close the lost-wakeup window. Isolate→guest sync is not
  needed (with no in-flight txns the isolate cannot add to the response ring).

## Security invariants

- **Copy-out before validate/use** (P2-2): never act on guest-writable memory.
- **Bounds-assert every access** (offset, `ptr+size`) to the ring extent; on
  violation the offending guest's **isolate is torn down and its ioctls error**
  (per-guest containment = equivalent to a guest SIGKILL; never cross-tenant,
  never the host). Corruption by an untrusted producer = **DoS-only**.
- **Forward progress / no infinite loop**: bound the consumer's walk to
  `N / min_record` per pass; `sched_yield` if a race-reloop spins too long.
- **acquire/release** on counters, sleep flag, record publish.
- **Per-isolate** rings + per-isolate spin-thread → a stalled/abused ring hurts
  only that guest; its virtqueue path keeps working.
- Offsets are **in-buffer**, never global pointers.

## Build phases

1. **Ring primitive + tests** (host-only unit test, SPSC, wrap, bounds, the
   adaptive spin/sleep doorbell, lost-wakeup soak). No GPU.
2. **Per-isolate ring setup over the virtqueue** (memfd mint → GPA install +
   SCM_RIGHTS to isolate; both map) + the grow handshake.
3. **Isolate spin-thread**: ring read → copy-out → validate → inline ioctl →
   response ring; integrate the sync/doorbell.
4. **Guest side**: ring producer/consumer + the sync-driven big loop; route
   fast-class ioctls to the ring, the rest to the virtqueue.
5. **Inline-vs-worker** dispatch in the isolate for slow isolate-serviceable
   ioctls; wire the per-cmd classification.
6. Measure decode end-to-end; expect `work` → ~10 µs, decode → toward host rate.

## Open items

- Run the per-cmd latency measurement to lock the ring control-cmd allowlist +
  inline-vs-worker thresholds (next step).
- Confirm which isolate-serviceable ioctls are genuinely slow (event-waits) so
  they take the worker path.
- Grow policy (when/by how much to resize a full ring) + back-pressure.
