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
