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

## Decode is 14× slower than host — root causes (2026-05-31)

Got the missing host baseline: same model/llama-cli on the host = **387 t/s** vs
guest **28 t/s** — decode is **14× slower**, NOT at parity. (Earlier "parity" was
matmul = ONE big compute-bound kernel; decode = hundreds of tiny launches/token,
so it's launch/exit-bound, a totally different regime.)

KVM exit profile during guest decode (`kvm:kvm_mmio` ftrace + debugfs counters)
found three culprits, in order discovered:

1. **HPET clocksource (70% of MMIO exits).** `-cpu host,hypervisor=off` cleared
   the hypervisor CPUID bit → guest never discovered kvm-clock → fell back to
   HPET → every clock read is a VM-exit (~3800/token). The `hypervisor=off` was
   to hide the VM from NVIDIA Code-43 detection, but in the FORWARDING model the
   NVIDIA driver runs on the host, not the guest — nothing to fool.
   **Fix: drop `hypervisor=off` → kvm-clock.** Forwarding + matmul still pass.

2. **virtio-nvgpu on legacy shared INTx, not MSI-X.** Every completion interrupt
   triggered shared-line ISR-status demux MMIO reads (~2150/token at the *net*
   device's BAR, sharing the line) + fasteoi. The block devices already used
   MSI-X; the nvgpu PCI wrapper left `nvectors` at the struct default 0 (not
   DEV_NVECTORS_UNSPECIFIED) so the usual idiom never enabled it.
   **Fix: force `vpci_dev->nvectors = 4` (3 VQs + config) before qdev_realize**
   (msix_init runs during device_plugged, so a later set is ignored). MSI-X
   Enable+ Count=4 confirmed; mmio_exits 2155→366/token.

**But single-stream decode barely moved (28 → 32 t/s).** Both fixes are real
(HPET 114k→0 exits; mmio 5×↓) and cut host-CPU/VM-exits per token — a density /
multi-tenant win — but they did NOT move single-stream throughput, and neither
did `halt_poll_ns` (0/200µs/2ms/8ms all ~32 t/s). So the 12× residual is **NOT**
VM-exit, interrupt, clocksource, or halt-scheduling bound. It's **per-launch
latency**: ~570 launches/token × ~54 µs/launch (vs 6.65 µs host) ≈ 31 ms/token.
Host view shows decode is single-threaded, one vCPU ~67% busy (~1/3 blocked) —
latency-bound in the launch+sync path itself, not exit-bound.

**Open (next investigation, guest-side):** where does the ~54 µs/launch go if not
exits/interrupts/halts? Candidates: doorbell-write path, completion-fence
visibility/coherency (PCIe-read vs cached), or guest-module CPU in the forward
hot path. Needs guest-side per-launch profiling, not host exit-counting.

## Per-subsystem microbenchmark (cuda_micro.c) — host vs guest, 2026-05-31

Driver-API microbenches, each isolating ONE subsystem (the right way to localize
the gap instead of inferring from LLM decode). Same binary, host vs guest:

| subtest | host | guest | ratio |
|---|---|---|---|
| 1 rm_control (cuMemGetInfo)   | 13.3 µs | 807 µs   | **60×** |
| 2 alloc+free (cuMemAlloc)     | 134 µs  | 3761 µs  | **28×** |
| 3 bandwidth HtoD              | 13.2 GB/s | 12.0 GB/s | ~parity |
| 3 bandwidth **DtoH**          | 10.2 GB/s | **0.1 GB/s** | **~100× slower** |
| 4 launch_sync (noop+sync)     | 6.5 µs  | 13.3 µs  | **2×** |
| 5 uvm_alloc (cuMemAllocManaged)| 163 µs | **fails (null→crash)** | broken |
| 6 uvm_migrate (CPU<->GPU)     | 3742 µs | (n/a, alloc fails) | broken |

Findings (several overturning earlier guesses):
- **DtoH is ~100× slower than HtoD** (0.1 vs 12 GB/s) while HtoD is at parity —
  an asymmetric bug in the device→host writeback path (memfd→shm readback),
  almost certainly unbatched/per-page. HIGH priority, was completely unsuspected.
- **rm_control 60× / alloc 28×** = the ~800 µs forwarding round-trip per ioctl
  (matches the earlier decomposition); the transport work targets this.
- **launch_sync only 2×** — the doorbell+fence path is NOT the big problem;
  this REFUTES the earlier "decode is launch-bound (~54µs/launch)" inference.
- **UVM managed memory is broken** — cuMemAllocManaged returns null in the guest
  (CPU-touch then segfaults); the managed/demand-paged path isn't supported.

Implication for decode: it's a MIX dominated by control round-trips (60×, the
most frequent per-token op) and possibly DtoH if results are copied back — NOT
launches. Fix priority: (1) the DtoH writeback bug (asymmetric, surprising),
(2) control/alloc round-trip (transport), (3) UVM managed (separate). Tool:
tests/integration/cuda_micro.c (driver API, dlopen libcuda, runs host+guest).

## Subtest 7 (poll_sync) + corrected DtoH picture (2026-05-31)

Added a poll-heavy subtest: CU_EVENT_BLOCKING_SYNC makes cuEventSynchronize block
on the event fd via poll() (vs the default spin). Loop: noop launch + event record
+ blocking sync.

| | host | guest | ratio |
|---|---|---|---|
| 4 launch_sync (spin)        | 6.1 µs  | 13.1 µs  | 2.1× |
| 7 poll_sync (blocking event)| 72.8 µs | 130.7 µs | **1.8×** |

**The poll/blocking-completion path is only 1.8× slower in the guest — NOT a
bottleneck.** This refutes the "poll dominates DtoH" reading: the ~92ms-per-poll
seen during DtoH was the poll *waiting* for slow migration work to finish, not
poll overhead. DtoH correctness verified OK (byte-exact).

So DtoH's real cost is the **host-side UVM per-page migration**: a plain
cuMemAlloc + cuMemcpyDtoH issues ~6118 `uvm 0x48` ioctls (per-page migration)
instead of a bulk copy-engine DMA; the guest blocks in poll waiting for it.
DtoH-slow and the cuMemAllocManaged err=999 are the SAME root area (UVM path).
Next: trace the host-side UVM migration during DtoH (why per-page 0x48, can it
be bulk/copy-engine) — that's the real lever, not the copy transport or poll.

## DtoH fix investigation (2026-06-01) — confirmed mechanism, found the lever, hit a wall

Goal: fix pageable cuMemcpyDtoH (100x slow). Findings:
- **Pinned (copy-engine) DtoH is ~instant: ~1857 GB/s vs pageable 0.1 GB/s.** So
  the copy-engine path is fine; only the pageable path is pathological.
- **Pageable slowness = per-page UVM_VALIDATE_VA_RANGE (0x48): ~6118 forwarded
  ioctls/copy.** libcuda routes pageable cuMemcpy through UVM's pageable-access
  path. DtoH correctness verified byte-exact.
- **cuMemHostAlloc (pinned) has an ~8MB cap**: works ≤8MB, FAILS ≥16MB with
  err=304 (OPERATING_SYSTEM). Separate bug; smells like a slot/mmap size limit.
  (libcuda's internal bounce buffers are small, so this doesn't block the fix.)
- **The fix lever**: make libcuda use the copy-engine/bounce path for pageable.
  TRIED forcing pageable-access UNSUPPORTED via the UVM_PAGEABLE_MEM_ACCESS(39)
  + _ON_GPU(70) ioctl response → **NO effect** (pageable DtoH still 0.1). So
  libcuda's pageable-copy decision is NOT gated on that UVM ioctl.
- **STUCK / next**: the decision is gated on the device attribute
  PAGEABLE_MEMORY_ACCESS (CUdeviceGetAttribute 88, =1 in guest, from an
  RM_CONTROL) or an internal libcuda decision. Need to identify the exact
  RM_CONTROL/field that reports pageable support and override it to 0, then
  re-test — OR fix the ~8MB pinned cap so apps using pinned go fast directly.
  Both are source-level digs (libcuda decision tracing / RM control byte-diff).
