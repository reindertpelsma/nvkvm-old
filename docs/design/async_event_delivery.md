# #101 NVENC throughput — CORRECTED root cause (2026-06-02)

## What #101's note assumed (WRONG)
"NVENC's GPU-completion OS_EVENT never wakes the guest poll/eventfd; async event
delivery (VQ_EVT) is unimplemented → ~18 ms poll-timeout per frame." A full
VQ_EVT delivery chain was built on that premise (guest `.poll` arms
POLL_ON_ISOLATE → stub ppoll-watches the host OS-event fd → RESP_POLL_EVENT →
QEMU BH posts vq_evt → guest `nvkvm_evt_deliver` wakes poll_wq).

**Result: ZERO improvement (still 28 fps).** Reverted. Here is why it can't work.

## What actually happens (host strace, definitive)
- libnvidia-encode's main thread waits on an **eventfd** (`poll([{fd=17
  <eventfd>}],1,100)`). On the host it is woken **128×** vs 13 timeouts.
- That eventfd is **`write()`-signalled by a worker thread** (21 writes / 21
  reads observed), NOT by the kernel and NOT by any OS-event fd.
- The worker (the busy encode thread) gets woken on `/dev/nvidia0` poll **0
  times** — on the host AND the guest. So the `/dev/nvidia0` OS-event-fd poll is
  **idle backoff parking, not the completion path**.
- The worker's only syscalls around signalling are ioctls (alloc/free/map) +
  reads of `/proc`, the input file, and a pipe — **never `/dev/nvidia`**. So it
  detects GPU-encode completion with **no syscall at all** → it reads a **mapped
  fence / semaphore** the GPU writes, then `write()`s the eventfd.

## Therefore: it's a mapped-memory coherence problem, not events
On the host the completion fence updates instantly (coherent host VRAM/sysmem
mapping) so the worker spins briefly and signals. On the guest the worker parks
on the 100 ms backoff poll because **its view of the completion fence/semaphore
updates slowly or stale** — the same class as the DtoH / WB-sysmem work
(#94 `578662f`, WB-sysmem `c5d5d8a`, [[decode_14x_slow_root_causes]]): a mapped
buffer that must be **write-back cached / coherent** in the guest but isn't.

## The real fix (next attempt)
1. Identify the NVENC completion semaphore/fence buffer: which RM alloc + which
   mapping (likely an `NV01_MEMORY_*` sysmem surface mmap'd by libnvidia-encode,
   or a semaphore-surface) carries the per-frame fence the worker polls.
   Method: on the guest, find the mapped VA range the worker reads in its spin
   (gdb watchpoint / instrument the mmap path / correlate the mmap'd offset with
   the RM object class), then check its guest mapping memory type.
2. Ensure that mapping is **WB-cached / coherent** in the guest, exactly like the
   DtoH migrated-range fix and the WB-sysmem mmap fix. Likely a memory-type
   decision in `nvkvm_mmap.c` keyed on the RM object/mapping class (mirror what
   c5d5d8a did, matching gVisor nvproxy's memory-type handling).
3. Verify: ffmpeg `h264_nvenc` raw-input fps → host-class, worker stops parking
   on the 100 ms poll (timeout count → near 0), per-frame `poll()` avg → sub-ms.

## Status
The VQ_EVT delivery chain remains scaffolded-but-stubbed (guest `nvkvm_evt_deliver`
+ callback exist; stub CMD_POLL + QEMU RESP_POLL_EVENT are no-ops). It is the
right primitive for *genuinely* fd-poll-driven waits, but NVENC is not one — do
not re-attempt it for #101. #101 is a fence-coherence fix.
