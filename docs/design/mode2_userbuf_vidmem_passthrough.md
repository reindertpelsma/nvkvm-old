# Mode-2 user-buffer data plane: real host-vidmem passthrough (the cup5/LLM hang fix)

Status: **diagnosis complete, implementation pending** (2026-06-14). Branch `consolidation`.

This doc captures the root-cause analysis of the Mode-2 bulk-copy / LLM-model-load hang and the
agreed fix direction (real host-vidmem passthrough for user allocations + CE forward). It is the
reviewable companion to the live agent-memory notes under
`/root/.claude/projects/-workspace-nvidia-gpu-passthrough/memory/mode2_execfwd_layer2.md`.

## Symptom

`cuMemcpyHtoD` of a `cuMemAlloc` buffer is glacial: **~50–90 MB/s**, host GPU util ~0%. cup2 (4 KB)
passes instantly via emulation; the LLM moves ~469 MB of weights and hangs the model load. Repro:
`tests/mode2/cup5.c` (driver-API bulk HtoD/DtoH + byte-verify + timing), harness
`scripts/mode2_diag/m558_bulk_dataplane_host.sh`.

## Diagnosis — three instrumented boots (cheap probes, no behaviour change)

The earlier theory ("the bulk CE copy is CPU-emulated; forward it to the host CE") was **wrong**.
What the data actually shows:

1. **Not a page-identity / `put=0` bug** (CE-INSTR probe). 292 channel-advance events, **zero**
   page-A≠page-B divergences: the guest's GP_PUT reaches the host USERD. Real symptom: host
   `GP_GET` stuck at 0 (host never consumes), and the **64 MB does not flow through CE emulation
   at all** (CE COPY total 1.4 MB; emulated ops don't scale with copy size).

2. **The guest CPU moves the bytes** (m559 CPU-attribution, 256 MB). cup5 in R(running) state;
   host GPU util ≈ 4% (idle); emulated-op bytes flat (~3 MB) regardless of copy size → not QEMU
   emulation, not the GPU. It's a **guest-side libcuda CPU `memcpy`** ("effectively HtoH").

3. **The mapping is UC, on a RAM memslot — not MMIO-trapping** (BAR1-TRAP-INSTR + m560
   double-copy). Zero BAR1 aperture writes for a whole 64 MB copy → no per-access vmexit (it's a
   RAM memslot). Three back-to-back copies to the *same* buffer are all equally slow
   (52.3 / 54.8 / 55.4 MB/s) → **not** first-touch fault/residency overhead; it's **persistent UC
   write cost**. `pat_memtype_list` confirms `uncached-minus` mappings. (A WB RAM memcpy is GB/s;
   ~53 MB/s userspace memcpy is the UC signature — too slow even for WC.)

### Why the mapping is UC (and why that's wrong here)

The stock guest `nvidia.ko` chooses CPU caching in `nv_encode_caching` from the **RM memory
descriptor's attributes** (type / aperture / caching flag), *not* from the guest's e820 RAM-vs-MMIO
map. `cuMemAlloc` is device/vidmem memory; on real hardware its CPU view is the BAR1 aperture mapped
UC-/WC (correct for a real PCIe BAR). In Mode-2 we expose fake-vidmem as a CPU-mappable region but
back it with a **guest-RAM memslot** — so the driver maps it with vidmem/BAR caching (UC-), which is
pathologically slow on plain RAM and **needless**: x86 PCIe DMA is cache-coherent (the GPU snoops
caches), so WB RAM the GPU DMAs into is safe. UC here is a descriptor artifact, not a DMA
requirement. (Real nvidia maps vidmem BAR **WC**, coalesced → fast; the doorbell page may be the
only legitimately-UC page.)

## Root cause — the buffer is a *fake page*

- Guest vidmem `RM_ALLOC` (NV01_MEMORY_LOCAL 0x003e/0x0040, NVOS32 VidHeapControl) **is forwarded**
  to the host → a **real host vidmem object + handle already exist** in the Mode-2 isolate
  (`m2_iso`). (`nvkvm_frontend.c:86-201`, `nvkvm_dispatch.c:283-312`.)
- But when the guest **maps** it: `RM_MAP_MEMORY` is forwarded then the host VA is zeroed
  (`nvkvm_dispatch.c:332` "guest must not use host VA"); the guest instead gets a GPA whose backing,
  **for user allocations, is fake guest-RAM / emulated `fb_page`**, *not* the real host vidmem
  object. So the host GPU holds a real buffer while the guest CPU writes to a **different**, fake
  RAM page — UC-, ~50 MB/s, GPU-invisible.
- The real double-mmap passthrough exists and is **proven**, but is wired only for **privileged**
  buffers (USERD/GPFIFO/GR-context), reactively, gated by `m2exec`. User data allocations were left
  fake as bring-up scaffolding (see `docs/design/mode2_memory_model.md:36-40` "bring-up vs parity",
  `mode2_dataplane_architecture.md:244-265` which names this exact gap).

This matches the architectural principle: **guest userspace should see almost no fake pages** —
anything guest userspace can obtain as a page is also obtainable by the QEMU/isolate via host
ioctls/mmap, so it should be direct passthrough (guest RAM, or real host-GPU MMIO mmap'd through).
Fake pages belong to the guest *kernel* side (PDTs etc.). This user buffer is wrongly fake.

## Reusable machinery (already proven — extend to user allocations)

| Function | File:line | Role |
|---|---|---|
| `nvkvm_m2_host_alloc_map_vidmem()` | `nvkvm_gpu_emul.c:4577` | alloc real host vidmem (class 0x0040) + host `RM_MAP_MEMORY` → `{qva,size}` (proven 651d860) |
| `nvkvm_m2_back_and_map()` | `:5056` | double-mmap a host vidmem obj into `m2_fbback[]` at guest-FB phys (guest CPU + host GPU share bytes) + map into host GR VAS at guest VA |
| `nvkvm_m2_back_and_map_sys()` | `:5260` | OS_DESCRIPTOR sysmem variant |
| `nvkvm_m2_map_dma()` | `:4834` | `RM_MAP_MEMORY_DMA` FIXED (unprivileged) into a VASpace |
| `nvkvm_m2_gpga_obj()` | `:5297` | register GPGA→host-obj so BAR1/PRAMIN resolve via `nvkvm_fb_host_overlay()` |
| isolate device-mmap into GPA window | `nvkvm_isolate_handlers.c:1808-1839` | `mmap(target,len,…,h->fd,offset)` — the real passthrough mmap; user vidmem mmap currently doesn't reach it (falls to anon/guest-RAM ~1858-1869) |

## Fix plan — option 3 (real vidmem + CE forward)

1. **Track** forwarded user-vidmem `RM_ALLOC` handles (guest↔host), distinct from privileged buffers.
2. **On the guest's CPU-mmap of such a buffer**, back its guest-FB-phys with the already-forwarded
   host vidmem object via `back_and_map`/`gpga_obj` (+ the isolate device-mmap into the GPA window /
   KVM memslot) instead of fake RAM, **mapped WC**.
3. Then the guest CPU memcpy hits real WC vidmem (GB/s, host-visible); or libcuda's CE path is
   forwarded to the host CE.
4. **Verify**: cup5 → GB/s + host util>0/visible + byte-exact; then llama (`m557`).

**First implementation step:** a targeted instrumented boot to confirm the *exact routing* of a
user-vidmem CPU-mmap in Mode-2 (which handler assigns its GPA/backing) — that is the precise hook
point for step 2.

Band-aid alternatives (WC-only or WB-only on the fake RAM page) were considered and **rejected** in
favour of real passthrough; the diagnosis is retained above for the trail.

## Diagnostic artifacts (this work)

- `tests/mode2/cup5.c` (bulk repro), `tests/mode2/cup6.c` (3-copy UC-vs-fault discriminator).
- `scripts/mode2_diag/m558_bulk_dataplane_host.sh`, `m559_cpu_attrib_host.sh`,
  `m560_copy_discriminator_host.sh`; `rtp_run_guest.sh` `cup6` subcommand.
- `nvkvm_gpu_emul.c` probes: `CE-INSTR` (per-advanced-channel page-A/B + gpga overlap, ~:2534),
  tagged `M5: CE COPY` line with client/gpfifo (:3762), `BAR1-TRAP-INSTR` cumulative counter
  (`nvkvm_baraperture_write`, :2876). Cheap logging; keep for fix verification.
