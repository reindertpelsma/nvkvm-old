# #101 NVENC throughput — FINAL corrected analysis (2026-06-02)

This file went through three wrong root-cause theories. The measurements below
(clean apples-to-apples, pre-generated raw input, host vs guest, same ffmpeg
command) settle it. **#101 is NOT a 7× throughput bug, NOT an unsignaled eventfd,
NOT a mapped-fence coherence bug.**

## The numbers (RTX 3060, driver 580.159.04, h264_nvenc preset p4, -f null)

| input path                 | host fps | guest fps | ratio |
|----------------------------|---------:|----------:|------:|
| 720p,  CPU raw input       |      932 |       895 | 0.96× (PARITY) |
| 1080p, CPU raw input       |      428 |        63 | 6.8×  |
| 1080p, CUDA `hwupload`     |      302 |       121 | 2.5×  |

- Latency-bound (single surface, no pipelining), 720p: guest 4.7–6.2 ms/frame vs
  host 2.0 — a minor (+3 ms) VM cross-thread/event wakeup tax, well inside any
  real-time budget. Not the issue.
- The historical "55 vs 373 / 28 fps / 7×" were short-clip cumulative-average
  artifacts + CPU-bound `testsrc` input generation. Same lesson as
  decode_14x_slow_root_causes / #95.

## What's actually slow (proven by gdb + smaps, not guessed)
At 1080p with CPU raw input, the guest **main ffmpeg thread is 91% of one core,
8/8 gdb samples in `__memcpy_avx` ← `av_image_copy` ← `avcodec_send_frame`** —
i.e. copying each input frame INTO NVENC's CPU input surface (the pointer from
`NvEncLockInputBuffer`). The GPU and the other 3 vCPUs sit idle. The host main
thread at 1080p is only 60% CPU and overlaps that memcpy with `poll()` → 428 fps.

Properties of that NVENC input buffer (3240 kB = one 1080p yuv420p frame):
- HOST smaps: `/dev/zero (deleted)` shared, VmFlags `rd wr sh mr mw me ms sd` —
  pinned sysmem, WB-cached, page-backed. Fast host memcpy.
- GUEST smaps: same buffer, VmFlags add `pf io` — it has been remapped by nvkvm's
  CPU-page **migrate-range** path (the #94 path) onto the GPA window; the guest
  writes through the window every frame.

## Ruled out (with evidence)
- **Cacheability / WC:** an experiment mapping ALL guest isolate mmaps WB gave
  ZERO change (still 63 fps). The buffer was already WB via the migrate-range
  path (#94, `578662f` line in nvkvm_mmap.c). So it is NOT a WC/uncached bug.
  (Bonus: matmul still PASSED under WB-all → on x86 KVM, true device-BAR pfns get
  EPT=UC forced by `kvm_is_mmio_pfn()` regardless of guest PAT, so a WB doorbell
  does not hang. The c5d5d8a "would hang" fear was untested/over-cautious.)
- **Per-frame forwarding / migration:** steady-state ioctls ≈ 0 (120 frames→1667
  ioctls, 360→1669; ~1600 are one-time init), writes ≈ init-only, ~16 futex/frame.
  No per-frame round-trips.
- **Eventfd / OS-event delivery (the original theory):** completion is a mapped
  fence + worker write()→eventfd, syscall-free per frame, same on host and guest.
  The VQ_EVT chain built for it gave zero improvement and was reverted. Do NOT
  re-attempt it for #101.

## Real root cause
Per-frame **large CPU writes into a GPA-window-backed buffer are much slower in
the guest than a native host-RAM memcpy.** CUDA/compute dodges this: per #94's
note, the guest fills HtoD buffers as cached anon RAM *before* migration, so it
never writes *through* the window. NVENC's CPU input surface is migrated once and
then written *through the window every frame* → the cost shows up.
Confirmed by the `hwupload` test: routing input via `cuMemcpyHtoD` (the at-parity
HtoD path) instead of the NVENC input surface ~2×'s guest throughput (63→121 fps).
The residual 2.5× at 1080p is the remaining per-frame 3 MB CPU→GPU upload cost
through the window. Likely virtualized-memory overhead (EPT TLB pressure / 2-level
page walk) on per-frame multi-MB CPU writes; not NVENC-specific.

## Product impact (why this is NOT a pre-public blocker)
- NVENC encode is correct and usable (#99). 720p at host parity.
- 1080p guest is 63–121 fps depending on input path — still real-time for
  1080p30/1080p60 streaming, which is the stated use case.
- The user's real pipeline (capture a GPU-resident framebuffer → NVENC, frame
  already on the GPU) never touches the slow NVENC CPU input surface — it behaves
  like (or better than) the `hwupload` path. The 6.8× is largely an artifact of
  feeding NVENC from CPU raw frames in the benchmark.

## Fix direction (deferred performance work, same family as #94)
1. **Huge-page-back the GPA window memslot** (THP / MADV_HUGEPAGE or hugetlbfs in
   QEMU). A 3 MB per-frame CPU write touches 768 4 KB EPT entries; 2 MB pages cut
   that to ~2 → far less EPT TLB pressure. Contained QEMU-side change; would speed
   ALL large guest accesses through the window (HtoD, DtoH, NVENC), not just this.
2. For write-heavy *reused* buffers, consider keeping them as cached anon guest
   RAM (stub-side sync) rather than remapping onto the window — but that
   reintroduces per-frame sync; measure before committing.
3. Document the GPU-resident-input guidance for downstream encode pipelines.

Validate any fix with the table above (CPU-raw 1080p is the sensitive case).
