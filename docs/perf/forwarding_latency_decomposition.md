# Forwarding latency decomposition (2026-05-31, vast.ai RTX 3060, 580.159.04)

Question: which syscalls are slower than host, and **is it syscalls at all?**
Method: `LD_PRELOAD` shim (`tests/integration/syscall_prof.c`) timing each libc-
wrapped syscall with vDSO `clock_gettime` (~20 ns overhead, no ptrace), plus
`/usr/bin/time -v` (counts ALL kernel time incl. asm-inline syscalls), plus
one-shot in-tree timers in the stub (rdtsc around the real nvidia ioctl) and in
QEMU (clock around the stub round-trip).  Workload: `gpu_bench` (ring OFF).

## Headline: it is NOT the syscalls — it is the per-ioctl round-trip

- **Compute is unaffected**: GEMM throughput 421.6 (guest) vs 422.9 (host) GFLOP/s.
- **The NVIDIA ioctl in the stub ≈ host native** (same driver, same speed):
  RM_ALLOC 105 µs (host 100), RM_CONTROL 229 µs (host 214), RM_FREE 75 µs,
  MAP 69 µs.  The driver is not slower under forwarding.
- **Each forwarded ioctl pays a ~1–1.6 ms round-trip**, roughly constant
  regardless of the underlying call's native cost.

### Layer decomposition (per RM_ALLOC, exemplar)

| layer | latency | share |
|---|---|---|
| NVIDIA driver call (in stub) | ~105 µs | ~7 % |
| + QEMU↔stub socket/worker round-trip | +277 µs (→382 µs) | ~17 % |
| + guest↔QEMU (virtio + thread-pool offload + wakeups/sched) | +1233 µs (→1615 µs) | ~76 % |

The dominant cost (~76 %) is the **guest↔QEMU hop**: VM-exit on the virtqueue
kick, the IOCTL_ON_ISOLATE thread-pool offload, the completion bounce back to the
device AioContext, and the interrupt that wakes the blocked guest vCPU — a chain
of ~4–5 scheduler wakeups, each ~100s of µs on this oversubscribed shared host.
`/usr/bin/time -v` corroborates: guest **9939 voluntary context switches vs 48 on
host** (~200×) — the workload is dominated by *blocking handoffs*, not CPU-in-
syscall.  Guest System time 5.4 s / wall 10.2 s with ~4.5 s purely blocked.

## So "is it syscalls?"

No.  The syscall (the NVIDIA ioctl) is cheap and the same speed as host.  The tax
is the **multi-hop blocking round-trip** to forward each ioctl, ~76 % of it in the
guest↔QEMU layer (scheduling/wakeup latency, amplified by host oversubscription).

## Why this matters per workload

- **Decode/compute throughput**: unaffected — the hot launch/sync path is mapped
  doorbell/fence memory (zero forwarded ioctls), so it never pays the round-trip.
  Launch RTT is only +47 µs (the few control ioctls), alloc is 56× because it is a
  *chain* of ~6 forwarded ioctls × ~1.2 ms each.
- The optimization lever is **fewer hops/wakeups per forwarded ioctl** (e.g.,
  inline trivial ioctls on the vq thread to skip the thread-pool bounce; or a
  shared-memory path that removes the blocking handoff — the SPSC ring does this,
  but only helps batchable, ioctl-bound paths, which decode is not).  A dedicated
  (non-oversubscribed) host would also cut the ~1.2 ms scheduling component.

## Config experiment: CPU pinning + KVM halt_poll_ns (2026-05-31)

Tested the "it's mostly scheduling" hypothesis with zero code: pinned QEMU threads
to cores 2-6, stub workers to 7-10 (host 0-1), swept `halt_poll_ns`.  Alloc bench
(ring OFF), 11-core vast.ai instance:

| config | alloc RTT (µs/pair) |
|---|---|
| baseline (unpinned, halt_poll=200µs) | 6812 |
| pinned, halt_poll=0 | 7204 |
| pinned, halt_poll=200µs (default) | 5473 |
| pinned, halt_poll=2ms | 3856 |
| pinned, halt_poll=10ms | 3952 (plateau) |

- **~43% cut for free** (6812 → ~3850), `halt_poll_ns` the dominant knob —
  the vCPU polls for the completion instead of yielding and eating a re-schedule.
- **Plateaus at ~2ms**: beyond that the vCPU-wake component is fully covered, so
  the rest is NOT vCPU scheduling.
- **The ~3850 µs/pair floor is genuine multi-hop software round-trip, not jitter**:
  host load 0.92, CPU steal negligible.  So the half-right verdict: config removes
  the scheduling half; the other half is real plumbing (QEMU iothread + thread-pool
  + completion bounce + socket + stub-worker wakeups, × several ioctls/pair).
- Launch RTT (~54 µs) and throughput (422 GFLOP/s) unchanged throughout — confirms
  the launch/compute path is not ioctl-round-trip-bound.

**Takeaway:** pinning is a free always-on win; `halt_poll_ns` is a latency-vs-CPU
tunable (higher burns idle-vCPU CPU, only worth it for latency-bound deployments).
The residual software floor would only fall to the transport rework (collapse hops
/ host kernel module) — justified ONLY if a real workload is setup-latency-bound,
which decode/compute are not (already at host parity).
