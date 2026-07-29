# C reference replay traces (task #90)

Four §6 replay traces recorded from the **C Mode-2 emulator** on real hardware, 2026-07-29.
They are the durable artefact: the C is a *perishable* oracle (it needs a booted guest, a
pinned kernel, a GA106 and a matching host driver, on a rented box), the traces are not.

Format: `src/qemu/nvkvm_m2_rec.h`. Recorder: `src/qemu/nvkvm_m2_rec.c` +
`m2rec/m2recfile/m2recmask` on `nvkvm-gpu-emul`. Decoder: `scripts/mode2_diag/rec_dump.py`.
Consumer spec: the rewrite's `docs/design/mode2_gsp_port_plan.md` §6/§6.3 and
`docs/design/c_rust_trace_differential.md`.

```
zstd -dc cap1_coldboot_hermetic.rec.zst > /tmp/c.rec
python3 ../../scripts/mode2_diag/rec_dump.py /tmp/c.rec --head 40
```

`MD5SUMS` covers the **decompressed** `.rec` files.

## The four captures

| file | records | raw / zst | props | what it is |
|---|---|---|---|---|
| `cap1_coldboot_hermetic.rec.zst` | 359 062 | 13.0 MB / 1.4 MB | `m2fwd=off m2exec=off m2romregs=off` — **hermetic** | ★ Cold GSP bring-up: PCI enumerate → VBIOS stream → FWSEC/WPR2 → LibOS boot args → msgq handshake → `GSP_INIT_DONE` → `nvidia-smi -q` enumerates the emulated GA106. **The only trace a replay can be closed over.** |
| `cap2_stalequeue_negative.rec.zst` | 886 999 | 34.2 MB / 3.1 MB | `m2fwd=on m2exec=on` — **NON-HERMETIC** | NEGATIVE. `cup2` (PASS) → `rmmod`/`insmod` twice in ONE QEMU lifetime. Life 2 dies on `msgqRxLink failed: -7 … NV_ERR_TIMEOUT`, life 3 on `unexpected WPR2 already up`. |
| `cap2b_stalequeue_nofn47.rec.zst` | 862 940 | 46.1 MB / 4.2 MB | `m2fwd=on m2exec=on` — **NON-HERMETIC** | ★ NEGATIVE, the sharp one. Driver restart **without** a prior CUDA process (so no `fn-47`). Contains **378 GSP command elements read out of arbitrary guest RAM and answered `NV_OK`**. |
| `cap3_matmul_forwarding.rec.zst` | 532 824 | 22.8 MB / 2.0 MB | `m2fwd=on m2exec=on` — **NON-HERMETIC** | `cuCtxCreate` → 2048² matmul (`cup8`, `bad=0 maxerr=0`, VERDICT PASS). Value is the **decision planes**, not replayability. |

Every header carries its own property vector, the declared filter, the guest/host driver
versions, the VBIOS md5 and an `nvidia-smi` summary — `rec_dump.py` prints it. All four are
`dense order: OK` with `n_errors=0`.

## Provenance

- Bench: vast.ai box, RTX 3060 = **GA106**, host driver **580.159.04 open**, host kernel
  6.8.0-59, QEMU 9.2.0.
- Guest: Ubuntu 24.04, kernel **6.8.0-117-generic** (the pin), **stock unpatched** open NVIDIA
  **580.159.04**, VBIOS `ga106_vbios.rom` md5 `48df40a04432aca6a35bee2785857eba`.
- Emulator source: `src/qemu/nvkvm_gpu_emul.c` md5 **`cced661c16f6856801d16dae151bc2f0`**,
  recorder `src/qemu/nvkvm_m2_rec.c` md5 **`d2ab3a95291396c0dce81e422a68e73a`** — the commit
  that adds this directory. (The bench tree is not a git checkout, so the header's
  `emulator-src-commit` line is empty by construction; the md5s are the binding.)

★ **The captured emulator source is `consolidation` HEAD, *not* the previously-validated
`862c7c2`.** `862c7c2` was the last revision this bench had ever compiled — every revision from
`3710b8e` on carries a duplicate forward declaration that is a `-Werror=redundant-decls` build
failure under the bench's QEMU 9.2 configure, which this task had to fix first. So the
`#14 P0/P1` work is in these traces and had never run on hardware before. It was re-validated
in the act of capturing: `cup2` rc=0 (cap2) and `cup8` `bad=0 maxerr=0` (cap3), both with the
recorder on.

## How to use the negative trace

`cap2b` is the one whose **passing condition is that the Rust differs**. The C's defect is
visible from the artefact alone, no log required — decode `rpc.function` at offset 60 of every
4096-byte `GuestRead`:

```
cap2b : 546 elements — 168 with a sane function id, 378 GARBAGE (28 distinct)
cap1  : 178 elements — 178 sane, 0 garbage
```

Those 378 are arbitrary guest RAM parsed as GSP RPC while `q_ready` pointed at a dead queue,
each answered `NV_OK`. The Rust must emit exactly one `Refused(QueueNotBound)` and **zero**
`ElementPosted` there, with `cap1`'s positive replay as the non-vacuity arm.

## What these traces cannot witness

Recorded before the capture, so a green diff is never mistaken for coverage:

1. **The completion plane has no C oracle at all.** The C never observes a host completion
   source; it forges completions. Nothing here constrains it.
2. **Three of the four traces are non-hermetic by construction.** With `m2fwd`/`m2exec` on, the
   stub `MAP_FIXED`s guest RAM and the **host GPU DMAs into it directly** — guest-visible bytes
   that pass through neither `nvkvm_dmaw` nor `nvkvm_dmar` nor any QEMU path. `pci_dma_map`
   (`nvkvm_gpu_emul.c`, `nvkvm_m2_share_guest_ram`) is the hole. Only `cap1` is closed.
3. **`cap1` is hermetic AND slightly counterfactual**: `m2fwd=off` is a path the C was never
   shipped on (its own property comment calls host-GPU forwarding "the ONLY supported Mode-2
   operating mode").
4. **One point on every axis**: one GPU, one host driver, one guest driver, one guest OS.
5. **`OverlaySnap` is absent from all four** (`m2romregs=off`). With the rom-device overlay on,
   the guest's reads of `IRQSTAT`/`MAILBOX0`/`CPUCTL`/`DMATRFCMD` do not trap at all and only
   the snapshot stands in for them.
