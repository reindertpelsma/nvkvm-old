# Mode-2 M6: BAR2 GMMU page-walk (address-virtualization keystone)

> ### STATUS — 2026-08-11 (w258 doc-hygiene sweep) / **ANSWERED 2026-06-03 — "NEXT" below is STALE by ~2 months**
>
> ⊘ **The literal string "Status: NEXT" below has read as current for two months.** It was true
> for about 28 minutes. Determined from git history, not from the prose:
>
> - ★ **The gate was cracked the same day.** `acaabbf` (2026-06-03, 28 min after this doc's only
>   commit `d155d74`) wired the VER2 walk to PCI BAR3; `fc067d0` (2026-06-03) is *"**BAR2 GMMU
>   works — kbusVerifyBar2 PASSES (address-virt gate cracked)**"*. The "remaining sub-test" this
>   doc names as the open item is closed.
> - ⊘ **The mechanism below also changed.** This doc proposes capturing the instance block via the
>   `0x1714` write. `da6a366` records that attempt as *"instblk PRAMIN-snoop attempt (**did NOT
>   fire**)"*, and `af05c87` re-sourced the PDB from `GspStaticConfigInfo.bar2PdeBase` instead.
>   ⇒ Read the walk description as background; do not implement the capture step.
>
> ⇒ Mode-2 went far past this rung: the C artifact reproduced `cuCtxCreate → 2048² matmul` at
> `bad=0 maxerr=0` on a stock guest (`CLAUDE.md`; `traces/mode2_c_reference/cap3_matmul_forwarding`).

Status: NEXT. The fake-boot + full GSP RPC/control replay is complete (the stock
driver runs to `kbusVerifyBar2`). The BAR0 PRAMIN window + sparse FB backing are
implemented (commit 8de2074) and the **first** kbusVerifyBar2 sub-test (BAR0
window write/read of 0xabcdabcd) PASSES. The remaining sub-test is the **MMU
test**, which needs the BAR2 aperture to translate guest accesses to FB physical
addresses via the guest-programmed page tables. This is the heart of Mode-2.

## What kbusVerifyBar2 MMU test does (kern_bus_gm107.c)
1. Maps a test page into BAR2 (`kbusMapRmAperture`) → CPU ptr `pOffset` into the
   BAR2 MMIO region; the page's FB phys addr = `testMemoryOffset`.
2. Writes `0xabcdabcd` through BAR2 (`MEM_WR32(pOffset+i)`).
3. Reads back through the **BAR0 PRAMIN window** at `testMemoryOffset` (which we
   already back). Garbage 0x0 today because BAR2 writes go nowhere.
4. Then writes `SAMPLEDATA+0x10` via PRAMIN and reads it back via BAR2.
So we must make BAR2[offset] read/write land in `fb_pages[ translate(offset) ]`.

## How BAR2 is programmed (to implement the walk)
- `NV_PBUS_BAR2_BLOCK` (BAR0 reg **0x1714**): PTR[27:0] = instance-block address
  (units per TARGET), TARGET[29:28] (VID_MEM=0), MODE[31] (0=PHYSICAL,1=VIRTUAL).
  Capture this write in nvkvm_bar0_write.
- The **instance block** (in FB, 1 KiB) holds the page-directory base at offsets
  `NV_RAMIN_PAGE_DIR_BASE_LO/HI/TARGET` (dev_ram.h, accessed via SF_OFFSET) and
  `NV_RAMIN_ADR_LIMIT_LO/HI`. Read the PDB (lo|hi<<32, in FB) from the FB backing
  at instBlockBase.
- Page tables are **NV_MMU_VER2** (GA10x): 8-byte PDE/PTE; multi-level
  (PD3→PD2→PD1→PD0→PTE) for the 4 KiB/64 KiB page sizes. Formats in
  `src/common/inc/swref/published/ampere/ga100/dev_mmu.h` (NV_MMU_VER2_PTE_*,
  NV_MMU_VER2_DUAL_PDE_* / NV_MMU_VER2_PDE_*). PTE has VALID, APERTURE
  (VID_MEM/SYS), ADDRESS (phys page >> shift). Big-vs-small page dir is the DUAL
  PDE (two sub-pointers per PDE entry).

## Implementation plan (nvkvm_gpu_emul.c)
1. Track `bar2_inst_block` from the 0x1714 write (PTR<<12 = FB addr of instblk).
2. `nvkvm_bar2_translate(s, bar2_off)`: read PDB from FB[instblk + RAMIN_PAGE_DIR_BASE],
   walk VER2 levels reading each PDE/PTE 8-byte entry from `fb_pages`, return the
   FB phys addr (or fault). Cache the last PDE/PTE if needed.
3. In the **BAR1/BAR2 aperture MMIO ops** (currently `nvkvm_aperture_ops` stub),
   on read/write: translate via the walk and hit `nvkvm_fb_{read,write}`.
   - BAR2 is the small aperture used for RM instance/page-table access;
     `NV_PBUS_BAR1_BLOCK` (0x1704) is the analogous BAR1 PDB if BAR1 is exercised.
4. Validate against kbusVerifyBar2: after this, the MMU test round-trips and
   kbusStateInitLocked returns NV_OK → driver proceeds past BUS init.

## Notes
- All page-table memory the driver writes goes through the PRAMIN window (already
  backed) or BAR2 itself, so once the walk reads from `fb_pages`, it sees the real
  entries the driver wrote.
- This is the generalizable address-virtualization core (per docs/design/mode2_plan.md
  "hardest = guest-PTE↔host DMA xlate"): the same walk later translates channel
  pushbuffers / USERD / compute allocations for first compute (M6/M7).
- Reference gVisor nvproxy does NOT do this (it forwards ioctls); the real
  reference is the open-gpu-kernel-modules GMMU code + dev_mmu.h, and nouveau's
  nvkm/subdev/mmu for an independent VER2 walk implementation.
