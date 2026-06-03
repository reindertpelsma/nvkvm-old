# Mode-2 device data model + auto-generation from NVIDIA source

## Goal (user, 2026-06-03)
What matters is **not** that we impersonate the exact host GPU — it's that **CUDA
compiles and runs in the guest** against whatever GPU we advertise. Implications:
- We advertise ONE real, supported NVIDIA GPU model, self-consistently.
- Because Mode-2 forwards real *compute* to a host GPU, the advertised **compute
  capability (arch) should match the host compute GPU** so generated SASS runs
  (or rely on PTX JIT). The *board/SKU* details need not match.
- So: pick a supported chip per host-arch; make its data a clean, swappable set.

## The data, classified by SOURCE (this is the crux of "auto-generate")

**A. SOURCE-DERIVABLE — auto-generatable from open-gpu-kernel-modules + SDK headers.**
These are constants/layouts NVIDIA publishes per-arch; a generator parses them so
they **auto-update when NVIDIA ships a new arch's headers**:
- Register offsets: NV_PRAMIN, NV_PBUS_BAR0_WINDOW/BAR2_BLOCK, NV_PMC_BOOT_0/42,
  NV_PTIMER, GSP/SEC falcon bases, WPR2 regs, GFW_BOOT, NV_USABLE_FB_SIZE_IN_MB…
  (swref/published/<arch>/<chip>/dev_*.h)
- GMMU page-table format + geometry (NV_MMU_VER2/VER3 PTE/PDE bit fields, per-level
  VA bit ranges) — dev_mmu.h + kern_gmmu_fmt_*.c
- Instance-block field offsets (NV_RAMIN_PAGE_DIR_BASE_*) — dev_ram.h
- Chip identity (PMC_BOOT_0 value, PCI device IDs) — partly source, partly public DB
- Per-class alloc-param sizes, control struct sizes — SDK ctrl/cl headers
=> Build `tools/mode2_gen_regs.py`: input = an OGKM checkout + target chip; output =
   `data/mode2/<arch>/regs.h` (offset/format tables). Adding a new card NVIDIA's
   open driver supports = run the generator against that driver tree. This directly
   satisfies "offsets auto-update for new cards."

**B. CAPTURE-REQUIRED — GSP-firmware/hardware-derived runtime values.**
These are computed by GSP firmware at boot from **silicon fuses + board config**;
they are NOT static tables in source, so they cannot be "generated", only captured
(their *structure* is in source, so we parse/validate the captured bytes):
- Device-info engine table (GET_DEVICE_INFO_TABLE): engine list w/ runlist/fault/
  pbdma IDs — fuse/floorsweeping dependent.
- Interrupt table (INTR_GET_KERNEL_TABLE): MC vectors per engine.
- GSP static config (GET_GSP_STATIC_INFO): FB regions, fb size, ECC, board name,
  fuses.
=> One-time capture per supported card via scripts/mode2_capture_host.sh (the
   streamed-dmesg harness). Stored as `data/mode2/<chip>/{devinfo,intr,gspstatic}.bin`.
   We CAN partially derive structure/engine-presence from the HAL in source to
   sanity-check or synthesize a plausible table when no card is available, but the
   authoritative values are captured.

**C. CONTROL RESPONSES (the 56 init controls)** — mostly chip-static; captured today.
Many could be derived (caps/clock tables) but capture is the reliable baseline.

## Honest summary for the user's question
- "Auto-update offsets when NVIDIA releases a new card" → **YES**, fully: a header
  generator over the open driver's swref tree (class A). This is the high-value,
  clearly-doable piece.
- "Generate the whole per-device dataset from source" → **partially**: classes A+C
  largely yes; class B (devinfo/intr/gspstatic) are physically fuse/firmware-derived
  and need a one-time capture per card (structure validated against source). That's
  not a limitation of our approach — those bytes don't exist as static source.

## Repo layout (target)
```
data/mode2/
  <arch>/regs.h              # generated (class A): offsets + GMMU format
  <chip>/devinfo.bin         # captured (class B)
  <chip>/intrtable.bin
  <chip>/gspstaticinfo.bin
  <chip>/initctrl/*.bin      # captured (class C), per cmd
  <chip>/manifest.json       # chip id, arch, source driver ver, capture date
tools/
  mode2_gen_regs.py          # parse OGKM swref/sdk -> data/mode2/<arch>/regs.h
  mode2_capture_to_data.py   # parse streamed-dmesg capture -> data/mode2/<chip>/*.bin
```
The emulator loads the chip set selected by a device property (default: detected
host-arch's reference chip), instead of the current hardcoded GA106 #includes.

## Migration from today
Current: per-chip data is hand-generated C headers (mode2_{devinfo,intrtable,
initctrl,gspstaticinfo}_ga106.h) #included directly. Step 1 = move the captured
bytes to data/mode2/ga106/*.bin + a loader; Step 2 = write mode2_gen_regs.py and
replace the hardcoded register offsets/GMMU constants with generated tables; Step 3
= a runtime/property chip selector. The VBIOS placeholder (mode2_generality_and_vbios)
is the redistribution-safe analog for the PROM window.
```
