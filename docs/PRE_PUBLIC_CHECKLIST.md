# Pre-public checklist — review before any external/public launch

Items deliberately parked as "good enough to proceed, but revisit before we go
public." Not blockers for current development; each has a recorded root cause so
it can be picked up as a focused effort.

---

## NVENC encode throughput (~7×) — task #101

**Status:** root-caused, parked. NVENC video **encode is correct and usable**
(H.264 + HEVC, valid bitstreams, host-identical RM statuses — task #99), at
**~55 fps / 1080p** vs host ~373 fps (raw input). Fine for real-time 1080p30
streaming today; **not** for high-fps / multi-stream / 4K.

**Why it matters before public:** remote GPU-VM workstations are viewed over the
wire via codecs, so NVENC throughput is the pixel-delivery rate. 55 fps caps the
remote-desktop experience at ~1080p30.

**Root cause (proven, not guessed):** NVENC waits on a per-frame **userspace
kernel eventfd** (`anon_inode:[eventfd]`, one per in-flight frame) with a 100 ms
timeout. In the guest that completion event is **never signaled**, so every frame
eats the full 100 ms timeout then re-checks and finds the frame done. On the host
the same eventfd *is* signaled promptly (→ 373 fps), so the driver uses eventfd
completion; the guest simply never receives the host-side signal. Not ioctl-bound
(~1.85 ioctls/frame) and not GPU-bound (encoder saturates with raw input).

**Two attempts that MISSED (so the next person doesn't repeat them):**
1. Guest VQ_EVT registry waking an `nvkvm_fd_ctx` `poll_wq` — **wrong target**;
   the OS-event fd is a real userspace eventfd, needs `eventfd_signal`, not poll_wq.
   (Harmless no-op scaffolding landed in commit `c55bd71`; reusable.)
2. QEMU eventfd watch hooked at the `dev_id 0xFF` open path
   (`nvkvm_isolate_handlers.c` open handler) — **never registers** for NVENC's
   event (proven: watch-add count 0). NVENC's eventfd does not flow through that
   path. Reverted.

**Remaining work (do the trace FIRST):** there are ≥2 eventfd-creation paths —
stub-side (handle-resolved at the stub, `fe_embedded_fd_off=8` for `NV_ESC_ALLOC_OS_EVENT`)
and QEMU-direct (`nvkvm_handle.c:107` makes an eventfd itself). Trace end-to-end
*which* host-side eventfd the driver actually signals for the event, then: watch
THAT fd on the QEMU main loop → relay via `VQ_EVT` → in the guest `eventfd_signal`
the userspace eventfd captured at `NV_ESC_ALLOC_OS_EVENT` (keyed by handle). The
`VQ_EVT` plumbing + guest registry from `c55bd71` are reusable once the wake
target is corrected to `eventfd_signal`. Same problem-class as the broader
control-path latency; a correct fix here speeds **every** event-driven NVIDIA wait.

Details: task #101; memory `nvenc_encode_working.md`.
