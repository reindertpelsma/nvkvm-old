# Mode-2 user-buffer data plane: real host-vidmem passthrough + CE forward (the cup5/LLM hang fix)

Status: **diagnosis complete & corrected, implementation pending** (2026-06-14, m562). Branch `consolidation`.

Root-cause analysis of the Mode-2 bulk-copy / LLM-model-load slowness and the agreed fix (real
host-vidmem backing for user allocations + copy-engine forward). Reviewable companion to the live
agent-memory notes (`memory/mode2_execfwd_layer2.md` tail).

> **2026-06-14 correction.** An earlier version of this doc blamed a *UC userspace mapping / guest
> libcuda CPU `memcpy`* and asserted the user vidmem `RM_ALLOC` "is forwarded to a real host vidmem
> object." A boot on the **open-580 RM core built from source** (patchable, with placement printks
> compiled into `src/nvidia`) **falsified both claims** — see Diagnosis below. The fix direction
> (real vidmem backing + CE forward) is unchanged and now better-justified.

## Symptom

`cuMemcpyHtoD` of a `cuMemAlloc` buffer is glacial: **~95–107 MB/s**, host GPU util **0%** (host
oracle on the same RTX 3060 does the identical 64 MB copy at **~7 GB/s** via CE). cup2 (4 KB) passes
instantly; the LLM moves ~469 MB of weights and the model load crawls/hangs. Repro: `tests/mode2/cup5.c`
(bulk HtoD/DtoH + byte-verify + timing); discriminator `tests/mode2/cup6.c` (3-copy + self-introspection).

## Diagnosis — ground truth from the open-580-from-source boot (m562)

We built `open-gpu-kernel-modules@580.159.04` **from source** as the guest driver (no `nv-kernel.o_binary`
blob → patchable RM), with two diagnostic printks compiled in, and measured the host side *during* the
copy. Hard facts:

1. **Placement is correct and matches real hardware.** `vidmemConstruct_IMPL` printk: the 64 MB
   `cuMemAlloc` is **FBMEM via PMA** — `pmaInit=1 bIsPmaAlloc=1`. `memmgrInitBaseFbRegions_FWCLIENT`
   printk: our faked GSP advertises a sane FB region table (`numFBRegions=5`, `usable=0x2ecad0000`
   ≈ 11.7 GiB). So the guest RM places the buffer in real vidmem exactly as on bare metal.

2. **libcuda picks the CE/DMA path, like host.** cup6's `/proc/self/maps`×pagemap introspection: the
   device pointer `dp` is **PROT_NONE (`---p`), no CPU mapping at all**. There is therefore **no
   guest-userspace `memcpy`** of the payload — libcuda issues a copy-engine `LAUNCH_DMA`, identical to
   the host oracle. (This retires the earlier "UC userspace memcpy" model, which had mis-attributed an
   unrelated PROT_NONE VA reservation at `0x10000000000`.)

3. **The data plane is fake — that is the whole bug.** Host `nvidia-smi` sampled *throughout* the
   copy: **util stayed 0% and `memory.used` never moved off baseline** (peaked 19 MiB, never the
   64 MB the guest "allocated"). Therefore:
   - the guest's PMA-FBMEM buffer is **fake-backed** (emulated FB / `g_malloc0` `fb_page` /
     guest-RAM), **not a real host vidmem object** — falsifying the prior "is forwarded to real host
     vidmem" claim; and
   - the copy **does not run on the host GPU** — it is **CPU work**. The `NVKVM-DPLANE` probe (below)
     confirmed the mover is **QEMU's emulated `LAUNCH_DMA` loop** at `nvkvm_gpu_emul.c:3755-3844`,
     which resolves and copies **4 bytes at a time** (`ce_bytes_total` scales with the payload; every
     byte funnels through `nvkvm_fb_write`) → the ~100 MB/s ceiling.

> **NVKVM-DPLANE diagnostic boot (2026-06-14) — refines the above.** A gated probe (CE per-call dest
> verdict + byte attribution) showed: (a) the **emulated CE loop is the data mover** — confirmed, not a
> kernel memcpy; (b) the dest is **mostly real-backed already** — `overlay_real_write_bytes ≈ 75 MB`
> (bulk 16/14/2 MB chunks `verdict=gpga`, real host vidmem) vs only `fbpage_write_bytes ≈ 8.6 MB` (small
> scattered 64 KB chunks `verdict=fbpage`, fake) plus 785 virtual-addressed copies. Host `memory.used`
> stays ~19 MB because those real regions are small and **overwritten** (staging/scratch reuse), not a
> fresh 64 MB object. ⇒ the dominant fix is **(B) forward the CE to the host GPU**; backing (A) is
> largely present and shrinks to closing the `fbpage` gaps + host-VAS-mapping the full user dest.

### What is NOT the problem (closed branches — do not re-investigate)

- **Placement / PMA / FB-region table** — correct (item 1). The faked-GSP `fbRegionInfoParams` is fine.
- **Cacheability / UC vs WB / `nv_encode_caching`** — moot: `dp` has no CPU mapping, so its cache
  attribute is irrelevant to the copy. The WB-everywhere principle ([[mode2_wb_cacheability_principle]])
  still holds generally, but it is **not** the cup5 lever.
- **libcuda's CE-vs-CPU heuristic** — correct: it chose CE, same as host.

## Root cause — the FBMEM the guest gets is a fake page, and the CE runs on the CPU

The guest RM correctly allocates FBMEM and emits a CE copy, but in Mode-2 that FBMEM is backed by
emulated/guest-RAM pages instead of a real host GPU vidmem object, and the CE descriptor is executed
by QEMU on the CPU instead of being submitted to the host GPU. Real double-mmap passthrough **exists
and is proven**, but is wired only for **privileged** buffers (USERD / GPFIFO / GR-context), reactively
via `m2exec`. User data allocations were left fake as bring-up scaffolding (see
`mode2_memory_model.md:36-40`, `mode2_dataplane_architecture.md:244-265`, which names this exact gap).

This matches the architectural principle: **guest userspace should see almost no fake pages** —
anything guest userspace can obtain as a page is also obtainable by the QEMU/isolate via host
ioctls/mmap, so it should be direct passthrough (guest RAM, or real host-GPU memory mmap'd through).
Fake pages belong to the guest *kernel* side (PDTs etc.). This user buffer is wrongly fake.

## Reusable machinery (already proven — extend to user allocations)

| Function | File:line | Role |
|---|---|---|
| `nvkvm_m2_host_alloc_map_vidmem()` | `nvkvm_gpu_emul.c:4577` | alloc real host vidmem (CONTIGUOUS\|VIDMEM) + host `RM_MAP_MEMORY` → `{qva,mapfd,size}` |
| `nvkvm_m2_back_and_map()` | `:5056` | double-mmap a host vidmem obj into `m2_fbback[]` at guest-FB phys (guest CPU + host GPU share bytes) + FIXED-map into host GR VAS at guest VA. Called today only with labels `ctx*`/`gpfifo`/`pushbuf`/`userd` |
| `nvkvm_m2_back_and_map_sys()` | `:5260` | OS_DESCRIPTOR sysmem variant |
| `nvkvm_m2_map_dma()` | `:4834` | `RM_MAP_MEMORY_DMA` FIXED (unprivileged) into a VASpace |
| `nvkvm_m2_gpga_obj()` | `:5297` | register GPGA→host-obj (`m2_objs[]`+`m2_gpga[]`) so BAR1/PRAMIN/overlay resolve via `nvkvm_fb_host_overlay()` |
| host GR GPFIFO forward (doorbell) | `nvkvm_m2_doorbell_setup():5119`, `nvkvm_m2_exec_doorbell():2580-2637` | AMPERE_USERMODE_A doorbell + work-submit token; rings real host GR channel. **CE-forward extends this** |
| emulated CE (to be bypassed for forwarded chans) | `nvkvm_chan_execute()` `LAUNCH_DMA` `:3755-3844` | current 4-byte CPU copy + completion-sema; user-CE sema already gated host-only |
| isolate device-mmap into GPA window | `nvkvm_isolate_handlers.c:1808-1839` | the real passthrough mmap; user vidmem mmap currently falls to anon/guest-RAM (~1858-1869) |

## Fix plan — option 3 (real vidmem backing + CE forward)

**(A) Back user FBMEM with a real host vidmem object.**
1. **Find the snoop hook** — the precise point where a guest user `cuMemAlloc` FBMEM allocation becomes
   observable to the QEMU device (guest NVOS32 / `NV01_MEMORY_LOCAL_USER` `RM_ALLOC`, and the guest-FB
   physical range PMA assigns it). User allocations are **not** snooped today; this is step 0.
2. For each such allocation, allocate the real host vidmem object (`host_alloc_map_vidmem`) and back the
   guest-FB-phys range with it via `back_and_map`/`gpga_obj` (double-mmap: CPU view at guest-FB phys,
   GPU view at the guest VA in the host GR VAS) instead of a fake `fb_page`.
3. **Verify (A):** re-run cup6 → host `memory.used` jumps ~64 MB (buffer now lives on the host GPU),
   byte-exact preserved.

**(B) Forward the CE copy to the host GPU.**
4. With src (guest sysmem staging, already host-DMA-able via the memfd window / OS_DESCRIPTOR) and dst
   (now real host vidmem) both real host objects, submit the `LAUNCH_DMA` to a host CE channel (new
   `m2_ce_channel`, or the GR channel's CE subchannel) and ring the doorbell — extend `exec_doorbell`
   (`:2580-2631`); bypass the emulated 4-byte loop for forwarded channels. User-CE completion sema is
   already gated host-only.
5. **Verify (B):** cup5/cup6 → **host util>0**, **GB/s**, byte-exact; then llama (`m557`).

This honours the directive to prove real host compute (util>0, HW-written results), not faster faking —
optimising the emulated CPU CE loop would be "faster faking" and is explicitly rejected.

**Diagnostic boot before coding (A):** one instrumented boot to pin (i) the exact handler/site where a
user `cuMemAlloc` FBMEM alloc + its guest-FB-phys is observable in QEMU, and (ii) confirm which CPU path
actually moves the 64 MB today (QEMU emulated-CE vs kernel memcpy into `fb_page`) — that fixes where the
backing is wired.

## Diagnostic artifacts

- Tests: `tests/mode2/cup5.c` (bulk repro), `tests/mode2/cup6.c` (3-copy + maps×pagemap introspection).
- Harnesses: `m558_bulk_dataplane_host.sh`, `m559_cpu_attrib_host.sh`, `m560_copy_discriminator_host.sh`,
  `m561_cache_probe_host.sh`, **`m562_placement_probe_host.sh`** (open-580 from-source + placement printks);
  `rtp_run_guest.sh` `cup6` subcommand.
- Open-580 from-source guest driver: host tree `research_clones/open-580` (full source); lean bootable
  tree `research_clones/open-580-guest` (kernel-open + materialized `nv-kernel.o_binary` + 0xFFF500 uvm
  patch); boot via `OGKM=research_clones/open-580-guest`. Placement printks in
  `open-580/src/nvidia/src/kernel/{gpu/mem_mgr/mem_mgr_gsp_client.c (NVKVM-FBREG), mem_mgr/video_mem.c (NVKVM-PLACE)}`.
- QEMU probes: `CE-INSTR` (~:2534), tagged `M5: CE COPY` (:3762), `BAR1-TRAP-INSTR` (`nvkvm_baraperture_write`, :2876).
