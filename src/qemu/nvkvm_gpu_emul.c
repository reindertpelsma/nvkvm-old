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

    /* M5 channel tracking: captured from the most-recent *_CHANNEL_GPFIFO_A
     * GSP_RM_ALLOC (fn 103).  During init there is a single CE channel (the
     * scrubber), so the doorbell-rung channel is the last one allocated.  These
     * locate the GPFIFO ring so the doorbell handler can walk submitted work. */
    uint64_t chan_gpfifo_va;   /* gpFifoOffset: GPU VA of the channel's GPFIFO ring */
    uint32_t chan_gpfifo_ent;  /* gpFifoEntries */
    uint32_t chan_class;       /* hClass of the tracked channel                     */
    uint64_t chan_inst_block;  /* instanceMem.base: channel instance block (unused: GSP-managed, empty) */
    bool     chan_inst_sys;    /* instanceMem.addressSpace == ADDR_SYSMEM(1)        */
    uint32_t chan_payload;     /* completion payload counter (incr per doorbell)    */
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
                    qemu_log("nvkvm-gpu[%s] M6: BAR2 root PDB (GSP static) = 0x%llx\n",
                             s->chip->name, (unsigned long long)s->bar2_pdb);
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
                }
            }
            if (s->trace && (fn == 76 || fn == 65)) {
                qemu_log("nvkvm-gpu[%s] M4:   fn=%u ctrl cmd=0x%x reqPsize=%u -> "
                         "respPsize=%u status=0x%x rpclen=%u\n", s->chip->name, fn,
                         ctrl, ldl_le_p(cmd + 96), ldl_le_p(resp + 96),
                         ldl_le_p(resp + 92), ldl_le_p(resp + 56));
            }
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
        /* Work submitted on the channel.  We don't run the CE; instead we
         * complete the submission by advancing the channel's finish-payload
         * semaphore, which is what channelWaitForFinishPayload polls.  Channel
         * buffer layout [pushbuffer|GPFIFO|hostSema(4)|finishPayload(4)] with
         * gpFifoOffset = pbGpuVA + channelPbSize, so the finish-payload VA =
         * gpFifoOffset + GPFIFO_SIZE(0x8000) + HOST_SEMA(4) = +0x8004.  Translate
         * via the channel PDB and write the incrementing payload (RM submits
         * payload = lastSubmittedPayload+1 starting at 1). */
        uint64_t sema_va = s->chan_gpfifo_va + 0x8004ull;
        bool is_sys = false;
        uint64_t phys = s->chan_gpfifo_va
            ? nvkvm_chan_translate(s, sema_va, &is_sys) : NVKVM_GMMU_FAULT;
        if (phys != NVKVM_GMMU_FAULT) {
            uint32_t payload = ++s->chan_payload;
            if (is_sys) {
                uint8_t b[4]; stl_le_p(b, payload);
                pci_dma_write(&s->parent_obj, phys, b, 4);
            } else {
                nvkvm_fb_write(s, phys, payload, 4);
            }
            qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x rl=%u ch=%u -> "
                     "completed: semaVA=0x%llx -> %s phys=0x%llx payload=%u\n",
                     s->chip->name, (uint32_t)val,
                     NVKVM_DOORBELL_RUNLIST((uint32_t)val),
                     NVKVM_DOORBELL_CHID((uint32_t)val),
                     (unsigned long long)sema_va, is_sys ? "SYS" : "FB",
                     (unsigned long long)phys, payload);
        } else {
            qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x -> sema VA 0x%llx "
                     "FAULTED (no completion); gpfifo=0x%llx instblk=0x%llx\n",
                     s->chip->name, (uint32_t)val, (unsigned long long)sema_va,
                     (unsigned long long)s->chan_gpfifo_va,
                     (unsigned long long)s->chan_inst_block);
        }
        return;
    }
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

/* M5 — translate a CHANNEL GPU VA to a physical address, rooted at the channel's
 * own PDB (read from its instance block), and report whether the leaf page is in
 * sysmem (GPA) or vidmem (FB) via *out_sys.  Unlike BAR2, channel buffers (the
 * scrubber's pushbuffer/semaphore) live in SYSMEM, so the leaf PTE APERTURE
 * (NV_MMU_VER2_PTE_APERTURE bits 2:1: 0=VID, 2/3=SYS) selects ADDRESS_VID
 * (bits32:8) vs ADDRESS_SYS (bits53:8), both <<12.  The page-table hierarchy
 * itself is assumed to live in FB (the GSP-client RM allocates the page directory
 * from FB, as for BAR2) — read via the PRAMIN/FB backing.  Returns
 * NVKVM_GMMU_FAULT on any miss (caller then does nothing — safe). */
static uint64_t nvkvm_chan_translate(NvkvmGpuEmul *s, uint64_t va, bool *out_sys)
{
    *out_sys = false;
    /* Root the walk at the VAS page-directory base whose VA range contains `va`
     * (snooped from VASPACE_COPY_SERVER_RESERVED_PDES) — the GSP-managed channel
     * instance block is empty in our FB so we can't read the PDB from it. */
    uint64_t tbl = 0;
    for (int i = 0; i < s->chan_vas_n; i++) {
        if (s->chan_vas[i].hvas == s->chan_hvaspace) {
            tbl = s->chan_vas[i].pdb;
            break;
        }
    }
    qemu_log("nvkvm-gpu[%s] M5: chan walk va=0x%llx hvas=0x%08x -> PDB=0x%llx "
             "(%d VAS)\n", s->chip->name, (unsigned long long)va,
             s->chan_hvaspace, (unsigned long long)tbl, s->chan_vas_n);
    if (tbl == 0) {
        return NVKVM_GMMU_FAULT;
    }
    /* PD3->PD2->PD1 (8B PDEs), then PD0 (16B dual PDE) — page tables in FB. */
    static const struct { int hi, lo; } lvl[3] = { {48,47}, {46,38}, {37,29} };
    for (int i = 0; i < 3; i++) {
        uint32_t idx = (uint32_t)((va >> lvl[i].lo) &
                                  ((1ull << (lvl[i].hi - lvl[i].lo + 1)) - 1));
        uint64_t pde = nvkvm_fb_rd64(s, tbl + (uint64_t)idx * 8);
        uint64_t next = NVKVM_VER2_ADDR_VID(pde);
        qemu_log("nvkvm-gpu[%s] M5:   PD%d idx=%u @0x%llx pde=0x%llx -> 0x%llx\n",
                 s->chip->name, 3 - i, idx, (unsigned long long)(tbl + idx * 8),
                 (unsigned long long)pde, (unsigned long long)next);
        tbl = next;
        if (tbl == 0) {
            return NVKVM_GMMU_FAULT;
        }
    }
    uint32_t idx0 = (uint32_t)((va >> 21) & 0xFF);
    uint64_t lo = nvkvm_fb_rd64(s, tbl + (uint64_t)idx0 * 16);
    uint64_t hi = nvkvm_fb_rd64(s, tbl + (uint64_t)idx0 * 16 + 8);
    uint64_t small_tbl = (((hi >> 8) & ((1ull << 25) - 1)) << 12);
    uint64_t big_tbl   = (((lo >> 4) & ((1ull << 28) - 1)) << 8);

    uint64_t pte; uint32_t pgshift; uint64_t pgmask;
    if (small_tbl != 0) {
        pte = nvkvm_fb_rd64(s, small_tbl + (uint64_t)((va >> 12) & 0x1FF) * 8);
        pgshift = 12; pgmask = 0xFFFull;
    } else if (big_tbl != 0) {
        pte = nvkvm_fb_rd64(s, big_tbl + (uint64_t)((va >> 16) & 0x1F) * 8);
        pgshift = 16; pgmask = 0xFFFFull;
    } else {
        return NVKVM_GMMU_FAULT;
    }
    if (!(pte & 1)) {                       /* PTE VALID bit0 */
        return NVKVM_GMMU_FAULT;
    }
    uint32_t aperture = (uint32_t)((pte >> 1) & 0x3);  /* APERTURE bits 2:1 */
    uint64_t page;
    if (aperture == 0) {                    /* VIDEO_MEMORY: ADDRESS_VID 32:8 */
        page = ((pte >> 8) & ((1ull << 25) - 1)) << 12;
        *out_sys = false;
    } else if (aperture == 2 || aperture == 3) { /* SYSTEM_*: ADDRESS_SYS 53:8 */
        page = ((pte >> 8) & ((1ull << 46) - 1)) << 12;
        *out_sys = true;
    } else {
        return NVKVM_GMMU_FAULT;            /* PEER — unsupported */
    }
    (void)pgmask;
    return page + (va & ((1ull << pgshift) - 1));
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
