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
#include "mode2_devinfo_ga106.h"   /* captured GA106 engine table (M5 replay) */
#include "mode2_initctrl_ga106.h"  /* captured GA106 init-control responses    */
#include "mode2_intrtable_ga106.h" /* captured GA106 interrupt table (M5)      */
#include "mode2_gspstaticinfo_ga106.h" /* captured GA106 GSP static config (M5) */
#include "mode2_compute_ctrls_ga106.h" /* captured GA106 cuInit compute-cap ctrls */

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

#include "mode2_regs_ga10x.h"  /* GA10x register offsets + GMMU VER2 format */

/* Max bytes for a single GSP RPC response message (header + body + params),
 * spanning multiple 4 KiB queue elements.  GET_DEVICE_INFO_TABLE is the largest
 * at paramsSize=24580 (+120 hdr); round up with headroom. */
#define NVKVM_RESP_MAX 40960u

/* GMMU walk "no translation" sentinel (returned by nvkvm_{bar2,chan}_translate). */
#define NVKVM_GMMU_FAULT        (~0ull)

/* DIAG (removable): low-FB window where the UVM/RM-internal channel's
 * GPFIFO/USERD/instblk/semaphore are allocated (observed 0x311xxxx..0x315xxxx). */
#define NVKVM_DIAG_LOFB_LO      0x3000000ull
#define NVKVM_DIAG_LOFB_HI      0x3300000ull

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
    bool     gsp_suspended;  /* set on fn=47 UNLOADING teardown.  kgspUnloadRm ->
                              * kgspWaitForProcessorSuspend polls FALCON_MAILBOX0
                              * for INTERRUPT_PROCESSOR_SUSPENDED_VALUE(0x80000000);
                              * the faked GSP must report suspended or the close
                              * hangs 4s (_threadNodeCheckTimeout) and WPR2 stays
                              * up -> next open EIO (WPR2 re-boot cascade).        */

    /* M4 — GSP-RM RPC shim. Cached message-queue layout (from RMARGS) + ring
     * state. The driver posts a command on the cmd queue then writes the cmd
     * QUEUE_HEAD doorbell (0x110c00); we read the command and post an echo
     * response (rpc_result=NV_OK) on the status queue. */
    bool     q_ready;        /* queue layout cached, GSP_INIT_DONE posted        */
    uint64_t q_shmem;        /* message-queue shared region GPA                  */
    uint32_t q_cmd_base;     /* cmdQueueOffset (cmd queue backing store offset)  */
    uint32_t q_stat_base;    /* statQueueOffset (status queue backing store off) */
    uint32_t q_msgsize;      /* msgq entry size (GSP_MSG_QUEUE_ELEMENT_SIZE_MIN) */
    uint32_t q_msgcount;     /* entries per queue                                */
    uint32_t q_cmd_entryoff; /* cmd queue entries offset                         */
    uint32_t q_stat_entryoff;/* status queue entries offset                      */
    uint32_t stat_writeptr;  /* status queue monotonic writePtr (in ELEMENTS)    */
    uint32_t stat_seqnum;    /* per-MESSAGE seqNum (guest rxSeqNum, +1 per reply) */
    uint32_t cmd_readptr;    /* cmd queue messages we've consumed/answered        */

    /* M6 — GPU memory: sparse FB backing + BAR0 PRAMIN window.  The driver
     * accesses VRAM before BAR2 is up via a 1MB window in BAR0 (NV_PRAMIN @
     * 0x700000): it programs NV_PBUS_BAR0_WINDOW (0x1700) BASE=FBaddr>>16,
     * TARGET=aperture, then reads/writes NV_PRAMIN+(FBaddr&0xffff).  We back
     * that with a sparse page table so writes read back (kbusVerifyBar2). */
    uint32_t bar0_window;    /* NV_PBUS_BAR0_WINDOW (0x1700): BASE[23:0]|TARGET[25:24] */
    GHashTable *fb_pages;    /* sparse FB: page index (addr>>12) -> malloc'd 4KB  */
    bool     bar2_virtual;   /* BAR2_BLOCK MODE bit31: 1=VIRTUAL (walk), 0=PHYSICAL (id) */
    uint64_t bar2_pdb;       /* BAR2 page-dir base from GspStaticConfigInfo.bar2PdeBase
                              * (the GSP binds BAR2; CPU bind is a no-op on GSP-client) */
    uint64_t bar2_inst_block;/* FB addr of the BAR2 instance block (NV_PBUS_BAR2_BLOCK
                              * 0x1714: PTR[27:0]<<12).  Holds the BAR2 page-dir base;
                              * BAR2 accesses are GMMU-VER2-walked through it.        */
    uint64_t bar1_pdb;       /* BAR1 (FB aperture) page-dir base from
                              * GspStaticConfigInfo.bar1PdeBase (offset 1664) + the
                              * UPDATE_BAR_PDE(BAR_1) root entry.  The driver maps
                              * channel USERD/pushbuffers into BAR1 and the CPU
                              * writes GP_PUT through it — must GMMU-walk to FB.     */

    /* M5 channel tracking: captured from the most-recent *_CHANNEL_GPFIFO_A
     * GSP_RM_ALLOC (fn 103).  During init there is a single CE channel (the
     * scrubber), so the doorbell-rung channel is the last one allocated.  These
     * locate the GPFIFO ring so the doorbell handler can walk submitted work. */
    uint64_t chan_gpfifo_va;   /* gpFifoOffset: GPU VA of the channel's GPFIFO ring */
    uint32_t chan_gpfifo_ent;  /* gpFifoEntries */
    uint32_t chan_class;       /* hClass of the tracked channel                     */
    uint64_t chan_inst_block;  /* instanceMem.base: channel instance block (unused: GSP-managed, empty) */
    bool     chan_inst_sys;    /* instanceMem.addressSpace == ADDR_SYSMEM(1)        */
    uint64_t chan_pdb;         /* PDB read from the executing channel's instance
                                * block (RAMIN +0x200): HW-authoritative VAS root.
                                * 0 if the instblk is empty (GSP-managed) -> fall
                                * back to the snooped chan_vas[] heuristic.        */
    uint32_t chan_payload;     /* completion payload counter (incr per doorbell)    */
    bool     chan_sem_released; /* set by chan_execute when it honored an explicit
                                  * NVC56F SEM_EXECUTE release from the pushbuffer    */
    uint64_t chan_userd;       /* userdMem.base: USERD memory (holds GP_PUT/GP_GET) */
    bool     chan_userd_sys;   /* userdMem.addressSpace == ADDR_SYSMEM              */
    uint32_t chan_gp_get;      /* our consumed GPFIFO index (entries [get,put) pend)*/

    /* Multi-channel table.  Init allocates several GPFIFO channels (e.g. the
     * CeUtils memory scrubber AND its self-verify channel), so a doorbell can
     * target ANY of them — not just the most-recently allocated one (which the
     * single chan_* fields above tracked, dropping the scrubber's work -> the
     * ce_utils.c:349 timeout).  On a doorbell we walk EVERY channel's pending
     * GPFIFO so we never need to map the doorbell token's chid to a channel. */
#define NVKVM_MAX_CHANS 32
    struct nvkvm_chan_entry {
        uint64_t gpfifo_va, userd;
        uint32_t gpfifo_ent, gp_get, hvaspace, payload;
        uint32_t client;        /* owning RM client (hClient) — VAS scope key */
        bool     userd_sys;
    } chans[NVKVM_MAX_CHANS];
    int chan_n;
    uint32_t chan_client;       /* working-set: client of the channel chan_exec runs */

    /* ── Address-virtualization #2 side-table (the reverse-driver core) ────────
     * For GSP-managed VASes the leaf PTEs are filled GSP-side and never land in
     * our FB, so nvkvm_walk_pdb FAULTs.  Instead we reconstruct GPU-VA -> physical
     * from the RM op that establishes the mapping: NV2080_CTRL_CMD_GPU_PROMOTE_CTX
     * (0x2080012b) hands GSP a table of context-buffer entries
     * {gpuPhysAddr, gpuVirtAddr, size, physAttr(aperture)}.  We record them here,
     * keyed by the channel's RM client (hChanClient) so VAs don't collide across
     * processes/VASes.  nvkvm_chan_translate consults this FIRST.
     * docs/design/mode2_address_virtualization.md (capture path #2). */
#define NVKVM_MAX_MAPS 1024
    struct nvkvm_va_map {
        uint32_t client;
        uint64_t va, phys, size;
        bool     sys;           /* aperture: true=sysmem(COH/NCOH), false=FB(vidmem) */
    } va_map[NVKVM_MAX_MAPS];
    int va_map_n;

    /* M7 — CPU interrupt tree (raise MSI-X on LEAF_TRIGGER; ISR reads TOP/LEAF) */
    uint32_t intr_leaf[NVKVM_VF_INTR_NLEAF];     /* pending per leaf reg */
    uint32_t intr_leaf_en[NVKVM_VF_INTR_NLEAF];  /* enables */
    uint32_t intr_top;                           /* pending subtree bitmask (TOP(0)) */
    uint32_t intr_top_en;
    /* VAS root page-dir bases snooped from VASPACE_COPY_SERVER_RESERVED_PDES
     * (0x90f10106): levels[0].physAddress roots the WHOLE VAS (the params' VA
     * range is only the reserved window, not the VAS extent), keyed by the
     * VASpace handle (control hObject).  Matched to a channel via the channel's
     * hVASpace.  This is the channel PDB source (the GSP-managed instblk is empty
     * in our FB). */
    struct { uint32_t hvas; uint64_t pdb; } chan_vas[16];
    int      chan_vas_n;
    uint32_t chan_hvaspace;    /* the tracked channel's hVASpace handle */

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
    case NV_PTIMER_TIME_PRIV_LEVEL_MASK:                    return "PTIMER_PLM";
    case NV_PGSP_FALCON_DMATRFCMD:                          return "GSP_DMATRFCMD";
    case NV_PSEC_FALCON_DMATRFCMD:                          return "SEC_DMATRFCMD";
    case NV_PSEC_FALCON_CPUCTL:                             return "SEC_CPUCTL";
    default:             return NULL;
    }
}

/* M6: sparse FB backing + BAR0 PRAMIN window (offsets in mode2_regs_ga10x.h) */

/* FB address that PRAMIN+off currently maps to: BASE[23:0]<<16 + window offset. */
static uint64_t nvkvm_pramin_fb_addr(NvkvmGpuEmul *s, hwaddr off)
{
    uint64_t base = (uint64_t)(s->bar0_window & 0x00FFFFFFu) << 16;
    return base + (off - NVKVM_PRAMIN_BASE);
}

static uint8_t *nvkvm_fb_page(NvkvmGpuEmul *s, uint64_t fb_addr, bool alloc)
{
    gpointer key = (gpointer)(uintptr_t)(fb_addr >> 12);
    uint8_t *p = g_hash_table_lookup(s->fb_pages, key);
    if (!p && alloc) {
        p = g_malloc0(4096);
        g_hash_table_insert(s->fb_pages, key, p);
    }
    return p;
}

/* Aligned reg accesses never straddle a 4 KiB page. */
static uint64_t nvkvm_fb_read(NvkvmGpuEmul *s, uint64_t fb_addr, unsigned size)
{
    uint8_t *p = nvkvm_fb_page(s, fb_addr, false);
    uint32_t o = fb_addr & 0xfffu;
    if (!p) {
        return 0;
    }
    switch (size) {
    case 1: return p[o];
    case 2: return lduw_le_p(p + o);
    case 4: return ldl_le_p(p + o);
    case 8: return ldq_le_p(p + o);
    default: return 0;
    }
}

static void nvkvm_fb_write(NvkvmGpuEmul *s, uint64_t fb_addr, uint64_t val,
                           unsigned size)
{
    uint8_t *p = nvkvm_fb_page(s, fb_addr, true);
    uint32_t o = fb_addr & 0xfffu;
    switch (size) {
    case 1: p[o] = (uint8_t)val; break;
    case 2: stw_le_p(p + o, (uint16_t)val); break;
    case 4: stl_le_p(p + o, (uint32_t)val); break;
    case 8: stq_le_p(p + o, val); break;
    default: break;
    }
}

/* M0: identity registers answered; everything else reads 0.  M1/M2 extend this
 * switch into the fake-the-boot state machine (GFW_BOOT, HWCFG2, RISCV_STATUS,
 * FWSEC/Booter mailboxes). */
static uint64_t nvkvm_reg_read(NvkvmGpuEmul *s, hwaddr off, unsigned size)
{
    /* M6: BAR0 PRAMIN window -> sparse FB backing. */
    if (off >= NVKVM_PRAMIN_BASE && off < NVKVM_PRAMIN_BASE + NVKVM_PRAMIN_SIZE) {
        return nvkvm_fb_read(s, nvkvm_pramin_fb_addr(s, off), size);
    }
    if (off == NVKVM_BAR0_WINDOW) {
        return s->bar0_window;
    }
    /* M7 — CPU interrupt tree reads (the ISR reads TOP to find pending subtrees,
     * then LEAF for the vectors). */
    if (off == NVKVM_VF_INTR_TOP0)        { return s->intr_top; }
    if (off == NVKVM_VF_INTR_TOP_EN_SET0 || off == NVKVM_VF_INTR_TOP_EN_CLR0) {
        return s->intr_top_en;
    }
    if (off >= NVKVM_VF_INTR_LEAF0 && off < NVKVM_VF_INTR_LEAF0 + NVKVM_VF_INTR_NLEAF*4) {
        return s->intr_leaf[(off - NVKVM_VF_INTR_LEAF0)/4];
    }
    if (off >= NVKVM_VF_INTR_LEAF_EN_SET0 && off < NVKVM_VF_INTR_LEAF_EN_SET0 + NVKVM_VF_INTR_NLEAF*4) {
        return s->intr_leaf_en[(off - NVKVM_VF_INTR_LEAF_EN_SET0)/4];
    }
    if (off >= NVKVM_VF_INTR_LEAF_EN_CLR0 && off < NVKVM_VF_INTR_LEAF_EN_CLR0 + NVKVM_VF_INTR_NLEAF*4) {
        return s->intr_leaf_en[(off - NVKVM_VF_INTR_LEAF_EN_CLR0)/4];
    }
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
    /* PTIMER PLM fully lowered: tmrSetCurrentTime_GV100 needs WRITE_PROTECTION
     * _LEVEL0=ENABLE (bit4) or it NV_ASSERT(0)s (timer_gv100.c:80). */
    case NV_PTIMER_TIME_PRIV_LEVEL_MASK: return 0xFFFFFFFFu;

    /* Report display fused-off => compute-only displayless GPU.  The driver's
     * gpuFuseSupportsDisplay_HAL gives NV_ERR_NOT_SUPPORTED in display
     * StatePreInit, skipping all display engine init (inst-mem/heads/channels). */
    case NV_FUSE_STATUS_OPT_DISPLAY: return NVKVM_FUSE_OPT_DISPLAY_DISABLED;

    /* M3 — Falcon DMA always idle+not-full (FWSEC on GSP, Booter on SEC2). */
    case NV_PGSP_FALCON_DMATRFCMD:
    case NV_PSEC_FALCON_DMATRFCMD: return NV_PFALCON_DMATRFCMD_IDLE_VAL;
    /* SEC2 falcon halted (Booter "finished"); SEC2 has no RISC-V advertised. */
    case NV_PSEC_FALCON_CPUCTL:    return NV_PFALCON_FALCON_CPUCTL_HALTED_TRUE;
    case NV_PSEC_FALCON_HWCFG2:    return 0;

    /* M3 — usable FB size (MiB) for the GSP WprMeta computation. */
    case NV_USABLE_FB_SIZE_IN_MB: return NVKVM_FB_SIZE_MB;

    /* M3 — GSP RISC-V core active once the Booter "started" it (post-FWSEC). */
    case NV_PGSP_RISCV_CPUCTL:
        return s->fwsec_ran ? NV_PRISCV_RISCV_CPUCTL_ACTIVE_STAT_VAL : 0;

    /* M3 — WPR2 is stateful: DOWN (0) until FWSEC "runs" (GSP STARTCPU), then
     * UP.  The driver requires WPR2 down before FWSEC, up after. */
    case NV_PFB_PRI_MMU_WPR2_ADDR_LO: return s->fwsec_ran ? NVKVM_WPR2_LO_VAL : 0;
    case NV_PFB_PRI_MMU_WPR2_ADDR_HI: return s->fwsec_ran ? NVKVM_WPR2_HI_VAL : 0;

    /* GSP falcon MAILBOX0: on teardown (kgspUnloadRm) the driver polls this for
     * INTERRUPT_PROCESSOR_SUSPENDED_VALUE (0x80000000).  Report suspended once
     * the UNLOADING RPC arrived so close() doesn't hang 4s. */
    case NV_PGSP_FALCON_MAILBOX0: return s->gsp_suspended ? 0x80000000u : 0;

    /* NV_VIRTUAL_FUNCTION_PRIV_ACCESS_COUNTER_NOTIFY_BUFFER_SIZE (VF 0xB80000 +
     * 0x3110): UVM_REGISTER_GPU's uvmGetAccessCounterBufferSize reads this and
     * multiplies by 32 for the notify-buffer byte size; 0 => memdescCreate(0) =>
     * NV_ERR_INVALID_ARGUMENT (access_cntr_buffer.c:72) => UVM register fails =>
     * cuInit bails.  Report 256 entries (8 KiB buffer). */
    case 0x00B83110u: return 256u;

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
    /* NV_PCFG config mirror in BAR0 (DEVICE_BASE(NV_PCFG)=0x88000).  The kernel
     * BIF reads the PCIe link registers here via GPU_BUS_CFG_RD32 (NOT real PCI
     * config space), and UVM's getPCIELinkRateMBps reads LINK_CAPABILITIES for
     * BUS_INFO PCIE_GPU_LINK_CAPS — 0 => "Unknown PCIe speed" => NV_ERR_INVALID_
     * STATE => UVM_REGISTER_GPU fails => cuInit bails.  Report Gen4 x16.
     *   0x88084 NV_XVE_LINK_CAPABILITIES: MAX_SPEED[3:0]=4, MAX_WIDTH[9:4]=16
     *   0x88088 NV_XVE_LINK_CONTROL_STATUS: CUR_SPEED[19:16]=4, WIDTH[25:20]=16 */
    if (off == 0x88084u) {
        return 4u | (16u << 4);
    }
    if (off == 0x88088u) {
        return (4u << 16) | (16u << 20);
    }
    uint64_t val = nvkvm_reg_read(s, off, size);

    /* Don't trace PTIMER reads (RM timeout loops poll millions of times) or the
     * PRAMIN window (BAR2/page-table setup hammers it). */
    if (s->trace && off != NV_PTIMER_TIME_0_GA10X &&
        off != NV_PTIMER_TIME_1_GA10X &&
        !(off >= NVKVM_PRAMIN_BASE && off < NVKVM_PRAMIN_BASE + NVKVM_PRAMIN_SIZE)) {
        const char *nm = nvkvm_reg_name(off);
        qemu_log("nvkvm-gpu[%s] #%llu BAR0 RD  off=0x%06llx sz=%u -> 0x%08llx%s%s\n",
                 s->chip->name, (unsigned long long)s->access_count++,
                 (unsigned long long)off, size, (unsigned long long)val,
                 nm ? "  " : "", nm ? nm : "");
    }
    return val;
}

/* GSP msgq checksum: u64 XOR-fold over the element, returned folded to 32 bits
 * (mirrors _checkSum32 in message_queue_priv.h). The driver requires the whole
 * element to fold to 0, so the sender stores checkSum = fold(element|checkSum=0). */
static uint32_t nvkvm_msgq_checksum32(const uint8_t *p, uint32_t len)
{
    uint64_t cs = 0;
    for (uint32_t i = 0; i < len; i += 8) {   /* matches "while (p < pEnd)" */
        cs ^= ldq_le_p(p + i);
    }
    return (uint32_t)(cs >> 32) ^ (uint32_t)cs;
}

/* M3 keystone step 2: post a GSP_INIT_DONE event into the GSP->CPU status queue
 * so the driver's kgspWaitForRmInitDone -> rpcRecvPoll returns NV_OK and
 * RmInitAdapter succeeds.  statusBase = shmem+statoff; entry slot 0 at
 * statusBase+entryOff; element = GSP_MSG_QUEUE_ELEMENT (authTag[16]/aad[16]/
 * checkSum@32/seqNum@36/elemCount@40/rpc@48) with rpc.function=GSP_INIT_DONE. */
/* Post one element to the GSP->CPU status queue with seqNum == stat_writeptr.
 * If src!=NULL it is a 4096-byte template (echo a received command): we keep
 * its rpc header/body and just override function + rpc_result.  Else build a
 * minimal header (used for GSP_INIT_DONE event).  Recomputes checksum, writes
 * the element to the ring slot, bumps the status tx writePtr. */
/* Post a SINGLE-element message to the GSP->CPU status queue.  The guest reads
 * the element-count from the elemCount field (@40), so a single 4096-byte
 * element with elemCount=1 keeps the status-queue seqNum in lockstep with the
 * guest's rxSeqNum.  Responses must therefore fit one element (params <= ~3976
 * bytes); larger captured controls are echoed instead (see service_cmdq).
 * `el` is a 4096-byte buffer already populated with the response (element
 * header + rpc header + body + params). */
static void nvkvm_m3_post_status(NvkvmGpuEmul *s, const uint8_t *src,
                                 uint32_t function, uint32_t rpc_result)
{
    PCIDevice *pdev = &s->parent_obj;
    /* A status message may span MULTIPLE queue elements when the rpc payload
     * (e.g. GET_DEVICE_INFO_TABLE paramsSize=24580) exceeds one element.  The
     * guest's GspMsgQueueReceiveStatus reads the first element, derives
     * nElements = ceil((hdrSize48 + rpc.length) / queueElementSizeMin), then
     * reads that many CONTIGUOUS slots and checksums (48 + rpc.length) bytes.
     * So we build the full message zero-padded to nElements*msgsize, set
     * elemCount, fold the checksum over the real message length, and write
     * each slot, advancing the write pointer by nElements. */
    static uint8_t el[NVKVM_RESP_MAX]; /* device emu is single-threaded */
    memset(el, 0, sizeof(el));
    if (src) {
        /* copy at most one element's worth of header+body from the seed; the
         * caller's resp buffer already holds the full payload, so copy it all */
        uint32_t copylen = 48u + ldl_le_p(src + 56);
        if (copylen > sizeof(el)) {
            copylen = sizeof(el);
        }
        memcpy(el, src, copylen);
    } else {
        stl_le_p(el + 48, 0x03000000u);  /* header_version MAJOR=3 MINOR=0 */
        stl_le_p(el + 52, 0x43505256u);  /* NV_VGPU_MSG_SIGNATURE_VALID */
        stl_le_p(el + 56, 36u);          /* length = sizeof(rpc_message_header) */
    }
    stl_le_p(el + 60, function);         /* rpc.function */
    stl_le_p(el + 64, rpc_result);       /* rpc.rpc_result */
    stl_le_p(el + 68, rpc_result);       /* rpc.rpc_result_private (RmRpc reads this) */
    stl_le_p(el + 36, s->stat_seqnum);   /* per-message seqNum (NOT element ptr) */

    uint32_t msgsize = s->q_msgsize ? s->q_msgsize : 4096u;
    uint32_t len = 48u + ldl_le_p(el + 56);          /* hdr48 + rpc.length */
    if (len > sizeof(el)) {
        len = sizeof(el);
    }
    uint32_t nelems = (len + msgsize - 1u) / msgsize; /* bytesToElements */
    if (nelems == 0) {
        nelems = 1;
    }
    stl_le_p(el + 40, nelems);           /* elemCount */
    stl_le_p(el + 32, 0);                /* zero checksum field before folding */
    /* zero-pad to an 8-byte boundary for the XOR fold (guest does the same) */
    stl_le_p(el + 32, nvkvm_msgq_checksum32(el, (len + 7u) & ~7u));

    for (uint32_t i = 0; i < nelems; i++) {
        uint32_t slot = s->q_msgcount
            ? ((s->stat_writeptr + i) % s->q_msgcount) : 0;
        uint64_t gpa = s->q_shmem + s->q_stat_base + s->q_stat_entryoff +
                       (uint64_t)slot * msgsize;
        pci_dma_write(pdev, gpa, el + (uint64_t)i * msgsize, msgsize);
    }

    s->stat_writeptr = (s->stat_writeptr + nelems) % s->q_msgcount; /* modulo ring */
    s->stat_seqnum++;                    /* per-message seqNum is ABSOLUTE (no wrap) */
    uint8_t wp[4];
    stl_le_p(wp, s->stat_writeptr);
    pci_dma_write(pdev, s->q_shmem + s->q_stat_base + 16, wp, sizeof(wp));
}

/* M3 keystone: post GSP_INIT_DONE (seqNum 0). */
static void nvkvm_m3_post_init_done(NvkvmGpuEmul *s)
{
    nvkvm_m3_post_status(s, NULL, 0x1001u /* GSP_INIT_DONE */, 0 /* NV_OK */);
    qemu_log("nvkvm-gpu[%s] M3: posted GSP_INIT_DONE (seqNum 0) -> "
             "RmInitAdapter should pass kgspWaitForRmInitDone\n", s->chip->name);
}

/* ── DIAG (address-virtualization bring-up, removable) ──────────────────────
 * Decode the alloc/control RPCs so we can build the GPU-VA -> physical side
 * table from the GSP_RM_ALLOC memory descriptors and GSP_RM_CONTROL map cmds.
 * fn=103 (GSP_RM_ALLOC) body: hClient@80, hParent@84, hObject@88, hClass@92,
 * paramsSize@100, params@112.  fn=76 (GSP_RM_CONTROL) body: hClient@80,
 * hObject@84, cmd@88, status@92, paramsSize@96, params@120. */
static void nvkvm_diag_hex(const char *tag, const char *chip, uint32_t key,
                           const uint8_t *p, int n)
{
    char line[256]; int o = 0;
    o += snprintf(line + o, sizeof(line) - o, "nvkvm-gpu[%s] DIAG %s key=0x%x:",
                  chip, tag, key);
    for (int i = 0; i < n && o < (int)sizeof(line) - 4; i++) {
        o += snprintf(line + o, sizeof(line) - o, "%s%02x",
                      (i % 8 == 0) ? " " : "", p[i]);
    }
    qemu_log("%s\n", line);
}

/* Scan a params blob for any 64-bit value within [base, base+span) and log the
 * offset + value.  Used to find which RPC carries the GPFIFO GPU-VA so we learn
 * the op that establishes the mapping (no struct-layout guessing). */
static void nvkvm_diag_scan_va(NvkvmGpuEmul *s, const char *what, uint32_t fn,
                               uint32_t cmd_or_class, const uint8_t *params,
                               int psize, uint64_t base, uint64_t span)
{
    int lim = psize < 1024 ? psize : 1024;
    for (int o = 0; o + 8 <= lim; o += 4) {
        uint64_t v = ldq_le_p(params + o);
        if (v >= base && v < base + span) {
            qemu_log("nvkvm-gpu[%s] DIAG %s fn=%u cc=0x%08x VAhit@+%d val=0x%llx\n",
                     s->chip->name, what, fn, cmd_or_class, o,
                     (unsigned long long)v);
        }
    }
}

/* Broad scan: log any 64-bit value that looks like a GPU VA (0x1.2-5.xx_xxxx)
 * or a sysmem GPA near the channel-semaphore region (0x1.0-1.8_xxxx_xxxx).  This
 * reveals EVERY VA<->phys association the guest communicates, so we can find
 * where the UVM channel's GPFIFO/pushbuffer/semaphore sysmem GPA is conveyed. */
static void nvkvm_diag_broad(NvkvmGpuEmul *s, const char *what, uint32_t cc,
                             const uint8_t *params, int psize)
{
    static uint32_t budget = 600;
    int lim = psize < 1024 ? psize : 1024;
    for (int o = 0; o + 8 <= lim; o += 4) {
        uint64_t v = ldq_le_p(params + o);
        bool va  = (v >= 0x120000000ull && v < 0x500000000ull);
        bool gpa = (v >= 0x100000000ull && v < 0x180000000ull);
        bool uvmsema = (v >= 0x121000000ull && v < 0x121100000ull);
        if ((va || gpa || uvmsema) && budget-- > 0) {
            qemu_log("nvkvm-gpu[%s] DIAG SCAN %s cc=0x%08x +%d = 0x%llx%s\n",
                     s->chip->name, what, cc, o, (unsigned long long)v,
                     uvmsema ? " [UVM-VA]" : va ? " [VA]" : " [GPA]");
        }
    }
}

static void nvkvm_diag_rpc(NvkvmGpuEmul *s, const uint8_t *cmd, uint32_t fn)
{
    if (fn == 103) {                                  /* GSP_RM_ALLOC */
        uint32_t hClient = ldl_le_p(cmd + 80), hParent = ldl_le_p(cmd + 84);
        uint32_t hObject = ldl_le_p(cmd + 88), hClass = ldl_le_p(cmd + 92);
        uint32_t psize   = ldl_le_p(cmd + 100);
        const uint8_t *params = cmd + 112;
        qemu_log("nvkvm-gpu[%s] DIAG ALLOC class=0x%04x hClient=0x%08x "
                 "hParent=0x%08x hObject=0x%08x psize=%u\n", s->chip->name,
                 hClass, hClient, hParent, hObject, psize);
        /* Memory classes: dump the descriptor head (base/size/aperture live here
         * for OS_DESC/SYSTEM/LOCAL_USER/VIRTUAL). */
        if (hClass == 0x003eu || hClass == 0x0040u || hClass == 0x0071u ||
            hClass == 0x50a0u || hClass == 0x90f1u || hClass == 0x00deu ||
            hClass == 0x007eu || hClass == 0x0070u) {
            nvkvm_diag_hex("ALLOCMEM", s->chip->name, hClass, params,
                           psize < 64 ? psize : 64);
        }
        nvkvm_diag_broad(s, "ALLOC", hClass, params, (int)psize);
        /* Full channel-params dump: reveals hVASpace + all memory descriptors
         * (instance/userd/ramfc/mthdbuf) so we see where the GPFIFO/sema live. */
        if (hClass == 0xc56fu || hClass == 0xc36fu) {
            int n = psize < 384 ? (int)psize : 384;
            for (int o = 0; o < n; o += 32) {
                nvkvm_diag_hex("CHANPARAMS", s->chip->name, (uint32_t)o,
                               params + o, (n - o) < 32 ? (n - o) : 32);
            }
        }
        /* Scan any alloc params for a reference to a known channel's GPFIFO VA. */
        for (int i = 0; i < s->chan_n; i++) {
            nvkvm_diag_scan_va(s, "ALLOC", fn, hClass, params, (int)psize,
                               s->chans[i].gpfifo_va & ~0xFFFFFull, 0x100000);
        }
    } else if (fn == 76) {                            /* GSP_RM_CONTROL */
        uint32_t hObject = ldl_le_p(cmd + 84), ctrl = ldl_le_p(cmd + 88);
        uint32_t psize   = ldl_le_p(cmd + 96);
        const uint8_t *params = cmd + 120;
        nvkvm_diag_broad(s, "CTRL", ctrl, params, (int)psize);
        /* Scan control params for any known channel GPFIFO VA neighborhood. */
        for (int i = 0; i < s->chan_n; i++) {
            uint64_t b = s->chans[i].gpfifo_va & ~0xFFFFFull;
            int lim = psize < 1024 ? (int)psize : 1024;
            for (int o = 0; o + 8 <= lim; o += 4) {
                uint64_t v = ldq_le_p(params + o);
                if (v >= b && v < b + 0x100000) {
                    qemu_log("nvkvm-gpu[%s] DIAG CTRL cmd=0x%08x hObject=0x%08x "
                             "VAhit@+%d val=0x%llx psize=%u\n", s->chip->name,
                             ctrl, hObject, o, (unsigned long long)v, psize);
                    nvkvm_diag_hex("CTRLwin", s->chip->name, ctrl,
                                   params + (o > 16 ? o - 16 : 0), 64);
                    break;
                }
            }
        }
    }
}

/* Record (or update) a GPU-VA -> physical mapping in the #2 side-table.  Keyed
 * by (client, va): a re-promote of the same VA replaces the entry. */
static void nvkvm_record_va_map(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                uint64_t phys, uint64_t size, bool sys)
{
    if (!va || !size) {
        return;
    }
    for (int i = 0; i < s->va_map_n; i++) {
        struct nvkvm_va_map *m = &s->va_map[i];
        if (m->client == client && m->va == va) {
            m->phys = phys; m->size = size; m->sys = sys;
            return;
        }
    }
    if (s->va_map_n >= NVKVM_MAX_MAPS) {
        return;                 /* table full — DoS-bounded; oldest stay */
    }
    struct nvkvm_va_map *m = &s->va_map[s->va_map_n++];
    m->client = client; m->va = va; m->phys = phys; m->size = size; m->sys = sys;
    qemu_log("nvkvm-gpu[%s] M5: va_map[%d] client=0x%08x va=0x%llx -> %s "
             "phys=0x%llx size=0x%llx\n", s->chip->name, s->va_map_n - 1, client,
             (unsigned long long)va, sys ? "SYS" : "FB",
             (unsigned long long)phys, (unsigned long long)size);
}

/* Parse NV2080_CTRL_CMD_GPU_PROMOTE_CTX (0x2080012b) and fold its context-buffer
 * entries into the #2 side-table.  Params @cmd+120 (GSP_RM_CONTROL body):
 *   hChanClient@+12, entryCount@+40, promoteEntry[]@+48 (32B each:
 *   gpuPhysAddr@0, gpuVirtAddr@8, size@16, physAttr@24, bufferId@28,
 *   bInitialize@30, bNonmapped@31).  physAttr[1:0]: 0=VIDMEM, 1/2=SYSMEM. */
static void nvkvm_snoop_promote_ctx(NvkvmGpuEmul *s, const uint8_t *cmd)
{
    const uint8_t *p = cmd + 120;
    uint32_t client = ldl_le_p(p + 12);
    uint32_t ec     = ldl_le_p(p + 40);
    if (ec > 64) {              /* NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES=20;
                                 * clamp generously, never trust guest count. */
        ec = 64;
    }
    for (uint32_t i = 0; i < ec; i++) {
        const uint8_t *e = p + 48 + (uint64_t)i * 32;
        uint64_t phys = ldq_le_p(e + 0), va = ldq_le_p(e + 8), sz = ldq_le_p(e + 16);
        uint32_t physAttr = ldl_le_p(e + 24);
        uint8_t  bNonmapped = e[31];
        if (!va || !sz || bNonmapped) {
            continue;           /* unmapped/phys-only entries don't enter the VAS */
        }
        nvkvm_record_va_map(s, client, va, phys, sz, (physAttr & 0x3u) != 0);
    }
}

/* M4: service the CPU->GSP command queue.  Called when the driver rings the cmd
 * QUEUE_HEAD doorbell (0x110c00).  For each new command, echo a response
 * (same function, rpc_result=NV_OK) onto the status queue so _issueRpcAndWait
 * returns.  Init RPCs are mostly SET_* and accept an NV_OK echo. */
static void nvkvm_m3_service_cmdq(NvkvmGpuEmul *s)
{
    PCIDevice *pdev = &s->parent_obj;
    if (!s->q_ready || !s->q_msgcount) {
        return;
    }
    uint8_t wpb[4];
    if (pci_dma_read(pdev, s->q_shmem + s->q_cmd_base + 16, wpb, 4) != MEMTX_OK) {
        return;
    }
    uint32_t cmd_writeptr = ldl_le_p(wpb);
    if (s->trace && s->cmd_readptr != cmd_writeptr) {
        qemu_log("nvkvm-gpu[%s] M4: cmdq enter cmd_wp=%u cmd_rp=%u (inflight=%d) "
                 "stat_wp=%u stat_seq=%u\n", s->chip->name, cmd_writeptr,
                 s->cmd_readptr, (int)(cmd_writeptr - s->cmd_readptr),
                 s->stat_writeptr, s->stat_seqnum);
    }
    /* msgq pointers are MODULO msgCount (msgq.c wraps writePtr/readPtr at
     * msgCount), not absolute.  Bound the loop by msgCount so a desync can never
     * spin forever (pending elements < msgCount by construction). */
    uint32_t guard = 0;
    while (s->cmd_readptr != cmd_writeptr && guard++ < s->q_msgcount) {
        uint32_t slot = s->cmd_readptr % s->q_msgcount;
        uint8_t cmd[4096];
        uint64_t gpa = s->q_shmem + s->q_cmd_base + s->q_cmd_entryoff +
                       (uint64_t)slot * s->q_msgsize;
        if (pci_dma_read(pdev, gpa, cmd, sizeof(cmd)) != MEMTX_OK) {
            break;
        }
        uint32_t fn = ldl_le_p(cmd + 60);
        /* Async one-way init RPCs expect NO response — echoing them shows up in
         * the driver as "Unexpected RPC event" and desyncs the seqNum.  Consume
         * them silently.  (72=GSP_SET_SYSTEM_INFO, 73=SET_REGISTRY, sent by
         * kgspSendInitRpcs before GSP-RM is up.)  Grow this list as the trace
         * reveals more one-way functions. */
        bool async = (fn == 72 || fn == 73);
        if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M4: cmd fn=%u seq=%u -> %s\n",
                     s->chip->name, fn, ldl_le_p(cmd + 36),
                     async ? "async (no response)" : "echo NV_OK");
        }
        nvkvm_diag_rpc(s, cmd, fn);   /* DIAG: decode alloc/control for side-table */
        /* #2 side-table: capture GPU-VA -> physical from PROMOTE_CTX (the GSP-RM
         * map op).  This is what makes GSP-managed-VAS channels (UVM) resolvable
         * without leaf PTEs in our FB. */
        if (fn == 76 && ldl_le_p(cmd + 88) == 0x2080012bu) {
            nvkvm_snoop_promote_ctx(s, cmd);
        }
        /* fn=47 UNLOADING_GUEST_DRIVER: the guest is tearing down.  On real HW the
         * teardown runs Booter Unload, which brings WPR2 back DOWN.  We don't run
         * Booter Unload, so mirror its effect: clear the GSP-boot state (WPR2,
         * RISCV-active, FWSEC-ran).  Without this, a re-insmod (no QEMU restart)
         * sees WPR2 still up and _kgspBootGspRm bails with NV_ERR_INVALID_STATE
         * ("unexpected WPR2 already up") — a false cascade that masks the real
         * init failure and forces a full VM/QEMU restart between iterations. */
        if (fn == 47) {
            s->fwsec_ran = false;       /* WPR2 down (booter-unload effect)      */
            s->gsp_suspended = true;    /* MAILBOX0 -> SUSPENDED for the close poll */
            qemu_log("nvkvm-gpu[%s] M4: UNLOADING -> WPR2 down + GSP suspended\n",
                     s->chip->name);
        }
        /* M5: snoop GSP_RM_ALLOC (fn 103) for a *_CHANNEL_GPFIFO_A alloc so we can
         * locate the GPFIFO ring when the doorbell rings.  rpc_gsp_rm_alloc body
         * @cmd+80: hClass@+12 (cmd+92), paramsSize@+20 (cmd+100), params@+32
         * (cmd+112).  NV_CHANNEL_ALLOC_PARAMS: gpFifoOffset@+8 (cmd+120, u64),
         * gpFifoEntries@+16 (cmd+128). Classes: PASCAL..BLACKWELL _GPFIFO_A all
         * end in 0x6F with the family nibble (C0/C3/C4/C5/C8/C9). */
        /* M5: snoop VASPACE_COPY_SERVER_RESERVED_PDES (0x90f10106) — the CPU
         * hands GSP its page-directory level phys addrs for a VA range.  Record
         * levels[0].physAddress (root PDB) + [virtAddrLo,virtAddrHi] so the
         * doorbell can root the channel GMMU walk (the GSP-managed instblk is
         * empty in our FB).  body @cmd+80: control cmd@+88 (cmd+88); params@cmd+120;
         * virtAddrLo@cmd+136, virtAddrHi@cmd+144, levels[0].physAddress@cmd+160. */
        if (fn == 76 && ldl_le_p(cmd + 88) == 0x90f10106u) {
            if (s->chan_vas_n < 16) {
                int k = s->chan_vas_n++;
                s->chan_vas[k].hvas = ldl_le_p(cmd + 84);   /* control hObject = VASpace */
                s->chan_vas[k].pdb  = ldq_le_p(cmd + 160);  /* levels[0].physAddress */
                qemu_log("nvkvm-gpu[%s] M5: VAS hObject=0x%08x PDB=0x%llx\n",
                         s->chip->name, s->chan_vas[k].hvas,
                         (unsigned long long)s->chan_vas[k].pdb);
            }
        }
        if (fn == 103) {
            uint32_t hclass = ldl_le_p(cmd + 92);
            if ((hclass & 0xFFFFu) >= 0xC06Fu && (hclass & 0xFFu) == 0x6Fu &&
                (hclass & 0xF000u) == 0xC000u) {
                s->chan_class      = hclass;
                s->chan_gpfifo_va  = ldq_le_p(cmd + 120);
                s->chan_gpfifo_ent = ldl_le_p(cmd + 128);
                s->chan_inst_block = ldq_le_p(cmd + 256);   /* instanceMem.base */
                s->chan_inst_sys   = (ldl_le_p(cmd + 272) == 1u); /* ADDR_SYSMEM */
                s->chan_hvaspace   = ldl_le_p(cmd + 140);   /* hVASpace handle */
                s->chan_userd      = ldq_le_p(cmd + 280);   /* userdMem.base */
                s->chan_userd_sys  = (ldl_le_p(cmd + 296) == 1u);
                s->chan_gp_get     = 0;
                /* Register in the multi-channel table (dedup by gpFifoVA). */
                if (s->chan_gpfifo_va && s->chan_userd) {
                    int cslot = -1;
                    for (int i = 0; i < s->chan_n; i++) {
                        if (s->chans[i].gpfifo_va == s->chan_gpfifo_va) { cslot = i; break; }
                    }
                    if (cslot < 0 && s->chan_n < NVKVM_MAX_CHANS) { cslot = s->chan_n++; }
                    if (cslot >= 0) {
                        s->chans[cslot].gpfifo_va  = s->chan_gpfifo_va;
                        s->chans[cslot].userd      = s->chan_userd;
                        s->chans[cslot].gpfifo_ent = s->chan_gpfifo_ent;
                        s->chans[cslot].userd_sys  = s->chan_userd_sys;
                        s->chans[cslot].hvaspace   = s->chan_hvaspace;
                        s->chans[cslot].client     = ldl_le_p(cmd + 80); /* hClient */
                        s->chans[cslot].gp_get     = 0;
                        s->chans[cslot].payload    = 0;
                    }
                }
                qemu_log("nvkvm-gpu[%s] M5: channel alloc class=0x%04x gpFifoVA="
                         "0x%llx ent=%u instblk=0x%llx(%s)\n",
                         s->chip->name, hclass,
                         (unsigned long long)s->chan_gpfifo_va, s->chan_gpfifo_ent,
                         (unsigned long long)s->chan_inst_block,
                         s->chan_inst_sys ? "sys" : "fb");
            }
        }
        if (!async) {
            /* Build the response in a large buffer — GSP_RM_CONTROL responses can
             * span multiple queue elements (paramsSize up to ~34 KB).  Seed it
             * from the command element (element header + 32B rpc header + 40B
             * rpc_gsp_rm_control body), then fill the control response.
             * Body@80: hClient@80, hObject@84, cmd@88, status@92, paramsSize@96,
             * ..., params@120.  RmRpc control reads body.status (@92). */
            static uint8_t resp[NVKVM_RESP_MAX];
            memset(resp, 0, sizeof(resp));
            memcpy(resp, cmd, 4096);
            uint32_t ctrl = (fn == 76) ? ldl_le_p(resp + 88) : 0;
            if (fn == 76) {
                stl_le_p(resp + 92, 0); /* body.status = NV_OK (default) */
                const nvkvm_ctrl_resp_t *cr = NULL;
                for (uint32_t i = 0; i < NVKVM_CTRL_RESP_COUNT; i++) {
                    if (nvkvm_ctrl_resps[i].cmd == ctrl) {
                        cr = &nvkvm_ctrl_resps[i];
                        break;
                    }
                }
                if (ctrl == 0x20800a5cu) {
                    /* INTERNAL_INTR_GET_KERNEL_TABLE: the real GSP supplies the
                     * interrupt table via boot static-info so the host CPU-RM
                     * never issues this control; the guest's fake GSP forces the
                     * fallback.  Replay the captured GA106 table (tableLen@120 +
                     * 24 entries of 16B).  Full struct = 4 + 128*16 = 2052B. */
                    memset(resp + 120, 0, INTRTABLE_GA106_PSIZE);
                    memcpy(resp + 120, intrtable_ga106, sizeof(intrtable_ga106));
                    /* subtreeMap[7] of NvU64 (per intrInitSubtreeMap_TU102, which
                     * the GSP mirrors into this control's reply): UVM_OWNED must
                     * map to subtree 1 (mask 0x2) so it equals the access-counter
                     * vector's subtree, else intrCacheIntrFields_TU102 asserts.
                     * idx: 0 DEFAULT, 1 ESCHED(stall subtree3=0x8), 2 ESCHED_NOTIF
                     * (subtree0=0x1), 3 RUNLIST, 4 RUNLIST_NOTIF, 5 UVM_OWNED
                     * (subtree1=0x2), 6 UVM_SHARED (subtree2=0x4). */
                    {
                        static const uint64_t subtree_map[7] = {
                            0x0ull, 0x8ull, 0x1ull, 0x0ull, 0x0ull, 0x2ull, 0x4ull
                        };
                        for (int k = 0; k < 7; k++)
                            stq_le_p(resp + 120 + INTRTABLE_GA106_SUBTREEMAP_OFF + k * 8,
                                     subtree_map[k]);
                    }
                    stl_le_p(resp + 96, INTRTABLE_GA106_PSIZE);
                    stl_le_p(resp + 56, 32u + 40u + INTRTABLE_GA106_PSIZE);
                } else if (ctrl == 0x20801112u) {
                    /* FIFO_GET_DEVICE_INFO_TABLE: paginated; replay real GA106
                     * engine table (separate capture). params@120: baseIndex@120,
                     * numEntries@124, bMore@128, entries@132 (100B each). */
                    uint32_t base = ldl_le_p(resp + 120);
                    uint32_t psize = 12u + 32u * DEVINFO_GA106_ENTRY_SIZE; /* 3212 */
                    memset(resp + 120, 0, psize);
                    stl_le_p(resp + 120, base);
                    if (base == 0) {
                        stl_le_p(resp + 124, DEVINFO_GA106_NUM_ENTRIES);
                        memcpy(resp + 132, devinfo_ga106_entries,
                               sizeof(devinfo_ga106_entries));
                    }
                    stl_le_p(resp + 96, psize);
                    stl_le_p(resp + 56, 32u + 40u + psize);
                } else if (ctrl == 0x20802a08u) {
                    /* CE_GET_FAULT_METHOD_BUFFER_SIZE: { NvU32 size }.  Our
                     * capture truncated the 4B payload (size replayed as 0) ->
                     * kchangrpInit_gv100 asserts bufSizeInBytes>0 when CPU-RM
                     * allocates the CE fault method buffer.  Synthesize one page
                     * (the buffer lives in sysmem and only our emulated CE uses
                     * it, so any non-zero page-aligned size satisfies it). */
                    stl_le_p(resp + 120, 0x1000u); /* size = 4 KiB */
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 96, 4u);
                    stl_le_p(resp + 56, 32u + 40u + 4u);
                } else if (ctrl == 0x20800102u || ctrl == 0x20801303u) {
                    /* GPU_GET_INFO_V2 / FB_GET_INFO_V2 (Phase-B compute caps):
                     * inline list {NvU32 count; {NvU32 index, NvU32 value}[]}.
                     * The guest requests a set of indices; fill each value from
                     * the captured GA106 map (real RTX 3060 ground truth), default
                     * 0 for any index we didn't capture.  This is a cuInit=100 fix
                     * (libcuda reads these for compute-cap/device validation). */
                    const nvkvm_idxval_t *map = (ctrl == 0x20800102u)
                        ? gpu_get_info_v2_map : fb_get_info_v2_map;
                    uint32_t mapn = (ctrl == 0x20800102u)
                        ? GPU_GET_INFO_V2_MAP_N : FB_GET_INFO_V2_MAP_N;
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t cnt = ldl_le_p(resp + 120);
                    if (cnt > 128) cnt = 128;
                    for (uint32_t e = 0; e < cnt; e++) {
                        uint32_t eoff = 124u + e * 8u;
                        if (eoff + 8u > 120u + ps) break;
                        uint32_t idx = ldl_le_p(resp + eoff);
                        uint32_t val = 0;
                        for (uint32_t k = 0; k < mapn; k++) {
                            if (map[k].index == idx) { val = map[k].value; break; }
                        }
                        stl_le_p(resp + eoff + 4, val);
                    }
                    stl_le_p(resp + 92, 0);              /* NV_OK */
                    stl_le_p(resp + 56, 32u + 40u + ps); /* psize unchanged */
                } else if (ctrl == 0x20802a07u) {
                    /* CE_GET_PHYSICAL_CAPS V2 {u32 ceEngineType; u8 capsTbl[2]}.
                     * UVM channel-manager ces_validate requires each usable CE
                     * to advertise SYSMEM + P2P; a zero capsTbl -> "Failed to
                     * initialize the channel manager: NV_ERR_NOT_SUPPORTED"
                     * (uvm_gpu.c init_gpu) -> UVM_REGISTER_GPU fails -> cuInit
                     * bails.  Set SYSMEM_READ(0x04)|SYSMEM_WRITE(0x08)|
                     * SYSMEM(0x20)|P2P(0x40) in capsTbl[0]. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    if (ps >= 6) {
                        resp[124] = 0x6Cu;
                        resp[125] = 0x00u;
                    }
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20801823u) {
                    /* BUS_GET_INFO_V2: inline {count; {index,data}[]}.  Fill the
                     * PCIe link entries so the driver's getPCIELinkRateMBps()
                     * succeeds; otherwise it returns NV_ERR_INVALID_STATE
                     * ("Unknown PCIe speed"), which propagates out of
                     * UVM_REGISTER_GPU (rmStatus=0x40) and makes cuInit bail.
                     * idx 0x2D PCIE_GEN_INFO: LINK_CAP_GEN[15:12]=3 (GEN4),
                     * CURR_LEVEL[19:16]=3; idx 0x07 LINK_CTRL_STATUS:
                     * LINK_SPEED[19:16]=4 (16GT/s), LINK_WIDTH[25:20]=16 (x16). */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t cnt = ldl_le_p(resp + 120);
                    if (cnt > 256) cnt = 256;
                    for (uint32_t e = 0; e < cnt; e++) {
                        uint32_t eoff = 124u + e * 8u;
                        if (eoff + 8u > 120u + ps) break;
                        uint32_t idx = ldl_le_p(resp + eoff);
                        if (idx == 0x03u) {
                            /* PCIE_GPU_LINK_CAPS: MAX_SPEED[3:0]=4 (16000MBPS),
                             * MAX_WIDTH[9:4]=16.  UVM's getPCIELinkRateMBps reads
                             * exactly this; 0 -> "Unknown PCIe speed" INVALID_STATE. */
                            stl_le_p(resp + eoff + 4, 4u | (16u << 4));
                        } else if (idx == 0x2Du) {
                            stl_le_p(resp + eoff + 4, (3u << 12) | (3u << 16));
                        } else if (idx == 0x07u) {
                            stl_le_p(resp + eoff + 4, (4u << 16) | (16u << 20));
                        }
                    }
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20803801u) {
                    /* GRMGR_GET_GR_FS_INFO: replay captured GA106 floorsweep blob
                     * (GPC/TPC/PES enable masks).  Capture is a 256B prefix; the
                     * leading query results are what cuInit reads. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t n = GRMGR_GR_FS_INFO_BLOB_N;
                    if (n > ps) n = ps;
                    memcpy(resp + 120, grmgr_gr_fs_info_blob, n);
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20800a01u && cr) {
                    /* INTERNAL_DISPLAY_GET_STATIC_INFO: replay captured 32B but
                     * SYNTHESIZE numDispChannels (struct off 32, params+120 =>
                     * resp+152).  Our capture truncated the 36B struct's tail, so
                     * the field replayed as 0 -> kdispStateInitLocked's
                     * portMemAllocNonPaged(sizeof*0)=NULL -> "Could not allocate
                     * clientChannelTable".  Compute-only never allocates a display
                     * channel, so any value that bounds dispChannelNum works; 128
                     * comfortably covers GA10x's core/window/cursor channel space
                     * with a negligible (~3 KiB) table.  (generate-to-satisfy) */
                    memset(resp + 120, 0, cr->psize);
                    memcpy(resp + 120, cr->data, cr->dlen);
                    stl_le_p(resp + 152, 128u); /* numDispChannels */
                    stl_le_p(resp + 92, cr->status);
                    stl_le_p(resp + 96, cr->psize);
                    stl_le_p(resp + 56, 32u + 40u + cr->psize);
                } else if (cr && (120u + cr->psize) <= NVKVM_RESP_MAX) {
                    /* general replay: captured GA106 init-control response
                     * (ROUTE_TO_PHYSICAL GET controls the echo can't fabricate).
                     * Multi-element capable now — nvkvm_m3_post_status splits the
                     * message across queue elements, so large controls (e.g.
                     * GET_DEVICE_INFO_TABLE 0x20800a40 psize=24580) replay too.
                     * Any captured tail beyond cr->dlen is zero (fine where the
                     * meaningful prefix is numEntries + entries). */
                    memset(resp + 120, 0, cr->psize);
                    memcpy(resp + 120, cr->data, cr->dlen);
                    stl_le_p(resp + 92, cr->status);
                    stl_le_p(resp + 96, cr->psize);
                    stl_le_p(resp + 56, 32u + 40u + cr->psize);
                } else if (ctrl == 0x208001b0u) {
                    /* GET_CONSTRUCTED_FALCON_INFO: empty list is valid */
                    stl_le_p(resp + 96, 1284u);
                    memset(resp + 120, 0, 1284);
                    stl_le_p(resp + 56, 32u + 40u + 1284u);
                }
                /* else: void/SET control — echo with status=NV_OK */
                /* DIAG(B-compute): log controls we did NOT fill with real data
                 * (cr==NULL and not special-cased) that REQUEST a non-zero
                 * response (a GET) — these return NV_OK+zeros and are the
                 * cuInit=100 compute-cap suspects. */
                if (!cr && ctrl != 0x20800a5cu && ctrl != 0x20801112u &&
                    ctrl != 0x20802a08u && ctrl != 0x208001b0u &&
                    ctrl != 0x20800102u && ctrl != 0x20801303u &&
                    ctrl != 0x20803801u) {
                    uint32_t reqpsize = ldl_le_p(resp + 96);
                    if (reqpsize > 0) {
                        qemu_log("nvkvm-gpu[%s] CTRL-UNFILLED cmd=0x%08x psize=%u "
                                 "-> echoed NV_OK+zeros\n",
                                 s->chip->name, ctrl, reqpsize);
                    }
                }
            }
            if (fn == 65) {
                /* GET_GSP_STATIC_INFO (fn 65): NOT a control — the GSP returns the
                 * full GspStaticConfigInfo struct directly at the rpc body
                 * (rpc_message->get_gsp_static_info_v14_00.data = element+80,
                 * right after the 32B rpc header).  Replay the captured GA106
                 * struct (1792B incl. fbRegionInfoParams numFBRegions=5).
                 * rpc.length = 32(hdr) + sizeof(struct); single element. */
                memcpy(resp + 80, gspstaticinfo_ga106, sizeof(gspstaticinfo_ga106));
                stl_le_p(resp + 56, 32u + GSPSTATICINFO_GA106_SIZE);
                /* GspStaticConfigInfo.bar2PdeBase @ offset 1672 (verified by a
                 * host printk) = the GSP-chosen BAR2 page-dir base.  The guest
                 * reads this and roots its BAR2 page tables here; use it as our
                 * GMMU walk root + enable VIRTUAL translation. */
                if (GSPSTATICINFO_GA106_SIZE >= NVKVM_GSPSTATIC_BAR2PDEBASE_OFF + 8) {
                    s->bar2_pdb = ldq_le_p(gspstaticinfo_ga106 + NVKVM_GSPSTATIC_BAR2PDEBASE_OFF);
                    s->bar2_virtual = (s->bar2_pdb != 0);
                    /* bar1PdeBase precedes bar2PdeBase (consecutive NvU64). */
                    s->bar1_pdb = ldq_le_p(gspstaticinfo_ga106 +
                                           NVKVM_GSPSTATIC_BAR2PDEBASE_OFF - 8);
                    qemu_log("nvkvm-gpu[%s] M6: BAR2 root PDB (GSP static) = 0x%llx "
                             "BAR1 root PDB = 0x%llx\n", s->chip->name,
                             (unsigned long long)s->bar2_pdb,
                             (unsigned long long)s->bar1_pdb);
                }
            }
            if (fn == 70) {
                /* UPDATE_BAR_PDE (fn 70): on bare-metal GSP-client the GSP binds
                 * BAR2; the kernel sends GSP the BAR2 root PDE to write into the
                 * bound root page directory (kern_bus.c:880).  Body @ element+80:
                 * barType@80 (1=BAR2), entryValue@88 (the root PDE -> kernel's
                 * next-level table), entryLevelShift@96.  We emulate the bind by
                 * writing entryValue into our FB backing at the GSP root PDB
                 * (bar2_pdb) index 0 — the GMMU walk then follows it into the
                 * kernel's page tables (already in FB via PRAMIN). */
                uint32_t bartype = ldl_le_p(cmd + 80);
                uint64_t entryval = ldq_le_p(cmd + 88);
                uint64_t lvlshift = ldq_le_p(cmd + 96);
                if (bartype == 1 /* NV_RPC_UPDATE_PDE_BAR_2 */ && s->bar2_pdb) {
                    nvkvm_fb_write(s, s->bar2_pdb, entryval, 8);
                    qemu_log("nvkvm-gpu[%s] M6: UPDATE_BAR_PDE BAR2 root[0] @ "
                             "0x%llx <- 0x%llx (shift=%llu)\n", s->chip->name,
                             (unsigned long long)s->bar2_pdb,
                             (unsigned long long)entryval,
                             (unsigned long long)lvlshift);
                } else if (bartype == 0 /* NV_RPC_UPDATE_PDE_BAR_1 */ && s->bar1_pdb) {
                    nvkvm_fb_write(s, s->bar1_pdb, entryval, 8);
                    qemu_log("nvkvm-gpu[%s] M6: UPDATE_BAR_PDE BAR1 root[0] @ "
                             "0x%llx <- 0x%llx (shift=%llu)\n", s->chip->name,
                             (unsigned long long)s->bar1_pdb,
                             (unsigned long long)entryval,
                             (unsigned long long)lvlshift);
                }
            }
            /* DIAG(init-stall): log every serviced RPC so we can see the last
             * one before the 4s _threadNodeCheckTimeout.  fn=76 controls also
             * print their ctrl cmd. */
            qemu_log("nvkvm-gpu[%s] M4: RPC fn=%u cmd=0x%x reqPsize=%u -> "
                     "respPsize=%u status=0x%x rpclen=%u\n", s->chip->name, fn,
                     (fn == 76 ? ctrl : 0), ldl_le_p(cmd + 96), ldl_le_p(resp + 96),
                     ldl_le_p(resp + 92), ldl_le_p(resp + 56));
            nvkvm_m3_post_status(s, resp, fn, 0 /* rpc_result NV_OK */);
        }
        /* Advance by the command's ELEMENT COUNT, not by 1.  A GSP_MSG_QUEUE
         * message spans ceil((HDR_SIZE(48) + rpc.length) / SIZE_MIN(4096))
         * queue elements; large controls (e.g. 0x20800a41 paramsSize=8204 =>
         * 3 elements) occupy continuation elements that carry raw payload, NOT
         * rpc headers.  Reading them as separate commands posted bogus fn=0
         * len=0 responses that later failed the driver's
         * GspMsgQueueReceiveStatus ("Incorrect message length 0", msgLen <
         * sizeof(GSP_MSG_QUEUE_ELEMENT)=80) -> NV_ERR_INVALID_PARAM_STRUCT 0x3a.
         * The continuation elements are consumed silently (one response per
         * logical command). */
        {
            uint32_t msglen = 48u + ldl_le_p(cmd + 56);
            uint32_t elems = (msglen + 4095u) / 4096u;
            if (elems == 0) {
                elems = 1;
            }
            s->cmd_readptr = (s->cmd_readptr + elems) % s->q_msgcount; /* wrap */
        }
    }
    /* ack consumption: advance the cmd-queue read pointer.  The GSP queues are
     * created with MSGQ_FLAGS_SWAP_RX (message_queue_cpu.c:180), so the readPtr
     * is SWAPPED into the OTHER queue's backing store: as the cmd-queue consumer
     * our pReadOutgoing = &pOurRxHdr->readPtr, and pOurRxHdr lives in the queue
     * WE created (the status queue).  So write the cmd readPtr to the STATUS
     * queue's rx header (stat_base + rxHdrOff 0x20) — the guest-producer reads
     * it there via its pReadIncoming.  Writing it to cmd_base+0x20 (no-swap
     * location) left the guest seeing 0 frees -> "buffer is full" once init
     * accumulated ~msgCount(63) command elements. */
    uint8_t rp[4];
    stl_le_p(rp, s->cmd_readptr);
    pci_dma_write(pdev, s->q_shmem + s->q_stat_base + 0x20, rp, sizeof(rp));
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

        /* RMARGS region (id8 "RMARGS") holds GSP_ARGUMENTS_CACHED, which begins
         * with MESSAGE_QUEUE_INIT_ARGUMENTS { u64 sharedMemPhysAddr; u32
         * pageTableEntryCount; NvLength cmdQueueOffset; NvLength statQueueOffset }.
         * The CPU<->GSP message-queue shared region is at sharedMemPhysAddr; the
         * GSP->CPU status queue (whose msgqTxHeader we must init for msgqRxLink)
         * is at sharedMemPhysAddr + statQueueOffset. */
        if (id8 == 0x0000524d41524753ULL /* "RMARGS" */) {
            uint8_t a[32];
            if (pci_dma_read(pdev, pa, a, sizeof(a)) == MEMTX_OK) {
                uint64_t shmem = ldq_le_p(a + 0);
                uint32_t ptec  = ldl_le_p(a + 8);
                uint64_t cmdoff = ldq_le_p(a + 16);
                uint64_t statoff = ldq_le_p(a + 24);
                qemu_log("nvkvm-gpu[%s] M3:   RMARGS msgq: sharedMemPA=0x%llx "
                         "pteCount=%u cmdQOff=0x%llx statQOff=0x%llx "
                         "=> statusQueue@0x%llx\n", s->chip->name,
                         (unsigned long long)shmem, ptec,
                         (unsigned long long)cmdoff, (unsigned long long)statoff,
                         (unsigned long long)(shmem + statoff));

                /* M3 keystone step 1: init the GSP->CPU status-queue tx header
                 * so the driver's msgqRxLink links (it polls forever otherwise).
                 * The status queue is structurally identical to the cmd queue
                 * (same size/msgSize/align), so copy the driver's known-good
                 * cmd-queue tx header verbatim, with writePtr=0 (no messages
                 * yet).  GSP is the TX side of the status queue. */
                if (shmem && statoff) {
                    uint8_t txh[32];
                    if (pci_dma_read(pdev, shmem + cmdoff, txh, sizeof(txh))
                            == MEMTX_OK) {
                        stl_le_p(txh + 16, 0); /* writePtr = 0 */
                        if (pci_dma_write(pdev, shmem + statoff, txh,
                                          sizeof(txh)) == MEMTX_OK) {
                            qemu_log("nvkvm-gpu[%s] M3:   wrote status-queue tx "
                                     "header @0x%llx (ver=%u size=0x%x msgSize=%u "
                                     "msgCount=%u rxHdrOff=0x%x entryOff=0x%x) "
                                     "-> msgqRxLink should link\n", s->chip->name,
                                     (unsigned long long)(shmem + statoff),
                                     ldl_le_p(txh+0), ldl_le_p(txh+4),
                                     ldl_le_p(txh+8), ldl_le_p(txh+12),
                                     ldl_le_p(txh+24), ldl_le_p(txh+28));
                            /* cache the queue layout for the M4 RPC shim */
                            s->q_shmem        = shmem;
                            s->q_cmd_base     = (uint32_t)cmdoff;
                            s->q_stat_base    = (uint32_t)statoff;
                            s->q_msgsize      = ldl_le_p(txh + 8);
                            s->q_msgcount     = ldl_le_p(txh + 12);
                            s->q_cmd_entryoff = ldl_le_p(txh + 28);
                            s->q_stat_entryoff= ldl_le_p(txh + 28);
                            s->stat_writeptr  = 0;
                            s->stat_seqnum    = 0;
                            s->cmd_readptr    = 0;
                            s->q_ready        = true;
                            /* step 2: post GSP_INIT_DONE (seqNum 0) */
                            nvkvm_m3_post_init_done(s);
                        }
                    }
                }
            }
        }
    }
}

static uint64_t nvkvm_chan_translate(NvkvmGpuEmul *s, uint64_t va, bool *out_sys);
static void nvkvm_chan_execute(NvkvmGpuEmul *s);
static uint64_t nvkvm_walk_pdb(NvkvmGpuEmul *s, uint64_t pdb, uint64_t va,
                               bool *out_sys);

static void nvkvm_bar0_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    NvkvmGpuEmul *s = opaque;

    /* M6: BAR0 PRAMIN window write -> sparse FB backing; window-base register. */
    if (off >= NVKVM_PRAMIN_BASE && off < NVKVM_PRAMIN_BASE + NVKVM_PRAMIN_SIZE) {
        uint64_t fa = nvkvm_pramin_fb_addr(s, off);
        nvkvm_fb_write(s, fa, val, size);
        /* Snoop the BAR2 instance-block PAGE_DIR_BASE.  On the GSP-client path the
         * CPU never writes NV_PBUS_BAR2_BLOCK (0x1714) — the GSP (which we fake)
         * binds BAR2 from the instance block the CPU builds in FB.  The instblk's
         * NV_RAMIN_PAGE_DIR_BASE is word128 (byte 0x200): TARGET[1:0]=VID_MEM(0),
         * PDB_LO[31:12].  Instance blocks are 4 KiB-aligned, so a 4-byte write at
         * (fb&0xFFF)==0x200 with a non-zero VID_MEM page-dir base is an instblk
         * bind; the most-recent one before kbusVerifyBar2 is BAR2's. */
        if (size == 4 && (fa & 0xFFFu) == 0x200u &&
            (val & 0x3u) == 0u && (val & 0xFFFFF000u) != 0u) {
            s->bar2_inst_block = fa - 0x200u;
            if (s->trace) {
                qemu_log("nvkvm-gpu[%s] M6: snooped BAR2 instblk @ FB 0x%llx "
                         "(PDB_LO word=0x%08x)\n", s->chip->name,
                         (unsigned long long)s->bar2_inst_block, (uint32_t)val);
            }
        }
        return;
    }
    if (off == NVKVM_BAR0_WINDOW) {
        s->bar0_window = (uint32_t)val;
        return;
    }
    /* M5 — work-submit doorbell.  Detect the channel submission (the guest wrote
     * the work-submit token).  TODO(M5): execute the channel — walk its GPFIFO ->
     * pushbuffer -> CE semaphore release and write the payload so the driver's
     * channelWaitForFinishPayload poll completes (currently times out at
     * ce_utils.c:349).  For now, log it so the doorbell offset/token are
     * confirmed against the GA100 HAL. */
    if (off == NVKVM_VF_DOORBELL) {
        /* Work submitted on SOME channel.  The doorbell token's chid would name
         * it, but during init multiple GPFIFO channels coexist (CeUtils scrubber
         * + its self-verify channel + the host/compute channel) and tracking only
         * the last-allocated one dropped the scrubber's work.  Instead walk EVERY
         * registered channel's pending GPFIFO: a channel with no new work has
         * GP_PUT==gp_get so nvkvm_chan_execute() bails harmlessly.  For each that
         * advanced, honor an explicit CE/NVC56F semaphore release from its
         * pushbuffer; else fall back to the implicit finish-payload semaphore at
         * gpFifoVA + GPFIFO_SIZE(0x8000) + HOST_SEMA(4) = +0x8004 with a
         * per-channel incrementing payload (channelWaitForFinishPayload polls
         * exactly that). */
        for (int i = 0; i < s->chan_n; i++) {
            struct nvkvm_chan_entry *c = &s->chans[i];
            /* Load this channel into the chan_* working set chan_execute reads. */
            s->chan_gpfifo_va  = c->gpfifo_va;
            s->chan_userd      = c->userd;
            s->chan_gpfifo_ent = c->gpfifo_ent;
            s->chan_userd_sys  = c->userd_sys;
            s->chan_hvaspace   = c->hvaspace;
            s->chan_client     = c->client;
            s->chan_gp_get     = c->gp_get;
            uint32_t before = c->gp_get;
            nvkvm_chan_execute(s);
            c->gp_get = s->chan_gp_get;          /* save consumed index */
            if (c->gp_get == before) {
                continue;                        /* no new work on this channel */
            }
            if (s->chan_sem_released) {
                continue;                        /* explicit release already done */
            }
            /* Fallback: implicit finish-payload semaphore. */
            uint64_t sema_va = c->gpfifo_va + 0x8004ull;
            bool is_sys = false;
            uint64_t phys = nvkvm_chan_translate(s, sema_va, &is_sys);
            if (phys != NVKVM_GMMU_FAULT) {
                uint32_t payload = ++c->payload;
                if (is_sys) { uint8_t b[4]; stl_le_p(b, payload);
                              pci_dma_write(&s->parent_obj, phys, b, 4); }
                else        { nvkvm_fb_write(s, phys, payload, 4); }
                qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x ch[%d] -> completed: "
                         "semaVA=0x%llx -> %s phys=0x%llx payload=%u\n",
                         s->chip->name, (uint32_t)val, i,
                         (unsigned long long)sema_va, is_sys ? "SYS" : "FB",
                         (unsigned long long)phys, payload);
            } else {
                qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x ch[%d] -> sema VA "
                         "0x%llx FAULTED; gpfifo=0x%llx\n", s->chip->name,
                         (uint32_t)val, i, (unsigned long long)sema_va,
                         (unsigned long long)c->gpfifo_va);
            }
        }
        return;
    }
    /* M7 — CPU interrupt tree writes. */
    if (off == NVKVM_VF_INTR_LEAF_TRIGGER) {
        /* The driver triggers an interrupt by writing its vector here: set the
         * leaf+top pending bits and raise the MSI so the ISR fires (this is what
         * _osVerifyInterrupts polls for). */
        uint32_t vec = (uint32_t)val & 0xFFFu;
        uint32_t leaf = vec / 32u, bit = vec % 32u, subtree = leaf / 2u;
        if (leaf < NVKVM_VF_INTR_NLEAF) {
            s->intr_leaf[leaf] |= (1u << bit);
            s->intr_top |= (1u << subtree);
            PCIDevice *pd = &s->parent_obj;
            if (msix_enabled(pd)) {
                msix_notify(pd, 0);   /* single stall vector; ISR demuxes via TOP/LEAF */
            } else {
                pci_set_irq(pd, 1);
            }
            qemu_log("nvkvm-gpu[%s] M7: INTR trigger vec=%u -> leaf[%u] bit%u "
                     "subtree%u, MSI raised\n", s->chip->name, vec, leaf, bit, subtree);
        }
        return;
    }
    if (off >= NVKVM_VF_INTR_LEAF0 && off < NVKVM_VF_INTR_LEAF0 + NVKVM_VF_INTR_NLEAF*4) {
        uint32_t i = (off - NVKVM_VF_INTR_LEAF0) / 4;     /* LEAF(i): write-1-to-clear */
        s->intr_leaf[i] &= ~(uint32_t)val;
        uint32_t st = i / 2u;
        if (s->intr_leaf[st*2] == 0 && (st*2+1 >= NVKVM_VF_INTR_NLEAF ||
            s->intr_leaf[st*2+1] == 0)) {
            s->intr_top &= ~(1u << st);
        }
        if (s->intr_top == 0 && !msix_enabled(&s->parent_obj)) {
            pci_set_irq(&s->parent_obj, 0);
        }
        return;
    }
    if (off >= NVKVM_VF_INTR_LEAF_EN_SET0 && off < NVKVM_VF_INTR_LEAF_EN_SET0 + NVKVM_VF_INTR_NLEAF*4) {
        s->intr_leaf_en[(off - NVKVM_VF_INTR_LEAF_EN_SET0)/4] |= (uint32_t)val; return;
    }
    if (off >= NVKVM_VF_INTR_LEAF_EN_CLR0 && off < NVKVM_VF_INTR_LEAF_EN_CLR0 + NVKVM_VF_INTR_NLEAF*4) {
        s->intr_leaf_en[(off - NVKVM_VF_INTR_LEAF_EN_CLR0)/4] &= ~(uint32_t)val; return;
    }
    if (off == NVKVM_VF_INTR_TOP_EN_SET0) { s->intr_top_en |= (uint32_t)val; return; }
    if (off == NVKVM_VF_INTR_TOP_EN_CLR0) { s->intr_top_en &= ~(uint32_t)val; return; }
    /* M6: NV_PBUS_BAR2_BLOCK (0x1714) PTR[27:0] = BAR2 instance-block FB addr
     * (in NV_RAMIN_BASE_SHIFT=12 units).  Caches the page-dir base source for
     * the BAR2 GMMU walk. */
    /* BAR2 bind register: NV_PBUS_BAR2_BLOCK (0x1714) on Maxwell, OR the
     * Turing/Ampere VF variant NV_VIRTUAL_FUNCTION_PRIV_BAR2_BLOCK at BAR0
     * 0xB80F48 (NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET 0xB80000 + 0xF48).  PTR
     * [27:0]<<12 = instblk FB addr; MODE bit31 = 1 VIRTUAL / 0 PHYSICAL. */
    if (off == NVKVM_PBUS_BAR2_BLOCK || off == NVKVM_VF_BAR2_BLOCK) {
        s->bar2_inst_block = (uint64_t)(val & 0x0FFFFFFFu) << NVKVM_BAR2_BLOCK_PTR_SHIFT;
        s->bar2_virtual    = (val & NVKVM_BAR2_BLOCK_MODE_VIRTUAL) != 0;
        qemu_log("nvkvm-gpu[%s] M6: BAR2_BLOCK@0x%llx -> instblk FB 0x%llx mode=%s\n",
                 s->chip->name, (unsigned long long)off,
                 (unsigned long long)s->bar2_inst_block,
                 s->bar2_virtual ? "VIRTUAL" : "PHYSICAL");
        return;
    }

    /* M3: GSP falcon STARTCPU => FWSEC "executes" => WPR2 becomes initialized.
     * (CPUCTL bit1 STARTCPU, or via CPUCTL_ALIAS 0x110130.) */
    if ((off == NV_PGSP_FALCON_CPUCTL || off == 0x00110130u) && (val & 0x2u)) {
        s->gsp_suspended = false;       /* fresh GSP boot — no longer suspended  */
        if (!s->fwsec_ran) {
            s->fwsec_ran = true;
            if (s->trace) {
                qemu_log("nvkvm-gpu[%s] M3: GSP STARTCPU -> FWSEC ran, WPR2 up\n",
                         s->chip->name);
            }
        }
    }

    /* M4: cmd-queue doorbell — the driver wrote NV_PGSP_QUEUE_HEAD(0) to notify
     * the GSP of new command(s).  Service the cmd queue (echo NV_OK responses). */
    if (off == NVKVM_GSP_QUEUE_HEAD0 && s->q_ready) {
        nvkvm_m3_service_cmdq(s);
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

/* BAR1 — RM "BAR1" FB aperture.  The driver maps channel USERD, pushbuffers and
 * semaphores (in FB) into BAR1 and the CPU reads/writes them through it — most
 * importantly GP_PUT @ USERD+0x8C, the channel work-submit.  A no-op stub here
 * silently dropped those writes (GP_PUT never reached the FB backing → the CE
 * scrubber's channel never advanced → ce_utils.c:349 timeout).  BAR1 is a GPU
 * virtual aperture (its own page tables in FB, root = bar1_pdb from
 * GspStaticConfigInfo.bar1PdeBase + the UPDATE_BAR_PDE(BAR_1) root entry), so a
 * BAR1 offset is a GPU VA: GMMU-VER2-walk it to FB/sysmem (nvkvm_walk_pdb). */
static uint64_t nvkvm_baraperture_read(void *opaque, hwaddr off, unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    if (!s->bar1_pdb) {
        return 0;
    }
    bool sys = false;
    uint64_t pa = nvkvm_walk_pdb(s, s->bar1_pdb, off, &sys);
    if (pa == NVKVM_GMMU_FAULT) {
        return 0;
    }
    uint64_t rv;
    if (sys) {
        uint8_t b[8] = {0};
        if (pci_dma_read(&s->parent_obj, pa, b, size) != MEMTX_OK) return 0;
        rv = ldn_le_p(b, size);
    } else {
        rv = nvkvm_fb_read(s, pa, size);
    }
    /* DIAG: BAR1 reads landing in the low-FB region (where the UVM channel's
     * GPFIFO/USERD/semaphore live) — a poll spin shows up as repeated reads of
     * one address; that address is the completion semaphore the guest waits on. */
    if (!sys && pa >= NVKVM_DIAG_LOFB_LO && pa < NVKVM_DIAG_LOFB_HI) {
        static uint64_t last_pa; static uint32_t rep; static uint32_t total;
        if (pa != last_pa) { last_pa = pa; rep = 0; }
        if ((rep++ % 4096) == 0 && total++ < 4000) {
            qemu_log("nvkvm-gpu[GA106] DIAG BAR1 RD off=0x%llx -> FB 0x%llx "
                     "= 0x%llx (rep~%u)\n", (unsigned long long)off,
                     (unsigned long long)pa, (unsigned long long)rv, rep);
        }
    }
    return rv;
}

static void nvkvm_baraperture_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    if (!s->bar1_pdb) {
        return;
    }
    bool sys = false;
    uint64_t pa = nvkvm_walk_pdb(s, s->bar1_pdb, off, &sys);
    if (pa == NVKVM_GMMU_FAULT) {
        return;
    }
    if (sys) {
        uint8_t b[8];
        stn_le_p(b, size, val);
        pci_dma_write(&s->parent_obj, pa, b, size);
    } else {
        nvkvm_fb_write(s, pa, val, size);
    }
    /* DIAG: BAR1 writes into the low-FB region reveal where the guest CPU lays
     * down the UVM channel's GPFIFO entry, pushbuffer, and inits the semaphore. */
    if (!sys && pa >= NVKVM_DIAG_LOFB_LO && pa < NVKVM_DIAG_LOFB_HI) {
        static uint32_t total;
        if (total++ < 2000) {
            qemu_log("nvkvm-gpu[GA106] DIAG BAR1 WR off=0x%llx -> FB 0x%llx "
                     "<- 0x%llx sz=%u\n", (unsigned long long)off,
                     (unsigned long long)pa, (unsigned long long)val, size);
        }
    }
}

static const MemoryRegionOps nvkvm_aperture_ops = {
    .read       = nvkvm_baraperture_read,
    .write      = nvkvm_baraperture_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = { .min_access_size = 1, .max_access_size = 8 },
};

/* ── M6: RM BAR2 aperture — GA10x GMMU VER2 page-walk over the FB backing ─────
 * BAR2 (PCI BAR3, 32 MiB) is a GPU virtual aperture: the guest programs page
 * tables (in FB) and an instance block (NV_PBUS_BAR2_BLOCK 0x1714) holding the
 * page-directory base, then accesses VRAM through BAR2.  We walk those tables to
 * translate a BAR2 offset (== GPU VA) to an FB phys addr, then hit fb_pages.
 * VER2 levels (kern_gmmu_fmt_gp10x.c): PD3 VA[48:47], PD2 [46:38], PD1 [37:29],
 * PD0 [28:21] (16B dual PDE), PT_small [20:12] (4 KiB) / PT_big [20:bigShift].
 * Entry addr = field<<shift; PTE/PDE ADDRESS_VID = bits 32:8 (<<12). */

static uint64_t nvkvm_fb_rd64(NvkvmGpuEmul *s, uint64_t fb_addr)
{
    return nvkvm_fb_read(s, fb_addr, 8);
}

/* Translate a BAR2 GPU VA to an FB physical address (VID_MEM path only — the
 * page tables and BAR2-mapped surfaces during init live in FB).  Returns
 * NVKVM_GMMU_FAULT on an unmapped/SYSMEM path. */
static uint64_t nvkvm_bar2_translate(NvkvmGpuEmul *s, uint64_t va)
{
    uint64_t tbl;
    if (s->bar2_pdb != 0) {
        /* Page-dir base reported by GSP in GspStaticConfigInfo.bar2PdeBase
         * (offset 1672), which we replay for GET_GSP_STATIC_INFO.  The guest
         * reads the same value and builds its BAR2 page tables (on demand, via
         * the PRAMIN window into our FB backing) rooted here, so this is the
         * walk root. */
        tbl = s->bar2_pdb;
    } else if (s->bar2_inst_block != 0) {
        /* Fallback: read PDB from an instance block (word128 @ +0x200 LO[31:12],
         * word129 @ +0x204 HI[31:0]). */
        uint64_t w128 = nvkvm_fb_read(s, s->bar2_inst_block + NVKVM_RAMIN_PDB_LO_OFF, 4);
        uint64_t w129 = nvkvm_fb_read(s, s->bar2_inst_block + NVKVM_RAMIN_PDB_HI_OFF, 4);
        tbl = (w128 & 0xFFFFF000ull) | (w129 << 32);
    } else {
        return NVKVM_GMMU_FAULT;
    }

    /* PD3 -> PD2 -> PD1 : single 8B PDEs. */
    static const struct { int hi, lo; } lvl[3] = {
        {48, 47}, {46, 38}, {37, 29}
    };
    for (int i = 0; i < 3; i++) {
        uint32_t idx = (uint32_t)((va >> lvl[i].lo) &
                                  ((1ull << (lvl[i].hi - lvl[i].lo + 1)) - 1));
        uint64_t pde = nvkvm_fb_rd64(s, tbl + (uint64_t)idx * 8);
        tbl = NVKVM_VER2_ADDR_VID(pde);
        if (tbl == 0) {
            return NVKVM_GMMU_FAULT;
        }
    }

    /* PD0: 16B dual PDE (big-page + small-page sub-tables). */
    uint32_t idx0 = (uint32_t)((va >> 21) & 0xFF);            /* [28:21], 8 bits */
    uint64_t lo = nvkvm_fb_rd64(s, tbl + (uint64_t)idx0 * 16);
    uint64_t hi = nvkvm_fb_rd64(s, tbl + (uint64_t)idx0 * 16 + 8);
    uint64_t small_tbl = (((hi >> 8) & ((1ull << 25) - 1)) << 12); /* SMALL bits 96:72, <<12 */
    uint64_t big_tbl   = (((lo >> 4) & ((1ull << 28) - 1)) << 8);  /* BIG  bits 32:4,  <<8  */

    uint64_t pte, page;
    if (small_tbl != 0) {                       /* 4 KiB pages: PT VA[20:12] */
        uint32_t idx = (uint32_t)((va >> 12) & 0x1FF);
        pte  = nvkvm_fb_rd64(s, small_tbl + (uint64_t)idx * 8);
        if (!(pte & 1)) {
            return NVKVM_GMMU_FAULT;            /* PTE VALID bit 0 */
        }
        page = NVKVM_VER2_ADDR_VID(pte);
        return page + (va & 0xFFFull);
    }
    if (big_tbl != 0) {                          /* 64 KiB pages: PT VA[20:16] */
        uint32_t idx = (uint32_t)((va >> 16) & 0x1F);
        pte  = nvkvm_fb_rd64(s, big_tbl + (uint64_t)idx * 8);
        if (!(pte & 1)) {
            return NVKVM_GMMU_FAULT;
        }
        page = NVKVM_VER2_ADDR_VID(pte);
        return page + (va & 0xFFFFull);
    }
    return NVKVM_GMMU_FAULT;
}

/* Read 8 bytes from a page-table entry in FB (vidmem) or sysmem (GPA). */
static uint64_t nvkvm_pt_rd64(NvkvmGpuEmul *s, uint64_t addr, bool sys)
{
    if (sys) {
        uint8_t b[8];
        if (pci_dma_read(&s->parent_obj, addr, b, 8) != MEMTX_OK) return 0;
        return ldq_le_p(b);
    }
    return nvkvm_fb_rd64(s, addr);
}

/* M5 — translate a CHANNEL GPU VA to a physical address, rooted at the channel's
 * own PDB (read from its instance block), and report whether the leaf page is in
 * sysmem (GPA) or vidmem (FB) via *out_sys.  Unlike BAR2, channel buffers (the
 * scrubber's pushbuffer/semaphore) live in SYSMEM, so the leaf PTE APERTURE
 * (NV_MMU_VER2_PTE_APERTURE bits 2:1: 0=VID, 2/3=SYS) selects ADDRESS_VID
 * (bits32:8) vs ADDRESS_SYS (bits53:8), both <<12.  The page-table hierarchy
 * itself is assumed to live in FB (the GSP-client RM allocates the page directory
 * from FB, as for BAR2) — read via the PRAMIN/FB backing.  Returns
 * NVKVM_GMMU_FAULT on any miss (caller then does nothing — safe). */
/* Walk VER2 from an explicit page-directory base `pdb`.  Returns the physical
 * address (and *out_sys = leaf in sysmem) or NVKVM_GMMU_FAULT. */
static uint64_t nvkvm_walk_pdb(NvkvmGpuEmul *s, uint64_t pdb, uint64_t va,
                               bool *out_sys)
{
    *out_sys = false;
    uint64_t tbl = pdb;
    if (tbl == 0) {
        return NVKVM_GMMU_FAULT;
    }
    /* PD3->PD2->PD1 (8B PDEs), then PD0 (16B dual PDE or 2 MiB PTE); aperture per level. */
    bool tsys = false;     /* PDB in FB; each PDE aperture says where next lives */
    static const struct { int hi, lo; } lvl[3] = { {48,47}, {46,38}, {37,29} };
    for (int i = 0; i < 3; i++) {
        uint32_t idx = (uint32_t)((va >> lvl[i].lo) &
                                  ((1ull << (lvl[i].hi - lvl[i].lo + 1)) - 1));
        uint64_t pde = nvkvm_pt_rd64(s, tbl + (uint64_t)idx * 8, tsys);
        uint32_t ap = (uint32_t)((pde >> 1) & 0x3);  /* PDE APERTURE: 1=VID,2/3=SYS */
        if (ap == 1) { tbl = ((pde >> 8) & ((1ull << 25) - 1)) << 12; tsys = false; }
        else if (ap == 2 || ap == 3) { tbl = ((pde >> 8) & ((1ull << 46) - 1)) << 12; tsys = true; }
        else { return NVKVM_GMMU_FAULT; }            /* INVALID */
        if (tbl == 0) {
            return NVKVM_GMMU_FAULT;
        }
    }
    /* PD0: 16B dual PDE.  BIG aperture lo bits2:1, SMALL aperture hi bits2:1.
     * SMALL addr VID hi32:8 / SYS hi53:8 (<<12); BIG addr VID lo32:4 / SYS lo53:4 (<<8). */
    uint32_t idx0 = (uint32_t)((va >> 21) & 0xFF);
    uint64_t lo = nvkvm_pt_rd64(s, tbl + (uint64_t)idx0 * 16, tsys);
    uint64_t hi = nvkvm_pt_rd64(s, tbl + (uint64_t)idx0 * 16 + 8, tsys);
    uint32_t big_ap = (uint32_t)((lo >> 1) & 0x3), small_ap = (uint32_t)((hi >> 1) & 0x3);
    /* A PD0 entry with VALID(bit0)=1 is itself a 2 MiB LEAF PTE (NV_MMU_VER2_PTE),
     * not a dual PDE pointing to 4K/64K sub-tables.  APERTURE bits2:1 (0=VID,
     * 2/3=SYS); VA[20:0] is the 2 MiB page offset. */
    if (lo & 1) {
        uint32_t lap = (uint32_t)((lo >> 1) & 0x3);
        uint64_t pg;
        if (lap == 0) { pg = ((lo >> 8) & ((1ull << 25) - 1)) << 12; *out_sys = false; }
        else if (lap == 2 || lap == 3) { pg = ((lo >> 8) & ((1ull << 46) - 1)) << 12; *out_sys = true; }
        else { return NVKVM_GMMU_FAULT; }
        return pg + (va & 0x1FFFFFull);
    }
    uint64_t pte; uint32_t pgshift; bool stsys;
    if (small_ap == 1 || small_ap == 2 || small_ap == 3) {
        stsys = (small_ap != 1);
        uint64_t st = stsys ? (((hi >> 8) & ((1ull << 46) - 1)) << 12)
                            : (((hi >> 8) & ((1ull << 25) - 1)) << 12);
        if (st == 0) { return NVKVM_GMMU_FAULT; }
        pte = nvkvm_pt_rd64(s, st + (uint64_t)((va >> 12) & 0x1FF) * 8, stsys);
        pgshift = 12;
    } else if (big_ap == 1 || big_ap == 2 || big_ap == 3) {
        stsys = (big_ap != 1);
        uint64_t bt = stsys ? (((lo >> 4) & ((1ull << 50) - 1)) << 8)
                            : (((lo >> 4) & ((1ull << 29) - 1)) << 8);
        if (bt == 0) { return NVKVM_GMMU_FAULT; }
        pte = nvkvm_pt_rd64(s, bt + (uint64_t)((va >> 16) & 0x1F) * 8, stsys);
        pgshift = 16;
    } else {
        return NVKVM_GMMU_FAULT;
    }
    if (!(pte & 1)) {                       /* PTE VALID bit0 */
        return NVKVM_GMMU_FAULT;
    }
    uint32_t aperture = (uint32_t)((pte >> 1) & 0x3);  /* PTE APERTURE: 0=VID,2/3=SYS */
    uint64_t page;
    if (aperture == 0) {
        page = ((pte >> 8) & ((1ull << 25) - 1)) << 12;
        *out_sys = false;
    } else if (aperture == 2 || aperture == 3) {
        page = ((pte >> 8) & ((1ull << 46) - 1)) << 12;
        *out_sys = true;
    } else {
        return NVKVM_GMMU_FAULT;
    }
    return page + (va & ((1ull << pgshift) - 1));
}

/* Translate a channel GPU VA by trying every snooped VAS PDB (from
 * VASPACE_COPY_SERVER_RESERVED_PDES) and returning the first that resolves.  The
 * channel's pushbuffer/sema live in its own VAS, but the scrubber's vid/sys test
 * surfaces may be in a different VAS, so match by which PD actually maps the VA
 * (a valid leaf PTE) rather than by the channel's hVASpace handle. */
static uint64_t nvkvm_chan_translate(NvkvmGpuEmul *s, uint64_t va, bool *out_sys)
{
    /* #2 side-table (PROMOTE_CTX) — authoritative and required for GSP-managed
     * VASes whose leaf PTEs never land in our FB (so the PDB walk below FAULTs).
     * Scoped to the executing channel's RM client so VAs can't collide across
     * processes.  Longest-prefix not needed: PROMOTE_CTX ranges are disjoint. */
    for (int i = 0; i < s->va_map_n; i++) {
        struct nvkvm_va_map *m = &s->va_map[i];
        if (m->client == s->chan_client && va >= m->va && va < m->va + m->size) {
            *out_sys = m->sys;
            return m->phys + (va - m->va);
        }
    }
    /* Authoritative: the executing channel's own PDB (from its instance block).
     * Correct even for hVASpace=0 (device-default) channels that don't match any
     * snooped VAS handle.  0 = instblk empty -> fall through to the heuristic. */
    if (s->chan_pdb) {
        uint64_t p = nvkvm_walk_pdb(s, s->chan_pdb, va, out_sys);
        if (p != NVKVM_GMMU_FAULT) { return p; }
    }
    /* Prefer the channel's own hVASpace first (fast path / disambiguation). */
    for (int i = 0; i < s->chan_vas_n; i++) {
        if (s->chan_vas[i].hvas == s->chan_hvaspace) {
            uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, va, out_sys);
            if (p != NVKVM_GMMU_FAULT) { return p; }
            break;
        }
    }
    for (int i = 0; i < s->chan_vas_n; i++) {
        uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, va, out_sys);
        if (p != NVKVM_GMMU_FAULT) { return p; }
    }
    *out_sys = false;
    return NVKVM_GMMU_FAULT;
}

/* M5 — read/write a 32-bit word at a PHYSICAL address in either aperture. */
static uint32_t nvkvm_phys_rd32(NvkvmGpuEmul *s, uint64_t phys, bool sys)
{
    if (sys) {
        uint8_t b[4];
        if (pci_dma_read(&s->parent_obj, phys, b, 4) != MEMTX_OK) return 0;
        return ldl_le_p(b);
    }
    return (uint32_t)nvkvm_fb_read(s, phys, 4);
}
static void nvkvm_phys_wr32(NvkvmGpuEmul *s, uint64_t phys, bool sys, uint32_t v)
{
    if (sys) {
        uint8_t b[4]; stl_le_p(b, v);
        pci_dma_write(&s->parent_obj, phys, b, 4);
    } else {
        nvkvm_fb_write(s, phys, v, 4);
    }
}
/* Read one 32-bit word at a CHANNEL GPU VA (translate then phys read). */
static bool nvkvm_chan_rd32(NvkvmGpuEmul *s, uint64_t va, uint32_t *out)
{
    bool sys; uint64_t p = nvkvm_chan_translate(s, va, &sys);
    if (p == NVKVM_GMMU_FAULT) return false;
    *out = nvkvm_phys_rd32(s, p, sys);
    return true;
}

/* M5 — EXECUTE the copy-engine work submitted on the doorbell-rung channel.
 * Walk the GPFIFO [chan_gp_get, GP_PUT) (GP_PUT read from USERD @ +0x8C), and for
 * each pushbuffer parse the FERMI method stream (header: SEC_OP[31:29],
 * METHOD_ADDR[11:0]<<2, COUNT[28:16]) for the NVB0B5/NVC7B5 copy class.  On
 * LAUNCH_DMA (0x300) perform the op for real: REMAP_ENABLE(bit10) => fill
 * OFFSET_OUT with SET_REMAP_CONST_A (memset); else copy OFFSET_IN->OFFSET_OUT for
 * LINE_LENGTH_IN bytes (x LINE_COUNT).  All addresses are channel VAs translated
 * per-word.  This makes the scrubber's CE self-verify (mem_mgr.c:469) see real
 * data.  Bounded + fault-safe (bail on any miss). */
static void nvkvm_chan_execute(NvkvmGpuEmul *s)
{
    if (!s->chan_gpfifo_va || !s->chan_userd || !s->chan_gpfifo_ent) {
        return;
    }
    uint32_t gp_put = s->chan_userd_sys
        ? nvkvm_phys_rd32(s, s->chan_userd + 0x8C, true)
        : (uint32_t)nvkvm_fb_read(s, s->chan_userd + 0x8C, 4);
    qemu_log("nvkvm-gpu[%s] M5: chan_exec gpfifo=0x%llx userd=0x%llx(%s) "
             "gp_get=%u gp_put=%u ent=%u\n", s->chip->name,
             (unsigned long long)s->chan_gpfifo_va,
             (unsigned long long)s->chan_userd, s->chan_userd_sys ? "sys" : "fb",
             s->chan_gp_get, gp_put, s->chan_gpfifo_ent);
    /* Pick the channel's VAS by CONTENT, not by handle.  The instance block is
     * empty (GSP-managed) so it gives no PDB, and hVASpace=0 (device-default)
     * channels match no snooped VAS handle -> the try-all fallback picks a wrong
     * VAS that maps gpFifoVA to a stale/zero page.  Instead, among the snooped
     * VAS PDBs, choose the one under which the pending GPFIFO entry reads
     * NON-ZERO (a valid pushbuffer pointer) — that is the VAS that actually owns
     * this channel's ring.  Pin it in chan_pdb so every translate in this walk
     * (entry/pushbuffer/sema) uses the same correct VAS. */
    s->chan_pdb = 0;
    if (gp_put < s->chan_gpfifo_ent && gp_put != s->chan_gp_get) {
        uint64_t eva = s->chan_gpfifo_va + (uint64_t)s->chan_gp_get * 8;
        for (int i = 0; i < s->chan_vas_n; i++) {
            bool sy = false;
            uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, eva, &sy);
            if (p == NVKVM_GMMU_FAULT) { continue; }
            if (nvkvm_phys_rd32(s, p, sy) != 0) {   /* valid GP_ENTRY0 (pb low) */
                s->chan_pdb = s->chan_vas[i].pdb;
                break;
            }
        }
        qemu_log("nvkvm-gpu[%s] M5: chan_exec hvas=0x%08x picked_pdb=0x%llx "
                 "gpfifoVA=0x%llx\n", s->chip->name, s->chan_hvaspace,
                 (unsigned long long)s->chan_pdb,
                 (unsigned long long)s->chan_gpfifo_va);
        /* DIAG: when content-pick fails, show what EACH snooped VAS resolves the
         * GPFIFO entry VA to (fault / phys+aperture) and the value read there. */
        if (s->chan_pdb == 0) {
            for (int i = 0; i < s->chan_vas_n; i++) {
                bool sy = false;
                uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, eva, &sy);
                if (p == NVKVM_GMMU_FAULT) {
                    qemu_log("nvkvm-gpu[%s] DIAG vas[%d] hvas=0x%08x pdb=0x%llx "
                             "eva=0x%llx -> FAULT\n", s->chip->name, i,
                             s->chan_vas[i].hvas, (unsigned long long)s->chan_vas[i].pdb,
                             (unsigned long long)eva);
                } else {
                    qemu_log("nvkvm-gpu[%s] DIAG vas[%d] hvas=0x%08x pdb=0x%llx "
                             "eva=0x%llx -> %s phys=0x%llx val=0x%08x\n", s->chip->name,
                             i, s->chan_vas[i].hvas, (unsigned long long)s->chan_vas[i].pdb,
                             (unsigned long long)eva, sy ? "SYS" : "FB",
                             (unsigned long long)p, nvkvm_phys_rd32(s, p, sy));
                }
            }
        }
    }
    if (gp_put >= s->chan_gpfifo_ent) {
        return;                                  /* implausible -> bail */
    }
    /* NVC56F host-channel semaphore-release tracking (methods 0x5c..0x6c).  The
     * golden-image / watchdog / scrubber channels append a SEM_EXECUTE RELEASE
     * after their engine work to signal completion; channelWaitForFinishPayload
     * polls that semaphore.  We honor the EXPLICIT release here (translate the
     * SEM addr, write the payload) WITHOUT running the GR/compute methods — per
     * the Phase-B design we never emulate GR, we only signal completion.  Tracked
     * at function scope so addr/payload set in one method group apply to a later
     * SEM_EXECUTE.  ADDR is 64-bit: LO bits[31:2] | HI<<32. */
    uint64_t sem_addr = 0;
    uint32_t sem_pay_lo = 0, sem_pay_hi = 0;
    /* CE-class (NVC8B5/NVB0B5) completion semaphore — the one
     * channelWaitForFinishPayload() polls (pbGpuVA + finishPayloadOffset); the
     * CeUtils memory scrubber waits on it (ce_utils.c:349).  Released by
     * LAUNCH_DMA when SEMAPHORE_TYPE != NONE.  Distinct from the NVC56F host
     * semaphore (sem_addr) which the same scrub pushbuffer ALSO releases at
     * semaOffset — honoring only the host one left finishPayload unwritten,
     * so the scrubber timed out. */
    uint64_t ce_sem_addr = 0;
    uint32_t ce_sem_pay = 0;
    s->chan_sem_released = false;
    uint32_t guard = 0;
    for (uint32_t idx = s->chan_gp_get; idx != gp_put &&
         guard < s->chan_gpfifo_ent; idx = (idx + 1) % s->chan_gpfifo_ent, guard++) {
        uint32_t e0, e1;
        uint64_t eva = s->chan_gpfifo_va + (uint64_t)idx * 8;
        if (!nvkvm_chan_rd32(s, eva, &e0) || !nvkvm_chan_rd32(s, eva + 4, &e1)) {
            qemu_log("nvkvm-gpu[%s] M5: chan_exec GPFIFO entry[%u] @VA 0x%llx "
                     "FAULTED (no VAS maps it)\n", s->chip->name, idx,
                     (unsigned long long)eva);
            break;
        }
        uint64_t pb   = (uint64_t)(e0 & 0xFFFFFFFCu) | ((uint64_t)(e1 & 0xFFu) << 32);
        uint32_t pblen = (e1 >> 10) & 0x1FFFFFu;   /* GP_ENTRY1_LENGTH: # method words */
        { uint32_t w0 = 0; bool pbok = nvkvm_chan_rd32(s, pb, &w0);
          qemu_log("nvkvm-gpu[%s] M5: chan_exec entry[%u] pb=0x%llx pblen=%u "
                   "pb_read=%s w0=0x%08x\n", s->chip->name, idx,
                   (unsigned long long)pb, pblen, pbok ? "ok" : "FAULT", w0); }
        /* method-stream parse */
        uint64_t off_in = 0, off_out = 0;
        uint32_t llen = 0, lcount = 1, remapA = 0;
        uint32_t src_pm = 0, dst_pm = 0;   /* SET_SRC/DST_PHYS_MODE target */
        for (uint32_t w = 0; w < pblen; ) {
            uint32_t hdr;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) { break; }
            w++;
            uint32_t secop = (hdr >> 29) & 0x7;
            uint32_t maddr = (hdr & 0xFFFu) << 2;
            uint32_t cnt   = (hdr >> 16) & 0x1FFFu;
            if (secop != 1 && secop != 3 && secop != 5) { continue; } /* INC/NON_INC/ONE_INC */
            for (uint32_t j = 0; j < cnt && w < pblen; j++, w++) {
                uint32_t d;
                if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) { w = pblen; break; }
                uint32_t m = (secop == 3) ? maddr : maddr + j * 4; /* NON_INC holds */
                switch (m) {
                case 0x400: off_in  = (off_in  & 0xFFFFFFFFull) | ((uint64_t)d << 32); break; /* IN_UPPER  */
                case 0x404: off_in  = (off_in  & ~0xFFFFFFFFull) | d; break;                  /* IN_LOWER  */
                case 0x408: off_out = (off_out & 0xFFFFFFFFull) | ((uint64_t)d << 32); break; /* OUT_UPPER */
                case 0x40C: off_out = (off_out & ~0xFFFFFFFFull) | d; break;                  /* OUT_LOWER */
                case 0x418: llen   = d; break;                                               /* LINE_LENGTH_IN */
                case 0x41C: lcount = d ? d : 1; break;                                        /* LINE_COUNT */
                case 0x260: src_pm = d & 3; break;                                           /* SET_SRC_PHYS_MODE */
                case 0x264: dst_pm = d & 3; break;                                           /* SET_DST_PHYS_MODE */
                case 0x700: remapA = d; break;                                               /* SET_REMAP_CONST_A */
                /* CE-class completion semaphore (NVC8B5_SET_SEMAPHORE_A/B/PAYLOAD).
                 * A=upper[24:0], B=lower[31:0], PAYLOAD=release value. */
                case 0x240: ce_sem_addr = (ce_sem_addr & 0xFFFFFFFFull) | ((uint64_t)(d & 0x01FFFFFFu) << 32); break;
                case 0x244: ce_sem_addr = (ce_sem_addr & ~0xFFFFFFFFull) | d; break;
                case 0x248: ce_sem_pay = d; break;
                case 0x300: {                                                                /* LAUNCH_DMA */
                    bool remap    = (d >> 10) & 1;
                    bool mscrub   = (d >> 23) & 1;   /* MEMORY_SCRUB_ENABLE [23] */
                    bool src_phys = (d >> 12) & 1;   /* SRC_TYPE PHYSICAL */
                    bool dst_phys = (d >> 13) & 1;   /* DST_TYPE PHYSICAL */
                    uint32_t sem_type = (d >> 3) & 0x3; /* SEMAPHORE_TYPE [4:3], !=0 => release */
                    uint64_t bytes = (uint64_t)llen * lcount;
                    if (bytes > (16u << 20)) bytes = 16u << 20;  /* safety cap */
                    /* Resolve a CE address: PHYSICAL -> the offset IS the phys addr
                     * (aperture from PHYS_MODE: 0=FB else sysmem); VIRTUAL ->
                     * translate via the channel VAS (leaf PTE picks FB/sys). */
                    #define NVKVM_CE_RESOLVE(off, phys, pm, sysv) \
                        ((phys) ? ((sysv) = ((pm) != 0), (off)) \
                                : nvkvm_chan_translate(s, (off), &(sysv)))
                    if (mscrub) {
                        /* MEMORY_SCRUB: zero the dst region.  Our FB backing is
                         * sparse-zero (unwritten reads return 0), so the data
                         * write is a no-op; the completion semaphore below is
                         * what unblocks the CeUtils scrubber.  No src is set. */
                    } else if (remap) {
                        for (uint64_t b = 0; b + 4 <= bytes; b += 4) {
                            bool sy; uint64_t p = NVKVM_CE_RESOLVE(off_out + b, dst_phys, dst_pm, sy);
                            if (p == NVKVM_GMMU_FAULT) break;
                            nvkvm_phys_wr32(s, p, sy, remapA);
                        }
                    } else {
                        for (uint64_t b = 0; b + 4 <= bytes; b += 4) {
                            bool ssy, dsy;
                            uint64_t sp = NVKVM_CE_RESOLVE(off_in + b,  src_phys, src_pm, ssy);
                            uint64_t dp = NVKVM_CE_RESOLVE(off_out + b, dst_phys, dst_pm, dsy);
                            if (sp == NVKVM_GMMU_FAULT || dp == NVKVM_GMMU_FAULT) break;
                            uint32_t v = nvkvm_phys_rd32(s, sp, ssy);
                            nvkvm_phys_wr32(s, dp, dsy, v);
                            if (b == 0) {
                                qemu_log("nvkvm-gpu[%s] M5:   COPY[0] src 0x%llx(%s)"
                                  "=0x%08x -> dst 0x%llx(%s)\n", s->chip->name,
                                  (unsigned long long)sp, ssy?"sys":"fb", v,
                                  (unsigned long long)dp, dsy?"sys":"fb");
                            }
                        }
                    }
                    #undef NVKVM_CE_RESOLVE
                    qemu_log("nvkvm-gpu[%s] M5: CE %s in=0x%llx(%s) out=0x%llx(%s) "
                             "bytes=%llu const=0x%x\n", s->chip->name,
                             mscrub ? "SCRUB" : remap ? "MEMSET" : "COPY",
                             (unsigned long long)off_in,
                             src_phys ? "phys" : "virt", (unsigned long long)off_out,
                             dst_phys ? "phys" : "virt", (unsigned long long)bytes, remapA);
                    /* CE-class completion semaphore release: LAUNCH_DMA with
                     * SEMAPHORE_TYPE != NONE writes ce_sem_pay to
                     * (pbGpuVA+finishPayloadOffset).  This is what the CeUtils
                     * scrubber's channelWaitForFinishPayload polls — the fast-
                     * scrub pushbuffer ALSO emits an NVC56F SEM_EXECUTE (host
                     * sema at semaOffset), so honoring only that left this one
                     * unwritten and the scrubber timed out (ce_utils.c:349). */
                    if (sem_type != 0 && ce_sem_addr) {
                        bool sy = false;
                        uint64_t p = nvkvm_chan_translate(s, ce_sem_addr, &sy);
                        if (p != NVKVM_GMMU_FAULT) {
                            if (sy) { uint8_t bb[4]; stl_le_p(bb, ce_sem_pay);
                                      pci_dma_write(&s->parent_obj, p, bb, 4); }
                            else    { nvkvm_fb_write(s, p, ce_sem_pay, 4); }
                            s->chan_sem_released = true;
                            qemu_log("nvkvm-gpu[%s] M5: CE_SEM_RELEASE addr=0x%llx "
                                     "-> %s phys=0x%llx payload=%u\n", s->chip->name,
                                     (unsigned long long)ce_sem_addr, sy ? "SYS" : "FB",
                                     (unsigned long long)p, ce_sem_pay);
                        }
                    }
                    break;
                }
                /* NVC56F host-channel semaphore methods. */
                case 0x5c: sem_addr = (sem_addr & ~0xFFFFFFFFull) | (d & 0xFFFFFFFCu); break; /* SEM_ADDR_LO[31:2] */
                case 0x60: sem_addr = (sem_addr & 0xFFFFFFFFull) | ((uint64_t)d << 32); break;/* SEM_ADDR_HI */
                case 0x64: sem_pay_lo = d; break;                                            /* SEM_PAYLOAD_LO */
                case 0x68: sem_pay_hi = d; break;                                            /* SEM_PAYLOAD_HI */
                case 0x6c: {                                                                 /* SEM_EXECUTE */
                    if ((d & 0x7u) == 0x1u && sem_addr) {   /* OPERATION == RELEASE */
                        bool sy = false;
                        uint64_t p = nvkvm_chan_translate(s, sem_addr, &sy);
                        if (p != NVKVM_GMMU_FAULT) {
                            bool sz64 = (d >> 24) & 1;       /* PAYLOAD_SIZE: 0=16B(64-bit val), 1=4B */
                            if (sz64) {
                                if (sy) { uint8_t b[4]; stl_le_p(b, sem_pay_lo);
                                          pci_dma_write(&s->parent_obj, p, b, 4); }
                                else    { nvkvm_fb_write(s, p, sem_pay_lo, 4); }
                            } else {
                                if (sy) { uint8_t b[8]; stl_le_p(b, sem_pay_lo);
                                          stl_le_p(b + 4, sem_pay_hi);
                                          pci_dma_write(&s->parent_obj, p, b, 8); }
                                else    { nvkvm_fb_write(s, p, sem_pay_lo, 4);
                                          nvkvm_fb_write(s, p + 4, sem_pay_hi, 4); }
                            }
                            s->chan_sem_released = true;
                            qemu_log("nvkvm-gpu[%s] M5: SEM_RELEASE addr=0x%llx -> %s "
                                     "phys=0x%llx payload=%u\n", s->chip->name,
                                     (unsigned long long)sem_addr, sy ? "SYS" : "FB",
                                     (unsigned long long)p, sem_pay_lo);
                        }
                    }
                    break;
                }
                default: break;
                }
            }
        }
    }
    s->chan_gp_get = gp_put;
}

static uint64_t nvkvm_bar2_read(void *opaque, hwaddr off, unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    /* PHYSICAL mode (or not yet bound) = identity FB access; VIRTUAL = GMMU walk.
     * During bootstrap the driver accesses the BAR2 page tables via BAR2-physical
     * before binding it virtual, so identity must work then. */
    uint64_t pa = s->bar2_virtual ? nvkvm_bar2_translate(s, off) : off;
    if (pa == NVKVM_GMMU_FAULT) {
        return 0;
    }
    return nvkvm_fb_read(s, pa, size);
}

static void nvkvm_bar2_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    NvkvmGpuEmul *s = opaque;
    uint64_t pa = s->bar2_virtual ? nvkvm_bar2_translate(s, off) : off;
    if (pa == NVKVM_GMMU_FAULT) {
        return;
    }
    nvkvm_fb_write(s, pa, val, size);
}

static const MemoryRegionOps nvkvm_bar2_ops = {
    .read       = nvkvm_bar2_read,
    .write      = nvkvm_bar2_write,
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
    s->q_ready = false;
    s->stat_writeptr = 0;
    s->stat_seqnum = 0;
    s->cmd_readptr = 0;

    /* M6: sparse FB backing for the BAR0 PRAMIN window (value = g_malloc0'd 4 KiB). */
    s->bar0_window = 0;
    s->bar2_inst_block = 0;
    s->bar2_virtual = false;
    s->bar2_pdb = 0;
    s->fb_pages = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                        NULL, g_free);

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
    /* BAR3 == RM "BAR2": the 32 MiB GPU-virtual instance/PTE aperture.  GMMU-VER2
     * walked to the FB backing (M6).  (PCI BAR0=regs, BAR1=RM BAR1 FB window.) */
    memory_region_init_io(&s->bar3, OBJECT(s), &nvkvm_bar2_ops, s,
                          "nvkvm-gpu-bar2", chip->bar3_size);
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
    /* This device is a "hybrid" — it declares BOTH INTERFACE_CONVENTIONAL_PCI_
     * DEVICE and INTERFACE_PCIE_DEVICE, so QEMU does NOT auto-set
     * QEMU_PCI_CAP_EXPRESS (see do_pci_register_device: only pure-PCIe devices
     * get it).  Behind a pcie-root-port the bus IS express, so set it manually
     * — otherwise pci_is_express() is false, the Express cap is never added,
     * and the driver's link-rate read at config 0x88 returns 0. */
    if (pci_bus_is_express(pci_get_bus(pci_dev))) {
        pci_dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
    }
    if (pci_is_express(pci_dev)) {
        /* Place the PCIe Express capability at config 0x78 — EXACTLY where a real
         * GA10x / RTX 3060 puts it ("Capabilities: [78] Express").  The NVIDIA
         * driver reads its link via GPU_BUS_CFG_RD32 at the ABSOLUTE config
         * offset NV_XVE_LINK_CONTROL_STATUS=0x88, which is only the cap's
         * LINK_CONTROL_STATUS when the cap base is 0x78 (0x78+0x10).  With the
         * cap elsewhere (QEMU default 0x60) the driver read garbage ->
         * calculatePCIELinkRateMBps "Unknown PCIe speed" -> NV_ERR_INVALID_STATE.
         * MSI-X auto-placed near 0x40, so 0x78..0xB4 is free. */
        uint8_t exp = 0x78;
        pcie_endpoint_cap_init(pci_dev, exp);
        /* LINK_CAP @ 0x84 (cap+0xC): MAX_LINK_SPEED[3:0]=4 (16GT/s), width[9:4]=16
         * — matches the real card's LnkCap 0x00453d04. */
        uint32_t lnkcap = pci_get_long(cfg + exp + PCI_EXP_LNKCAP);
        lnkcap = (lnkcap & ~0x3FFu) | 4u | (16u << 4);
        pci_set_long(cfg + exp + PCI_EXP_LNKCAP, lnkcap);
        /* LINK_CONTROL_STATUS @ 0x88: LNKSTA @ 0x8A -> dword 0x88[31:16].
         * CURRENT_LINK_SPEED[19:16]=4, NEG_LINK_WIDTH[25:20]=16. */
        pci_set_word(cfg + exp + PCI_EXP_LNKSTA, (uint16_t)(4u | (16u << 4)));
    }
}

static void nvkvm_gpu_emul_exit(PCIDevice *pci_dev)
{
    NvkvmGpuEmul *s = NVKVM_GPU_EMUL(pci_dev);
    msix_unuse_all_vectors(pci_dev);
    msix_uninit(pci_dev, &s->msix, &s->msix);
    g_free(s->vbios);
    if (s->fb_pages) {
        g_hash_table_destroy(s->fb_pages);
    }
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
