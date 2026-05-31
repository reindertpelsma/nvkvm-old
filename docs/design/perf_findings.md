# Decode-Latency Perf Findings (host vs guest)

Status: IN PROGRESS (2026-05-31). Measurements + probable fixes for the
single-stream LLM-decode latency gap. Companion to `fastpath_command_buffer`
discussion and memory `decode_ioctl_profile` / `perf_host_vs_guest`.

## Measurements

1. **Compute/data path = ~0% overhead.** GEMM throughput host 450.5 vs guest
   452.3 GFLOP/s (within noise). The passthrough-DMA-mmap design makes sustained
   compute bare-metal. (`tests/integration/gpu_bench.c`)

2. **Single-stream decode is heavily penalized.** Qwen-0.5B: host 368 t/s vs
   guest ~24 t/s decode (~15×); host 800 vs guest 274 t/s prefill (~2.9×). The
   guest decode rate is ~flat across model size (0.5B ~24, 7B ~20) and across
   CUDA-graphs on/off → a fixed per-token *latency floor*, not a compute or
   launch-count effect.

3. **ioctl profile is identical host vs guest.** Both issue the same ~985
   ioctls over a 16-token run (~20/token steady-state), ~70% RM_CONTROL (0x2a).
   So libcuda's control-command volume is intrinsic — NOT a guest-specific slow
   fallback. The gap is *per-ioctl latency*, not count.

4. **Per-ioctl overhead ≈ 880 µs.** 37 ms/token guest overhead ÷ ~42 ioctls ≈
   880 µs extra per ioctl vs host.

5. **The QEMU↔isolate socketpair is NOT the bottleneck.** Standalone ping-pong
   (`/tmp/sp_pingpong.c`, host, 11 cores):
   - cross-core busy-poll: **~10 µs** RTT (stable under scheduler load)
   - cross-core blocking: ~30–35 µs
   - same-core blocking, oversubscribed: ~123 µs
   Eliminating the socketpair wakeup saves ~20 µs/ioctl ≈ **~2% of the gap**.

## Conclusion / bottleneck hypothesis

The dominant cost (~850 µs of the ~880 µs/ioctl) is **the guest↔host crossing**,
not the host-side socketpair: the guest task blocks on `wait_for_completion` →
the **vCPU deschedules/idles → completion IRQ injection → vCPU wakeup**. In a VM
this round-trip is far costlier than a host socketpair wakeup (the ping-pong was
host-only, no VM transitions).

**Implication for the command buffer:** it pays off ONLY via its **guest-side
spin** on shared completion memory (no vCPU deschedule, no IRQ wakeup). A
QEMU↔isolate-only fast-path buys ~2% and is not worth building. The guest-side
half is the prize.

## Experiment 2: guest-side completion spin — NEGATIVE (vCPU-wakeup ruled out)

Made the guest module bounded-spin (~600 µs budget, `try_wait_for_completion` +
`cpu_relax`) on the forwarded-ioctl completion before blocking. Result: decode
**unchanged at ~28 t/s** (same as the non-spin baseline; the original "24" was
pre-security-fix noise). So keeping the vCPU running (no idle→IRQ→wakeup) buys
**nothing** → the guest-side vCPU wakeup is **NOT** the bottleneck.

## Reframed bottleneck (after ruling out socketpair AND guest-wakeup)

The ~1.6 ms/ioctl overhead (≈37 ms/token ÷ ~20 ioctls) is on the **host side**,
in QEMU's machinery between receiving the virtqueue kick and producing the
response — candidates, none yet isolated:
- thread-pool dispatch latency (`thread_pool_submit_aio`) getting the ioctl work
  to a pool thread under load;
- the reader-thread → worker pthread_cond handoff for the response;
- the completion callback (`nvkvm_ioctl_work_done`) running under the **BQL** —
  if vCPUs hold the BQL, the completion is delayed (ms-scale under contention);
- per-ioctl serialization on QEMU/stub locks.

A spin on the host side wasn't tested; the **command buffer (guest↔isolate,
bypassing QEMU entirely)** removes ALL of the above — the isolate's spin-thread
does recv→ioctl→send inline with no thread-pool, no cond handoff, no BQL. So the
command buffer remains the likely fix, now for a host-QEMU reason rather than a
guest-wakeup one.

## Next experiment (localize the host-side cost before building)

Add timestamps in QEMU: t0=kick received (tx_handler), t1=worker starts (after
pool dispatch), t2=sock_send to stub, t3=response received, t4=writeback/BQL
completion; and in the stub: recv→ioctl-start→ioctl-done→send. Run decode, dump
the per-ioctl breakdown. This pinpoints whether the ~1.6 ms is pool dispatch,
the cond handoff, BQL contention, or response production — and confirms the
command buffer would remove it, before committing to the build.

## Probable fixes (ranked by expected payoff, pending the next experiment)

1. **Guest-side completion spin** (bounded, then block) — eliminate the vCPU
   deschedule/IRQ-wakeup on each forwarded ioctl. Likely the bulk of the prize.
   Must be adaptive (spin tens of µs → futex/block) to avoid burning a vCPU when
   idle, and respect the security model (no privileged pages exposed).
2. **Shared-mem command buffer** (guest↔isolate, both spin on separate cores) —
   the full version: removes socketpair + VM-exits + wakeups for pure ioctls.
   Floor ≈ host rate (≈368 t/s), since the host still executes ~42 ioctls/token.
   See `fastpath_command_buffer` notes; needs copy-in TOCTOU discipline (cf.
   audit P2-2), per-isolate spinloop, lost-wakeup-safe SYNC.
3. **Adaptive stub mutex** (spin→futex) — modest; stability/groundwork. The
   current `fs_mutex` is a correct non-adaptive Drepper futex mutex.
4. Socketpair→shared-ring busy-poll (QEMU↔isolate) — DEPRIORITIZED (~2%).

## Why this is the floor without SR-IOV

SR-IOV is faster because hardware VF isolation lets the guest ring the GPU
doorbell directly with no mediation. On a commodity (non-SR-IOV) GPU the isolate
MUST execute each forwarded ioctl to enforce the security boundary — that
host-ioctl-per-command is the irreducible price of software isolation on a
shared GPU, and exactly what lets nvkvm run where SR-IOV cannot.
