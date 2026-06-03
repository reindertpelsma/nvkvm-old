/*
 * nvkvm_gpu_emul.c — Mode-2 emulated NVIDIA GPU PCI device (M0).
 *
 * Mode-2 "reverse driver": the guest runs the real, stock NVIDIA driver against
 * this emulated GPU.  We "fake the boot" (never run a real GSP), translate the
 * addresses the driver programs, and forward real compute to the host GPU via
 * the Mode-1 core.  See docs/design/mode2_plan.md and
 * docs/design/nvidia_gpu_internals.md.
 *
 * M0 scope (this commit): present a self-consistent GA106 (RTX 3060) PCI
 * function with the real NVIDIA BAR layout and a BAR0 register aperture that
 *   (a) logs every access (offset, size, value, R/W) — the ground-truth trace
 *       of what RmInitAdapter wants, and
 *   (b) answers the chip-identity registers (NV_PMC_BOOT_0 / _42) so the
 *       driver's chip detection + HAL selection succeed and it proceeds far
 *       enough to expose the next stall.
 * Everything else reads back 0 for now.  The boot-register state machine
 * (M1/M2: GFW_BOOT, HWCFG2._RISCV, RISCV_STATUS, mailboxes) is stubbed behind
 * the reg_read/reg_write switch and filled in next.
 *
 * Design rules carried from the plan:
 *  - MULTI-GPU: per-instance state only.  No globals, no g_nvkvm_device
 *    singleton (the Mode-1 anti-pattern).  Every register block, BAR, and (later)
 *    RPC endpoint / isolate table is a field of NvkvmGpuEmul, so N of these can
 *    coexist in one VM, each bound to its own host GPU.
 *  - The chip identity lives in a single NvkvmGpuChip descriptor so PCI IDs,
 *    PMC_BOOT_0, PMC_BOOT_42 and HWCFG all describe the same silicon
 *    (spike §5.2 self-consistent identity).
 *
 * This is the thin C QEMU device shell.  The untrusted-input logic core
 * (GSP-RPC decode, address virtualization) will be Rust behind a narrow C ABI
 * (docs/design/mode2_plan.md "Language"); none of that exists yet at M0.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

/* ── Chip identity ─────────────────────────────────────────────────────────
 *
 * Single source of truth for "which silicon are we pretending to be".  GA106
 * (RTX 3060) by default so the in-guest driver selects the same HAL family
 * (ampere/ga10x) the host GPU uses and the downstream Mode-1 forwarding speaks
 * the same register/RPC dialect.
 *
 * NV_PMC_BOOT_0 (dev_boot / nv_ref.h):
 *   ARCHITECTURE_0 [28:24] = GA100 family = 0x17
 *   IMPLEMENTATION [23:20] = 6  (GA106; see g_hal_archimpl.h:76)
 *   MAJOR_REVISION [7:4], MINOR_REVISION [3:0] = stepping (0xA1)
 *   => 0x176000A1
 * NV_PMC_BOOT_42 (0xA00):
 *   ARCHITECTURE [29:24] = 0x17, IMPLEMENTATION [23:20] = 6,
 *   MAJOR_REVISION [19:16] = 0xA, MINOR_REVISION [15:12] = 1
 *   => 0x176A1000  (CHIP_ID [29:20] = 0x176)
 */
typedef struct NvkvmGpuChip {
    const char *name;
    uint16_t    vendor_id;        /* 0x10DE NVIDIA */
    uint16_t    device_id;        /* 0x2503 GA106 / RTX 3060 */
    uint16_t    sub_vendor_id;
    uint16_t    sub_device_id;
    uint8_t     revision;         /* PCI revision id (0xA1 for GA106-300) */
    uint32_t    pmc_boot_0;       /* BAR0 + 0x000 */
    uint32_t    pmc_boot_42;      /* BAR0 + 0xA00 */
    uint64_t    bar0_size;        /* REGS aperture (16 MiB) */
    uint64_t    bar1_size;        /* FB aperture   (256 MiB stub for M0) */
    uint64_t    bar3_size;        /* usermode/IMEM aperture (32 MiB) */
} NvkvmGpuChip;

static const NvkvmGpuChip nvkvm_chip_ga106 = {
    .name          = "GA106",
    .vendor_id     = 0x10DE,
    .device_id     = 0x2504,        /* RTX 3060 LHR — matches the dev host card */
    .sub_vendor_id = 0x1462,        /* MSI */
    .sub_device_id = 0x397D,        /* matches host SSID + dumped VBIOS PCIR */
    .revision      = 0xA1,
    .pmc_boot_0    = 0x176000A1u,
    .pmc_boot_42   = 0x176A1000u,
    .bar0_size     = 16ull  << 20,  /* 16 MiB  */
    .bar1_size     = 256ull << 20,  /* 256 MiB (real card is larger; stub) */
    .bar3_size     = 32ull  << 20,  /* 32 MiB  */
};

/* ── Register offsets ──────────────────────────────────────────────────────
 * (full glossary in docs/design/nvidia_gpu_internals.md §1.1) */
/* M0 — chip identity (dev_boot / nv_ref.h) */
#define NV_PMC_BOOT_0   0x00000000u
#define NV_PMC_BOOT_1   0x00000004u
#define NV_PMC_BOOT_42  0x00000A00u

/* M1 — GFW (GPU firmware) boot completion, spike check #1.
 * PGC6 AON secure-scratch (dev_gc6_island{,_addendum}.h, ga102).  The driver
 * (_gpuIsGfwBootCompleted_TU102) first checks the PLM was lowered, then reads
 * the GFW_BOOT progress and requires COMPLETED (0xFF). */
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK 0x00118128u
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT      0x00118234u /* GROUP_05(0) */
#define NV_PGC6_GFW_BOOT_PROGRESS_COMPLETED                 0x000000FFu

/* M2 — VBIOS PROM window.  The GSP RM init (kgspExtractVbiosFromRom_TU102)
 * reads the VBIOS image byte/dword-wise from NV_PROM_DATA(i) = 0x300000 + i in
 * BAR0, validating the PCI ROM signature, IFR header, PCIR struct and the
 * expansion-ROM chain.  We back this window with a real GA106 VBIOS dumped from
 * the host card (matching device id 0x2504), so the driver's parser is exact. */
#define NV_PROM_DATA_BASE 0x00300000u
#define NV_PROM_DATA_SIZE 0x00100000u   /* 1 MiB window (dumped image is padded) */

/* M2 — GSP falcon bring-up.  The driver resets/starts the GSP falcon to run
 * FWSEC/Booter, then kflcnWaitForHalt_TU102 spin-polls CPUCTL_HALTED.  In
 * fake-the-boot the falcon never runs, so we report it halted immediately.
 * GSP falcon register block = NV_PGSP base 0x110000; CPUCTL = base + 0x100. */
#define NV_PGSP_FALCON_CPUCTL        0x00110100u
#define NV_PFALCON_FALCON_CPUCTL_HALTED_TRUE 0x00000010u  /* bit 4 */

/* GSP falcon HWCFG2 (base+0xf4): kflcnIsRiscvCpuEnabled_TU102 requires
 * _RISCV=ENABLE (bit10); _MEM_SCRUBBING=DONE is value 0 (bit12 clear). So a
 * RISCV-capable, scrubbing-done config = bit10 set only. */
#define NV_PGSP_FALCON_HWCFG2        0x001100F4u
#define NV_PFALCON_FALCON_HWCFG2_RISCV_ENABLE_VAL 0x00000400u /* bit 10 */

/* M3 — GSP-RPC. The driver hands the LibOS init-args descriptor GPA via the
 * GSP falcon mailboxes (kgspProgramLibosBootArgsAddr_TU102). We capture it and
 * read the message-queue shared region from guest RAM. See
 * docs/design/mode2_m3_gsp_rpc.md. */
#define NV_PGSP_FALCON_MAILBOX0      0x00110040u
#define NV_PGSP_FALCON_MAILBOX1      0x00110044u
/* LibosMemoryRegionInitArgument: {u64 id8; u64 pa; u64 size; u8 kind; u8 loc;}
 * padded to 8 → 32-byte stride. */
#define LIBOS_REGION_STRIDE          32u
#define LIBOS_REGION_LOC_SYSMEM      1u  /* enum: NONE,SYSMEM,FB (loc) */

/* PTIMER — the GPU nanosecond clock.  On GA10x relocated to 0xbb0000:
 * TIME_0 (low) = 0xbb0080, TIME_1 (high) = 0xbb0084 (confirmed by the call
 * chain timeoutSet->tmrGetCurrentTimeEx->tmrGetTimeEx_GM107->_regRead).
 * CRITICAL: must return a real, monotonically increasing counter — every RM
 * timeout loop computes (now - start); a constant value never elapses and the
 * driver spins forever.  Back it with QEMU's virtual clock (ns). */
#define NV_PTIMER_TIME_0_GA10X       0x00BB0080u
#define NV_PTIMER_TIME_1_GA10X       0x00BB0084u

/* M3 — Falcon DMA (FWSEC/Booter ucode load).  s_dmaTransfer_GA102 polls
 * DMATRFCMD (falcon+0x118) for FULL==FALSE (queue not full) and IDLE==TRUE
 * (engine idle/done).  We don't run the falcons, so report the DMA always
 * idle+not-full => value 0x2 (IDLE=TRUE bit1, FULL=FALSE bit0).  FWSEC runs on
 * the GSP falcon (0x110000); Booter runs on SEC2 (0x840000).  CPUCTL+0x100
 * HALTED so kflcnWaitForHalt passes for both. */
#define NV_PFALCON_DMATRFCMD_IDLE_VAL 0x00000002u
#define NV_PGSP_FALCON_DMATRFCMD     0x00110118u
#define NV_PSEC_FALCON_DMATRFCMD     0x00840118u
#define NV_PSEC_FALCON_CPUCTL        0x00840100u
#define NV_PSEC_FALCON_HWCFG2        0x008400F4u

/* M3 — WPR2 (Write-Protect Region 2 in FB), set up by FWSEC.  After "running"
 * FWSEC the driver reads WPR2_ADDR_HI and requires _VAL (bits 31:4) != 0
 * (_kgspIsWpr2Initialized).  Report a plausible positive region [LO,HI] so the
 * check passes (we don't have real FB; the region is nominal in fake-the-boot).
 * dev_fb.h: WPR2_ADDR_LO=0x1FA824, WPR2_ADDR_HI=0x1FA828, 4KB-aligned. */
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO  0x001FA824u
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI  0x001FA828u
#define NVKVM_WPR2_LO_VAL            0x10000000u  /* nominal FB region base */
#define NVKVM_WPR2_HI_VAL            0x10100000u  /* base + ~16 MiB (HI>LO,!=0) */

/* ── Device state (per instance — multi-GPU safe) ──────────────────────────*/
#define TYPE_NVKVM_GPU_EMUL "nvkvm-gpu-emul"
OBJECT_DECLARE_SIMPLE_TYPE(NvkvmGpuEmul, NVKVM_GPU_EMUL)

#define NVKVM_GPU_MSIX_VECTORS 8   /* room for PMC top-level + per-engine */

struct NvkvmGpuEmul {
    PCIDevice parent_obj;

    /* identity */
    const NvkvmGpuChip *chip;

    /* BARs */
    MemoryRegion bar0;   /* REGS  — MMIO, trapped/logged                     */
    MemoryRegion bar1;   /* FB    — MMIO stub for M0 (address-virt layer L8) */
    MemoryRegion bar3;   /* IMEM/usermode — MMIO stub                        */
    MemoryRegion msix;   /* MSI-X table/PBA BAR (BAR5)                       */

    /* VBIOS served from the BAR0 PROM window (M2) */
    char    *vbios_path;     /* "vbios=" property: file with a real VBIOS dump */
    uint8_t *vbios;          /* loaded image (NV_PROM_DATA_SIZE bytes, padded)  */
    uint64_t prom_reads;     /* count (don't per-access trace — VBIOS is ~1 MiB)*/

    /* M3 — GSP-RPC message queue */
    uint32_t mbox0, mbox1;   /* GSP falcon mailbox halves (LibOS boot-args GPA) */
    bool     bootargs_dumped;/* one-shot: read+log the queue region once        */
    bool     fwsec_ran;      /* set when GSP falcon STARTCPU written: FWSEC "ran"
                              * -> WPR2 becomes "initialized" (stateful: the
                              * driver checks WPR2 DOWN before FWSEC, UP after).  */

    /* knobs */
    bool     trace;          /* log every BAR0 access                        */
    uint64_t access_count;   /* monotonically increasing, for the trace      */
};

/* ── BAR0 register aperture ────────────────────────────────────────────────*/

static const char *nvkvm_reg_name(hwaddr off)
{
    switch (off) {
    case NV_PMC_BOOT_0:  return "PMC_BOOT_0";
    case NV_PMC_BOOT_1:  return "PMC_BOOT_1";
    case NV_PMC_BOOT_42: return "PMC_BOOT_42";
    case NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK: return "GFW_BOOT_PLM";
    case NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT:      return "GFW_BOOT";
    case NV_PGSP_FALCON_CPUCTL:                              return "GSP_CPUCTL";
    case NV_PGSP_FALCON_HWCFG2:                              return "GSP_HWCFG2";
    case NV_PTIMER_TIME_0_GA10X:                            return "PTIMER_TIME_0";
    case NV_PTIMER_TIME_1_GA10X:                            return "PTIMER_TIME_1";
    case NV_PGSP_FALCON_DMATRFCMD:                          return "GSP_DMATRFCMD";
    case NV_PSEC_FALCON_DMATRFCMD:                          return "SEC_DMATRFCMD";
    case NV_PSEC_FALCON_CPUCTL:                             return "SEC_CPUCTL";
    default:             return NULL;
    }
}

/* M0: identity registers answered; everything else reads 0.  M1/M2 extend this
 * switch into the fake-the-boot state machine (GFW_BOOT, HWCFG2, RISCV_STATUS,
 * FWSEC/Booter mailboxes). */
static uint64_t nvkvm_reg_read(NvkvmGpuEmul *s, hwaddr off, unsigned size)
{
    switch (off) {
    case NV_PMC_BOOT_0:  return s->chip->pmc_boot_0;
    case NV_PMC_BOOT_42: return s->chip->pmc_boot_42;
    case NV_PMC_BOOT_1:  return 0; /* VGPU=REAL, no virtualization advertised */

    /* M1 — fake the GFW boot.  PLM "fully lowered" (all privilege levels
     * granted: bit0 READ_PROTECTION_LEVEL0 must be ENABLE); GFW_BOOT progress
     * COMPLETED so gpuWaitForGfwBootComplete_TU102 succeeds. */
    case NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK: return 0xFFFFFFFFu;
    case NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT:
        return NV_PGC6_GFW_BOOT_PROGRESS_COMPLETED;

    /* M2 — GSP falcon already halted (FWSEC/Booter "finished"). */
    case NV_PGSP_FALCON_CPUCTL: return NV_PFALCON_FALCON_CPUCTL_HALTED_TRUE;
    /* M2 — GSP falcon has a RISC-V core, memory scrubbing done. */
    case NV_PGSP_FALCON_HWCFG2: return NV_PFALCON_FALCON_HWCFG2_RISCV_ENABLE_VAL;

    /* M3 — PTIMER (GPU ns clock). Real monotonic counter from QEMU's virtual
     * clock so RM timeout loops actually elapse (constant value => infinite
     * spin). TIME_0 low 32 (5-bit aligned), TIME_1 high 32. */
    case NV_PTIMER_TIME_0_GA10X:
        return (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) & 0xFFFFFFE0u;
    case NV_PTIMER_TIME_1_GA10X:
        return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >> 32);

    /* M3 — Falcon DMA always idle+not-full (FWSEC on GSP, Booter on SEC2). */
    case NV_PGSP_FALCON_DMATRFCMD:
    case NV_PSEC_FALCON_DMATRFCMD: return NV_PFALCON_DMATRFCMD_IDLE_VAL;
    /* SEC2 falcon halted (Booter "finished"); SEC2 has no RISC-V advertised. */
    case NV_PSEC_FALCON_CPUCTL:    return NV_PFALCON_FALCON_CPUCTL_HALTED_TRUE;
    case NV_PSEC_FALCON_HWCFG2:    return 0;

    /* M3 — WPR2 is stateful: DOWN (0) until FWSEC "runs" (GSP STARTCPU), then
     * UP.  The driver requires WPR2 down before FWSEC, up after. */
    case NV_PFB_PRI_MMU_WPR2_ADDR_LO: return s->fwsec_ran ? NVKVM_WPR2_LO_VAL : 0;
    case NV_PFB_PRI_MMU_WPR2_ADDR_HI: return s->fwsec_ran ? NVKVM_WPR2_HI_VAL : 0;

    default:             return 0;
    }
}

/* PROM window: return VBIOS bytes (little-endian dword at the aligned offset).
 * Not traced per-access — the driver streams the whole ~1 MiB image. */
static bool nvkvm_prom_read(NvkvmGpuEmul *s, hwaddr off, unsigned size,
                            uint64_t *out)
{
    if (!s->vbios || off < NV_PROM_DATA_BASE ||
        off >= NV_PROM_DATA_BASE + NV_PROM_DATA_SIZE) {
        return false;
    }
    hwaddr p = off - NV_PROM_DATA_BASE;
    uint64_t v = 0;
    for (unsigned i = 0; i < size && p + i < NV_PROM_DATA_SIZE; i++) {
        v |= (uint64_t)s->vbios[p + i] << (8 * i);
    }
    s->prom_reads++;
    *out = v;
    return true;
}

static uint64_t nvkvm_bar0_read(void *opaque, hwaddr off, unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    uint64_t prom;
    if (nvkvm_prom_read(s, off, size, &prom)) {
        return prom;
    }
    uint64_t val = nvkvm_reg_read(s, off, size);

    /* Don't trace PTIMER reads — RM timeout loops poll them millions of times. */
    if (s->trace && off != NV_PTIMER_TIME_0_GA10X &&
        off != NV_PTIMER_TIME_1_GA10X) {
        const char *nm = nvkvm_reg_name(off);
        qemu_log("nvkvm-gpu[%s] #%llu BAR0 RD  off=0x%06llx sz=%u -> 0x%08llx%s%s\n",
                 s->chip->name, (unsigned long long)s->access_count++,
                 (unsigned long long)off, size, (unsigned long long)val,
                 nm ? "  " : "", nm ? nm : "");
    }
    return val;
}

/* M3-step-1: read the LibOS init-args region array from guest RAM at the GPA
 * the driver programmed into the mailboxes, log each region, and (for the
 * SYSMEM message-queue region) dump the command-queue msgqTxHeader.  This
 * proves the GPA path and gives ground truth before we synthesize responses. */
static void nvkvm_m3_dump_bootargs(NvkvmGpuEmul *s)
{
    uint64_t gpa = ((uint64_t)s->mbox1 << 32) | s->mbox0;
    PCIDevice *pdev = &s->parent_obj;

    qemu_log("nvkvm-gpu[%s] M3: LibOS boot-args GPA = 0x%016llx\n",
             s->chip->name, (unsigned long long)gpa);
    if (gpa == 0) {
        return;
    }

    for (int i = 0; i < 16; i++) {
        uint8_t e[LIBOS_REGION_STRIDE];
        if (pci_dma_read(pdev, gpa + (uint64_t)i * LIBOS_REGION_STRIDE,
                         e, sizeof(e)) != MEMTX_OK) {
            qemu_log("nvkvm-gpu[%s] M3:  region[%d] read failed\n",
                     s->chip->name, i);
            break;
        }
        uint64_t id8  = ldq_le_p(e + 0);
        uint64_t pa   = ldq_le_p(e + 8);
        uint64_t sz   = ldq_le_p(e + 16);
        uint8_t  kind = e[24];
        uint8_t  loc  = e[25];
        if (id8 == 0 && pa == 0 && sz == 0) {
            break; /* end of array */
        }
        qemu_log("nvkvm-gpu[%s] M3:  region[%d] id8=0x%016llx pa=0x%016llx "
                 "size=0x%llx kind=%u loc=%u\n", s->chip->name, i,
                 (unsigned long long)id8, (unsigned long long)pa,
                 (unsigned long long)sz, kind, loc);

        /* The message-queue shared region is the SYSMEM one; peek its
         * command-queue msgqTxHeader (32 bytes at pa). */
        if (loc == LIBOS_REGION_LOC_SYSMEM && sz >= 0x40000) {
            uint8_t h[32];
            if (pci_dma_read(pdev, pa, h, sizeof(h)) == MEMTX_OK) {
                qemu_log("nvkvm-gpu[%s] M3:   cmdq txHdr ver=%u size=0x%x "
                         "msgSize=%u msgCount=%u writePtr=%u flags=0x%x "
                         "rxHdrOff=0x%x entryOff=0x%x\n", s->chip->name,
                         ldl_le_p(h+0), ldl_le_p(h+4), ldl_le_p(h+8),
                         ldl_le_p(h+12), ldl_le_p(h+16), ldl_le_p(h+20),
                         ldl_le_p(h+24), ldl_le_p(h+28));
            }
        }
    }
}

static void nvkvm_bar0_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    NvkvmGpuEmul *s = opaque;

    /* M3: GSP falcon STARTCPU => FWSEC "executes" => WPR2 becomes initialized.
     * (CPUCTL bit1 STARTCPU, or via CPUCTL_ALIAS 0x110130.) */
    if ((off == NV_PGSP_FALCON_CPUCTL || off == 0x00110130u) && (val & 0x2u)) {
        if (!s->fwsec_ran) {
            s->fwsec_ran = true;
            if (s->trace) {
                qemu_log("nvkvm-gpu[%s] M3: GSP STARTCPU -> FWSEC ran, WPR2 up\n",
                         s->chip->name);
            }
        }
    }

    /* M3: capture the LibOS boot-args GPA from the GSP falcon mailboxes. */
    if (off == NV_PGSP_FALCON_MAILBOX0) {
        s->mbox0 = (uint32_t)val;
    } else if (off == NV_PGSP_FALCON_MAILBOX1) {
        s->mbox1 = (uint32_t)val;
        if (!s->bootargs_dumped && (s->mbox0 | s->mbox1)) {
            s->bootargs_dumped = true;
            nvkvm_m3_dump_bootargs(s);
        }
    }

    if (s->trace) {
        const char *nm = nvkvm_reg_name(off);
        qemu_log("nvkvm-gpu[%s] #%llu BAR0 WR  off=0x%06llx sz=%u <- 0x%08llx%s%s\n",
                 s->chip->name, (unsigned long long)s->access_count++,
                 (unsigned long long)off, size, (unsigned long long)val,
                 nm ? "  " : "", nm ? nm : "");
    }
    /* M0: writes are observed only.  M1/M2 add the state machine. */
}

static const MemoryRegionOps nvkvm_bar0_ops = {
    .read       = nvkvm_bar0_read,
    .write      = nvkvm_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl       = { .min_access_size = 4, .max_access_size = 4 },
    .valid      = { .min_access_size = 1, .max_access_size = 8 },
};

/* BAR1 (FB) and BAR3 (usermode/IMEM): M0 stubs that log and read 0.  These
 * become the address-virtualization windows backed by real host-isolate
 * mappings (parity; see mode2_perf_dma_multigpu) at M4/M5. */
static uint64_t nvkvm_baraperture_read(void *opaque, hwaddr off, unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    if (s->trace) {
        qemu_log("nvkvm-gpu[%s] #%llu APER RD  off=0x%llx sz=%u -> 0\n",
                 s->chip->name, (unsigned long long)s->access_count++,
                 (unsigned long long)off, size);
    }
    return 0;
}

static void nvkvm_baraperture_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    if (s->trace) {
        qemu_log("nvkvm-gpu[%s] #%llu APER WR  off=0x%llx sz=%u <- 0x%llx\n",
                 s->chip->name, (unsigned long long)s->access_count++,
                 (unsigned long long)off, size, (unsigned long long)val);
    }
}

static const MemoryRegionOps nvkvm_aperture_ops = {
    .read       = nvkvm_baraperture_read,
    .write      = nvkvm_baraperture_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = { .min_access_size = 1, .max_access_size = 8 },
};

/* ── realize / unrealize ───────────────────────────────────────────────────*/

static void nvkvm_gpu_emul_realize(PCIDevice *pci_dev, Error **errp)
{
    NvkvmGpuEmul *s = NVKVM_GPU_EMUL(pci_dev);
    const NvkvmGpuChip *chip = &nvkvm_chip_ga106;
    uint8_t *cfg = pci_dev->config;

    s->chip = chip;
    s->access_count = 0;
    s->prom_reads = 0;
    s->mbox0 = 0;
    s->mbox1 = 0;
    s->bootargs_dumped = false;
    s->fwsec_ran = false;

    /* M2: load the VBIOS image for the PROM window (if a path was given). */
    s->vbios = NULL;
    if (s->vbios_path && s->vbios_path[0]) {
        FILE *f = fopen(s->vbios_path, "rb");
        if (!f) {
            error_setg(errp, "nvkvm-gpu-emul: cannot open vbios '%s'",
                       s->vbios_path);
            return;
        }
        s->vbios = g_malloc0(NV_PROM_DATA_SIZE);
        size_t n = fread(s->vbios, 1, NV_PROM_DATA_SIZE, f);
        fclose(f);
        if (s->vbios[0] != 0x55 || s->vbios[1] != 0xAA) {
            warn_report("nvkvm-gpu-emul: vbios '%s' lacks 0x55AA signature "
                        "(read %zu bytes)", s->vbios_path, n);
        }
    }

    /* Class 0x030000 = VGA-compatible 3D controller, as a real GeForce reports.
     * (Mode-2's "display for free" rides on this.) */
    pci_set_word(cfg + PCI_SUBSYSTEM_VENDOR_ID, chip->sub_vendor_id);
    pci_set_word(cfg + PCI_SUBSYSTEM_ID,        chip->sub_device_id);

    /* BAR0: REGS, 32-bit non-prefetchable MMIO (matches real GA10x). */
    memory_region_init_io(&s->bar0, OBJECT(s), &nvkvm_bar0_ops, s,
                          "nvkvm-gpu-regs", chip->bar0_size);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* BAR1: FB aperture, 64-bit prefetchable (occupies BAR1+BAR2). */
    memory_region_init_io(&s->bar1, OBJECT(s), &nvkvm_aperture_ops, s,
                          "nvkvm-gpu-fb", chip->bar1_size);
    pci_register_bar(pci_dev, 1,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->bar1);

    /* BAR3: usermode/IMEM aperture, 64-bit prefetchable (occupies BAR3+BAR4).
     * Driver assigns FB then IMEM to the next valid 64-bit BARs after REGS. */
    memory_region_init_io(&s->bar3, OBJECT(s), &nvkvm_aperture_ops, s,
                          "nvkvm-gpu-usermode", chip->bar3_size);
    pci_register_bar(pci_dev, 3,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->bar3);

    /* MSI-X (BAR5): the driver expects message interrupts (GSP->CPU doorbell,
     * engine completion).  Table lives in its own BAR; we raise vectors from
     * host eventfd/poll later (mode2_interrupt_delivery). */
    memory_region_init(&s->msix, OBJECT(s), "nvkvm-gpu-msix", 0x1000);
    pci_register_bar(pci_dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->msix);
    if (msix_init(pci_dev, NVKVM_GPU_MSIX_VECTORS,
                  &s->msix, 5, 0x0,
                  &s->msix, 5, 0x800,
                  0x00, errp) < 0) {
        /* Non-fatal for M0 observation: fall back to INTx so the device still
         * enumerates and we capture the BAR0 trace. */
        error_free(*errp);
        *errp = NULL;
    } else {
        for (int v = 0; v < NVKVM_GPU_MSIX_VECTORS; v++) {
            msix_vector_use(pci_dev, v);
        }
    }

    /* PCIe capability so the driver sees an Express endpoint, not legacy PCI.
     * Only valid when actually on a PCIe bus (q35 root complex / root port);
     * on a conventional PCI bus we enumerate as a plain PCI device.  A real
     * GeForce is express, so prefer plugging this behind a pcie-root-port. */
    if (pci_is_express(pci_dev)) {
        pcie_endpoint_cap_init(pci_dev, 0x60);
    }
}

static void nvkvm_gpu_emul_exit(PCIDevice *pci_dev)
{
    NvkvmGpuEmul *s = NVKVM_GPU_EMUL(pci_dev);
    msix_unuse_all_vectors(pci_dev);
    msix_uninit(pci_dev, &s->msix, &s->msix);
    g_free(s->vbios);
}

/* ── QOM boilerplate ───────────────────────────────────────────────────────*/

static Property nvkvm_gpu_emul_props[] = {
    DEFINE_PROP_BOOL("trace", NvkvmGpuEmul, trace, true),
    DEFINE_PROP_STRING("vbios", NvkvmGpuEmul, vbios_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvkvm_gpu_emul_class_init(ObjectClass *klass, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);
    const NvkvmGpuChip *chip = &nvkvm_chip_ga106;

    k->realize   = nvkvm_gpu_emul_realize;
    k->exit      = nvkvm_gpu_emul_exit;
    k->vendor_id = chip->vendor_id;
    k->device_id = chip->device_id;
    k->revision  = chip->revision;
    k->class_id  = PCI_CLASS_DISPLAY_VGA;   /* 0x0300; we override prog-if=0 */

    dc->desc = "nvkvm Mode-2 emulated NVIDIA GPU (GA106)";
    device_class_set_props(dc, nvkvm_gpu_emul_props);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo nvkvm_gpu_emul_info = {
    .name          = TYPE_NVKVM_GPU_EMUL,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NvkvmGpuEmul),
    .class_init    = nvkvm_gpu_emul_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void nvkvm_gpu_emul_register_types(void)
{
    type_register_static(&nvkvm_gpu_emul_info);
}
type_init(nvkvm_gpu_emul_register_types)
