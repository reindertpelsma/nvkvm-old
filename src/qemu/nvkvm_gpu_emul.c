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
#include "virtio_nvgpu.h"   /* M5: Mode-1 forwarding stack (isolate API + NVOS structs) */
#include "exec/cpu-common.h" /* M6.0: qemu_ram_foreach_block/get_fd — guest-RAM memfd for item-4 */

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

/* Runtime NV_CHANNEL_ALLOC_PARAMS layout used by the guest GSP-RM RPC payload.
 * The generated vGPU copy is smaller; the live guest payload has the full
 * channel SDK layout with errorNotifierMem at +248. */
#define NVKVM_M2_CHAN_ERROR_NOTIFIER_MEM_OFF 248u
#define NVKVM_M2_MEMORY_DESC_SIZE             24u
#define NVKVM_M2_NV_NOTIFICATION_SIZE         16u
#define NVKVM_M2_ADDR_SYSMEM                  1u
#define NVKVM_M2_ADDR_FBMEM                   2u
#define NVKVM_M2_WORK_SUBMIT_NOTIFY_INDEX     1u
#define NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF      20u
#define NVKVM_M2_GSP_SWGEN0_VECTOR            155u
#define NVKVM_M2_GSP_SWGEN0_BIT               (1u << 6)
#define NVKVM_M2_UVM_SHADOW_CAP               262144u
#define NVKVM_M2_GPFIFO_LARGE_RING_ENTRIES    64u
#define NVKVM_M2_GPFIFO_EMPTY_LOG_LIMIT       128u

static inline bool nvkvm_m2_is_gpfifo_channel_class(uint32_t hclass)
{
    return (hclass & 0xffu) == 0x6fu && (hclass & 0xf000u) == 0xc000u;
}

static inline void nvkvm_m2_host_gpu_store_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /*
     * Host GPU vidmem/userd mappings can be WC.  The host doorbell must not
     * overtake QEMU's CPU writes of pushbuffers, QMDs, constants, or UVM data.
     */
    __asm__ __volatile__("sfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}

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
    bool     chan_unresolved;   /* pending entry needs real host/GPA backing before
                                  * local fallback may consume GP_GET */
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
        uint64_t gpfifo_phys;  /* M5.29: BAR1-resolved true ring page, persisted per channel */
        uint64_t pdb;          /* M5.29: content-picked PDB for this channel's working set */
        uint64_t err_notifier_base, err_notifier_size;
        uint32_t gpfifo_ent, gp_get, hvaspace, payload;
        uint32_t err_notifier_as;
        uint32_t work_submit_notifier_index;
        uint32_t client;        /* owning RM client (hClient) — VAS scope key */
        bool     userd_sys;
        uint32_t hobject;       /* M5.12: the channel's RM handle (== host handle: shadow_fwd
                                 * creates the host channel with the SAME hObject) */
        uint32_t host_token;    /* M5.12: host channel work-submit token (0xc36f0108), for the
                                 * GP_PUT-driven doorbell demux: ring THIS channel's token */
        bool     token_valid;
        bool     token_failed;
        uint32_t tsg;           /* M5.25: parent TSG (a06c) handle — must be GPFIFO_SCHEDULE'd
                                 * before a ring runs (guest's schedule control isn't forwarded) */
        bool     scheduled;     /* M5.25: TSG GPFIFO_SCHEDULE'd on the host once */
        bool     host_inflight; /* host GR has been rung for [host_inflight_get, host_inflight_put) */
        uint32_t host_inflight_get;
        uint32_t host_inflight_put;
        uint32_t host_inflight_polls;
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

    /* M5.16 — physical FB pages the guest CPU has WRITTEN via BAR1 (vidmem).
     * For a GSP-managed channel VAS the channel-GMMU walk of gpFifoVA resolves to
     * a stale aliasing page (reads 0), but the guest's OWN CPU mapping of the ring
     * goes through BAR1 (bar1_pdb, CPU-built PTEs) and lands in our FB at the TRUE
     * backing page (proven: compute GPFIFO write hit FB 0x3130000 via BAR1 off
     * 0xa0000, while the VAS walk wrongly gave 0x2eee10000).  Record those pages,
     * most-recent-first, so chan_execute can resolve the GPFIFO to where the guest
     * actually wrote it rather than trusting the unreliable channel-VAS walk. */
#define NVKVM_MAX_BAR1PG 512
    struct { uint64_t page; uint64_t seq; } bar1_wpg[NVKVM_MAX_BAR1PG];
    int      bar1_wpg_n;
    uint64_t bar1_wpg_seq;
    uint64_t chan_gpfifo_phys;  /* M5.16: if non-0, read GP entries DIRECTLY from this
                                 * FB phys (the BAR1-resolved true ring), bypassing the
                                 * stale channel-VAS walk for the GPFIFO entry. */
    uint32_t chan_gp_put_seen;  /* M8.98: GP_PUT value observed by chan_execute's
                                 * prepare read; used if a follow-up USERD read
                                 * invalidates a host USERD overlay back to zero. */
    bool     chan_gp_put_valid;

    /* M7 — CPU interrupt tree (raise MSI-X on LEAF_TRIGGER; ISR reads TOP/LEAF) */
    uint32_t intr_leaf[NVKVM_VF_INTR_NLEAF];     /* pending per leaf reg */
    uint32_t intr_leaf_en[NVKVM_VF_INTR_NLEAF];  /* enables */
    uint32_t intr_top;                           /* pending subtree bitmask (TOP(0)) */
    uint32_t intr_top_en;

    /* M5/M7 — GSP os-event delivery.  cuCtxCreate's blocking-sync wait parks
     * libcuda in poll() on an os-event fd; the channel completes (semaphore
     * released) but no completion interrupt is delivered, so poll() never wakes
     * (lost wakeup).  We record every NV01_EVENT_OS_EVENT (0x0079) alloc, and on
     * a doorbell-completed channel post a GSP NV_VGPU_MSG_EVENT_POST_EVENT
     * (0x1003) for each + raise the GSP falcon SWGEN0 interrupt (vector 155 =
     * MC_ENGINE_IDX_GSP stall) so kgspService drains the queue -> _kgspRpcPostEvent
     * -> osNotifyEvent -> nv_post_event wakes the poll.  Mirrors Mode-1 #127. */
    struct {
        uint32_t hclient, hparent, hevent, hsrc, event_class, notify_index;
        uint64_t data;
    } osevents[64];
    int      osevent_n;
#define NVKVM_M2_HOST_COMPLETION_Q 256
    struct {
        uint32_t token;                           /* host work-submit token to post when SWGEN0 drains */
        bool     token_valid;
    } m2_host_completion_q[NVKVM_M2_HOST_COMPLETION_Q];
    uint16_t m2_host_completion_head;
    uint16_t m2_host_completion_tail;
    uint32_t m2_host_completion_dropped;
    bool     m2_local_completion_pending;        /* local CE/setup completion happened before an os-event existed */
    uint32_t m2_local_completion_token;          /* guest work-submit token that caused the local completion */
    bool     m2_local_completion_token_valid;
    uint32_t m2_local_completion_posted_token;   /* M8.107: last local token posted to current os-event set */
    bool     m2_local_completion_posted_valid;
    int      m2_local_completion_posted_osevent_n;
    bool     gsp_swgen0_pending;                 /* GSP falcon IRQSTAT SWGEN0 latched */
    /* VAS root page-dir bases snooped from VASPACE_COPY_SERVER_RESERVED_PDES
     * (0x90f10106): levels[0].physAddress roots the WHOLE VAS (the params' VA
     * range is only the reserved window, not the VAS extent), keyed by the
     * VASpace handle (control hObject).  Matched to a channel via the channel's
     * hVASpace.  This is the channel PDB source (the GSP-managed instblk is empty
     * in our FB). */
    struct { uint32_t hvas; uint64_t pdb; } chan_vas[16];
    int      chan_vas_n;
    uint32_t chan_hvaspace;    /* the tracked channel's hVASpace handle */

    /* DEBUG-PROOF backdoor (mode2_uvm_complete): the patched guest UVM reports
     * its tracking-semaphore GPA + payload here so QEMU can forge the channel
     * completion for GSP-internal UVM channels whose sysmem mappings aren't in
     * the RPC stream (see docs/design/mode2_address_virtualization.md).  This is
     * a bring-up PROOF that forging the completion unblocks cuInit; production
     * needs a validated guest<->VMM mapping-report channel (untrusted guest). */
    uint32_t dbg_gpa_lo, dbg_gpa_hi;

    /* M5 compute forwarding (docs/design/mode2_compute_forwarding.md). The
     * emulated GPU hosts its own forwarding backend (separate QEMU process from
     * any Mode-1 instance, so this is additive and cannot disturb Mode-1). M5.0
     * smoke test validates the Mode-2-process -> host-GPU path at realize(). */
    bool     m2fwd;                      /* device prop: host-GPU forwarding (default ON; debug-only off) */
    struct nvkvm_isolate_table m2_iso;   /* per-Mode-2 isolate table (own host stub) */
    bool     m2_iso_ready;               /* lazy: isolate created + devices opened   */
    uint32_t m2_iso_id;                  /* the per-guest host isolate id            */
    uint32_t m2_ctl_h;                   /* handle for /dev/nvidiactl in the isolate */
    uint32_t m2_gpu_h;                   /* handle for /dev/nvidia0                   */
    int      m2_gpu_fd;                  /* the stub's /dev/nvidia0 fd in QEMU (SCM_RIGHTS) — for mmap */
    uint32_t m2_fwd_n;                   /* count of forwarded RPCs (diag)           */
    /* M5.1b: guest RM client handle -> host (synthetic, non-colliding) client.
     * Guest client handles live in the global 0xc1xxxxxx namespace and collide
     * with pre-existing host clients (NV_ERR_INSERT_DUPLICATE_NAME), so we remap
     * each to a 0xdeadNNNN handle the host RM accepts, and translate client refs
     * (h_root, and h_object_parent when it names a client). Objects stay verbatim. */
    struct { uint32_t g, h; } m2_cmap[128];
    int      m2_cmap_n;
    /* M5.3: per-mapping fresh /dev/nvidia0 isolate-fd handle allocator. Handles
     * 1=ctl, 2=gpu are fixed; on-demand context-buffer mappings draw from here
     * (nvidia binds exactly one CPU mapping per device fd, so each needs its own). */
    uint32_t m2_maph_next;
    /* M5.3: VASpaces (FERMI_VASPACE_A 0x90f1) forwarded under each (client,device),
     * so we can give the GR channelgroup an explicit hVASpace when the guest left it
     * 0 (device-default), which doesn't resolve on the forwarded host device. */
    struct { uint32_t client, dev, vas; } m2_devvas[32];
    int      m2_devvas_n;
    /* M5.3: TSG (0xa06c) handle -> engineType, so a channel that passes engineType=0
     * (NULL, "inherit") can be given its TSG's engine explicitly on the host. */
    struct { uint32_t tsg, engine; } m2_tsgeng[32];
    int      m2_tsgeng_n;
    /* M5.3 data-plane: the GR-client subdevice (NV20_SUBDEVICE_0 0x2080) handle, needed
     * to issue GR_GET_CTX_BUFFER_INFO on the host shadow context after the compute object
     * is forwarded — the first step of backing the guest's context buffers with real host
     * GPU state (the proven cuCtxCreate fix). Tracked per GR client. */
    struct { uint32_t client, subdev; } m2_subdev[64];
    int      m2_subdev_n;
    struct { uint32_t client, subdev, diag; bool failed; } m2_diag[16];
    int      m2_diag_n;
    struct { uint32_t client, hobject; } m2_debug[16];
    int      m2_debug_n;
    uint32_t m2_post_launch_diag_logs;
    /* M14: device-info-table (engine enumeration) captured LIVE from the host GPU via a
     * private QEMU-owned client/subdevice — no hardcoded per-GPU blob. 100B/entry, 32/page. */
    uint8_t  m2_devinfo[256 * 100];
    uint32_t m2_devinfo_n;               /* entries captured (0 = none) */
    bool     m2_devinfo_tried;           /* capture attempted once (success or fail) */
#define NVKVM_M2_MAX_FBBACK 512
    /* M5.3 DATA-PLANE (double-mmap): FB ranges backed by real host GPU memory. When the
     * guest reads/writes a context-buffer FB address that we've backed with the host
     * shadow context's counterpart (mapped via the proven RM_MAP_MEMORY primitive), serve
     * it from host_qva instead of the local g_malloc0 FB page — so the guest sees real
     * GPU-initialized state (the proven cuCtxCreate fix). Inert until populated. The
     * bring-up mechanism; KVM-memslot backing is the perf endpoint (see design doc). */
    struct { uint64_t fb_base, size; void *host_qva; } m2_fbback[NVKVM_M2_MAX_FBBACK];
    int      m2_fbback_n;
    /* M5.3 DIAG: crash-window FB-read probe. Set true the moment the GR compute
     * object (0xc7c0) alloc returns OK — libcuda then reads GR-context GPU memory
     * (no further ioctl per the trace) and crashes (rbp=0). Logging every FB read
     * after this flag pins the EXACT buffer (fb_addr+value+backed?) and its access
     * path: appears here => served via FB/BAR1 (FB-overlay backing applies); window
     * empty => libcuda reads it via the UVM mmap to guest-RAM (needs memslot). */
    bool     m2_crashwin;
    uint32_t m2_crashwin_reads;
    bool     m2_in_walk;     /* true while reading a GMMU PDE/PTE — excludes page-walk
                              * noise from the CRASHWIN probe so only LEAF data reads
                              * (the buffer values libcuda actually consumes) are logged */
    /* M5.4 data-plane: per forwarded channel, the host-allocated USERD we provide as
     * hUserdMemory[0] (handle-bearing -> mappable, unlike RM's own USERD) and mmap into
     * QEMU, registered in m2_fbback at the guest's userd.base. So the host channel's real
     * USERD IS the guest's USERD view (double-mmap): guest GP_PUT lands in host USERD and
     * the host GPU advances GP_GET there for the guest's poll to see. */
    struct { uint32_t client, chan; uint32_t h_userd; void *qva; uint64_t fb_base, size; }
             m2_chanbuf[32];
    int      m2_chanbuf_n;
    uint32_t m2_databuf_next;   /* unique host handle allocator for data-plane objects */
    uint64_t m2_cur_gva;        /* BAR1/BAR2 aperture GPU VA of the in-flight leaf access
                                 * (~0=none), so the CRASHWIN probe can report the guest GPU
                                 * VA that maps to a polled FB address. */
    bool     m2_mapdma_tested;  /* M5.5: one-shot RM_MAP_MEMORY_DMA-FIXED primitive validation */
    bool     m2_inventory_done; /* M5.6: one-shot GR working-set inventory dump at doorbell */
    bool     m2_sem_probe_done; /* M5.13: one-shot DRY-RUN locate of the completion semaphore PDB */
    struct { uint64_t page; uint32_t reads; bool probed; } m2_hotzero[8]; /* M8.8 diag */
    uint32_t m2_hotzero_probe_count;     /* M8.46: cap expensive root probes */
    uint32_t m2_hotzero_notify_releases; /* M8.46: bounded notifier-scan releases */
    bool     m2exec;            /* M5.7 prop: execution-plane backing (default ON; debug-only off) */
    bool     m2_exec_done;      /* M5.7: one-shot working-set back+map */
    uint32_t m2_exec_sweeps;    /* M5.10: # of doorbell-time GR-VAS re-sweeps done (bounded) */
    uint32_t m2_last_db_token;  /* M5.11: last guest work-submit token seen at the doorbell (dedup log) */
    bool     m2_last_db_valid;
    uint32_t m2_gr_client;      /* M5.7: the GR compute client (set at crashwin arm) */
    uint64_t m2_gr_report_sem_addr;    /* M8.32: last real GR/UVM report semaphore */
    uint32_t m2_gr_report_sem_payload; /* M8.32: last payload written there */
    bool     m2_gr_report_sem_one_word;
    /* M5.7: per-client NV01_MEMORY_VIRTUAL mapper over the client's GR VASpace, so the
     * execution path can map_dma FIXED the guest's working-set buffers into the host
     * channel's address space at the guest VAs (see [[mode2-mapdma-primitive]]). */
    struct { uint32_t client, hvirt, hvas, hdev; } m2_grmap[8];
    int      m2_grmap_n;
    /* M5.28 PER-CHANNEL VAS (user-directed): each forwarded GR channel runs in its OWN
     * fresh nvkvm-allocated VAS (FERMI_VASPACE_A under the channel's forwarded device),
     * NOT the guest's forwarded VAS (0xcaf00005) — that one the host RM auto-promoted its
     * GR ctx into, so the guest's working-set VAs collide (st=0x51 / Xid 32). Keyed by the
     * parent TSG handle (all channels in a TSG share its VAS). The working set is mapped
     * into THIS vas (fvirt over fvas) instead of the per-client grmapper. m2_cur_cvas is the
     * active index for the current map ops (set per-channel in the doorbell loop; -1 = use
     * the legacy per-client grmapper, e.g. CeUtils). */
    struct { uint32_t client, tsg, hdev, fvas, fvirt; bool populated; } m2_cvas[16];
    int      m2_cvas_n;
    int      m2_cur_cvas;
    uint32_t m2_gr_channel;     /* M5.8: the host GR channel handle (c56f under GR TSG) */
    uint32_t m2_gr_tsg;         /* M5.8: the host GR TSG handle (a06c, channel's parent) */
    /* M7 (cuCtxCreate fix): the HOST's real GR-object alloc reply params (NV_GR_ALLOCATION_
     * PARAMETERS, 16B incl the GSP-filled `caps` output @+12). Captured by shadow_fwd after the
     * forwarded 0xc7c0 alloc, then passed through into the GSP-RPC reply (resp+112) so the guest
     * copies the REAL caps back instead of the echoed request (caps=0). The auditor proved the
     * guest copies the LOCAL class size (16B) from resp+112 ignoring reply paramsSize, so the
     * old M5.3 force-paramsSize->0 was moot; forwarding the real params is the correct fix. */
    uint8_t  m2_gr_reply[64];
    uint32_t m2_gr_reply_obj;   /* hObject this reply belongs to (match in the reply builder) */
    uint32_t m2_gr_reply_psize; /* host's RETURNED alloc_parms_size (paramsSize the real RM wrote) */
    bool     m2_gr_reply_valid;
    uint32_t m2_chan_reply_obj; /* hObject whose forwarded channel reply was captured */
    uint32_t m2_chan_reply_internal_flags;
    bool     m2_chan_reply_valid;
    void    *m2_usermode_qva;   /* M5.8: mmap of host AMPERE_USERMODE_A doorbell page */
    uint32_t m2_gr_token;       /* M5.8: host GR channel work-submit token (doorbell value) */
    bool     m2_doorbell_ready; /* M5.8: usermode mapped + token fetched */
    uint64_t m2semval;          /* M5.14 DIAG prop: if nonzero, fb_read of m2sempage returns this
                                 * (satisfy the guest-kernel post-PROMOTE_CTX ctx-completion poll
                                 * that nothing writes in the fake-GSP model; userspace never
                                 * observes it). 0 = disabled. */
    uint64_t m2sempage;         /* M5.14: guest-FB page (4K-aligned) the sentinel applies to */
    bool     m2_sem_page_released; /* M8.45: reactive debug release for hot-polled sempage */
    uint32_t m2semforcehigh;    /* M8.100 DEBUG: after a stuck mirrored CE finish poll, force high */
    uint32_t m2_sempage_force_reads;
    bool     m2_sempage_force_done;
    char    *m2pbmap_path;      /* M8.11 DEBUG: host file with guest user-mmap
                                  * pushbuffer VA->GPA ranges, one "va gpa size" per line */
    uint32_t m2mapflags;        /* M8.76 DEBUG: exact RM_MAP_MEMORY_DMA flags override */
    uint32_t m2mapflags_high;   /* M8.76 DEBUG: exact flags override for high/UVM VAs */
    uint32_t m2pcas;            /* M8.89 DEBUG: PCAS injection mode for inline-QMD launches */
    uint32_t m2refscan;         /* M8.94 DEBUG: broad adjacent-word UVM ref scanner */
    uint32_t m2setobj;          /* M8.95 DEBUG: synthetic compute SET_OBJECT prefix */
    uint32_t m2legacyvas;       /* M8.97 DEBUG: old c7c0-time forwarded-VAS sweep */
    uint32_t m2_mapdma_logs;
    uint32_t m2_mapdma_high_logs;
    struct { uint64_t va, gpa, size; } m2_pbmap[8192];
    int      m2_pbmap_n;
    time_t   m2_pbmap_mtime;
    int64_t  m2_pbmap_size;
    uint32_t m2_pbmap_logs;
    /* M8.14 DEBUG: guest-kernel HtoD shadow rows.  A debug guest kernel module
     * copies cuMemcpyHtoD source bytes into guest RAM and reports
     * <deviceVA, shadowGPA, size> through BAR0 0xFFF520..0xFFF538. */
    struct { uint64_t va, gpa, size; bool bar1; } m2_uvm_shadow[NVKVM_M2_UVM_SHADOW_CAP];
    int      m2_uvm_shadow_n;
    uint32_t m2_uvm_shadow_logs;
    bool     m2_uvm_shadow_retry_pending;
    /* M8.15: UVM external-allocation device ranges.  The guest bridge reports
     * UVM_MAP_EXTERNAL_ALLOCATION as <base, len>.  QEMU allocates one coherent
     * host object per range, exposes it to local CE resolution via a fake GPGA
     * alias at the same numeric VA, and maps it into the active GR cvas before
     * ringing the host channel. */
    struct {
        uint64_t va, size;
        int obj_idx;
        uint32_t mapped_cvas_mask;
        uint32_t hClient, hMemory; /* M8.90: original UVM_MAP_EXTERNAL backing */
    } m2_uvm_ext[256];
    int      m2_uvm_ext_n;
    uint32_t m2_uvm_ext_logs;
    uint32_t m2_uvm_map_logs;
    uint32_t m2_uvm_lo, m2_uvm_hi;
    uint32_t m2_uvm_gpa_lo, m2_uvm_gpa_hi;
    uint32_t m2_uvm_size_lo, m2_uvm_size_hi;
    /* M5.27: VAs already backed+mapped (dedup pushbuffer/sema/gpfifo maps).  Was 128 — the
     * compute working set (30 GP entries x ~8 channels of pushbuffers + semas + gpfifos) blows
     * past that, and once full the dedup silently STOPPED recording, so every VA re-mapped on
     * EVERY doorbell -> dmaAllocMapping flood + a leaked host mem object per re-map + host VAS
     * exhaustion -> legitimate buffer maps then failed -> the host GPU stalled mid-channel
     * (GP_GET stuck). Size it well past any cuCtxCreate/matmul working set. */
#define NVKVM_MAX_MAPPED_VA 65536
    struct { uint32_t client; int cvas; uint64_t va; } m2_mapped_va[NVKVM_MAX_MAPPED_VA];
    int      m2_mapped_va_n;
    /* M6.0 (item-4 prereq): guest RAM as a shared memfd, so the STUB can mmap any guest GPA
     * and OS_DESCRIPTOR-register it -> the host GPU can DMA into the guest's sysmem GR buffers
     * (the un-backed objects libcuda reads, [[mode2-cuctxcreate-pagetable-poll]]). Found at
     * realize from the largest fd-backed RAMBlock (memory-backend-memfd,share=on). */
    int      m2_guest_ram_fd;   /* memfd fd of guest RAM (-1 if not memfd-backed) */
    void    *m2_guest_ram_hva;  /* QEMU host VA of guest RAM base */
    uint64_t m2_guest_ram_size;
    /* M6.1 (item-4 step 2): the guest-RAM memfd shared into the STUB. handle_table = the
     * Mode-1 fd registry ([[mode2-per-proc-isolate-handle-reuse]]) so we can send_handle +
     * isolate_mmap. The stub MAP_FIXEDs guest RAM at m2_stub_ram_base; for a guest GPA G the
     * stub VA is m2_stub_ram_base+G -> OS_DESCRIPTOR there for host-GPU DMA (item-4 step 3). */
    struct nvkvm_handle_table m2_ht;
    uint32_t m2_guest_ram_handle;
    uint64_t m2_stub_ram_base;  /* stub VA where guest RAM is MAP_FIXED (0 = not shared) */
    bool     m2_ram_shared;
    bool     m2_gpu_registered;  /* M6.2: m2_gpu_h REGISTER_FD'd to the ctl session */

    /* M7 REFACTOR (user-directed) — the proper memory model that replaces fb_pages/m2_fbback:
     * gpu_memory_object = ONE real backing (host RM alloc, double-mmapped: cpu_qva for the
     * guest-CPU/QEMU view, gr_va for the host-GPU view via the host GR VAS). The GPGA page
     * table maps a guest-GPU-physical range -> (object, offset). The SAME nvkvm/RM handle backs
     * both views, so host GPU and guest CPU are coherent. See docs/design/mode2_dataplane_
     * architecture.md "REFACTOR PLAN". m2_fbback stays as the legacy fallback until retired. */
    struct {
        uint8_t  mode;        /* 0=physical(FB-backed general), 1=special(reg page) */
        void    *cpu_qva;     /* QEMU/guest-CPU mapping of the host object (NULL=none) */
        uint64_t size;
        uint32_t client;      /* host RM client (nvkvm-tracked) */
        uint32_t hMemory;     /* host RM object handle = the 'real' backing */
        uint64_t gr_va;       /* host GR-VAS VA where map_dma'd (0=not GPU-mapped) */
        bool     forwarded;   /* real forwarded guest RM/UVM object, not scratch */
    } m2_objs[128];
    int      m2_objs_n;
    struct {                  /* GPGA page-range -> (object, offset_in_target) */
        uint64_t gpga_base, size;
        int      obj_idx;     /* index into m2_objs[] (-1 = none) */
        uint64_t off;         /* offset_in_target */
        bool     readable, writable;
    } m2_gpga[256];
    int      m2_gpga_n;

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

static void nvkvm_m2_probe_sem_pdb(NvkvmGpuEmul *s, uint32_t client, uint64_t target);
static void nvkvm_fb_write(NvkvmGpuEmul *s, uint64_t fb_addr, uint64_t val,
                           unsigned size);
static void nvkvm_phys_wr32(NvkvmGpuEmul *s, uint64_t phys, bool sys,
                            uint32_t v);
static void nvkvm_m2_write_guest_userd_gp_get(NvkvmGpuEmul *s,
                                              const struct nvkvm_chan_entry *c,
                                              uint32_t gp_get,
                                              const char *why);
static void nvkvm_m2_repair_guest_userd_gp_put(NvkvmGpuEmul *s,
                                               const struct nvkvm_chan_entry *c,
                                               uint32_t submitted_put,
                                               const char *why);

#define NVKVM_M2_NOTIFY_SCAN_FIRST  0x0f4u
#define NVKVM_M2_NOTIFY_SCAN_LAST   0xffcu
#define NVKVM_M2_NOTIFY_EAGER_LAST  0x1fcu

static bool nvkvm_m2_release_sempage_on_poll(NvkvmGpuEmul *s, uint64_t fb_addr,
                                             unsigned size, uint64_t *value,
                                             const char *source)
{
    (void)size;
    (void)value;
    uint64_t page = fb_addr & ~0xfffull;

    if (!s->m2exec || s->m2semval || !s->m2sempage ||
        page != (s->m2sempage & ~0xfffull) ||
        !s->m2_gr_report_sem_payload) {
        return false;
    }

    if (!s->m2_sem_page_released) {
        nvkvm_fb_write(s, s->m2sempage, s->m2_gr_report_sem_payload, 4);
        s->m2_sem_page_released = true;
        qemu_log("nvkvm-gpu[%s] M8.45 SEMPAGE_POLL_RELEASE "
                 "fb=0x%llx poll=0x%llx payload=%u source=%s\n",
                 s->chip->name, (unsigned long long)s->m2sempage,
                 (unsigned long long)fb_addr, s->m2_gr_report_sem_payload,
                 source ? source : "?");
    }

    return false;
}

static bool nvkvm_m2_mirror_kernel_ce_progress(NvkvmGpuEmul *s, uint64_t sem_addr,
                                               uint32_t payload, uint64_t *out_redir)
{
    /*
     * The guest RM polls BAR2 VA 0xfea000 -> FB 0x2efbaf000 while the local
     * kernel CE scrubber channel releases its finish payload at
     * gpfifoVA+0x8004 (observed 0x12006c004).  A fixed sentinel only clears the
     * first payload; mirror the actual incrementing payload as a proof bridge.
     */
    if (!s->m2exec || s->m2semval || !s->m2sempage || !s->chan_gpfifo_va ||
        sem_addr != s->chan_gpfifo_va + 0x8004ull ||
        s->chan_gpfifo_va != 0x120064000ull) {
        return false;
    }

    nvkvm_fb_write(s, s->m2sempage, payload, 4);
    if (out_redir) {
        *out_redir = s->m2sempage;
    }

    static uint32_t mirror_logs;
    if (s->trace && (mirror_logs++ < 128 || (mirror_logs & 0xffu) == 0)) {
        qemu_log("nvkvm-gpu[%s] M8.99 CE_PROGRESS_MIRROR "
                 "gpfifo=0x%llx sem=0x%llx payload=%u fb=0x%llx count=%u\n",
                 s->chip->name, (unsigned long long)s->chan_gpfifo_va,
                 (unsigned long long)sem_addr, payload,
                 (unsigned long long)s->m2sempage, mirror_logs);
    }
    return true;
}

static void nvkvm_m2_force_stuck_sempage_high(NvkvmGpuEmul *s, uint64_t fb_addr,
                                              unsigned size, uint64_t *value,
                                              const char *source)
{
    /*
     * Narrow proof knob: the kernel CE scrubber currently mirrors payload 51 to
     * the guest-polled BAR2 sempage, then the guest keeps reading the same word.
     * If forcing that finish payload high advances cuCtxCreate, the remaining
     * production work is to find the missing target/completion accounting.
     */
    if (!s->m2exec || !s->m2semforcehigh || s->m2semval || !s->m2sempage ||
        !value || size != 4 ||
        (fb_addr & ~0xfffull) != (s->m2sempage & ~0xfffull) ||
        fb_addr != s->m2sempage || *value < 51u ||
        *value >= 0x80000000ull) {
        return;
    }

    if (s->m2_sempage_force_reads != UINT32_MAX) {
        s->m2_sempage_force_reads++;
    }
    if (!s->m2_sempage_force_done && s->m2_sempage_force_reads >= 16) {
        uint64_t old = *value;
        uint32_t forced = 0xffffffffu;
        nvkvm_fb_write(s, s->m2sempage, forced, 4);
        *value = forced;
        s->m2_sempage_force_done = true;
        qemu_log("nvkvm-gpu[%s] M8.100 SEMPAGE_STUCK_FORCE "
                 "fb=0x%llx from=0x%llx to=0xffffffff reads=%u source=%s\n",
                 s->chip->name, (unsigned long long)s->m2sempage,
                 (unsigned long long)old, s->m2_sempage_force_reads,
                 source ? source : "?");
    }
}

static void nvkvm_m2_note_hotzero(NvkvmGpuEmul *s, uint64_t fb_addr, unsigned size,
                                  uint64_t value, const char *source)
{
    if (!s->m2_crashwin || s->m2_in_walk || value != 0) {
        return;
    }
    uint64_t page = fb_addr & ~0xfffull;
    int slot = -1, empty = -1, cold = 0;
    for (int i = 0; i < 8; i++) {
        if (s->m2_hotzero[i].page == page) {
            slot = i;
            break;
        }
        if (!s->m2_hotzero[i].page && empty < 0) {
            empty = i;
        }
        if (s->m2_hotzero[i].reads < s->m2_hotzero[cold].reads) {
            cold = i;
        }
    }
    if (slot < 0) {
        slot = empty >= 0 ? empty : cold;
        s->m2_hotzero[slot].page = page;
        s->m2_hotzero[slot].reads = 0;
        s->m2_hotzero[slot].probed = false;
    }
    if (s->m2_hotzero[slot].reads != UINT32_MAX) {
        s->m2_hotzero[slot].reads++;
    }
    if (!s->m2_hotzero[slot].probed && s->m2_hotzero[slot].reads == 64) {
        static uint32_t hotzero_logs;
        bool sempage = s->m2sempage &&
                       page == (s->m2sempage & ~0xfffull);
        bool probe_roots = sempage && s->m2_hotzero_probe_count < 1;
        s->m2_hotzero[slot].probed = true;
        if (s->trace && (sempage || hotzero_logs++ < 128)) {
            qemu_log("nvkvm-gpu[%s] M8.8 HOTZERO page=0x%llx sample_fb=0x%llx "
                     "sz=%u source=%s gva=0x%llx client=0x%08x reads=%u; %s\n",
                     s->chip->name, (unsigned long long)page,
                     (unsigned long long)fb_addr, size, source ? source : "?",
                     (unsigned long long)s->m2_cur_gva, s->m2_gr_client,
                     s->m2_hotzero[slot].reads,
                     probe_roots ? "probing roots" : "root probe skipped");
        }
        if (probe_roots) {
            s->m2_hotzero_probe_count++;
            nvkvm_m2_probe_sem_pdb(s, s->m2_gr_client, page);
        }
        if (s->m2exec && !s->m2semval && !s->m2_sem_page_released &&
            sempage && s->m2_gr_report_sem_payload) {
            nvkvm_fb_write(s, s->m2sempage, s->m2_gr_report_sem_payload, 4);
            s->m2_sem_page_released = true;
            qemu_log("nvkvm-gpu[%s] M8.45 SEMPAGE_HOTZERO_RELEASE "
                     "fb=0x%llx payload=%u source=%s\n",
                     s->chip->name, (unsigned long long)s->m2sempage,
                     s->m2_gr_report_sem_payload, source ? source : "?");
        }
    }
}

/* M5.3 DATA-PLANE: if fb_addr falls in a range backed by real host GPU memory
 * (double-mmap), return the host VA for that byte; else NULL (use local FB page).
 * Inert until m2_fbback[] is populated. */
/* M5.15 DIAG: log every device->guest DMA write while the crash-window is armed, to catch the
 * mistranslated pci_dma_write that zeroes libcuda's saved-rbp slot (the cuCtxCreate rbp=0 SIGSEGV).
 * Set from realize; gated on m2_crashwin so it only fires after the 0xc7c0 GR alloc. */
static NvkvmGpuEmul *g_nvkvm_dma_s;
static int g_nvkvm_dma_logs;
static MemTxResult nvkvm_dmaw(PCIDevice *dev, dma_addr_t gpa, const void *buf, dma_addr_t len)
{
    if (g_nvkvm_dma_s && g_nvkvm_dma_s->m2_crashwin && g_nvkvm_dma_logs < 512) {
        g_nvkvm_dma_logs++;
        uint64_t v0 = (len >= 8) ? ldq_le_p(buf) : (len >= 4 ? ldl_le_p(buf) : 0);
        void *caller = __builtin_return_address(0);
        qemu_log("nvkvm-gpu[GA106] M5.15 DMAW gpa=0x%llx len=%llu v0=0x%llx site=%p\n",
                 (unsigned long long)gpa, (unsigned long long)len, (unsigned long long)v0, caller);
    }
    return pci_dma_write(dev, gpa, buf, len);
}

static void nvkvm_m2_flush_host_cpu_range(const void *addr, uint64_t size);
static void nvkvm_m2_invalidate_host_cpu_range(const void *addr, uint64_t size);

static uint8_t *nvkvm_fb_host_overlay(NvkvmGpuEmul *s, uint64_t fb_addr)
{
    /* Explicit double-mmap ranges (USERD, GPFIFO bridge, ctx shadows) are live
     * host-channel objects.  They must override generic GPGA aliases at the same
     * guest-FB address; otherwise GP_PUT writes can update an inert object while
     * the host channel's USERD qva stays stale. */
    for (int i = s->m2_fbback_n - 1; i >= 0; i--) {
        if (fb_addr >= s->m2_fbback[i].fb_base &&
            fb_addr <  s->m2_fbback[i].fb_base + s->m2_fbback[i].size) {
            return (uint8_t *)s->m2_fbback[i].host_qva +
                   (fb_addr - s->m2_fbback[i].fb_base);
        }
    }
    /* M7 REFACTOR: the GPGA table (gpu_memory_object model) maps remaining FB
     * aliases to their backing gpu_memory_object. Empty => local fb_pages. */
    for (int i = 0; i < s->m2_gpga_n; i++) {
        if (fb_addr >= s->m2_gpga[i].gpga_base &&
            fb_addr <  s->m2_gpga[i].gpga_base + s->m2_gpga[i].size) {
            int oi = s->m2_gpga[i].obj_idx;
            if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva) {
                break;                       /* GPGA known but no CPU backing -> fb_pages */
            }
            return (uint8_t *)s->m2_objs[oi].cpu_qva + s->m2_gpga[i].off +
                   (fb_addr - s->m2_gpga[i].gpga_base);
        }
    }
    return NULL;
}

static bool nvkvm_m2_fbback_covers(NvkvmGpuEmul *s, uint64_t fb_addr,
                                   uint64_t size)
{
    uint64_t end = fb_addr + size;
    if (end < fb_addr) {
        return false;
    }
    uint64_t cur = fb_addr;
    while (cur < end) {
        uint64_t next = cur;
        for (int i = s->m2_fbback_n - 1; i >= 0; i--) {
            uint64_t b = s->m2_fbback[i].fb_base;
            uint64_t e = b + s->m2_fbback[i].size;
            if (e < b) {
                continue;
            }
            if (cur >= b && cur < e && e > next) {
                next = e;
            }
        }
        if (next == cur) {
            return false;
        }
        cur = next;
    }
    return true;
}

/* Aligned reg accesses never straddle a 4 KiB page. */
static uint64_t nvkvm_fb_read(NvkvmGpuEmul *s, uint64_t fb_addr, unsigned size)
{
    /* M5.14 DIAG: satisfy the guest-kernel post-PROMOTE_CTX completion poll. The fake-GSP model
     * never runs the real golden-image/ctx-init work, so the vidmem status word the guest RM
     * busy-polls stays 0 forever. Inject a sentinel (host owns real ctx-switch; guest userspace
     * never observes this kernel-internal word). One read per offset within the page is served. */
    if (s->m2semval && (fb_addr & ~0xfffull) == (s->m2sempage & ~0xfffull)) {
        uint64_t v = (size >= 8) ? s->m2semval : (s->m2semval & ((1ull << (size * 8)) - 1));
        if (s->m2_crashwin && s->m2_crashwin_reads < 512) {
            s->m2_crashwin_reads++;
            qemu_log("nvkvm-gpu[GA106] M5.14 SEM-INJECT fb=0x%llx sz=%u -> 0x%llx\n",
                     (unsigned long long)fb_addr, size, (unsigned long long)v);
        }
        return v;
    }
    uint64_t sem_v;
    if (nvkvm_m2_release_sempage_on_poll(s, fb_addr, size, &sem_v, "FB-RD")) {
        if (s->m2_crashwin && s->m2_crashwin_reads < 512) {
            s->m2_crashwin_reads++;
            qemu_log("nvkvm-gpu[GA106] M8.45 SEMPAGE-RD fb=0x%llx sz=%u -> 0x%llx\n",
                     (unsigned long long)fb_addr, size,
                     (unsigned long long)sem_v);
        }
        return sem_v;
    }
    uint8_t *hp = ((s->m2_fbback_n || s->m2_gpga_n) ? nvkvm_fb_host_overlay(s, fb_addr) : NULL);
    if (hp) {                            /* M5.3: served from real host GPU memory */
        uint64_t v;
        nvkvm_m2_invalidate_host_cpu_range(hp, size);
        switch (size) {
        case 1: v = *hp; break;
        case 2: v = lduw_le_p(hp); break;
        case 4: v = ldl_le_p(hp); break;
        case 8: v = ldq_le_p(hp); break;
        default: v = 0; break;
        }
        if (s->m2_crashwin && !s->m2_in_walk && s->m2_crashwin_reads < 512) {
            s->m2_crashwin_reads++;
            qemu_log("nvkvm-gpu[GA106] CRASHWIN RD fb=0x%llx sz=%u = 0x%llx "
                     "(HOST-BACKED) gva=0x%llx\n", (unsigned long long)fb_addr, size,
                     (unsigned long long)v, (unsigned long long)s->m2_cur_gva);
        }
        nvkvm_m2_force_stuck_sempage_high(s, fb_addr, size, &v, "HOST-BACKED");
        nvkvm_m2_note_hotzero(s, fb_addr, size, v, "HOST-BACKED");
        return v;
    }
    uint8_t *p = nvkvm_fb_page(s, fb_addr, false);
    uint32_t o = fb_addr & 0xfffu;
    uint64_t v;
    if (!p) {
        v = 0;
    } else {
        switch (size) {
        case 1: v = p[o]; break;
        case 2: v = lduw_le_p(p + o); break;
        case 4: v = ldl_le_p(p + o); break;
        case 8: v = ldq_le_p(p + o); break;
        default: v = 0; break;
        }
    }
    nvkvm_m2_force_stuck_sempage_high(s, fb_addr, size, &v,
                                      p ? "LOCAL-FB" : "UNBACKED-ZERO");
    /* M5.3 DIAG: crash-window probe — log FB reads after the 0xc7c0 alloc. A read
     * returning 0 from an UN-backed page (p==NULL) is a prime suspect for the value
     * that corrupts libcuda's frame; its fb_addr identifies the buffer to back. */
    if (s->m2_crashwin && !s->m2_in_walk && s->m2_crashwin_reads < 512) {
        s->m2_crashwin_reads++;
        qemu_log("nvkvm-gpu[GA106] CRASHWIN RD fb=0x%llx sz=%u = 0x%llx%s gva=0x%llx\n",
                 (unsigned long long)fb_addr, size, (unsigned long long)v,
                 p ? "" : " (UNBACKED-ZERO)", (unsigned long long)s->m2_cur_gva);
    }
    nvkvm_m2_note_hotzero(s, fb_addr, size, v, p ? "LOCAL-FB" : "UNBACKED-ZERO");
    return v;
}

static void nvkvm_fb_write(NvkvmGpuEmul *s, uint64_t fb_addr, uint64_t val,
                           unsigned size)
{
    uint8_t *hp = ((s->m2_fbback_n || s->m2_gpga_n) ? nvkvm_fb_host_overlay(s, fb_addr) : NULL);
    if (hp) {                            /* M5.3: written through to real host GPU memory */
        switch (size) {
        case 1: *hp = (uint8_t)val; break;
        case 2: stw_le_p(hp, (uint16_t)val); break;
        case 4: stl_le_p(hp, (uint32_t)val); break;
        case 8: stq_le_p(hp, val); break;
        default: break;
        }
        nvkvm_m2_flush_host_cpu_range(hp, size);
        return;
    }
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

static bool nvkvm_m2_write_work_submit_notifier(NvkvmGpuEmul *s,
                                                struct nvkvm_chan_entry *c,
                                                uint32_t token,
                                                const char *why)
{
    uint32_t index = c ? c->work_submit_notifier_index : 0;
    uint64_t off = (uint64_t)index * NVKVM_M2_NV_NOTIFICATION_SIZE;

    if (!c || !c->err_notifier_base ||
        c->err_notifier_size < off + NVKVM_M2_NV_NOTIFICATION_SIZE) {
        return false;
    }
    bool sys = c->err_notifier_as == NVKVM_M2_ADDR_SYSMEM;
    if (!sys && c->err_notifier_as != NVKVM_M2_ADDR_FBMEM) {
        static uint32_t skip_logs;

        if (skip_logs++ < 32) {
            qemu_log("nvkvm-gpu[%s] M8.52 WORK_NOTIFIER skip %s "
                     "chan=0x%08x idx=%u base=0x%llx size=0x%llx as=%u\n",
                     s->chip->name, why ? why : "completion", c->hobject,
                     index, (unsigned long long)c->err_notifier_base,
                     (unsigned long long)c->err_notifier_size,
                     c->err_notifier_as);
        }
        return false;
    }

    uint64_t addr = c->err_notifier_base + off;
    uint64_t t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    nvkvm_phys_wr32(s, addr + 0, sys, (uint32_t)(t >> 32));
    nvkvm_phys_wr32(s, addr + 4, sys, (uint32_t)t);
    nvkvm_phys_wr32(s, addr + 8, sys, token);
    nvkvm_phys_wr32(s, addr + 12, sys, 0xffff0000u);

    static uint32_t notify_logs;
    if (notify_logs++ < 128) {
        qemu_log("nvkvm-gpu[%s] M8.52 WORK_NOTIFIER %s chan=0x%08x "
                 "idx=%u %s=0x%llx token=0x%08x size=0x%llx\n",
                 s->chip->name, why ? why : "completion", c->hobject,
                 index, sys ? "gpa" : "fb", (unsigned long long)addr, token,
                 (unsigned long long)c->err_notifier_size);
    }
    return true;
}

/* M0: identity registers answered; everything else reads 0.  M1/M2 extend this
 * switch into the fake-the-boot state machine (GFW_BOOT, HWCFG2, RISCV_STATUS,
 * FWSEC/Booter mailboxes). */
static uint64_t nvkvm_reg_read(NvkvmGpuEmul *s, hwaddr off, unsigned size)
{
    /* M6: BAR0 PRAMIN window -> sparse FB backing. */
    if (off >= NVKVM_PRAMIN_BASE && off < NVKVM_PRAMIN_BASE + NVKVM_PRAMIN_SIZE) {
        uint64_t fa = nvkvm_pramin_fb_addr(s, off);
        uint64_t rv = nvkvm_fb_read(s, fa, size);
        if (s->m2_crashwin) {
            static uint32_t pcnt;
            if (pcnt++ < 400) {
                qemu_log("nvkvm-gpu[GA106] M8.8 PRAMIN RD off=0x%llx -> FB 0x%llx "
                         "= 0x%llx sz=%u\n", (unsigned long long)off,
                         (unsigned long long)fa, (unsigned long long)rv, size);
            }
        }
        return rv;
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

    /* M5/M7 — GSP falcon interrupt registers for os-event delivery.
     * kgspService_TU102 calls kflcnGetPendingHostInterrupts = IRQSTAT & IRQMASK
     * & IRQDEST (legacy) or IRQSTAT & RISCV_IRQMASK & RISCV_IRQDEST (riscv).  We
     * advertise SWGEN0 (bit6) enabled in all mask/dest regs and latch it in
     * IRQSTAT when an event is pending; cleared via IRQSCLR write.  Offsets:
     * NV_PGSP base 0x110000; FALCON IRQSTAT +0x08, IRQMASK +0x18, IRQDEST +0x1c;
     * RISCV base 0x111000; RISCV_IRQMASK +0x528, RISCV_IRQDEST +0x52c. */
    case 0x00110008u: return s->gsp_swgen0_pending ? NVKVM_M2_GSP_SWGEN0_BIT : 0; /* FALCON IRQSTAT */
    case 0x00110018u: return NVKVM_M2_GSP_SWGEN0_BIT;                             /* FALCON IRQMASK */
    case 0x0011001cu: return NVKVM_M2_GSP_SWGEN0_BIT;                             /* FALCON IRQDEST */
    case 0x00111528u: return NVKVM_M2_GSP_SWGEN0_BIT;                             /* RISCV  IRQMASK */
    case 0x0011152cu: return NVKVM_M2_GSP_SWGEN0_BIT;                             /* RISCV  IRQDEST */

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
        nvkvm_dmaw(pdev, gpa, el + (uint64_t)i * msgsize, msgsize);
    }

    s->stat_writeptr = (s->stat_writeptr + nelems) % s->q_msgcount; /* modulo ring */
    s->stat_seqnum++;                    /* per-message seqNum is ABSOLUTE (no wrap) */
    uint8_t wp[4];
    stl_le_p(wp, s->stat_writeptr);
    nvkvm_dmaw(pdev, s->q_shmem + s->q_stat_base + 16, wp, sizeof(wp));
}

/* M3 keystone: post GSP_INIT_DONE (seqNum 0). */
static void nvkvm_m3_post_init_done(NvkvmGpuEmul *s)
{
    nvkvm_m3_post_status(s, NULL, 0x1001u /* GSP_INIT_DONE */, 0 /* NV_OK */);
    qemu_log("nvkvm-gpu[%s] M3: posted GSP_INIT_DONE (seqNum 0) -> "
             "RmInitAdapter should pass kgspWaitForRmInitDone\n", s->chip->name);
}

static bool nvkvm_m2_post_event_packed_data(void);

/* M5/M7 — post a GSP NV_VGPU_MSG_EVENT_POST_EVENT (0x1003).  The body is
 * rpc_post_event_v17_00 {NvHandle hClient@0; NvHandle hEvent@4; NvU32
 * notifyIndex@8; NvU32 data@12; NvU16 info16@16; NvU32 status@20; NvU32
 * eventDataSize@24; NvBool bNotifyList@28; NvU8 eventData[]@29} placed at the
 * rpc params offset.  The GSP message element is {48-byte element header,
 * 32-byte rpc_message_header, params...}, so params (rpc_message_data) live at
 * el+48+32 = el+80 — the SAME base the working GSP_RM_CONTROL reply uses
 * (rpc.length = 32 + 40-byte gsp_rm_control header + psize -> control params at
 * el+120).  bNotifyList=0 => _kgspRpcPostEvent does CliGetEventInfo(hClient,
 * hEvent) then osNotifyEvent on the matching event.  The notify-list path
 * accepts the event handle but only drives notifier-list processing; it does
 * not reach nv_post_event for libcuda's poll waiter.  Keep the direct event
 * path and pass the low notifier index from NV0005, not its high-tagged raw
 * value. */
static void nvkvm_m3_post_event(NvkvmGpuEmul *s, uint32_t hclient,
                                uint32_t hevent, uint32_t notify_index,
                                uint32_t data, uint16_t info16,
                                uint32_t status, bool notify_list,
                                bool data_valid)
{
    static uint8_t el[256];               /* device emu is single-threaded */
    uint32_t event_data_size = data_valid ? sizeof(uint32_t) : 0u;
    bool packed_data = nvkvm_m2_post_event_packed_data();

    memset(el, 0, sizeof(el));
    stl_le_p(el + 48, 0x03000000u);       /* rpc header_version MAJOR=3 */
    stl_le_p(el + 52, 0x43505256u);       /* NV_VGPU_MSG_SIGNATURE_VALID */
    stl_le_p(el + 80 +  0, hclient);      /* hClient */
    stl_le_p(el + 80 +  4, hevent);       /* hEvent  */
    stl_le_p(el + 80 +  8, notify_index); /* notifyIndex */
    stl_le_p(el + 80 + 12, data);         /* data */
    stw_le_p(el + 80 + 16, info16);       /* info16 */
    stl_le_p(el + 80 + 20, status);       /* status */
    stl_le_p(el + 80 + 24, event_data_size); /* eventDataSize */
    if (packed_data) {
        stb_p(el + 80 + 28, notify_list ? 1u : 0u); /* packed NvBool */
        if (event_data_size >= sizeof(uint32_t)) {
            stl_le_p(el + 80 + 29, data); /* eventData[0..3] */
        }
        stl_le_p(el + 56, 32u + 29u + event_data_size);
    } else {
        stl_le_p(el + 80 + 28, notify_list ? 1u : 0u); /* bNotifyList */
        if (event_data_size >= sizeof(uint32_t)) {
            stl_le_p(el + 80 + 32, data); /* eventData[0..3] */
        }
        stl_le_p(el + 56, 32u + 32u + event_data_size);
    }
    nvkvm_m3_post_status(s, el, 0x1003u /* NV_VGPU_MSG_EVENT_POST_EVENT */, 0);
}

static void nvkvm_m2_deassert_legacy_irq_if_idle(NvkvmGpuEmul *s)
{
    if (s->intr_top == 0 && !msix_enabled(&s->parent_obj)) {
        pci_set_irq(&s->parent_obj, 0);
    }
}

static void nvkvm_m2_clear_intr_vector(NvkvmGpuEmul *s, uint32_t vec,
                                       const char *why)
{
    uint32_t leaf = vec / 32u;
    uint32_t bit = vec % 32u;
    uint32_t subtree = leaf / 2u;

    if (leaf >= NVKVM_VF_INTR_NLEAF) {
        return;
    }

    bool had_leaf = (s->intr_leaf[leaf] & (1u << bit)) != 0;
    s->intr_leaf[leaf] &= ~(1u << bit);
    if (s->intr_leaf[subtree * 2u] == 0 &&
        (subtree * 2u + 1u >= NVKVM_VF_INTR_NLEAF ||
         s->intr_leaf[subtree * 2u + 1u] == 0)) {
        s->intr_top &= ~(1u << subtree);
    }
    nvkvm_m2_deassert_legacy_irq_if_idle(s);

    if (s->trace && had_leaf) {
        static uint32_t clear_logs;
        if (clear_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.104 INTR_VECTOR_CLEAR vec=%u via %s "
                     "leaf=%u bit=%u intr_top=0x%08x\n",
                     s->chip->name, vec, why ? why : "clear", leaf, bit,
                     s->intr_top);
        }
    }
}

/* M5/M7 — latch the GSP falcon SWGEN0 interrupt and raise the GSP engine's
 * stall vector (155 = 0x9b for MC_ENGINE_IDX_GSP=50 on GA106, from the captured
 * INTERNAL_INTR_GET_KERNEL_TABLE).  Mirrors the INTR_LEAF_TRIGGER path: set the
 * leaf+top pending bits and notify MSI-X so the guest ISR -> kgspServiceInterrupt
 * -> kgspService reads SWGEN0 and drains the GSP message queue. */
static void nvkvm_gsp_raise_swgen0(NvkvmGpuEmul *s)
{
    s->gsp_swgen0_pending = true;
    uint32_t vec = NVKVM_M2_GSP_SWGEN0_VECTOR;
    uint32_t leaf = vec / 32u, bit = vec % 32u, subtree = leaf / 2u;
    if (leaf < NVKVM_VF_INTR_NLEAF) {
        s->intr_leaf[leaf] |= (1u << bit);
        s->intr_top        |= (1u << subtree);
        PCIDevice *pd = &s->parent_obj;
        if (msix_enabled(pd)) {
            msix_notify(pd, 0);
        } else {
            pci_set_irq(pd, 1);
        }
    }
}

static uint32_t nvkvm_m2_event34_mask(void)
{
    static bool init;
    static uint32_t mask = (1u << NVKVM_M2_WORK_SUBMIT_NOTIFY_INDEX) | (1u << 2);

    if (!init) {
        const char *env = getenv("NVKVM_M2_EVENT34_MASK");
        if (env && *env) {
            char *end = NULL;
            unsigned long val = strtoul(env, &end, 0);

            if (end != env) {
                mask = (uint32_t)val;
            }
        }
        init = true;
    }
    return mask;
}

static bool nvkvm_m2_service_interrupts_zero(void)
{
    static bool init;
    static bool zero;

    if (!init) {
        const char *env = getenv("NVKVM_M2_SERVICE_INTERRUPTS_ZERO");
        zero = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return zero;
}

static bool nvkvm_m2_broad_notify_block(void)
{
    static bool init;
    static bool enabled;

    if (!init) {
        const char *env = getenv("NVKVM_M2_BROAD_NOTIFY_BLOCK");
        enabled = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return enabled;
}

static uint32_t nvkvm_m2_event3c_mask(void)
{
    static bool init;
    static uint32_t mask = 0xffffffffu;

    if (!init) {
        const char *env = getenv("NVKVM_M2_EVENT3C_MASK");
        if (env && *env) {
            char *end = NULL;
            unsigned long val = strtoul(env, &end, 0);

            if (end != env) {
                mask = (uint32_t)val;
            }
        }
        init = true;
    }
    return mask;
}

static bool nvkvm_m2_event3c_match_token(void)
{
    static bool init;
    static bool enabled;

    if (!init) {
        const char *env = getenv("NVKVM_M2_EVENT3C_MATCH_TOKEN");
        enabled = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return enabled;
}

static bool nvkvm_m2_post_event_packed_data(void)
{
    static bool init;
    static bool enabled;

    if (!init) {
        const char *env = getenv("NVKVM_M2_POST_EVENT_PACKED_DATA");
        enabled = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return enabled;
}

static bool nvkvm_m2_event3c_host_notify_list(void)
{
    static bool init;
    static bool enabled;

    if (!init) {
        const char *env = getenv("NVKVM_M2_EVENT3C_HOST_NOTIFY_LIST");
        enabled = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return enabled;
}

static bool nvkvm_m2_event3c_raw_notify_index(void)
{
    static bool init;
    static bool enabled;

    if (!init) {
        const char *env = getenv("NVKVM_M2_EVENT3C_RAW_NOTIFY_INDEX");
        enabled = env && *env && strcmp(env, "0") != 0;
        init = true;
    }
    return enabled;
}

typedef enum NvkvmM2EventMode {
    NVKVM_M2_EVENT_HOST_GR,
    NVKVM_M2_EVENT_LOCAL_UVM,
} NvkvmM2EventMode;

/* M5/M7 — deliver completion to every registered os-event, then raise SWGEN0
 * once (the guest drains all queued POST_EVENTs in one service pass). */
static int nvkvm_gsp_deliver_events(NvkvmGpuEmul *s, uint32_t work_token,
                                    bool work_token_valid,
                                    NvkvmM2EventMode mode)
{
    if (s->osevent_n <= 0) {
        return 0;
    }
    /* CRITICAL: the status queue is SHARED with RPC responses and has strictly
     * monotonic per-message seqNums in a small ring.  Posting an event batch on
     * every doorbell (hundreds of times) overflows the ring before the guest
     * drains it -> the guest's rpcRecvPoll sees a seqNum gap ("Bad sequence
     * number") and the whole RPC path breaks.  Gate on the previous batch being
     * drained: only post when SWGEN0 was already cleared by the guest's
     * kgspService (IRQSCLR write), bounding outstanding messages to one batch. */
    if (s->gsp_swgen0_pending) {
        static uint32_t gate_logs;
        nvkvm_gsp_raise_swgen0(s);
        if (s->trace && gate_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.33 EVENT_REASSERT osevents=%d "
                     "stat_wp=%u stat_seq=%u: SWGEN0 still pending\n",
                     s->chip->name, s->osevent_n, s->stat_writeptr,
                     s->stat_seqnum);
        }
        return 0;
    }
    int posted = 0;
    uint32_t event34_mask = nvkvm_m2_event34_mask();
    uint32_t event3c_mask = nvkvm_m2_event3c_mask();
    bool event3c_match_token = nvkvm_m2_event3c_match_token();
    bool event3c_host_notify_list = nvkvm_m2_event3c_host_notify_list();
    bool event3c_raw_notify_index = nvkvm_m2_event3c_raw_notify_index();
    uint32_t local_idx = s->m2_local_completion_token_valid ?
                         (s->m2_local_completion_token & 0xfffu) : 0xffffffffu;
    bool host_has_event34 = false;

    if (mode == NVKVM_M2_EVENT_HOST_GR) {
        for (int i = 0; i < s->osevent_n; i++) {
            uint32_t raw_notify = s->osevents[i].notify_index;
            uint32_t notify_index = raw_notify & 0xffffu;

            if ((raw_notify & 0xff000000u) == 0x34000000u &&
                notify_index < 32u &&
                (event34_mask & (1u << notify_index))) {
                host_has_event34 = true;
                break;
            }
        }
    }
    for (int i = 0; i < s->osevent_n; i++) {
        uint32_t hobj = s->osevents[i].hevent;
        uint32_t raw_notify = s->osevents[i].notify_index;
        uint32_t notify_index = s->osevents[i].notify_index & 0xffffu;
        uint32_t data = 0;
        uint32_t status = 0;
        /*
         * Host-GR completion is not a channel fault. CUDA's channel-parent OS
         * events carry high-tagged raw notify values such as 0x34xxxxxx and
         * 0x3cxxxxxx. Prefer the 0x34 work-submit token/wake pair for host-GR
         * completions when present. Some Mode-2 UVM/bootstrap paths register
         * only 0x3c OS events; in that case a host-GR completion must still
         * wake those fds after the work notifier memory has been updated.
         */
        bool notify_list = false;
        if (raw_notify & 0xff000000u) {
            uint32_t tag = raw_notify & 0xff000000u;
            bool event34_allowed = mode == NVKVM_M2_EVENT_HOST_GR &&
                tag == 0x34000000u && notify_index < 32u &&
                (event34_mask & (1u << notify_index));
            /*
             * 0x3c events are channel-local CUDA events.  Host-GR completions
             * use them as a fallback when no 0x34 work-submit event is present,
             * carrying the real work token/status.  Local CE/UVM completions
             * also need a wake on direct cuCtxCreate paths, but only when QEMU
             * has a real local completion token.  No-token local batches can
             * turn into an event storm and overrun the shared GSP status ring.
             */
            bool event3c_local_allowed =
                mode == NVKVM_M2_EVENT_LOCAL_UVM &&
                s->m2_local_completion_token_valid &&
                (!event3c_match_token || notify_index == local_idx);
            bool event3c_allowed = tag == 0x3c000000u && notify_index < 32u &&
                (event3c_mask & (1u << notify_index)) &&
                ((mode == NVKVM_M2_EVENT_HOST_GR && !host_has_event34) ||
                 event3c_local_allowed);
            if (event34_allowed || event3c_allowed) {
                notify_list = event3c_allowed &&
                              mode == NVKVM_M2_EVENT_HOST_GR &&
                              event3c_host_notify_list;
            } else {
                if (s->trace) {
                    qemu_log("nvkvm-gpu[%s] M8.40 POST_EVENT skip channel-local "
                             "hClient=0x%08x hObj=0x%08x hParent=0x%08x "
                             "rawNotify=0x%08x notifyIndex=%u mode=%s "
                             "event34Mask=0x%08x event3cMask=0x%08x "
                             "event3cMatch=%d localTok=%s0x%08x\n",
                             s->chip->name, s->osevents[i].hclient, hobj,
                             s->osevents[i].hparent, raw_notify, notify_index,
                             mode == NVKVM_M2_EVENT_LOCAL_UVM ? "local" : "host",
                             event34_mask, event3c_mask, event3c_match_token,
                             s->m2_local_completion_token_valid ? "" : "!",
                             s->m2_local_completion_token);
                }
                continue;
            }
        }
        if ((raw_notify & 0xff000000u) == 0x3c000000u &&
            mode == NVKVM_M2_EVENT_LOCAL_UVM &&
            s->m2_local_completion_token_valid) {
            data = s->m2_local_completion_token;
            status = 0xffffu;
        } else if ((raw_notify & 0xff000000u) == 0x3c000000u &&
            mode == NVKVM_M2_EVENT_HOST_GR && work_token_valid) {
            /*
             * Some CUDA bring-up paths register only the 0x3c channel-local
             * events.  They still represent host-GR work completion here, so
             * carry the same token/status payload as the 0x34 work-submit
             * event instead of posting an empty wake.
             */
            data = work_token;
            status = 0xffffu;
        } else if ((raw_notify & 0xff000000u) == 0x34000000u &&
            notify_index == 1u && work_token_valid) {
            data = work_token;
            status = 0xffffu;
        } else if ((raw_notify & 0xff000000u) == 0x34000000u &&
                   notify_index == 1u && !work_token_valid) {
            /*
             * Index 1 is CUDA's work-submit notifier.  Posting it without a
             * valid token overwrites the same event stream used by real GR
             * completions with data=0/status=0.  Local CE/UVM completions are
             * semaphore-driven; keep their wake on index 2 and leave index 1 to
             * host-GR completions that can provide the real work-submit token.
             */
            if (s->trace) {
                static uint32_t local_idx1_skip_logs;
                if (local_idx1_skip_logs++ < 128) {
                    qemu_log("nvkvm-gpu[%s] M8.54 POST_EVENT skip local "
                             "work-submit idx1 hClient=0x%08x hObj=0x%08x "
                             "rawNotify=0x%08x: no work token\n",
                             s->chip->name, s->osevents[i].hclient, hobj,
                             raw_notify);
                }
            }
            continue;
        }
        uint32_t post_notify_index =
            event3c_raw_notify_index &&
            (raw_notify & 0xff000000u) == 0x3c000000u ?
            raw_notify : notify_index;
        nvkvm_m3_post_event(s, s->osevents[i].hclient, hobj,
                            post_notify_index, data, 0, status, notify_list,
                            !notify_list);
        posted++;
        if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M8.38 POST_EVENT %s hClient=0x%08x "
                     "hObj=0x%08x hParent=0x%08x hSrc=0x%08x "
                     "hEvent=0x%08x hClass=0x%08x rawNotify=0x%08x "
                     "notifyIndex=%u data=0x%08x status=0x%08x dataValid=%u "
                     "nv0005Data=0x%llx\n",
                     s->chip->name, notify_list ? "notify-list" : "direct",
                     s->osevents[i].hclient, hobj, s->osevents[i].hparent,
                     s->osevents[i].hsrc, s->osevents[i].hevent,
                     s->osevents[i].event_class, raw_notify, post_notify_index,
                     data, status, !notify_list,
                     (unsigned long long)s->osevents[i].data);
        }
    }
    if (posted <= 0) {
        if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M8.40 POST_EVENT batch skipped all "
                     "os-events (%d registered)\n",
                     s->chip->name, s->osevent_n);
        }
        return 0;
    }
    nvkvm_gsp_raise_swgen0(s);
    if (s->trace) {
        qemu_log("nvkvm-gpu[%s] M7: delivered %d/%d os-event(s) + raised GSP "
                 "SWGEN0 (vec 155)\n", s->chip->name, posted, s->osevent_n);
    }
    return posted;
}

static bool nvkvm_m2_host_completion_queued(NvkvmGpuEmul *s)
{
    return s->m2_host_completion_head != s->m2_host_completion_tail;
}

static int nvkvm_m2_host_completion_count(NvkvmGpuEmul *s)
{
    return (s->m2_host_completion_tail - s->m2_host_completion_head +
            NVKVM_M2_HOST_COMPLETION_Q) % NVKVM_M2_HOST_COMPLETION_Q;
}

static void nvkvm_m2_queue_host_completion(NvkvmGpuEmul *s, uint32_t token,
                                           bool token_valid, const char *why)
{
    uint16_t tail = s->m2_host_completion_tail;
    uint16_t next = (tail + 1u) % NVKVM_M2_HOST_COMPLETION_Q;

    for (uint16_t i = s->m2_host_completion_head;
         i != s->m2_host_completion_tail;
         i = (i + 1u) % NVKVM_M2_HOST_COMPLETION_Q) {
        if (s->m2_host_completion_q[i].token_valid == token_valid &&
            s->m2_host_completion_q[i].token == token) {
            static uint32_t dedupe_logs;
            if (s->trace && dedupe_logs++ < 128) {
                qemu_log("nvkvm-gpu[%s] M8.55 HOST_EVENT_QUEUE dedupe via %s "
                         "queued=%d token=%s0x%08x\n",
                         s->chip->name, why ? why : "?",
                         nvkvm_m2_host_completion_count(s),
                         token_valid ? "" : "!", token);
            }
            return;
        }
    }

    if (next == s->m2_host_completion_head) {
        s->m2_host_completion_head =
            (s->m2_host_completion_head + 1u) % NVKVM_M2_HOST_COMPLETION_Q;
        s->m2_host_completion_dropped++;
        if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M8.55 HOST_EVENT_QUEUE overflow via %s: "
                     "drop-oldest dropped=%u new_token=%s0x%08x\n",
                     s->chip->name, why ? why : "?",
                     s->m2_host_completion_dropped,
                     token_valid ? "" : "!", token);
        }
    }

    s->m2_host_completion_q[tail].token = token;
    s->m2_host_completion_q[tail].token_valid = token_valid;
    s->m2_host_completion_tail = next;
    if (s->trace) {
        static uint32_t queue_logs;
        if (queue_logs++ < 256) {
            qemu_log("nvkvm-gpu[%s] M8.55 HOST_EVENT_QUEUE via %s "
                     "queued=%d token=%s0x%08x\n",
                     s->chip->name, why ? why : "?",
                     nvkvm_m2_host_completion_count(s),
                     token_valid ? "" : "!", token);
        }
    }
}

static void nvkvm_m2_try_deliver_host_completion(NvkvmGpuEmul *s,
                                                  const char *why)
{
    if (!nvkvm_m2_host_completion_queued(s) || s->osevent_n <= 0) {
        return;
    }
    if (s->gsp_swgen0_pending) {
        static uint32_t gated_logs;
        uint16_t head = s->m2_host_completion_head;
        uint32_t token = s->m2_host_completion_q[head].token;
        bool token_valid = s->m2_host_completion_q[head].token_valid;
        if (s->trace && gated_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.55 HOST_EVENT pending via %s: "
                     "SWGEN0 still pending queued=%d front_token=%s0x%08x\n",
                     s->chip->name, why ? why : "?",
                     nvkvm_m2_host_completion_count(s),
                     token_valid ? "" : "!", token);
        }
        nvkvm_gsp_raise_swgen0(s);
        return;
    }

    while (nvkvm_m2_host_completion_queued(s) && !s->gsp_swgen0_pending) {
        uint16_t head = s->m2_host_completion_head;
        uint32_t token = s->m2_host_completion_q[head].token;
        bool token_valid = s->m2_host_completion_q[head].token_valid;

        if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M8.55 HOST_EVENT_TRY via %s "
                     "osevents=%d queued=%d token=%s0x%08x\n",
                     s->chip->name, why ? why : "?", s->osevent_n,
                     nvkvm_m2_host_completion_count(s),
                     token_valid ? "" : "!", token);
        }
        if (nvkvm_gsp_deliver_events(s, token, token_valid,
                                     NVKVM_M2_EVENT_HOST_GR) <= 0) {
            return;
        }
        s->m2_host_completion_head =
            (s->m2_host_completion_head + 1u) % NVKVM_M2_HOST_COMPLETION_Q;
    }
}

static void nvkvm_m2_try_deliver_local_completion(NvkvmGpuEmul *s,
                                                  const char *why)
{
    if (!s->m2_local_completion_pending || s->osevent_n <= 0) {
        return;
    }
    if (s->m2_local_completion_token_valid &&
        s->m2_local_completion_posted_valid &&
        s->m2_local_completion_token == s->m2_local_completion_posted_token &&
        s->osevent_n <= s->m2_local_completion_posted_osevent_n) {
        static uint32_t dup_logs;
        if (s->trace && dup_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.107 LOCAL_EVENT skip duplicate "
                     "via %s token=0x%08x osevents=%d posted_osevents=%d\n",
                     s->chip->name, why ? why : "?",
                     s->m2_local_completion_token, s->osevent_n,
                     s->m2_local_completion_posted_osevent_n);
        }
        s->m2_local_completion_pending = false;
        s->m2_local_completion_token_valid = false;
        return;
    }
    if (nvkvm_m2_host_completion_queued(s)) {
        static uint32_t host_first_logs;
        if (s->trace && host_first_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.49 LOCAL_EVENT deferred via %s: "
                     "host completion pending\n",
                     s->chip->name, why ? why : "?");
        }
        nvkvm_m2_try_deliver_host_completion(s, why);
        return;
    }
    if (s->gsp_swgen0_pending) {
        static uint32_t gated_logs;
        if (s->trace && gated_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.49 LOCAL_EVENT pending via %s: "
                     "SWGEN0 still pending\n", s->chip->name,
                     why ? why : "?");
        }
        nvkvm_gsp_raise_swgen0(s);
        return;
    }
    if (s->trace) {
        qemu_log("nvkvm-gpu[%s] M8.49 LOCAL_EVENT_TRY via %s "
                 "osevents=%d\n", s->chip->name, why ? why : "?",
                 s->osevent_n);
    }
    bool token_valid = s->m2_local_completion_token_valid;
    uint32_t token = s->m2_local_completion_token;
    if (nvkvm_gsp_deliver_events(s, 0, false,
                                 NVKVM_M2_EVENT_LOCAL_UVM) > 0) {
        if (token_valid) {
            s->m2_local_completion_posted_token = token;
            s->m2_local_completion_posted_valid = true;
            s->m2_local_completion_posted_osevent_n = s->osevent_n;
        }
        s->m2_local_completion_pending = false;
        s->m2_local_completion_token_valid = false;
    }
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
    if (!s->trace) {
        return;                 /* verbose bring-up decode — gated behind -trace */
    }
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
    } else if (fn == 21) {                            /* DUP_OBJECT */
        uint32_t hClient = ldl_le_p(cmd + 80), hParent = ldl_le_p(cmd + 84);
        uint32_t hObject = ldl_le_p(cmd + 88), hClientSrc = ldl_le_p(cmd + 92);
        uint32_t hSrcObject = ldl_le_p(cmd + 96), flags = ldl_le_p(cmd + 100);
        uint32_t autoFree = ldl_le_p(cmd + 104);
        qemu_log("nvkvm-gpu[%s] DIAG DUP hClient=0x%08x hParent=0x%08x "
                 "hObject=0x%08x hClientSrc=0x%08x hSrcObject=0x%08x "
                 "flags=0x%x autoFree=%u\n", s->chip->name, hClient, hParent,
                 hObject, hClientSrc, hSrcObject, flags, autoFree);
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
static void nvkvm_m2_shadow_fwd(NvkvmGpuEmul *s, const uint8_t *cmd, uint32_t fn); /* M5.1 fwd-decl */
static int nvkvm_m2_control1(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hObject,
                             uint32_t cmd, void *params, uint32_t psize, uint32_t *st); /* M5.3 fwd-decl */
static void nvkvm_m2_back_channel_userd(NvkvmGpuEmul *s, uint32_t hClient,
                                        uint32_t chanObj, uint8_t *auxbuf,
                                        uint32_t psize); /* M5.4 fwd-decl */
static void nvkvm_m2_mapdma_selftest(NvkvmGpuEmul *s, uint32_t hClient); /* M5.5 fwd-decl */
static uint64_t nvkvm_m2_gpa_to_stub_va(NvkvmGpuEmul *s, uint64_t gpa); /* M6.2 fwd-decl */
static int nvkvm_m2_os_descriptor(NvkvmGpuEmul *s, uint32_t client, uint32_t device,
                                  uint32_t hMem, uint64_t stub_va, uint64_t size,
                                  uint32_t *st); /* M6.2 fwd-decl */
static void nvkvm_m2_osdesc_selftest(NvkvmGpuEmul *s, uint32_t hClient); /* M6.2 fwd-decl */
static uint32_t nvkvm_m2_grmapper(NvkvmGpuEmul *s, uint32_t client); /* M5.7 fwd-decl */
static int nvkvm_m2_cvas_get(NvkvmGpuEmul *s, uint32_t client, uint32_t tsg); /* M5.28 fwd-decl */
static void nvkvm_m2_populate_cvas(NvkvmGpuEmul *s, struct nvkvm_chan_entry *c); /* M5.28 fwd-decl */
static int nvkvm_m2_map_dma(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hDevice,
                            uint32_t hVas, uint32_t hMemory, uint64_t offset,
                            uint64_t length, bool fixed, uint64_t va,
                            uint32_t *st, uint64_t *out_va); /* M5.5 fwd-decl */
static bool nvkvm_m2_back_and_map(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                  uint64_t phys, uint64_t size, bool copy_content,
                                  const char *label); /* M5.7 */
static void nvkvm_m2_doorbell_setup(NvkvmGpuEmul *s, uint32_t client); /* M5.8 fwd-decl */
static void nvkvm_m2_exec_doorbell(NvkvmGpuEmul *s); /* M5.9 fwd-decl */
static void nvkvm_m2_ring_host_channel(NvkvmGpuEmul *s, int ch_index,
                                       struct nvkvm_chan_entry *c,
                                       uint32_t token, const char *tag,
                                       uint32_t ring_put_override,
                                       bool ring_put_override_valid);
static void nvkvm_m2_forward_promote_ctx(NvkvmGpuEmul *s, const uint8_t *cmd); /* M6.4 fwd-decl */
static bool nvkvm_m2_back_and_map_sys(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                      uint64_t gpa, uint64_t size); /* M6.5 fwd-decl */
static void nvkvm_m2_enum_gr_sysmem(NvkvmGpuEmul *s, uint32_t client); /* M6.5 fwd-decl */
static void nvkvm_m2_capture_devinfo(NvkvmGpuEmul *s); /* M14 fwd-decl */
static bool nvkvm_chan_sem_wr32(NvkvmGpuEmul *s, uint64_t va, uint32_t payload,
                                uint64_t *out_redir); /* M5.18 fwd-decl */
static int nvkvm_m2_write_channel_notify_block(NvkvmGpuEmul *s, uint32_t payload,
                                               const char *why,
                                               uint64_t *out_last_redir); /* M8.44 */
static bool nvkvm_m2_va_seen(NvkvmGpuEmul *s, uint32_t client, uint64_t va); /* M5.19 fwd-decl */
static bool nvkvm_m2_va_is_seen(NvkvmGpuEmul *s, uint32_t client, uint64_t va); /* M8.96 */
static void nvkvm_m2_va_forget(NvkvmGpuEmul *s, uint32_t client, uint64_t va); /* M8.11 */
static bool nvkvm_m2_pbmap_lookup(NvkvmGpuEmul *s, uint64_t va, uint64_t size,
                                  uint64_t *out_gpa); /* M8.11 */
static bool nvkvm_m2_bar1_gpa_to_off(NvkvmGpuEmul *s, uint64_t gpa,
                                      uint64_t size, uint64_t *out_off); /* M8.48 */
static bool nvkvm_m2_uvm_shadow_lookup(NvkvmGpuEmul *s, uint64_t va,
                                        uint64_t size, uint64_t *out_gpa,
                                        bool *out_bar1); /* M8.14 */
static bool nvkvm_m2_uvm_shadow_resolve(NvkvmGpuEmul *s, uint64_t va,
                                         uint64_t size, uint64_t *out_phys,
                                         bool *out_sys); /* M8.48 */
static bool nvkvm_m2_uvm_shadow_read(NvkvmGpuEmul *s, uint64_t va,
                                      void *dst, uint64_t size); /* M8.48 */
static bool nvkvm_m2_uvm_shadow_rd32(NvkvmGpuEmul *s, uint64_t va,
                                      uint32_t *out); /* M8.48 */
static bool nvkvm_m2_uvm_shadow_wr32(NvkvmGpuEmul *s, uint64_t va,
                                      uint32_t val); /* M8.48 */
static bool nvkvm_m2_uvm_ext_lookup(NvkvmGpuEmul *s, uint64_t va, uint64_t size,
                                     uint64_t *out_phys); /* M8.15 */
static int nvkvm_m2_uvm_ext_find(NvkvmGpuEmul *s, uint64_t va,
                                  uint64_t size); /* M8.15 */
static bool nvkvm_m2_uvm_ext_trace_range(uint64_t va, uint64_t size); /* M8.23 */
static int nvkvm_m2_uvm_ext_record(NvkvmGpuEmul *s, uint64_t va,
                                   uint64_t size); /* M8.15 */
static bool nvkvm_m2_uvm_ext_ensure_obj(NvkvmGpuEmul *s, uint32_t client,
                                         int idx); /* M8.15 */
static void nvkvm_m2_uvm_ext_map_all(NvkvmGpuEmul *s, uint32_t client,
                                      bool map_gr); /* M8.15 */
static bool nvkvm_m2_uvm_ext_peek32(NvkvmGpuEmul *s, uint64_t va,
                                    uint32_t *out); /* M8.93 */
static void nvkvm_m2_uvm_ext_sync_to_pbmap(NvkvmGpuEmul *s, uint64_t lo,
                                           uint64_t hi,
                                           const char *why); /* M8.21 */
static uint32_t nvkvm_m2_tsg_engine(NvkvmGpuEmul *s, uint32_t tsg); /* M8.12 */
static bool nvkvm_m2_host_tsg_is_scheduled(NvkvmGpuEmul *s, uint32_t client,
                                           uint32_t tsg); /* M8.27 */
static void nvkvm_m2_mark_host_tsg_scheduled(NvkvmGpuEmul *s, uint32_t client,
                                             uint32_t tsg,
                                             bool scheduled); /* M8.27 */
static bool nvkvm_m2_schedule_host_tsg(NvkvmGpuEmul *s, uint32_t client,
                                       uint32_t tsg,
                                       const char *why); /* M8.27 */

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
            /* M6.4 (item-4 step 4, the PROMOTE_CTX experiment): forward PROMOTE_CTX to the
             * host with each sysmem buffer's gpuPhysAddr substituted to OUR backing (the
             * OS_DESCRIPTOR'd guest RAM), so the host GR context maps the guest's GR VAs onto
             * the guest's actual memory -> host GPU DMA-fills what libcuda reads. Proves we
             * own the GR VA layout (the user's "fix any GR VA" question). Gated m2exec. */
            if (s->m2exec) {
                nvkvm_m2_forward_promote_ctx(s, cmd);
            }
        }
        if (fn == 76 && ldl_le_p(cmd + 88) == 0xc36f010au) {
            uint32_t hobj = ldl_le_p(cmd + 84);
            uint32_t psize = ldl_le_p(cmd + 96);
            uint32_t index = psize >= 4 ? ldl_le_p(cmd + 120) : 0;

            for (int i = 0; i < s->chan_n; i++) {
                if (s->chans[i].hobject == hobj) {
                    s->chans[i].work_submit_notifier_index = index;
                    qemu_log("nvkvm-gpu[%s] M8.52 WORK_NOTIFIER_INDEX "
                             "chan=0x%08x idx=%u via ctrl 0xc36f010a\n",
                             s->chip->name, hobj, index);
                    break;
                }
            }
        }
        /* M5.1a: shadow-forward the guest's actual RM alloc stream to the real
         * host GPU (gated; non-disruptive — guest still uses the faked response). */
        if (s->m2fwd) {
            nvkvm_m2_shadow_fwd(s, cmd, fn);
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
            uint32_t alloc_psize = ldl_le_p(cmd + 100);
            /* M5/M7 — record NV01_EVENT_OS_EVENT (0x0079) allocations so we can
             * post a GSP POST_EVENT on channel completion (the blocking-sync
             * wakeup).  GSP_RM_ALLOC body: hClient@80, hObject@88(=hEvent),
             * hClass@92, params@112 = NV0005_ALLOC_PARAMETERS {hParentClient@0,
             * hSrcResource@4, hClass@8, notifyIndex@12, data@16}. */
            if (hclass == 0x0079u &&
                s->osevent_n < (int)ARRAY_SIZE(s->osevents)) {
                uint32_t hcli = ldl_le_p(cmd + 80);
                uint32_t hpar = ldl_le_p(cmd + 84);
                uint32_t hev  = ldl_le_p(cmd + 88);
                uint32_t hsrc = ldl_le_p(cmd + 116);
                uint32_t ecls = ldl_le_p(cmd + 120);
                uint32_t nidx = ldl_le_p(cmd + 124);
                uint64_t edata = ldq_le_p(cmd + 128);
                uint32_t low_idx = nidx & 0xffffu;
                /* de-dup (the same event may be re-seen on replay) */
                bool seen = false;
                for (int i = 0; i < s->osevent_n; i++) {
                    if (s->osevents[i].hclient == hcli &&
                        s->osevents[i].hevent  == hev) { seen = true; break; }
                }
                if (!seen) {
                    s->osevents[s->osevent_n].hclient      = hcli;
                    s->osevents[s->osevent_n].hparent      = hpar;
                    s->osevents[s->osevent_n].hevent       = hev;
                    s->osevents[s->osevent_n].hsrc         = hsrc;
                    s->osevents[s->osevent_n].event_class  = ecls;
                    s->osevents[s->osevent_n].notify_index = nidx;
                    s->osevents[s->osevent_n].data         = edata;
                    s->osevent_n++;
                    if (s->trace) {
                        qemu_log("nvkvm-gpu[%s] M7: recorded os-event hClient=0x%08x "
                                 "hParent=0x%08x hEvent=0x%08x hSrc=0x%08x "
                                 "hClass=0x%08x notifyIndex=0x%08x data=0x%llx (#%d)\n",
                                 s->chip->name, hcli, hpar, hev, hsrc, ecls,
                                 nidx, (unsigned long long)edata, s->osevent_n);
                    }
                    if ((nidx & 0xff000000u) == 0x34000000u &&
                        low_idx == NVKVM_M2_WORK_SUBMIT_NOTIFY_INDEX) {
                        for (int i = 0; i < s->chan_n; i++) {
                            if (s->chans[i].hobject == hpar) {
                                s->chans[i].work_submit_notifier_index = low_idx;
                                qemu_log("nvkvm-gpu[%s] M8.52 WORK_NOTIFIER_INDEX "
                                         "chan=0x%08x idx=%u via event hEvent=0x%08x\n",
                                         s->chip->name, hpar, low_idx, hev);
                                break;
                            }
                        }
                    }
                    /*
                     * Do not synthesize POST_EVENT from the allocation hook.
                     * RM has created the NV01_EVENT_OS_EVENT object, but its
                     * notifier share can still have an empty pEventList here;
                     * posting a notify-list event in this window trips
                     * _kgspRpcPostEvent's pNotifyList assertion. Leave any
                     * pending local completion latched for the next doorbell or
                     * service pass, after CUDA has finished registering the
                     * event list entries.
                     */
                }
            }
            if (nvkvm_m2_is_gpfifo_channel_class(hclass)) {
                const uint8_t *params = cmd + 112;
                uint64_t err_base = 0;
                uint64_t err_size = 0;
                uint32_t err_as = 0;

                if (alloc_psize >= NVKVM_M2_CHAN_ERROR_NOTIFIER_MEM_OFF +
                                   NVKVM_M2_MEMORY_DESC_SIZE) {
                    const uint8_t *err =
                        params + NVKVM_M2_CHAN_ERROR_NOTIFIER_MEM_OFF;
                    err_base = ldq_le_p(err + 0);
                    err_size = ldq_le_p(err + 8);
                    err_as = ldl_le_p(err + 16);
                }
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
                        s->chans[cslot].err_notifier_base = err_base;
                        s->chans[cslot].err_notifier_size = err_size;
                        s->chans[cslot].err_notifier_as = err_as;
                        s->chans[cslot].gpfifo_ent = s->chan_gpfifo_ent;
                        s->chans[cslot].userd_sys  = s->chan_userd_sys;
                        s->chans[cslot].hvaspace   = s->chan_hvaspace;
                        s->chans[cslot].client     = ldl_le_p(cmd + 80); /* hClient */
                        s->chans[cslot].hobject    = ldl_le_p(cmd + 88); /* channel handle */
                        s->chans[cslot].tsg        = ldl_le_p(cmd + 84); /* M5.25: parent TSG */
                        s->chans[cslot].scheduled  = false;
                        s->chans[cslot].gp_get     = 0;
                        s->chans[cslot].payload    = 0;
                        s->chans[cslot].token_valid = false;
                        s->chans[cslot].token_failed = false;
                        s->chans[cslot].work_submit_notifier_index =
                            NVKVM_M2_WORK_SUBMIT_NOTIFY_INDEX;
                    }
                }
                qemu_log("nvkvm-gpu[%s] M5: channel alloc class=0x%04x gpFifoVA="
                         "0x%llx ent=%u instblk=0x%llx(%s) "
                         "errNotifier=0x%llx size=0x%llx as=%u\n",
                         s->chip->name, hclass,
                         (unsigned long long)s->chan_gpfifo_va, s->chan_gpfifo_ent,
                         (unsigned long long)s->chan_inst_block,
                         s->chan_inst_sys ? "sys" : "fb",
                         (unsigned long long)err_base,
                         (unsigned long long)err_size, err_as);
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
            /* M5.3 FIX (cuCtxCreate SIGSEGV): GR-object allocs (compute/3D, e.g.
             * AMPERE_COMPUTE_B 0xc7c0) register NV_GR_ALLOCATION_PARAMETERS (16B)
             * as RS_OPTIONAL.  libcuda passes a non-NULL pAllocParms backed by only
             * an 8-byte stack slot with paramsSize=0; a real GSP returns paramsSize=0
             * (no params writeback) so the host driver copies 0 bytes back.  Our echo
             * reply returned the full 16B params, and the guest's GSP-client deserialize
             * copied 16B into libcuda's 8-byte buffer — clobbering a saved rbp on the
             * stack -> NULL-rbp deref -> SIGSEGV in cuCtxCreate (proven byte-exact,
             * host-vs-guest, 2026-06-04).  Match the real GSP: drop the params from the
             * reply for GR objects so the guest copies nothing back.  GR object classes:
             * low byte 0xC0 (compute) / 0x97 (3D), family >= 0xB0; excludes DMA-copy
             * (0xB5), subdevice (0x2080), channel (0x..6F). */
            if (fn == 103) {
                uint32_t hc = ldl_le_p(resp + 92);
                uint32_t lb = hc & 0xffu, fam = (hc >> 8) & 0xffu;
                uint32_t opsize = ldl_le_p(resp + 100);
                uint32_t robj = ldl_le_p(resp + 88);
                /* M8.2 DIAG (rbp-clobber hunt): the cuCtxCreate SIGSEGV is a ~368B zeros
                 * writeback overflowing libcuda's stack params buffer (368 == c56f channel
                 * alloc psize). Log every alloc's reply paramsSize so we can see if our reply
                 * exceeds libcuda's request (the overflow) — esp. the c56f channel alloc. */
                qemu_log("nvkvm-gpu[%s] M8.2 alloc-reply class=0x%04x obj=0x%08x "
                         "reply_paramsSize=%u\n", s->chip->name, hc, robj, opsize);
                if (nvkvm_m2_is_gpfifo_channel_class(hc) &&
                    opsize >= NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF + 4u &&
                    s->m2_chan_reply_valid &&
                    s->m2_chan_reply_obj == robj) {
                    uint32_t old_flags =
                        ldl_le_p(resp + 112 + NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF);
                    stl_le_p(resp + 112 + NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF,
                             s->m2_chan_reply_internal_flags);
                    s->m2_chan_reply_valid = false;
                    qemu_log("nvkvm-gpu[%s] M8.103 channel alloc reply "
                             "class=0x%04x obj=0x%08x internalFlags "
                             "0x%08x->0x%08x\n",
                             s->chip->name, hc, robj, old_flags,
                             s->m2_chan_reply_internal_flags);
                }
                if (fam >= 0xb0u && (lb == 0xc0u || lb == 0x97u)) {
                    uint32_t cap_psize = (s->m2_gr_reply_valid && s->m2_gr_reply_obj == robj)
                                             ? s->m2_gr_reply_psize : 0u;
                    uint32_t req_psize = opsize;
                    if (req_psize > NVKVM_RESP_MAX - 112u) {
                        req_psize = NVKVM_RESP_MAX - 112u;
                    }
                    /*
                     * GR alloc params are optional. libcuda sets outer paramsSize=0 but
                     * passes a non-NULL pAllocParms stack slot; guest RM still derives the
                     * 16B class size and copy_to_user's that many bytes after a successful
                     * GSP alloc. If the RPC response is shortened to paramsSize=0, the
                     * driver's local rpc_params->params stays zero padded and that copyout
                     * clears libcuda's saved rbp. Keep the original request bytes present
                     * in the response payload so the unavoidable copyout restores the stack,
                     * while reporting semantic paramsSize=0 like native RM.
                     */
                    if (req_psize) {
                        memcpy(resp + 112, cmd + 112, req_psize);
                        stl_le_p(resp + 56, 32u + 32u + req_psize);
                    }
                    uint32_t hp = 0;                   /* no semantic GR alloc params returned */
                    stl_le_p(resp + 100, hp);
                    s->m2_gr_reply_valid = false;
                    qemu_log("nvkvm-gpu[%s] M8.4 GR-obj 0x%04x: reply paramsSize=%u "
                             "len-preserve=%u host_caps_psize %u dropped [rbp-restore]\n",
                             s->chip->name, hc, hp, req_psize, cap_psize);
                }
            }
            uint32_t ctrl = (fn == 76) ? ldl_le_p(resp + 88) : 0;
            if (fn == 76) {
                stl_le_p(resp + 92, 0); /* body.status = NV_OK (default) */
                /* M9: capture the caller's REQUEST paramsSize (the buffer libcuda/CPU-RM
                 * allocated) BEFORE any handler overwrites resp+96, so we can clamp the
                 * reply size and never overrun the caller's buffer (see clamp below). */
                uint32_t req_psize = ldl_le_p(resp + 96);
                const nvkvm_ctrl_resp_t *cr = NULL;
                for (uint32_t i = 0; i < NVKVM_CTRL_RESP_COUNT; i++) {
                    if (nvkvm_ctrl_resps[i].cmd == ctrl) {
                        cr = &nvkvm_ctrl_resps[i];
                        break;
                    }
                }
                if (ctrl == 0x20801702u) {
                    /* M8.5/M8.29: MC_SERVICE_INTERRUPTS is a side-effect control. It used to
                     * be forwarded so host RM/GSP could drain real channel work. Once Mode-2
                     * soft-completes UVM-backed GR packets, forwarding this poll lets host RM
                     * observe the deliberately unsynchronized GR channel and report Xid 43.
                     * Keep the guest-visible interrupt service local in m2exec, but still run
                     * QEMU's local doorbell/pbmap drains below. nvkvm_m2_exec_doorbell()
                     * posts os-events when it actually completes work; do not post an
                     * unconditional event batch from a mere poll, or the guest can wake on
                     * channel/error events with no new completed GPFIFO and enter reset. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[4] = {0};
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    uint32_t engines_in = ps >= 4 ? ldl_le_p(cbuf) : 0;
                    bool guest_only = s->m2exec;
                    if (!guest_only && s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, ldl_le_p(resp + 80),
                                               ldl_le_p(resp + 84), ctrl,
                                               cbuf, ps, &st);
                    } else if (guest_only) {
                        /*
                         * MC_SERVICE_INTERRUPTS is invoked with an engine mask
                         * and returns the serviced mask.  The default remains
                         * the requested mask while the event/notifier bridge is
                         * being isolated; NVKVM_M2_SERVICE_INTERRUPTS_ZERO=1
                         * forces an all-zero serviced mask as a diagnostic for
                         * the cuCtxSynchronize poll path.
                         */
                        if (nvkvm_m2_service_interrupts_zero() && cn) {
                            memset(cbuf, 0, cn);
                        }
                        rc = 0;
                        st = 0;
                    }
                    if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    {
                        static uint32_t service_logs;
                        uint32_t log_idx = service_logs++;

                        if (s->trace && (log_idx < 128 || (log_idx & 0x3ffu) == 0)) {
                            qemu_log("nvkvm-gpu[%s] M8.31 SERVICE_INTERRUPTS "
                                     "engines=0x%08x->0x%08x ps=%u fwd_rc=%d "
                                     "fwd_st=0x%x%s%s count=%u\n",
                                     s->chip->name, engines_in,
                                     ps >= 4 ? ldl_le_p(resp + 120) : 0, ps, rc, st,
                                     guest_only ?
                                     (nvkvm_m2_service_interrupts_zero() ?
                                      "  GUEST-ONLY-ZERO" : "  GUEST-ONLY-MASK") : "",
                                     (!guest_only && rc == 0 && st == 0) ? "  HOST-SERVICED" :
                                     (!guest_only ? "  guest-visible OK fallback" : ""),
                                     log_idx + 1);
                        }
                    }
                    /* M8.11/M8.30: cuCtxCreate now loops through SERVICE_INTERRUPTS after the
                     * first work-submit. If a later diagnostic provides user-mmap
                     * pushbuffer GPA ranges (m2pbmap), retry mapping/ringing pending
                     * host channels here; the original doorbell has already happened.
                     * Completion-triggered os-event delivery stays latched: service
                     * polls without completed work remain event-quiet, but a poll
                     * after SWGEN0 has drained can now flush a pending local CE wake. */
                    nvkvm_m2_exec_doorbell(s);
                    nvkvm_m2_try_deliver_host_completion(s, "service-interrupt");
                    nvkvm_m2_try_deliver_local_completion(s, "service-interrupt");
                    nvkvm_m2_uvm_ext_sync_to_pbmap(s, 0x204000000ull,
                                                   0x205000000ull,
                                                   "service-interrupt");
                } else if (ctrl == 0x906f0102u) {
                    /* M8.18/M8.29: NV906F_CTRL_CMD_RESET_CHANNEL is fallout after a fault.
                     * For m2exec GR channels, forwarding the guest reset into host RM keeps
                     * host channel 0x0c in an Xid 43 loop even when QEMU has soft-completed
                     * the guest packet. Acknowledge the guest reset locally and clear its
                     * output params; leave non-GR reset forwarding intact. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t hClient = ldl_le_p(resp + 80);
                    uint32_t hObject = ldl_le_p(resp + 84);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[16] = {0};
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    uint32_t in0 = ps >= 4 ? ldl_le_p(cbuf + 0) : 0;
                    uint32_t in1 = ps >= 8 ? ldl_le_p(cbuf + 4) : 0;
                    uint32_t in2 = ps >= 12 ? ldl_le_p(cbuf + 8) : 0;
                    uint32_t in3 = ps >= 16 ? ldl_le_p(cbuf + 12) : 0;
                    bool guest_only = false;
                    for (int ci = 0; ci < s->chan_n; ci++) {
                        struct nvkvm_chan_entry *c = &s->chans[ci];
                        if (c->client == hClient && c->hobject == hObject &&
                            nvkvm_m2_tsg_engine(s, c->tsg) == 1u) {
                            guest_only = s->m2exec;
                            break;
                        }
                    }
                    if (guest_only) {
                        rc = 0;
                        st = 0;
                    } else if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, hClient, hObject, ctrl,
                                               cbuf, ps, &st);
                    }
                    if (guest_only && cn) {
                        memset(resp + 120, 0, cn);
                    } else if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    } else if (cn) {
                        memset(resp + 120, 0, cn);
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.29 RESET_CHANNEL ctrl hClient=0x%08x "
                             "hObject=0x%08x ps=%u in=%08x/%08x/%08x/%08x "
                             "fwd_rc=%d fwd_st=0x%x out=%08x/%08x/%08x/%08x%s%s\n",
                             s->chip->name, hClient, hObject, ps, in0, in1, in2, in3,
                             rc, st,
                             ps >= 4 ? ldl_le_p(resp + 120) : 0,
                             ps >= 8 ? ldl_le_p(resp + 124) : 0,
                             ps >= 12 ? ldl_le_p(resp + 128) : 0,
                             ps >= 16 ? ldl_le_p(resp + 132) : 0,
                             guest_only ? "  GUEST-ONLY-ZEROED" : "",
                             (!guest_only && rc == 0 && st == 0) ? "  HOST-SERVICED" :
                             (!guest_only ? "  guest-visible OK fallback" : ""));
                } else if (ctrl == 0xa06c0101u) {
                    /* M8.7/M8.27: NVA06C_CTRL_CMD_GPFIFO_SCHEDULE is the guest RM's
                     * authoritative moment to put a TSG on the runlist. For GR TSGs,
                     * however, the host USERD/GPFIFO is double-mapped: scheduling the
                     * host before QEMU has decided whether a pushbuffer is soft-complete
                     * lets the host GPU fetch guest UVM setup work without an explicit
                     * HOSTGR ring. Acknowledge guest GR schedule enables here and defer
                     * the host schedule until nvkvm_m2_ring_host_channel(). */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t hClient = ldl_le_p(resp + 80);
                    uint32_t hObject = ldl_le_p(resp + 84);
                    uint32_t engine = nvkvm_m2_tsg_engine(s, hObject);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[3] = {0};
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    int marked = 0;
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    bool defer_gr_enable = s->m2fwd && engine == 1u &&
                                           ps <= sizeof(cbuf) &&
                                           ps >= 1 && cbuf[0] != 0;
                    if (defer_gr_enable) {
                        rc = 0;
                        st = 0;
                    } else if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, hClient, hObject, ctrl, cbuf, ps, &st);
                    }
                    if (rc == 0 && st == 0) {
                        bool host_scheduled = !defer_gr_enable &&
                                              (ps < 1 || cbuf[0] != 0);
                        nvkvm_m2_mark_host_tsg_scheduled(s, hClient, hObject,
                                                         host_scheduled);
                        for (int ci = 0; ci < s->chan_n; ci++) {
                            if (s->chans[ci].client == hClient &&
                                s->chans[ci].tsg == hObject) {
                                marked++;
                            }
                        }
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.7 GPFIFO_SCHEDULE ctrl hClient=0x%08x "
                             "hObject=0x%08x engine=0x%x ps=%u enable=%u "
                             "skipSubmit=%u skipEnable=%u fwd_rc=%d fwd_st=0x%x "
                             "marked=%d%s%s\n",
                             s->chip->name, hClient, hObject, engine, ps,
                             ps >= 1 ? cbuf[0] : 0, ps >= 2 ? cbuf[1] : 0,
                             ps >= 3 ? cbuf[2] : 0, rc, st, marked,
                             defer_gr_enable ? "  DEFER-HOST" : "",
                             defer_gr_enable ? "" :
                             ((rc == 0 && st == 0) ? "  HOST-SERVICED"
                                                   : "  guest-visible OK fallback"));
                } else if (ctrl == 0xa06c0103u) {
                    /* M8.77: NVA06C_CTRL_CMD_SET_TIMESLICE is a small TSG-side
                     * setup control that native libcuda forwards before launch
                     * (params observed as 0008000000000000).  Echoing NV_OK drops
                     * the host scheduler side effect, while forwarding it is safe:
                     * unlike GPFIFO_SCHEDULE, it does not submit the host channel. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t hClient = ldl_le_p(resp + 80);
                    uint32_t hObject = ldl_le_p(resp + 84);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[32];
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    memset(cbuf, 0, sizeof(cbuf));
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, hClient, hObject, ctrl,
                                               cbuf, ps, &st);
                    }
                    if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.77 TSG_SET_TIMESLICE ctrl "
                             "hClient=0x%08x hObject=0x%08x ps=%u "
                             "fwd_rc=%d fwd_st=0x%x%s\n",
                             s->chip->name, hClient, hObject, ps, rc, st,
                             (rc == 0 && st == 0) ? "  HOST-SERVICED"
                                                   : "  guest-visible OK fallback");
                } else if (ctrl == 0x20801210u) {
                    /* M8.78: NV2080_CTRL_CMD_GR_SET_CTXSW_PREEMPTION_MODE is
                     * issued on the subdevice with the TSG handle embedded in
                     * params[4].  Native sends it immediately before launch
                     * setup; returning NV_OK+echo leaves the host GR context
                     * without the same preemption/scheduling state. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t hClient = ldl_le_p(resp + 80);
                    uint32_t hObject = ldl_le_p(resp + 84);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[64];
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    memset(cbuf, 0, sizeof(cbuf));
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, hClient, hObject, ctrl,
                                               cbuf, ps, &st);
                    }
                    if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.78 GR_SET_CTXSW_PREEMPTION "
                             "hClient=0x%08x hObject=0x%08x tsg=0x%08x "
                             "ps=%u fwd_rc=%d fwd_st=0x%x%s\n",
                             s->chip->name, hClient, hObject,
                             ps >= 8 ? ldl_le_p(cbuf + 4) : 0, ps, rc, st,
                             (rc == 0 && st == 0) ? "  HOST-SERVICED"
                                                   : "  guest-visible OK fallback");
                } else if (ctrl == 0x83de0309u) {
                    /* M8.80: NV83DE_CTRL_CMD_DEBUG_SET_EXCEPTION_MASK is part
                     * of the native pre-launch control stream.  Replay preserves
                     * the guest-visible bytes, but the host shadow debug object
                     * still needs the side effect before the host channel runs. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t hClient = ldl_le_p(resp + 80);
                    uint32_t hObject = ldl_le_p(resp + 84);
                    uint8_t cbuf[16];
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    memset(cbuf, 0, sizeof(cbuf));
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, hClient, hObject, ctrl,
                                               cbuf, ps, &st);
                    }
                    if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    } else if (ps >= 4) {
                        stl_le_p(resp + 120, 0x3au);
                    }
                    stl_le_p(resp + 92, 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.80 DEBUG_SET_EXCEPTION_MASK "
                             "hClient=0x%08x hObject=0x%08x ps=%u "
                             "fwd_rc=%d fwd_st=0x%x mask=0x%08x%s\n",
                             s->chip->name, hClient, hObject, ps, rc, st,
                             ps >= 4 ? ldl_le_p(resp + 120) : 0,
                             (rc == 0 && st == 0) ? "  HOST-SERVICED"
                                                   : "  guest-visible replay fallback");
                } else if (ctrl == 0xa06c010au) {
                    /* M8.6: NVA06C_CTRL_CMD_INTERNAL_PROMOTE_FAULT_METHOD_BUFFERS is all input
                     * params, but it has host/GSP side effects: it promotes the TSG fault-method
                     * buffers created during channel-group allocation. Returning NV_OK+zeros
                     * drops that side effect; cuCtxCreate then spins before any GP work is
                     * submitted, with libcuda waiting for its channel-group state to become
                     * ready. Forward narrowly to the host shadow TSG, preserving guest-visible OK
                     * fallback if the shadow object cannot service it. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    uint32_t st = 0xffffu;
                    int rc = -1;
                    uint8_t cbuf[256];
                    uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                    memset(cbuf, 0, sizeof(cbuf));
                    if (cn) {
                        memcpy(cbuf, resp + 120, cn);
                    }
                    if (s->m2fwd && ps <= sizeof(cbuf)) {
                        rc = nvkvm_m2_control1(s, ldl_le_p(resp + 80),
                                               ldl_le_p(resp + 84), ctrl,
                                               cbuf, ps, &st);
                    }
                    if (rc == 0 && st == 0 && cn) {
                        memcpy(resp + 120, cbuf, cn);
                    }
                    stl_le_p(resp + 92, (rc == 0 && st == 0) ? st : 0u);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                    qemu_log("nvkvm-gpu[%s] M8.6 PROMOTE_FAULT_METHOD_BUFFERS "
                             "ps=%u entries=%u fwd_rc=%d fwd_st=0x%x%s\n",
                             s->chip->name, ps, ps >= 88 ? ldl_le_p(resp + 120 + 84) : 0,
                             rc, st, (rc == 0 && st == 0) ? "  HOST-SERVICED"
                                                         : "  guest-visible OK fallback");
                } else if (s->m2fwd && (ctrl == 0x906f0101u || ctrl == 0x0080170du)) {
                    /* item-3 (cuCtxCreate crash hunt): FORWARD these GET controls to
                     * the real host GPU instead of replying NV_OK+zeros.  A faked
                     * count/size here can shift libcuda's stack layout so the later
                     * 0xc7c0 GR-alloc copy_to_user overruns pAllocParms (proven crash
                     * mechanism).  Candidates: 0x906f0101 GET_CLASS_ENGINEID (returns
                     * engineID — libcuda sizes per-engine arrays from it), 0x0080170d
                     * FIFO_GET_CHANNELLIST (returns a channel COUNT that may size a
                     * libcuda stack buffer).  nvkvm_m2_control1 does guest->host handle
                     * translation and writes the host reply back into the buffer. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    if (ps && (120u + ps) <= NVKVM_RESP_MAX) {
                        uint8_t cbuf[4096];
                        uint32_t cn = ps < sizeof(cbuf) ? ps : (uint32_t)sizeof(cbuf);
                        memcpy(cbuf, resp + 120, cn);
                        uint32_t st = 0xffff;
                        int rc = nvkvm_m2_control1(s, ldl_le_p(resp + 80),
                                                   ldl_le_p(resp + 84), ctrl,
                                                   cbuf, cn, &st);
                        if (rc == 0) {
                            memcpy(resp + 120, cbuf, cn);
                            stl_le_p(resp + 92, st);
                        } else {
                            stl_le_p(resp + 92, 0); /* fall back to NV_OK echo */
                        }
                        stl_le_p(resp + 56, 32u + 40u + ps);
                        qemu_log("nvkvm-gpu[%s] item-3 FWD ctrl=0x%08x ps=%u rc=%d "
                                 "st=0x%x reply[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                 s->chip->name, ctrl, ps, rc, st,
                                 resp[120], resp[121], resp[122], resp[123],
                                 resp[124], resp[125], resp[126], resp[127]);
                    }
                } else if (ctrl == 0x2080012fu) {
                    /* M10 (cuCtxCreate rbp=0 ROOT CAUSE — host-vs-guest ioctl diff, 2026-06-06):
                     * NV2080_CTRL_CMD_GPU_QUERY_ECC_STATUS (0x2080012f, params 1464B).  A GeForce
                     * GA106 has NO ECC, so the REAL driver returns NV_ERR_NOT_SUPPORTED (0x56) and
                     * the RM sets SKIP_COPYOUT -> libcuda's 1464B ECC buffer is left untouched.  Our
                     * fake NV_OK instead copied 1464B of echoed/zero garbage over that buffer, so
                     * libcuda acted on bogus ECC state and later crashed (saved-rbp clobber ->
                     * rbp=0 SIGSEGV at libcuda+0x466560).  Match the host: report NOT_SUPPORTED.
                     * (Found via the nvioctl_trace LD_PRELOAD host/guest diff: this was the ONLY
                     * control whose status diverged — host 0x56 vs guest 0x0.) */
                    stl_le_p(resp + 92, 0x56u);   /* NV_ERR_NOT_SUPPORTED */
                } else if (ctrl == 0x20803002u) {
                    /* M10: NV2080_CTRL_CMD_NVLINK_GET_NVLINK_STATUS (0x20803002).  The
                     * params struct is huge (NV2080_CTRL_NVLINK_LINK_STATUS_INFO
                     * linkInfo[MAX_ARR_SIZE], ~13 KB).  On a no-NVLink GeForce (GA106) the
                     * real driver returns NV_ERR_NOT_SUPPORTED, on which the RM sets
                     * RMAPI_PARAM_COPY_FLAGS_SKIP_COPYOUT and copies NOTHING back to libcuda.
                     * Our fake NV_OK echo instead forced a full ~13 KB param copy_to_user into
                     * libcuda's stack buffer.  Match the host: report NOT_SUPPORTED so the guest
                     * skips the copyout.  (No NVLink on this part, so this is the correct status.) */
                    stl_le_p(resp + 92, 0x56u);   /* NV_ERR_NOT_SUPPORTED */
                } else if (ctrl == 0x20800a5cu) {
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
                    /* FIFO_GET_DEVICE_INFO_TABLE: the engine enumeration that drives the guest's
                     * classDB + KernelCE objects. M14 (user direction — forward, don't hardcode):
                     * this control is ROUTE_TO_PHYSICAL but NOT privileged/internal (flags 0x5c040),
                     * so FORWARD it to the host's real GSP and return THIS GPU's actual engine table
                     * dynamically — no per-GPU baked blob (which was captured truncated: 10 entries,
                     * no video engines -> guest GET_CLASSLIST_V2 numClasses 97 vs host 107). Fall back
                     * to the captured GA106 blob only if the forward isn't serviceable yet. */
                    uint32_t base = ldl_le_p(resp + 120);
                    if (s->m2fwd) {
                        nvkvm_m2_capture_devinfo(s);   /* one-shot live host capture */
                    }
                    if (s->m2_devinfo_n > 0) {
                        /* serve from the live host capture, paginated (32/page) */
                        uint32_t psize = 12u + 32u * 100u; /* 3212 */
                        memset(resp + 120, 0, psize);
                        stl_le_p(resp + 120, base);
                        uint32_t n_this = 0;
                        if (base < s->m2_devinfo_n) {
                            n_this = s->m2_devinfo_n - base;
                            if (n_this > 32u) n_this = 32u;
                            memcpy(resp + 132, s->m2_devinfo + (uint64_t)base * 100,
                                   (uint64_t)n_this * 100);
                        }
                        stl_le_p(resp + 124, n_this);
                        stl_le_p(resp + 128, (base + n_this < s->m2_devinfo_n) ? 1u : 0u);
                        stl_le_p(resp + 96, psize);
                        stl_le_p(resp + 92, 0);
                        stl_le_p(resp + 56, 32u + 40u + psize);
                    } else {
                        /* fallback: captured GA106 blob (only if live capture unavailable) */
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
                    }
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
                        if (ctrl == 0x20800102u) {
                            /* Match host RM: bit 31 is reserved in NV2080_CTRL_GPU_INFO_INDEX
                             * and is stripped from the returned list entry.  The guest request
                             * currently arrives as 0x80000011; leaving that bit set is the
                             * remaining non-gpuId control divergence in the cuCtxCreate trace. */
                            idx &= 0x7fffffffu;
                            stl_le_p(resp + eoff, idx);
                        }
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
                } else if (ctrl == 0x20803601u) {
                    /* M5.3 forge gap (host-vs-guest control diff): version-ish struct
                     * {u32@0=1; u8@4=1; u8@5=1; char ver[]@6}. libcuda reads the
                     * driver/GSP version; an all-zero reply (CTRL-UNFILLED) may steer it
                     * into a bad path during cuCtxCreate. Replay the host 580.159.04. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    memset(resp + 120, 0, ps);
                    if (ps >= 6) { stl_le_p(resp + 120, 1u); resp[124] = 1u; resp[125] = 1u; }
                    if (ps >= 16) { memcpy(resp + 126, "580.159.04", 10); }
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20802a0au) {
                    /* M5.3 forge gap: host returns four u16 {0x3e3,0x3e3,0x3e2,0x3e2}@0
                     * and 0x0f@128 (looks like per-unit caps/clocks). Replay. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    memset(resp + 120, 0, ps);
                    if (ps >= 8) {
                        stw_le_p(resp + 120, 0x03e3); stw_le_p(resp + 122, 0x03e3);
                        stw_le_p(resp + 124, 0x03e2); stw_le_p(resp + 126, 0x03e2);
                    }
                    if (ps >= 129) { resp[120 + 128] = 0x0f; }
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20802a0bu) {
                    /* M13 (host-vs-guest ptrace diff, 2026-06-06): CE_GET_ALL_PHYSICAL_CAPS.
                     * The guest kernel's subdeviceCtrlCmdCeGetAllCaps builds the user-visible
                     * CE_GET_ALL_CAPS reply from THIS GSP physical-caps RPC (kernel_ce_shared.c).
                     * We weren't answering it -> base caps stayed 0 -> guest CE_GET_ALL_CAPS
                     * capsTbl=0 vs host 0x03e3 (the nvtrace/nvdecode diff caught this). Struct =
                     * { NvU8 capsTbl[64][2]; NvU64 present@128 }. Replay the host GA106 values:
                     * CE0/CE1=0x03e3 (GRCE|SHARED|SYSMEM|P2P|BL>64K), CE2/CE3=0x03e2, present=0xf. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    memset(resp + 120, 0, ps);
                    if (ps >= 8) {
                        stw_le_p(resp + 120, 0x03e3); stw_le_p(resp + 122, 0x03e3);
                        stw_le_p(resp + 124, 0x03e2); stw_le_p(resp + 126, 0x03e2);
                    }
                    if (ps >= 136) { stq_le_p(resp + 120 + 128, 0x0full); }
                    stl_le_p(resp + 92, 0);
                    stl_le_p(resp + 56, 32u + 40u + ps);
                } else if (ctrl == 0x20808162u) {
                    /* M5.3 forge gap: host returns bool=1. */
                    uint32_t ps = ldl_le_p(resp + 96);
                    if (ps >= 1) { resp[120] = 1u; }
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
                /* else: void/SET control — echo with status=NV_OK.
                 * NOTE (M11, 2026-06-06): a blanket "default-forward instead of fake NV_OK" was
                 * tried and REVERTED — it regresses cuInit.  GSP-INTERNAL controls (e.g. the
                 * NV2080 interface-0x0a / 0x2a "_INTERNAL_*" series like 0x20802a0a/0x20802a12)
                 * are serviced by the real host's GSP and return NV_OK there, but when re-issued
                 * by our unprivileged forwarded client via the userspace RM_CONTROL ioctl they
                 * return 0x1b (INSUFFICIENT_PERMISSIONS) or 0x56 (NOT_SUPPORTED) — so status alone
                 * cannot classify them (0x56 is legit for ECC/NVLINK but wrong here).  Correct fix
                 * = a CURATED allowlist of forwardable controls (host-native trace as oracle:
                 * scripts/mode2_diag), forwarding those and REPLAYING captured data for the
                 * GSP-internal rest.  See [[mode2-control-forward-vs-replay]]. */

                /* M9 (cuCtxCreate SIGSEGV root cause — proven 2026-06-05): a control reply
                 * must NEVER write more bytes than the caller's params buffer (= the REQUEST
                 * paramsSize the caller allocated).  Several handlers above force a fixed,
                 * larger reply paramsSize than was requested; the guest kernel's params
                 * copyout then overruns the caller's (often stack) buffer.  Watchpoint +
                 * host-vs-guest gdb pinned the crash: a kernel copy_to_user during a control
                 * inside cuCtxCreate's GR-object setup zeroes libcuda's saved-rbp stack slot
                 * -> rbp=0 -> SIGSEGV at libcuda+0x466560.  Clamp reply paramsSize to the
                 * request.  (The caller always allocates its full struct, so legitimate large
                 * controls — INTR_GET_KERNEL_TABLE, DEVICE_INFO_TABLE — request that size and
                 * are unaffected; the clamp only fires on a genuine over-size, which is a bug
                 * and a guest-OOB-write hazard.) */
                if (req_psize > 0) {
                    uint32_t out_psize = ldl_le_p(resp + 96);
                    if (out_psize > req_psize) {
                        qemu_log("nvkvm-gpu[%s] M9 CTRL-CLAMP cmd=0x%08x reply psize %u -> %u "
                                 "(caller buffer)\n", s->chip->name, ctrl, out_psize, req_psize);
                        stl_le_p(resp + 96, req_psize);
                        stl_le_p(resp + 56, 32u + 40u + req_psize);
                    }
                }
                /* DIAG(B-compute): log controls we did NOT fill with real data
                 * (cr==NULL and not special-cased) that REQUEST a non-zero
                 * response (a GET) — these return NV_OK+zeros and are the
                 * cuInit=100 compute-cap suspects. */
                if (!cr && ctrl != 0x20800a5cu && ctrl != 0x20801112u &&
                    ctrl != 0x20802a08u && ctrl != 0x208001b0u &&
                    ctrl != 0x20800102u && ctrl != 0x20801303u &&
                    ctrl != 0x20803801u && ctrl != 0x20801702u &&
                    ctrl != 0x906f0102u && ctrl != 0xa06c0101u &&
                    ctrl != 0xa06c0103u && ctrl != 0x20801210u &&
                    ctrl != 0x83de0309u &&
                    ctrl != 0xa06c010au) {
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
    nvkvm_dmaw(pdev, s->q_shmem + s->q_stat_base + 0x20, rp, sizeof(rp));
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
                        if (nvkvm_dmaw(pdev, shmem + statoff, txh,
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
static void nvkvm_chan_execute(NvkvmGpuEmul *s, bool prepare_only);
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
    /* DEBUG-PROOF backdoor: the patched guest UVM reports a tracking-semaphore
     * GPA (lo@0xFFF500, hi@0xFFF504) then writes the payload@0xFFF508 to commit;
     * QEMU forges the GPU's CE SEM_RELEASE by DMA-writing the payload to that
     * guest-RAM GPA, unblocking the UVM channel busy-poll that no observable RPC
     * lets us resolve. Bring-up proof only (see struct comment). */
    if (off == 0xFFF500u) { s->dbg_gpa_lo = (uint32_t)val; return; }
    if (off == 0xFFF504u) { s->dbg_gpa_hi = (uint32_t)val; return; }
    if (off == 0xFFF508u) {
        uint64_t gpa = ((uint64_t)s->dbg_gpa_hi << 32) | s->dbg_gpa_lo;
        uint8_t b[4]; stl_le_p(b, (uint32_t)val);
        if (gpa) {
            nvkvm_dmaw(&s->parent_obj, gpa, b, 4);
            qemu_log("nvkvm-gpu[%s] M5: DBG-FORGE uvm sema GPA=0x%llx <- payload=%u\n",
                     s->chip->name, (unsigned long long)gpa, (uint32_t)val);
        }
        return;
    }
    /* M8.14 DEBUG HtoD shadow bridge.  Guest kernel instrumentation reports a
     * coherent guest-RAM page containing cuMemcpyHtoD source bytes, keyed by
     * the destination CUDA device VA.  QEMU resolves later CE reads from that
     * VA through this live table, replacing the older LD_PRELOAD+m2pbmap row. */
    if (off == 0xFFF520u) { s->m2_uvm_lo = (uint32_t)val; return; }
    if (off == 0xFFF524u) { s->m2_uvm_hi = (uint32_t)val; return; }
    if (off == 0xFFF528u) { s->m2_uvm_gpa_lo = (uint32_t)val; return; }
    if (off == 0xFFF52cu) { s->m2_uvm_gpa_hi = (uint32_t)val; return; }
    if (off == 0xFFF530u) { s->m2_uvm_size_lo = (uint32_t)val; return; }
    if (off == 0xFFF534u) { s->m2_uvm_size_hi = (uint32_t)val; return; }
    if (off == 0xFFF538u) {
        uint64_t va = ((uint64_t)s->m2_uvm_hi << 32) | s->m2_uvm_lo;
        uint64_t gpa = ((uint64_t)s->m2_uvm_gpa_hi << 32) | s->m2_uvm_gpa_lo;
        uint64_t sz = ((uint64_t)s->m2_uvm_size_hi << 32) | s->m2_uvm_size_lo;
        bool uvm_ext_report = ((uint32_t)val & 0x80000000u) != 0;
        if (uvm_ext_report && va && sz && sz <= (512ull << 20)) {
            uint32_t hClient = (uint32_t)(gpa >> 32);
            uint32_t hMemory = (uint32_t)gpa;
            int slot = nvkvm_m2_uvm_ext_record(s, va, sz);
            if (slot >= 0 && hClient && hMemory) {
                s->m2_uvm_ext[slot].hClient = hClient;
                s->m2_uvm_ext[slot].hMemory = hMemory;
            }
            bool high_va = nvkvm_m2_uvm_ext_trace_range(va, sz);
            static uint32_t high_ext_logs;
            if (s->m2_uvm_ext_logs++ < 128 ||
                (high_va && high_ext_logs++ < 128)) {
                qemu_log("nvkvm-gpu[%s] M8.15 UVM-EXT[%d] VA=0x%llx "
                         "size=0x%llx hClient=0x%08x hMemory=0x%08x "
                         "commit=0x%x\n",
                         s->chip->name, slot, (unsigned long long)va,
                         (unsigned long long)sz, hClient, hMemory,
                         (uint32_t)val);
            }
        } else if (va && gpa && sz && sz <= (16ull << 20)) {
            int slot = -1;
            for (int i = 0; i < s->m2_uvm_shadow_n; i++) {
                if (s->m2_uvm_shadow[i].va == va) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                if (s->m2_uvm_shadow_n < (int)ARRAY_SIZE(s->m2_uvm_shadow)) {
                    slot = s->m2_uvm_shadow_n++;
                } else {
                    slot = (int)(val % ARRAY_SIZE(s->m2_uvm_shadow));
                }
            }
            s->m2_uvm_shadow[slot].va = va;
            s->m2_uvm_shadow[slot].gpa = gpa;
            s->m2_uvm_shadow[slot].size = sz;
            s->m2_uvm_shadow[slot].bar1 =
                nvkvm_m2_bar1_gpa_to_off(s, gpa, sz, NULL);
            uint64_t phys = 0;
            bool applied = false;
            if (nvkvm_m2_uvm_ext_lookup(s, va, sz, &phys)) {
                for (int i = 0; i < s->m2_gpga_n; i++) {
                    if (phys >= s->m2_gpga[i].gpga_base &&
                        phys + sz <= s->m2_gpga[i].gpga_base + s->m2_gpga[i].size) {
                        int oi = s->m2_gpga[i].obj_idx;
                        if (oi >= 0 && oi < s->m2_objs_n && s->m2_objs[oi].cpu_qva) {
                            uint8_t *dst = (uint8_t *)s->m2_objs[oi].cpu_qva +
                                           s->m2_gpga[i].off +
                                           (phys - s->m2_gpga[i].gpga_base);
                            if (nvkvm_m2_uvm_shadow_read(s, va, dst, sz)) {
                                applied = true;
                            }
                        }
                        break;
                    }
                }
                bool high_va = nvkvm_m2_uvm_ext_trace_range(va, sz);
                static uint32_t high_shadow_apply_logs;
	                if (applied && (s->m2_uvm_shadow_logs < 128 ||
	                                (high_va && high_shadow_apply_logs++ < 128))) {
	                    qemu_log("nvkvm-gpu[%s] M8.15 UVM-SHADOW applied VA=0x%llx "
	                             "GPA=0x%llx size=0x%llx\n", s->chip->name,
	                             (unsigned long long)va, (unsigned long long)gpa,
	                             (unsigned long long)sz);
	                }
	            }
            if (s->m2_doorbell_ready && va < 0x300000000ull &&
                s->m2_uvm_shadow_retry_pending) {
                static uint32_t retry_logs;
                s->m2_uvm_shadow_retry_pending = false;
                if (retry_logs++ < 128) {
                    qemu_log("nvkvm-gpu[%s] M8.43 UVM-SHADOW retry doorbell "
                             "VA=0x%llx size=0x%llx%s\n", s->chip->name,
                             (unsigned long long)va, (unsigned long long)sz,
                             applied ? " applied" : "");
                }
                nvkvm_m2_exec_doorbell(s);
            }
            bool high_va = nvkvm_m2_uvm_ext_trace_range(va, sz);
            static uint32_t high_shadow_logs;
            if (s->m2_uvm_shadow_logs++ < 64 ||
                (high_va && high_shadow_logs++ < 128)) {
                qemu_log("nvkvm-gpu[%s] M8.14 UVM-SHADOW[%d] VA=0x%llx "
                         "GPA=0x%llx size=0x%llx commit=0x%x%s\n",
                         s->chip->name, slot, (unsigned long long)va,
                         (unsigned long long)gpa, (unsigned long long)sz,
                         (uint32_t)val,
                         s->m2_uvm_shadow[slot].bar1 ? " BAR1" : "");
            }
        } else if (s->trace) {
            qemu_log("nvkvm-gpu[%s] M8.14 UVM-SHADOW reject VA=0x%llx "
                     "GPA=0x%llx size=0x%llx commit=0x%x\n",
                     s->chip->name, (unsigned long long)va,
                     (unsigned long long)gpa, (unsigned long long)sz,
                     (uint32_t)val);
        }
        return;
    }
    /* M5 — work-submit doorbell.  Detect the channel submission (the guest wrote
     * the work-submit token).  TODO(M5): execute the channel — walk its GPFIFO ->
     * pushbuffer -> CE semaphore release and write the payload so the driver's
     * channelWaitForFinishPayload poll completes (currently times out at
     * ce_utils.c:349).  For now, log it so the doorbell offset/token are
     * confirmed against the GA100 HAL. */
    if (off == NVKVM_VF_DOORBELL) {
        /* M5.11 (doorbell-demux observability): the guest writes its work-submit TOKEN here
         * (val). On Ampere the token encodes the guest vChid+runlist (NVC36F GET_WORK_SUBMIT_TOKEN).
         * Today we ignore it and ring the host GR token unconditionally — wrong for multi-channel.
         * Log each distinct token (deduped) so we can map guest-token -> channel for the real
         * vChid->sChid demux. No behavior change. */
        if (s->m2_crashwin && (!s->m2_last_db_valid || s->m2_last_db_token != (uint32_t)val)) {
            s->m2_last_db_token = (uint32_t)val; s->m2_last_db_valid = true;
            qemu_log("nvkvm-gpu[%s] M5.11 DOORBELL token=0x%08x (chid-field guesses: [3:0]=%u "
                     "[11:0]=%u [27:0]=0x%x); chan_n=%d\n", s->chip->name, (uint32_t)val,
                     (uint32_t)val & 0xf, (uint32_t)val & 0xfff, (uint32_t)val & 0x0fffffff, s->chan_n);
        }
        /* M5.6 EXECUTION-PLANE INVENTORY: once cuCtxCreate has built the GR context
         * (crashwin armed) and starts submitting work, dump the EXACT working set the
         * execution path must back+FIXED-map: va_map = the #2 side-table (PROMOTE_CTX
         * GPU-VA->guest-FB ctx buffers) and chans[] = the channel rings (GPFIFO/USERD).
         * One-shot; logging only. */
        if (s->m2_crashwin && !s->m2_inventory_done) {
            s->m2_inventory_done = true;
            qemu_log("nvkvm-gpu[%s] M5.6 INVENTORY @doorbell va_map_n=%d chan_n=%d:\n",
                     s->chip->name, s->va_map_n, s->chan_n);
            for (int i = 0; i < s->va_map_n; i++) {
                qemu_log("nvkvm-gpu[%s] M5.6   va_map[%d] client=0x%08x VA=0x%llx -> "
                         "%s phys=0x%llx size=0x%llx\n", s->chip->name, i,
                         s->va_map[i].client, (unsigned long long)s->va_map[i].va,
                         s->va_map[i].sys ? "SYS" : "FB",
                         (unsigned long long)s->va_map[i].phys,
                         (unsigned long long)s->va_map[i].size);
            }
            for (int i = 0; i < s->chan_n; i++) {
                qemu_log("nvkvm-gpu[%s] M5.6   chan[%d] client=0x%08x gpfifo_va=0x%llx "
                         "ent=%u userd=0x%llx(%s) hvas=0x%08x\n", s->chip->name, i,
                         s->chans[i].client, (unsigned long long)s->chans[i].gpfifo_va,
                         s->chans[i].gpfifo_ent, (unsigned long long)s->chans[i].userd,
                         s->chans[i].userd_sys ? "sys" : "fb", s->chans[i].hvaspace);
            }
        }
        /* M5.7 EXECUTION PLANE (gated m2exec, default off): back the GR working set with
         * real host GPU memory and FIXED-map it into the GR channel's VASpace at the guest
         * VAs, so the host channel's MMU resolves the guest's submitted work. One-shot.
         * The va_map (PROMOTE_CTX) entries carry VA<->guest-FB<->size for the ctx buffers
         * (under sibling RM clients that share the GR address space); we map them under the
         * GR compute client's GR vaspace (0x5c000007), proven mappable in M5.5 P2. This is
         * the FIRST execution-path increment; doorbell-forward + GPFIFO/pushbuffer phys
         * resolution follow. */
        if (s->m2exec && !s->m2_exec_done && s->m2_gr_client) {
            s->m2_exec_done = true;
            uint32_t grc = s->m2_gr_client;
            int mapped = 0;
            for (int i = 0; i < s->va_map_n; i++) {
                if (s->va_map[i].sys) {
                    continue;                 /* sysmem leaf: GPU->CPU DMA path, not here */
                }
                char lbl[24];
                snprintf(lbl, sizeof(lbl), "ctx%d", i);
                if (nvkvm_m2_back_and_map(s, grc, s->va_map[i].va, s->va_map[i].phys,
                                          s->va_map[i].size, false, lbl)) {
                    mapped++;
                }
            }
            qemu_log("nvkvm-gpu[%s] M5.7 EXEC: backed %d/%d FB working-set buffers into GR "
                     "client 0x%08x VASpace\n", s->chip->name, mapped, s->va_map_n, grc);
            /* The GR channel's GPFIFO VA is a UVM mapping NOT forwarded to the host, so it
             * is FREE in 0x5c000007 (probe: FIXED map there SUCCEEDS, unlike the already-
             * host-mapped ctx VAs). For real operation we must DOUBLE-mmap it: resolve its
             * guest-FB phys by walking the guest GR page tables (try each snooped VAS PDB),
             * so the guest's submitted GP entries (written via BAR->fb_write at that phys)
             * land in the SAME host memory the host channel's GPFIFO reads. */
            for (int i = 0; i < s->chan_n; i++) {
                if (s->chans[i].client != grc || !s->chans[i].gpfifo_va) {
                    continue;
                }
                uint64_t gva = s->chans[i].gpfifo_va, gphys = 0;
                for (int v = 0; v < s->chan_vas_n; v++) {
                    bool sy = false;
                    uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[v].pdb, gva, &sy);
                    if (p != NVKVM_GMMU_FAULT && !sy) {
                        gphys = p & ~0xfffull;
                        qemu_log("nvkvm-gpu[%s] M5.7 GPFIFO VA=0x%llx resolved via VAS[%d] "
                                 "pdb=0x%llx -> FB phys=0x%llx\n", s->chip->name,
                                 (unsigned long long)gva, v,
                                 (unsigned long long)s->chan_vas[v].pdb,
                                 (unsigned long long)gphys);
                        break;
                    }
                }
                if (!gphys) {
                    qemu_log("nvkvm-gpu[%s] M5.7 GPFIFO VA=0x%llx phys UNRESOLVED "
                             "(chan_vas_n=%d) — VA-only map\n", s->chip->name,
                             (unsigned long long)gva, s->chan_vas_n);
                }
                if (gphys) {
                    s->chans[i].gpfifo_phys = gphys;
                }
                nvkvm_m2_back_and_map(s, grc, gva, gphys, 0x10000, true, "gpfifo");
                break;
            }
            /* M5.8: set up doorbell-forward primitives (map host USERMODE + fetch the GR
             * channel work-submit token). NOT rung yet — ringing before pushbuffers are
             * mapped + the channel scheduled would fault/wedge the host GPU. */
            nvkvm_m2_doorbell_setup(s, grc);
        }
        /* M5.9: real execution forward — map this doorbell's new GR pushbuffers and ring
         * the host doorbell only after the mapper proved the pending GPFIFO entries are
         * backed.  The local Phase-B parser below is now limited to non-GR channels; letting
         * it consume/ring GR work bypassed the user-mmap GPA bridge and could fault host GR. */
        nvkvm_m2_exec_doorbell(s);
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
        bool any_completed = false;
        for (int i = 0; i < s->chan_n; i++) {
            struct nvkvm_chan_entry *c = &s->chans[i];
            uint32_t engine = nvkvm_m2_tsg_engine(s, c->tsg);
            if (s->m2exec && engine == 1u) {
                static uint32_t gr_local_skip_cnt;
                if (gr_local_skip_cnt++ < 64) {
                    qemu_log("nvkvm-gpu[%s] M8.19 skip local GR consume ch[%d] "
                             "TSG=0x%08x; host GR is driven by exec_doorbell\n",
                             s->chip->name, i, c->tsg);
                }
                continue;
            }
            /* Load this channel into the chan_* working set chan_execute reads. */
            s->chan_gpfifo_va  = c->gpfifo_va;
            s->chan_userd      = c->userd;
            s->chan_gpfifo_ent = c->gpfifo_ent;
            s->chan_userd_sys  = c->userd_sys;
            s->chan_hvaspace   = c->hvaspace;
            s->chan_client     = c->client;
            s->chan_gp_get     = c->gp_get;
            s->chan_gpfifo_phys = c->gpfifo_phys;
            s->chan_pdb        = c->pdb;
            /* M5.28: route this channel's working-set maps into ITS per-channel fresh VAS
             * (matched by parent TSG). On first touch, mirror the channel's whole guest PDB
             * into the fresh VAS so every guest VA places into a VAS we own (no host-RM
             * ctx self-promote collision). m2_cur_cvas stays set for chan_execute's reactive
             * maps below, then is reset after the iteration. */
            s->m2_cur_cvas = -1;
            for (int ci = 0; ci < s->m2_cvas_n; ci++) {
                if (s->m2_cvas[ci].client == c->client && s->m2_cvas[ci].tsg == c->tsg) {
                    s->m2_cur_cvas = ci; break;
                }
            }
            if (s->m2_cur_cvas >= 0 && !s->m2_cvas[s->m2_cur_cvas].populated) {
                nvkvm_m2_populate_cvas(s, c);
                s->m2_cvas[s->m2_cur_cvas].populated = true;
            }
            if (c->gpfifo_ent > NVKVM_M2_GPFIFO_LARGE_RING_ENTRIES) {
                nvkvm_m2_repair_guest_userd_gp_put(s, c, c->gp_get,
                                                   "local-stale-large-ring");
            }
            uint32_t before = c->gp_get;
            nvkvm_chan_execute(s, false);
            c->gpfifo_phys = s->chan_gpfifo_phys; /* persist M5.16 resolution */
            c->pdb = s->chan_pdb;
            if (s->chan_unresolved) {
                qemu_log("nvkvm-gpu[%s] M8.11 ch[%d] unresolved: gp_get stays %u "
                         "gp_put pending for host/GPA bridge\n",
                         s->chip->name, i, before);
                c->gp_get = before;
                continue;
            }
            c->gp_get = s->chan_gp_get;          /* save consumed index */
            if (c->gp_get == before) {
                continue;                        /* no new work on this channel */
            }
            nvkvm_m2_repair_guest_userd_gp_put(s, c, c->gp_get,
                                               "local-doorbell");
            nvkvm_m2_write_guest_userd_gp_get(s, c, c->gp_get, "local-doorbell");
            any_completed = true;
            static uint32_t ce_skip_cnt;
            if (ce_skip_cnt++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.16 local channel ch[%d] TSG=0x%08x "
                         "engine=0x%x; no host ring\n",
                         s->chip->name, i, c->tsg, engine);
            }
            if (s->chan_sem_released) {
                continue;                        /* explicit release already done */
            }
            /* Fallback: implicit finish-payload semaphore. */
            uint64_t sema_va = c->gpfifo_va + 0x8004ull;
            bool is_sys = false;
            uint64_t phys = nvkvm_chan_translate(s, sema_va, &is_sys);
            if (phys != NVKVM_GMMU_FAULT || s->chan_gpfifo_phys) {
                uint32_t payload = ++c->payload;
                uint64_t redir = 0;
                uint64_t notify_redir = 0;
                nvkvm_chan_sem_wr32(s, sema_va, payload, &redir);  /* M5.18: also write the BAR1 page libcuda polls */
                int notify_writes = nvkvm_m2_write_channel_notify_block(s, payload,
                                                                        "implicit",
                                                                        &notify_redir);
                qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x ch[%d] -> completed: "
                         "semaVA=0x%llx -> %s phys=0x%llx payload=%u redir=0x%llx "
                         "notify_writes=%d notify_redir=0x%llx\n",
                         s->chip->name, (uint32_t)val, i,
                         (unsigned long long)sema_va, is_sys ? "SYS" : "FB",
                         (unsigned long long)phys, payload, (unsigned long long)redir,
                         notify_writes, (unsigned long long)notify_redir);
            } else {
                qemu_log("nvkvm-gpu[%s] M5: DOORBELL tok=0x%08x ch[%d] -> sema VA "
                         "0x%llx FAULTED; gpfifo=0x%llx\n", s->chip->name,
                         (uint32_t)val, i, (unsigned long long)sema_va,
                         (unsigned long long)c->gpfifo_va);
            }
        }
        s->m2_cur_cvas = -1;    /* M5.28: clear per-channel VAS routing after the loop */
        /* M8.39/M8.49: local CE/setup completions can happen before CUDA has
         * registered the blocking-sync os-event.  Latch that completion and
         * deliver it once an event exists, while host-GR completion still posts
         * from nvkvm_m2_exec_doorbell() with the work-submit token. */
        if (any_completed) {
            static uint32_t local_event_skip_cnt;
            s->m2_local_completion_pending = true;
            s->m2_local_completion_token = (uint32_t)val;
            s->m2_local_completion_token_valid = (uint32_t)val != 0;
            if (local_event_skip_cnt++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.39 LOCAL_EVENT_PENDING completed local "
                         "doorbell work token=%s0x%08x; waiting for os-event "
                         "registration\n",
                         s->chip->name,
                         s->m2_local_completion_token_valid ? "" : "!",
                         s->m2_local_completion_token);
            }
            nvkvm_m2_try_deliver_local_completion(s, "local-doorbell");
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
        nvkvm_m2_deassert_legacy_irq_if_idle(s);
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

    /* M5/M7 — GSP falcon IRQSCLR (0x110004): write-1-to-clear SWGEN0 (bit6).
     * kgspService clears the edge before draining the queue. */
    if (off == 0x00110004u) {
        if (val & NVKVM_M2_GSP_SWGEN0_BIT) {
            bool was_pending = s->gsp_swgen0_pending;
            s->gsp_swgen0_pending = false;
            nvkvm_m2_clear_intr_vector(s, NVKVM_M2_GSP_SWGEN0_VECTOR,
                                       "swgen0-clear");
            static uint32_t clr_logs;
            if (s->trace && clr_logs++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.33 SWGEN0_CLEAR val=0x%llx "
                         "was_pending=%d\n",
                         s->chip->name, (unsigned long long)val, was_pending);
            }
            nvkvm_m2_try_deliver_host_completion(s, "swgen0-clear");
            nvkvm_m2_try_deliver_local_completion(s, "swgen0-clear");
        }
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
        if (s->m2_crashwin) {                /* M6.6 DIAG: does libcuda read BAR1 but FAULT? */
            static uint32_t fcnt;
            if (fcnt++ < 200) {
                qemu_log("nvkvm-gpu[GA106] M6.6 BAR1 RD off=0x%llx -> WALK-FAULT (returns 0)\n",
                         (unsigned long long)off);
            }
        }
        return 0;
    }
    uint64_t rv;
    if (sys) {
        uint8_t b[8] = {0};
        if (pci_dma_read(&s->parent_obj, pa, b, size) != MEMTX_OK) return 0;
        rv = ldn_le_p(b, size);
    } else {
        s->m2_cur_gva = off ? off : 0;       /* CRASHWIN: report the guest GPU VA */
        rv = nvkvm_fb_read(s, pa, size);
        s->m2_cur_gva = 0;
    }
    /* DIAG: BAR1 reads landing in the low-FB region (where the UVM channel's
     * GPFIFO/USERD/semaphore live) — a poll spin shows up as repeated reads of
     * one address; that address is the completion semaphore the guest waits on. */
    if (s->trace && !sys && pa >= NVKVM_DIAG_LOFB_LO && pa < NVKVM_DIAG_LOFB_HI) {
        static uint64_t last_pa; static uint32_t rep; static uint32_t total;
        if (pa != last_pa) { last_pa = pa; rep = 0; }
        if ((rep++ % 4096) == 0 && total++ < 4000) {
            qemu_log("nvkvm-gpu[GA106] DIAG BAR1 RD off=0x%llx -> FB 0x%llx "
                     "= 0x%llx (rep~%u)\n", (unsigned long long)off,
                     (unsigned long long)pa, (unsigned long long)rv, rep);
        }
    }
    /* M5.10 DIAG: after the GR compute object constructs (crashwin), log ALL BAR1 reads
     * (off -> resolved FB/SYS + value + whether m2_fbback-backed) to settle the
     * cuCtxCreate access path: do libcuda's pre-crash reads go via BAR1 (trapped here)
     * and to which FB, and is that FB backed? Capped. */
    if (s->m2_crashwin) {
        static uint32_t bcnt;
        if (bcnt++ < 400) {
            bool backed = (s->m2_fbback_n && nvkvm_fb_host_overlay(s, pa) != NULL);
            qemu_log("nvkvm-gpu[GA106] M5.10 BAR1 RD off=0x%llx -> %s 0x%llx = 0x%llx %s\n",
                     (unsigned long long)off, sys ? "SYS" : "FB", (unsigned long long)pa,
                     (unsigned long long)rv, backed ? "[BACKED]" : "[unbacked]");
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
        nvkvm_dmaw(&s->parent_obj, pa, b, size);
    } else {
        nvkvm_fb_write(s, pa, val, size);
        /* M5.16: remember this vidmem page as a guest-CPU-written backing (the
         * authoritative ring location; see bar1_wpg comment).  MRU-ordered. */
        uint64_t pg = pa & ~0xFFFull;
        int hit = -1;
        for (int i = 0; i < s->bar1_wpg_n; i++) {
            if (s->bar1_wpg[i].page == pg) { hit = i; break; }
        }
        if (hit < 0) {
            if (s->bar1_wpg_n < NVKVM_MAX_BAR1PG) {
                hit = s->bar1_wpg_n++;
            } else {                         /* evict LRU (smallest seq) */
                hit = 0;
                for (int i = 1; i < s->bar1_wpg_n; i++) {
                    if (s->bar1_wpg[i].seq < s->bar1_wpg[hit].seq) { hit = i; }
                }
            }
            s->bar1_wpg[hit].page = pg;
        }
        s->bar1_wpg[hit].seq = ++s->bar1_wpg_seq;
    }
    /* DIAG: BAR1 writes into the low-FB region reveal where the guest CPU lays
     * down the UVM channel's GPFIFO entry, pushbuffer, and inits the semaphore. */
    if (s->trace && !sys && pa >= NVKVM_DIAG_LOFB_LO && pa < NVKVM_DIAG_LOFB_HI) {
        static uint32_t total;
        if (total++ < 2000) {
            qemu_log("nvkvm-gpu[GA106] DIAG BAR1 WR off=0x%llx -> FB 0x%llx "
                     "<- 0x%llx sz=%u\n", (unsigned long long)off,
                     (unsigned long long)pa, (unsigned long long)val, size);
        }
    }
    /* M5.16 DIAG: the LOFB-only filter above is blind to the COMPUTE channel's
     * GPFIFO/pushbuffer, which resolve to HIGH FB (e.g. 0x2eee10000 via the CPU-
     * built device-default VAS).  cuCtxCreate's GP-entry never showed up because
     * its write was never logged — not because it never happened.  Log EVERY
     * vidmem BAR1 write outside LOFB too (hard-capped), so the next run settles
     * whether the guest lays the GP entry into the page our GMMU walk resolves
     * (FB 0x2eee1xxxx) — if it does, the data-capture path is correct and the
     * content-pick "require non-zero" heuristic is the only thing rejecting it;
     * if it doesn't, the GPFIFO is reached via a different aperture/backing. */
    if (s->trace && !sys && !(pa >= NVKVM_DIAG_LOFB_LO && pa < NVKVM_DIAG_LOFB_HI)) {
        static uint32_t htotal;
        if (htotal++ < 4000) {
            qemu_log("nvkvm-gpu[GA106] M5.16 BAR1 WR off=0x%llx -> FB 0x%llx "
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

static uint64_t nvkvm_bar2_pt_rd64(NvkvmGpuEmul *s, uint64_t fb_addr)
{
    bool old = s->m2_in_walk;
    s->m2_in_walk = true;
    uint64_t v = nvkvm_fb_rd64(s, fb_addr);
    s->m2_in_walk = old;
    return v;
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
        uint64_t pde = nvkvm_bar2_pt_rd64(s, tbl + (uint64_t)idx * 8);
        tbl = NVKVM_VER2_ADDR_VID(pde);
        if (tbl == 0) {
            return NVKVM_GMMU_FAULT;
        }
    }

    /* PD0: 16B dual PDE (big-page + small-page sub-tables). */
    uint32_t idx0 = (uint32_t)((va >> 21) & 0xFF);            /* [28:21], 8 bits */
    uint64_t lo = nvkvm_bar2_pt_rd64(s, tbl + (uint64_t)idx0 * 16);
    uint64_t hi = nvkvm_bar2_pt_rd64(s, tbl + (uint64_t)idx0 * 16 + 8);
    uint64_t small_tbl = (((hi >> 8) & ((1ull << 25) - 1)) << 12); /* SMALL bits 96:72, <<12 */
    uint64_t big_tbl   = (((lo >> 4) & ((1ull << 28) - 1)) << 8);  /* BIG  bits 32:4,  <<8  */

    uint64_t pte, page;
    if (small_tbl != 0) {                       /* 4 KiB pages: PT VA[20:12] */
        uint32_t idx = (uint32_t)((va >> 12) & 0x1FF);
        pte  = nvkvm_bar2_pt_rd64(s, small_tbl + (uint64_t)idx * 8);
        if (!(pte & 1)) {
            return NVKVM_GMMU_FAULT;            /* PTE VALID bit 0 */
        }
        page = NVKVM_VER2_ADDR_VID(pte);
        return page + (va & 0xFFFull);
    }
    if (big_tbl != 0) {                          /* 64 KiB pages: PT VA[20:16] */
        uint32_t idx = (uint32_t)((va >> 16) & 0x1F);
        pte  = nvkvm_bar2_pt_rd64(s, big_tbl + (uint64_t)idx * 8);
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
    s->m2_in_walk = true;                /* exclude this PTE read from the CRASHWIN probe */
    uint64_t v = nvkvm_fb_rd64(s, addr);
    s->m2_in_walk = false;
    return v;
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

/* M5.21: the executing channel's OWN VAS PDB, derived from its client's GR
 * VASpace (chan_client -> m2_devvas vas handle -> chan_vas pdb).  This is the
 * AUTHORITATIVE address space for the channel's pushbuffer/sema — unlike the
 * content-pick heuristic below, which scans ALL snooped VASes and can land on a
 * FOREIGN client's VAS that merely aliases the same guest VA (the confirmed
 * wrong-channel bug: the compute channel's working set resolved through the
 * probe client 0xc1d0000a's VAS 0x2efa4c000 instead of its own 0x3114000).
 * Returns 0 if the client's VAS or its PDB isn't known yet (caller falls back). */
static uint64_t nvkvm_chan_own_pdb(NvkvmGpuEmul *s)
{
    if (!s->chan_client) {
        return 0;
    }
    uint32_t hvas = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == s->chan_client) {
            hvas = s->m2_devvas[i].vas;
            break;
        }
    }
    if (!hvas) {
        return 0;
    }
    for (int i = 0; i < s->chan_vas_n; i++) {
        if (s->chan_vas[i].hvas == hvas) {
            return s->chan_vas[i].pdb;
        }
    }
    return 0;
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
        nvkvm_dmaw(&s->parent_obj, phys, b, 4);
    } else {
        nvkvm_fb_write(s, phys, v, 4);
    }
}

/* M5.18 — write a completion-semaphore payload at a CHANNEL GPU VA, redirected to
 * the location the guest CPU actually READS.  For a GSP-managed vidmem channel the
 * channel-VAS walk gives a stale aliasing FB page (libcuda never reads it); the
 * guest accesses the channel buffer through BAR1, which the earlier trace showed is
 * FB-contiguous from chan_gpfifo_phys (the page where the guest wrote the GP entry:
 * GPFIFO BAR1 0xa0000->FB 0x3130000, USERD 0xb0000->0x3140000).  So when the sema
 * VA lies inside the channel's GPFIFO buffer window, ALSO write it at
 * chan_gpfifo_phys + (va - chan_gpfifo_va) — the page libcuda polls.  We write both
 * the channel-VAS page (harmless, may be the real one for sysmem semas) AND the
 * BAR1-relative page, so sysmem semas (other channels) are unaffected. */
#define NVKVM_CHAN_BUF_WINDOW 0x20000ull   /* GPFIFO + USERD + sema slack (128 KiB) */
static bool nvkvm_chan_sem_wr32(NvkvmGpuEmul *s, uint64_t va, uint32_t payload,
                                uint64_t *out_redir)
{
    bool wrote = false;
    uint64_t gpa = 0;
    if (nvkvm_m2_pbmap_lookup(s, va, 4, &gpa)) {
        nvkvm_phys_wr32(s, gpa, true, payload);
        wrote = true;
    }
    if (nvkvm_m2_uvm_shadow_wr32(s, va, payload)) {
        wrote = true;
    }
    uint64_t uvm_phys = 0;
    bool uvm_ext_wrote = false;
    if (nvkvm_m2_uvm_ext_lookup(s, va, 4, &uvm_phys)) {
        nvkvm_phys_wr32(s, uvm_phys, false, payload);
        wrote = true;
        uvm_ext_wrote = true;
    }
    bool sy; uint64_t p = nvkvm_chan_translate(s, va, &sy);
    if (p != NVKVM_GMMU_FAULT) {
        if (!wrote) {
            nvkvm_phys_wr32(s, p, sy, payload);
            wrote = true;
        }
        if (s->m2exec) {
            for (int i = 0; i < s->chan_vas_n; i++) {
                bool asy = false;
                uint64_t ap = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, va, &asy);
                if (ap == NVKVM_GMMU_FAULT || (ap == p && asy == sy)) {
                    continue;
                }
                nvkvm_phys_wr32(s, ap, asy, payload);
                wrote = true;
            }
        }
    }
    /* M5.19 — REAL forward prep: if the completion sema is SYSMEM, map it into the
     * host GR VAS so the REAL host GPU writes the payload here (guest GPA -> shared
     * memfd -> OS_DESCRIPTOR WB -> FIXED map at the matching VA).  Guest then reads
     * the host GPU's write coherently (WB snooped).  Idempotent; m2exec-gated. */
    if (s->m2exec && s->m2_cur_cvas >= 0 &&
        p != NVKVM_GMMU_FAULT && sy &&
        !nvkvm_m2_va_seen(s, s->chan_client, va & ~0xfffull)) {
        uint64_t gbase = p & ~0xfffull;
        bool mok = nvkvm_m2_back_and_map_sys(s, s->chan_client, va & ~0xfffull, gbase, 0x1000);
        qemu_log("nvkvm-gpu[%s] M5.19 fwd-map sema VA=0x%llx gpa=0x%llx -> %s\n",
                 s->chip->name, (unsigned long long)(va & ~0xfffull),
                 (unsigned long long)gbase, mok ? "MAPPED (host GPU writes completion, WB)"
                                                : "map-FAILED");
    }
    if (s->chan_gpfifo_phys && va >= s->chan_gpfifo_va &&
        va <  s->chan_gpfifo_va + NVKVM_CHAN_BUF_WINDOW) {
        uint64_t rp = s->chan_gpfifo_phys + (va - s->chan_gpfifo_va);
        nvkvm_fb_write(s, rp, payload, 4);     /* the page libcuda actually polls */
        if (out_redir) { *out_redir = rp; }
        wrote = true;
    } else if (out_redir) {
        *out_redir = 0;
    }
    if (uvm_ext_wrote) {
        nvkvm_m2_uvm_ext_sync_to_pbmap(s, va, va + 4, "sem-wr");
    }
    return wrote;
}

static int nvkvm_m2_write_channel_notify_block(NvkvmGpuEmul *s, uint32_t payload,
                                               const char *why,
                                               uint64_t *out_last_redir)
{
    static const uint64_t page_bases[] = { 0x0, 0x1000 };
    static const uint64_t singles[] = { 0x8004 };
    uint64_t first_redir = 0;
    uint64_t last_redir = 0;
    int wrote = 0;

    if (!s->chan_gpfifo_va) {
        if (out_last_redir) {
            *out_last_redir = 0;
        }
        return 0;
    }
    /*
     * M8.44 started as a debug bridge that sprayed likely notifier/semaphore
     * offsets when the exact completion location was unknown.  UVM progress
     * semaphores are 64-bit; spraying every 4 bytes can hit the high dword and
     * manufacture values such as 0x100000082.  Keep it available for isolation,
     * but default production/debug-bridge runs to exact parsed semaphores plus
     * the channel's work-submit notifier.
     */
    if (!nvkvm_m2_broad_notify_block()) {
        static uint32_t skip_logs;

        if (out_last_redir) {
            *out_last_redir = 0;
        }
        if (s->trace && skip_logs++ < 32) {
            qemu_log("nvkvm-gpu[%s] M8.44 CHAN_NOTIFY_BLOCK skip %s "
                     "gpfifo=0x%llx payload=%u broad=off\n",
                     s->chip->name, why ? why : "completion",
                     (unsigned long long)s->chan_gpfifo_va, payload);
        }
        return 0;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(page_bases); i++) {
        for (uint32_t off = NVKVM_M2_NOTIFY_SCAN_FIRST;
             off <= NVKVM_M2_NOTIFY_EAGER_LAST; off += 4) {
            uint64_t redir = 0;
            if (nvkvm_chan_sem_wr32(s, s->chan_gpfifo_va + page_bases[i] + off,
                                    payload, &redir)) {
                wrote++;
                if (redir) {
                    if (!first_redir) {
                        first_redir = redir;
                    }
                    last_redir = redir;
                }
            }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(singles); i++) {
        uint64_t redir = 0;
        if (nvkvm_chan_sem_wr32(s, s->chan_gpfifo_va + singles[i],
                                payload, &redir)) {
            wrote++;
            if (redir) {
                if (!first_redir) {
                    first_redir = redir;
                }
                last_redir = redir;
            }
        }
    }

    if (out_last_redir) {
        *out_last_redir = last_redir;
    }
    if (wrote) {
        static uint32_t notify_logs;
        if (notify_logs++ < 256) {
            qemu_log("nvkvm-gpu[%s] M8.44 CHAN_NOTIFY_BLOCK %s "
                     "gpfifo=0x%llx payload=%u writes=%d "
                     "first_redir=0x%llx last_redir=0x%llx\n",
                     s->chip->name, why ? why : "completion",
                     (unsigned long long)s->chan_gpfifo_va, payload, wrote,
                     (unsigned long long)first_redir,
                     (unsigned long long)last_redir);
        }
    }
    return wrote;
}

static bool nvkvm_chan_wr32(NvkvmGpuEmul *s, uint64_t va, uint32_t payload)
{
    bool wrote = false;
    uint64_t gpa = 0;
    if (nvkvm_m2_pbmap_lookup(s, va, 4, &gpa)) {
        nvkvm_phys_wr32(s, gpa, true, payload);
        wrote = true;
    }
    if (nvkvm_m2_uvm_shadow_wr32(s, va, payload)) {
        wrote = true;
    }
    uint64_t uvm_phys = 0;
    bool uvm_ext_wrote = false;
    if (nvkvm_m2_uvm_ext_lookup(s, va, 4, &uvm_phys)) {
        nvkvm_phys_wr32(s, uvm_phys, false, payload);
        wrote = true;
        uvm_ext_wrote = true;
    }
    if (wrote) {
        if (uvm_ext_wrote) {
            nvkvm_m2_uvm_ext_sync_to_pbmap(s, va, va + 4, "wr32");
        }
        return true;
    }
    bool sy = false;
    uint64_t p = nvkvm_chan_translate(s, va, &sy);
    if (p != NVKVM_GMMU_FAULT) {
        nvkvm_phys_wr32(s, p, sy, payload);
        return true;
    }
    return false;
}

static void nvkvm_m2_release_uvm_ce_report_group(NvkvmGpuEmul *s,
                                                 uint64_t sem_addr,
                                                 uint32_t payload,
                                                 const char *why)
{
    if (!s->m2exec || !payload || (sem_addr & 0xfu)) {
        return;
    }

    uint64_t group = sem_addr & ~0x3full;
    uint64_t group_off = group & 0xfffull;
    if (group_off < 0xf00u || group_off + 0x34u > 0x1000u ||
        sem_addr - group > 0x20u) {
        return;
    }

    uint64_t phys = 0;
    if (!nvkvm_m2_uvm_ext_lookup(s, group, 0x34u, &phys)) {
        return;
    }

    for (uint64_t off = 0; off <= 0x20u; off += 0x10u) {
        uint32_t cur = 0;
        if (!nvkvm_m2_uvm_ext_peek32(s, group + off, &cur) ||
            cur != payload) {
            return;
        }
    }

    uint32_t agg = 0;
    if (nvkvm_m2_uvm_ext_peek32(s, group + 0x30u, &agg) &&
        agg == payload) {
        return;
    }

    uint64_t redir = 0;
    if (nvkvm_chan_sem_wr32(s, group + 0x30u, payload, &redir)) {
        static uint32_t group_logs;
        if (s->trace && (group_logs++ < 128 || (group_logs & 0xffu) == 0)) {
            qemu_log("nvkvm-gpu[%s] M8.101 UVM_CE_REPORT_GROUP %s "
                     "group=0x%llx sem=0x%llx aggregate=0x%llx "
                     "payload=%u redir=0x%llx count=%u\n",
                     s->chip->name, why ? why : "ce-sem",
                     (unsigned long long)group,
                     (unsigned long long)sem_addr,
                     (unsigned long long)(group + 0x30u), payload,
                     (unsigned long long)redir, group_logs);
        }
    }
}

static uint64_t nvkvm_chan_resolve(NvkvmGpuEmul *s, uint64_t va, bool *out_sys)
{
    uint64_t gpa = 0;
    if (nvkvm_m2_pbmap_lookup(s, va, 4, &gpa)) {
        *out_sys = true;
        return gpa;
    }
    if (nvkvm_m2_uvm_shadow_resolve(s, va, 4, &gpa, out_sys)) {
        return gpa;
    }
    uint64_t uvm_phys = 0;
    if (nvkvm_m2_uvm_ext_lookup(s, va, 4, &uvm_phys)) {
        *out_sys = false;
        return uvm_phys;
    }
    return nvkvm_chan_translate(s, va, out_sys);
}

static bool nvkvm_m2_pbmap_rd32(NvkvmGpuEmul *s, uint64_t va, uint32_t *out)
{
    uint64_t gpa = 0;
    if (!nvkvm_m2_pbmap_lookup(s, va, 4, &gpa)) {
        return false;
    }
    *out = nvkvm_phys_rd32(s, gpa, true);
    return true;
}

/* Read one 32-bit word at a CHANNEL GPU VA (pbmap bridge, then translate+phys read). */
static bool nvkvm_chan_rd32(NvkvmGpuEmul *s, uint64_t va, uint32_t *out)
{
    if (nvkvm_m2_pbmap_rd32(s, va, out)) {
        return true;
    }
    if (nvkvm_m2_uvm_shadow_rd32(s, va, out)) {
        return true;
    }
    uint64_t uvm_phys = 0;
    if (nvkvm_m2_uvm_ext_lookup(s, va, 4, &uvm_phys)) {
        *out = nvkvm_phys_rd32(s, uvm_phys, false);
        return true;
    }
    bool sys; uint64_t p = nvkvm_chan_translate(s, va, &sys);
    if (p == NVKVM_GMMU_FAULT) return false;
    *out = nvkvm_phys_rd32(s, p, sys);
    if (*out == 0 && !sys && s->m2exec) {
        for (int i = 0; i < s->chan_vas_n; i++) {
            bool asys = false;
            uint64_t ap = nvkvm_walk_pdb(s, s->chan_vas[i].pdb, va, &asys);
            if (ap == NVKVM_GMMU_FAULT || (ap == p && asys == sys)) {
                continue;
            }
            uint32_t av = nvkvm_phys_rd32(s, ap, asys);
            if (av != 0) {
                static uint32_t alias_logs;
                if (alias_logs++ < 256) {
                    qemu_log("nvkvm-gpu[%s] M8.47 CHAN_RD_ALIAS "
                             "VA=0x%llx primary=FB:0x%llx zero "
                             "alt[%d]=%s:0x%llx val=0x%08x\n",
                             s->chip->name, (unsigned long long)va,
                             (unsigned long long)p, i, asys ? "SYS" : "FB",
                             (unsigned long long)ap, av);
                }
                *out = av;
                break;
            }
        }
    }
    return true;
}

static void nvkvm_m2_trace_gpfifo_lookahead(NvkvmGpuEmul *s, uint32_t gp_put)
{
    static uint64_t last_key;
    static uint32_t log_cnt;

    if (!s->trace || !s->chan_gpfifo_ent) {
        return;
    }

    if (s->chan_gpfifo_va != 0x120064000ull) {
        return;
    }
    if (gp_put >= s->chan_gpfifo_ent || gp_put <= s->chan_gp_get) {
        return;
    }

    uint64_t key = s->chan_gpfifo_va ^
                   ((uint64_t)s->chan_gp_get << 16) ^
                   ((uint64_t)gp_put << 48);
    if (key == last_key || log_cnt++ >= 96) {
        return;
    }
    last_key = key;

    qemu_log("nvkvm-gpu[%s] M8.55 GPFIFO_LOOKAHEAD gpfifo=0x%llx "
             "phys=0x%llx gp_get=%u gp_put=%u ent=%u\n",
             s->chip->name, (unsigned long long)s->chan_gpfifo_va,
             (unsigned long long)s->chan_gpfifo_phys,
             s->chan_gp_get, gp_put, s->chan_gpfifo_ent);

    for (uint32_t a = 0; a < 8; a++) {
        uint32_t idx = gp_put + a;
        if (idx >= s->chan_gpfifo_ent) {
            break;
        }
        uint64_t off = (uint64_t)idx * 8;
        uint32_t e0 = 0, e1 = 0;
        bool eok = false;
        if (s->chan_gpfifo_phys && off + 8 <= 0x1000) {
            e0 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off, 4);
            e1 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off + 4, 4);
            eok = true;
        } else {
            uint64_t eva = s->chan_gpfifo_va + off;
            eok = nvkvm_chan_rd32(s, eva, &e0) &&
                  nvkvm_chan_rd32(s, eva + 4, &e1);
        }
        if (!eok) {
            qemu_log("nvkvm-gpu[%s] M8.55   ahead[%u] idx=%u entry FAULT\n",
                     s->chip->name, a, idx);
            continue;
        }
        if (!e0 && !e1) {
            if (a < 4) {
                qemu_log("nvkvm-gpu[%s] M8.55   ahead[%u] idx=%u empty\n",
                         s->chip->name, a, idx);
            }
            continue;
        }

        uint64_t pb = (uint64_t)(e0 & 0xfffffffc) |
                      ((uint64_t)(e1 & 0xffu) << 32);
        uint32_t pblen = (e1 >> 10) & 0x1fffffu;
        uint32_t w0 = 0;
        bool pbok = pb && pblen && pblen <= 0x40000u &&
                    nvkvm_chan_rd32(s, pb, &w0);
        uint64_t ce_addr = 0;
        uint32_t ce_pay = 0;
        uint32_t ce_launch = 0;

        if (pbok && pblen <= 256u) {
            for (uint32_t w = 0; w < pblen;) {
                uint32_t hdr = 0;
                if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
                    break;
                }
                w++;
                uint32_t secop = (hdr >> 29) & 0x7u;
                uint32_t cnt = (hdr >> 16) & 0x1fffu;
                uint32_t maddr = (hdr & 0xfffu) << 2;
                if (secop != 1u && secop != 3u && secop != 5u) {
                    continue;
                }
                if (!cnt || cnt > 0x400u || w + cnt > pblen) {
                    break;
                }
                for (uint32_t j = 0; j < cnt; j++, w++) {
                    uint32_t d = 0;
                    if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) {
                        w = pblen;
                        break;
                    }
                    uint32_t m = (secop == 3u) ? maddr : (maddr + j * 4u);
                    switch (m) {
                    case 0x240:
                        ce_addr = (ce_addr & 0xffffffffull) |
                                  ((uint64_t)(d & 0x01ffffffu) << 32);
                        break;
                    case 0x244:
                        ce_addr = (ce_addr & ~0xffffffffull) | d;
                        break;
                    case 0x248:
                        ce_pay = d;
                        break;
                    case 0x300:
                        if (((d >> 3) & 0x3u) != 0) {
                            ce_launch = d;
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        qemu_log("nvkvm-gpu[%s] M8.55   ahead[%u] idx=%u e0=0x%08x e1=0x%08x "
                 "pb=0x%llx pblen=%u pb_read=%s w0=0x%08x "
                 "ce_addr=0x%llx ce_pay=%u ce_launch=0x%08x\n",
                 s->chip->name, a, idx, e0, e1,
                 (unsigned long long)pb, pblen,
                 pbok ? "ok" : "no", w0,
                 (unsigned long long)ce_addr, ce_pay, ce_launch);
    }
}

static bool nvkvm_m2_gpfifo_pending_count(uint32_t gp_get, uint32_t gp_put,
                                           uint32_t entries, uint32_t *count)
{
    if (count) {
        *count = 0;
    }
    if (!entries || gp_get >= entries || gp_put >= entries) {
        return false;
    }
    if (gp_put == gp_get) {
        return false;
    }
    if (gp_put < gp_get && entries > NVKVM_M2_GPFIFO_LARGE_RING_ENTRIES) {
        return false;
    }
    if (count) {
        *count = (gp_put > gp_get) ? (gp_put - gp_get)
                                   : (entries - gp_get + gp_put);
    }
    return true;
}

static bool nvkvm_m2_gpfifo_entry_valid(uint32_t e0, uint32_t e1,
                                        uint64_t *pb, uint32_t *pblen)
{
    uint64_t entry_pb = (uint64_t)(e0 & 0xFFFFFFFCu) |
                        ((uint64_t)(e1 & 0xFFu) << 32);
    uint32_t entry_pblen = (e1 >> 10) & 0x1FFFFFu;

    if (pb) {
        *pb = entry_pb;
    }
    if (pblen) {
        *pblen = entry_pblen;
    }
    return entry_pb != 0 && entry_pblen != 0;
}

static void nvkvm_m2_map_gpfifo_span(NvkvmGpuEmul *s, uint32_t first_idx,
                                     uint32_t last_idx)
{
    if (!s->m2exec || !s->chan_gpfifo_phys || !s->chan_gpfifo_va ||
        first_idx >= last_idx) {
        return;
    }

    uint64_t first = (uint64_t)first_idx * 8u;
    uint64_t last = (uint64_t)last_idx * 8u;
    uint64_t cur = first & ~0xfffull;
    uint64_t end = (last + 0xfffull) & ~0xfffull;
    uint64_t ring_end = (uint64_t)s->chan_gpfifo_ent * 8u;
    if (end > ring_end) {
        end = (ring_end + 0xfffull) & ~0xfffull;
    }

    while (cur < end) {
        uint64_t need_va = s->chan_gpfifo_va + cur;
        uint64_t need_phys = s->chan_gpfifo_phys + cur;
        uint64_t next_64k = (need_va + 0x10000ull) & ~0xffffull;
        uint64_t chunk = end - cur;
        if (next_64k > need_va && chunk > next_64k - need_va) {
            chunk = next_64k - need_va;
        }
        chunk = (chunk + 0xfffull) & ~0xfffull;
        uint64_t map_va = need_va & ~0xffffull;
        uint64_t delta = need_va - map_va;
        uint64_t map_phys = (need_phys >= delta) ? need_phys - delta : need_phys;
        uint64_t map_sz = delta + chunk;
        bool overlay_ok = nvkvm_m2_fbback_covers(s, need_phys, chunk);
        bool va_seen = nvkvm_m2_va_is_seen(s, s->chan_client, map_va);
        if (!overlay_ok || !va_seen) {
            if (va_seen) {
                nvkvm_m2_va_forget(s, s->chan_client, map_va);
            }
            bool gok = nvkvm_m2_back_and_map(s, s->chan_client, map_va,
                                             map_phys, map_sz, true,
                                             "gpfifo-bridge");
            bool overlay_after = nvkvm_m2_fbback_covers(s, need_phys, chunk);
            if (gok && overlay_after) {
                (void)nvkvm_m2_va_seen(s, s->chan_client, map_va);
            }
            qemu_log("nvkvm-gpu[%s] M5.24 GPFIFO double-mmap va=0x%llx phys=0x%llx "
                     "need=0x%llx..0x%llx map_va=0x%llx map_phys=0x%llx "
                     "delta=0x%llx sz=0x%llx client=0x%08x seen=%d "
                     "overlay=%d/%d -> %s\n", s->chip->name,
                     (unsigned long long)s->chan_gpfifo_va,
                     (unsigned long long)s->chan_gpfifo_phys,
                     (unsigned long long)need_va,
                     (unsigned long long)(need_va + chunk),
                     (unsigned long long)map_va,
                     (unsigned long long)map_phys,
                     (unsigned long long)delta,
                     (unsigned long long)map_sz,
                     s->chan_client, va_seen, overlay_ok, overlay_after,
                     (gok && overlay_after) ?
                         "MAPPED (host channel fetches guest GP entries)" :
                         "map-FAILED");
        } else if (!va_seen) {
            (void)nvkvm_m2_va_seen(s, s->chan_client, map_va);
        }
        cur += chunk;
    }
}

static int nvkvm_m2_release_gr_report_sems(NvkvmGpuEmul *s, uint64_t pb,
                                           uint32_t pblen, const char *why)
{
    if (!pblen || pblen > 0x40000u) {
        return 0;
    }
    uint64_t cr_sem_addr = 0;
    uint32_t cr_sem_pay = 0;
    int released = 0;
    for (uint32_t w = 0; w < pblen;) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if (secop != 1u && secop != 3u && secop != 5u) {
            continue;
        }
        if (!cnt || cnt > 0x400u || w + cnt > pblen) {
            break;
        }
        for (uint32_t j = 0; j < cnt; j++, w++) {
            uint32_t d = 0;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) {
                return released;
            }
            uint32_t m = (secop == 3u) ? maddr : (maddr + j * 4u);
            switch (m) {
            case 0x1b00:
                cr_sem_addr = (cr_sem_addr & 0xffffffffull) |
                              ((uint64_t)(d & 0xffu) << 32);
                break;
            case 0x1b04:
                cr_sem_addr = (cr_sem_addr & ~0xffffffffull) | d;
                break;
            case 0x1b08:
                cr_sem_pay = d;
                break;
            case 0x1b0c:
                if ((d & 0x3u) == 0x0u && cr_sem_addr) {
                    bool one_word = (d >> 28) & 1u;
                    uint64_t redir = 0;
                    uint32_t before = 0, cur = 0, poll_us = 0;
                    bool have_real = nvkvm_m2_uvm_ext_peek32(s, cr_sem_addr, &before) ||
                                     nvkvm_chan_rd32(s, cr_sem_addr, &before);
                    cur = before;
                    for (int poll = 0; have_real && cur != cr_sem_pay && poll < 20; poll++) {
                        g_usleep(100);
                        poll_us += 100;
                        if (!nvkvm_m2_uvm_ext_peek32(s, cr_sem_addr, &cur) &&
                            !nvkvm_chan_rd32(s, cr_sem_addr, &cur)) {
                            have_real = false;
                            break;
                        }
                    }
                    static uint32_t real_probe_logs;
                    if (real_probe_logs++ < 256 || cur == cr_sem_pay) {
                        qemu_log("nvkvm-gpu[%s] M8.93 GR_REPORT_REAL_PROBE %s "
                                 "pb=0x%llx addr=0x%llx payload=%u "
                                 "before=0x%08x after=0x%08x poll_us=%u %s\n",
                                 s->chip->name, why ? why : "init",
                                 (unsigned long long)pb,
                                 (unsigned long long)cr_sem_addr, cr_sem_pay,
                                 before, cur, poll_us,
                                 have_real && cur == cr_sem_pay ? "HOST-WROTE" : "not-yet");
                    }
	                    if (nvkvm_chan_sem_wr32(s, cr_sem_addr, cr_sem_pay, &redir)) {
	                        if (!one_word) {
	                            nvkvm_chan_wr32(s, cr_sem_addr + 4, 0);
                            nvkvm_chan_wr32(s, cr_sem_addr + 8, 0);
                            nvkvm_chan_wr32(s, cr_sem_addr + 12, 0);
                            if (redir) {
                                nvkvm_fb_write(s, redir + 4, 0, 4);
                                nvkvm_fb_write(s, redir + 8, 0, 4);
                                nvkvm_fb_write(s, redir + 12, 0, 4);
	                            }
	                        }
	                        s->m2_gr_report_sem_addr = cr_sem_addr;
	                        s->m2_gr_report_sem_payload = cr_sem_pay;
	                        s->m2_gr_report_sem_one_word = one_word;
	                        released++;
	                        qemu_log("nvkvm-gpu[%s] M8.22 GR_REPORT_SEM %s "
	                                 "pb=0x%llx addr=0x%llx payload=%u redir=0x%llx\n",
                                 s->chip->name, why ? why : "init",
                                 (unsigned long long)pb,
                                 (unsigned long long)cr_sem_addr, cr_sem_pay,
                                 (unsigned long long)redir);
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    return released;
}

static bool nvkvm_m2_release_gr_implicit_progress(NvkvmGpuEmul *s,
                                                  uint32_t gp_put)
{
    if (!s->m2_gr_report_sem_addr || gp_put == 0) {
        return false;
    }
    if (s->m2_gr_report_sem_payload >= gp_put) {
        return false;
    }
    if (gp_put - s->m2_gr_report_sem_payload > 4u) {
        return false;
    }
    uint64_t redir = 0;
    if (!nvkvm_chan_sem_wr32(s, s->m2_gr_report_sem_addr, gp_put, &redir)) {
        return false;
    }
    if (!s->m2_gr_report_sem_one_word) {
        nvkvm_chan_wr32(s, s->m2_gr_report_sem_addr + 4, 0);
        nvkvm_chan_wr32(s, s->m2_gr_report_sem_addr + 8, 0);
        nvkvm_chan_wr32(s, s->m2_gr_report_sem_addr + 12, 0);
        if (redir) {
            nvkvm_fb_write(s, redir + 4, 0, 4);
            nvkvm_fb_write(s, redir + 8, 0, 4);
            nvkvm_fb_write(s, redir + 12, 0, 4);
        }
    }
    qemu_log("nvkvm-gpu[%s] M8.32 GR_REPORT_IMPLICIT "
             "addr=0x%llx payload=%u prev=%u redir=0x%llx\n",
             s->chip->name, (unsigned long long)s->m2_gr_report_sem_addr,
             gp_put, s->m2_gr_report_sem_payload, (unsigned long long)redir);
    s->m2_gr_report_sem_payload = gp_put;
    return true;
}

static int nvkvm_m2_release_gr_reports_for_gpfifo(NvkvmGpuEmul *s,
                                                  uint64_t gpf_phys,
                                                  uint32_t from,
                                                  uint32_t to,
                                                  uint32_t ent,
                                                  const char *why)
{
    int released = 0;
    if (!gpf_phys || from >= to || to > ent) {
        return 0;
    }
    for (uint32_t idx = from; idx < to; idx++) {
        uint64_t epa = gpf_phys + (uint64_t)idx * 8;
        uint32_t e0 = (uint32_t)nvkvm_fb_read(s, epa, 4);
        uint32_t e1 = (uint32_t)nvkvm_fb_read(s, epa + 4, 4);
        uint64_t pb = (uint64_t)(e0 & 0xFFFFFFFCu) |
                      ((uint64_t)(e1 & 0xFFu) << 32);
        uint32_t pblen = (e1 >> 10) & 0x1FFFFFu;
        if (!pb || !pblen) {
            continue;
        }
        released += nvkvm_m2_release_gr_report_sems(s, pb, pblen, why);
    }
    if (released) {
        qemu_log("nvkvm-gpu[%s] M8.35 GR_REPORT_HOST_RING "
                 "gp_get=%u->%u sems=%d\n",
                 s->chip->name, from, to, released);
    }
    return released;
}

static void nvkvm_m2_trace_gr_pushbuf(NvkvmGpuEmul *s, int ch_idx,
                                      uint32_t gpidx, uint64_t pb,
                                      uint32_t pblen, const char *why)
{
    if (!s->trace || !pb || pblen < 16 || pblen > 0x40000u) {
        return;
    }
    static uint32_t dump_cnt;
    if (dump_cnt++ >= 16) {
        return;
    }

    qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_RAW %s ch[%d] idx=%u "
             "pb=0x%llx words=%u\n",
             s->chip->name, why ? why : "mapped", ch_idx, gpidx,
             (unsigned long long)pb, pblen);
    for (uint32_t w = 0; w < pblen; w += 8) {
        uint32_t v[8] = {0};
        char ok[9] = "--------";
        for (uint32_t j = 0; j < 8 && w + j < pblen; j++) {
            if (nvkvm_chan_rd32(s, pb + (uint64_t)(w + j) * 4, &v[j])) {
                ok[j] = 'v';
            }
        }
        qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_RAW +0x%04x %c:%08x %c:%08x "
                 "%c:%08x %c:%08x %c:%08x %c:%08x %c:%08x %c:%08x\n",
                 s->chip->name, w,
                 ok[0], v[0], ok[1], v[1], ok[2], v[2], ok[3], v[3],
                 ok[4], v[4], ok[5], v[5], ok[6], v[6], ok[7], v[7]);
    }

    uint32_t groups = 0;
    for (uint32_t w = 0; w < pblen && groups < 256;) {
        uint32_t hdr = 0;
        uint32_t hw = w;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_METHOD fault hdrw=%u\n",
                     s->chip->name, hw);
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if (secop != 1u && secop != 3u && secop != 5u) {
            continue;
        }
        if (!cnt || cnt > 0x1000u || w + cnt > pblen) {
            qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_METHOD invalid hdrw=%u "
                     "hdr=0x%08x secop=%u m=0x%04x cnt=%u rem=%u\n",
                     s->chip->name, hw, hdr, secop, maddr, cnt, pblen - w);
            break;
        }
        groups++;
        qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_METHOD hdrw=%u hdr=0x%08x "
                 "secop=%u m=0x%04x cnt=%u\n",
                 s->chip->name, hw, hdr, secop, maddr, cnt);
        for (uint32_t j = 0; j < cnt; j++, w++) {
            uint32_t d = 0;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) {
                qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_DATA fault word=%u\n",
                         s->chip->name, w);
                return;
            }
            uint32_t m = (secop == 3u) ? maddr : (maddr + j * 4u);
            if (j < 8 || m == 0x1b00u || m == 0x1b04u ||
                m == 0x1b08u || m == 0x1b0cu ||
                (d >= 0x20000000u && d < 0xf0000000u)) {
                qemu_log("nvkvm-gpu[%s] M8.23 GR_LONG_DATA word=%u "
                         "m=0x%04x d=0x%08x\n",
                         s->chip->name, w, m, d);
            }
        }
    }
}

static void nvkvm_m2_trace_gr_inline_detail(NvkvmGpuEmul *s, uint64_t pb,
                                            uint32_t pblen,
                                            uint32_t inline_words,
                                            const char *why)
{
    if (!s->trace || !pb || !pblen || pblen > 0x40000u) {
        return;
    }
    static uint32_t detail_cnt;
    if (detail_cnt++ >= 64) {
        return;
    }

    uint32_t trailing = pblen > inline_words ? pblen - inline_words : 0;
    uint32_t raw_lim = pblen < 256u ? pblen : 256u;
    qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_DETAIL %s "
             "pb=0x%llx words=%u inline_words=%u trailing=%u "
             "dump_words=%u\n",
             s->chip->name, why ? why : "inline",
             (unsigned long long)pb, pblen, inline_words, trailing,
             raw_lim);

    for (uint32_t w = 0; w < raw_lim; w += 8) {
        uint32_t v[8] = {0};
        char ok[9] = "--------";
        for (uint32_t j = 0; j < 8 && w + j < raw_lim; j++) {
            if (nvkvm_chan_rd32(s, pb + (uint64_t)(w + j) * 4, &v[j])) {
                ok[j] = 'v';
            }
        }
        qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_RAW +0x%04x "
                 "%c:%08x %c:%08x %c:%08x %c:%08x "
                 "%c:%08x %c:%08x %c:%08x %c:%08x\n",
                 s->chip->name, w,
                 ok[0], v[0], ok[1], v[1], ok[2], v[2], ok[3], v[3],
                 ok[4], v[4], ok[5], v[5], ok[6], v[6], ok[7], v[7]);
    }

    uint32_t groups = 0;
    for (uint32_t w = 0; w < pblen && groups < 128;) {
        uint32_t hdr = 0;
        uint32_t hw = w;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_METHOD fault hdrw=%u\n",
                     s->chip->name, hw);
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if (secop != 1u && secop != 3u && secop != 5u) {
            continue;
        }
        if (!cnt || cnt > 0x1000u || w + cnt > pblen) {
            qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_METHOD invalid hdrw=%u "
                     "hdr=0x%08x secop=%u m=0x%04x cnt=%u rem=%u\n",
                     s->chip->name, hw, hdr, secop, maddr, cnt, pblen - w);
            break;
        }
        groups++;
        qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_METHOD hdrw=%u "
                 "hdr=0x%08x secop=%u m=0x%04x cnt=%u%s\n",
                 s->chip->name, hw, hdr, secop, maddr, cnt,
                 hw >= inline_words ? " TRAILING" : "");
        for (uint32_t j = 0; j < cnt; j++, w++) {
            uint32_t d = 0;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) {
                qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_DATA fault word=%u\n",
                         s->chip->name, w);
                return;
            }
            uint32_t m = (secop == 3u) ? maddr : (maddr + j * 4u);
            if (j < 12 || m == 0x1b00u || m == 0x1b04u ||
                m == 0x1b08u || m == 0x1b0cu || hw >= inline_words ||
                w >= inline_words) {
                qemu_log("nvkvm-gpu[%s] M8.33 GR_INLINE_DATA word=%u "
                         "m=0x%04x d=0x%08x%s\n",
                         s->chip->name, w, m, d,
                         w >= inline_words ? " TRAILING" : "");
            }
        }
    }
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
static void nvkvm_chan_execute(NvkvmGpuEmul *s, bool prepare_only)
{
    s->chan_unresolved = false;
    s->chan_gp_put_valid = false;
    if (!s->chan_gpfifo_va || !s->chan_userd || !s->chan_gpfifo_ent) {
        return;
    }
    uint32_t gp_put = s->chan_userd_sys
        ? nvkvm_phys_rd32(s, s->chan_userd + 0x8C, true)
        : (uint32_t)nvkvm_fb_read(s, s->chan_userd + 0x8C, 4);
    s->chan_gp_put_seen = gp_put;
    s->chan_gp_put_valid = true;
    qemu_log("nvkvm-gpu[%s] M5: chan_exec gpfifo=0x%llx userd=0x%llx(%s) "
             "gp_get=%u gp_put=%u ent=%u\n", s->chip->name,
             (unsigned long long)s->chan_gpfifo_va,
             (unsigned long long)s->chan_userd, s->chan_userd_sys ? "sys" : "fb",
             s->chan_gp_get, gp_put, s->chan_gpfifo_ent);
    uint32_t pending_count = 0;
    if (!nvkvm_m2_gpfifo_pending_count(s->chan_gp_get, gp_put,
                                       s->chan_gpfifo_ent, &pending_count)) {
        return;
    }
    bool gp_wrapped = gp_put < s->chan_gp_get;
    if (gp_wrapped) {
        static uint32_t wrap_logs;
        if (s->trace && wrap_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.56 GP_PUT_WRAP "
                     "gpfifo=0x%llx gp_get=%u gp_put=%u ent=%u pending=%u\n",
                     s->chip->name, (unsigned long long)s->chan_gpfifo_va,
                     s->chan_gp_get, gp_put, s->chan_gpfifo_ent, pending_count);
        }
    }
    nvkvm_m2_trace_gpfifo_lookahead(s, gp_put);
    /* Pick the channel's VAS by CONTENT, not by handle.  The instance block is
     * empty (GSP-managed) so it gives no PDB, and hVASpace=0 (device-default)
     * channels match no snooped VAS handle -> the try-all fallback picks a wrong
     * VAS that maps gpFifoVA to a stale/zero page.  Instead, among the snooped
     * VAS PDBs, choose the one under which the pending GPFIFO entry reads
     * NON-ZERO (a valid pushbuffer pointer) — that is the VAS that actually owns
     * this channel's ring.  Pin it in chan_pdb so every translate in this walk
     * (entry/pushbuffer/sema) uses the same correct VAS. */
    if (s->chan_pdb == 0) {
        uint64_t eva = s->chan_gpfifo_va + (uint64_t)s->chan_gp_get * 8;
        /* M5.21: prefer the channel's OWN client VAS — authoritative, avoids the
         * cross-client aliasing the content-pick below falls into. */
        uint64_t own = nvkvm_chan_own_pdb(s);
        if (own) {
            bool sy = false;
            uint64_t p = nvkvm_walk_pdb(s, own, eva, &sy);
            if (p != NVKVM_GMMU_FAULT && nvkvm_phys_rd32(s, p, sy) != 0) {
                s->chan_pdb = own;
            }
        }
        for (int i = 0; s->chan_pdb == 0 && i < s->chan_vas_n; i++) {
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
        /* M5.14: content-pick failed -> device-default-VAS channel (hVASpace=0), whose VAS is never
         * snooped into chan_vas[] (only explicit VASpace objects are). Derive the channel's PDB
         * from its INSTANCE BLOCK (RAMIN PAGE_DIR_BASE @0x200 LO / @0x204 HI) — the HW-authoritative
         * VAS root for THIS channel (libcuda's compute USERMODE channel gpfifo 0x121010000). Use it
         * only if it resolves the pending GPFIFO entry to a non-zero pushbuffer pointer. */
        if (s->chan_pdb == 0 && s->chan_inst_block) {
            bool isys = s->chan_inst_sys;
            uint32_t plo = isys ? nvkvm_phys_rd32(s, s->chan_inst_block + NVKVM_RAMIN_PDB_LO_OFF, true)
                                : (uint32_t)nvkvm_fb_read(s, s->chan_inst_block + NVKVM_RAMIN_PDB_LO_OFF, 4);
            uint32_t phi = isys ? nvkvm_phys_rd32(s, s->chan_inst_block + NVKVM_RAMIN_PDB_HI_OFF, true)
                                : (uint32_t)nvkvm_fb_read(s, s->chan_inst_block + NVKVM_RAMIN_PDB_HI_OFF, 4);
            uint64_t ipdb = ((uint64_t)phi << 32) | ((uint64_t)plo & 0xFFFFF000ull);
            if (ipdb) {
                bool sy = false;
                uint64_t p = nvkvm_walk_pdb(s, ipdb, eva, &sy);
                uint32_t v = (p != NVKVM_GMMU_FAULT) ? nvkvm_phys_rd32(s, p, sy) : 0;
                qemu_log("nvkvm-gpu[%s] M5.14: instblk=0x%llx(%s) PDB=0x%llx; gpfifo eva=0x%llx -> "
                         "%s val=0x%08x\n", s->chip->name, (unsigned long long)s->chan_inst_block,
                         isys ? "sys" : "fb", (unsigned long long)ipdb, (unsigned long long)eva,
                         (p == NVKVM_GMMU_FAULT) ? "FAULT" : (sy ? "SYS" : "FB"), v);
                if (p != NVKVM_GMMU_FAULT && v != 0) { s->chan_pdb = ipdb; }
            } else {
                qemu_log("nvkvm-gpu[%s] M5.14: instblk=0x%llx PDB empty (GSP-managed)\n",
                         s->chip->name, (unsigned long long)s->chan_inst_block);
            }
        }
        /* DIAG: when content-pick fails, show what EACH snooped VAS resolves the
         * GPFIFO entry VA to (fault / phys+aperture) and the value read there. */
        if (s->trace && s->chan_pdb == 0) {
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
    /* M5.16 — last-resort GPFIFO resolution for GSP-managed channel VASes.  When
     * the channel-VAS walk above failed to find a PDB under which the pending GP
     * entry reads non-zero (chan_pdb==0), the ring's leaf PTE isn't reliable in
     * our FB — but the guest's OWN CPU mapping wrote the entry through BAR1, and
     * that landed in our FB at the TRUE backing page (recorded in bar1_wpg).
     * Try each guest-written vidmem page (MRU first) as the GPFIFO base: if the
     * pending entry there decodes to a plausible pushbuffer (non-zero, sane len,
     * pb VA resolves), pin it as chan_gpfifo_phys so the entry read below reads
     * the real ring instead of the stale aliased page.  This is the data-plane
     * keystone: stop trusting the channel-VAS walk for GSP-managed rings. */
    if (prepare_only && s->chan_gpfifo_phys) {
        uint64_t off = (uint64_t)s->chan_gp_get * 8;
        if (off + 8 <= 0x1000) {
            uint32_t e0 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off, 4);
            uint32_t e1 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off + 4, 4);
            if (e0 == 0 && e1 == 0) {
                qemu_log("nvkvm-gpu[%s] M8.19 GR prepare: stale GPFIFO phys 0x%llx "
                         "entry[%u] empty; retry BAR1/userd-adjacent resolution\n",
                         s->chip->name, (unsigned long long)s->chan_gpfifo_phys,
                         s->chan_gp_get);
                s->chan_gpfifo_phys = 0;
            }
        }
    }
    if (s->chan_gpfifo_phys == 0) {
        bool visited[NVKVM_MAX_BAR1PG] = { false };
        uint64_t off = (uint64_t)s->chan_gp_get * 8;
        uint64_t eva = s->chan_gpfifo_va + off;
        uint32_t want_e0 = 0, want_e1 = 0;
        bool have_want = false;
        if (s->chan_pdb) {
            bool sy0 = false, sy1 = false;
            uint64_t p0 = nvkvm_walk_pdb(s, s->chan_pdb, eva, &sy0);
            uint64_t p1 = nvkvm_walk_pdb(s, s->chan_pdb, eva + 4, &sy1);
            if (p0 != NVKVM_GMMU_FAULT && p1 != NVKVM_GMMU_FAULT) {
                want_e0 = nvkvm_phys_rd32(s, p0, sy0);
                want_e1 = nvkvm_phys_rd32(s, p1, sy1);
                have_want = true;
            }
        }
        uint64_t hints[2]; int hint_n = 0;
        if (!s->chan_userd_sys) {
            if (s->chan_userd >= 0x10000) {
                hints[hint_n++] = s->chan_userd - 0x10000;
            }
            if (s->chan_userd >= 0x2000) {
                hints[hint_n++] = s->chan_userd - 0x2000;
            }
        }
        int hint_i = 0, scan = 0;
        while (off + 8 <= 0x1000) {
            int best = -1;
            uint64_t cand = 0, seq = 0;
            const char *src = "BAR1-written";
            bool userd_adj = false;
            if (hint_i < hint_n) {
                cand = hints[hint_i++];
                src = "USERD-adjacent";
                userd_adj = true;
                bool recorded = false;
                for (int i = 0; i < s->bar1_wpg_n; i++) {
                    if (s->bar1_wpg[i].page == cand) {
                        recorded = true;
                        if (s->bar1_wpg[i].seq > seq) { seq = s->bar1_wpg[i].seq; }
                    }
                }
                if (!recorded) { continue; }
            } else {
                if (scan++ >= s->bar1_wpg_n) { break; }
                /* pick MRU (highest seq) unvisited */
                for (int i = 0; i < s->bar1_wpg_n; i++) {
                    if (visited[i] || !s->bar1_wpg[i].page) { continue; }
                    if (best < 0 || s->bar1_wpg[i].seq > s->bar1_wpg[best].seq) { best = i; }
                }
                if (best < 0) { break; }
                visited[best] = true;
                cand = s->bar1_wpg[best].page;
                seq = s->bar1_wpg[best].seq;
            }
            uint32_t e0 = (uint32_t)nvkvm_fb_read(s, cand + off, 4);
            uint32_t e1 = (uint32_t)nvkvm_fb_read(s, cand + off + 4, 4);
            if (e0 == 0 && e1 == 0) { continue; }
            if (have_want && (e0 != want_e0 || e1 != want_e1)) { continue; }
            uint64_t pb = (uint64_t)(e0 & 0xFFFFFFFCu) | ((uint64_t)(e1 & 0xFFu) << 32);
            uint32_t pblen = (e1 >> 10) & 0x1FFFFFu;
            if (!pb || pblen == 0 || pblen > 0x40000) { continue; }
            /* The decoded pushbuffer must resolve to REAL content (a valid method
             * header), not just any non-faulting page.  chan_translate's try-all
             * fallback picks the FIRST VAS that maps pb — often a wrong aliasing
             * VAS whose page reads 0 (proven: vas[0] pdb 0x2efba5000 -> empty,
             * vs the channel's real device-default vas pdb 0x2efa4c000 -> the
             * pushbuffer 0x20016000).  So content-pick the VAS under which pb's
             * first word is non-zero and PIN it as chan_pdb — that VAS owns the
             * whole channel working set (pushbuffer + sema), so every subsequent
             * translate uses it.  Skip this GPFIFO candidate if NO VAS yields a
             * non-zero pb word (the page was a stale/foreign ring). */
            uint64_t pb_pdb = s->chan_pdb;
            if (pb_pdb) {
                bool sy = false;
                uint64_t pp = nvkvm_walk_pdb(s, pb_pdb, pb, &sy);
                if (pp == NVKVM_GMMU_FAULT || nvkvm_phys_rd32(s, pp, sy) == 0) {
                    pb_pdb = 0;
                }
            }
            /* M5.21: prefer the channel's OWN client VAS so pb (and the whole
             * working set) resolves through the right address space and mirrors
             * under the right client — not a foreign client's aliasing VAS. */
            uint64_t own = nvkvm_chan_own_pdb(s);
            if (pb_pdb == 0 && own) {
                bool sy = false;
                uint64_t pp = nvkvm_walk_pdb(s, own, pb, &sy);
                if (pp != NVKVM_GMMU_FAULT && nvkvm_phys_rd32(s, pp, sy) != 0) {
                    pb_pdb = own;
                }
            }
            for (int v = 0; pb_pdb == 0 && v < s->chan_vas_n; v++) {
                bool sy = false;
                uint64_t pp = nvkvm_walk_pdb(s, s->chan_vas[v].pdb, pb, &sy);
                if (pp == NVKVM_GMMU_FAULT) { continue; }
                if (nvkvm_phys_rd32(s, pp, sy) != 0) { pb_pdb = s->chan_vas[v].pdb; break; }
            }
            if (pb_pdb == 0 && !userd_adj) {
                continue;                    /* pb has no real backing in any VAS */
            }
            s->chan_gpfifo_phys = cand;
            if (s->chan_pdb == 0 && pb_pdb) {
                s->chan_pdb = pb_pdb;        /* pin the channel's true VAS for pb/sema */
            }
            qemu_log("nvkvm-gpu[%s] M5.16: GPFIFO resolved via %s page "
                     "FB 0x%llx (seq %llu) -> entry pb=0x%llx len=%u; pinned VAS "
                     "pdb=0x%llx mirror=%s\n",
                     s->chip->name, src, (unsigned long long)cand,
                     (unsigned long long)seq,
                     (unsigned long long)pb, pblen,
                     (unsigned long long)s->chan_pdb,
                     pb_pdb ? (have_want ? "matched existing PDB entry"
                                          : "VAS-walk gave wrong page")
                            : "accepted USERD-adjacent entry-only");
            break;
        }
    }
    if (gp_wrapped) {
        uint64_t off = (uint64_t)s->chan_gp_get * 8u;
        uint64_t eva = s->chan_gpfifo_va + off;
        uint32_t e0 = 0, e1 = 0;
        bool eok = false;
        if (s->chan_gpfifo_phys && off + 8 <= 0x1000) {
            e0 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off, 4);
            e1 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + off + 4, 4);
            eok = true;
        } else {
            eok = nvkvm_chan_rd32(s, eva, &e0) &&
                  nvkvm_chan_rd32(s, eva + 4, &e1);
        }
        if (eok && e0 == 0 && e1 == 0) {
            static uint32_t wrap_empty_logs;
            if (s->trace && wrap_empty_logs++ < 128) {
                qemu_log("nvkvm-gpu[%s] M8.56 GP_PUT_WRAP_EMPTY "
                         "gpfifo=0x%llx gp_get=%u gp_put=%u ent=%u; "
                         "advance local get to put\n",
                         s->chip->name, (unsigned long long)s->chan_gpfifo_va,
                         s->chan_gp_get, gp_put, s->chan_gpfifo_ent);
            }
            if (!prepare_only) {
                s->chan_gp_get = gp_put;
            }
            return;
        }
    }
    /* M5.24 GPFIFO double-mmap (host-channel bridge step 2): the host channel
     * expects its GPFIFO ring at gpFifoOffset (gpfifo_va) in its VAS — client-
     * allocated, NOT RM-allocated — but we never mapped it, so the rung host channel
     * fetched empty entries.  The guest wrote its GP entries (vidmem) via BAR1 to
     * chan_gpfifo_phys (M5.16-resolved).  back_and_map: alloc host GPU mem, seed-copy
     * the current entries, double-mmap at chan_gpfifo_phys (future guest BAR1 GP
     * writes land in host mem), and map_dma FIXED at gpfifo_va into the channel's VAS
     * (via the client grmapper — same VAS the host channel runs in, M5.20/M5.21).
     * Then the rung host channel fetches the REAL GP entries -> the pushbuffers
     * (already zero-copy-mapped, M5.19) -> runs + writes the completion.  Gated on
     * m2exec + a resolved GSP-managed ring (chan_gpfifo_phys); idempotent per VA. */
    if (s->m2exec && s->chan_gpfifo_phys && s->chan_gpfifo_va) {
        if (gp_wrapped) {
            nvkvm_m2_map_gpfifo_span(s, s->chan_gp_get, s->chan_gpfifo_ent);
            nvkvm_m2_map_gpfifo_span(s, 0, gp_put);
        } else {
            nvkvm_m2_map_gpfifo_span(s, s->chan_gp_get, gp_put);
        }
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
    /* COMPUTE/3D-class (NVC7C0 AMPERE_COMPUTE_B etc) report-semaphore release.
     * cuCtxCreate's compute init work releases a dedicated completion semaphore
     * via SET_REPORT_SEMAPHORE_A/B(addr)+C(payload)+D(trigger,OPERATION=RELEASE);
     * libcuda's blocking-sync poll spins on THAT semaphore.  The parser
     * previously only honored CE + NVC56F host releases, so the compute
     * completion sema was never written and the wait hung (the dataless os-event
     * wake fired but the decisive bit stayed 0).  ADDR_UPPER[7:0]@0x1b00,
     * ADDR_LOWER[31:0]@0x1b04, PAYLOAD@0x1b08, D@0x1b0c. */
    uint64_t cr_sem_addr = 0;
    uint32_t cr_sem_pay = 0;
    s->chan_sem_released = false;
    uint32_t guard = 0;
    for (uint32_t idx = s->chan_gp_get; idx != gp_put &&
         guard < s->chan_gpfifo_ent; idx = (idx + 1) % s->chan_gpfifo_ent, guard++) {
        uint32_t e0, e1;
        uint64_t eva = s->chan_gpfifo_va + (uint64_t)idx * 8;
        if (s->chan_gpfifo_phys && (uint64_t)idx * 8 + 8 <= 0x1000) {
            /* M5.16: read the entry from the BAR1-resolved true ring page (the
             * channel-VAS walk gives a stale page for GSP-managed VASes). */
            e0 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + (uint64_t)idx * 8, 4);
            e1 = (uint32_t)nvkvm_fb_read(s, s->chan_gpfifo_phys + (uint64_t)idx * 8 + 4, 4);
        } else if (!nvkvm_chan_rd32(s, eva, &e0) || !nvkvm_chan_rd32(s, eva + 4, &e1)) {
            qemu_log("nvkvm-gpu[%s] M5: chan_exec GPFIFO entry[%u] @VA 0x%llx "
                     "FAULTED (no VAS maps it)\n", s->chip->name, idx,
                     (unsigned long long)eva);
            break;
        }
        uint64_t pb = 0;
        uint32_t pblen = 0;   /* GP_ENTRY1_LENGTH: # method words */
        if (!nvkvm_m2_gpfifo_entry_valid(e0, e1, &pb, &pblen)) {
            static uint32_t empty_entry_logs;

            if (gp_wrapped && guard > 0) {
                static uint32_t wrap_tail_empty_logs;
                if (s->trace &&
                    wrap_tail_empty_logs++ < NVKVM_M2_GPFIFO_EMPTY_LOG_LIMIT) {
                    qemu_log("nvkvm-gpu[%s] M8.102 GPFIFO wrap-tail empty "
                             "gpfifo=0x%llx idx=%u gp_get=%u gp_put=%u "
                             "phys=0x%llx e0=0x%08x e1=0x%08x; advance to "
                             "wrapped put\n",
                             s->chip->name,
                             (unsigned long long)s->chan_gpfifo_va, idx,
                             s->chan_gp_get, gp_put,
                             (unsigned long long)s->chan_gpfifo_phys, e0, e1);
                }
                if (!prepare_only) {
                    s->chan_gp_get = gp_put;
                }
                return;
            }
            s->chan_unresolved = true;
            if (s->trace &&
                empty_entry_logs++ < NVKVM_M2_GPFIFO_EMPTY_LOG_LIMIT) {
                qemu_log("nvkvm-gpu[%s] M8.102 GPFIFO empty/unbacked entry "
                         "gpfifo=0x%llx idx=%u gp_get=%u gp_put=%u "
                         "phys=0x%llx e0=0x%08x e1=0x%08x; keep GP_GET "
                         "retryable\n",
                         s->chip->name,
                         (unsigned long long)s->chan_gpfifo_va, idx,
                         s->chan_gp_get, gp_put,
                         (unsigned long long)s->chan_gpfifo_phys, e0, e1);
            }
            return;
        }
        /* M5.19 — REAL forward prep: make the host GPU able to read this pushbuffer
         * DIRECTLY from guest sysmem.  The pushbuffer is SYSMEM (resolved via the
         * pinned chan_pdb -> SYS guest GPA).  Map guest VA -> GPA -> shared-memfd
         * stub VA -> OS_DESCRIPTOR(COHERENCY_CACHED=WB) -> FIXED-map at the matching
         * VA in the host GR VAS.  The host channel (shadow_fwd, same handles) then
         * reads the exact bytes the guest wrote — no trap, WB-coherent.  Gated on
         * m2exec; idempotent (m2_va_seen). */
        if (s->m2exec && pb && pblen) {
            bool psy = false; uint64_t pgpa = nvkvm_chan_translate(s, pb, &psy);
            if (pgpa != NVKVM_GMMU_FAULT && psy) {        /* sysmem pushbuffer only */
                uint64_t pbbase = pb & ~0xfffull;
                uint64_t gbase  = pgpa - (pb - pbbase);   /* GPA of the page base */
                uint64_t msz    = (((pb + (uint64_t)pblen * 4) - pbbase) + 0xfffull) & ~0xfffull;
                if (!nvkvm_m2_va_seen(s, s->chan_client, pbbase)) {
                    bool mok = nvkvm_m2_back_and_map_sys(s, s->chan_client, pbbase, gbase, msz);
                    qemu_log("nvkvm-gpu[%s] M5.19 fwd-map pushbuffer VA=0x%llx gpa=0x%llx "
                             "sz=0x%llx client=0x%08x -> %s\n", s->chip->name,
                             (unsigned long long)pbbase, (unsigned long long)gbase,
                             (unsigned long long)msz, s->chan_client,
                             mok ? "MAPPED (host GPU reads guest sysmem, WB)" : "map-FAILED");
                }
            }
        }
        uint32_t w0 = 0;
        bool pbok = nvkvm_chan_rd32(s, pb, &w0);
        qemu_log("nvkvm-gpu[%s] M5: chan_exec entry[%u] pb=0x%llx pblen=%u "
                 "pb_read=%s w0=0x%08x\n", s->chip->name, idx,
                 (unsigned long long)pb, pblen, pbok ? "ok" : "FAULT", w0);
        if (prepare_only) {
            s->chan_unresolved = true;
            return;
        }
        /* M5.17 DIAG: when the userspace pushbuffer's first word reads 0 (no valid
         * method header), the VAS-selection picked a wrong/aliasing page.  Dump how
         * pb resolves under EACH snooped VAS (phys+aperture+value), and the SYS read
         * at the same numeric addr — to find which VAS owns the compute pushbuffer
         * and whether it's a sysmem-aperture miss.  Capped one-shot. */
        if (s->trace && w0 == 0) {
            static uint32_t pbd;
            if (pbd++ < 12) {
                for (int v = 0; v < s->chan_vas_n; v++) {
                    bool sy = false;
                    uint64_t pp = nvkvm_walk_pdb(s, s->chan_vas[v].pdb, pb, &sy);
                    if (pp == NVKVM_GMMU_FAULT) {
                        qemu_log("nvkvm-gpu[%s] M5.17 pb=0x%llx vas[%d] hvas=0x%08x "
                                 "pdb=0x%llx -> FAULT\n", s->chip->name,
                                 (unsigned long long)pb, v, s->chan_vas[v].hvas,
                                 (unsigned long long)s->chan_vas[v].pdb);
                    } else {
                        qemu_log("nvkvm-gpu[%s] M5.17 pb=0x%llx vas[%d] hvas=0x%08x "
                                 "pdb=0x%llx -> %s phys=0x%llx fbval=0x%08x sysval=0x%08x\n",
                                 s->chip->name, (unsigned long long)pb, v,
                                 s->chan_vas[v].hvas, (unsigned long long)s->chan_vas[v].pdb,
                                 sy ? "SYS" : "FB", (unsigned long long)pp,
                                 (uint32_t)nvkvm_fb_read(s, pp, 4),
                                 nvkvm_phys_rd32(s, pp, true));
                    }
                }
            }
        }
        if (pb && pblen && w0 == 0 &&
            pb >= 0x200000000ull && pb < 0x300000000ull &&
            s->chan_client == s->m2_gr_client) {
            uint64_t sz = ((pb & 0xfffull) + (uint64_t)pblen * 4 + 0xfffull) & ~0xfffull;
            uint64_t pbgpa = 0;
            bool have = nvkvm_m2_pbmap_lookup(s, pb & ~0xfffull, sz, &pbgpa);
            s->chan_unresolved = true;
            qemu_log("nvkvm-gpu[%s] M8.11 user-mmap pushbuf pending ch_client=0x%08x "
                     "pb=0x%llx sz=0x%llx %s%s0x%llx; keep GP_GET=%u retryable\n",
                     s->chip->name, s->chan_client, (unsigned long long)(pb & ~0xfffull),
                     (unsigned long long)sz, have ? "GPA=" : "needs m2pbmap ",
                     have ? "" : "", (unsigned long long)(have ? pbgpa : 0),
                     s->chan_gp_get);
            return;
        }
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
                /* M5.22 (a) INSTRUMENT: dump every decoded method so we can see
                 * EXACTLY what cuCtxCreate submits (esp. any completion-signalling
                 * method the parser doesn't yet honor, and the compute QMD launch).
                 * Capped + trace-gated; compiled in for bring-up. */
                if (s->trace) {
                    static uint32_t m22n;
                    if (m22n++ < 1200) {
                        qemu_log("nvkvm-gpu[%s] M5.22 method client=0x%08x gpfifo=0x%llx "
                                 "m=0x%04x d=0x%08x\n", s->chip->name, s->chan_client,
                                 (unsigned long long)s->chan_gpfifo_va, m, d);
                    }
                }
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
                    bool sem_only = sem_type != 0 && !remap && !mscrub &&
                                    ((d & 0x00000180u) == 0);
                    uint64_t bytes = (uint64_t)llen * lcount;
                    if (bytes > (16u << 20)) bytes = 16u << 20;  /* safety cap */
                    bool wrote_dst = false;
                    if (s->trace) {
                        static uint32_t ce_launch_logs;
                        bool high_va = off_in >= 0x700000000000ull ||
                                       off_out >= 0x700000000000ull;
                        if ((high_va || off_in == 0) && ce_launch_logs++ < 256) {
                            qemu_log("nvkvm-gpu[%s] M8.16 CE LAUNCH d=0x%08x "
                                     "remap=%u mscrub=%u sem=%u src_phys=%u dst_phys=%u "
                                     "src_pm=%u dst_pm=%u in=0x%llx out=0x%llx "
                                     "llen=0x%x lcount=0x%x bytes=%llu remapA=0x%08x\n",
                                     s->chip->name, d, remap, mscrub, sem_type,
                                     src_phys, dst_phys, src_pm, dst_pm,
                                     (unsigned long long)off_in,
                                     (unsigned long long)off_out, llen, lcount,
                                     (unsigned long long)bytes, remapA);
                        }
                    }
                    /* Resolve a CE address: PHYSICAL -> the offset IS the phys addr
                     * (aperture from PHYS_MODE: 0=FB else sysmem); VIRTUAL ->
                     * translate via the channel VAS (leaf PTE picks FB/sys). */
                    #define NVKVM_CE_RESOLVE(off, phys, pm, sysv) \
                        ((phys) ? ((sysv) = ((pm) != 0), (off)) \
                                : nvkvm_chan_resolve(s, (off), &(sysv)))
                    if (sem_only) {
                        /* LAUNCH_DMA can be a pure completion-semaphore release.
                         * Do not reuse the previous copy/fill addresses and length. */
                    } else if (mscrub) {
                        /* MEMORY_SCRUB: zero the dst region.  Our FB backing is
                         * sparse-zero (unwritten reads return 0), so the data
                         * write is a no-op; the completion semaphore below is
                         * what unblocks the CeUtils scrubber.  No src is set. */
                    } else if (remap) {
                        for (uint64_t b = 0; b + 4 <= bytes; b += 4) {
                            bool sy; uint64_t p = NVKVM_CE_RESOLVE(off_out + b, dst_phys, dst_pm, sy);
                            if (p == NVKVM_GMMU_FAULT) break;
                            nvkvm_phys_wr32(s, p, sy, remapA);
                            wrote_dst = true;
                        }
                    } else {
                        for (uint64_t b = 0; b + 4 <= bytes; b += 4) {
                            bool ssy, dsy;
                            uint64_t sp = NVKVM_CE_RESOLVE(off_in + b,  src_phys, src_pm, ssy);
                            uint64_t dp = NVKVM_CE_RESOLVE(off_out + b, dst_phys, dst_pm, dsy);
                            if (sp == NVKVM_GMMU_FAULT || dp == NVKVM_GMMU_FAULT) {
                                bool zero_src_uvm = !src_phys && !dst_phys &&
                                                    off_in == 0 &&
                                                    (off_out + b) >= 0x700000000000ull;
                                if (zero_src_uvm && sp == NVKVM_GMMU_FAULT &&
                                    dp != NVKVM_GMMU_FAULT) {
                                    static uint32_t ce_zero_src_logs;
                                    nvkvm_phys_wr32(s, dp, dsy, 0);
                                    wrote_dst = true;
                                    if (s->trace && ce_zero_src_logs++ < 128) {
                                        qemu_log("nvkvm-gpu[%s] M8.16 CE ZERO-SRC "
                                                 "out=0x%llx->%s phys=0x%llx "
                                                 "bytes=%llu d=0x%08x\n",
                                                 s->chip->name,
                                                 (unsigned long long)(off_out + b),
                                                 dsy ? "sys" : "fb",
                                                 (unsigned long long)dp,
                                                 (unsigned long long)bytes, d);
                                    }
                                    continue;
                                }
                                if (s->trace) {
                                    static uint32_t ce_fault_logs, ce_high_fault_logs;
                                    bool high_va = (off_in + b) >= 0x700000000000ull ||
                                                   (off_out + b) >= 0x700000000000ull;
                                    bool log_fault = high_va
                                        ? ce_high_fault_logs++ < 128
                                        : ce_fault_logs++ < 64;
                                    if (log_fault) {
                                        qemu_log("nvkvm-gpu[%s] M8.13 CE COPY resolve fault "
                                                 "in=0x%llx->%s phys=0x%llx "
                                                 "out=0x%llx->%s phys=0x%llx bytes=%llu "
                                                 "src_phys=%u dst_phys=%u src_pm=%u dst_pm=%u\n",
                                                 s->chip->name,
                                                 (unsigned long long)(off_in + b),
                                                 sp == NVKVM_GMMU_FAULT ? "FAULT" : (ssy ? "sys" : "fb"),
                                                 (unsigned long long)sp,
                                                 (unsigned long long)(off_out + b),
                                                 dp == NVKVM_GMMU_FAULT ? "FAULT" : (dsy ? "sys" : "fb"),
                                                 (unsigned long long)dp,
                                                 (unsigned long long)bytes,
                                                 src_phys, dst_phys, src_pm, dst_pm);
                                    }
                                }
                                break;
                            }
                            uint32_t v = nvkvm_phys_rd32(s, sp, ssy);
                            nvkvm_phys_wr32(s, dp, dsy, v);
                            wrote_dst = true;
                            if (b == 0) {
                                static uint32_t ce_copy0_logs;

                                if (s->trace && ce_copy0_logs++ < 256) {
                                    qemu_log("nvkvm-gpu[%s] M5:   COPY[0] src "
                                             "0x%llx(%s)=0x%08x -> dst 0x%llx(%s)\n",
                                             s->chip->name,
                                             (unsigned long long)sp, ssy ? "sys" : "fb",
                                             v, (unsigned long long)dp,
                                             dsy ? "sys" : "fb");
                                }
                            }
                        }
                    }
                    if (wrote_dst && !dst_phys && bytes &&
                        nvkvm_m2_uvm_ext_find(s, off_out, bytes) >= 0) {
                        uint64_t sync_end = off_out + bytes;
                        if (sync_end >= off_out) {
                            nvkvm_m2_uvm_ext_sync_to_pbmap(s, off_out, sync_end,
                                                           "ce-copy");
                        }
                    }
                    #undef NVKVM_CE_RESOLVE
                    {
                        static uint32_t ce_summary_logs;

                        if (s->trace && ce_summary_logs++ < 512) {
                            qemu_log("nvkvm-gpu[%s] M5: CE %s in=0x%llx(%s) "
                                     "out=0x%llx(%s) bytes=%llu const=0x%x\n",
                                     s->chip->name,
                                     sem_only ? "SEM_ONLY" : mscrub ? "SCRUB" :
                                     remap ? "MEMSET" : "COPY",
                                     (unsigned long long)off_in,
                                     src_phys ? "phys" : "virt",
                                     (unsigned long long)off_out,
                                     dst_phys ? "phys" : "virt",
                                     (unsigned long long)bytes, remapA);
                        }
                    }
                    /* CE-class completion semaphore release: LAUNCH_DMA with
                     * SEMAPHORE_TYPE != NONE writes ce_sem_pay to
                     * (pbGpuVA+finishPayloadOffset).  This is what the CeUtils
                     * scrubber's channelWaitForFinishPayload polls — the fast-
                     * scrub pushbuffer ALSO emits an NVC56F SEM_EXECUTE (host
                     * sema at semaOffset), so honoring only that left this one
                     * unwritten and the scrubber timed out (ce_utils.c:349). */
                    if (sem_type != 0 && ce_sem_addr) {
                        uint64_t redir = 0;                    /* M5.18: also write the BAR1 page libcuda polls */
                        if (nvkvm_chan_sem_wr32(s, ce_sem_addr, ce_sem_pay, &redir)) {
                            nvkvm_m2_mirror_kernel_ce_progress(s, ce_sem_addr,
                                                               ce_sem_pay, &redir);
                            nvkvm_m2_write_channel_notify_block(s, ce_sem_pay,
                                                                 "ce-sem", NULL);
                            s->chan_sem_released = true;
                            {
                                static uint32_t ce_sem_logs;
                                uint32_t log_idx = ce_sem_logs++;
                                bool uvm_report =
                                    ce_sem_addr >= 0x20440ff00ull &&
                                    ce_sem_addr < 0x204410000ull;

                                if (s->trace && (uvm_report || log_idx < 512 ||
                                                 (log_idx & 0x3ffu) == 0)) {
                                    qemu_log("nvkvm-gpu[%s] M5: CE_SEM_RELEASE "
                                             "addr=0x%llx payload=%u redir=0x%llx "
                                             "count=%u\n",
                                             s->chip->name,
                                             (unsigned long long)ce_sem_addr, ce_sem_pay,
                                             (unsigned long long)redir, log_idx + 1);
                                }
                            }
                            nvkvm_m2_release_uvm_ce_report_group(s, ce_sem_addr,
                                                                 ce_sem_pay,
                                                                 "ce-sem");
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
                        bool sz64 = (d >> 24) & 1;           /* PAYLOAD_SIZE: 0=16B(64-bit val), 1=4B */
                        uint64_t redir = 0;                  /* M5.18: also write the BAR1 page libcuda polls */
                        if (nvkvm_chan_sem_wr32(s, sem_addr, sem_pay_lo, &redir)) {
                            if (!sz64) {                     /* 64-bit value: high word too */
                                nvkvm_chan_wr32(s, sem_addr + 4, sem_pay_hi);
                                if (redir) { nvkvm_fb_write(s, redir + 4, sem_pay_hi, 4); }
                            }
                            nvkvm_m2_write_channel_notify_block(s, sem_pay_lo,
                                                                 "host-sem", NULL);
                            s->chan_sem_released = true;
                            {
                                static uint32_t host_sem_logs;

                                if (s->trace && host_sem_logs++ < 256) {
                                    qemu_log("nvkvm-gpu[%s] M5: SEM_RELEASE "
                                             "addr=0x%llx payload=%u redir=0x%llx\n",
                                             s->chip->name,
                                             (unsigned long long)sem_addr, sem_pay_lo,
                                             (unsigned long long)redir);
                                }
                            }
                        }
                    }
                    break;
                }
                /* COMPUTE/3D-class (NVC7C0+) report-semaphore release. */
                case 0x1b00: cr_sem_addr = (cr_sem_addr & 0xFFFFFFFFull) | ((uint64_t)(d & 0xFFu) << 32); break; /* ADDR_UPPER */
                case 0x1b04: cr_sem_addr = (cr_sem_addr & ~0xFFFFFFFFull) | d; break;                          /* ADDR_LOWER */
                case 0x1b08: cr_sem_pay = d; break;                                                            /* PAYLOAD */
                case 0x1b0c: {                                                                                 /* D: trigger */
                    if ((d & 0x3u) == 0x0u && cr_sem_addr) {   /* OPERATION == RELEASE */
                        bool one_word = (d >> 28) & 1;         /* STRUCTURE_SIZE: 1=ONE_WORD(4B), 0=FOUR_WORDS(16B w/ ts) */
                        uint64_t redir = 0;                    /* M5.18: also write the BAR1 page libcuda polls */
                        bool ok = nvkvm_chan_sem_wr32(s, cr_sem_addr, cr_sem_pay, &redir);
                        if (ok) {
                            if (!one_word) {                   /* 4-word: also zero the 12B timestamp */
                                nvkvm_chan_wr32(s, cr_sem_addr + 4, 0);
                                nvkvm_chan_wr32(s, cr_sem_addr + 8, 0);
                                nvkvm_chan_wr32(s, cr_sem_addr + 12, 0);
                                if (redir) { nvkvm_fb_write(s, redir + 4, 0, 4);
                                             nvkvm_fb_write(s, redir + 8, 0, 4);
                                             nvkvm_fb_write(s, redir + 12, 0, 4); }
                            }
                            nvkvm_m2_write_channel_notify_block(s, cr_sem_pay,
                                                                 "compute-report", NULL);
                            s->chan_sem_released = true;
                            {
                                static uint32_t compute_sem_logs;

                                if (s->trace && compute_sem_logs++ < 256) {
                                    qemu_log("nvkvm-gpu[%s] M5: COMPUTE_REPORT_SEM "
                                             "addr=0x%llx payload=%u redir=0x%llx "
                                             "awaken=%d\n",
                                             s->chip->name,
                                             (unsigned long long)cr_sem_addr, cr_sem_pay,
                                             (unsigned long long)redir,
                                             (int)((d >> 20) & 1));
                                }
                            }
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
    uint64_t old_gva = s->m2_cur_gva;
    if (s->m2_crashwin) {
        s->m2_cur_gva = off;
    }
    uint64_t rv = nvkvm_fb_read(s, pa, size);
    if (s->m2_crashwin) {
        static uint32_t b2cnt;
        if (b2cnt++ < 400) {
            qemu_log("nvkvm-gpu[GA106] M8.8 BAR2 RD off=0x%llx -> FB 0x%llx "
                     "= 0x%llx sz=%u%s\n", (unsigned long long)off,
                     (unsigned long long)pa, (unsigned long long)rv, size,
                     s->bar2_virtual ? "" : " [PHYS]");
        }
        s->m2_cur_gva = old_gva;
    }
    return rv;
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

/* M5.1: lazily stand up the per-guest host isolate + open the real GPU control
 * and device nodes. Persists for the guest's lifetime (the host-side context the
 * forwarded RM stream builds on). Returns true once ready; on failure disables
 * forwarding cleanly so the guest keeps running on the faked path. */
/* M6.1 (item-4 step 2): share the guest-RAM memfd into the stub and MAP_FIXED it at a
 * reserved stub VA window, so the host nvidia driver (stub process) can address any guest
 * GPA — the prerequisite for OS_DESCRIPTOR-registering the guest's sysmem GR buffers (item-4
 * step 3) so the host GPU DMA-fills them. Reuses the Mode-1 handle_table + send_handle +
 * isolate_mmap (the user's foreseen handle-reuse, [[mode2-per-proc-isolate-handle-reuse]]).
 * One-shot; idempotent. */
#define NVKVM_M2_STUB_RAMWIN 0x7e0000000000ull   /* 126 TiB: free in the fresh stub process */
static bool nvkvm_m2_share_guest_ram(NvkvmGpuEmul *s)
{
    if (s->m2_ram_shared) {
        return true;
    }
    if (s->m2_guest_ram_fd < 0 || s->m2_guest_ram_size == 0) {
        qemu_log("nvkvm-gpu[%s] M6.1 share guest-RAM: no memfd (fd=%d) — need "
                 "memory-backend-memfd,share=on\n", s->chip->name, s->m2_guest_ram_fd);
        return false;
    }
    /* The stub keys handles in ONE id space shared with the isolate's device handles
     * (ctl=1, gpu=2) and the data-plane map fds (m2_maph_next, 16+). Seed a high id so the
     * guest-RAM handle can't collide (id 1 clobbered the stub's /dev/nvidiactl -> ENOTTY). */
    s->m2_ht.next_id = 0x8000u;
    uint32_t hid = 0;
    if (nvkvm_handle_alloc_pending(&s->m2_ht, 1 /*session*/, 0, &hid) != 0) {
        qemu_log("nvkvm-gpu[%s] M6.1 share guest-RAM: alloc_pending failed\n", s->chip->name);
        return false;
    }
    if (nvkvm_handle_attach_fd(&s->m2_ht, hid, s->m2_guest_ram_fd) != 0) {
        nvkvm_handle_abort_open(&s->m2_ht, hid);
        qemu_log("nvkvm-gpu[%s] M6.1 share guest-RAM: attach_fd failed\n", s->chip->name);
        return false;
    }
    int sr = nvkvm_isolate_send_handle(&s->m2_iso, &s->m2_ht, s->m2_iso_id, hid);
    int mr = nvkvm_isolate_mmap(&s->m2_iso, s->m2_iso_id, hid, NVKVM_M2_STUB_RAMWIN,
                                s->m2_guest_ram_size, 0, PROT_READ | PROT_WRITE, MAP_SHARED);
    s->m2_guest_ram_handle = hid;
    s->m2_stub_ram_base    = NVKVM_M2_STUB_RAMWIN;
    s->m2_ram_shared       = (sr == 0 && mr == 0);
    qemu_log("nvkvm-gpu[%s] M6.1 share guest-RAM: handle=%u send=%d mmap=%d stub_base=0x%llx "
             "size=0x%llx -> %s\n", s->chip->name, hid, sr, mr,
             (unsigned long long)NVKVM_M2_STUB_RAMWIN,
             (unsigned long long)s->m2_guest_ram_size,
             s->m2_ram_shared ? "SHARED (stub can address guest RAM)" : "FAILED");
    return s->m2_ram_shared;
}

static bool nvkvm_m2_iso_ensure(NvkvmGpuEmul *s)
{
    if (s->m2_iso_ready) {
        return true;
    }
    nvkvm_isolate_table_init(&s->m2_iso);
    s->m2_iso.abi_profile = NVKVM_ABI_580;   /* host is 580.159.04 */
    uint32_t id = 0;
    if (nvkvm_isolate_create(&s->m2_iso, 1 /*session*/, NULL, &id) != 0 || id == 0) {
        qemu_log("nvkvm-gpu[%s] M5.1: isolate_create FAILED — forwarding OFF\n",
                 s->chip->name);
        s->m2fwd = false;
        return false;
    }
    int ctlfd = -1, gpufd = -1;
    int r1 = nvkvm_isolate_open_device(&s->m2_iso, id, 1, NVKVM_DEV_CTL,    O_RDWR, &ctlfd);
    int r2 = nvkvm_isolate_open_device(&s->m2_iso, id, 2, NVKVM_DEV_GPU(0), O_RDWR, &gpufd);
    s->m2_gpu_fd = gpufd;   /* the stub's /dev/nvidia0 fd in QEMU's process (SCM_RIGHTS) */
    if (r1 != 0 || r2 != 0) {
        qemu_log("nvkvm-gpu[%s] M5.1: open ctl/gpu FAILED r1=%d r2=%d — forwarding OFF\n",
                 s->chip->name, r1, r2);
        nvkvm_isolate_kill(&s->m2_iso, id);
        s->m2fwd = false;
        return false;
    }
    s->m2_iso_id = id; s->m2_ctl_h = 1; s->m2_gpu_h = 2; s->m2_iso_ready = true;
    s->m2_cur_cvas = -1;        /* M5.28: no per-channel VAS active until the doorbell loop sets it */
    qemu_log("nvkvm-gpu[%s] M5.1: host isolate %u ready (pid=%d, ctl+gpu0 open)\n",
             s->chip->name, id, (int)nvkvm_isolate_host_pid(&s->m2_iso, id));
    /* M6.1 (item-4 step 2): share guest RAM into the stub so it can OS_DESCRIPTOR guest GPAs. */
    nvkvm_m2_share_guest_ram(s);
    return true;
}

/* M5.1b: map a guest RM client handle -> a host (synthetic, non-colliding)
 * handle, minting a fresh 0xdeadNNNN on first sight. Host clients live in
 * 0xc1xxxxxx, so 0xdeadNNNN never collides. */
static uint32_t nvkvm_m2_client(NvkvmGpuEmul *s, uint32_t g)
{
    for (int i = 0; i < s->m2_cmap_n; i++) {
        if (s->m2_cmap[i].g == g) {
            return s->m2_cmap[i].h;
        }
    }
    if (s->m2_cmap_n >= (int)(sizeof(s->m2_cmap) / sizeof(s->m2_cmap[0]))) {
        return g;                        /* table full -> verbatim (may collide) */
    }
    uint32_t h = 0xdead0001u + (uint32_t)s->m2_cmap_n;
    s->m2_cmap[s->m2_cmap_n].g = g;
    s->m2_cmap[s->m2_cmap_n].h = h;
    s->m2_cmap_n++;
    return h;
}
static bool nvkvm_m2_client_known(NvkvmGpuEmul *s, uint32_t g)
{
    for (int i = 0; i < s->m2_cmap_n; i++) {
        if (s->m2_cmap[i].g == g) {
            return true;
        }
    }
    return false;
}

/* M5.1a SHADOW-forward: replay the guest's RM alloc on the real host GPU in
 * PARALLEL — the guest still proceeds on the faked GSP response, so this is
 * non-disruptive. It validates the real alloc stream forwards and reveals where
 * the two-RM GPU-phys reconciliation first breaks (expected at the channel alloc,
 * whose instanceMem.base is the guest CPU-RM PMA's FB offset, meaningless to the
 * host RM's PMA). GSP_RM_ALLOC body @cmd: hClient@80,hParent@84,hObject@88,
 * hClass@92,paramsSize@100,params@112. -> NV_ESC_RM_ALLOC (NVOS64), params as aux.
 * DUP_OBJECT (fn 21) uses the NVOS55 field order at @80, plus an autoFree word. */
static void nvkvm_m2_shadow_fwd(NvkvmGpuEmul *s, const uint8_t *cmd, uint32_t fn)
{
    if (fn != 103 && fn != 10 && fn != 21) {
        return;                          /* allocs (103), frees (10), dups (21) */
    }
    if (!nvkvm_m2_iso_ensure(s)) {
        return;
    }
    /* M8.91: forward GSP DUP_OBJECT. UVM external memory handles are created by
     * duplicating existing RM objects into the UVM client; without this, the host
     * isolate has no object named by UVM_MAP_EXTERNAL_ALLOCATION's hMemory and
     * RM_MAP_MEMORY returns NV_ERR_OBJECT_NOT_FOUND (0x57). */
    if (fn == 21) {
        uint32_t dClient = ldl_le_p(cmd + 80), dParent = ldl_le_p(cmd + 84);
        uint32_t dObject = ldl_le_p(cmd + 88), sClient = ldl_le_p(cmd + 92);
        uint32_t sObject = ldl_le_p(cmd + 96), flags = ldl_le_p(cmd + 100);
        uint32_t autoFree = ldl_le_p(cmd + 104);
        struct nvos55_parameters d;
        memset(&d, 0, sizeof(d));
        d.h_client = nvkvm_m2_client(s, dClient);
        d.h_parent = nvkvm_m2_client_known(s, dParent) ? nvkvm_m2_client(s, dParent)
                                                       : dParent;
        d.h_object = dObject;
        d.h_client_src = nvkvm_m2_client_known(s, sClient) ? nvkvm_m2_client(s, sClient)
                                                           : sClient;
        d.h_src_object = nvkvm_m2_client_known(s, sObject) ? nvkvm_m2_client(s, sObject)
                                                           : sObject;
        d.flags = flags;
        unsigned int dc = (3u << 30) | ((unsigned int)sizeof(d) << 16) |
                          ((unsigned int)'F' << 8) | NV_ESC_RM_DUP_OBJECT;
        uint32_t dnv = 0;
        uint64_t dfault = 0;
        int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, dc,
                                     &d, sizeof(d), NULL, 0, 0, &dnv, &dfault);
        s->m2_fwd_n++;
        qemu_log("nvkvm-gpu[%s] M8.91 SHADOW[%u] dup hClient=0x%08x/0x%08x "
                 "hParent=0x%08x/0x%08x hObject=0x%08x hClientSrc=0x%08x/0x%08x "
                 "hSrcObject=0x%08x/0x%08x flags=0x%x autoFree=%u -> rc=%d "
                 "status=0x%x nv=0x%x%s\n", s->chip->name, s->m2_fwd_n,
                 dClient, d.h_client, dParent, d.h_parent, dObject,
                 sClient, d.h_client_src, sObject, d.h_src_object,
                 flags, autoFree, rc, d.status, dnv,
                 (rc == 0 && d.status == 0) ? "  OK" : "  <-- ERR/MISMATCH");
        return;
    }
    /* M5.1c: forward FREE so host objects/channels don't accumulate (the
     * un-freed channels exhausted the host's channel-ID heap). */
    if (fn == 10) {
        uint32_t fClient = ldl_le_p(cmd + 80), fParent = ldl_le_p(cmd + 84);
        uint32_t fObj = ldl_le_p(cmd + 88);
        struct nvos00_parameters f;
        memset(&f, 0, sizeof(f));
        f.h_root = nvkvm_m2_client(s, fClient);
        f.h_object_parent = nvkvm_m2_client_known(s, fParent) ? nvkvm_m2_client(s, fParent)
                                                              : fParent;
        f.h_object_old = nvkvm_m2_client_known(s, fObj) ? nvkvm_m2_client(s, fObj) : fObj;
        unsigned int fc = (3u << 30) | ((unsigned int)sizeof(f) << 16) |
                          ((unsigned int)'F' << 8) | NV_ESC_RM_FREE;
        uint32_t fst = 0; uint64_t ff = 0;
        nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, fc,
                            &f, sizeof(f), NULL, 0, 0, &fst, &ff);
        return;
    }
    static uint8_t auxbuf[16384];
    uint32_t hClient = ldl_le_p(cmd + 80), hParent = ldl_le_p(cmd + 84);
    uint32_t hObject = ldl_le_p(cmd + 88), hClass = ldl_le_p(cmd + 92);
    uint32_t psize   = ldl_le_p(cmd + 100);
    if (psize > sizeof(auxbuf)) {
        psize = sizeof(auxbuf);
    }
    memcpy(auxbuf, cmd + 112, psize);
    /* M5.3 DIAG: compare libcuda's working VASpace (0xcaf00005) vs the UVM RM-internal
     * one (0x5c000007) that resolves to a NULL OBJVASPACE on the host. NV_VASPACE_
     * ALLOCATION_PARAMETERS: index@0, flags@4, vaSize@8(u64), vaStartInternal@16(u64),
     * vaLimitInternal@24(u64), bigPageSize@32, vaBase@40(u64). */
    if (hClass == 0x90f1u && psize >= 4) {
        char hex[256]; int n = (int)(psize < 48 ? psize : 48); int o = 0;
        for (int i = 0; i + 4 <= n; i += 4) {
            o += snprintf(hex + o, sizeof(hex) - o, "%s@%d=0x%08x",
                          i ? " " : "", i, ldl_le_p(auxbuf + i));
        }
        qemu_log("nvkvm-gpu[%s] M5.3 DIAG 90f1 VAS obj=0x%08x client=0x%08x "
                 "psize=%u: %s\n", s->chip->name, hObject, hClient, psize, hex);
    }
    /* M5.3 EXPERIMENT: the UVM GR VASpace is forwarded with flags IS_EXTERNALLY_OWNED
     * (BIT3) | ENABLE_PAGE_FAULTING (BIT6) = 0x48, whose page tables the guest UVM
     * manages — unforwardable, so the host gets an unmanaged shell (NULL OBJVASPACE)
     * -> ctxshare INVALID_STATE. Hypothesis: the host doesn't need UVM management of
     * THIS vaspace; it only needs a functional RM-managed VASpace to build its shadow
     * GR context (buffer CONTENTS are matched later via the data plane). So strip
     * EXTERNALLY_OWNED + PAGE_FAULTING (0x48) on the host copy → host RM owns the page
     * tables → real OBJVASPACE → ctxshare/channel/compute can construct. */
    if (hClass == 0x90f1u && psize >= 8) {
        uint32_t vflags = ldl_le_p(auxbuf + 4);
        if (vflags & 0x48u) {
            stl_le_p(auxbuf + 4, vflags & ~0x48u);
            qemu_log("nvkvm-gpu[%s] M5.3 90f1 strip EXT_OWNED|PAGE_FAULT flags 0x%x->0x%x\n",
                     s->chip->name, vflags, vflags & ~0x48u);
        }
    }
    /* M5.3 data-plane: remember the NV20_SUBDEVICE_0 (0x2080) handle per GR client —
     * GR_GET_CTX_BUFFER_INFO is issued on the subdevice to enumerate the host shadow
     * context's real buffers (the data to mirror into the guest's BAR-backed buffers). */
    if (hClass == 0x2080u && s->m2_subdev_n < 64) {
        s->m2_subdev[s->m2_subdev_n].client = hClient;
        s->m2_subdev[s->m2_subdev_n].subdev = hObject;
        s->m2_subdev_n++;
    }
    /* M5.3: remember each FERMI_VASPACE_A (0x90f1) forwarded under a (client,device)
     * so the GR channelgroup can be given an explicit hVASpace below. */
    if (hClass == 0x90f1u && s->m2_devvas_n < 32) {
        s->m2_devvas[s->m2_devvas_n].client = hClient;
        s->m2_devvas[s->m2_devvas_n].dev    = hParent;   /* VASpace parent = device */
        s->m2_devvas[s->m2_devvas_n].vas    = hObject;
        s->m2_devvas_n++;
    }
    /* M5.3 FIX: the GR-engine channelgroup (KEPLER_CHANNEL_GROUP_A 0xa06c,
     * engineType@12 == NV2080_ENGINE_TYPE_GRAPHICS=1) leaves hVASpace@8 == 0
     * (device-default), which fails NV_ERR_INVALID_OBJECT_HANDLE (0x33) on the
     * forwarded host device (no default VAS). The COPY-engine TSGs pass an explicit
     * handle and construct fine. Substitute the first VASpace forwarded under the
     * same (client,device) so the GR TSG — the compute object's parent chain — can
     * construct and the host RM self-promotes its GR context.
     * NV_CHANNEL_GROUP_ALLOCATION_PARAMS: hObjectError@0,hObjectEccError@4,
     * hVASpace@8, engineType@12. */
    if (hClass == 0xa06cu && psize >= 16) {
        uint32_t cur_vas = ldl_le_p(auxbuf + 8);
        uint32_t engine  = ldl_le_p(auxbuf + 12);
        uint32_t sub = 0;
        for (int i = 0; i < s->m2_devvas_n; i++) {
            if (s->m2_devvas[i].client == hClient && s->m2_devvas[i].dev == hParent) {
                sub = s->m2_devvas[i].vas;
                break;                       /* first VASpace under this device */
            }
        }
        /* M5.28 PER-CHANNEL VAS: a GR/compute TSG (engineType GRAPHICS=1) gets a FRESH
         * nvkvm-owned VAS, ALWAYS — replacing whatever VAS libcuda passed (the compute TSGs
         * reference the guest's forwarded VAS explicitly, the one the host RM self-promotes GR
         * ctx into -> st=0x51 collisions / Xid 32). cvas is keyed by the TSG handle (hObject);
         * the ctxshare + channel below inherit/reference it. Other engine TSGs keep the legacy
         * forwarded VAS — NOTE: extending this to COPY engines (0x9..0x12) was tried and HUNG
         * the guest (it redirects the copy channels off the main guest VAS 0xcaf00005, which the
         * guest driver relies on; faulted -> PMC_BOOT_0 reset spin). Copy-channel collisions
         * (18, no Xid) need a different approach. */
        if (engine == 1u) {
            int ci = nvkvm_m2_cvas_get(s, hClient, hObject);
            if (ci >= 0) {
                stl_le_p(auxbuf + 8, s->m2_cvas[ci].fvas);
                qemu_log("nvkvm-gpu[%s] M5.28 a06c GR TSG hVASpace 0x%08x -> 0x%08x "
                         "[per-chan fresh] (engineType=%u tsg=0x%08x)\n", s->chip->name,
                         cur_vas, s->m2_cvas[ci].fvas, engine, hObject);
            } else if (cur_vas == 0u && sub) {
                stl_le_p(auxbuf + 8, sub);
                qemu_log("nvkvm-gpu[%s] M5.28 a06c GR TSG cvas alloc FAILED; fallback "
                         "forwarded VAS 0x%08x (engineType=%u)\n", s->chip->name, sub, engine);
            } else {
                qemu_log("nvkvm-gpu[%s] M5.28 a06c GR TSG cvas FAILED + no fallback "
                         "(cur_vas=0x%08x client=0x%08x)\n", s->chip->name, cur_vas, hClient);
            }
        } else if (cur_vas == 0u && sub) {
            stl_le_p(auxbuf + 8, sub);
            qemu_log("nvkvm-gpu[%s] M5.3 a06c non-GR TSG hVASpace 0 -> 0x%08x "
                     "(engineType=%u)\n", s->chip->name, sub, engine);
        }
    }
    /* M5.3: record TSG (0xa06c) handle -> engineType@12 for the channel engineType fix. */
    if (hClass == 0xa06cu && psize >= 16 && s->m2_tsgeng_n < 32) {
        s->m2_tsgeng[s->m2_tsgeng_n].tsg    = hObject;
        s->m2_tsgeng[s->m2_tsgeng_n].engine = ldl_le_p(auxbuf + 12);
        s->m2_tsgeng_n++;
    }
    /* M5.3: FERMI_CONTEXT_SHARE_A (0x9067) NV_CTXSHARE_ALLOCATION_PARAMETERS has
     * hVASpace@0. The GR context share (under the GR TSG) leaves it 0 (device
     * default) → NV_ERR_INVALID_STATE (0x40) on the host, and it must match the
     * TSG/channel VASpace. Substitute the same first-VASpace-for-client. */
    if (hClass == 0x9067u && psize >= 12) {
        uint32_t cvas = ldl_le_p(auxbuf), cfl = ldl_le_p(auxbuf + 4),
                 csub = ldl_le_p(auxbuf + 8);
        uint32_t sub = 0;
        for (int i = 0; i < s->m2_devvas_n; i++) {
            if (s->m2_devvas[i].client == hClient) { sub = s->m2_devvas[i].vas; break; }
        }
        /* M5.28: the ctxshare is parented to the GR TSG (hParent). If that TSG was given a
         * per-channel fresh VAS above, the ctxshare's hVASpace@0 must reference the SAME
         * fresh VAS (RM requires the share's VAS == the TSG's VAS), else the host channel
         * runs in a different VAS than the one we populate. */
        int ci = -1;
        for (int i = 0; i < s->m2_cvas_n; i++) {
            if (s->m2_cvas[i].client == hClient && s->m2_cvas[i].tsg == hParent) { ci = i; break; }
        }
        if (ci >= 0) {
            stl_le_p(auxbuf, s->m2_cvas[ci].fvas);
            qemu_log("nvkvm-gpu[%s] M5.28 9067 ctxshare hVASpace@0 0x%08x -> 0x%08x "
                     "[per-chan fresh, tsg=0x%08x]\n", s->chip->name, cvas,
                     s->m2_cvas[ci].fvas, hParent);
        } else {
            qemu_log("nvkvm-gpu[%s] M5.3 DIAG 9067 ctxshare hVASpace@0=0x%08x flags@4=0x%x "
                     "subctxId@8=0x%x hClient=0x%08x trackedVAS=0x%08x (devvas_n=%d cvas=%d)\n",
                     s->chip->name, cvas, cfl, csub, hClient, sub, s->m2_devvas_n, ci);
        }
    }
    /* M5.1c experiment: for channel classes, drop hObjectError (params+0) — its
     * error-notifier memory object isn't forwarded yet, so RM's notifier lookup
     * fails (kchannelGetNotifierInfo OBJECT_NOT_FOUND). Zeroing it lets the
     * channel construct without a notifier; revisit when memory objects forward. */
    if (nvkvm_m2_is_gpfifo_channel_class(hClass) && psize >= 4) {
        stl_le_p(auxbuf, 0u);                        /* hObjectError = 0 */
        /* hUserdMemory[0] @ params+32: "ignored if 0" -> the host CPU-RM allocates
         * USERD itself (kernel_channel.c:309); instance memory is RM-allocated on
         * the normal host-RM path too. So zeroing the client-USERD handle lets the
         * channel fully construct with RM-managed memory (M5.3a).
         * M5.4: for the GR channel (the one whose completion poll hangs cuCtxCreate,
         * CRASHWIN fb=0x420208c), instead BACK its USERD with real host GPU memory
         * (double-mmap) so the host GPU's GP_GET is visible to the guest's poll.
         * Identify it by its parent TSG's engine (GRAPHICS=1). Other channels keep
         * hUserdMemory[0]=0 (host RM allocates) until they're backed too. */
        if (psize >= 36) {
            stl_le_p(auxbuf + 32, 0u);               /* default: host RM allocates USERD */
            /* M5.4: back the GR channel's USERD with real host GPU memory (double-mmap).
             * Identify it by parent TSG engine (GRAPHICS=1). Backing ALL channels was
             * tried (incl. sentinel-handle probe channels 0xbaba0045/0x31415900) and
             * introduced status=0x51/0x33 errors on those probe channels, so restrict to
             * the GR channel (the cuCtxCreate context) — proven clean. The libcuda COPY
             * channels keep RM-allocated USERD. NOTE: USERD-backing alone does NOT clear
             * the cuCtxCreate hang — the dominant wait (CRASHWIN fb=0x2efbaf000, 331x,
             * PRAMIN-accessed gva=0) is a non-USERD FB semaphore the host channel must
             * EXECUTE to write (M5.4 steps 2-3: GPFIFO+pushbuffer double-mmap + doorbell). */
            bool is_gr = false;
            for (int i = 0; i < s->m2_tsgeng_n; i++) {
                if (s->m2_tsgeng[i].tsg == hParent && s->m2_tsgeng[i].engine == 1u) {
                    is_gr = true; break;
                }
            }
            /* M5.23 USERD double-mmap (host-channel bridge step 1): back the USERD of
             * EVERY real forwarded channel with host GPU memory + double-mmap at the
             * guest USERD FB addr, so the guest's GP_PUT (userd+0x8C) lands where the
             * host GPU reads GP_PUT/GP_GET — the prerequisite for the rung host channel
             * to actually run.  EXCLUDE libcuda PROBE/sentinel channels (0xbaba.. /
             * 0x31415..): M5.4 proved backing those returns 0x51/0x33 (they aren't real
             * runnable channels).  No-copy: the double-mmap shares the page, not a copy. */
            bool is_sentinel = ((hObject & 0xffff0000u) == 0xbaba0000u) ||
                               ((hObject & 0xffffff00u) == 0x31415900u);
            if (!is_sentinel) {
                nvkvm_m2_back_channel_userd(s, hClient, hObject, auxbuf, psize);
            }
            if (is_gr) {
                s->m2_gr_channel = hObject;  /* M5.8: track for work-submit-token */
                s->m2_gr_tsg     = hParent;  /* M5.8: GR TSG (for GPFIFO_SCHEDULE) */
            }
        }
        /* M5.3: NV_CHANNEL_ALLOC_PARAMS hVASpace@28 (alloc_channel.h). Like the GR
         * channelgroup, the GR channel leaves it 0 (device default) which won't
         * resolve on the forwarded host device -> OBJECT_NOT_FOUND. Substitute the
         * VASpace forwarded under the same (client,device-of-the-channelgroup). The
         * channel's hParent is the TSG, so look up by the TSG's device — track via
         * the a06c we already saw. Simplest: substitute the first VASpace tracked for
         * this client (the GR VAS). Also dump for diagnosis. */
        if (psize >= 64) {
            uint32_t hctxshare = ldl_le_p(auxbuf + 24);
            uint32_t hvas      = ldl_le_p(auxbuf + 28);
            qemu_log("nvkvm-gpu[%s] M5.3 DIAG c56f obj=0x%08x hParent=0x%08x "
                     "hContextShare@24=0x%08x hVASpace@28=0x%08x gpFifoOff@8=0x%llx "
                     "psize=%u\n", s->chip->name, hObject, hParent, hctxshare, hvas,
                     (unsigned long long)ldq_le_p(auxbuf + 8), psize);
            /* memory descriptors region (NV_MEMORY_DESC_PARAMS @144/168/192/216 for
             * instanceMem/userdMem/ramfcMem/mthdbufMem; base@+0,addrSpace@+16) — the
             * suspected two-RM reconciliation point (guest-FB bases). Dump u64s. */
            if (psize >= 240) {
                qemu_log("nvkvm-gpu[%s] M5.3 DIAG c56f memdescs: inst.base@144=0x%llx "
                         "as@160=0x%x userd.base@168=0x%llx ramfc.base@192=0x%llx "
                         "mthd.base@216=0x%llx engineType@128=0x%x\n", s->chip->name,
                         (unsigned long long)ldq_le_p(auxbuf + 144), ldl_le_p(auxbuf + 160),
                         (unsigned long long)ldq_le_p(auxbuf + 168),
                         (unsigned long long)ldq_le_p(auxbuf + 192),
                         (unsigned long long)ldq_le_p(auxbuf + 216), ldl_le_p(auxbuf + 128));
            }
            /* NOTE: do NOT substitute the channel's hVASpace. ALL these channels are
             * TSG channels (parented to a 0xa06c group); the host RM rejects any
             * explicit vaspace on a TSG channel ("TSG channels can't use an explicit
             * vaspace", kernel_channel.c) — they inherit the TSG's vaspace. The earlier
             * substitution was a red herring that broke the libcuda COPY channels; the
             * GR channel only needed its ctxshare to exist (the EXTERNALLY_OWNED strip).
             * hVASpace stays 0 here. (void to silence unused.) */
            (void)hctxshare;
            /* M5.3: NV_CHANNEL_ALLOC_PARAMS engineType@128. The GR channel passes 0
             * (NULL/inherit); on the host give it the parent TSG's engine explicitly. */
            if (psize >= 132 && ldl_le_p(auxbuf + 128) == 0u) {
                for (int i = 0; i < s->m2_tsgeng_n; i++) {
                    if (s->m2_tsgeng[i].tsg == hParent && s->m2_tsgeng[i].engine != 0) {
                        stl_le_p(auxbuf + 128, s->m2_tsgeng[i].engine);
                        qemu_log("nvkvm-gpu[%s] M5.3 c56f engineType 0 -> 0x%x "
                                 "(from TSG 0x%08x)\n", s->chip->name,
                                 s->m2_tsgeng[i].engine, hParent);
                        break;
                    }
                }
            }
        }
    }
    struct nvos64_parameters p;
    memset(&p, 0, sizeof(p));
    /* M5.1b: translate client refs. h_root is always the owning client; register
     * + remap it. h_object_parent is a client for device allocs (== hClient) but
     * an object for deeper allocs — translate only if it's a known client. */
    uint32_t h_root = nvkvm_m2_client(s, hClient);
    uint32_t h_parent = nvkvm_m2_client_known(s, hParent) ? nvkvm_m2_client(s, hParent)
                                                          : hParent;
    p.h_root = h_root; p.h_object_parent = h_parent; p.h_object_new = hObject;
    p.h_class = hClass; p.alloc_parms_size = psize;
    unsigned int ic = (3u << 30) | ((unsigned int)sizeof(p) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_ALLOC;
    uint32_t nvstatus = 0xdeadbeefu;
    uint64_t fault = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, ic,
                                 &p, sizeof(p), auxbuf, psize, 0, &nvstatus, &fault);
    s->m2_fwd_n++;
    qemu_log("nvkvm-gpu[%s] M5.1 SHADOW[%u] alloc class=0x%04x hParent=0x%08x "
             "hObj=0x%08x -> rc=%d status=0x%x%s\n", s->chip->name, s->m2_fwd_n,
             hClass, hParent, hObject, rc, p.status,
             (rc == 0 && p.status == 0) ? "  OK" : "  <-- ERR/MISMATCH");
    if (nvkvm_m2_is_gpfifo_channel_class(hClass) &&
        psize >= NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF + 4u) {
        if (rc == 0 && p.status == 0) {
            s->m2_chan_reply_obj = hObject;
            s->m2_chan_reply_internal_flags =
                ldl_le_p(auxbuf + NVKVM_M2_CHAN_INTERNAL_FLAGS_OFF);
            s->m2_chan_reply_valid = true;
            qemu_log("nvkvm-gpu[%s] M8.103 captured channel alloc reply "
                     "class=0x%04x obj=0x%08x internalFlags=0x%08x\n",
                     s->chip->name, hClass, hObject,
                     s->m2_chan_reply_internal_flags);
        } else if (s->m2_chan_reply_valid &&
                   s->m2_chan_reply_obj == hObject) {
            s->m2_chan_reply_valid = false;
        }
    }
    if (hClass == 0x83deu && rc == 0 && p.status == 0) {
        bool seen = false;
        for (int i = 0; i < s->m2_debug_n; i++) {
            if (s->m2_debug[i].client == hClient &&
                s->m2_debug[i].hobject == hObject) {
                seen = true;
                break;
            }
        }
        if (!seen && s->m2_debug_n < 16) {
            s->m2_debug[s->m2_debug_n].client = hClient;
            s->m2_debug[s->m2_debug_n].hobject = hObject;
            s->m2_debug_n++;
            qemu_log("nvkvm-gpu[%s] M8.82 DEBUG_OBJECT client=0x%08x "
                     "hObj=0x%08x tracked=%d\n",
                     s->chip->name, hClient, hObject, s->m2_debug_n);
        }
    }

    /* M7 (cuCtxCreate fix): for GR-object allocs (fam>=0xb0, lowbyte 0xc0 compute / 0x97 3D —
     * NV_GR_ALLOCATION_PARAMETERS, 16B with a GSP-filled `caps` output), capture the HOST's real
     * reply params (auxbuf is in/out; the host RM wrote them) so the GSP-RPC reply builder can
     * pass them through to the guest instead of echoing the request (caps=0). */
    {
        uint32_t lb = hClass & 0xffu, fam = (hClass >> 8) & 0xffu;
        if (fam >= 0xb0u && (lb == 0xc0u || lb == 0x97u) && rc == 0 && p.status == 0) {
            uint32_t n = psize < sizeof(s->m2_gr_reply) ? psize : (uint32_t)sizeof(s->m2_gr_reply);
            memcpy(s->m2_gr_reply, auxbuf, n);
            s->m2_gr_reply_obj = hObject;
            s->m2_gr_reply_psize = p.alloc_parms_size; /* host's RETURNED paramsSize */
            s->m2_gr_reply_valid = true;
            qemu_log("nvkvm-gpu[%s] M7 captured host GR-alloc reply 0x%04x obj=0x%08x "
                     "caps@12=0x%08x host_ret_psize=%u (req_psize=%u)\n", s->chip->name, hClass, hObject,
                     n >= 16 ? ldl_le_p(auxbuf + 12) : 0, p.alloc_parms_size, psize);
        }
    }

    /* M5.3 DATA-PLANE step 1 (enumerate): once the compute object (AMPERE_COMPUTE_B
     * 0xc7c0) constructs on the host shadow context, query GR_GET_CTX_BUFFER_INFO on the
     * subdevice to read the REAL host context-buffer set (size/physAddr/aperture/type).
     * RESULT (2026-06-04): this control is PRIVILEGED -> returns st=0x1b
     * (NV_ERR_INSUFFICIENT_PERMISSIONS) for the unprivileged stub, exactly like
     * GET_SURFACE_PHYS_ATTR. Per the hard security constraint (QEMU stays unprivileged in
     * prod), the "read/mirror host GR context buffers" data-plane approach is BLOCKED on
     * the unprivileged path. Kept as a documented probe; the data plane must instead be
     * solved by forging the guest-side GSP state so libcuda's context buffers are
     * self-consistent (unprivileged), or via an unprivileged host-content path TBD.
     * hParent of the compute object IS the channel. */
    if (hClass == 0xc7c0u && rc == 0 && p.status == 0) {
        /* M5.3 DIAG: arm the crash-window FB-read probe. libcuda now reads GR-context
         * GPU memory and crashes (rbp=0); the reads logged from here pin the buffer. */
        if (!s->m2_crashwin) {
            s->m2_crashwin = true;
            s->m2_gr_client = hClient;        /* M5.7: the GR compute client */
            qemu_log("nvkvm-gpu[GA106] CRASHWIN ARMED (after 0xc7c0 compute obj "
                     "0x%08x) client=0x%08x — logging subsequent FB reads\n",
                     hObject, hClient);
        }
        /* M5.5: validate the RM_MAP_MEMORY_DMA-FIXED primitive once, on the GR client's
         * real host VASpace. Proves we can place a mapping at a VA we choose — the
         * irreducible step for forwarding the guest's working set into the host VAS. */
        if (!s->m2_mapdma_tested) {
            s->m2_mapdma_tested = true;
            nvkvm_m2_mapdma_selftest(s, hClient);
            nvkvm_m2_osdesc_selftest(s, hClient);   /* M6.2: OS_DESCRIPTOR guest RAM (item-4 step 3) */
        }
        /* M6.5 (item-4 step 4): DISCOVERY sweep — walk the GR VAS page tables, enumerate every
         * sysmem leaf, and OS_DESCRIPTOR+map_dma each into the host GR VASpace so the host GPU
         * can DMA into the guest's actual NVOS32-local sysmem GR buffers (the crash buffers).
         * Idempotent; re-run on each later compute-obj alloc to catch mappings built afterward. */
        if (s->m2exec && s->m2legacyvas) {
            nvkvm_m2_enum_gr_sysmem(s, hClient);
        } else if (s->m2exec) {
            qemu_log("nvkvm-gpu[%s] M8.97 skip legacy c7c0 GR-VAS sweep "
                     "client=0x%08x; using per-channel/reactive mapping\n",
                     s->chip->name, hClient);
        }
        uint32_t subdev = 0;
        for (int i = 0; i < s->m2_subdev_n; i++) {
            if (s->m2_subdev[i].client == hClient) { subdev = s->m2_subdev[i].subdev; break; }
        }
        if (!subdev) {
            qemu_log("nvkvm-gpu[%s] M5.3 ctxbuf: no subdevice tracked for client 0x%08x\n",
                     s->chip->name, hClient);
        } else {
            /* params: hUserClient@0, hChannel@4, bufferCount@8, ctxBufferInfo[64]@16
             * (each 80B: alignment@0,size@8,bufferHandle@16,pageCount@24,physAddr@32,
             * bufferType@40,aperture@44,kind@48,pageSize@52,flags@56,uuid@60). */
            static uint8_t cb[16 + 64 * 80];
            memset(cb, 0, sizeof(cb));
            stl_le_p(cb + 0, nvkvm_m2_client(s, hClient)); /* hUserClient = host client */
            stl_le_p(cb + 4, hParent);                     /* hChannel = the GR channel  */
            uint32_t st = 0xffff;
            int crc = nvkvm_m2_control1(s, hClient, subdev, 0x20801219u, cb, sizeof(cb), &st);
            uint32_t cnt = ldl_le_p(cb + 8);
            qemu_log("nvkvm-gpu[%s] M5.3 GR_GET_CTX_BUFFER_INFO chan=0x%08x sub=0x%08x "
                     "-> crc=%d st=0x%x bufferCount=%u\n", s->chip->name, hParent, subdev,
                     crc, st, cnt);
            if (crc == 0 && st == 0 && cnt <= 64) {
                for (uint32_t i = 0; i < cnt; i++) {
                    const uint8_t *e = cb + 16 + (uint64_t)i * 80;
                    qemu_log("nvkvm-gpu[%s]   ctxbuf[%u] type=%u aperture=%u size=0x%llx "
                             "physAddr=0x%llx align=0x%llx pageSize=%u\n", s->chip->name, i,
                             ldl_le_p(e + 40), ldl_le_p(e + 44),
                             (unsigned long long)ldq_le_p(e + 8),
                             (unsigned long long)ldq_le_p(e + 32),
                             (unsigned long long)ldq_le_p(e + 0), ldl_le_p(e + 52));
                }
            }
        }
    }
}

/* M5.3 helper: forward one RM_ALLOC (NVOS64) with client remap; returns nvstatus. */
static int nvkvm_m2_alloc1(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hParent,
                           uint32_t hObject, uint32_t hClass,
                           void *aux, uint32_t auxlen, uint32_t *st)
{
    struct nvos64_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_root = nvkvm_m2_client(s, hClient);
    p.h_object_parent = nvkvm_m2_client_known(s, hParent) ? nvkvm_m2_client(s, hParent)
                                                          : hParent;
    p.h_object_new = hObject; p.h_class = hClass; p.alloc_parms_size = auxlen;
    unsigned int ic = (3u << 30) | ((unsigned int)sizeof(p) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_ALLOC;
    uint32_t nv = 0; uint64_t f = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, ic,
                                 &p, sizeof(p), aux, auxlen, 0, &nv, &f);
    if (st) { *st = p.status; }
    return rc;
}

/* M5.3 helper: forward one RM_CONTROL (NVOS54) with client remap; params marshalled
 * as aux (the stub relocates the params@16 pointer). h_object is an object handle,
 * not a client, so it is passed verbatim unless it names a known client. */
static int nvkvm_m2_control1(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hObject,
                             uint32_t cmd, void *params, uint32_t psize, uint32_t *st)
{
    struct nvos54_parameters p;
    memset(&p, 0, sizeof(p));
    p.h_client = nvkvm_m2_client(s, hClient);
    p.h_object = nvkvm_m2_client_known(s, hObject) ? nvkvm_m2_client(s, hObject) : hObject;
    p.cmd = cmd; p.params_size = psize;
    unsigned int ic = (3u << 30) | ((unsigned int)sizeof(p) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_CONTROL;
    uint32_t nv = 0; uint64_t f = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, ic,
                                 &p, sizeof(p), params, psize, 0, &nv, &f);
    if (st) { *st = p.status; }
    return rc;
}

/* M5.3: query the host GPU-physical (FB) address of a vidmem object via
 * NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR (0x410103). Returns the FB phys offset in
 * *phys (the value PROMOTE_CTX needs to point the host GPU at this buffer). The
 * 48B params: memOffset@0 (in: offset, out: phys), memAperture@20 (0=VIDMEM). */
static bool nvkvm_m2_host_phys(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hMem,
                               uint64_t *phys, uint32_t *aperture)
{
    uint8_t pa[48];
    memset(pa, 0, sizeof(pa));
    uint32_t st = 0xffff;
    int rc = nvkvm_m2_control1(s, hClient, hMem, 0x410103u, pa, sizeof(pa), &st);
    if (rc != 0 || st != 0) {
        qemu_log("nvkvm-gpu[%s] M5.3: GET_SURFACE_PHYS_ATTR 0x%x rc=%d st=0x%x\n",
                 s->chip->name, hMem, rc, st);
        return false;
    }
    if (phys)     { *phys = ldq_le_p(pa); }
    if (aperture) { *aperture = ldl_le_p(pa + 20); }
    return true;
}

/* Result of the M5.3 data-plane primitive: real host GPU memory mapped into QEMU. */
struct nvkvm_host_map {
    void    *qva;       /* QEMU VA of the host GPU memory (NULL on failure)    */
    int      mapfd;     /* QEMU's fd for the fresh device open (SCM_RIGHTS)    */
    uint32_t h_mem;     /* RM memory handle on the host                        */
    uint32_t maph;      /* isolate-fd handle of the fresh device fd            */
    uint64_t size;
};

static bool nvkvm_m2_host_map_memory(NvkvmGpuEmul *s, uint32_t hClient,
                                     uint32_t hDevice, uint32_t hMem,
                                     uint64_t size, const char *why,
                                     struct nvkvm_host_map *out)
{
    memset(out, 0, sizeof(*out));
    if (!size || !nvkvm_m2_iso_ensure(s)) {
        return false;
    }

    /* Fresh /dev/nvidia0 fd — nvidia binds exactly one CPU mapping per device fd. */
    if (s->m2_maph_next < 16) {
        s->m2_maph_next = 16;
    }
    uint32_t maph = s->m2_maph_next++;
    int mapfd = -1;
    if (nvkvm_isolate_open_device(&s->m2_iso, s->m2_iso_id, maph,
                                  NVKVM_DEV_GPU(0), O_RDWR, &mapfd) != 0 ||
        mapfd < 0) {
        qemu_log("nvkvm-gpu[%s] M8.90: map-fd open failed %s hMem=0x%x "
                 "(maph=%u)\n", s->chip->name, why ? why : "memory",
                 hMem, maph);
        return false;
    }

    struct nv_ioctl_nvos33_parameters_with_fd mm;
    memset(&mm, 0, sizeof(mm));
    mm.h_client = nvkvm_m2_client(s, hClient);
    mm.h_device = hDevice;
    mm.h_memory = hMem;
    mm.length   = size;
    mm.fd       = (int32_t)maph;                 /* device fd to mmap (stub translates) */
    unsigned int mc = (3u << 30) | ((unsigned int)sizeof(mm) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_MAP_MEMORY;
    uint32_t mnv = 0;
    uint64_t mf = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, mc,
                                 &mm, sizeof(mm), NULL, 0, 0, &mnv, &mf);
    if (rc != 0 || mm.status != 0) {
        qemu_log("nvkvm-gpu[%s] M8.90: RM_MAP_MEMORY %s hMem=0x%x "
                 "client=0x%08x/0x%08x dev=0x%08x len=0x%llx "
                 "rc=%d st=0x%x\n", s->chip->name, why ? why : "memory",
                 hMem, hClient, nvkvm_m2_client(s, hClient), hDevice,
                 (unsigned long long)size, rc, mm.status);
        close(mapfd);
        return false;
    }

    void *qva = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mapfd, 0);
    if (qva == MAP_FAILED) {
        qemu_log("nvkvm-gpu[%s] M8.90: mmap host mem %s hMem=0x%x "
                 "len=0x%llx failed: %s\n", s->chip->name,
                 why ? why : "memory", hMem, (unsigned long long)size,
                 strerror(errno));
        close(mapfd);
        return false;
    }

    out->qva = qva;
    out->mapfd = mapfd;
    out->h_mem = hMem;
    out->maph = maph;
    out->size = size;
    return true;
}

/* M5.3 DATA-PLANE PRIMITIVE (proven 651d860). Allocate a host GPU vidmem object of
 * `size` under (hClient,hDevice) on the real GPU, RM_MAP_MEMORY it on the CONTROL
 * device (NV_CTL_DEVICE_ONLY), and mmap QEMU's copy of a fresh device fd at offset 0
 * (vm_pgoff must be 0; per-fd mmap_context). Returns true with `out` filled, so QEMU
 * holds a real host-GPU-memory VA — the host half of the context-buffer double-mmap.
 * Caller owns the unique RM memory handle `hMem`. */
static bool nvkvm_m2_host_alloc_map_vidmem(NvkvmGpuEmul *s, uint32_t hClient,
                                           uint32_t hDevice, uint32_t hMem,
                                           uint64_t size, struct nvkvm_host_map *out)
{
    memset(out, 0, sizeof(*out));
    if (!nvkvm_m2_iso_ensure(s)) {
        return false;
    }
    struct nv_memory_allocation_params_v545 mp;
    memset(&mp, 0, sizeof(mp));
    mp.owner     = hClient;
    mp.type      = 0;                            /* NVOS32_TYPE_IMAGE */
    mp.attr      = (2u << 27) | (0u << 25);      /* CONTIGUOUS | LOCATION_VIDMEM */
    mp.size      = size;
    mp.alignment = 0x10000;
    uint32_t st = 0xffff;
    nvkvm_m2_alloc1(s, hClient, hDevice, hMem, 0x0040u, &mp, sizeof(mp), &st);
    if (st != 0) {
        qemu_log("nvkvm-gpu[%s] M5.3: host vidmem alloc 0x%x size=0x%llx failed st=0x%x\n",
                 s->chip->name, hMem, (unsigned long long)size, st);
        return false;
    }
    return nvkvm_m2_host_map_memory(s, hClient, hDevice, hMem, size,
                                    "fresh-vidmem", out);
}

/* M6.2 (item-4 step 3): translate a guest GPA to the stub VA where the guest-RAM memfd is
 * MAP_FIXED'd. Uses pci_dma_map to get QEMU's host VA for the GPA (hole-safe across the q35
 * PCI hole), then stub_va = stub_base + (hva - ram_base_hva) since the stub mmapped the SAME
 * memfd at m2_stub_ram_base. Returns 0 if not shared / GPA outside the main RAM block. */
static uint64_t nvkvm_m2_gpa_to_stub_va(NvkvmGpuEmul *s, uint64_t gpa)
{
    if (!s->m2_ram_shared || !s->m2_guest_ram_hva) {
        return 0;
    }
    dma_addr_t len = 0x1000;
    void *p = pci_dma_map(&s->parent_obj, gpa, &len, DMA_DIRECTION_TO_DEVICE);
    if (!p) {
        return 0;
    }
    uint64_t off = (uint64_t)((uintptr_t)p - (uintptr_t)s->m2_guest_ram_hva);
    pci_dma_unmap(&s->parent_obj, p, len, DMA_DIRECTION_TO_DEVICE, 0);
    if (off >= s->m2_guest_ram_size) {
        return 0;                                /* GPA not in the main (memfd) RAM block */
    }
    return s->m2_stub_ram_base + off;
}

/* M6.2 (item-4 step 3): OS_DESCRIPTOR-register guest RAM (at stub VA) as a host sysmem object,
 * so the host nvidia driver pins the guest pages and the host GPU can DMA into them — the fix
 * for libcuda's un-backed sysmem GR buffers ([[mode2-cuctxcreate-pagetable-poll]]). NVOS02
 * (NV_ESC_RM_ALLOC_MEMORY, NR 0x27), hClass=NV01_MEMORY_SYSTEM_OS_DESCRIPTOR (0x0071),
 * p_memory = the descriptor (stub VA the kernel pin_user_pages walks), limit = size-1, flags =
 * PHYSICALITY_NONCONTIGUOUS | LOCATION_PCI | COHERENCY_CACHED. */
static int nvkvm_m2_os_descriptor(NvkvmGpuEmul *s, uint32_t client, uint32_t device,
                                  uint32_t hMem, uint64_t stub_va, uint64_t size, uint32_t *st)
{
    /* The GR client lives in the ctl-fd session; OS_DESCRIPTOR runs on the /dev/nvidia0 device
     * fd, which must be REGISTER_FD'd to that ctl session or RM returns 0x23 INVALID_CLIENT
     * (real host libcuda does NV_ESC_REGISTER_FD(nvidia0, ctl_fd) before using the device fd). */
    if (!s->m2_gpu_registered) {
        struct nv_ioctl_register_fd rf; memset(&rf, 0, sizeof(rf));
        rf.ctl_fd = (int32_t)s->m2_ctl_h;   /* handle; stub translates @off 0 -> real ctl fd */
        unsigned int rc2 = (3u << 30) | ((unsigned int)sizeof(rf) << 16) |
                           ((unsigned int)'F' << 8) | NV_ESC_REGISTER_FD;
        uint32_t rnv = 0; uint64_t rf2 = 0;
        int rr = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_gpu_h, rc2,
                                     &rf, sizeof(rf), NULL, 0, 0, &rnv, &rf2);
        s->m2_gpu_registered = (rr == 0);
        qemu_log("nvkvm-gpu[%s] M6.2 REGISTER_FD(gpu0, ctl) rc=%d -> %s\n", s->chip->name,
                 rr, s->m2_gpu_registered ? "registered" : "FAILED");
    }
    struct nv_ioctl_nvos02_parameters_with_fd p;
    memset(&p, 0, sizeof(p));
    p.h_root          = nvkvm_m2_client(s, client);
    p.h_object_parent = nvkvm_m2_client_known(s, device) ? nvkvm_m2_client(s, device) : device;
    p.h_object_new    = hMem;
    p.h_class         = 0x00000071u;             /* NV01_MEMORY_SYSTEM_OS_DESCRIPTOR */
    /* Flags captured from a real host CUDA OS_DESCRIPTOR (cuMemHostAlloc/Register on driver
     * 580.159.04): 0x40001010 = NONCONTIG(0x10) | LOCATION_PCI(0) | COHERENCY_CACHED(0x1000) |
     * MAPPING_NO_MAP(0x40000000, bits31:30=1). MAPPING_NO_MAP is required — without it the
     * driver returns EINVAL (it tried to auto-map a describe-only allocation). */
    p.flags           = 0x40001010u;
    p.p_memory        = stub_va;                 /* [IN] descriptor: stub VA of the guest RAM */
    p.limit           = size ? (size - 1) : 0;
    p.fd              = -1;
    unsigned int ic = (3u << 30) | ((unsigned int)sizeof(p) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_ALLOC_MEMORY;
    uint32_t nv = 0; uint64_t f = 0;
    /* OS_DESCRIPTOR must run on the GPU DEVICE fd (/dev/nvidia0, m2_gpu_h), NOT the ctl fd —
     * a real host CUDA app does it on /dev/nvidia0 (captured). ctl fd -> EINVAL. */
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_gpu_h, ic,
                                 &p, sizeof(p), NULL, 0, 0, &nv, &f);
    if (st) { *st = p.status; }
    return rc;
}

/* M6.2 selftest: OS_DESCRIPTOR the first real GR sysmem buffer (va_map sys=true entry) to prove
 * the shared-memfd -> stub-VA -> OS_DESCRIPTOR chain works (host RM pins guest RAM). One-shot. */
static void nvkvm_m2_osdesc_selftest(NvkvmGpuEmul *s, uint32_t hClient)
{
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == hClient) { hDev = s->m2_devvas[i].dev; break; }
    }
    int idx = -1;
    for (int i = 0; i < s->va_map_n; i++) {
        if (s->va_map[i].sys && s->va_map[i].size) { idx = i; break; }
    }
    if (!hDev || idx < 0) {
        qemu_log("nvkvm-gpu[%s] M6.2 osdesc-selftest: no dev (0x%08x) or no sysmem va_map "
                 "entry (va_map_n=%d)\n", s->chip->name, hDev, s->va_map_n);
        return;
    }
    uint64_t gpa = s->va_map[idx].phys, sz = s->va_map[idx].size;
    uint64_t sva = nvkvm_m2_gpa_to_stub_va(s, gpa);
    if (!sva) {
        qemu_log("nvkvm-gpu[%s] M6.2 osdesc-selftest: GPA 0x%llx -> no stub VA (shared=%d)\n",
                 s->chip->name, (unsigned long long)gpa, s->m2_ram_shared);
        return;
    }
    uint32_t hMem = 0xdd000000u | (s->m2_databuf_next++ & 0xffffu);
    uint32_t st = 0xffff;
    int rc = nvkvm_m2_os_descriptor(s, hClient, hDev, hMem, sva, sz, &st);
    qemu_log("nvkvm-gpu[%s] M6.2 osdesc-selftest: GR sysmem buf GPA=0x%llx size=0x%llx -> "
             "stub_va=0x%llx OS_DESCRIPTOR hMem=0x%08x rc=%d st=0x%x %s\n", s->chip->name,
             (unsigned long long)gpa, (unsigned long long)sz, (unsigned long long)sva, hMem,
             rc, st, (rc == 0 && st == 0) ? "  OK — host RM pinned guest RAM!"
                                          : "  <-- ERR (tune flags/descriptor)");
    /* M6.3 (item-4 step 4): map the pinned guest RAM into the GR VASpace at the guest's GR VA,
     * so the host GPU's MMU resolves that VA to the guest's sysmem buffer (host GPU then
     * DMA-reads/writes the SAME memory the guest CPU sees). Reuses the M5.5 map_dma primitive +
     * the per-client GR virtmem mapper. */
    if (rc == 0 && st == 0) {
        uint64_t va = s->va_map[idx].va;
        uint32_t hVirt = nvkvm_m2_grmapper(s, hClient);
        uint32_t mst = 0xffff; uint64_t outva = 0;
        int mrc = hVirt ? nvkvm_m2_map_dma(s, hClient, hDev, hVirt, hMem, 0, sz, true, va,
                                           &mst, &outva) : -1;
        qemu_log("nvkvm-gpu[%s] M6.3 map pinned guest RAM into GR VAS: hVirt=0x%08x va=0x%llx "
                 "-> rc=%d st=0x%x outva=0x%llx %s\n", s->chip->name, hVirt,
                 (unsigned long long)va, mrc, mst, (unsigned long long)outva,
                 (mrc == 0 && mst == 0 && outva == va)
                     ? "  OK — host GPU can now reach the guest's sysmem GR buffer!"
                     : (mst == 0x51u ? "  ALREADY-MAPPED" : "  <-- ERR"));
        /* M6.3b (the user's "can we fix ANY GR VA?" for SYSMEM): map the SAME OS_DESCRIPTOR'd
         * guest RAM at a VA WE choose (free, not host-pre-mapped) into the GR VAS. st=0 proves
         * we control the sysmem GR-VA layout end-to-end (guest RAM placeable at any chosen GR VA
         * = the item-4 step-4 placement primitive, validated). Distinct from M6.3 which reuses
         * the guest's own (often host-occupied) VA. */
        if (hVirt) {
            uint64_t freeva = 0x300000000ull;        /* well clear of GR ctx (0x120xxxxxx) + UVM */
            uint32_t fst = 0xffff; uint64_t fova = 0;
            int frc = nvkvm_m2_map_dma(s, hClient, hDev, hVirt, hMem, 0, sz, true, freeva,
                                       &fst, &fova);
            qemu_log("nvkvm-gpu[%s] M6.3b place guest-RAM sysmem at CHOSEN free GR VA=0x%llx -> "
                     "rc=%d st=0x%x outva=0x%llx %s\n", s->chip->name,
                     (unsigned long long)freeva, frc, fst, (unsigned long long)fova,
                     (frc == 0 && fst == 0 && fova == freeva)
                         ? "  OK — we OWN the sysmem GR-VA layout (item-4 step-4 primitive proven)"
                         : (fst == 0x51u ? "  ALREADY-MAPPED (pick another VA)" : "  <-- ERR"));
        }
    }
}

/* M6.4 (item-4): forward the guest's PROMOTE_CTX to the host with each sysmem buffer's
 * gpuPhysAddr substituted to OUR backing's host physical. For each promote entry that's
 * sysmem + mapped: OS_DESCRIPTOR the guest RAM at its GPA -> host hMem -> GET_SURFACE_PHYS_ATTR
 * -> host phys; write that into the entry's gpuPhysAddr. Then forward the (substituted)
 * PROMOTE_CTX control (0x2080012b) on the GR subdevice. Effect: the host GR context maps the
 * guest's GR VAs onto the guest's actual sysmem -> host GPU DMA-fills what libcuda reads.
 * (Reframe: we don't replay the guest's calls — we reproduce the GR-VA->backing EFFECT.) */
static void nvkvm_m2_forward_promote_ctx(NvkvmGpuEmul *s, const uint8_t *cmd)
{
    uint32_t hClient = ldl_le_p(cmd + 80), hObject = ldl_le_p(cmd + 84);
    uint32_t psize   = ldl_le_p(cmd + 96);
    if (psize < 48 || psize > 8192) {
        return;
    }
    static uint8_t pc[8192];
    memcpy(pc, cmd + 120, psize);                 /* the PROMOTE_CTX params */
    uint32_t ec = ldl_le_p(pc + 40);
    if (ec > 20) { ec = 20; }
    uint32_t dev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == hClient) { dev = s->m2_devvas[i].dev; break; }
    }
    int subst = 0;
    for (uint32_t i = 0; i < ec; i++) {
        uint8_t *e = pc + 48 + (uint64_t)i * 32;
        uint64_t phys = ldq_le_p(e + 0), va = ldq_le_p(e + 8), sz = ldq_le_p(e + 16);
        uint32_t physAttr = ldl_le_p(e + 24);
        uint8_t  bNonmapped = e[31];
        if (!va || !sz || bNonmapped || (physAttr & 0x3u) == 0) {
            continue;                             /* skip vidmem / unmapped / phys-only */
        }
        uint64_t sva = nvkvm_m2_gpa_to_stub_va(s, phys);
        if (!sva || !dev) { continue; }
        uint32_t hMem = 0xde000000u | (s->m2_databuf_next++ & 0xffffu);
        uint32_t ost = 0xffff;
        if (nvkvm_m2_os_descriptor(s, hClient, dev, hMem, sva, sz, &ost) != 0 || ost != 0) {
            continue;
        }
        uint64_t hphys = 0; uint32_t aper = 0xff;
        if (!nvkvm_m2_host_phys(s, hClient, hMem, &hphys, &aper) || !hphys) {
            continue;
        }
        stq_le_p(e + 0, hphys);                   /* substitute gpuPhysAddr -> our backing */
        subst++;
    }
    uint32_t st = 0xffff;
    int rc = nvkvm_m2_control1(s, hClient, hObject, 0x2080012bu, pc, psize, &st);
    qemu_log("nvkvm-gpu[%s] M6.4 forward PROMOTE_CTX: client=0x%08x subdev=0x%08x entries=%u "
             "subst=%d -> rc=%d st=0x%x %s\n", s->chip->name, hClient, hObject, ec, subst,
             rc, st, (rc == 0 && st == 0) ? "  OK — host GR ctx mapped onto guest RAM!"
                                          : "  <-- ERR");
}

/* M5.5 EXECUTION-PLANE PRIMITIVE: map a host memory object into a host VASpace at a
 * FIXED GPU VA (the guest's chosen VA), via NV_ESC_RM_MAP_MEMORY_DMA. This is the
 * irreducible primitive the whole data plane rests on: it puts the guest's working-set
 * buffers (GPFIFO / pushbuffer / semaphores / ctx buffers) into the host channel's VAS
 * at the guest VAs, so when the host GPU runs the channel its MMU resolves the same VAs
 * the guest submitted — i.e. the host executes the guest's real work (no faking).
 *   hDma  = the VASpace handle (FERMI_VASPACE_A); for a VASpace (non-CTXDMA) target,
 *           dmaOffset is [IN] when DMA_OFFSET_FIXED_TRUE, [OUT] otherwise.
 *   NVOS46 V580 layout (nvkvm_abi: size 64, status@56 — flags2+kindOverride pushed it
 *   past the 535/56-byte base): hClient@0 hDevice@4 hDma@8 hMemory@12 offset@16(u64)
 *   length@24(u64) flags@32 flags2@36 kindOverride@40 dmaOffset@48(u64) status@56.
 * Returns the ioctl rc; *st gets the RM status; *out_va gets the resulting GPU VA. */
static int nvkvm_m2_map_dma(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hDevice,
                            uint32_t hVas, uint32_t hMemory, uint64_t offset,
                            uint64_t length, bool fixed, uint64_t va,
                            uint32_t *st, uint64_t *out_va)
{
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    uint32_t rmClient = nvkvm_m2_client(s, hClient);
    uint32_t rmDevice = nvkvm_m2_client_known(s, hDevice) ? nvkvm_m2_client(s, hDevice)
                                                          : hDevice;
    uint32_t rmVas = nvkvm_m2_client_known(s, hVas) ? nvkvm_m2_client(s, hVas)
                                                    : hVas;
    uint32_t rmMemory = nvkvm_m2_client_known(s, hMemory) ? nvkvm_m2_client(s, hMemory)
                                                          : hMemory;
    stl_le_p(p + 0,  rmClient);        /* hClient (remapped)        */
    stl_le_p(p + 4,  rmDevice);        /* hDevice                   */
    stl_le_p(p + 8,  rmVas);           /* hDma = VASpace mapper     */
    stl_le_p(p + 12, rmMemory);        /* hMemory                   */
    stq_le_p(p + 16, offset);
    stq_le_p(p + 24, length);
    /* flags: ACCESS_READ_WRITE(0) | SHADER_ACCESS_READ_WRITE(3<<6) |
     * PAGE_SIZE_{4KB,BIG} | DMA_OFFSET_FIXED_TRUE(bit15).  The FE can fetch
     * pushbuffers with default access, but QMD program/CB/output pages are
     * touched by shader execution; make that permission explicit. */
    uint32_t flags = 0x000000c0u;
    if (fixed) {
        flags |= 0x00008000u;
        if (((offset | length | va) & 0xffffull) == 0 && length >= 0x10000ull) {
            flags |= 0x00000200u;   /* NVOS46_FLAGS_PAGE_SIZE_BIG */
        } else {
            flags |= 0x00000100u;   /* NVOS46_FLAGS_PAGE_SIZE_4KB */
        }
    }
    uint32_t default_flags = flags;
    const char *flag_src = "default";
    bool high_va = fixed && va >= 0x700000000000ull;
    if (high_va && s->m2mapflags_high) {
        flags = s->m2mapflags_high;
        flag_src = "m2mapflags_high";
    } else if (s->m2mapflags) {
        flags = s->m2mapflags;
        flag_src = "m2mapflags";
    }
    stl_le_p(p + 32, flags);
    if (fixed) {
        stq_le_p(p + 48, va);                              /* dmaOffset [IN] = FIXED VA */
    }
    unsigned int ic = (3u << 30) | ((unsigned int)64 << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_MAP_MEMORY_DMA;
    uint32_t nv = 0; uint64_t f = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, ic,
                                 p, sizeof(p), NULL, 0, 0, &nv, &f);
    uint32_t out_st = ldl_le_p(p + 56);
    uint64_t out = ldq_le_p(p + 48);
    if (s->trace) {
        uint32_t log_idx = s->m2_mapdma_logs++;
        uint32_t high_idx = high_va ? s->m2_mapdma_high_logs++ : 0;
        if (rc || out_st || log_idx < 256u ||
            (high_va && high_idx < 512u)) {
            qemu_log("nvkvm-gpu[%s] M8.76 MAP_DMA %s client=0x%08x/0x%08x "
                     "dev=0x%08x/0x%08x hDma=0x%08x/0x%08x "
                     "hMem=0x%08x/0x%08x off=0x%llx len=0x%llx "
                     "fixed=%d va=0x%llx flags=0x%08x default=0x%08x "
                     "out=0x%llx rc=%d st=0x%x cvas=%d\n",
                     s->chip->name, flag_src, hClient, rmClient, hDevice,
                     rmDevice, hVas, rmVas, hMemory, rmMemory,
                     (unsigned long long)offset,
                     (unsigned long long)length, fixed,
                     (unsigned long long)va, flags, default_flags,
                     (unsigned long long)out, rc, out_st, s->m2_cur_cvas);
        }
    }
    if (st)     { *st = out_st; }
    if (out_va) { *out_va = out; }
    return rc;
}

/* M5.5: allocate an NV01_MEMORY_VIRTUAL (class 0x0070) mapper spanning a VASpace. RM's
 * RM_MAP_MEMORY_DMA mapper (hDma) must be a VirtualMemory resource — virtual_mem.c is the
 * only class that implements MapTo; vaspace_api.c does NOT — so a raw FERMI_VASPACE_A or
 * Device handle as hDma returns INVALID_OBJECT_HANDLE. NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS
 * (cl0070.h, 24B): offset@0(u64), limit@8(u64), hVASpace@16(u32) (NULL => device default,
 * else a FERMI_VASPACE_A). One mapper per vaspace, then many FIXED map_dma into it. */
static int nvkvm_m2_alloc_virtmem(NvkvmGpuEmul *s, uint32_t hClient, uint32_t hDevice,
                                  uint32_t hVirt, uint32_t hVASpace, uint32_t *st)
{
    uint8_t p[24];
    memset(p, 0, sizeof(p));
    stq_le_p(p + 0, 0);            /* offset = 0 */
    stq_le_p(p + 8, 0);            /* limit  = 0 (=> max) */
    stl_le_p(p + 16, hVASpace);    /* hVASpace (0 = device default) */
    return nvkvm_m2_alloc1(s, hClient, hDevice, hVirt, 0x0070u, p, sizeof(p), st);
}

/* M5.28 PER-CHANNEL VAS: get (allocating on first use) the fresh nvkvm-owned VAS context
 * for a (client, tsg). The fresh FERMI_VASPACE_A is allocated under the channel's FORWARDED
 * device (the TSG is parented to it, so RM requires the VAS share that device); a virtmem
 * mapper (NV01_MEMORY_VIRTUAL) spans it for FIXED map_dma. We substitute fvas into the GR
 * TSG's hVASpace (and its ctxshare) in shadow_fwd, and route the channel's working-set maps
 * here (m2_cur_cvas) instead of the guest's forwarded VAS — so every guest VA places into a
 * VAS WE fully control, killing the host-RM-self-promote collision (st=0x51 / Xid 32).
 * Returns index into m2_cvas[], or -1. */
static int nvkvm_m2_cvas_get(NvkvmGpuEmul *s, uint32_t client, uint32_t tsg)
{
    for (int i = 0; i < s->m2_cvas_n; i++) {
        if (s->m2_cvas[i].client == client && s->m2_cvas[i].tsg == tsg) { return i; }
    }
    if (s->m2_cvas_n >= 16) { return -1; }
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) { hDev = s->m2_devvas[i].dev; break; }
    }
    if (!hDev) {
        qemu_log("nvkvm-gpu[%s] M5.28 cvas_get: no forwarded device for client 0x%08x\n",
                 s->chip->name, client);
        return -1;
    }
    uint32_t fVas  = 0xce200000u | (s->m2_databuf_next++ & 0xffffu);
    uint32_t fVirt = 0xce300000u | (s->m2_databuf_next++ & 0xffffu);
    uint8_t vasp[56]; memset(vasp, 0, sizeof(vasp));
    uint32_t vst = 0xffff, vmst = 0xffff;
    nvkvm_m2_alloc1(s, client, hDev, fVas, 0x90f1u, vasp, sizeof(vasp), &vst);
    if (vst == 0) {
        nvkvm_m2_alloc_virtmem(s, client, hDev, fVirt, fVas, &vmst);
    }
    qemu_log("nvkvm-gpu[%s] M5.28 cvas_get: client=0x%08x tsg=0x%08x dev=0x%08x -> "
             "fresh vas=0x%08x(st=0x%x) virtmem=0x%08x(st=0x%x)%s\n", s->chip->name,
             client, tsg, hDev, fVas, vst, fVirt, vmst,
             (vst == 0 && vmst == 0) ? "  OK" : "  <-- ERR");
    if (vst != 0 || vmst != 0) { return -1; }
    int idx = s->m2_cvas_n++;
    s->m2_cvas[idx].client    = client;
    s->m2_cvas[idx].tsg       = tsg;
    s->m2_cvas[idx].hdev      = hDev;
    s->m2_cvas[idx].fvas      = fVas;
    s->m2_cvas[idx].fvirt     = fVirt;
    s->m2_cvas[idx].populated = false;
    return idx;
}

/* M5.7 EXECUTION PLANE: get (allocating once) the NV01_MEMORY_VIRTUAL mapper spanning a
 * client's GR VASpace. Returns the virtmem handle (0 on failure). The mapper is the hDma
 * for all FIXED map_dma into that vaspace. */
static uint32_t nvkvm_m2_grmapper(NvkvmGpuEmul *s, uint32_t client)
{
    /* M5.28: when a per-channel VAS is active for this client, route ALL FIXED map_dma
     * into ITS fresh nvkvm-owned virtmem mapper (not the guest's forwarded VAS). */
    if (s->m2_cur_cvas >= 0 && s->m2_cur_cvas < s->m2_cvas_n &&
        s->m2_cvas[s->m2_cur_cvas].client == client) {
        return s->m2_cvas[s->m2_cur_cvas].fvirt;
    }
    for (int i = 0; i < s->m2_grmap_n; i++) {
        if (s->m2_grmap[i].client == client) {
            return s->m2_grmap[i].hvirt;
        }
    }
    uint32_t hDev = 0, hVas = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) {
            hDev = s->m2_devvas[i].dev; hVas = s->m2_devvas[i].vas; break;
        }
    }
    if (!hDev || !hVas || s->m2_grmap_n >= 8) {
        qemu_log("nvkvm-gpu[%s] M5.7 grmapper: no dev/vas for client 0x%08x\n",
                 s->chip->name, client);
        return 0;
    }
    uint32_t hVirt = 0xdb000000u | (s->m2_databuf_next++ & 0xffffu);
    uint32_t st = 0xffff;
    nvkvm_m2_alloc_virtmem(s, client, hDev, hVirt, hVas, &st);
    if (st != 0) {
        /* M5.20 FRESH-VAS FALLBACK: the guest's forwarded VAS (0xcaf00000) for the
         * COMPUTE client is parented to a libcuda PROBE device (0x3141590x) and is an
         * index=3 / EXTERNALLY_OWNED-class VASpace; the host RM rejects NV01_MEMORY_
         * VIRTUAL over it with 0x57 (INSUFFICIENT_PERMISSIONS). The self-contained
         * selftest path (fresh client->device->vaspace->virtmem) succeeds, so allocate
         * a FRESH, normal RM-managed device+VASpace under THIS client and map into it.
         * The host compute channel is pointed at this fresh VAS in shadow_fwd (M5.21)
         * so its working set (pushbuffer/sema mapped here) resolves when it runs.
         * Contained to the failure path — the CeUtils clients (st==0 above) are
         * untouched. */
        uint32_t fDev  = 0xdf100000u | (s->m2_databuf_next++ & 0xffffu);
        uint32_t fVas  = 0xdf200000u | (s->m2_databuf_next++ & 0xffffu);
        uint32_t fVirt = 0xdf300000u | (s->m2_databuf_next++ & 0xffffu);
        uint8_t devp[56]; memset(devp, 0, sizeof(devp));
        uint8_t vasp[56]; memset(vasp, 0, sizeof(vasp));
        uint32_t dst = 0xffff, vst = 0xffff, st2 = 0xffff;
        nvkvm_m2_alloc1(s, client, client, fDev, 0x0080u, devp, sizeof(devp), &dst);
        nvkvm_m2_alloc1(s, client, fDev, fVas, 0x90f1u, vasp, sizeof(vasp), &vst);
        if (dst == 0 && vst == 0) {
            nvkvm_m2_alloc_virtmem(s, client, fDev, fVirt, fVas, &st2);
        }
        qemu_log("nvkvm-gpu[%s] M5.20 grmapper: guest-VAS 0x%08x virtmem st=0x%x -> "
                 "FRESH dev=0x%08x(st=0x%x) vas=0x%08x(st=0x%x) virtmem=0x%08x(st=0x%x)\n",
                 s->chip->name, hVas, st, fDev, dst, fVas, vst, fVirt, st2);
        if (!(dst == 0 && vst == 0 && st2 == 0)) {
            return 0;
        }
        hVirt = fVirt; hVas = fVas; hDev = fDev;
    }
    s->m2_grmap[s->m2_grmap_n].client = client;
    s->m2_grmap[s->m2_grmap_n].hvirt  = hVirt;
    s->m2_grmap[s->m2_grmap_n].hvas   = hVas;
    s->m2_grmap[s->m2_grmap_n].hdev   = hDev;
    s->m2_grmap_n++;
    qemu_log("nvkvm-gpu[%s] M5.7 grmapper: client 0x%08x -> virtmem 0x%08x over VAS "
             "0x%08x (dev 0x%08x)\n", s->chip->name, client, hVirt, hVas, hDev);
    return hVirt;
}

/* M5.7 EXECUTION PLANE unit op: back a guest working-set buffer with real host GPU vidmem
 * and place it in the GR channel's address space at the guest's VA.
 *  (1) alloc host vidmem(size) under the GR client+device;
 *  (2) double-mmap it into m2_fbback at the guest-FB phys, so guest CPU access (BAR/PRAMIN
 *      -> nvkvm_fb_read/write) and the host GPU share the SAME bytes (no faking);
 *  (3) map_dma FIXED at the guest VA into the client's GR virtmem mapper, so the host GPU's
 *      MMU resolves that VA to this memory when it runs the channel.
 * Returns true on success. CONTENT DIRECTION: `copy_content`=true for GUEST-written buffers
 * (GPFIFO/pushbuffers) -> copy the guest's current FB bytes into the host vidmem so the host
 * GPU reads the real commands; false for GPU-written buffers (completion semaphore) which the
 * host fills. The FB overlay is registered ONLY on a successful PLACE (st=0): a 0x51
 * (already-host-mapped, e.g. ctx) must NOT be overlaid or we'd shadow the host's real buffer
 * with zeroed memory. phys==0 => VA-only mapping (no overlay). */
static bool nvkvm_m2_back_and_map(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                  uint64_t phys, uint64_t size, bool copy_content,
                                  const char *label)
{
    uint32_t hVirt = nvkvm_m2_grmapper(s, client);
    if (!hVirt) {
        return false;
    }
    uint32_t hDev = 0;
    if (s->m2_cur_cvas >= 0 && s->m2_cur_cvas < s->m2_cvas_n &&
        s->m2_cvas[s->m2_cur_cvas].client == client) {
        hDev = s->m2_cvas[s->m2_cur_cvas].hdev;
    }
    if (!hDev) {
        for (int i = 0; i < s->m2_grmap_n; i++) {
            if (s->m2_grmap[i].client == client) {
                hDev = s->m2_grmap[i].hdev;
                break;
            }
        }
    }
    uint64_t map_va = va & ~0xffffull;
    uint64_t va_delta = va - map_va;
    uint64_t asize = (va_delta + size + 0xffffull) & ~0xffffull;
    if (!asize) {
        asize = 0x10000ull;
    }
    if (s->m2_fbback_n >= NVKVM_M2_MAX_FBBACK) {
        qemu_log("nvkvm-gpu[%s] M5.7 back_and_map: m2_fbback full\n", s->chip->name);
        return false;
    }
    uint32_t hMem = 0xdc000000u | (s->m2_databuf_next++ & 0xffffu);
    struct nvkvm_host_map hm;
    if (!nvkvm_m2_host_alloc_map_vidmem(s, client, hDev, hMem, asize, &hm)) {
        qemu_log("nvkvm-gpu[%s] M5.7 back_and_map[%s]: host vidmem alloc failed\n",
                 s->chip->name, label);
        return false;
    }
    uint32_t st = 0xffff; uint64_t outva = 0;
    int rc = nvkvm_m2_map_dma(s, client, hDev, hVirt, hMem, 0, asize, true,
                              map_va, &st, &outva);
    /* st=0x51 (NV_ERR_NO_MEMORY) on a FIXED map => the VA is ALREADY mapped in the host
     * VASpace (host RM self-promoted its GR ctx at the same VAs). Desired for ctx buffers —
     * host already has them; do NOT overlay. Only genuinely-unmapped buffers get placed. */
    bool already = (st == 0x51u);
    bool ok = (rc == 0 && st == 0 && outva == map_va);
    if (phys && ok) {                            /* overlay ONLY a buffer we actually placed */
        if (copy_content) {                      /* preserve guest-written bytes (cmds) */
            uint64_t copy_size = (size + 0xfffull) & ~0xfffull;
            if (copy_size > asize - va_delta) {
                copy_size = asize - va_delta;
            }
            for (uint64_t off = 0; off < copy_size; off += 4096) {
                uint8_t *src = nvkvm_fb_host_overlay(s, phys + off);
                if (src) {
                    nvkvm_m2_invalidate_host_cpu_range(src, 4096);
                } else {
                    src = nvkvm_fb_page(s, phys + off, false);
                }
                if (src) {
                    memcpy((uint8_t *)hm.qva + va_delta + off, src, 4096);
                }
            }
        }
        s->m2_fbback[s->m2_fbback_n].fb_base  = phys;
        s->m2_fbback[s->m2_fbback_n].size     = asize - va_delta;
        s->m2_fbback[s->m2_fbback_n].host_qva = (uint8_t *)hm.qva + va_delta;
        s->m2_fbback_n++;
    }
    qemu_log("nvkvm-gpu[%s] M5.7 back_and_map[%s] VA=0x%llx phys=0x%llx size=0x%llx "
             "map_va=0x%llx delta=0x%llx map_size=0x%llx copy=%d -> "
             "hMem=0x%08x qva=%p map rc=%d st=0x%x va=0x%llx%s\n",
             s->chip->name, label, (unsigned long long)va,
             (unsigned long long)phys, (unsigned long long)size,
             (unsigned long long)map_va, (unsigned long long)va_delta,
             (unsigned long long)asize, copy_content, hMem, hm.qva, rc, st,
             (unsigned long long)outva,
             ok ? "  OK PLACED" : already ? "  ALREADY-HOST-MAPPED" : "  <-- ERR");
    return ok || already;
}

static void *nvkvm_m2_host_userd_qva(NvkvmGpuEmul *s, uint32_t client, uint32_t hobject)
{
    for (int k = 0; k < s->m2_chanbuf_n; k++) {
        if (s->m2_chanbuf[k].client == client &&
            s->m2_chanbuf[k].chan == hobject) {
            return s->m2_chanbuf[k].qva;
        }
    }
    return NULL;
}

static void nvkvm_m2_ring_host_doorbell(NvkvmGpuEmul *s, uint32_t token)
{
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((uint8_t *)s->m2_usermode_qva + 0x90);

    __sync_synchronize();
    *doorbell = token;
    __sync_synchronize();
}

static void nvkvm_m2_probe_gpfifo_pre_ring(NvkvmGpuEmul *s,
                                           const struct nvkvm_chan_entry *c,
                                           uint32_t from, uint32_t to,
                                           const char *tag)
{
    if (!s->trace || !c || !c->gpfifo_phys || from >= to ||
        to > c->gpfifo_ent) {
        return;
    }

    static uint32_t logs;
    if (logs >= 512) {
        return;
    }

    uint32_t start = (to > from + 4u) ? (to - 4u) : from;
    for (uint32_t idx = start; idx < to && logs < 512; idx++) {
        uint64_t epa = c->gpfifo_phys + (uint64_t)idx * 8u;
        uint8_t *hp = nvkvm_fb_host_overlay(s, epa);
        uint8_t *lp = nvkvm_fb_page(s, epa, false);
        uint32_t lo = epa & 0xfffu;
        uint32_t he0 = 0, he1 = 0, le0 = 0, le1 = 0;
        bool have_h = hp != NULL;
        bool have_l = lp && lo + 8u <= 0x1000u;

        if (have_h) {
            nvkvm_m2_flush_host_cpu_range(hp, 8);
            he0 = ldl_le_p(hp + 0);
            he1 = ldl_le_p(hp + 4);
        }
        if (have_l) {
            le0 = ldl_le_p(lp + lo);
            le1 = ldl_le_p(lp + lo + 4u);
        }

        uint32_t e0 = have_h ? he0 : le0;
        uint32_t e1 = have_h ? he1 : le1;
        uint64_t pb = (uint64_t)(e0 & 0xFFFFFFFCu) |
                      ((uint64_t)(e1 & 0xFFu) << 32);
        uint32_t pblen = (e1 >> 10) & 0x1FFFFFu;

        logs++;
        qemu_log("nvkvm-gpu[%s] M8.94 HOST_GPFIFO_PRE_RING %s "
                 "chan=0x%08x idx=%u range=%u->%u phys=0x%llx "
                 "host=%s %08x,%08x local=%s %08x,%08x "
                 "pb=0x%llx words=%u%s\n",
                 s->chip->name, tag ? tag : "ring", c->hobject, idx,
                 from, to, (unsigned long long)epa,
                 have_h ? "ok" : "miss", he0, he1,
                 have_l ? "ok" : "miss", le0, le1,
                 (unsigned long long)pb, pblen,
                 (have_h && have_l && (he0 != le0 || he1 != le1)) ?
                 " HOST-LOCAL-DIVERGE" : "");
    }
}

static void nvkvm_m2_write_guest_userd_gp_get(NvkvmGpuEmul *s,
                                              const struct nvkvm_chan_entry *c,
                                              uint32_t gp_get,
                                              const char *why)
{
    if (!c || !c->userd) {
        return;
    }
    if (c->userd_sys) {
        nvkvm_phys_wr32(s, c->userd + 0x88, true, gp_get);
    } else {
        nvkvm_fb_write(s, c->userd + 0x88, gp_get, 4);
    }

    static uint32_t log_cnt;
    if (log_cnt++ < 256) {
        qemu_log("nvkvm-gpu[%s] M8.28 guest USERD GP_GET %s "
                 "chan=0x%08x userd=0x%llx(%s) <- %u\n",
                 s->chip->name, why ? why : "update", c->hobject,
                 (unsigned long long)c->userd,
                 c->userd_sys ? "sys" : "fb", gp_get);
    }
}

static void nvkvm_m2_write_guest_userd_gp_put(NvkvmGpuEmul *s,
                                              const struct nvkvm_chan_entry *c,
                                              uint32_t gp_put,
                                              const char *why)
{
    if (!c || !c->userd || gp_put > c->gpfifo_ent) {
        return;
    }
    if (c->userd_sys) {
        nvkvm_phys_wr32(s, c->userd + 0x8C, true, gp_put);
    } else {
        nvkvm_fb_write(s, c->userd + 0x8C, gp_put, 4);
    }

    static uint32_t log_cnt;
    if (log_cnt++ < 256) {
        qemu_log("nvkvm-gpu[%s] M8.101 guest USERD GP_PUT %s "
                 "chan=0x%08x userd=0x%llx(%s) <- %u\n",
                 s->chip->name, why ? why : "update", c->hobject,
                 (unsigned long long)c->userd,
                 c->userd_sys ? "sys" : "fb", gp_put);
    }
}

static void nvkvm_m2_repair_guest_userd_gp_put(NvkvmGpuEmul *s,
                                               const struct nvkvm_chan_entry *c,
                                               uint32_t submitted_put,
                                               const char *why)
{
    if (!c || !c->userd || submitted_put > c->gpfifo_ent) {
        return;
    }

    uint32_t cur_put = c->userd_sys ?
        nvkvm_phys_rd32(s, c->userd + 0x8C, true) :
        (uint32_t)nvkvm_fb_read(s, c->userd + 0x8C, 4);

    if (cur_put == submitted_put || cur_put > submitted_put) {
        return;
    }

    nvkvm_m2_write_guest_userd_gp_put(s, c, submitted_put, why);
}

static bool nvkvm_m2_host_tsg_is_scheduled(NvkvmGpuEmul *s, uint32_t client,
                                           uint32_t tsg)
{
    for (int i = 0; i < s->chan_n; i++) {
        if (s->chans[i].client == client && s->chans[i].tsg == tsg &&
            s->chans[i].scheduled) {
            return true;
        }
    }
    return false;
}

static void nvkvm_m2_mark_host_tsg_scheduled(NvkvmGpuEmul *s, uint32_t client,
                                             uint32_t tsg,
                                             bool scheduled)
{
    for (int i = 0; i < s->chan_n; i++) {
        if (s->chans[i].client == client && s->chans[i].tsg == tsg) {
            s->chans[i].scheduled = scheduled;
        }
    }
}

static bool nvkvm_m2_schedule_host_tsg(NvkvmGpuEmul *s, uint32_t client,
                                       uint32_t tsg,
                                       const char *why)
{
    if (!s->m2fwd || !client || !tsg) {
        return false;
    }
    if (nvkvm_m2_host_tsg_is_scheduled(s, client, tsg)) {
        return true;
    }

    uint8_t sp[3] = {1, 0, 0};
    uint32_t st = 0xffffu;
    int rc = nvkvm_m2_control1(s, client, tsg, 0xa06c0101u, sp, sizeof(sp), &st);
    bool ok = (rc == 0 && st == 0);
    if (ok) {
        nvkvm_m2_mark_host_tsg_scheduled(s, client, tsg, true);
    }
    qemu_log("nvkvm-gpu[%s] M8.27 host GR schedule TSG=0x%08x client=0x%08x "
             "why=%s rc=%d st=0x%x%s\n",
             s->chip->name, tsg, client, why ? why : "ring", rc, st,
             ok ? "  OK" : "  <-- ERR");
    return ok;
}

static bool nvkvm_m2_read_channel_notify_slot(NvkvmGpuEmul *s,
                                              const struct nvkvm_chan_entry *c,
                                              uint32_t index,
                                              uint32_t v[4])
{
    if (!c || !v || !c->err_notifier_base) {
        return false;
    }

    uint64_t off = (uint64_t)index * NVKVM_M2_NV_NOTIFICATION_SIZE;
    if (c->err_notifier_size < off + NVKVM_M2_NV_NOTIFICATION_SIZE) {
        return false;
    }

    uint64_t addr = c->err_notifier_base + off;
    for (uint32_t i = 0; i < 4; i++) {
        if (c->err_notifier_as == NVKVM_M2_ADDR_SYSMEM) {
            v[i] = nvkvm_phys_rd32(s, addr + (uint64_t)i * 4u, true);
        } else if (c->err_notifier_as == NVKVM_M2_ADDR_FBMEM) {
            v[i] = (uint32_t)nvkvm_fb_read(s, addr + (uint64_t)i * 4u, 4);
        } else {
            return false;
        }
    }
    return true;
}

static uint32_t nvkvm_m2_diag_object_get(NvkvmGpuEmul *s, uint32_t client)
{
    for (int i = 0; i < s->m2_diag_n; i++) {
        if (s->m2_diag[i].client == client) {
            return s->m2_diag[i].failed ? 0 : s->m2_diag[i].diag;
        }
    }
    if (s->m2_diag_n >= 16) {
        return 0;
    }

    uint32_t subdev = 0;
    for (int i = 0; i < s->m2_subdev_n; i++) {
        if (s->m2_subdev[i].client == client) {
            subdev = s->m2_subdev[i].subdev;
            break;
        }
    }
    if (!subdev) {
        return 0;
    }

    uint32_t diag = 0xd1a90000u | (s->m2_databuf_next++ & 0xffffu);
    uint32_t st = 0xffffu;
    int rc = nvkvm_m2_alloc1(s, client, subdev, diag, 0x208fu,
                             NULL, 0, &st);
    int idx = s->m2_diag_n++;
    s->m2_diag[idx].client = client;
    s->m2_diag[idx].subdev = subdev;
    s->m2_diag[idx].diag = diag;
    s->m2_diag[idx].failed = !(rc == 0 && st == 0);

    qemu_log("nvkvm-gpu[%s] M8.81 DIAG_ALLOC client=0x%08x subdev=0x%08x "
             "diag=0x%08x rc=%d st=0x%x%s\n",
             s->chip->name, client, subdev, diag, rc, st,
             s->m2_diag[idx].failed ? "  <-- ERR" : "  OK");
    return s->m2_diag[idx].failed ? 0 : diag;
}

static uint32_t nvkvm_m2_debug_object_for_client(NvkvmGpuEmul *s,
                                                 uint32_t client)
{
    for (int i = 0; i < s->m2_debug_n; i++) {
        if (s->m2_debug[i].client == client) {
            return s->m2_debug[i].hobject;
        }
    }
    return 0;
}

static void nvkvm_m2_log_post_launch_rm_state(NvkvmGpuEmul *s,
                                              const struct nvkvm_chan_entry *c,
                                              const char *why)
{
    if (!c || s->m2_post_launch_diag_logs++ >= 16) {
        return;
    }

    uint32_t diag = nvkvm_m2_diag_object_get(s, c->client);
    if (diag) {
        uint8_t p[16];
        memset(p, 0, sizeof(p));
        stl_le_p(p + 0, c->hobject);
        stl_le_p(p + 4, nvkvm_m2_client(s, c->client));
        uint32_t st = 0xffffu;
        int rc = nvkvm_m2_control1(s, c->client, diag, 0x208f0403u,
                                   p, sizeof(p), &st);
        qemu_log("nvkvm-gpu[%s] M8.81 DIAG_CHANNEL_STATE %s "
                 "chan=0x%08x client=0x%08x hostClient=0x%08x "
                 "diag=0x%08x rc=%d st=0x%x enabled=%u scheduled=%u "
                 "cpuMap=%u contention=%u runlistSet=%u deferRC=%u\n",
                 s->chip->name, why ? why : "post-launch", c->hobject,
                 c->client, nvkvm_m2_client(s, c->client), diag, rc, st,
                 p[8], p[9], p[10], p[11], p[12], p[13]);
    }

    {
        uint8_t p[1] = {0};
        uint32_t st = 0xffffu;
        int rc = nvkvm_m2_control1(s, c->client, c->hobject, 0x906f0105u,
                                   p, sizeof(p), &st);
        qemu_log("nvkvm-gpu[%s] M8.83 DEFER_RC_STATE %s "
                 "chan=0x%08x rc=%d st=0x%x pending=%u\n",
                 s->chip->name, why ? why : "post-launch", c->hobject,
                 rc, st, p[0]);
    }

    {
        enum { MMU_FAULT_PARAMS_SIZE = 104, MMU_FAULT_SHADER_VA_OFF = 48 };
        uint8_t p[MMU_FAULT_PARAMS_SIZE];
        char fault_string[33];
        memset(p, 0, sizeof(p));
        uint32_t st = 0xffffu;
        int rc = nvkvm_m2_control1(s, c->client, c->hobject, 0x906f0106u,
                                   p, sizeof(p), &st);
        memcpy(fault_string, p + 12, 32);
        fault_string[32] = 0;
        for (int i = 0; i < 32; i++) {
            if ((unsigned char)fault_string[i] < 0x20 ||
                (unsigned char)fault_string[i] > 0x7e) {
                fault_string[i] = fault_string[i] ? '.' : 0;
            }
        }
        qemu_log("nvkvm-gpu[%s] M8.83 MMU_FAULT_INFO %s "
                 "chan=0x%08x rc=%d st=0x%x addr=0x%08x%08x "
                 "type=0x%08x str=\"%s\" shaderVA=[0x%llx,0x%llx,"
                 "0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
                 s->chip->name, why ? why : "post-launch", c->hobject,
                 rc, st, ldl_le_p(p + 0), ldl_le_p(p + 4),
                 ldl_le_p(p + 8), fault_string,
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 0),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 8),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 16),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 24),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 32),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 40),
                 (unsigned long long)ldq_le_p(p + MMU_FAULT_SHADER_VA_OFF + 48));
    }

    uint32_t dbg = nvkvm_m2_debug_object_for_client(s, c->client);
    if (dbg) {
        enum { SM_PARAMS_SIZE = 4824 };
        uint8_t p[SM_PARAMS_SIZE];
        memset(p, 0, sizeof(p));
        stl_le_p(p + 0, c->hobject);
        stl_le_p(p + 4, 1u);
        stl_le_p(p + 4820, 0u);
        uint32_t st = 0xffffu;
        int rc = nvkvm_m2_control1(s, c->client, dbg, 0x83de030cu,
                                   p, sizeof(p), &st);
        uint32_t global = ldl_le_p(p + 8);
        uint32_t warp = ldl_le_p(p + 12);
        uint32_t global_mask = ldl_le_p(p + 20);
        uint32_t warp_mask = ldl_le_p(p + 24);
        uint64_t esr_addr = ldq_le_p(p + 32);
        uint64_t pc64 = ldq_le_p(p + 40);
        uint32_t cga = ldl_le_p(p + 48);
        uint32_t cga_mask = ldl_le_p(p + 52);
        uint32_t mmu_old = ldl_le_p(p + 4808);
        uint32_t mmu_valid = ldl_le_p(p + 4812);
        uint32_t mmu_info = ldl_le_p(p + 4816);
        qemu_log("nvkvm-gpu[%s] M8.82 SM_ERROR_STATE %s "
                 "dbg=0x%08x chan=0x%08x rc=%d st=0x%x "
                 "global=0x%08x warp=0x%08x globalMask=0x%08x "
                 "warpMask=0x%08x esrAddr=0x%llx pc=0x%llx "
                 "cga=0x%08x cgaMask=0x%08x mmuOld=0x%08x "
                 "mmuValid=%u mmuInfo=0x%08x\n",
                 s->chip->name, why ? why : "post-launch", dbg,
                 c->hobject, rc, st, global, warp, global_mask, warp_mask,
                 (unsigned long long)esr_addr, (unsigned long long)pc64,
                 cga, cga_mask, mmu_old, mmu_valid, mmu_info);
    }
}

static void nvkvm_m2_log_host_channel_state(NvkvmGpuEmul *s,
                                            const struct nvkvm_chan_entry *c,
                                            const char *why)
{
    if (!c) {
        return;
    }

    static uint32_t state_logs;
    if (state_logs++ >= 128) {
        return;
    }

    uint8_t b06f[4] = {0};
    uint8_t c56f[4] = {0};
    uint32_t bst = 0xffffu, cst = 0xffffu;
    int brc = nvkvm_m2_control1(s, c->client, c->hobject, 0xb06f010fu,
                                b06f, sizeof(b06f), &bst);
    int crc = nvkvm_m2_control1(s, c->client, c->hobject, 0xc56f010fu,
                                c56f, sizeof(c56f), &cst);
    uint32_t err[4] = {0}, work[4] = {0};
    bool have_err = nvkvm_m2_read_channel_notify_slot(s, c, 0, err);
    bool have_work = nvkvm_m2_read_channel_notify_slot(
        s, c, c->work_submit_notifier_index, work);

    qemu_log("nvkvm-gpu[%s] M8.73 HOSTGR_STATE %s chan=0x%08x "
             "b06f010f rc=%d st=0x%x state=0x%08x "
             "c56f010f rc=%d st=0x%x state=0x%08x "
             "notify0=%s %08x,%08x,%08x,%08x "
             "notify%u=%s %08x,%08x,%08x,%08x\n",
             s->chip->name, why ? why : "state", c->hobject,
             brc, bst, ldl_le_p(b06f), crc, cst, ldl_le_p(c56f),
             have_err ? "ok" : "miss", err[0], err[1], err[2], err[3],
             c->work_submit_notifier_index,
             have_work ? "ok" : "miss", work[0], work[1], work[2], work[3]);
}

static bool nvkvm_m2_finish_host_gr_get(NvkvmGpuEmul *s, int ch_index,
                                        struct nvkvm_chan_entry *c,
                                        uint32_t hget, uint32_t token,
                                        bool token_valid, const char *why)
{
    if (!c || hget == c->gp_get || hget > c->gpfifo_ent) {
        return false;
    }
    if (hget < c->gp_get) {
        static uint32_t wrap_logs;
        if (s->trace && wrap_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.105 HOSTGR complete-wrap-defer ch[%d] "
                     "chan=0x%08x gp_get=%u host_get=%u ent=%u via %s\n",
                     s->chip->name, ch_index, c->hobject, c->gp_get, hget,
                     c->gpfifo_ent, why ? why : "host-get");
        }
        return false;
    }

    uint32_t old_get = c->gp_get;
    s->chan_gpfifo_va  = c->gpfifo_va;
    s->chan_userd      = c->userd;
    s->chan_gpfifo_ent = c->gpfifo_ent;
    s->chan_userd_sys  = c->userd_sys;
    s->chan_hvaspace   = c->hvaspace;
    s->chan_client     = c->client;
    s->chan_gp_get     = c->gp_get;
    s->chan_gpfifo_phys = c->gpfifo_phys;
    s->chan_pdb        = c->pdb;
    nvkvm_m2_release_gr_reports_for_gpfifo(s, c->gpfifo_phys, old_get, hget,
                                           c->gpfifo_ent, why);
    c->gp_get = hget;
    c->host_inflight = false;
    c->host_inflight_get = 0;
    c->host_inflight_put = 0;
    c->host_inflight_polls = 0;

    if (token_valid) {
        nvkvm_m2_write_work_submit_notifier(s, c, token, why);
    }
    nvkvm_m2_repair_guest_userd_gp_put(s, c, hget, why);
    nvkvm_m2_write_guest_userd_gp_get(s, c, hget, why);
    nvkvm_m2_queue_host_completion(s, token, token_valid, why);

    qemu_log("nvkvm-gpu[%s] M8.105 HOSTGR complete ch[%d] "
             "chan=0x%08x gp_get=%u->%u token=%s0x%08x via %s\n",
             s->chip->name, ch_index, c->hobject, old_get, hget,
             token_valid ? "" : "!", token, why ? why : "host-get");
    return true;
}

static void nvkvm_m2_ring_host_channel(NvkvmGpuEmul *s, int ch_index,
                                       struct nvkvm_chan_entry *c,
                                       uint32_t token, const char *tag,
                                       uint32_t ring_put_override,
                                       bool ring_put_override_valid)
{
    if (nvkvm_m2_tsg_engine(s, c->tsg) == 1u &&
        !nvkvm_m2_schedule_host_tsg(s, c->client, c->tsg, tag)) {
        qemu_log("nvkvm-gpu[%s] M8.27 HOSTGR %s ch[%d] token=0x%08x "
                 "schedule failed; ring skipped\n",
                 s->chip->name, tag ? tag : "ring", ch_index, token);
        return;
    }

    void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
    uint32_t get0 = uqva ? ldl_le_p((uint8_t *)uqva + 0x88) : 0xffffffffu;
    uint32_t put0 = uqva ? ldl_le_p((uint8_t *)uqva + 0x8C) : 0xffffffffu;
    uint32_t guest_put = 0xffffffffu;
    if (c->userd) {
        guest_put = c->userd_sys ?
            nvkvm_phys_rd32(s, c->userd + 0x8C, true) :
            (uint32_t)nvkvm_fb_read(s, c->userd + 0x8C, 4);
    }
    uint32_t ring_put = guest_put;
    if (ring_put_override_valid && ring_put_override <= c->gpfifo_ent) {
        static uint32_t override_logs;
        if (override_logs++ < 64 &&
            (ring_put == 0xffffffffu || ring_put != ring_put_override)) {
            qemu_log("nvkvm-gpu[%s] M8.100 HOST_USERD_PUT_OVERRIDE "
                     "ch[%d] token=0x%08x parser_put=%u guest_put=%u "
                     "host_put=%u get=%u; using parser-observed GP_PUT\n",
                     s->chip->name, ch_index, token, ring_put_override,
                     guest_put, put0, get0);
        }
        ring_put = ring_put_override;
    }
    bool override_wrap = ring_put_override_valid && get0 != 0xffffffffu &&
                         ring_put_override < get0;
    if (!override_wrap &&
        uqva && put0 != 0xffffffffu && put0 <= c->gpfifo_ent &&
        put0 >= get0 &&
        (ring_put == 0xffffffffu || ring_put < put0)) {
        static uint32_t preserve_logs;
        if (preserve_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.99 HOST_USERD_PUT_PRESERVE "
                     "ch[%d] token=0x%08x host_put=%u guest_put=%u "
                     "get=%u; keeping newer host USERD GP_PUT\n",
                     s->chip->name, ch_index, token, put0, guest_put, get0);
        }
        ring_put = put0;
    }
    static uint32_t log_cnt;
    bool do_log = log_cnt++ < 256;

    if (uqva && ring_put != 0xffffffffu) {
        if (ring_put != put0) {
            stl_le_p((uint8_t *)uqva + 0x8C, ring_put);
        }
        nvkvm_m2_flush_host_cpu_range((uint8_t *)uqva + 0x8C, 4);
    }
    nvkvm_m2_host_gpu_store_fence();
    if (do_log) {
        qemu_log("nvkvm-gpu[%s] M8.64 HOST_GPU_STORE_FENCE before doorbell "
                 "ch[%d] token=0x%08x guest_put=%u ring_put=%u host_put0=%u\n",
                 s->chip->name, ch_index, token, guest_put, ring_put, put0);
    }
    if (uqva && ring_put != 0xffffffffu && get0 <= ring_put &&
        ring_put <= c->gpfifo_ent) {
        nvkvm_m2_probe_gpfifo_pre_ring(s, c, get0, ring_put, tag);
    }
    nvkvm_m2_ring_host_doorbell(s, token);

    if (do_log && uqva) {
        g_usleep(1000);
    }

    uint32_t get1 = uqva ? ldl_le_p((uint8_t *)uqva + 0x88) : 0xffffffffu;
    uint32_t put1 = uqva ? ldl_le_p((uint8_t *)uqva + 0x8C) : 0xffffffffu;

    if (do_log) {
        qemu_log("nvkvm-gpu[%s] M8.17 HOSTGR %s ch[%d] token=0x%08x "
                 "(client=0x%08x hObj=0x%08x gpfifo=0x%llx) "
                 "hostUSERD get=%u->%u put=%u->%u guest_put=%u ring_put=%u%s\n",
                 s->chip->name, tag ? tag : "ring", ch_index, token,
                 c->client, c->hobject, (unsigned long long)c->gpfifo_va,
                 get0, get1, put0, put1, guest_put, ring_put,
                 uqva ? "" : " [no host USERD qva]");
    }
}

/* M5.8 DOORBELL-FORWARD setup (no ring): alloc the host AMPERE_USERMODE_A (0xc561) doorbell
 * register page under the GR client's subdevice, RM_MAP_MEMORY + mmap it into QEMU, and fetch
 * the host GR channel's work-submit token (NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN
 * 0xc36f0108). To RING (later, once pushbuffers are mapped + channel scheduled) we write the
 * token to usermode_qva + NVC361_NOTIFY_CHANNEL_PENDING (0x90) -> the HOST GPU runs the
 * channel. NOT rung here: ringing before the working set is mapped/scheduled would fault the
 * host GPU (wedge). This validates the two new primitives (usermode map + token). */
static void nvkvm_m2_doorbell_setup(NvkvmGpuEmul *s, uint32_t client)
{
    if (s->m2_doorbell_ready || !s->m2_gr_channel) {
        return;
    }
    uint32_t hDev = 0, subdev = 0;
    for (int i = 0; i < s->m2_grmap_n; i++) {
        if (s->m2_grmap[i].client == client) { hDev = s->m2_grmap[i].hdev; break; }
    }
    for (int i = 0; i < s->m2_subdev_n; i++) {
        if (s->m2_subdev[i].client == client) { subdev = s->m2_subdev[i].subdev; break; }
    }
    if (!hDev || !subdev) {
        qemu_log("nvkvm-gpu[%s] M5.8 doorbell: no dev/subdev for client 0x%08x\n",
                 s->chip->name, client);
        return;
    }
    uint32_t hUM = 0xde900001u, st = 0xffff;
    nvkvm_m2_alloc1(s, client, subdev, hUM, 0xc561u, NULL, 0, &st);   /* AMPERE_USERMODE_A */
    qemu_log("nvkvm-gpu[%s] M5.8 doorbell: AMPERE_USERMODE_A alloc st=0x%x\n",
             s->chip->name, st);
    if (st != 0) {
        return;
    }
    if (s->m2_maph_next < 16) { s->m2_maph_next = 16; }
    uint32_t maph = s->m2_maph_next++;
    int mapfd = -1;
    if (nvkvm_isolate_open_device(&s->m2_iso, s->m2_iso_id, maph, NVKVM_DEV_GPU(0),
                                  O_RDWR, &mapfd) != 0 || mapfd < 0) {
        qemu_log("nvkvm-gpu[%s] M5.8 doorbell: usermode map-fd open failed\n", s->chip->name);
        return;
    }
    struct nv_ioctl_nvos33_parameters_with_fd mm;
    memset(&mm, 0, sizeof(mm));
    mm.h_client = nvkvm_m2_client(s, client);
    mm.h_device = hDev;
    mm.h_memory = hUM;
    mm.length   = 0x10000;                        /* NVC361_NV_USERMODE__SIZE = 64 KiB */
    mm.fd       = (int32_t)maph;
    unsigned int mc = (3u << 30) | ((unsigned int)sizeof(mm) << 16) |
                      ((unsigned int)'F' << 8) | NV_ESC_RM_MAP_MEMORY;
    uint32_t mnv = 0; uint64_t mf = 0;
    int rc = nvkvm_isolate_ioctl(&s->m2_iso, s->m2_iso_id, s->m2_ctl_h, mc,
                                 &mm, sizeof(mm), NULL, 0, 0, &mnv, &mf);
    if (rc != 0 || mm.status != 0) {
        qemu_log("nvkvm-gpu[%s] M5.8 doorbell: usermode RM_MAP_MEMORY rc=%d st=0x%x\n",
                 s->chip->name, rc, mm.status);
        return;
    }
    void *qva = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, mapfd, 0);
    if (qva == MAP_FAILED) {
        qemu_log("nvkvm-gpu[%s] M5.8 doorbell: usermode mmap failed: %s\n",
                 s->chip->name, strerror(errno));
        return;
    }
    s->m2_usermode_qva = qva;
    uint8_t tp[4]; memset(tp, 0, sizeof(tp));
    uint32_t tst = 0xffff;
    int trc = nvkvm_m2_control1(s, client, s->m2_gr_channel, 0xc36f0108u, tp, 4, &tst);
    s->m2_gr_token = ldl_le_p(tp);
    s->m2_doorbell_ready = (trc == 0 && tst == 0);
    qemu_log("nvkvm-gpu[%s] M5.8 doorbell: usermode qva=%p GR chan=0x%08x WORK_SUBMIT_TOKEN "
             "trc=%d st=0x%x token=0x%08x -> %s\n", s->chip->name, qva, s->m2_gr_channel,
             trc, tst, s->m2_gr_token,
             s->m2_doorbell_ready ? "READY (ring deferred until pushbuffers mapped+scheduled)"
                                  : "TOKEN-FAILED");
    /* M8.27: do not schedule the host GR TSG here. Host USERD/GPFIFO mirrors the
     * guest, so schedule alone can let the host GPU fetch UVM bootstrap work that
     * QEMU intended to soft-complete. Schedule lazily only at the actual HOSTGR ring. */
    if (s->m2_gr_tsg) {
        qemu_log("nvkvm-gpu[%s] M8.27 doorbell: defer host GPFIFO_SCHEDULE "
                 "TSG=0x%08x until real HOSTGR ring\n",
                 s->chip->name, s->m2_gr_tsg);
    }
}

/* M5.9: resolve a GR-VAS guest VA -> guest-FB phys by trying each snooped VAS PDB (FB leaf
 * only; sysmem leaves are the GPU->CPU DMA path, handled elsewhere). 0 on miss. */
static uint64_t nvkvm_m2_resolve_fb(NvkvmGpuEmul *s, uint64_t va)
{
    for (int v = 0; v < s->chan_vas_n; v++) {
        bool sy = false;
        uint64_t p = nvkvm_walk_pdb(s, s->chan_vas[v].pdb, va, &sy);
        if (p != NVKVM_GMMU_FAULT && !sy) { return p; }
    }
    return 0;
}
/* M5.9: has this VA already been backed+mapped? (dedup repeated pushbuffers). Adds if new. */
static bool nvkvm_m2_va_seen(NvkvmGpuEmul *s, uint32_t client, uint64_t va)
{
    if (nvkvm_m2_va_is_seen(s, client, va)) {
        return true;
    }
    if (s->m2_mapped_va_n < NVKVM_MAX_MAPPED_VA) {
        int cvas = s->m2_cur_cvas;
        s->m2_mapped_va[s->m2_mapped_va_n].client = client;
        s->m2_mapped_va[s->m2_mapped_va_n].cvas = cvas;
        s->m2_mapped_va[s->m2_mapped_va_n].va = va;
        s->m2_mapped_va_n++;
    }
    return false;
}

static bool nvkvm_m2_va_is_seen(NvkvmGpuEmul *s, uint32_t client, uint64_t va)
{
    int cvas = s->m2_cur_cvas;
    for (int i = 0; i < s->m2_mapped_va_n; i++) {
        if (s->m2_mapped_va[i].client == client &&
            s->m2_mapped_va[i].cvas == cvas &&
            s->m2_mapped_va[i].va == va) {
            return true;
        }
    }
    return false;
}

static void nvkvm_m2_va_forget(NvkvmGpuEmul *s, uint32_t client, uint64_t va)
{
    int cvas = s->m2_cur_cvas;
    for (int i = 0; i < s->m2_mapped_va_n; i++) {
        if (s->m2_mapped_va[i].client == client &&
            s->m2_mapped_va[i].cvas == cvas &&
            s->m2_mapped_va[i].va == va) {
            memmove(&s->m2_mapped_va[i], &s->m2_mapped_va[i + 1],
                    (size_t)(s->m2_mapped_va_n - i - 1) * sizeof(s->m2_mapped_va[0]));
            s->m2_mapped_va_n--;
            return;
        }
    }
}

static void nvkvm_m2_pbmap_reload(NvkvmGpuEmul *s)
{
    if (!s->m2pbmap_path || !s->m2pbmap_path[0]) {
        return;
    }
    struct stat st;
    if (stat(s->m2pbmap_path, &st) != 0) {
        return;
    }
    if (s->m2_pbmap_n &&
        s->m2_pbmap_mtime == st.st_mtime &&
        s->m2_pbmap_size == (int64_t)st.st_size) {
        return;
    }
    FILE *f = fopen(s->m2pbmap_path, "r");
    if (!f) {
        return;
    }
    int n = 0;
    for (;;) {
        unsigned long long va = 0, gpa = 0, size = 0;
        int rc = fscanf(f, "%llx %llx %llx", &va, &gpa, &size);
        if (rc == EOF) {
            break;
        }
        if (rc != 3) {
            int ch;
            do {
                ch = fgetc(f);
            } while (ch != EOF && ch != '\n');
            continue;
        }
        if (va && size && n < (int)ARRAY_SIZE(s->m2_pbmap)) {
            s->m2_pbmap[n].va = va;
            s->m2_pbmap[n].gpa = gpa;
            s->m2_pbmap[n].size = size;
            n++;
        }
    }
    fclose(f);
    s->m2_pbmap_n = n;
    s->m2_pbmap_mtime = st.st_mtime;
    s->m2_pbmap_size = st.st_size;
    if (s->m2_pbmap_logs++ < 16) {
        qemu_log("nvkvm-gpu[%s] M8.11 pbmap reload '%s': %d ranges\n",
                 s->chip->name, s->m2pbmap_path, n);
    }
}

static bool nvkvm_m2_pbmap_lookup(NvkvmGpuEmul *s, uint64_t va, uint64_t size,
                                  uint64_t *out_gpa)
{
    nvkvm_m2_pbmap_reload(s);
    if (!size) {
        size = 1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return false;
    }
    for (int i = 0; i < s->m2_pbmap_n; i++) {
        uint64_t rva = s->m2_pbmap[i].va;
        uint64_t rend = rva + s->m2_pbmap[i].size;
        if (rend < rva) {
            continue;
        }
        if (va >= rva && end <= rend) {
            if (out_gpa) {
                *out_gpa = s->m2_pbmap[i].gpa + (va - rva);
            }
            return true;
        }
    }
    return false;
}

static bool nvkvm_m2_bar1_gpa_to_off(NvkvmGpuEmul *s, uint64_t gpa,
                                      uint64_t size, uint64_t *out_off)
{
    if (!size) {
        size = 1;
    }
    uint64_t end = gpa + size;
    if (end < gpa) {
        return false;
    }

    pcibus_t base_pc = pci_get_bar_addr(PCI_DEVICE(s), 1);
    if (base_pc == PCI_BAR_UNMAPPED) {
        return false;
    }
    uint64_t base = (uint64_t)base_pc;
    uint64_t bar_end = base + s->chip->bar1_size;
    if (bar_end < base || gpa < base || end > bar_end) {
        return false;
    }
    if (out_off) {
        *out_off = gpa - base;
    }
    return true;
}

static bool nvkvm_m2_phys_read_buf(NvkvmGpuEmul *s, uint64_t phys, bool sys,
                                   void *dst, uint64_t size)
{
    uint8_t *out = dst;
    uint64_t done = 0;

    while (done < size) {
        uint64_t cur = phys + done;
        uint64_t chunk = 0x1000ull - (cur & 0xfffull);
        if (chunk > size - done) {
            chunk = size - done;
        }
        if (sys) {
            if (pci_dma_read(&s->parent_obj, cur, out + done, chunk) != MEMTX_OK) {
                return false;
            }
        } else {
            uint8_t *hp = nvkvm_fb_host_overlay(s, cur);
            if (hp) {
                nvkvm_m2_invalidate_host_cpu_range(hp, chunk);
                memcpy(out + done, hp, chunk);
            } else {
                uint8_t *p = nvkvm_fb_page(s, cur, false);
                if (p) {
                    memcpy(out + done, p + (cur & 0xfffull), chunk);
                } else {
                    memset(out + done, 0, chunk);
                }
            }
        }
        done += chunk;
    }
    return true;
}

static bool nvkvm_m2_phys_write_buf(NvkvmGpuEmul *s, uint64_t phys, bool sys,
                                    const void *src, uint64_t size)
{
    const uint8_t *in = src;
    uint64_t done = 0;

    while (done < size) {
        uint64_t cur = phys + done;
        uint64_t chunk = 0x1000ull - (cur & 0xfffull);
        if (chunk > size - done) {
            chunk = size - done;
        }
        if (sys) {
            if (nvkvm_dmaw(&s->parent_obj, cur, in + done, chunk) != MEMTX_OK) {
                return false;
            }
        } else {
            uint8_t *hp = nvkvm_fb_host_overlay(s, cur);
            if (hp) {
                memcpy(hp, in + done, chunk);
            } else {
                uint8_t *p = nvkvm_fb_page(s, cur, true);
                memcpy(p + (cur & 0xfffull), in + done, chunk);
            }
        }
        done += chunk;
    }
    return true;
}

static bool nvkvm_m2_uvm_shadow_lookup(NvkvmGpuEmul *s, uint64_t va,
                                        uint64_t size, uint64_t *out_gpa,
                                        bool *out_bar1)
{
    if (!size) {
        size = 1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return false;
    }
    for (int i = 0; i < s->m2_uvm_shadow_n; i++) {
        uint64_t rva = s->m2_uvm_shadow[i].va;
        uint64_t rend = rva + s->m2_uvm_shadow[i].size;
        if (rend < rva) {
            continue;
        }
        if (va >= rva && end <= rend) {
            if (out_gpa) {
                *out_gpa = s->m2_uvm_shadow[i].gpa + (va - rva);
            }
            if (out_bar1) {
                *out_bar1 = s->m2_uvm_shadow[i].bar1;
            }
            return true;
        }
    }
    return false;
}

static bool nvkvm_m2_uvm_shadow_resolve(NvkvmGpuEmul *s, uint64_t va,
                                         uint64_t size, uint64_t *out_phys,
                                         bool *out_sys)
{
    uint64_t raw = 0;
    bool bar1 = false;
    if (!nvkvm_m2_uvm_shadow_lookup(s, va, size, &raw, &bar1)) {
        return false;
    }
    if (!bar1) {
        if (out_phys) {
            *out_phys = raw;
        }
        if (out_sys) {
            *out_sys = true;
        }
        return true;
    }

    uint64_t off = 0;
    if (!nvkvm_m2_bar1_gpa_to_off(s, raw, size, &off) || !s->bar1_pdb) {
        return false;
    }
    bool sys = false;
    uint64_t phys = nvkvm_walk_pdb(s, s->bar1_pdb, off, &sys);
    if (phys == NVKVM_GMMU_FAULT) {
        static uint32_t fault_logs;
        if (fault_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.48 UVM-SHADOW BAR1 fault "
                     "VA=0x%llx raw=0x%llx off=0x%llx size=0x%llx\n",
                     s->chip->name, (unsigned long long)va,
                     (unsigned long long)raw, (unsigned long long)off,
                     (unsigned long long)size);
        }
        return false;
    }
    if (out_phys) {
        *out_phys = phys;
    }
    if (out_sys) {
        *out_sys = sys;
    }
    return true;
}

static bool nvkvm_m2_uvm_shadow_read(NvkvmGpuEmul *s, uint64_t va,
                                      void *dst, uint64_t size)
{
    uint8_t *out = dst;
    uint64_t done = 0;

    while (done < size) {
        uint64_t chunk = 0x1000ull - ((va + done) & 0xfffull);
        if (chunk > size - done) {
            chunk = size - done;
        }
        uint64_t phys = 0;
        bool sys = false;
        if (!nvkvm_m2_uvm_shadow_resolve(s, va + done, chunk, &phys, &sys) ||
            !nvkvm_m2_phys_read_buf(s, phys, sys, out + done, chunk)) {
            return false;
        }
        done += chunk;
    }
    return true;
}

static bool nvkvm_m2_uvm_shadow_write(NvkvmGpuEmul *s, uint64_t va,
                                       const void *src, uint64_t size)
{
    const uint8_t *in = src;
    uint64_t done = 0;

    while (done < size) {
        uint64_t chunk = 0x1000ull - ((va + done) & 0xfffull);
        if (chunk > size - done) {
            chunk = size - done;
        }
        uint64_t phys = 0;
        bool sys = false;
        if (!nvkvm_m2_uvm_shadow_resolve(s, va + done, chunk, &phys, &sys) ||
            !nvkvm_m2_phys_write_buf(s, phys, sys, in + done, chunk)) {
            return false;
        }
        done += chunk;
    }
    return true;
}

static bool nvkvm_m2_uvm_shadow_rd32(NvkvmGpuEmul *s, uint64_t va,
                                      uint32_t *out)
{
    uint8_t b[4];
    if (!out || !nvkvm_m2_uvm_shadow_read(s, va, b, sizeof(b))) {
        return false;
    }
    *out = ldl_le_p(b);
    return true;
}

static bool nvkvm_m2_uvm_shadow_wr32(NvkvmGpuEmul *s, uint64_t va,
                                      uint32_t val)
{
    uint8_t b[4];
    stl_le_p(b, val);
    return nvkvm_m2_uvm_shadow_write(s, va, b, sizeof(b));
}

static int nvkvm_m2_uvm_ext_record(NvkvmGpuEmul *s, uint64_t va, uint64_t size)
{
    if (!size) {
        return -1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return -1;
    }
    for (int i = 0; i < s->m2_uvm_ext_n; i++) {
        uint64_t rva = s->m2_uvm_ext[i].va;
        uint64_t rend = rva + s->m2_uvm_ext[i].size;
        if (rend < rva) {
            continue;
        }
        if (va >= rva && end <= rend) {
            return i;
        }
        if (rva >= va && rend <= end) {
            s->m2_uvm_ext[i].va = va;
            s->m2_uvm_ext[i].size = size;
            return i;
        }
    }
    if (s->m2_uvm_ext_n >= (int)ARRAY_SIZE(s->m2_uvm_ext)) {
        if (nvkvm_m2_uvm_ext_trace_range(va, size)) {
            static uint32_t full_logs;
            if (full_logs++ < 32) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT record FULL VA=0x%llx "
                         "size=0x%llx slots=%d\n", s->chip->name,
                         (unsigned long long)va, (unsigned long long)size,
                         s->m2_uvm_ext_n);
            }
        }
        return -1;
    }
    int idx = s->m2_uvm_ext_n++;
    s->m2_uvm_ext[idx].va = va;
    s->m2_uvm_ext[idx].size = size;
    s->m2_uvm_ext[idx].obj_idx = -1;
    s->m2_uvm_ext[idx].mapped_cvas_mask = 0;
    s->m2_uvm_ext[idx].hClient = 0;
    s->m2_uvm_ext[idx].hMemory = 0;
    if (nvkvm_m2_uvm_ext_trace_range(va, size)) {
        static uint32_t high_record_logs;
        if (high_record_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT record[%d] high VA=0x%llx "
                     "size=0x%llx\n", s->chip->name, idx,
                     (unsigned long long)va, (unsigned long long)size);
        }
    }
    return idx;
}

static bool nvkvm_m2_uvm_ext_trace_range(uint64_t va, uint64_t size)
{
    uint64_t end = va + (size ? size - 1 : 0);
    return va >= 0x700000000000ull ||
           (end >= va && end >= 0x700000000000ull);
}

static int nvkvm_m2_uvm_ext_find(NvkvmGpuEmul *s, uint64_t va, uint64_t size)
{
    if (!size) {
        size = 1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return -1;
    }
    for (int i = 0; i < s->m2_uvm_ext_n; i++) {
        uint64_t rva = s->m2_uvm_ext[i].va;
        uint64_t rend = rva + s->m2_uvm_ext[i].size;
        if (rend < rva) {
            continue;
        }
        if (va >= rva && end <= rend) {
            return i;
        }
    }
    return -1;
}

static bool nvkvm_m2_uvm_ext_is_forwarded(NvkvmGpuEmul *s, int idx)
{
    if (idx < 0 || idx >= s->m2_uvm_ext_n) {
        return false;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    return oi >= 0 && oi < s->m2_objs_n && s->m2_objs[oi].forwarded;
}

static bool nvkvm_m2_uvm_ext_lookup(NvkvmGpuEmul *s, uint64_t va, uint64_t size,
                                    uint64_t *out_phys)
{
    if (!size) {
        size = 1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return false;
    }
    for (int i = 0; i < s->m2_uvm_ext_n; i++) {
        uint64_t rva = s->m2_uvm_ext[i].va;
        uint64_t rend = rva + s->m2_uvm_ext[i].size;
        int oi = s->m2_uvm_ext[i].obj_idx;
        if (va >= rva && end <= rend &&
            (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva) &&
            s->m2_uvm_ext[i].hClient) {
            (void)nvkvm_m2_uvm_ext_ensure_obj(s, s->m2_uvm_ext[i].hClient, i);
            oi = s->m2_uvm_ext[i].obj_idx;
        }
        if (rend < rva || oi < 0 || oi >= s->m2_objs_n ||
            !s->m2_objs[oi].cpu_qva) {
            continue;
        }
        if (va >= rva && end <= rend) {
            if (out_phys) {
                *out_phys = va;                  /* fake FB phys, GPGA-backed */
            }
            return true;
        }
    }
    return false;
}

static void nvkvm_m2_uvm_ext_apply_shadows(NvkvmGpuEmul *s, int idx)
{
    if (idx < 0 || idx >= s->m2_uvm_ext_n) {
        return;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva) {
        return;
    }
    uint64_t base = s->m2_uvm_ext[idx].va;
    uint64_t size = s->m2_uvm_ext[idx].size;
    uint8_t *qva = s->m2_objs[oi].cpu_qva;
    int applied = 0;
    for (int i = 0; i < s->m2_uvm_shadow_n; i++) {
        uint64_t sva = s->m2_uvm_shadow[i].va;
        uint64_t ssz = s->m2_uvm_shadow[i].size;
        if (!ssz || sva < base || sva + ssz < sva || sva + ssz > base + size) {
            continue;
        }
        if (nvkvm_m2_uvm_shadow_read(s, sva, qva + (sva - base), ssz)) {
            applied++;
        }
    }
    bool high_va = nvkvm_m2_uvm_ext_trace_range(base, size);
    static uint32_t high_apply_logs;
    if (applied && (s->m2_uvm_map_logs < 128 ||
                    (high_va && high_apply_logs++ < 128))) {
        qemu_log("nvkvm-gpu[%s] M8.15 UVM-EXT apply %d HtoD shadow rows "
                 "for VA=0x%llx size=0x%llx\n",
                 s->chip->name, applied, (unsigned long long)base,
                 (unsigned long long)size);
    }
}

static bool nvkvm_m2_uvm_ext_copy_from_pbmap(NvkvmGpuEmul *s, uint64_t va,
                                             uint64_t size, const char *why)
{
    if (!size) {
        size = 1;
    }
    uint64_t end = va + size;
    if (end < va) {
        return false;
    }
    for (int i = 0; i < s->m2_uvm_ext_n; i++) {
        uint64_t base = s->m2_uvm_ext[i].va;
        uint64_t rend = base + s->m2_uvm_ext[i].size;
        int oi = s->m2_uvm_ext[i].obj_idx;
        if (rend < base || oi < 0 || oi >= s->m2_objs_n ||
            !s->m2_objs[oi].cpu_qva || va < base || end > rend) {
            continue;
        }
        uint8_t *qva = s->m2_objs[oi].cpu_qva;
	        uint64_t cur = va;
	        uint64_t copied = 0;
	        uint64_t first_gpa = 0;
	        uint64_t shadow_copied = 0;
	        unsigned misses = 0;
	        while (cur < end) {
	            uint64_t chunk = 0x1000ull - (cur & 0xfffull);
	            if (chunk > end - cur) {
	                chunk = end - cur;
	            }
            uint64_t gpa = 0;
            bool from_shadow = false;
            if (!nvkvm_m2_pbmap_lookup(s, cur, chunk, &gpa)) {
                if (!nvkvm_m2_uvm_shadow_lookup(s, cur, chunk, &gpa, NULL)) {
                    misses++;
                    cur += chunk;
                    continue;
                }
                from_shadow = true;
                shadow_copied += chunk;
            }
            if (!first_gpa) {
                first_gpa = gpa;
            }
            if (from_shadow ?
                !nvkvm_m2_uvm_shadow_read(s, cur, qva + (cur - base), chunk) :
                pci_dma_read(&s->parent_obj, gpa, qva + (cur - base),
                             chunk) != MEMTX_OK) {
                misses++;
            } else {
                copied += chunk;
            }
            cur += chunk;
        }
        bool high_va = nvkvm_m2_uvm_ext_trace_range(va, size);
        static uint32_t high_seed_logs;
        if ((copied || misses) &&
            (s->m2_uvm_map_logs++ < 512 ||
             (high_va && high_seed_logs++ < 128))) {
	            qemu_log("nvkvm-gpu[%s] M8.20 UVM-EXT seed %s VA=0x%llx "
	                     "size=0x%llx firstGPA=0x%llx copied=0x%llx "
	                     "shadow=0x%llx misses=%u\n",
	                     s->chip->name, why ? why : "pbmap",
	                     (unsigned long long)va, (unsigned long long)size,
	                     (unsigned long long)first_gpa,
	                     (unsigned long long)copied,
	                     (unsigned long long)shadow_copied, misses);
	        }
        return copied == size && misses == 0;
    }
    return false;
}

static void nvkvm_m2_uvm_ext_sync_to_pbmap(NvkvmGpuEmul *s, uint64_t lo,
                                           uint64_t hi, const char *why)
{
    if (hi <= lo) {
        return;
    }
    nvkvm_m2_pbmap_reload(s);
    uint64_t copied = 0;
    unsigned rows = 0;
    for (int r = 0; r < s->m2_pbmap_n; r++) {
        uint64_t rva = s->m2_pbmap[r].va;
        uint64_t rend = rva + s->m2_pbmap[r].size;
        if (rend < rva || rend <= lo || rva >= hi) {
            continue;
        }
        uint64_t cur = rva < lo ? lo : rva;
        uint64_t stop = rend > hi ? hi : rend;
        while (cur < stop) {
            bool hit = false;
            for (int i = 0; i < s->m2_uvm_ext_n; i++) {
                uint64_t base = s->m2_uvm_ext[i].va;
                uint64_t eend = base + s->m2_uvm_ext[i].size;
                int oi = s->m2_uvm_ext[i].obj_idx;
                if (eend < base || cur < base || cur >= eend ||
                    oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva) {
                    continue;
                }
                uint64_t chunk = stop - cur;
                if (chunk > eend - cur) {
                    chunk = eend - cur;
                }
                uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (cur - base);
                uint64_t gpa = s->m2_pbmap[r].gpa + (cur - rva);
                if (pci_dma_write(&s->parent_obj, gpa, qva, chunk) == MEMTX_OK) {
                    copied += chunk;
                    rows++;
                }
                cur += chunk;
                hit = true;
                break;
            }
            if (!hit) {
                break;
            }
        }
    }
    if (copied) {
        static uint32_t sync_logs;
        if (sync_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.21 UVM-EXT sync %s VA=0x%llx..0x%llx "
                     "copied=0x%llx rows=%u\n", s->chip->name,
                     why ? why : "pbmap", (unsigned long long)lo,
                     (unsigned long long)hi, (unsigned long long)copied, rows);
        }
    }
}

static bool nvkvm_m2_uvm_ext_ensure_obj(NvkvmGpuEmul *s, uint32_t client, int idx)
{
    if (idx < 0 || idx >= s->m2_uvm_ext_n) {
        return false;
    }
    if (s->m2_uvm_ext[idx].obj_idx >= 0) {
        return true;
    }
    if (s->m2_objs_n >= (int)ARRAY_SIZE(s->m2_objs) ||
        s->m2_gpga_n >= (int)ARRAY_SIZE(s->m2_gpga)) {
        if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t cap_logs;
            if (cap_logs++ < 32) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT obj FAIL capacity idx=%d "
                         "objs=%d/%llu gpga=%d/%llu VA=0x%llx size=0x%llx\n",
                         s->chip->name, idx, s->m2_objs_n,
                         (unsigned long long)ARRAY_SIZE(s->m2_objs),
                         s->m2_gpga_n,
                         (unsigned long long)ARRAY_SIZE(s->m2_gpga),
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) {
            hDev = s->m2_devvas[i].dev;
            break;
        }
    }
    if (!hDev) {
        if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t hdev_logs;
            if (hdev_logs++ < 32) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT obj FAIL no hDev "
                         "client=0x%08x idx=%d VA=0x%llx size=0x%llx\n",
                         s->chip->name, client, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    uint64_t asize = (s->m2_uvm_ext[idx].size + 0xffffull) & ~0xffffull;
    if (!asize) {
        asize = 0x10000ull;
    }
    uint64_t obj_size = asize;
    struct nvkvm_host_map hm;
    bool forwarded = false;
    uint32_t hMem = s->m2_uvm_ext[idx].hMemory;
    uint32_t hMemClient = s->m2_uvm_ext[idx].hClient ?
                          s->m2_uvm_ext[idx].hClient : client;
    uint32_t hMemDev = hDev;

    if (hMem && s->m2_uvm_ext[idx].hClient &&
        s->m2_uvm_ext[idx].hClient != client) {
        hMemDev = 0;
        for (int i = 0; i < s->m2_devvas_n; i++) {
            if (s->m2_devvas[i].client == s->m2_uvm_ext[idx].hClient) {
                hMemDev = s->m2_devvas[i].dev;
                break;
            }
        }
    }

    if (hMem && hMemDev) {
        uint64_t fwd_size = s->m2_uvm_ext[idx].size;
        forwarded = nvkvm_m2_host_map_memory(s, hMemClient, hMemDev, hMem,
                                             fwd_size, "uvm-ext-forwarded", &hm);
        if (forwarded) {
            obj_size = fwd_size;
            static uint32_t fwd_logs;
            if (fwd_logs++ < 160 ||
                nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                             s->m2_uvm_ext[idx].size)) {
                qemu_log("nvkvm-gpu[%s] M8.90 UVM-EXT forwarded obj "
                         "idx=%d VA=0x%llx size=0x%llx hClient=0x%08x "
                         "hMem=0x%08x hDev=0x%08x qva=%p\n",
                         s->chip->name, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)obj_size, hMemClient, hMem,
                         hMemDev, hm.qva);
            }
        }
        if (!forwarded &&
            nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t local_logs;
            if (local_logs++ < 128) {
                qemu_log("nvkvm-gpu[%s] M8.92 UVM-EXT guest hMemory absent "
                         "idx=%d VA=0x%llx size=0x%llx hClient=0x%08x "
                         "hMem=0x%08x hDev=0x%08x; using local backing\n",
                         s->chip->name, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size,
                         hMemClient, hMem, hMemDev);
            }
        }
    }

    if (!forwarded) {
        hMem = 0xda800000u | (s->m2_databuf_next++ & 0xffffu);
        if (!nvkvm_m2_host_alloc_map_vidmem(s, client, hDev, hMem, asize, &hm)) {
            if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                             s->m2_uvm_ext[idx].size)) {
                static uint32_t alloc_logs;
                if (alloc_logs++ < 32) {
                    qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT obj FAIL alloc_map "
                             "client=0x%08x hDev=0x%08x hMem=0x%08x idx=%d "
                             "VA=0x%llx size=0x%llx asize=0x%llx\n",
                             s->chip->name, client, hDev, hMem, idx,
                             (unsigned long long)s->m2_uvm_ext[idx].va,
                             (unsigned long long)s->m2_uvm_ext[idx].size,
                             (unsigned long long)asize);
                }
            }
            return false;
        }
    }

    if (!forwarded && !hm.qva) {
        if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t alloc_logs;
            if (alloc_logs++ < 32) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT obj FAIL no qva "
                         "client=0x%08x hDev=0x%08x idx=%d "
                         "VA=0x%llx size=0x%llx\n",
                         s->chip->name, client, hDev, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    /*
     * Synthetic fallback objects are blank scratch backing owned by QEMU, but a
     * forwarded UVM hMemory is the guest/host driver's real allocation.  Do not
     * clear forwarded memory here; doing so erases the bytes that UVM or CE work
     * has already placed in the backing object.
     */
    if (!forwarded) {
        memset(hm.qva, 0, obj_size);
    }
    int oi = s->m2_objs_n++;
    s->m2_objs[oi].mode = 0;
    s->m2_objs[oi].cpu_qva = hm.qva;
    s->m2_objs[oi].size = obj_size;
    s->m2_objs[oi].client = client;
    s->m2_objs[oi].hMemory = hm.h_mem;
    s->m2_objs[oi].gr_va = 0;
    s->m2_objs[oi].forwarded = forwarded;

    int gi = s->m2_gpga_n++;
    s->m2_gpga[gi].gpga_base = s->m2_uvm_ext[idx].va;
    s->m2_gpga[gi].size = obj_size;
    s->m2_gpga[gi].obj_idx = oi;
    s->m2_gpga[gi].off = 0;
    s->m2_gpga[gi].readable = true;
    s->m2_gpga[gi].writable = true;

    s->m2_uvm_ext[idx].obj_idx = oi;
    nvkvm_m2_uvm_ext_apply_shadows(s, idx);
    bool high_va = nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                                s->m2_uvm_ext[idx].size);
    static uint32_t high_obj_logs;
    if (s->m2_uvm_map_logs++ < 128 ||
        (high_va && high_obj_logs++ < 128)) {
        qemu_log("nvkvm-gpu[%s] M8.15 UVM-EXT obj[%d] VA=0x%llx size=0x%llx "
                 "hMem=0x%08x%s cpu_qva=%p gpga=%d\n",
                 s->chip->name, oi, (unsigned long long)s->m2_uvm_ext[idx].va,
                 (unsigned long long)obj_size, hm.h_mem,
                 forwarded ? " forwarded" : "", hm.qva, gi);
    }
    return true;
}

static bool nvkvm_m2_uvm_ext_map_one(NvkvmGpuEmul *s, uint32_t client, int idx,
                                     bool map_gr)
{
    if (!nvkvm_m2_uvm_ext_ensure_obj(s, client, idx)) {
        if (idx >= 0 && idx < s->m2_uvm_ext_n &&
            nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t ensure_fail_logs;
            if (ensure_fail_logs++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT map FAIL ensure "
                         "client=0x%08x idx=%d VA=0x%llx size=0x%llx\n",
                         s->chip->name, client, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    if (!map_gr) {
        return true;
    }
    int cvas = s->m2_cur_cvas;
    if (cvas < 0 || cvas >= 16) {
        if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t cvas_fail_logs;
            if (cvas_fail_logs++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT map FAIL cvas=%d "
                         "client=0x%08x idx=%d VA=0x%llx size=0x%llx\n",
                         s->chip->name, cvas, client, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    if (s->m2_uvm_ext[idx].mapped_cvas_mask & (1u << cvas)) {
        return true;
    }
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) {
            hDev = s->m2_devvas[i].dev;
            break;
        }
    }
    uint32_t hVirt = nvkvm_m2_grmapper(s, client);
    int oi = s->m2_uvm_ext[idx].obj_idx;
    if (!hDev || !hVirt || oi < 0 || oi >= s->m2_objs_n) {
        if (nvkvm_m2_uvm_ext_trace_range(s->m2_uvm_ext[idx].va,
                                         s->m2_uvm_ext[idx].size)) {
            static uint32_t route_fail_logs;
            if (route_fail_logs++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.23 UVM-EXT map FAIL route "
                         "client=0x%08x hDev=0x%08x hVirt=0x%08x obj=%d "
                         "idx=%d VA=0x%llx size=0x%llx\n",
                         s->chip->name, client, hDev, hVirt, oi, idx,
                         (unsigned long long)s->m2_uvm_ext[idx].va,
                         (unsigned long long)s->m2_uvm_ext[idx].size);
            }
        }
        return false;
    }
    uint32_t mst = 0xffff;
    uint64_t outva = 0;
    uint64_t va = s->m2_uvm_ext[idx].va;
    uint64_t len = s->m2_objs[oi].size;
    int mrc = nvkvm_m2_map_dma(s, client, hDev, hVirt, s->m2_objs[oi].hMemory,
                               0, len, true, va, &mst, &outva);
    bool ok = (mrc == 0 && mst == 0 && outva == va);
    bool already = (mst == 0x51u);
    if (ok || already) {
        s->m2_uvm_ext[idx].mapped_cvas_mask |= (1u << cvas);
        s->m2_objs[oi].gr_va = va;
    }
    bool high_va = nvkvm_m2_uvm_ext_trace_range(va, len);
    static uint32_t high_map_logs;
    if (s->m2_uvm_map_logs++ < 256 ||
        (high_va && high_map_logs++ < 128)) {
        qemu_log("nvkvm-gpu[%s] M8.15 UVM-EXT map VA=0x%llx size=0x%llx "
                 "obj=%d cvas=%d hVirt=0x%08x -> rc=%d st=0x%x out=0x%llx %s\n",
                 s->chip->name, (unsigned long long)va, (unsigned long long)len,
                 oi, cvas, hVirt, mrc, mst, (unsigned long long)outva,
                 ok ? "PLACED" : already ? "ALREADY" : "FAILED");
    }
    return ok || already;
}

static void nvkvm_m2_flush_host_cpu_range(const void *addr, uint64_t size)
{
    if (!addr || !size) {
        return;
    }
#if defined(__x86_64__) || defined(__i386__)
    const uint8_t *p = addr;
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)63u;
    uintptr_t end = ((uintptr_t)p + size + 63u) & ~(uintptr_t)63u;

    for (uintptr_t cur = start; cur < end; cur += 64u) {
        __asm__ __volatile__("clflush (%0)" :: "r"((const void *)cur) : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");
#else
    msync((void *)addr, size, MS_SYNC);
#endif
}

static void nvkvm_m2_invalidate_host_cpu_range(const void *addr, uint64_t size)
{
    if (!addr || !size) {
        return;
    }
#if defined(__x86_64__) || defined(__i386__)
    const uint8_t *p = addr;
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)63u;
    uintptr_t end = ((uintptr_t)p + size + 63u) & ~(uintptr_t)63u;

    for (uintptr_t cur = start; cur < end; cur += 64u) {
        __asm__ __volatile__("clflush (%0)" :: "r"((const void *)cur) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");
#else
    msync((void *)addr, size, MS_INVALIDATE);
#endif
}

static void nvkvm_m2_uvm_ext_flush_span(NvkvmGpuEmul *s, int idx,
                                        uint64_t va, uint64_t size,
                                        const char *why)
{
    if (idx < 0 || idx >= s->m2_uvm_ext_n || !size) {
        return;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        va < base || va + size < va ||
        va + size > base + s->m2_objs[oi].size) {
        return;
    }

    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (va - base);
    nvkvm_m2_flush_host_cpu_range(qva, size);

    static uint32_t flush_logs;
    if (flush_logs++ < 256 || nvkvm_m2_uvm_ext_trace_range(va, size)) {
        qemu_log("nvkvm-gpu[%s] M8.65 UVM-EXT flush %s "
                 "VA=0x%llx size=0x%llx idx=%d obj=%d qva=%p\n",
                 s->chip->name, why ? why : "span",
                 (unsigned long long)va, (unsigned long long)size,
                 idx, oi, qva);
    }
}

static void nvkvm_m2_uvm_ext_invalidate_span(NvkvmGpuEmul *s, int idx,
                                             uint64_t va, uint64_t size,
                                             const char *why)
{
    if (idx < 0 || idx >= s->m2_uvm_ext_n || !size) {
        return;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        va < base || va + size < va ||
        va + size > base + s->m2_objs[oi].size) {
        return;
    }

    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (va - base);
    nvkvm_m2_invalidate_host_cpu_range(qva, size);

    static uint32_t inv_logs;
    if (inv_logs++ < 256 || nvkvm_m2_uvm_ext_trace_range(va, size)) {
        qemu_log("nvkvm-gpu[%s] M8.105 UVM-EXT invalidate %s "
                 "VA=0x%llx size=0x%llx idx=%d obj=%d qva=%p\n",
                 s->chip->name, why ? why : "span",
                 (unsigned long long)va, (unsigned long long)size,
                 idx, oi, qva);
    }
}

static bool nvkvm_m2_uvm_ext_map_span(NvkvmGpuEmul *s, uint32_t client,
                                      int idx, uint64_t va, uint64_t size,
                                      const char *why)
{
    if (!size) {
        size = 1;
    }
    if (!nvkvm_m2_uvm_ext_ensure_obj(s, client, idx)) {
        return false;
    }
    if (idx < 0 || idx >= s->m2_uvm_ext_n) {
        return false;
    }
    int cvas = s->m2_cur_cvas;
    if (cvas < 0 || cvas >= 16) {
        return false;
    }
    uint64_t base = s->m2_uvm_ext[idx].va;
    uint64_t req_end = va + size;
    if (req_end < va || va < base) {
        return false;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].hMemory) {
        return false;
    }
    uint64_t obj_end = base + s->m2_objs[oi].size;
    if (obj_end < base || req_end > obj_end) {
        return false;
    }
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) {
            hDev = s->m2_devvas[i].dev;
            break;
        }
    }
    uint32_t hVirt = nvkvm_m2_grmapper(s, client);
    if (!hDev || !hVirt) {
        return false;
    }

    uint64_t start = va & ~0xffffull;
    uint64_t end = (req_end + 0xffffull) & ~0xffffull;
    if (end < start || start < base || end > obj_end) {
        return false;
    }
    int mapped = 0;
    int already_count = 0;
    for (uint64_t cur = start; cur < end; cur += 0x10000ull) {
        if (nvkvm_m2_va_is_seen(s, client, cur)) {
            already_count++;
            continue;
        }
        uint32_t mst = 0xffff;
        uint64_t outva = 0;
        int mrc = nvkvm_m2_map_dma(s, client, hDev, hVirt,
                                   s->m2_objs[oi].hMemory,
                                   cur - base, 0x10000ull, true, cur,
                                   &mst, &outva);
        bool ok = (mrc == 0 && mst == 0 && outva == cur);
        bool already = (mst == 0x51u);
        if (ok || already) {
            (void)nvkvm_m2_va_seen(s, client, cur);
            mapped++;
            s->m2_objs[oi].gr_va = cur;
            continue;
        }
        nvkvm_m2_va_forget(s, client, cur);
        static uint32_t fail_logs;
        if (fail_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.25 UVM-EXT span map FAIL %s "
                     "VA=0x%llx chunk=0x%llx idx=%d obj=%d cvas=%d "
                     "hVirt=0x%08x rc=%d st=0x%x out=0x%llx\n",
                     s->chip->name, why ? why : "span",
                     (unsigned long long)va, (unsigned long long)cur,
                     idx, oi, cvas, hVirt, mrc, mst,
                     (unsigned long long)outva);
        }
        return false;
    }

    bool high_va = nvkvm_m2_uvm_ext_trace_range(va, size);
    static uint32_t span_logs;
    static uint32_t high_span_logs;
    if (span_logs++ < 256 || (high_va && high_span_logs++ < 128)) {
        qemu_log("nvkvm-gpu[%s] M8.25 UVM-EXT span map %s "
                 "VA=0x%llx size=0x%llx span=0x%llx..0x%llx "
                 "idx=%d obj=%d cvas=%d hVirt=0x%08x mapped=%d already=%d\n",
                 s->chip->name, why ? why : "span",
                 (unsigned long long)va, (unsigned long long)size,
                 (unsigned long long)start, (unsigned long long)end,
                 idx, oi, cvas, hVirt, mapped, already_count);
    }
    nvkvm_m2_uvm_ext_flush_span(s, idx, va, size, why);
    return true;
}

static void nvkvm_m2_uvm_ext_map_all(NvkvmGpuEmul *s, uint32_t client, bool map_gr)
{
    for (int i = 0; i < s->m2_uvm_ext_n; i++) {
        nvkvm_m2_uvm_ext_map_one(s, client, i, map_gr);
    }
}

static bool nvkvm_m2_uvm_ext_peek32(NvkvmGpuEmul *s, uint64_t va,
                                    uint32_t *out)
{
    if (!out) {
        return false;
    }
    int idx = nvkvm_m2_uvm_ext_find(s, va, 4);
    if (idx < 0) {
        return false;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        va < base || va + 4 > base + s->m2_objs[oi].size) {
        return false;
    }
    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (va - base);
    nvkvm_m2_invalidate_host_cpu_range(qva, 4);
    *out = ldl_le_p(qva);
    return true;
}

static uint32_t nvkvm_m2_uvm_ext_count_nonzero32(NvkvmGpuEmul *s,
                                                 uint64_t va,
                                                 uint64_t size)
{
    int idx = nvkvm_m2_uvm_ext_find(s, va, size ? size : 4);
    if (idx < 0) {
        return 0;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        va < base || va + size < va || va + size > base + s->m2_objs[oi].size) {
        return 0;
    }
    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (va - base);
    nvkvm_m2_invalidate_host_cpu_range(qva, size);
    uint32_t n = 0;
    for (uint64_t off = 0; off + 4 <= size; off += 4) {
        if (ldl_le_p(qva + off) != 0) {
            n++;
        }
    }
    return n;
}

static bool nvkvm_m2_map_gr_pushbuf_ref_va(NvkvmGpuEmul *s, uint32_t client,
                                           uint64_t pb, uint32_t word,
                                           uint64_t va, const char *why)
{
    if (!nvkvm_m2_uvm_ext_trace_range(va, 4)) {
        return false;
    }
    int idx = nvkvm_m2_uvm_ext_find(s, va, 4);
    if (idx < 0) {
        return false;
    }
    if (!nvkvm_m2_uvm_ext_map_span(s, client, idx, va, 4, why)) {
        return false;
    }

    static uint32_t ref_logs;
    if (ref_logs++ < 192) {
        uint64_t page = va & ~0xffffull;
        uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        bool have = nvkvm_m2_uvm_ext_peek32(s, va, &d0);
        nvkvm_m2_uvm_ext_peek32(s, va + 4, &d1);
        nvkvm_m2_uvm_ext_peek32(s, va + 8, &d2);
        nvkvm_m2_uvm_ext_peek32(s, va + 12, &d3);
        uint32_t nz = nvkvm_m2_uvm_ext_count_nonzero32(s, page, 0x10000ull);
        qemu_log("nvkvm-gpu[%s] M8.24 UVM-EXT refmap "
                 "pb=0x%llx word=%u VA=0x%llx idx=%d kind=%s "
                 "peek=%s %08x,%08x,%08x,%08x page_nz32=%u\n",
                 s->chip->name, (unsigned long long)pb, word,
                 (unsigned long long)va, idx, why ? why : "ref",
                 have ? "ok" : "miss", d0, d1, d2, d3, nz);
    }
    return true;
}

static int nvkvm_m2_map_gr_pushbuf_ref_pair(NvkvmGpuEmul *s, uint32_t client,
                                            uint64_t pb, uint32_t word,
                                            uint32_t prev, uint32_t cur)
{
    uint64_t va[2];
    const char *why[2];
    int n = 0;

    va[n] = ((uint64_t)prev << 32) | cur;          why[n++] = "ref-be";
    va[n] = ((uint64_t)cur << 32) | prev;          why[n++] = "ref-le";

    int mapped = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (va[i] == va[j]) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (nvkvm_m2_map_gr_pushbuf_ref_va(s, client, pb, word, va[i],
                                           why[i])) {
            mapped++;
        }
    }
    return mapped;
}

static int nvkvm_m2_map_gr_pushbuf_refs(NvkvmGpuEmul *s, uint32_t client,
                                        uint64_t pb, uint32_t pblen)
{
    int mapped = 0;
    uint32_t prev = 0;
    bool have_prev = false;
    for (uint32_t w = 0; w < pblen; w++) {
        uint32_t cur = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &cur)) {
            break;
        }
        if (have_prev) {
            mapped += nvkvm_m2_map_gr_pushbuf_ref_pair(s, client, pb, w,
                                                       prev, cur);
        }
        prev = cur;
        have_prev = true;
    }
    return mapped;
}

static void nvkvm_m2_dump_launch_uvm_result(NvkvmGpuEmul *s, uint64_t pb,
                                            uint32_t pblen, const char *why)
{
    static uint32_t dump_count;
    if (!s->trace || !pb || pblen < 7 || dump_count++ >= 128) {
        return;
    }

    uint32_t ref_logs = 0;
    uint32_t prev = 0;
    bool have_prev = false;
    for (uint32_t w = 0; w < pblen && ref_logs < 16; w++) {
        uint32_t cur = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &cur)) {
            break;
        }
        if (!have_prev) {
            prev = cur;
            have_prev = true;
            continue;
        }

        uint64_t cand[2] = {
            ((uint64_t)prev << 32) | cur,
            ((uint64_t)cur << 32) | prev,
        };
        for (int ci = 0; ci < 2 && ref_logs < 16; ci++) {
            uint64_t va = cand[ci];
            if (va < 0x100000000ull) {
                continue;
            }
            int idx = nvkvm_m2_uvm_ext_find(s, va, 4);
            if (idx < 0) {
                continue;
            }
            int oi = s->m2_uvm_ext[idx].obj_idx;
            uint64_t base = s->m2_uvm_ext[idx].va;
            if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
                va < base || va + 16u > base + s->m2_objs[oi].size) {
                continue;
            }

            uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (va - base);
            nvkvm_m2_invalidate_host_cpu_range(qva, 16);
            uint32_t v0 = ldl_le_p(qva + 0);
            uint32_t v1 = ldl_le_p(qva + 4);
            uint32_t v2 = ldl_le_p(qva + 8);
            uint32_t v3 = ldl_le_p(qva + 12);
            uint32_t poll_us = 0;
            for (int poll = 0; poll < 50 && v0 != 0x12345678u; poll++) {
                g_usleep(1000);
                poll_us += 1000;
                nvkvm_m2_invalidate_host_cpu_range(qva, 16);
                v0 = ldl_le_p(qva + 0);
                v1 = ldl_le_p(qva + 4);
                v2 = ldl_le_p(qva + 8);
                v3 = ldl_le_p(qva + 12);
            }
            qemu_log("nvkvm-gpu[%s] M8.69 LAUNCH_REF_RESULT %s "
                     "pb=0x%llx word=%u va=0x%llx idx=%d obj=%d "
                     "v=%08x,%08x,%08x,%08x poll_us=%u%s\n",
                     s->chip->name, why ? why : "launch",
                     (unsigned long long)pb, w,
                     (unsigned long long)va, idx, oi,
                     v0, v1, v2, v3, poll_us,
                     v0 == 0x12345678u ? " MAGIC" : "");
            ref_logs++;
        }
        prev = cur;
    }

    uint32_t wv[7];
    for (uint32_t w = 0; w + 6 < pblen; w++) {
        bool ok = true;
        for (uint32_t j = 0; j < 7; j++) {
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + j) * 4, &wv[j])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }

        uint64_t a = ((uint64_t)wv[1] << 32) | wv[0];
        uint64_t b = ((uint64_t)wv[3] << 32) | wv[2];
        uint64_t c = ((uint64_t)wv[5] << 32) | wv[4];
        uint32_t n = wv[6];
        if (n == 0 || n > 4096 ||
            !nvkvm_m2_uvm_ext_trace_range(a, 4) ||
            !nvkvm_m2_uvm_ext_trace_range(b, 4) ||
            !nvkvm_m2_uvm_ext_trace_range(c, 4)) {
            continue;
        }

        int ai = nvkvm_m2_uvm_ext_find(s, a, 4);
        int bi = nvkvm_m2_uvm_ext_find(s, b, 4);
        int ci = nvkvm_m2_uvm_ext_find(s, c, 4);
        if (ai < 0 || bi < 0 || ci < 0) {
            continue;
        }

        uint32_t av[8] = {0}, bv[8] = {0}, cv[8] = {0};
        int idxs[3] = { ai, bi, ci };
        uint64_t vas[3] = { a, b, c };
        uint32_t *outs[3] = { av, bv, cv };
        uint8_t *cqva = NULL;
        for (int k = 0; k < 3; k++) {
            int oi = s->m2_uvm_ext[idxs[k]].obj_idx;
            uint64_t base = s->m2_uvm_ext[idxs[k]].va;
            if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
                vas[k] < base || vas[k] + sizeof(av) > base + s->m2_objs[oi].size) {
                continue;
            }
            uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (vas[k] - base);
            nvkvm_m2_invalidate_host_cpu_range(qva, sizeof(av));
            if (k == 2) {
                cqva = qva;
            }
            for (int j = 0; j < 8; j++) {
                outs[k][j] = ldl_le_p(qva + j * 4);
            }
        }

        uint32_t poll_us = 0;
        if (cqva) {
            for (int poll = 0; poll < 200; poll++) {
                bool nonzero = false;
                nvkvm_m2_invalidate_host_cpu_range(cqva, sizeof(cv));
                for (int j = 0; j < 8; j++) {
                    cv[j] = ldl_le_p(cqva + j * 4);
                    nonzero = nonzero || cv[j] != 0;
                }
                if (nonzero) {
                    break;
                }
                g_usleep(1000);
                poll_us += 1000;
            }
        }

        qemu_log("nvkvm-gpu[%s] M8.58 LAUNCH_UVM_RESULT %s "
                 "pb=0x%llx word=%u A=0x%llx B=0x%llx C=0x%llx N=%u "
                 "A0=%08x,%08x,%08x,%08x B0=%08x,%08x,%08x,%08x "
                 "C0=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x poll_us=%u\n",
                 s->chip->name, why ? why : "launch", (unsigned long long)pb, w,
                 (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, n,
                 av[0], av[1], av[2], av[3], bv[0], bv[1], bv[2], bv[3],
                 cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
                 poll_us);
        return;
    }
}

static bool nvkvm_m2_gr_inline_header(NvkvmGpuEmul *s, uint64_t pb,
                                      uint32_t pblen, uint64_t *out_dst,
                                      uint32_t *out_bytes,
                                      uint32_t *out_inline_words)
{
    if (!pb || pblen < 16) {
        return false;
    }
    uint32_t h0 = 0, hi = 0, lo = 0, h1 = 0, bytes = 0, one = 0;
    uint32_t h2 = 0, cls = 0, h3 = 0;
    if (!nvkvm_chan_rd32(s, pb + 0, &h0) ||
        !nvkvm_chan_rd32(s, pb + 4, &hi) ||
        !nvkvm_chan_rd32(s, pb + 8, &lo) ||
        !nvkvm_chan_rd32(s, pb + 12, &h1) ||
        !nvkvm_chan_rd32(s, pb + 16, &bytes) ||
        !nvkvm_chan_rd32(s, pb + 20, &one) ||
        !nvkvm_chan_rd32(s, pb + 24, &h2) ||
        !nvkvm_chan_rd32(s, pb + 28, &cls) ||
        !nvkvm_chan_rd32(s, pb + 32, &h3)) {
        return false;
    }
    uint32_t h0_sec = (h0 >> 29) & 0x7u;
    uint32_t h0_cnt = (h0 >> 16) & 0x1fffu;
    uint32_t h0_m = (h0 & 0xfffu) << 2;
    uint32_t h1_sec = (h1 >> 29) & 0x7u;
    uint32_t h1_cnt = (h1 >> 16) & 0x1fffu;
    uint32_t h1_m = (h1 & 0xfffu) << 2;
    uint32_t h2_sec = (h2 >> 29) & 0x7u;
    uint32_t h2_cnt = (h2 >> 16) & 0x1fffu;
    uint32_t h2_m = (h2 & 0xfffu) << 2;
    uint32_t h3_sec = (h3 >> 29) & 0x7u;
    uint32_t h3_m = (h3 & 0xfffu) << 2;
    if (h0_sec != 1u || h0_cnt != 2u || h0_m != 0x188u ||
        h1_sec != 1u || h1_cnt != 2u || h1_m != 0x180u ||
        h2_sec != 1u || h2_cnt != 1u || h2_m != 0x1b0u ||
        h3_sec != 3u || h3_m != 0x1b4u || one != 1u || cls != 0x41u ||
        !bytes || bytes > 64u * 1024u) {
        return false;
    }

    uint32_t words = (bytes + 3u) / 4u;
    uint32_t inline_words = 9u + words;
    if (inline_words > pblen) {
        return false;
    }
    if (out_dst) {
        *out_dst = ((uint64_t)hi << 32) | lo;
    }
    if (out_bytes) {
        *out_bytes = bytes;
    }
    if (out_inline_words) {
        *out_inline_words = inline_words;
    }
    return true;
}

static bool nvkvm_m2_gr_inline_tail_has_host_work(NvkvmGpuEmul *s,
                                                  uint64_t pb,
                                                  uint32_t pblen,
                                                  uint32_t inline_words)
{
    for (uint32_t w = inline_words; w < pblen;) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            return false;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if (secop != 1u && secop != 3u && secop != 5u) {
            continue;
        }
        if (!cnt || cnt > 0x1000u || w + cnt > pblen) {
            return false;
        }
        if (maddr == 0x318u && cnt >= 16u) {
            return true;
        }
        w += cnt;
    }
    return false;
}

static bool nvkvm_m2_copy_gr_inline_uvm_payload(NvkvmGpuEmul *s,
                                                uint32_t client,
                                                uint64_t pb,
                                                uint32_t pblen,
                                                uint64_t dst,
                                                uint32_t bytes,
                                                uint32_t inline_words,
                                                const char *why)
{
    int idx = nvkvm_m2_uvm_ext_find(s, dst, bytes);
    if (idx < 0) {
        return false;
    }
    if (!nvkvm_m2_uvm_ext_ensure_obj(s, client, idx)) {
        return false;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva) {
        return false;
    }
    uint64_t base = s->m2_uvm_ext[idx].va;
    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + (dst - base);
    uint32_t words = (bytes + 3u) / 4u;
    uint64_t copied = 0;
    for (uint32_t j = 0; j < words; j++) {
        uint32_t d = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)(9u + j) * 4, &d)) {
            return false;
        }
        uint32_t chunk = (bytes - copied) >= 4 ? 4 : (uint32_t)(bytes - copied);
        uint8_t tmp[4];
        stl_le_p(tmp, d);
        memcpy(qva + copied, tmp, chunk);
        copied += chunk;
    }
    bool high_va = nvkvm_m2_uvm_ext_trace_range(dst, bytes);
    static uint32_t inline_copy_logs;
    static uint32_t inline_copy_high_logs;
    if (inline_copy_logs++ < 128 || (high_va && inline_copy_high_logs++ < 128)) {
        qemu_log("nvkvm-gpu[%s] M8.42 GR_INLINE_UVM_COPY %s dst=0x%llx "
                 "bytes=0x%x idx=%d pb=0x%llx copied=0x%llx "
                 "inline_words=%u\n",
                 s->chip->name, why ? why : "inline",
                 (unsigned long long)dst, bytes, idx,
                 (unsigned long long)pb, (unsigned long long)copied,
                 inline_words);
    }
    return copied == bytes;
}

static int nvkvm_m2_prepare_gr_inline_uvm_host_work(NvkvmGpuEmul *s,
                                                    uint32_t client,
                                                    uint64_t pb,
                                                    uint32_t pblen,
                                                    const char *why)
{
    int prepared = 0;
    for (uint32_t w = 0; w + 9u <= pblen; w++) {
        uint64_t sub = pb + (uint64_t)w * 4u;
        uint64_t dst = 0;
        uint32_t bytes = 0;
        uint32_t inline_words = 0;
        if (!nvkvm_m2_gr_inline_header(s, sub, pblen - w, &dst, &bytes,
                                       &inline_words)) {
            continue;
        }

        int idx = nvkvm_m2_uvm_ext_find(s, dst, bytes);
        bool copied = false;
        bool mapped = false;
        if (idx >= 0) {
            copied = nvkvm_m2_copy_gr_inline_uvm_payload(s, client, sub,
                                                         pblen - w, dst,
                                                         bytes, inline_words,
                                                         why ? why : "host-work");
            mapped = nvkvm_m2_uvm_ext_map_span(s, client, idx, dst, bytes,
                                               "inline-host-dst");
            if (copied || mapped) {
                prepared++;
            }
        }

        static uint32_t prep_logs;
        if (prep_logs++ < 160) {
            qemu_log("nvkvm-gpu[%s] M8.61 GR_INLINE_HOST_PREP %s "
                     "pb=0x%llx offw=%u dst=0x%llx bytes=0x%x "
                     "idx=%d copied=%d mapped=%d inline_words=%u\n",
                     s->chip->name, why ? why : "host-work",
                     (unsigned long long)pb, w, (unsigned long long)dst,
                     bytes, idx, copied, mapped, inline_words);
        }

        if (inline_words > 1u) {
            w += inline_words - 1u;
        }
    }
    return prepared;
}

static bool nvkvm_m2_handle_gr_inline_uvm(NvkvmGpuEmul *s, uint32_t client,
                                          uint64_t pb, uint32_t pblen)
{
    uint64_t dst = 0;
    uint32_t bytes = 0;
    uint32_t inline_words = 0;
    if (!nvkvm_m2_gr_inline_header(s, pb, pblen, &dst, &bytes,
                                   &inline_words)) {
        return false;
    }
    uint32_t trailing = pblen > inline_words ? pblen - inline_words : 0;
    if (nvkvm_m2_gr_inline_tail_has_host_work(s, pb, pblen,
                                              inline_words)) {
        nvkvm_m2_trace_gr_inline_detail(s, pb, pblen, inline_words,
                                        "host-work");
        qemu_log("nvkvm-gpu[%s] M8.34 GR_INLINE_HOST_WORK "
                 "dst=0x%llx bytes=0x%x pb=0x%llx words=%u "
                 "inline_words=%u trailing=%u: host ring required\n",
                 s->chip->name, (unsigned long long)dst, bytes,
                 (unsigned long long)pb, pblen, inline_words, trailing);
        return false;
    }
    int idx = nvkvm_m2_uvm_ext_find(s, dst, bytes);
    if (idx < 0) {
        return false;
    }
    if (s->m2exec && s->m2_cur_cvas >= 0 &&
        nvkvm_m2_uvm_ext_trace_range(dst, bytes)) {
        static uint32_t host_inline_logs;
        if (host_inline_logs++ < 160) {
            qemu_log("nvkvm-gpu[%s] M8.66 GR_INLINE_UVM_HOST_RING "
                     "dst=0x%llx bytes=0x%x pb=0x%llx words=%u "
                     "inline_words=%u trailing=%u: host ring required\n",
                     s->chip->name, (unsigned long long)dst, bytes,
                     (unsigned long long)pb, pblen, inline_words, trailing);
        }
        return false;
    }
    if ((pblen <= 512u && (bytes >= 0x100u || trailing >= 32u)) ||
        bytes == 0x160u) {
        nvkvm_m2_trace_gr_inline_detail(s, pb, pblen, inline_words,
                                        "match");
    }
    if (!nvkvm_m2_copy_gr_inline_uvm_payload(s, client, pb, pblen, dst,
                                             bytes, inline_words,
                                             "soft-complete")) {
        return false;
    }
    int sems = nvkvm_m2_release_gr_report_sems(s, pb, pblen, "inline-uvm");
    qemu_log("nvkvm-gpu[%s] M8.24 GR_INLINE_UVM dst=0x%llx bytes=0x%x "
             "idx=%d pb=0x%llx copied=0x%llx sems=%d "
             "inline_words=%u trailing=%u SOFT-COMPLETE\n",
             s->chip->name, (unsigned long long)dst, bytes, idx,
             (unsigned long long)pb, (unsigned long long)bytes, sems,
             inline_words, trailing);
    return true;
}

static bool nvkvm_m2_handle_gr_uvm_bootstrap(NvkvmGpuEmul *s, uint64_t pb,
                                             uint32_t pblen)
{
    if (!pb || pblen != 216u) {
        return false;
    }

    uint32_t w0 = 0, cls0 = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    uint32_t cpy_hdr = 0, cpy_cls = 0, sem_hdr = 0;
    if (!nvkvm_chan_rd32(s, pb + 0, &w0) ||
        !nvkvm_chan_rd32(s, pb + 4, &cls0) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)10 * 4, &rep0) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)11 * 4, &rep1) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)136 * 4, &rep2) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)137 * 4, &rep3) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)197 * 4, &cpy_hdr) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)198 * 4, &cpy_cls) ||
        !nvkvm_chan_rd32(s, pb + (uint64_t)211 * 4, &sem_hdr)) {
        return false;
    }

    if (w0 != 0x20012000u || cls0 != 0x0000c7c0u ||
        rep0 != 0x20012092u || rep1 != 0x0005403fu ||
        rep2 != 0x20012092u || rep3 != 0x00054000u ||
        cpy_hdr != 0x20018000u || cpy_cls != 0x0000c7b5u ||
        sem_hdr != 0x200426c0u) {
        return false;
    }

    int sems = nvkvm_m2_release_gr_report_sems(s, pb, pblen,
                                               "uvm-bootstrap");
    qemu_log("nvkvm-gpu[%s] M8.26 GR_UVM_BOOTSTRAP "
             "pb=0x%llx words=%u sems=%d SOFT-COMPLETE\n",
             s->chip->name, (unsigned long long)pb, pblen, sems);
    return sems > 0;
}

static bool nvkvm_m2_try_soft_complete_uvm_gr(NvkvmGpuEmul *s,
                                              uint32_t client,
                                              uint64_t pb,
                                              uint32_t pblen,
                                              uint32_t gpidx)
{
    uint64_t inline_dst = 0;
    uint32_t inline_bytes = 0;
    uint32_t inline_words = 0;
    bool has_inline = nvkvm_m2_gr_inline_header(s, pb, pblen, &inline_dst,
                                                &inline_bytes,
                                                &inline_words);
    if (has_inline &&
        nvkvm_m2_gr_inline_tail_has_host_work(s, pb, pblen, inline_words)) {
        uint32_t trailing = pblen > inline_words ? pblen - inline_words : 0;
        int prepared = nvkvm_m2_prepare_gr_inline_uvm_host_work(s, client,
                                                                pb, pblen,
                                                                "host-work");
        nvkvm_m2_trace_gr_inline_detail(s, pb, pblen, inline_words,
                                        "host-work");
        qemu_log("nvkvm-gpu[%s] M8.34 GR UVM-EXT host-work ch? "
                 "dst=0x%llx bytes=0x%x pb=0x%llx words=%u "
                 "inline_words=%u trailing=%u prepared_inline=%d: "
                 "no soft sem release\n",
                 s->chip->name, (unsigned long long)inline_dst,
                 inline_bytes, (unsigned long long)pb, pblen,
                 inline_words, trailing, prepared);
        return false;
    }
    if (nvkvm_m2_handle_gr_inline_uvm(s, client, pb, pblen)) {
        return true;
    }
    if (has_inline) {
        static uint32_t inline_host_logs;
        if (inline_host_logs++ < 128) {
            qemu_log("nvkvm-gpu[%s] M8.68 GR_INLINE_HOST_REQUIRED "
                     "pb=0x%llx words=%u dst=0x%llx bytes=0x%x "
                     "inline_words=%u: skip report-only soft-complete\n",
                     s->chip->name, (unsigned long long)pb, pblen,
                     (unsigned long long)inline_dst, inline_bytes,
                     inline_words);
        }
        return false;
    }
    if (nvkvm_m2_handle_gr_uvm_bootstrap(s, pb, pblen)) {
        return true;
    }
    if (s->m2exec && s->m2_cur_cvas >= 0 && pblen <= 32u && gpidx >= 56u) {
        static uint32_t report_host_logs;
        if (report_host_logs++ < 160) {
            qemu_log("nvkvm-gpu[%s] M8.70 GR_REPORT_HOST_REQUIRED "
                     "idx=%u pb=0x%llx words=%u cvas=%d: skip report-only "
                     "soft-complete\n",
                     s->chip->name, gpidx, (unsigned long long)pb, pblen,
                     s->m2_cur_cvas);
        }
        return false;
    }
    int sems = nvkvm_m2_release_gr_report_sems(s, pb, pblen, "uvm-init");
    if (sems > 0 && pblen <= 32) {
        qemu_log("nvkvm-gpu[%s] M8.24 GR_REPORT_ONLY "
                 "pb=0x%llx words=%u sems=%d SOFT-COMPLETE\n",
                 s->chip->name, (unsigned long long)pb, pblen, sems);
        return true;
    }
    return false;
}

static uint32_t nvkvm_m2_tsg_engine(NvkvmGpuEmul *s, uint32_t tsg)
{
    for (int i = 0; i < s->m2_tsgeng_n; i++) {
        if (s->m2_tsgeng[i].tsg == tsg) {
            return s->m2_tsgeng[i].engine;
        }
    }
    return 0;
}

static bool nvkvm_m2_map_pbmap_page(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                    const char *why)
{
    uint64_t base = va & ~0xfffull;
    if (nvkvm_m2_va_is_seen(s, client, base)) {
        return true;
    }
    int uvm_idx = nvkvm_m2_uvm_ext_find(s, base, 0x1000);
    if (uvm_idx >= 0) {
        bool ok = nvkvm_m2_uvm_ext_map_span(s, client, uvm_idx, base,
                                            0x1000, why ? why : "target");
        static uint32_t uvm_logs;
        if (uvm_logs++ < 160) {
            qemu_log("nvkvm-gpu[%s] M8.67 UVM-EXT target map %s "
                     "VA=0x%llx idx=%d cvas=%d -> %s\n",
                     s->chip->name, why ? why : "target",
                     (unsigned long long)base, uvm_idx, s->m2_cur_cvas,
                     ok ? "MAPPED" : "FAILED");
        }
        return ok;
    }
    uint64_t gpa = 0;
    if (!nvkvm_m2_pbmap_lookup(s, base, 0x1000, &gpa)) {
        uint64_t phys = 0;
        bool sys = false;
        if (!nvkvm_m2_uvm_shadow_resolve(s, base, 0x1000, &phys, &sys)) {
            phys = nvkvm_chan_translate(s, base, &sys);
            if (phys == NVKVM_GMMU_FAULT) {
                return false;
            }
            if (nvkvm_m2_va_seen(s, client, base)) {
                return true;
            }
            bool ok = sys ?
                nvkvm_m2_back_and_map_sys(s, client, base, phys, 0x1000) :
                nvkvm_m2_back_and_map(s, client, base, phys, 0x1000,
                                      true, why ? why : "chan-target");
            if (!ok) {
                nvkvm_m2_va_forget(s, client, base);
            }
            static uint32_t chan_logs;
            if (chan_logs++ < 160) {
                qemu_log("nvkvm-gpu[%s] M8.106 CHAN target map %s "
                         "VA=0x%llx phys=0x%llx sys=%d cvas=%d -> %s\n",
                         s->chip->name, why ? why : "target",
                         (unsigned long long)base, (unsigned long long)phys,
                         sys ? 1 : 0, s->m2_cur_cvas,
                         ok ? "MAPPED" : "FAILED");
            }
            return ok;
        }
        if (nvkvm_m2_va_seen(s, client, base)) {
            return true;
        }
        bool ok = sys ?
            nvkvm_m2_back_and_map_sys(s, client, base, phys, 0x1000) :
            nvkvm_m2_back_and_map(s, client, base, phys, 0x1000,
                                  true, why ? why : "shadow-target");
        if (!ok) {
            nvkvm_m2_va_forget(s, client, base);
        }
        static uint32_t shadow_logs;
        if (shadow_logs++ < 160) {
            qemu_log("nvkvm-gpu[%s] M8.71 UVM-SHADOW target map %s "
                     "VA=0x%llx phys=0x%llx sys=%d cvas=%d -> %s\n",
                     s->chip->name, why ? why : "target",
                     (unsigned long long)base, (unsigned long long)phys,
                     sys ? 1 : 0, s->m2_cur_cvas, ok ? "MAPPED" : "FAILED");
        }
        return ok;
    }
    if (nvkvm_m2_va_seen(s, client, base)) {
        return true;
    }
    bool ok = nvkvm_m2_back_and_map_sys(s, client, base, gpa, 0x1000);
    if (!ok) {
        nvkvm_m2_va_forget(s, client, base);
    }
    static uint32_t log_cnt;
    if (log_cnt++ < 160) {
        qemu_log("nvkvm-gpu[%s] M8.12 pbmap target map %s VA=0x%llx GPA=0x%llx "
                 "cvas=%d -> %s\n", s->chip->name, why ? why : "target",
                 (unsigned long long)base, (unsigned long long)gpa, s->m2_cur_cvas,
                 ok ? "MAPPED" : "FAILED");
    }
    return ok;
}

static int nvkvm_m2_map_pbmap_sem_targets(NvkvmGpuEmul *s, uint32_t client,
                                          uint64_t pb, uint32_t pblen)
{
    if (!pblen || pblen > 0x40000u) {
        return 0;
    }
    uint64_t sem_addr = 0, ce_sem_addr = 0, cr_sem_addr = 0;
    int mapped = 0;
    for (uint32_t w = 0; w < pblen; ) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7;
        uint32_t maddr = (hdr & 0xFFFu) << 2;
        uint32_t cnt   = (hdr >> 16) & 0x1FFFu;
        if (secop != 1 && secop != 3 && secop != 5) {
            continue;
        }
        if (!cnt || cnt > 0x400u || w + cnt > pblen) {
            break;
        }
        for (uint32_t j = 0; j < cnt && w < pblen; j++, w++) {
            uint32_t d = 0;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &d)) {
                return mapped;
            }
            uint32_t m = (secop == 3) ? maddr : maddr + j * 4;
            switch (m) {
            case 0x240: ce_sem_addr = (ce_sem_addr & 0xFFFFFFFFull) |
                                       ((uint64_t)(d & 0x01FFFFFFu) << 32); break;
            case 0x244: ce_sem_addr = (ce_sem_addr & ~0xFFFFFFFFull) | d; break;
            case 0x300:
                if (((d >> 3) & 0x3u) != 0 && ce_sem_addr) {
                    bool ok = nvkvm_m2_map_pbmap_page(s, client, ce_sem_addr,
                                                       "ce-sema");
                    static uint32_t ce_logs;
                    if (ce_logs++ < 160) {
                        qemu_log("nvkvm-gpu[%s] M8.75 SEM_TARGET_MAP ce-sema "
                                 "pb=0x%llx addr=0x%llx launch=0x%08x "
                                 "cvas=%d -> %s\n",
                                 s->chip->name, (unsigned long long)pb,
                                 (unsigned long long)ce_sem_addr, d,
                                 s->m2_cur_cvas, ok ? "MAPPED" : "FAILED");
                    }
                    if (ok) {
                        mapped++;
                    }
                }
                break;
            case 0x5c: sem_addr = (sem_addr & ~0xFFFFFFFFull) | (d & 0xFFFFFFFCu); break;
            case 0x60: sem_addr = (sem_addr & 0xFFFFFFFFull) | ((uint64_t)d << 32); break;
            case 0x6c:
                if ((d & 0x7u) == 0x1u && sem_addr) {
                    bool ok = nvkvm_m2_map_pbmap_page(s, client, sem_addr,
                                                       "host-sema");
                    static uint32_t host_logs;
                    if (host_logs++ < 160) {
                        qemu_log("nvkvm-gpu[%s] M8.75 SEM_TARGET_MAP host-sema "
                                 "pb=0x%llx addr=0x%llx execute=0x%08x "
                                 "cvas=%d -> %s\n",
                                 s->chip->name, (unsigned long long)pb,
                                 (unsigned long long)sem_addr, d,
                                 s->m2_cur_cvas, ok ? "MAPPED" : "FAILED");
                    }
                    if (ok) {
                        mapped++;
                    }
                }
                break;
            case 0x1b00: cr_sem_addr = (cr_sem_addr & 0xFFFFFFFFull) |
                                         ((uint64_t)(d & 0xFFu) << 32); break;
            case 0x1b04: cr_sem_addr = (cr_sem_addr & ~0xFFFFFFFFull) | d; break;
            case 0x1b0c:
                if ((d & 0x3u) == 0x0u && cr_sem_addr) {
                    bool ok = nvkvm_m2_map_pbmap_page(s, client, cr_sem_addr,
                                                       "compute-report");
                    static uint32_t compute_logs;
                    if (compute_logs++ < 240) {
                        qemu_log("nvkvm-gpu[%s] M8.75 SEM_TARGET_MAP compute-report "
                                 "pb=0x%llx addr=0x%llx execute=0x%08x "
                                 "cvas=%d -> %s\n",
                                 s->chip->name, (unsigned long long)pb,
                                 (unsigned long long)cr_sem_addr, d,
                                 s->m2_cur_cvas, ok ? "MAPPED" : "FAILED");
                    }
                    if (ok) {
                        mapped++;
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    return mapped;
}

static bool nvkvm_m2_map_launch_qmd_va(NvkvmGpuEmul *s, uint32_t client,
                                       uint64_t pb, uint32_t word,
                                       uint64_t va, uint64_t size,
                                       const char *why)
{
    if (!nvkvm_m2_uvm_ext_trace_range(va, size ? size : 4)) {
        return false;
    }
    int idx = nvkvm_m2_uvm_ext_find(s, va, size ? size : 4);
    if (idx < 0) {
        return false;
    }
    if (!nvkvm_m2_uvm_ext_map_span(s, client, idx, va, size ? size : 4,
                                   why)) {
        return false;
    }

    static uint32_t qmd_ref_logs;
    if (qmd_ref_logs++ < 192) {
        uint64_t page = va & ~0xffffull;
        uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        bool have = nvkvm_m2_uvm_ext_peek32(s, va, &d0);
        nvkvm_m2_uvm_ext_peek32(s, va + 4, &d1);
        nvkvm_m2_uvm_ext_peek32(s, va + 8, &d2);
        nvkvm_m2_uvm_ext_peek32(s, va + 12, &d3);
        uint32_t nz = nvkvm_m2_uvm_ext_count_nonzero32(s, page, 0x10000ull);
        qemu_log("nvkvm-gpu[%s] M8.59 QMD refmap "
                 "pb=0x%llx word=%u VA=0x%llx size=0x%llx idx=%d kind=%s "
                 "peek=%s %08x,%08x,%08x,%08x page_nz32=%u\n",
                 s->chip->name, (unsigned long long)pb, word,
                 (unsigned long long)va, (unsigned long long)(size ? size : 4),
                 idx, why ? why : "qmd",
                 have ? "ok" : "miss", d0, d1, d2, d3, nz);
    }
    return true;
}

static void nvkvm_m2_log_pte_info(NvkvmGpuEmul *s, uint32_t client,
                                  uint64_t va, const char *why)
{
    if (s->m2_cur_cvas < 0 || s->m2_cur_cvas >= s->m2_cvas_n ||
        s->m2_cvas[s->m2_cur_cvas].client != client || !va) {
        return;
    }

    static uint32_t pte_logs;
    if (pte_logs++ >= 160) {
        return;
    }

    enum { PTE_PARAMS_SIZE = 184, PTE_BLOCKS_OFF = 16, PTE_BLOCK_SIZE = 32 };
    uint8_t p[PTE_PARAMS_SIZE];
    memset(p, 0, sizeof(p));
    stq_le_p(p + 0, va);
    stl_le_p(p + 8, 0u);         /* subDeviceId */
    p[12] = 0;                   /* skipVASpaceInit */
    stl_le_p(p + 176, s->m2_cvas[s->m2_cur_cvas].fvas);

    uint32_t st = 0xffffu;
    int rc = nvkvm_m2_control1(s, client, s->m2_cvas[s->m2_cur_cvas].hdev,
                               0x00801801u, p, sizeof(p), &st);
    qemu_log("nvkvm-gpu[%s] M8.84 PTE_INFO %s "
             "client=0x%08x cvas=%d fvas=0x%08x va=0x%llx "
             "rc=%d st=0x%x blocks="
             "[0:pg=0x%llx ent=0x%llx kind=0x%x flags=0x%08x] "
             "[1:pg=0x%llx ent=0x%llx kind=0x%x flags=0x%08x] "
             "[2:pg=0x%llx ent=0x%llx kind=0x%x flags=0x%08x]\n",
             s->chip->name, why ? why : "pte", client, s->m2_cur_cvas,
             s->m2_cvas[s->m2_cur_cvas].fvas, (unsigned long long)va,
             rc, st,
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 0 * PTE_BLOCK_SIZE + 0),
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 0 * PTE_BLOCK_SIZE + 8),
             ldl_le_p(p + PTE_BLOCKS_OFF + 0 * PTE_BLOCK_SIZE + 20),
             ldl_le_p(p + PTE_BLOCKS_OFF + 0 * PTE_BLOCK_SIZE + 24),
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 1 * PTE_BLOCK_SIZE + 0),
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 1 * PTE_BLOCK_SIZE + 8),
             ldl_le_p(p + PTE_BLOCKS_OFF + 1 * PTE_BLOCK_SIZE + 20),
             ldl_le_p(p + PTE_BLOCKS_OFF + 1 * PTE_BLOCK_SIZE + 24),
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 2 * PTE_BLOCK_SIZE + 0),
             (unsigned long long)ldq_le_p(p + PTE_BLOCKS_OFF + 2 * PTE_BLOCK_SIZE + 8),
             ldl_le_p(p + PTE_BLOCKS_OFF + 2 * PTE_BLOCK_SIZE + 20),
             ldl_le_p(p + PTE_BLOCKS_OFF + 2 * PTE_BLOCK_SIZE + 24));
}

static bool nvkvm_m2_seed_inline_qmd(NvkvmGpuEmul *s, uint64_t qmd_va,
                                     const uint32_t q[64])
{
    int idx = nvkvm_m2_uvm_ext_find(s, qmd_va, 64u * sizeof(uint32_t));
    if (idx < 0) {
        return false;
    }
    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        qmd_va < base ||
        qmd_va + 64u * sizeof(uint32_t) > base + s->m2_objs[oi].size) {
        return false;
    }

    uint8_t *dst = (uint8_t *)s->m2_objs[oi].cpu_qva + (qmd_va - base);
    uint32_t before0 = ldl_le_p(dst);
    uint32_t before1 = ldl_le_p(dst + 4);
    for (uint32_t i = 0; i < 64u; i++) {
        stl_le_p(dst + i * 4u, q[i]);
    }
    nvkvm_m2_flush_host_cpu_range(dst, 64u * sizeof(uint32_t));

    static uint32_t seed_logs;
    if (seed_logs++ < 64) {
        qemu_log("nvkvm-gpu[%s] M8.60 INLINE_QMD_SEED "
                 "VA=0x%llx idx=%d obj=%d before=%08x,%08x "
                 "after=%08x,%08x q48=%08x q49=%08x q51=%08x\n",
                 s->chip->name, (unsigned long long)qmd_va, idx, oi,
                 before0, before1, q[0], q[1], q[48], q[49], q[51]);
    }
    return true;
}

static void nvkvm_m2_trace_compute_qmd_packet(NvkvmGpuEmul *s, uint64_t pb,
                                              uint32_t pblen, uint32_t qmd_hw,
                                              uint64_t qmd_va,
                                              const uint32_t q[64],
                                              uint64_t program_va,
                                              uint64_t prefetch_va,
                                              uint32_t prefetch_size,
                                              uint32_t cb_valid,
                                              const char *stage,
                                              uint32_t gpidx)
{
    if (!s->trace || !pb || !pblen || !q || pblen > 0x40000u) {
        return;
    }

    static uint32_t dumps;
    if (dumps++ >= 16) {
        return;
    }

    uint32_t sched_word = q[6];
    uint32_t qmd_ver = q[18] & 0xfu;
    uint32_t qmd_major = (q[18] >> 4) & 0xfu;
    qemu_log("nvkvm-gpu[%s] M8.87 QMD_DETAIL %s "
             "pb=0x%llx words=%u qmd_hdrw=%u gpidx=%u qmd_va=0x%llx "
             "program=0x%llx prefetch=0x%llx/0x%x cb_valid=0x%02x "
             "qmd_ver=%u.%u sched_word=0x%08x legacy_sem0=%u "
             "legacy_sem1=%u legacy_req_pcas=%u q18=0x%08x q19=0x%08x "
             "q20=0x%08x q51=0x%08x\n",
             s->chip->name, stage ? stage : "launch",
             (unsigned long long)pb, pblen, qmd_hw, gpidx,
             (unsigned long long)qmd_va, (unsigned long long)program_va,
             (unsigned long long)prefetch_va, prefetch_size, cb_valid,
             qmd_major, qmd_ver, sched_word,
             (sched_word >> 10) & 1u, (sched_word >> 11) & 1u,
             (sched_word >> 12) & 1u, q[18], q[19], q[20], q[51]);

    for (uint32_t i = 0; i < 64u; i += 8u) {
        qemu_log("nvkvm-gpu[%s] M8.87 QMD_WORDS %s "
                 "pb=0x%llx q%02u %08x %08x %08x %08x "
                 "%08x %08x %08x %08x\n",
                 s->chip->name, stage ? stage : "launch",
                 (unsigned long long)pb, i,
                 q[i + 0], q[i + 1], q[i + 2], q[i + 3],
                 q[i + 4], q[i + 5], q[i + 6], q[i + 7]);
    }

    uint32_t groups = 0;
    for (uint32_t w = 0; w < pblen && groups < 128u; ) {
        uint32_t hw = w;
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4u, &hdr)) {
            qemu_log("nvkvm-gpu[%s] M8.87 METHOD_RDFAIL %s "
                     "pb=0x%llx word=%u\n",
                     s->chip->name, stage ? stage : "launch",
                     (unsigned long long)pb, w);
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            continue;
        }
        if (cnt > 0x1000u || w + cnt > pblen) {
            qemu_log("nvkvm-gpu[%s] M8.87 METHOD_INVALID %s "
                     "pb=0x%llx hdrw=%u hdr=0x%08x secop=%u subch=%u "
                     "m=0x%04x cnt=%u rem=%u\n",
                     s->chip->name, stage ? stage : "launch",
                     (unsigned long long)pb, hw, hdr, secop, subch,
                     maddr, cnt, pblen - w);
            break;
        }

        qemu_log("nvkvm-gpu[%s] M8.87 METHOD %s "
                 "pb=0x%llx hdrw=%u hdr=0x%08x secop=%u subch=%u "
                 "m=0x%04x cnt=%u%s\n",
                 s->chip->name, stage ? stage : "launch",
                 (unsigned long long)pb, hw, hdr, secop, subch, maddr, cnt,
                 hw == qmd_hw ? " QMD" : "");
        uint32_t max_data = cnt < 8u ? cnt : 8u;
        if (hw == qmd_hw && cnt >= 66u) {
            max_data = 4u; /* Full QMD words are dumped above. */
        }
        for (uint32_t j = 0; j < max_data; j++) {
            uint32_t d = 0;
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + j) * 4u, &d)) {
                break;
            }
            uint32_t m = (secop == 3u) ? maddr : maddr + j * 4u;
            qemu_log("nvkvm-gpu[%s] M8.87 METHOD_DATA %s "
                     "pb=0x%llx word=%u m=0x%04x d=0x%08x%s\n",
                     s->chip->name, stage ? stage : "launch",
                     (unsigned long long)pb, w + j, m, d,
                     hw == qmd_hw ? " QMD" : "");
        }
        w += cnt;
        groups++;
    }
}

static bool nvkvm_m2_trace_compute_qmd_from_pb(NvkvmGpuEmul *s, uint64_t pb,
                                               uint32_t pblen,
                                               const char *stage,
                                               uint32_t gpidx)
{
    if (!s->trace || !pb || pblen < 67u || pblen > 0x40000u) {
        return false;
    }

    for (uint32_t w = 0; w + 66u <= pblen; ) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4u, &hdr)) {
            break;
        }
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            w++;
            continue;
        }
        if (w + 1u + cnt > pblen) {
            break;
        }
        if (subch != 1u || maddr != 0x0318u || cnt < 66u) {
            w += 1u + cnt;
            continue;
        }

        uint32_t qmd_hi_shift8 = 0, qmd_lo_shift8 = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + 1u) * 4u,
                             &qmd_hi_shift8) ||
            !nvkvm_chan_rd32(s, pb + (uint64_t)(w + 2u) * 4u,
                             &qmd_lo_shift8)) {
            return false;
        }
        uint32_t q[64];
        for (uint32_t i = 0; i < 64u; i++) {
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + 3u + i) * 4u,
                                 &q[i])) {
                return false;
            }
        }

        uint64_t qmd_va = ((((uint64_t)qmd_hi_shift8) << 32) |
                           qmd_lo_shift8) << 8;
        uint32_t qmd_major = (q[18] >> 4) & 0xfu;
        uint32_t prefetch_hi_shifted = (qmd_major >= 3u) ?
                                       (q[51] & 0x1ffu) :
                                       (q[31] & 0x1ffu);
        uint32_t prefetch_size = ((qmd_major >= 3u ?
                                   (q[51] >> 9) : (q[31] >> 9)) &
                                  0x1ffu) << 8;
        uint64_t prefetch_va = ((((uint64_t)prefetch_hi_shifted) << 32) |
                                q[8]) << 8;
        uint32_t cb_valid = q[20] & 0xffu;
        uint64_t program_va = (((uint64_t)(q[49] & 0x1ffffu)) << 32) |
                              q[48];
        nvkvm_m2_trace_compute_qmd_packet(s, pb, pblen, w, qmd_va, q,
                                          program_va, prefetch_va,
                                          prefetch_size, cb_valid,
                                          stage, gpidx);
        return true;
    }
    return false;
}

static int nvkvm_m2_map_launch_qmd_refs(NvkvmGpuEmul *s, uint32_t client,
                                        uint64_t pb, uint32_t pblen)
{
    int mapped = 0;
    for (uint32_t w = 0; w + 66u <= pblen; ) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4, &hdr)) {
            break;
        }
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            w++;
            continue;
        }
        if (w + 1u + cnt > pblen) {
            break;
        }
        if (subch != 1u || maddr != 0x318u || cnt < 66u) {
            w += 1u + cnt;
            continue;
        }

        uint32_t qmd_hi_shift8 = 0, qmd_lo_shift8 = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + 1u) * 4,
                             &qmd_hi_shift8) ||
            !nvkvm_chan_rd32(s, pb + (uint64_t)(w + 2u) * 4,
                             &qmd_lo_shift8)) {
            break;
        }

        uint32_t q[64];
        bool qok = true;
        for (uint32_t i = 0; i < 64u; i++) {
            if (!nvkvm_chan_rd32(s, pb + (uint64_t)(w + 3u + i) * 4,
                                 &q[i])) {
                qok = false;
                break;
            }
        }
        if (!qok) {
            break;
        }

        uint64_t qmd_va = ((((uint64_t)qmd_hi_shift8) << 32) |
                           qmd_lo_shift8) << 8;
        bool qmd_uvm_ext = false;
        bool qmd_pbmap = false;
        bool qmd_seeded = false;
        if (qmd_va) {
            int qmd_idx = nvkvm_m2_uvm_ext_find(s, qmd_va, 0x1000);
            if (qmd_idx >= 0) {
                qmd_uvm_ext = nvkvm_m2_uvm_ext_map_span(s, client,
                                                        qmd_idx, qmd_va,
                                                        0x1000,
                                                        "inline-qmd");
                if (qmd_uvm_ext) {
                    qmd_seeded = nvkvm_m2_seed_inline_qmd(s, qmd_va, q);
                }
            }
        }
        if (qmd_va && !qmd_uvm_ext) {
            qmd_pbmap = nvkvm_m2_map_pbmap_page(s, client, qmd_va,
                                                "inline-qmd");
        }
        if (qmd_uvm_ext || qmd_pbmap) {
            nvkvm_m2_log_pte_info(s, client, qmd_va, "inline-qmd");
        }

        uint32_t qmd_ver = q[18] & 0xfu;
        uint32_t qmd_major = (q[18] >> 4) & 0xfu;
        uint32_t prefetch_hi_shifted = (qmd_major >= 3u) ?
                                       (q[51] & 0x1ffu) :
                                       (q[31] & 0x1ffu);
        uint32_t prefetch_size = ((qmd_major >= 3u ?
                                   (q[51] >> 9) : (q[31] >> 9)) & 0x1ffu) << 8;
        uint32_t sass_version = qmd_major >= 3u ?
                                ((q[51] >> 24) & 0xffu) :
                                ((q[31] >> 24) & 0xffu);
        uint64_t prefetch_va = ((((uint64_t)prefetch_hi_shifted) << 32) |
                                q[8]) << 8;
        uint32_t cb_valid = q[20] & 0xffu;
        uint64_t program_va = (((uint64_t)(q[49] & 0x1ffffu)) << 32) |
                              q[48];
        uint64_t program_map_size = prefetch_size ? prefetch_size : 0x10000ull;
        if (program_map_size < 0x40000ull) {
            program_map_size = 0x40000ull;
        }
        nvkvm_m2_trace_compute_qmd_packet(s, pb, pblen, w, qmd_va, q,
                                          program_va, prefetch_va,
                                          prefetch_size, cb_valid,
                                          "map-launch", UINT32_MAX);
        if (nvkvm_m2_map_launch_qmd_va(s, client, pb, w + 3u + 48u,
                                       program_va, program_map_size,
                                       "qmd-program")) {
            mapped++;
            nvkvm_m2_log_pte_info(s, client, program_va, "qmd-program");
        }
        if (prefetch_va && prefetch_size) {
            if (nvkvm_m2_map_launch_qmd_va(s, client, pb, w + 3u + 8u,
                                           prefetch_va, prefetch_size,
                                           "qmd-prefetch")) {
                mapped++;
                nvkvm_m2_log_pte_info(s, client, prefetch_va, "qmd-prefetch");
            }
        }

        static uint32_t qmd_logs;
        if (qmd_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.59 QMD launch "
                     "pb=0x%llx hdrw=%u inline_qmd=0x%llx "
                     "inline_map=%s%s program=0x%llx map_size=0x%llx "
                     "prefetch=0x%llx/0x%x "
                     "cb_valid=0x%02x qmd_ver=%u.%u sass=0x%x "
                     "cta=(%u,%u,%u) threads=(%u,%u,%u)\n",
                     s->chip->name, (unsigned long long)pb, w,
                     (unsigned long long)qmd_va,
                     qmd_uvm_ext ? "uvm-ext" :
                         qmd_pbmap ? "pbmap" :
                         qmd_va ? "miss" : "none",
                     qmd_seeded ? "+seed" : "",
                     (unsigned long long)program_va,
                     (unsigned long long)program_map_size,
                     (unsigned long long)prefetch_va, prefetch_size,
                     cb_valid, qmd_major, qmd_ver, sass_version,
                     q[12], q[13] & 0xffffu, q[14] & 0xffffu,
                     (q[18] >> 16) & 0xffffu, q[19] & 0xffffu,
                     (q[19] >> 16) & 0xffffu);
        }

        for (uint32_t i = 0; i < 8u; i++) {
            if (!(cb_valid & (1u << i))) {
                continue;
            }
            uint32_t lo = q[32u + i * 2u];
            uint32_t hi = q[33u + i * 2u];
            uint64_t cb_va = (((uint64_t)(hi & 0x1ffffu)) << 32) | lo;
            uint64_t cb_size = ((uint64_t)((hi >> 19) & 0x1fffu)) << 4;
            if (!cb_size) {
                cb_size = 0x10;
            }
            char why[32];
            snprintf(why, sizeof(why), "qmd-cb%u", i);
            if (nvkvm_m2_map_launch_qmd_va(s, client, pb,
                                           w + 3u + 32u + i * 2u,
                                           cb_va, cb_size, why)) {
                mapped++;
                if (i < 2u || i == 7u) {
                    nvkvm_m2_log_pte_info(s, client, cb_va, why);
                }
                if (i == 0u) {
                    uint32_t out_lo = 0, out_hi = 0;
                    if (nvkvm_m2_uvm_ext_peek32(s, cb_va + 0x160u, &out_lo) &&
                        nvkvm_m2_uvm_ext_peek32(s, cb_va + 0x164u, &out_hi)) {
                        uint64_t out_va = ((uint64_t)out_hi << 32) | out_lo;
                        nvkvm_m2_log_pte_info(s, client, out_va, "qmd-cb0-out");
                    }
                }
            }
        }

        w += 1u + cnt;
    }
    return mapped;
}

static void nvkvm_m2_trace_qmd_host_words(NvkvmGpuEmul *s, uint64_t pb,
                                          const uint8_t *qva,
                                          uint32_t pblen)
{
    if (!s->trace || !qva || !pblen || pblen > 0x40000u) {
        return;
    }

    static uint32_t qmd_host_dumps;
    if (qmd_host_dumps++ >= 32) {
        return;
    }

    uint32_t qmd_hw = UINT32_MAX;
    uint32_t groups = 0;
    for (uint32_t w = 0; w < pblen && groups < 256u;) {
        uint32_t hw = w;
        uint32_t hdr = ldl_le_p(qva + (uint64_t)w * 4u);
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;

        w++;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            continue;
        }
        if (cnt > 0x1000u || w + cnt > pblen) {
            qemu_log("nvkvm-gpu[%s] M8.74 QMD_HOST_METHOD invalid "
                     "pb=0x%llx hdrw=%u hdr=0x%08x secop=%u subch=%u "
                     "m=0x%04x cnt=%u rem=%u\n",
                     s->chip->name, (unsigned long long)pb, hw, hdr,
                     secop, subch, maddr, cnt, pblen - w);
            break;
        }

        if (subch == 1u && maddr == 0x318u && cnt >= 66u) {
            qmd_hw = hw;
        }
        if (groups++ < 24u || hw == qmd_hw) {
            qemu_log("nvkvm-gpu[%s] M8.74 QMD_HOST_METHOD "
                     "pb=0x%llx hdrw=%u hdr=0x%08x secop=%u subch=%u "
                     "m=0x%04x cnt=%u%s\n",
                     s->chip->name, (unsigned long long)pb, hw, hdr,
                     secop, subch, maddr, cnt,
                     hw == qmd_hw ? " QMD" : "");
            uint32_t max_data = cnt < 8u ? cnt : 8u;
            if (hw == qmd_hw && cnt >= 66u) {
                max_data = 16u;
            }
            for (uint32_t j = 0; j < max_data; j++) {
                uint32_t d = ldl_le_p(qva + (uint64_t)(w + j) * 4u);
                uint32_t m = (secop == 3u) ? maddr : maddr + j * 4u;
                qemu_log("nvkvm-gpu[%s] M8.74 QMD_HOST_DATA "
                         "pb=0x%llx word=%u m=0x%04x d=0x%08x%s\n",
                         s->chip->name, (unsigned long long)pb, w + j,
                         m, d, hw == qmd_hw ? " QMD" : "");
            }
        }
        w += cnt;
    }

    uint32_t first0 = pblen > 0 ? ldl_le_p(qva) : 0;
    uint32_t first1 = pblen > 1 ? ldl_le_p(qva + 4) : 0;
    qemu_log("nvkvm-gpu[%s] M8.74 QMD_HOST_SUMMARY pb=0x%llx words=%u "
             "first=%08x,%08x qmd_hdrw=%s%u\n",
             s->chip->name, (unsigned long long)pb, pblen, first0, first1,
             qmd_hw == UINT32_MAX ? "miss+" : "", qmd_hw);

    if (qmd_hw != UINT32_MAX) {
        uint32_t start = qmd_hw > 8u ? qmd_hw - 8u : 0;
        uint32_t end = qmd_hw + 76u;
        if (end > pblen) {
            end = pblen;
        }
        for (uint32_t w = start; w < end; w += 8u) {
            uint32_t v[8] = {0};
            for (uint32_t j = 0; j < 8u && w + j < end; j++) {
                v[j] = ldl_le_p(qva + (uint64_t)(w + j) * 4u);
            }
            qemu_log("nvkvm-gpu[%s] M8.74 QMD_HOST_RAW pb=0x%llx "
                     "+0x%04x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                     s->chip->name, (unsigned long long)pb, w,
                     v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
    }
}

static bool nvkvm_m2_has_inline_qmd_launch(NvkvmGpuEmul *s, uint64_t pb,
                                           uint32_t pblen)
{
    if (!pb || pblen < 67u) {
        return false;
    }
    for (uint32_t w = 0; w + 66u <= pblen; ) {
        uint32_t hdr = 0;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4u, &hdr)) {
            break;
        }
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            w++;
            continue;
        }
        if (w + 1u + cnt > pblen) {
            break;
        }
        if (subch == 1u && maddr == 0x318u && cnt >= 66u) {
            return true;
        }
        w += 1u + cnt;
    }
    return false;
}

static bool nvkvm_m2_compute_method_interesting(uint32_t maddr)
{
    switch (maddr) {
    case 0x0180: /* LINE_LENGTH_IN */
    case 0x0188: /* OFFSET_OUT_UPPER */
    case 0x01b0: /* LAUNCH_DMA */
    case 0x0280: /* SET_COMPUTE_CLASS_VERSION */
    case 0x0284: /* CHECK_COMPUTE_CLASS_VERSION */
    case 0x0288: /* SET_QMD_VERSION */
    case 0x0290: /* CHECK_QMD_VERSION */
    case 0x0298: /* INVALIDATE_SKED_CACHES */
    case 0x029c: /* SET_QMD_VIRTUALIZATION_CONTROL */
    case 0x02b0: /* SET_CWD_SLOT_COUNT */
    case 0x02b4: /* SEND_PCAS_A */
    case 0x02b8: /* SEND_PCAS_B */
    case 0x02bc: /* SEND_SIGNALING_PCAS_B */
    case 0x02c0: /* SEND_SIGNALING_PCAS2_B */
    case 0x0310: /* SET_SPA_VERSION */
    case 0x0318: /* SET_INLINE_QMD_ADDRESS_A */
    case 0x1698: /* INVALIDATE_SHADER_CACHES_NO_WFI */
    case 0x1b00: /* SET_REPORT_SEMAPHORE_A */
        return true;
    default:
        return false;
    }
}

static bool nvkvm_m2_needs_compute_set_object(NvkvmGpuEmul *s, uint64_t pb,
                                              uint32_t pblen,
                                              char *why, size_t why_size)
{
    if (!pb || pblen < 2u || pblen > 0x40000u) {
        return false;
    }

    uint32_t w0 = 0, w1 = 0;
    if (nvkvm_chan_rd32(s, pb, &w0) &&
        nvkvm_chan_rd32(s, pb + 4u, &w1) &&
        w0 == 0x20012000u && w1 == 0x0000c7c0u) {
        return false;
    }

    for (uint32_t w = 0; w < pblen;) {
        uint32_t hdr = 0;
        uint32_t hw = w;
        if (!nvkvm_chan_rd32(s, pb + (uint64_t)w * 4u, &hdr)) {
            break;
        }
        w++;
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            continue;
        }
        if (cnt > 0x1000u || w + cnt > pblen) {
            break;
        }
        if (subch != 1u) {
            w += cnt;
            continue;
        }
        if (maddr == 0x0000u && cnt >= 1u) {
            uint32_t cls = 0;
            if (nvkvm_chan_rd32(s, pb + (uint64_t)w * 4u, &cls) &&
                cls == 0x0000c7c0u) {
                return false;
            }
        }
        if ((maddr == 0x0318u && cnt >= 66u) ||
            nvkvm_m2_compute_method_interesting(maddr)) {
            if (why && why_size) {
                snprintf(why, why_size, "m=0x%04x cnt=%u hdrw=%u",
                         maddr, cnt, hw);
            }
            return true;
        }
        w += cnt;
    }
    return false;
}

static uint32_t nvkvm_m2_compute_hdr(uint32_t maddr, uint32_t cnt)
{
    return 0x20000000u | ((cnt & 0x1fffu) << 16) |
           (1u << 13) | ((maddr >> 2) & 0xfffu);
}

static bool nvkvm_m2_inject_compute_pcas(NvkvmGpuEmul *s, uint64_t pb,
                                         uint32_t *p_pblen,
                                         uint64_t gpf_phys,
                                         uint32_t gpidx)
{
    if (!s->m2pcas || !p_pblen || !pb || !gpf_phys ||
        *p_pblen < 67u || *p_pblen > 0x3fff0u) {
        return false;
    }

    uint32_t pblen = *p_pblen;
    uint64_t bytes = (uint64_t)pblen * 4u;
    int idx = nvkvm_m2_uvm_ext_find(s, pb, bytes + 32u);
    if (idx < 0) {
        return false;
    }

    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    uint64_t off = pb - base;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        pb < base || off + bytes + 32u > s->m2_objs[oi].size) {
        return false;
    }

    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + off;
    for (uint32_t w = 0; w + 66u <= pblen; ) {
        uint32_t hdr = ldl_le_p(qva + (uint64_t)w * 4u);
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            w++;
            continue;
        }
        if (w + 1u + cnt > pblen) {
            break;
        }
        if (subch != 1u || maddr != 0x0318u || cnt < 66u) {
            w += 1u + cnt;
            continue;
        }

        uint64_t qmd_va = ((((uint64_t)ldl_le_p(qva + (uint64_t)(w + 1u) * 4u)) << 32) |
                           ldl_le_p(qva + (uint64_t)(w + 2u) * 4u)) << 8;
        uint32_t insert = w + 1u + cnt;
        if (insert < pblen) {
            uint32_t next_hdr = ldl_le_p(qva + (uint64_t)insert * 4u);
            uint32_t next_subch = (next_hdr >> 13) & 0x7u;
            uint32_t next_maddr = (next_hdr & 0xfffu) << 2;
            if (next_subch == 1u &&
                (next_maddr == 0x02b4u || next_maddr == 0x02b8u ||
                 next_maddr == 0x02bcu || next_maddr == 0x02c0u)) {
                return false;
            }
        }

        uint32_t mode = s->m2pcas & 0xfu;
        uint32_t add[8];
        uint32_t addn = 0;
        if (mode == 2u || mode == 3u || mode == 4u) {
            add[addn++] = nvkvm_m2_compute_hdr(0x02b4u, 2u);
            add[addn++] = (uint32_t)(qmd_va >> 8);
            add[addn++] = 0x00000000u; /* FROM=0, DELTA=0 for one inline QMD. */
        }
        if (mode == 2u) {
            add[addn++] = nvkvm_m2_compute_hdr(0x02bcu, 1u);
            add[addn++] = 0x00000002u; /* SCHEDULE_TRUE */
        } else {
            uint32_t action = 0x00000002u; /* PCAS2 SCHEDULE */
            if (mode == 3u) {
                action = 0x00000009u;     /* PREFETCH_SCHEDULE */
            } else if (mode == 4u) {
                action = 0x0000000bu;     /* FORCE_REQUIRE_SCHEDULING */
            }
            add[addn++] = nvkvm_m2_compute_hdr(0x02c0u, 1u);
            add[addn++] = action;
        }

        if (!addn || off + ((uint64_t)pblen + addn) * 4u > s->m2_objs[oi].size) {
            return false;
        }
        memmove(qva + (uint64_t)(insert + addn) * 4u,
                qva + (uint64_t)insert * 4u,
                (uint64_t)(pblen - insert) * 4u);
        for (uint32_t i = 0; i < addn; i++) {
            stl_le_p(qva + (uint64_t)(insert + i) * 4u, add[i]);
        }
        pblen += addn;
        nvkvm_m2_flush_host_cpu_range(qva, (uint64_t)pblen * 4u);

        uint64_t epa = gpf_phys + (uint64_t)gpidx * 8u;
        uint32_t e1 = (uint32_t)nvkvm_fb_read(s, epa + 4u, 4);
        uint32_t new_e1 = (e1 & ~0x7ffffc00u) | ((pblen & 0x1fffffu) << 10);
        nvkvm_fb_write(s, epa + 4u, new_e1, 4);
        uint8_t *hp = nvkvm_fb_host_overlay(s, epa + 4u);
        if (hp) {
            nvkvm_m2_flush_host_cpu_range(hp, 4);
        }
        *p_pblen = pblen;
        qemu_log("nvkvm-gpu[%s] M8.89 COMPUTE_PCAS_INJECT "
                 "mode=%u pb=0x%llx qmd=0x%llx insert=%u add=%u "
                 "gpidx=%u e1=0x%08x->0x%08x words=%u\n",
                 s->chip->name, mode, (unsigned long long)pb,
                 (unsigned long long)qmd_va, insert, addn, gpidx,
                 e1, new_e1, pblen);
        nvkvm_m2_trace_qmd_host_words(s, pb, qva, pblen);
        return true;
    }

    return false;
}

static bool nvkvm_m2_prefix_compute_set_object(NvkvmGpuEmul *s, uint64_t pb,
                                               uint32_t *p_pblen,
                                               uint64_t gpf_phys,
                                               uint32_t gpidx)
{
    /*
     * M8.88: The guest stream already binds subchannel 1 with SET_OBJECT during
     * the compute bootstrap. Prefixing later I2M/QMD packets resets persistent
     * compute-class state immediately before launch, which can make the report
     * semaphore retire without the shader running. M8.95 keeps that default but
     * exposes m2setobj=1 for controlled host-subchannel binding tests.
     */
    if (!s->m2setobj) {
        return false;
    }

    char why[48] = "unknown";

    if (!p_pblen || !pb || !gpf_phys || *p_pblen > 0x3fffeu ||
        !nvkvm_m2_needs_compute_set_object(s, pb, *p_pblen,
                                           why, sizeof(why))) {
        return false;
    }

    uint32_t w0 = 0, w1 = 0;
    if (nvkvm_chan_rd32(s, pb, &w0) &&
        nvkvm_chan_rd32(s, pb + 4u, &w1) &&
        w0 == 0x20012000u && w1 == 0x0000c7c0u) {
        return false;
    }

    uint32_t pblen = *p_pblen;
    uint64_t bytes = (uint64_t)pblen * 4u;
    bool had_qmd = nvkvm_m2_trace_compute_qmd_from_pb(s, pb, pblen,
                                                      "pre-prefix", gpidx);
    int idx = nvkvm_m2_uvm_ext_find(s, pb, bytes + 8u);
    if (idx < 0) {
        static uint32_t miss_logs;
        if (miss_logs++ < 64) {
            qemu_log("nvkvm-gpu[%s] M8.72 GR_QMD_SET_OBJECT_PREFIX miss "
                     "pb=0x%llx words=%u gpf=0x%llx gpidx=%u\n",
                     s->chip->name, (unsigned long long)pb, pblen,
                     (unsigned long long)gpf_phys, gpidx);
        }
        return false;
    }

    int oi = s->m2_uvm_ext[idx].obj_idx;
    uint64_t base = s->m2_uvm_ext[idx].va;
    uint64_t off = pb - base;
    if (oi < 0 || oi >= s->m2_objs_n || !s->m2_objs[oi].cpu_qva ||
        pb < base || off + bytes + 8u > s->m2_objs[oi].size) {
        return false;
    }

    uint8_t *qva = (uint8_t *)s->m2_objs[oi].cpu_qva + off;
    uint32_t old_pblen = pblen;
    memmove(qva + 8u, qva, bytes);
    stl_le_p(qva, 0x20012000u);
    stl_le_p(qva + 4u, 0x0000c7c0u);
    pblen += 2u;

    bool pcas2_added = false;
    for (uint32_t w = 0; w + 66u <= pblen; ) {
        uint32_t hdr = ldl_le_p(qva + (uint64_t)w * 4u);
        uint32_t secop = (hdr >> 29) & 0x7u;
        uint32_t cnt = (hdr >> 16) & 0x1fffu;
        uint32_t subch = (hdr >> 13) & 0x7u;
        uint32_t maddr = (hdr & 0xfffu) << 2;
        if ((secop != 1u && secop != 3u && secop != 5u) || !cnt) {
            w++;
            continue;
        }
        if (w + 1u + cnt > pblen) {
            break;
        }
        if (subch == 1u && maddr == 0x0318u && cnt >= 66u) {
            uint32_t insert = w + 1u + cnt;
            uint32_t next_hdr = insert < pblen ?
                                ldl_le_p(qva + (uint64_t)insert * 4u) : 0;
            uint32_t next_subch = (next_hdr >> 13) & 0x7u;
            uint32_t next_maddr = (next_hdr & 0xfffu) << 2;
            bool have_pcas = next_subch == 1u &&
                             (next_maddr == 0x02bcu ||
                              next_maddr == 0x02c0u);
            if (!have_pcas &&
                off + ((uint64_t)pblen + 2u) * 4u <= s->m2_objs[oi].size) {
                memmove(qva + (uint64_t)(insert + 2u) * 4u,
                        qva + (uint64_t)insert * 4u,
                        (uint64_t)(pblen - insert) * 4u);
                stl_le_p(qva + (uint64_t)insert * 4u, 0x200120b0u);
                stl_le_p(qva + (uint64_t)(insert + 1u) * 4u, 0x00000002u);
                qemu_log("nvkvm-gpu[%s] M8.86 COMPUTE_PCAS2_SCHEDULE "
                         "pb=0x%llx insert=%u words=%u->%u gpidx=%u\n",
                         s->chip->name, (unsigned long long)pb, insert,
                         pblen, pblen + 2u, gpidx);
                pblen += 2u;
                pcas2_added = true;
            }
            break;
        }
        w += 1u + cnt;
    }

    nvkvm_m2_flush_host_cpu_range(qva, (uint64_t)pblen * 4u);

    uint32_t new_pblen = pblen;
    uint64_t epa = gpf_phys + (uint64_t)gpidx * 8u;
    uint32_t e1 = (uint32_t)nvkvm_fb_read(s, epa + 4u, 4);
    uint32_t new_e1 = (e1 & ~0x7ffffc00u) | ((new_pblen & 0x1fffffu) << 10);
    nvkvm_fb_write(s, epa + 4u, new_e1, 4);
    uint8_t *hp = nvkvm_fb_host_overlay(s, epa + 4u);
    if (hp) {
        nvkvm_m2_flush_host_cpu_range(hp, 4);
    }
    *p_pblen = new_pblen;
    nvkvm_m2_trace_qmd_host_words(s, pb, qva, new_pblen);
    if (had_qmd) {
        nvkvm_m2_trace_compute_qmd_from_pb(s, pb, new_pblen,
                                          "post-prefix", gpidx);
    }

    qemu_log("nvkvm-gpu[%s] M8.72 GR_QMD_SET_OBJECT_PREFIX "
             "pb=0x%llx words=%u->%u idx=%d obj=%d gpf=0x%llx "
             "gpidx=%u e1=0x%08x->0x%08x\n",
             s->chip->name, (unsigned long long)pb, old_pblen, new_pblen,
             idx, oi, (unsigned long long)gpf_phys, gpidx, e1, new_e1);
    qemu_log("nvkvm-gpu[%s] M8.85 COMPUTE_SET_OBJECT_PREFIX why=%s "
             "pb=0x%llx words=%u->%u gpidx=%u pcas2=%u\n",
             s->chip->name, why, (unsigned long long)pb, old_pblen,
             new_pblen, gpidx, pcas2_added ? 1u : 0u);
    return true;
}

/* M6.5 (item-4 DISCOVERY+backing): place a contiguous guest-RAM SYSMEM run at its GR VA in
 * the host GR VASpace, so the host GPU can DMA into the guest's actual buffer. Reuses the
 * M6.2/M6.3b primitive chain: gpa->stub VA (memfd, 1:1) -> OS_DESCRIPTOR (host RM pins guest
 * RAM) -> per-client GR virtmem mapper -> FIXED map_dma at the guest VA. (st=0x51 = the VA is
 * already host-resident, e.g. self-promoted GR ctx — treated as success, do not re-place.) */
static bool nvkvm_m2_back_and_map_sys(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                                      uint64_t gpa, uint64_t size)
{
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) { hDev = s->m2_devvas[i].dev; break; }
    }
    if (!hDev || !size) { return false; }
    uint64_t sva = nvkvm_m2_gpa_to_stub_va(s, gpa);
    if (!sva) { return false; }
    uint32_t hMem = 0xdf000000u | (s->m2_databuf_next++ & 0xffffu);
    uint32_t ost = 0xffff;
    if (nvkvm_m2_os_descriptor(s, client, hDev, hMem, sva, size, &ost) != 0 || ost != 0) {
        return false;
    }
    uint32_t hVirt = nvkvm_m2_grmapper(s, client);
    if (!hVirt) { return false; }
    uint32_t mst = 0xffff; uint64_t outva = 0;
    int mrc = nvkvm_m2_map_dma(s, client, hDev, hVirt, hMem, 0, size, true, va, &mst, &outva);
    bool ok = (mrc == 0 && mst == 0 && outva == va);
    bool already = (mst == 0x51u);
    qemu_log("nvkvm-gpu[%s] M6.5 back_sys VA=0x%llx gpa=0x%llx size=0x%llx -> hMem=0x%08x "
             "os_st=0x%x map rc=%d st=0x%x %s\n", s->chip->name, (unsigned long long)va,
             (unsigned long long)gpa, (unsigned long long)size, hMem, ost, mrc, mst,
             ok ? "  PLACED" : already ? "  ALREADY-MAPPED" : "  <-- ERR");
    return ok || already;
}

/* M7 R2: the unified gpu_memory_object backing primitive (replaces back_and_map's split
 * FB-overlay-vs-map). Allocates ONE blank host vidmem object and double-mmaps it:
 *   CPU view  — cpu_qva, registered in the GPGA table so guest BAR1/PRAMIN reads of `gpga`
 *               resolve (nvkvm_fb_host_overlay) to this object (replaces dead fb_pages);
 *   GPU view  — FIXED map_dma at the guest VA into the host GR VAS (the host GPU sees the
 *               same bytes). 0x51 = host self-promoted its own object at this VA -> our GPU
 *               view isn't placed (gr_va=0); R3 makes the host adopt OURS instead.
 * One nvkvm/RM handle backs both views = coherent. Returns obj_idx, or -1. Idempotent caller
 * (dedup by va via m2_va_seen). */
static int nvkvm_m2_gpga_obj(NvkvmGpuEmul *s, uint32_t client, uint64_t va,
                             uint64_t gpga, uint64_t size)
{
    if (s->m2_objs_n >= 128 || s->m2_gpga_n >= 256) {
        return -1;
    }
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == client) { hDev = s->m2_devvas[i].dev; break; }
    }
    if (!hDev || !size) {
        return -1;
    }
    uint64_t asize = (size + 0xffffu) & ~0xffffull;        /* 64 KiB granular */
    uint32_t hMem = 0xda000000u | (s->m2_databuf_next++ & 0xffffu);
    struct nvkvm_host_map hm;
    if (!nvkvm_m2_host_alloc_map_vidmem(s, client, hDev, hMem, asize, &hm)) {
        return -1;
    }
    /* GPU view: FIXED-map into the host GR VAS at the guest VA. */
    uint32_t hVirt = nvkvm_m2_grmapper(s, client);
    uint32_t mst = 0xffff; uint64_t outva = 0; int mrc = -1;
    if (hVirt) {
        mrc = nvkvm_m2_map_dma(s, client, hDev, hVirt, hm.h_mem, 0, asize, true, va, &mst, &outva);
    }
    bool gpu_mapped = (mrc == 0 && mst == 0 && outva == va);
    int oi = s->m2_objs_n++;
    s->m2_objs[oi].mode = 0;                                /* physical (FB-backed general) */
    s->m2_objs[oi].cpu_qva = hm.qva;
    s->m2_objs[oi].size = asize;
    s->m2_objs[oi].client = client;
    s->m2_objs[oi].hMemory = hm.h_mem;
    s->m2_objs[oi].gr_va = gpu_mapped ? va : 0;
    s->m2_objs[oi].forwarded = false;
    int gi = s->m2_gpga_n++;
    s->m2_gpga[gi].gpga_base = gpga;
    s->m2_gpga[gi].size = asize;
    s->m2_gpga[gi].obj_idx = oi;
    s->m2_gpga[gi].off = 0;
    s->m2_gpga[gi].readable = true;
    s->m2_gpga[gi].writable = true;
    qemu_log("nvkvm-gpu[%s] M7 R2 gpga_obj: va=0x%llx gpga=0x%llx size=0x%llx hMem=0x%08x "
             "cpu_qva=%p gpu_mapped=%d(st=0x%x) obj=%d gpga_n=%d\n", s->chip->name,
             (unsigned long long)va, (unsigned long long)gpga, (unsigned long long)asize,
             hm.h_mem, hm.qva, gpu_mapped, mst, oi, s->m2_gpga_n);
    return oi;
}

/* M6.5 leaf accumulator: coalesce contiguous (VA,GPA,sys) leaf pages into runs, back each
 * SYSMEM run via the primitive above. (Vidmem leaves are host-resident already — M6.4.) */
struct nvkvm_leaf_acc { NvkvmGpuEmul *s; uint32_t client; uint64_t va0, gpa0, len;
                        int sys, runs, backed; uint64_t sysbytes, vidbytes;
                        /* M5.13 DRY-RUN probe: when dry, do NOT back anything; just log which
                         * leaf (VA range) maps the `target` guest-phys. Used to definitively
                         * identify the completion-semaphore page's owning PDB + GR-VA before we
                         * commit to backing it (blindly backing e.g. a BAR2 walk would overlay
                         * the guest's own page tables with blank objects -> wedge). */
                        bool dry; uint64_t target; const char *tag; bool found; };

static void nvkvm_m2_leaf_flush(struct nvkvm_leaf_acc *a)
{
    if (a->len == 0) { return; }
    a->runs++;
    if (a->dry) {
        if (a->target >= a->gpa0 && a->target < a->gpa0 + a->len) {
            uint64_t hit_va = a->va0 + (a->target - a->gpa0);
            a->found = true;
            qemu_log("nvkvm-gpu[GA106] M5.13 PROBE[%s] *** target gpa=0x%llx FOUND: %s "
                     "run VA=0x%llx gpa=0x%llx len=0x%llx -> sem GR-VA=0x%llx ***\n",
                     a->tag ? a->tag : "?", (unsigned long long)a->target,
                     a->sys ? "SYS" : "VID", (unsigned long long)a->va0,
                     (unsigned long long)a->gpa0, (unsigned long long)a->len,
                     (unsigned long long)hit_va);
        }
        a->len = 0;
        return;
    }
    if (a->sys) {
        a->sysbytes += a->len;
        if (!nvkvm_m2_va_seen(a->s, a->client, a->va0) &&
            nvkvm_m2_back_and_map_sys(a->s, a->client, a->va0, a->gpa0, a->len)) {
            a->backed++;
        }
    } else {
        /* M6.6 (user direction): vidmem leaf — back with a BLANK host vidmem object,
         * double-mmapped (back_and_map: m2_fbback CPU side at the FB addr + FIXED map_dma
         * GPU side at the guest VA). The buffer is OPAQUE: the guest manages its contents
         * and the host GPU fills the golden ctx on execution — both sides share ONE
         * coherent host object. Replaces the dead malloc'd fb_pages backing that the host
         * GPU can't touch (the cuCtxCreate crash = libcuda reads that dead vidmem as zero).
         * 0x51 from map_dma = the host self-promoted its own object at this VA (no overlay;
         * needs the avoid-self-promotion path). copy_content=false (blank). */
        a->vidbytes += a->len;
        if (!nvkvm_m2_va_seen(a->s, a->client, a->va0) &&
            nvkvm_m2_gpga_obj(a->s, a->client, a->va0, a->gpa0, a->len) >= 0) {
            a->backed++;          /* M7 R2: unified gpu_memory_object (GPGA + GR-VAS) */
        }
    }
    a->len = 0;
}

static void nvkvm_m2_leaf_add(struct nvkvm_leaf_acc *a, uint64_t va, uint64_t gpa,
                              int sys, uint64_t pgsz)
{
    if (a->len && a->sys == sys && va == a->va0 + a->len && gpa == a->gpa0 + a->len) {
        a->len += pgsz;                          /* extend run (VA+GPA both contiguous) */
        return;
    }
    nvkvm_m2_leaf_flush(a);
    a->va0 = va; a->gpa0 = gpa; a->sys = sys; a->len = pgsz;
}

/* Recursive GMMU-VER2 descent: PD3->PD2->PD1 (8B PDEs), PD0 (16B dual-PDE / 2 MiB leaf),
 * small (4K) / big (64K) PTs. Mirrors nvkvm_walk_pdb's decode but ENUMERATES every valid
 * leaf instead of resolving one VA. `budget` bounds total entries visited (sparse tables). */
static void nvkvm_m2_pt_enum(NvkvmGpuEmul *s, uint64_t tbl, bool tsys, int level,
                             uint64_t vabase, struct nvkvm_leaf_acc *a, int *budget)
{
    if (*budget <= 0 || tbl == 0) { return; }
    static const struct { int lo, n; } L[3] = { {47, 2}, {38, 512}, {29, 512} };
    if (level < 3) {
        for (uint32_t i = 0; i < (uint32_t)L[level].n && *budget > 0; i++) {
            uint64_t pde = nvkvm_pt_rd64(s, tbl + (uint64_t)i * 8, tsys);
            uint32_t ap = (uint32_t)((pde >> 1) & 0x3);
            uint64_t nt; bool ntsys;
            if (ap == 1) { nt = ((pde >> 8) & ((1ull << 25) - 1)) << 12; ntsys = false; }
            else if (ap == 2 || ap == 3) { nt = ((pde >> 8) & ((1ull << 46) - 1)) << 12; ntsys = true; }
            else { continue; }
            nvkvm_m2_pt_enum(s, nt, ntsys, level + 1,
                             vabase | ((uint64_t)i << L[level].lo), a, budget);
        }
        return;
    }
    for (uint32_t i = 0; i < 256 && *budget > 0; i++) {       /* PD0: 256 dual-PDEs */
        uint64_t e = tbl + (uint64_t)i * 16;
        uint64_t lo = nvkvm_pt_rd64(s, e, tsys), hi = nvkvm_pt_rd64(s, e + 8, tsys);
        uint64_t pdva = vabase | ((uint64_t)i << 21);
        (*budget)--;
        if (lo & 1) {                                          /* 2 MiB leaf PTE */
            uint32_t lap = (uint32_t)((lo >> 1) & 0x3); uint64_t pg; int sys;
            if (lap == 0) { pg = ((lo >> 8) & ((1ull << 25) - 1)) << 12; sys = 0; }
            else if (lap == 2 || lap == 3) { pg = ((lo >> 8) & ((1ull << 46) - 1)) << 12; sys = 1; }
            else { continue; }
            nvkvm_m2_leaf_add(a, pdva, pg, sys, 0x200000ull);
            continue;
        }
        uint32_t big_ap = (uint32_t)((lo >> 1) & 0x3), small_ap = (uint32_t)((hi >> 1) & 0x3);
        if (small_ap == 1 || small_ap == 2 || small_ap == 3) {
            bool stsys = (small_ap != 1);
            uint64_t st = stsys ? (((hi >> 8) & ((1ull << 46) - 1)) << 12)
                                : (((hi >> 8) & ((1ull << 25) - 1)) << 12);
            for (uint32_t j = 0; st && j < 512 && *budget > 0; j++) {
                uint64_t pte = nvkvm_pt_rd64(s, st + (uint64_t)j * 8, stsys); (*budget)--;
                if (!(pte & 1)) { continue; }
                uint32_t apt = (uint32_t)((pte >> 1) & 0x3); uint64_t pg; int sys;
                if (apt == 0) { pg = ((pte >> 8) & ((1ull << 25) - 1)) << 12; sys = 0; }
                else if (apt == 2 || apt == 3) { pg = ((pte >> 8) & ((1ull << 46) - 1)) << 12; sys = 1; }
                else { continue; }
                nvkvm_m2_leaf_add(a, pdva | ((uint64_t)j << 12), pg, sys, 0x1000ull);
            }
        }
        if (big_ap == 1 || big_ap == 2 || big_ap == 3) {
            bool btsys = (big_ap != 1);
            uint64_t bt = btsys ? (((lo >> 4) & ((1ull << 50) - 1)) << 8)
                                : (((lo >> 4) & ((1ull << 29) - 1)) << 8);
            for (uint32_t j = 0; bt && j < 32 && *budget > 0; j++) {
                uint64_t pte = nvkvm_pt_rd64(s, bt + (uint64_t)j * 8, btsys); (*budget)--;
                if (!(pte & 1)) { continue; }
                uint32_t apt = (uint32_t)((pte >> 1) & 0x3); uint64_t pg; int sys;
                if (apt == 0) { pg = ((pte >> 8) & ((1ull << 25) - 1)) << 12; sys = 0; }
                else if (apt == 2 || apt == 3) { pg = ((pte >> 8) & ((1ull << 46) - 1)) << 12; sys = 1; }
                else { continue; }
                nvkvm_m2_leaf_add(a, pdva | ((uint64_t)j << 16), pg, sys, 0x10000ull);
            }
        }
    }
}

/* M6.5 (item-4 step 4, the DISCOVERY sweep): the crash buffers are NVOS32-local sysmem GR
 * buffers with NO GSP-RPC, so QEMU only learns their GR-VA->guest-GPA mapping by WALKING the
 * GR VAS page tables (the guest RM builds them in guest-RAM-as-vidmem). Walk each snooped VAS
 * PDB, enumerate every sysmem leaf, coalesce runs, and OS_DESCRIPTOR+map_dma each into the host
 * GR VASpace (host GPU can then DMA into the guest's real sysmem working set). Idempotent
 * (re-runs only back NEW VAs). Gated by the m2exec caller. Bounded by `budget`. */
static void nvkvm_m2_enum_gr_sysmem(NvkvmGpuEmul *s, uint32_t client)
{
    int budget = 300000;                          /* total PT entries to visit (sparse) */
    for (int v = 0; v < s->chan_vas_n && budget > 0; v++) {
        uint64_t pdb = s->chan_vas[v].pdb;
        if (!pdb) { continue; }
        struct nvkvm_leaf_acc a; memset(&a, 0, sizeof(a));
        a.s = s; a.client = client;
        nvkvm_m2_pt_enum(s, pdb, false, 0, 0, &a, &budget);
        nvkvm_m2_leaf_flush(&a);
        qemu_log("nvkvm-gpu[%s] M6.5 enum_gr_sysmem: vas=0x%08x pdb=0x%llx runs=%d "
                 "sysbytes=0x%llx vidbytes=0x%llx backed=%d (budget_left=%d)\n", s->chip->name,
                 s->chan_vas[v].hvas, (unsigned long long)pdb, a.runs,
                 (unsigned long long)a.sysbytes, (unsigned long long)a.vidbytes,
                 a.backed, budget);
    }
}

/* M5.28 PER-CHANNEL VAS population: mirror the guest channel's ENTIRE address space into its
 * fresh nvkvm-owned VAS. Walk the channel's OWN guest PDB (chan_own_pdb, derived from the
 * channel's client -> forwarded VAS -> snooped PDB), enumerate every valid leaf, and FIXED
 * map_dma each into the fresh VAS at the same GPU VA (sysmem -> OS_DESCRIPTOR guest RAM WB;
 * vidmem -> blank host vidmem object via the GPGA table). m2_cur_cvas MUST be set by the caller
 * so grmapper routes the maps into THIS channel's fvas (not the guest forwarded VAS). Because
 * the VAS is one WE own (no host-RM ctx self-promote), every guest VA places without st=0x51 —
 * the Xid-32 collision class. Idempotent via the global m2_va_seen dedup. */
static void nvkvm_m2_populate_cvas(NvkvmGpuEmul *s, struct nvkvm_chan_entry *c)
{
    uint64_t pdb = nvkvm_chan_own_pdb(s);          /* uses s->chan_client (caller set it) */
    if (!pdb) {
        qemu_log("nvkvm-gpu[%s] M5.28 populate_cvas: client=0x%08x tsg=0x%08x — no own PDB "
                 "(VAS not snooped yet); reactive map only\n", s->chip->name,
                 c->client, c->tsg);
        return;
    }
    int budget = 300000;
    struct nvkvm_leaf_acc a; memset(&a, 0, sizeof(a));
    a.s = s; a.client = c->client;
    nvkvm_m2_pt_enum(s, pdb, false, 0, 0, &a, &budget);
    nvkvm_m2_leaf_flush(&a);
    qemu_log("nvkvm-gpu[%s] M5.28 populate_cvas: client=0x%08x tsg=0x%08x pdb=0x%llx -> "
             "cvas[%d] fvas=0x%08x runs=%d sysbytes=0x%llx vidbytes=0x%llx backed=%d "
             "(budget_left=%d)\n", s->chip->name, c->client, c->tsg,
             (unsigned long long)pdb, s->m2_cur_cvas,
             s->m2_cur_cvas >= 0 ? s->m2_cvas[s->m2_cur_cvas].fvas : 0,
             a.runs, (unsigned long long)a.sysbytes, (unsigned long long)a.vidbytes,
             a.backed, budget);
}

/* M5.13 DRY-RUN: locate which page-directory maps a target guest-phys (the completion
 * semaphore 0x2efbaf000), at what GR-VA, WITHOUT backing anything. Walks every candidate root
 * — the snooped GR VASes (chan_vas[]) plus the BAR1/BAR2 aperture PDBs — so we can see whether
 * the semaphore lives in a GR channel VAS (mappable into the host GR VAS at the same GR-VA) or
 * only in a kernel aperture (which would need a different bridge). Pure diagnostic; one-shot. */
static void nvkvm_m2_probe_sem_pdb(NvkvmGpuEmul *s, uint32_t client, uint64_t target)
{
    struct { const char *tag; uint64_t pdb; } roots[16 + 2];
    int nr = 0;
    for (int v = 0; v < s->chan_vas_n && nr < 16; v++) {
        if (s->chan_vas[v].pdb) {
            roots[nr].tag = "chan_vas"; roots[nr].pdb = s->chan_vas[v].pdb; nr++;
        }
    }
    if (s->bar1_pdb) { roots[nr].tag = "bar1_pdb"; roots[nr].pdb = s->bar1_pdb; nr++; }
    if (s->bar2_pdb) { roots[nr].tag = "bar2_pdb"; roots[nr].pdb = s->bar2_pdb; nr++; }
    qemu_log("nvkvm-gpu[%s] M5.13 PROBE start: target gpa=0x%llx across %d roots "
             "(chan_vas_n=%d bar1=0x%llx bar2=0x%llx)\n", s->chip->name,
             (unsigned long long)target, nr, s->chan_vas_n,
             (unsigned long long)s->bar1_pdb, (unsigned long long)s->bar2_pdb);
    for (int r = 0; r < nr; r++) {
        int budget = 300000;
        struct nvkvm_leaf_acc a; memset(&a, 0, sizeof(a));
        a.s = s; a.client = client; a.dry = true; a.target = target; a.tag = roots[r].tag;
        nvkvm_m2_pt_enum(s, roots[r].pdb, false, 0, 0, &a, &budget);
        nvkvm_m2_leaf_flush(&a);
        qemu_log("nvkvm-gpu[%s] M5.13 PROBE root[%d] %s pdb=0x%llx runs=%d sysB=0x%llx "
                 "vidB=0x%llx found=%d (budget_left=%d)\n", s->chip->name, r, roots[r].tag,
                 (unsigned long long)roots[r].pdb, a.runs, (unsigned long long)a.sysbytes,
                 (unsigned long long)a.vidbytes, a.found, budget);
    }
}

/* M5.9 EXECUTION FORWARD (per doorbell): map the GR channel's newly-submitted pushbuffers
 * into the host GR VASpace (double-mmap + copy the guest's command bytes) so the host GPU's
 * MMU resolves them, then RING the host doorbell (per-channel token, unconditional since M5.22)
 * so the HOST GPU actually runs the guest's work and writes the completion semaphore
 * for real ([[mode2-real-forward-not-fake]]). USERD (GP_PUT) + GPFIFO are already double-
 * mmapped; here we add the pushbuffers each entry points at. Idempotent via the mapped set. */
static void nvkvm_m2_exec_doorbell(NvkvmGpuEmul *s)
{
    if (!s->m2exec) { return; }
    uint32_t grc = s->m2_gr_client;
    /* M8.23: stop the old broad doorbell-time GR VAS sweep. It was useful before
     * the reactive pbmap/UVM/report-semaphore mappers existed, but UVM-heavy
     * submits make it allocate host VA for unrelated leaves and trigger
     * dmaAllocMapping_GM107 pressure. Keep a one-shot log/probe marker and let
     * the targeted mappers below handle the actual new working set. */
    if (grc && s->m2_exec_sweeps < 1) {
        s->m2_exec_sweeps++;
        qemu_log("nvkvm-gpu[%s] M8.23 skip broad doorbell GR-VAS sweep "
                 "(client 0x%08x); using reactive pushbuf/UVM/sem mapping\n",
                 s->chip->name, grc);
    }
    /* M5.13: one-shot DRY-RUN locate of the completion semaphore (0x2efbaf000, the page the
     * guest RM busy-polls during cuCtxCreate) so we learn its owning PDB + GR-VA before backing.
     * No side effects. */
    if (grc && !s->m2_sem_probe_done) {
        s->m2_sem_probe_done = true;
        nvkvm_m2_probe_sem_pdb(s, grc, 0x2efbaf000ull);
    }
    if (!s->m2_doorbell_ready) { return; }
    /* M5.12 (chid/token table): fetch each forwarded channel's HOST work-submit token once.
     * shadow_fwd creates the host channel with the SAME hObject, so 0xc36f0108 on the guest's
     * channel handle hits the host channel. The GP_PUT-driven demux rings THIS token for whichever
     * channel advanced (vs. decoding vChid from the guest token). M5.22: the ring is now
     * unconditional (per-channel token). */
    for (int i = 0; i < s->chan_n; i++) {
        struct nvkvm_chan_entry *c = &s->chans[i];
        if (c->token_valid || c->token_failed || !c->hobject || !c->gpfifo_va) {
            continue;
        }
        uint8_t tp[4]; memset(tp, 0, sizeof(tp)); uint32_t tst = 0xffff;
        int trc = nvkvm_m2_control1(s, c->client, c->hobject, 0xc36f0108u, tp, 4, &tst);
        if (trc == 0 && tst == 0) {
            c->host_token = ldl_le_p(tp); c->token_valid = true;
            qemu_log("nvkvm-gpu[%s] M5.12 chan[%d] hObj=0x%08x gpfifo=0x%llx -> HOST token=0x%08x "
                     "(rl=%u chid=%u)\n", s->chip->name, i, c->hobject,
                     (unsigned long long)c->gpfifo_va, c->host_token,
                     (c->host_token >> 16) & 0xffff, c->host_token & 0xffff);
        } else {
            c->token_failed = true;
            if (s->trace) {
                qemu_log("nvkvm-gpu[%s] M8.57 chan[%d] hObj=0x%08x "
                         "gpfifo=0x%llx GET_WORK_SUBMIT_TOKEN failed rc=%d "
                         "st=0x%x; will not retry\n",
                         s->chip->name, i, c->hobject,
                         (unsigned long long)c->gpfifo_va, trc, tst);
            }
        }
    }
    bool host_completed = false;
    bool local_completed = false;
    for (int i = 0; i < s->chan_n; i++) {
        struct nvkvm_chan_entry *c = &s->chans[i];
        if (c->client != grc || !c->gpfifo_va || !c->gpfifo_ent) { continue; }
        uint32_t engine = nvkvm_m2_tsg_engine(s, c->tsg);
        int old_cvas = s->m2_cur_cvas;
        uint32_t old_chan_client = s->chan_client;
        s->chan_client = c->client;
        s->m2_cur_cvas = -1;
        for (int ci = 0; ci < s->m2_cvas_n; ci++) {
            if (s->m2_cvas[ci].client == c->client &&
                s->m2_cvas[ci].tsg == c->tsg) {
                s->m2_cur_cvas = ci;
                break;
            }
        }
        if (engine == 1u && s->m2_cur_cvas >= 0 &&
            !s->m2_cvas[s->m2_cur_cvas].populated) {
            nvkvm_m2_populate_cvas(s, c);
            s->m2_cvas[s->m2_cur_cvas].populated = true;
        }
        if (engine == 1u) {
            void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
            if (uqva) {
                uint32_t hget = ldl_le_p((uint8_t *)uqva + 0x88);
                if (hget > c->gp_get && hget <= c->gpfifo_ent) {
                    uint32_t tok = c->token_valid ? c->host_token : s->m2_gr_token;
                    bool tok_valid = c->token_valid || s->m2_doorbell_ready;
                    if (nvkvm_m2_finish_host_gr_get(s, i, c, hget, tok,
                                                    tok_valid, "host-sync")) {
                        host_completed = true;
                    }
                }
            }
        }
        if (engine == 1u && c->gp_get < c->gpfifo_ent) {
            uint32_t guest_put_pre = c->userd_sys ?
                nvkvm_phys_rd32(s, c->userd + 0x8C, true) :
                (uint32_t)nvkvm_fb_read(s, c->userd + 0x8C, 4);
            void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
            uint32_t host_put_pre = uqva ?
                ldl_le_p((uint8_t *)uqva + 0x8C) : 0xffffffffu;
            if (guest_put_pre < c->gp_get &&
                host_put_pre != 0xffffffffu &&
                host_put_pre >= c->gp_get &&
                host_put_pre < c->gpfifo_ent) {
                static uint32_t stale_wrap_logs;
                if (stale_wrap_logs++ < 128) {
                    qemu_log("nvkvm-gpu[%s] M8.56 GR GP_PUT_STALE_BACKWARD ch[%d] "
                             "gpfifo=0x%llx gp_get=%u guest_put=%u host_put=%u; "
                             "repair guest USERD\n",
                             s->chip->name, i,
                             (unsigned long long)c->gpfifo_va,
                             c->gp_get, guest_put_pre, host_put_pre);
                }
                nvkvm_m2_write_guest_userd_gp_put(s, c, host_put_pre,
                                                  "stale-backward-repair");
                if (host_put_pre == c->gp_get) {
                    goto restore_next_chan;
                }
            }
        }
        s->chan_gpfifo_va  = c->gpfifo_va;
        s->chan_userd      = c->userd;
        s->chan_gpfifo_ent = c->gpfifo_ent;
        s->chan_userd_sys  = c->userd_sys;
        s->chan_hvaspace   = c->hvaspace;
        s->chan_client     = c->client;
        s->chan_gp_get     = c->gp_get;
        s->chan_gpfifo_phys = c->gpfifo_phys;
        s->chan_pdb        = c->pdb;
        nvkvm_chan_execute(s, true);
        c->gpfifo_phys = s->chan_gpfifo_phys;
        c->pdb = s->chan_pdb;
        uint64_t gpf_phys = c->gpfifo_phys;
        if (!gpf_phys) {
            static uint32_t m810_defer_cnt;
            if (m810_defer_cnt++ < 120) {
                qemu_log("nvkvm-gpu[%s] M8.10 GR pushbuf defer ch[%d] "
                         "gpfifo=0x%llx cvas=%d: waiting for BAR1 GPFIFO phys\n",
                         s->chip->name, i, (unsigned long long)c->gpfifo_va,
                         s->m2_cur_cvas);
            }
            goto restore_next_chan;
        }
        uint32_t gp_put = c->userd_sys ?
            nvkvm_phys_rd32(s, c->userd + 0x8C, true) :
            (uint32_t)nvkvm_fb_read(s, c->userd + 0x8C, 4);
        if ((gp_put == c->gp_get || gp_put < c->gp_get) &&
            s->chan_gp_put_valid &&
            s->chan_gp_put_seen > c->gp_get &&
            s->chan_gp_put_seen <= c->gpfifo_ent) {
            static uint32_t m898_logs;
            if (m898_logs++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.98 GP_PUT snapshot ch[%d] "
                         "gpfifo=0x%llx current=%u seen=%u gp_get=%u; "
                         "using prepare-observed value\n",
                         s->chip->name, i,
                         (unsigned long long)c->gpfifo_va, gp_put,
                         s->chan_gp_put_seen, c->gp_get);
            }
            gp_put = s->chan_gp_put_seen;
        }
        if (engine == 1u && c->host_inflight) {
            void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
            uint32_t hget = uqva ? ldl_le_p((uint8_t *)uqva + 0x88) :
                            0xffffffffu;
            if (hget > c->gp_get && hget <= c->gpfifo_ent) {
                uint32_t tok = c->token_valid ? c->host_token : s->m2_gr_token;
                bool tok_valid = c->token_valid || s->m2_doorbell_ready;
                if (nvkvm_m2_finish_host_gr_get(s, i, c, hget, tok,
                                                tok_valid, "host-inflight")) {
                    host_completed = true;
                }
            } else {
                c->host_inflight_polls++;
                static uint32_t inflight_logs;
                if (s->trace && (inflight_logs++ < 128 ||
                    (c->host_inflight_polls & 0x3ffu) == 0)) {
                    uint32_t hput = uqva ? ldl_le_p((uint8_t *)uqva + 0x8C) :
                                    0xffffffffu;
                    qemu_log("nvkvm-gpu[%s] M8.105 HOSTGR inflight wait ch[%d] "
                             "chan=0x%08x gp_get=%u gp_put=%u "
                             "inflight=%u->%u polls=%u host_get=%u host_put=%u\n",
                             s->chip->name, i, c->hobject, c->gp_get,
                             gp_put, c->host_inflight_get,
                             c->host_inflight_put, c->host_inflight_polls,
                             hget, hput);
                }
                goto restore_next_chan;
            }
        }
        uint32_t pending_count = 0;
        if (!nvkvm_m2_gpfifo_pending_count(c->gp_get, gp_put, c->gpfifo_ent,
                                           &pending_count)) {
            goto restore_next_chan;
        }
        bool gp_wrapped = gp_put < c->gp_get;
        if (gp_wrapped) {
            static uint32_t gr_wrap_logs;
            if (s->trace && gr_wrap_logs++ < 128) {
                qemu_log("nvkvm-gpu[%s] M8.56 GR GP_PUT_WRAP ch[%d] "
                         "gpfifo=0x%llx gp_get=%u gp_put=%u ent=%u pending=%u\n",
                         s->chip->name, i, (unsigned long long)c->gpfifo_va,
                         c->gp_get, gp_put, c->gpfifo_ent, pending_count);
            }
            uint64_t first_epa = gpf_phys + (uint64_t)c->gp_get * 8u;
            uint32_t first_e0 = (uint32_t)nvkvm_fb_read(s, first_epa, 4);
            uint32_t first_e1 = (uint32_t)nvkvm_fb_read(s, first_epa + 4, 4);
            if (first_e0 == 0 && first_e1 == 0) {
                void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
                if (uqva) {
                    stl_le_p((uint8_t *)uqva + 0x88, gp_put);
                }
                nvkvm_m2_write_guest_userd_gp_get(s, c, gp_put, "wrap-empty");
                qemu_log("nvkvm-gpu[%s] M8.56 GR GP_PUT_WRAP_EMPTY ch[%d] "
                         "gpfifo=0x%llx gp_get=%u gp_put=%u: no pending entry\n",
                         s->chip->name, i, (unsigned long long)c->gpfifo_va,
                         c->gp_get, gp_put);
                c->gp_get = gp_put;
                goto restore_next_chan;
            }
        }
        if (engine != 1u) {
            nvkvm_m2_uvm_ext_map_all(s, c->client, false);
        }
        int newmaps = 0, pushbufs = 0, unresolved = 0, soft_completed = 0;
        uint64_t host_work_pb = 0;
        uint32_t host_work_pblen = 0;
        for (uint32_t n = 0, idx = c->gp_get; n < pending_count;
             n++, idx = (idx + 1) % c->gpfifo_ent) {
            uint64_t epa = gpf_phys + (uint64_t)idx * 8;
            uint32_t e0 = (uint32_t)nvkvm_fb_read(s, epa, 4);
            uint32_t e1 = (uint32_t)nvkvm_fb_read(s, epa + 4, 4);
            uint64_t pb = 0;
            uint32_t pblen = 0;
            if (!nvkvm_m2_gpfifo_entry_valid(e0, e1, &pb, &pblen)) {
                continue;
            }
            pushbufs++;
            uint64_t pbbase = pb & ~0xfffull;
            uint64_t sz = ((pb - pbbase) + (uint64_t)pblen * 4 + 0xfff) & ~0xfffull;
            if (!sz) { sz = 0x1000; }
            if (engine != 1u) {
                continue;                       /* parser handles CE/copy TSGs */
            }
            int uvm_ext_idx = nvkvm_m2_uvm_ext_find(s, pbbase, sz);
            bool uvm_ext_pb = uvm_ext_idx >= 0;
            if (!uvm_ext_pb && nvkvm_m2_va_seen(s, c->client, pbbase)) {
                static uint32_t m810_seen_cnt;
                if (m810_seen_cnt++ < 120) {
                    qemu_log("nvkvm-gpu[%s] M8.10 GR pushbuf already mapped ch[%d] "
                             "idx=%u cvas=%d fvas=0x%08x VA=0x%llx\n",
                             s->chip->name, i, idx, s->m2_cur_cvas,
                             s->m2_cur_cvas >= 0 ? s->m2_cvas[s->m2_cur_cvas].fvas : 0,
                             (unsigned long long)pbbase);
                }
                newmaps += nvkvm_m2_map_pbmap_sem_targets(s, c->client, pb, pblen);
                if (nvkvm_m2_handle_gr_inline_uvm(s, c->client, pb, pblen)) {
                    soft_completed++;
                } else {
                    nvkvm_m2_prefix_compute_set_object(s, pb, &pblen,
                                                       gpf_phys, idx);
                    nvkvm_m2_inject_compute_pcas(s, pb, &pblen,
                                                 gpf_phys, idx);
                    newmaps += nvkvm_m2_prepare_gr_inline_uvm_host_work(s,
                                                                         c->client,
                                                                         pb,
                                                                         pblen,
                                                                         "already-mapped");
                    if (s->m2refscan) {
                        nvkvm_m2_map_gr_pushbuf_refs(s, c->client, pb,
                                                     pblen);
                    }
                    newmaps += nvkvm_m2_map_launch_qmd_refs(s, c->client,
                                                            pb, pblen);
                    nvkvm_m2_trace_gr_pushbuf(s, i, idx, pb, pblen,
                                              "already-mapped");
                }
                continue;
            }
            uint64_t pbgpa = 0;
            uint64_t pbphys = 0;
            bool pok = false;
            if (uvm_ext_pb) {
                if (!nvkvm_m2_uvm_ext_map_one(s, c->client, uvm_ext_idx, false)) {
                    unresolved++;
                    if (unresolved <= 8) {
                        qemu_log("nvkvm-gpu[%s] M8.20 UVM-EXT pushbuf unresolved ch[%d] "
                                 "idx=%u VA=0x%llx sz=0x%llx: object ensure failed\n",
                                 s->chip->name, i, idx, (unsigned long long)pbbase,
                                 (unsigned long long)sz);
                    }
                    continue;
                }
                bool forwarded_pb = nvkvm_m2_uvm_ext_is_forwarded(s, uvm_ext_idx);
                if (forwarded_pb) {
                    nvkvm_m2_uvm_ext_invalidate_span(s, uvm_ext_idx, pbbase, sz,
                                                     "pushbuf-forwarded");
                    pok = true;
                } else {
                    pok = nvkvm_m2_uvm_ext_copy_from_pbmap(s, pbbase, sz, "pushbuf");
                }
                if (!pok) {
                    unresolved++;
                    if (unresolved <= 8) {
                        qemu_log("nvkvm-gpu[%s] M8.20 UVM-EXT pushbuf unresolved ch[%d] "
                                 "idx=%u VA=0x%llx sz=0x%llx: no complete pbmap seed\n",
                                 s->chip->name, i, idx, (unsigned long long)pbbase,
                                 (unsigned long long)sz);
                    }
                    continue;
                }
                if (s->m2_uvm_map_logs++ < 256) {
                    qemu_log("nvkvm-gpu[%s] M8.15 UVM-EXT pushbuf ch[%d] idx=%u "
                             "VA=0x%llx sz=0x%llx cvas=%d %s\n",
                             s->chip->name, i, idx, (unsigned long long)pbbase,
                             (unsigned long long)sz, s->m2_cur_cvas,
                             forwarded_pb ? "forwarded" : "seeded");
                }
                newmaps += nvkvm_m2_map_pbmap_sem_targets(s, c->client, pb, pblen);
                if (nvkvm_m2_try_soft_complete_uvm_gr(s, c->client, pb, pblen,
                                                      idx)) {
                    static uint32_t premap_soft_logs;
                    if (premap_soft_logs++ < 128) {
                        qemu_log("nvkvm-gpu[%s] M8.24 GR UVM-EXT pre-map "
                                 "soft-complete ch[%d] idx=%u pb=0x%llx words=%u: "
                                 "skip host GR-VAS map\n",
                                 s->chip->name, i, idx,
                                 (unsigned long long)pb, pblen);
                    }
                    soft_completed++;
                    continue;
                }
                if (!nvkvm_m2_uvm_ext_map_span(s, c->client, uvm_ext_idx,
                                               pbbase, sz, "pushbuf")) {
                    unresolved++;
                    if (unresolved <= 8) {
                        qemu_log("nvkvm-gpu[%s] M8.20 UVM-EXT pushbuf unresolved ch[%d] "
                                 "idx=%u VA=0x%llx sz=0x%llx: host map failed\n",
                                 s->chip->name, i, idx, (unsigned long long)pbbase,
                                 (unsigned long long)sz);
                    }
                    continue;
                }
            } else if (nvkvm_m2_pbmap_lookup(s, pbbase, sz, &pbgpa)) {
                pok = nvkvm_m2_back_and_map_sys(s, c->client, pbbase, pbgpa, sz);
                qemu_log("nvkvm-gpu[%s] M8.11 user-mmap pushbuf map ch[%d] idx=%u "
                         "VA=0x%llx GPA=0x%llx sz=0x%llx cvas=%d -> %s\n",
                         s->chip->name, i, idx, (unsigned long long)pbbase,
                         (unsigned long long)pbgpa, (unsigned long long)sz,
                         s->m2_cur_cvas, pok ? "MAPPED" : "FAILED");
            } else {
                pbphys = nvkvm_m2_resolve_fb(s, pbbase);
                bool looks_user_mmap = (pbbase >= 0x200000000ull && pbbase < 0x300000000ull &&
                                        c->client == grc);
                if (looks_user_mmap) {
                    unresolved++;
                    nvkvm_m2_va_forget(s, c->client, pbbase);
                    if (unresolved <= 8) {
                        qemu_log("nvkvm-gpu[%s] M8.11 user-mmap pushbuf unresolved ch[%d] "
                                 "idx=%u VA=0x%llx sz=0x%llx: no m2pbmap GPA yet\n",
                                 s->chip->name, i, idx, (unsigned long long)pbbase,
                                 (unsigned long long)sz);
                    }
                    continue;
                }
                pok = nvkvm_m2_back_and_map(s, c->client, pbbase, pbphys, sz, true, "pushbuf");
            }
            if (!pok) {
                unresolved++;
                nvkvm_m2_va_forget(s, c->client, pbbase);
            }
            if (pok) {
                newmaps++;
                bool soft_done = !uvm_ext_pb &&
                    nvkvm_m2_handle_gr_inline_uvm(s, c->client, pb, pblen);
                if (soft_done) {
                    soft_completed++;
                } else {
                    nvkvm_m2_prefix_compute_set_object(s, pb, &pblen,
                                                       gpf_phys, idx);
                    nvkvm_m2_inject_compute_pcas(s, pb, &pblen,
                                                 gpf_phys, idx);
                    host_work_pb = pb;
                    host_work_pblen = pblen;
                    newmaps += nvkvm_m2_prepare_gr_inline_uvm_host_work(s,
                                                                         c->client,
                                                                         pb,
                                                                         pblen,
                                                                         uvm_ext_pb ? "uvm-ext" : "mapped");
                    if (s->m2refscan) {
                        nvkvm_m2_map_gr_pushbuf_refs(s, c->client, pb,
                                                     pblen);
                    }
                    newmaps += nvkvm_m2_map_launch_qmd_refs(s, c->client,
                                                            pb, pblen);
                    nvkvm_m2_trace_gr_pushbuf(s, i, idx, pb, pblen,
                                              uvm_ext_pb ? "uvm-ext" : "mapped");
                }
                newmaps += nvkvm_m2_map_pbmap_sem_targets(s, c->client, pb, pblen);
            }
            static uint32_t m810_map_cnt;
            if (m810_map_cnt++ < 240) {
                qemu_log("nvkvm-gpu[%s] M8.10 GR pushbuf map ch[%d] idx=%u "
                         "cvas=%d fvas=0x%08x VA=0x%llx phys=0x%llx sz=0x%llx -> %s\n",
                         s->chip->name, i, idx, s->m2_cur_cvas,
                         s->m2_cur_cvas >= 0 ? s->m2_cvas[s->m2_cur_cvas].fvas : 0,
                         (unsigned long long)pbbase,
                         (unsigned long long)(pbgpa ? pbgpa : pbphys),
                         (unsigned long long)sz, pok ? "MAPPED" : "FAILED");
            }
        }
        if (!pushbufs) {
            qemu_log("nvkvm-gpu[%s] M8.10 GR pushbuf defer ch[%d] gp_get=%u gp_put=%u "
                     "gpfifo_phys=0x%llx: no nonzero entries\n", s->chip->name, i,
                     c->gp_get, gp_put, (unsigned long long)gpf_phys);
            goto restore_next_chan;
        }
        if (unresolved) {
            s->m2_uvm_shadow_retry_pending = true;
            qemu_log("nvkvm-gpu[%s] M8.11 GR pushbuf defer ch[%d] gp_get=%u gp_put=%u "
                     "unresolved=%d cvas=%d: waiting for user-mmap GPA bridge\n",
                     s->chip->name, i, c->gp_get, gp_put, unresolved, s->m2_cur_cvas);
            goto restore_next_chan;
        }
        if (engine == 1u && soft_completed == pushbufs) {
            void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
            uint32_t payload = ++c->payload;
            uint64_t notify_redir = 0;
            if (uqva) {
                uint32_t hput = ldl_le_p((uint8_t *)uqva + 0x8C);
                if (hput < gp_put) {
                    stl_le_p((uint8_t *)uqva + 0x8C, gp_put);
                }
                stl_le_p((uint8_t *)uqva + 0x88, gp_put);
            }
            int notify_writes = nvkvm_m2_write_channel_notify_block(s, payload,
                                                                    "gr-soft",
                                                                    &notify_redir);
            uint32_t notify_token = c->token_valid ? c->host_token : s->m2_gr_token;
            bool notify_token_valid = c->token_valid || s->m2_doorbell_ready;
            if (notify_token_valid) {
                nvkvm_m2_write_work_submit_notifier(s, c, notify_token,
                                                    "gr-soft");
            }
            nvkvm_m2_release_gr_implicit_progress(s, gp_put);
            nvkvm_m2_repair_guest_userd_gp_put(s, c, gp_put,
                                               "soft-complete");
            nvkvm_m2_write_guest_userd_gp_get(s, c, gp_put, "soft-complete");
            qemu_log("nvkvm-gpu[%s] M8.24 GR soft-complete ch[%d] gp_get=%u->%u "
                     "pushbufs=%d cvas=%d payload=%u notify_writes=%d "
                     "notify_redir=0x%llx: no host ring\n",
                     s->chip->name, i, c->gp_get, gp_put, pushbufs,
                     s->m2_cur_cvas, payload, notify_writes,
                     (unsigned long long)notify_redir);
            c->gp_get = gp_put;
            c->host_inflight = false;
            c->host_inflight_get = 0;
            c->host_inflight_put = 0;
            c->host_inflight_polls = 0;
            host_completed = true;
            nvkvm_m2_queue_host_completion(s,
                                            c->token_valid ? c->host_token :
                                            s->m2_gr_token,
                                            c->token_valid ||
                                            s->m2_doorbell_ready,
                                            "gr-soft");
            goto restore_next_chan;
        }
        qemu_log("nvkvm-gpu[%s] M5.9 exec_doorbell GR gp_get=%u->%u pushbufs=%d "
                 "newpushbufs=%d cvas=%d fvas=0x%08x engine=0x%x\n", s->chip->name, c->gp_get,
                 gp_put, pushbufs, newmaps,
                 s->m2_cur_cvas,
                 s->m2_cur_cvas >= 0 ? s->m2_cvas[s->m2_cur_cvas].fvas : 0, engine);
        /* M5.22: ring THIS channel's own host token (per-channel, unconditional —
         * m2ring removed).  Prefer the per-channel token; fall back to the GR token
         * for the GR channel whose USERD is double-mmapped (M5.4). */
        if (s->m2_usermode_qva && engine == 1u && s->m2_cur_cvas >= 0) {
            uint32_t tok = c->token_valid ? c->host_token : s->m2_gr_token;
            bool tok_valid = c->token_valid || s->m2_doorbell_ready;
            nvkvm_m2_ring_host_channel(s, i, c, tok, "M5.9", gp_put, true);
            c->host_inflight = true;
            c->host_inflight_get = c->gp_get;
            c->host_inflight_put = gp_put;
            c->host_inflight_polls = 0;
            void *uqva = nvkvm_m2_host_userd_qva(s, c->client, c->hobject);
            if (uqva) {
                uint32_t hget = ldl_le_p((uint8_t *)uqva + 0x88);
                if (hget > c->gp_get && hget <= c->gpfifo_ent) {
                    if (host_work_pb && host_work_pblen) {
                        bool qmd_launch =
                            nvkvm_m2_has_inline_qmd_launch(s, host_work_pb,
                                                           host_work_pblen);
                        nvkvm_m2_dump_launch_uvm_result(s, host_work_pb,
                                                        host_work_pblen,
                                                        "host-ring");
                        nvkvm_m2_log_host_channel_state(s, c, "host-ring");
                        if (qmd_launch) {
                            nvkvm_m2_log_post_launch_rm_state(s, c, "host-qmd");
                        }
                    }
                    if (nvkvm_m2_finish_host_gr_get(s, i, c, hget, tok,
                                                    tok_valid, "host-ring")) {
                        host_completed = true;
                    }
                }
            }
        } else if (engine != 1u) {
            static uint32_t skip_cnt;
            if (skip_cnt++ < 64) {
                qemu_log("nvkvm-gpu[%s] M8.12 skip host ring ch[%d] TSG=0x%08x "
                             "engine=0x%x; local CE/parser fallback will consume\n",
                             s->chip->name, i, c->tsg, engine);
            }
        }
        if (engine == 1u) {
            goto restore_next_chan;
        }
        uint32_t before = c->gp_get;
        if (c->gpfifo_ent > NVKVM_M2_GPFIFO_LARGE_RING_ENTRIES) {
            nvkvm_m2_repair_guest_userd_gp_put(s, c, c->gp_get,
                                               "service-stale-large-ring");
        }
        s->chan_gpfifo_va  = c->gpfifo_va;
        s->chan_userd      = c->userd;
        s->chan_gpfifo_ent = c->gpfifo_ent;
        s->chan_userd_sys  = c->userd_sys;
        s->chan_hvaspace   = c->hvaspace;
        s->chan_client     = c->client;
        s->chan_gp_get     = c->gp_get;
        s->chan_gpfifo_phys = c->gpfifo_phys;
        s->chan_pdb        = c->pdb;
        nvkvm_chan_execute(s, false);
        c->gpfifo_phys = s->chan_gpfifo_phys;
        c->pdb = s->chan_pdb;
        if (!s->chan_unresolved) {
            c->gp_get = s->chan_gp_get;
                if (c->gp_get != before) {
                    nvkvm_m2_repair_guest_userd_gp_put(s, c, c->gp_get,
                                                       "local-service");
                    nvkvm_m2_write_guest_userd_gp_get(s, c, c->gp_get,
                                                      "local-service");
                    s->m2_local_completion_token = c->token_valid ?
                                                   c->host_token : 0;
                    s->m2_local_completion_token_valid = c->token_valid;
                    local_completed = true;
                }
            }
restore_next_chan:
        s->m2_cur_cvas = old_cvas;
        s->chan_client = old_chan_client;
    }
    if (host_completed) {
        nvkvm_m2_try_deliver_host_completion(s, "host-service");
    }
    if (local_completed) {
        s->m2_local_completion_pending = true;
        nvkvm_m2_try_deliver_local_completion(s, "local-service");
    }
}

/* M5.5 one-shot validation of the RM_MAP_MEMORY_DMA primitive via the CORRECT mapper
 * (NV01_MEMORY_VIRTUAL). P1: fully private client->device->vaspace->virtmem->memory,
 * map NON-FIXED then FIXED. P2: against the forwarded GR VASpace (alloc a virtmem mapper
 * referencing 0x5c000007, map a fresh host vidmem FIXED at a guest VA). Pure validation —
 * private handles, never the live forward chain — cannot regress GR build. */
static void nvkvm_m2_mapdma_selftest(NvkvmGpuEmul *s, uint32_t hClient)
{
    uint64_t sz = 0x10000;                        /* 64 KiB (PMA granularity) */

    /* ---- Part 1: fully self-contained tuple ---- */
    const uint32_t C = 0xc1ee0011u, DEV = 0xde110001u, VAS = 0xde110002u,
                   VIRT = 0xde110004u, MEM = 0xde110003u;
    uint32_t st = 0xffff;
    uint32_t c0 = C;
    nvkvm_m2_alloc1(s, C, 0, 0, 0x0u, &c0, sizeof(c0), &st);
    uint8_t devp[56]; memset(devp, 0, sizeof(devp));
    nvkvm_m2_alloc1(s, C, C, DEV, 0x0080u, devp, sizeof(devp), &st);
    uint32_t dst = st;
    uint8_t vasp[56]; memset(vasp, 0, sizeof(vasp));   /* NV_VASPACE_ALLOCATION_PARAMETERS, default */
    nvkvm_m2_alloc1(s, C, DEV, VAS, 0x90f1u, vasp, sizeof(vasp), &st);
    uint32_t vst = st;
    uint32_t vmst = 0xffff;
    nvkvm_m2_alloc_virtmem(s, C, DEV, VIRT, VAS, &vmst);
    qemu_log("nvkvm-gpu[%s] M5.5 selftest P1: dev st=0x%x vaspace st=0x%x virtmem(0x0070) "
             "st=0x%x\n", s->chip->name, dst, vst, vmst);
    struct nvkvm_host_map hm;
    if (nvkvm_m2_host_alloc_map_vidmem(s, C, DEV, MEM, sz, &hm)) {
        uint32_t s1 = 0xffff, s2 = 0xffff; uint64_t v1 = 0, v2 = 0;
        int r1 = nvkvm_m2_map_dma(s, C, DEV, VIRT, MEM, 0, sz, false, 0, &s1, &v1);
        qemu_log("nvkvm-gpu[%s] M5.5 [P1a] map hDma=VIRTMEM NON-FIXED -> rc=%d st=0x%x "
                 "va=0x%llx%s\n", s->chip->name, r1, s1, (unsigned long long)v1,
                 (r1 == 0 && s1 == 0) ? "  OK" : "  <-- ERR");
        uint64_t want = 0x7f0000000000ull;
        int r2 = nvkvm_m2_map_dma(s, C, DEV, VIRT, MEM, 0, sz, true, want, &s2, &v2);
        qemu_log("nvkvm-gpu[%s] M5.5 [P1b] map hDma=VIRTMEM FIXED@0x%llx -> rc=%d st=0x%x "
                 "va=0x%llx%s\n", s->chip->name, (unsigned long long)want, r2, s2,
                 (unsigned long long)v2,
                 (r2 == 0 && s2 == 0 && v2 == want) ? "  OK FIXED-PLACEMENT-WORKS"
                                                    : "  <-- ERR");
        munmap(hm.qva, hm.size);
    } else {
        qemu_log("nvkvm-gpu[%s] M5.5 selftest P1: private vidmem alloc failed\n",
                 s->chip->name);
    }

    /* ---- Part 2: against the forwarded GR client's real VASpace ---- */
    uint32_t hDev = 0, hVas = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == hClient) {
            hDev = s->m2_devvas[i].dev; hVas = s->m2_devvas[i].vas; break;
        }
    }
    qemu_log("nvkvm-gpu[%s] M5.5 selftest P2: GR client 0x%08x -> host 0x%08x dev=0x%08x "
             "vas=0x%08x\n", s->chip->name, hClient, nvkvm_m2_client(s, hClient), hDev, hVas);
    if (hDev && hVas) {
        uint32_t hVirt = 0xdb000000u | (s->m2_databuf_next & 0xffffu);
        uint32_t hMem  = 0xda100000u | (s->m2_databuf_next++ & 0xffffu);
        uint32_t gvm = 0xffff;
        nvkvm_m2_alloc_virtmem(s, hClient, hDev, hVirt, hVas, &gvm);
        struct nvkvm_host_map gm;
        if (gvm == 0 && nvkvm_m2_host_alloc_map_vidmem(s, hClient, hDev, hMem, sz, &gm)) {
            uint64_t want = 0x200000000ull;       /* a guest-style VA in the GR vaspace */
            uint32_t gs = 0xffff; uint64_t gv = 0;
            int gr = nvkvm_m2_map_dma(s, hClient, hDev, hVirt, hMem, 0, sz, true, want, &gs, &gv);
            qemu_log("nvkvm-gpu[%s] M5.5 [P2] virtmem(0x%08x) over GR VAS 0x%08x map "
                     "FIXED@0x%llx -> rc=%d st=0x%x va=0x%llx%s\n", s->chip->name, hVirt,
                     hVas, (unsigned long long)want, gr, gs, (unsigned long long)gv,
                     (gr == 0 && gs == 0) ? "  OK GR-VAS-MAP-WORKS" : "  <-- ERR");
            munmap(gm.qva, gm.size);
        } else {
            qemu_log("nvkvm-gpu[%s] M5.5 [P2] virtmem alloc st=0x%x (skip map)\n",
                     s->chip->name, gvm);
        }
    }
}

/* M5.4 DATA-PLANE: back a forwarded channel's USERD with REAL host GPU memory.
 * The guest's NV_CHANNEL_ALLOC_PARAMS carries a userd memdesc (base@168, size@176,
 * addressSpace@184) naming a guest-FB address where the guest driver maps USERD via
 * BAR1. We allocate a host vidmem object, hand it to the host channel as
 * hUserdMemory[0] (@auxbuf+32) so the host channel USES it, mmap it into QEMU, and
 * register the guest-FB userd.base range in m2_fbback. Then guest reads/writes of its
 * USERD (GP_PUT/GP_GET) go through the BAR-aperture->FB path to the SAME host memory
 * the host GPU uses — the double-mmap that makes GP_GET observable to the guest poll.
 * Defensive: skips (leaving hUserdMemory[0] for the caller to zero) on any anomaly. */
static void nvkvm_m2_back_channel_userd(NvkvmGpuEmul *s, uint32_t hClient,
                                        uint32_t chanObj, uint8_t *auxbuf,
                                        uint32_t psize)
{
    if (psize < 192) {
        return;                              /* no userd memdesc present */
    }
    uint64_t ubase = ldq_le_p(auxbuf + 168);
    uint64_t usize = ldq_le_p(auxbuf + 176);
    uint32_t uas   = ldl_le_p(auxbuf + 184); /* addressSpace: 2=FBMEM, 1=SYSMEM */
    if (ubase == 0) {
        return;                              /* guest didn't place USERD — let RM alloc */
    }
    if (s->m2_chanbuf_n >= 32 || s->m2_fbback_n >= NVKVM_M2_MAX_FBBACK) {
        return;
    }
    /* Find the channel's device (VASpace parent) tracked for this client. */
    uint32_t hDev = 0;
    for (int i = 0; i < s->m2_devvas_n; i++) {
        if (s->m2_devvas[i].client == hClient) { hDev = s->m2_devvas[i].dev; break; }
    }
    if (!hDev) {
        qemu_log("nvkvm-gpu[%s] M5.4 USERD-back: no device for client 0x%08x — skip\n",
                 s->chip->name, hClient);
        return;
    }
    uint64_t asize = usize ? ((usize + 0xfff) & ~0xfffull) : 0x1000;
    uint32_t hUserd = 0xda000000u | (s->m2_databuf_next++ & 0xffffu);
    struct nvkvm_host_map hm;
    if (!nvkvm_m2_host_alloc_map_vidmem(s, hClient, hDev, hUserd, asize, &hm)) {
        qemu_log("nvkvm-gpu[%s] M5.4 USERD-back: host alloc failed (chan 0x%08x) — skip\n",
                 s->chip->name, chanObj);
        return;
    }
    uint64_t old_userd_off = ldq_le_p(auxbuf + 64);
    stl_le_p(auxbuf + 32, hUserd);           /* hUserdMemory[0] = host USERD handle */
    stq_le_p(auxbuf + 64, 0);                /* userdOffset[0] inside that object */
    s->m2_fbback[s->m2_fbback_n].fb_base = ubase;
    s->m2_fbback[s->m2_fbback_n].size    = asize;
    s->m2_fbback[s->m2_fbback_n].host_qva = hm.qva;
    s->m2_fbback_n++;
    s->m2_chanbuf[s->m2_chanbuf_n].client = hClient;
    s->m2_chanbuf[s->m2_chanbuf_n].chan   = chanObj;
    s->m2_chanbuf[s->m2_chanbuf_n].h_userd = hUserd;
    s->m2_chanbuf[s->m2_chanbuf_n].qva    = hm.qva;
    s->m2_chanbuf[s->m2_chanbuf_n].fb_base = ubase;
    s->m2_chanbuf[s->m2_chanbuf_n].size   = asize;
    s->m2_chanbuf_n++;
    qemu_log("nvkvm-gpu[%s] M5.4 USERD-back: chan 0x%08x USERD guest-FB 0x%llx "
             "(memdesc sz=0x%llx as=%u oldOff=0x%llx) -> host hUserd=0x%08x "
             "off=0 qva=%p asize=0x%llx "
             "[DOUBLE-MMAP]\n", s->chip->name, chanObj, (unsigned long long)ubase,
             (unsigned long long)usize, uas, (unsigned long long)old_userd_off,
             hUserd, hm.qva,
             (unsigned long long)asize);
}

/* M14: capture THIS host GPU's real device-info-table (engine enumeration) once, via a
 * private QEMU-owned client→device→subdevice on the host. The guest's classDB and KernelCE
 * objects are built from this table, so serving the host's real one makes the guest advertise
 * exactly the engines/classes the physical GPU has — no hardcoded per-GPU blob (which was
 * captured truncated: 10 entries, missing video engines → numClasses 97 vs host 107).
 * GPU-AGNOSTIC: contains zero GA106-specific data; works for any NVIDIA GPU the host exposes.
 *
 * FINDING (2026-06-06): 0x20801112 flags=0x5c040 has NEITHER _PRIVILEGED(0x4) NOR
 * _NON_PRIVILEGED(0x8) -> defaults to KERNEL_PRIVILEGED, so an UNPRIVILEGED isolate client
 * gets NV_ERR_NOT_SUPPORTED (0x1b) here. This code therefore falls back to the captured blob.
 * The unprivileged dynamic route (TODO if numClasses ever matters for correctness — it does
 * NOT for compute; the 10 missing classes are video engines NVENC/NVDEC/NVJPG/OFA, and the GR
 * compute class 0xc7c0 IS already advertised) is to SYNTHESIZE the table from GET_ENGINES_V2
 * (0x20800170, flags=0x48 = NON_PRIVILEGED), which an unprivileged client CAN issue.
 * Paginated: 32 entries/call, 100B/entry; bMore drives the loop. */
static void nvkvm_m2_capture_devinfo(NvkvmGpuEmul *s)
{
    if (s->m2_devinfo_tried) {
        return;
    }
    s->m2_devinfo_tried = true;
    if (!nvkvm_m2_iso_ensure(s)) {
        return;
    }
    const uint32_t C = 0xc1ee0011u, DEV = 0xde100011u, SUB = 0xde100012u;
    uint32_t st = 0xffff;
    uint32_t c0 = C;
    nvkvm_m2_alloc1(s, C, 0, 0, 0x0u, &c0, sizeof(c0), &st);           /* NV01_ROOT */
    if (st != 0) {
        qemu_log("nvkvm-gpu[%s] M14 devinfo: client alloc st=0x%x\n", s->chip->name, st);
        return;
    }
    uint8_t dev[56]; memset(dev, 0, sizeof(dev));
    nvkvm_m2_alloc1(s, C, C, DEV, 0x0080u, dev, sizeof(dev), &st);     /* NV01_DEVICE_0 */
    if (st != 0) {
        qemu_log("nvkvm-gpu[%s] M14 devinfo: device alloc st=0x%x\n", s->chip->name, st);
        return;
    }
    uint32_t sub = 0;
    nvkvm_m2_alloc1(s, C, DEV, SUB, 0x2080u, &sub, sizeof(sub), &st);  /* NV20_SUBDEVICE_0 */
    if (st != 0) {
        qemu_log("nvkvm-gpu[%s] M14 devinfo: subdevice alloc st=0x%x\n", s->chip->name, st);
        return;
    }
    static uint8_t buf[12 + 32 * 100];
    uint32_t base = 0, total = 0; int pages = 0;
    for (;;) {
        memset(buf, 0, sizeof(buf));
        stl_le_p(buf + 0, base);                  /* baseIndex */
        st = 0xffff;
        int rc = nvkvm_m2_control1(s, C, SUB, 0x20801112u, buf, sizeof(buf), &st);
        if (rc != 0 || st != 0) {
            qemu_log("nvkvm-gpu[%s] M14 devinfo: control base=%u rc=%d st=0x%x\n",
                     s->chip->name, base, rc, st);
            break;
        }
        uint32_t num = ldl_le_p(buf + 4), bMore = ldl_le_p(buf + 8);
        if (num > 32u) num = 32u;
        if (total + num > 256u) num = 256u - total;
        memcpy(s->m2_devinfo + (uint64_t)total * 100, buf + 12, (uint64_t)num * 100);
        total += num;
        if (++pages > 16 || !bMore || total >= 256u) break;
        base += num ? num : 32u;
    }
    s->m2_devinfo_n = total;
    qemu_log("nvkvm-gpu[%s] M14 devinfo CAPTURED %u engine entries LIVE from host GPU "
             "(no blob)\n", s->chip->name, total);
}

/* M5.3 DATA-PLANE PROOF: validate that QEMU can put REAL host GPU memory into its
 * own address space via the isolate's /dev/nvidia0 fd (received over SCM_RIGHTS).
 * Build a minimal client→device→subdevice on the host GPU, then exercise the
 * reusable primitive helper and write+read a pattern. */
static void nvkvm_m2_memtest(NvkvmGpuEmul *s)
{
    if (!nvkvm_m2_iso_ensure(s)) {
        return;
    }
    const uint32_t C = 0xc1ee0001u, DEV = 0xde100001u, SUB = 0xde100002u,
                   MEM = 0xde100003u;
    uint32_t st = 0xffff;
    /* client (NV01_ROOT): aux = NV0000_ALLOC_PARAMS {hClient} */
    uint32_t c0 = C;
    nvkvm_m2_alloc1(s, C, 0, 0, 0x0u, &c0, sizeof(c0), &st);
    qemu_log("nvkvm-gpu[%s] MEMTEST client    -> 0x%x\n", s->chip->name, st);
    /* device NV01_DEVICE_0 (0x0080): NV0080_ALLOC_PARAMS (deviceId + handles), 56B zeros */
    uint8_t dev[56]; memset(dev, 0, sizeof(dev));
    nvkvm_m2_alloc1(s, C, C, DEV, 0x0080u, dev, sizeof(dev), &st);
    qemu_log("nvkvm-gpu[%s] MEMTEST device    -> 0x%x\n", s->chip->name, st);
    /* subdevice NV20_SUBDEVICE_0 (0x2080): NV2080_ALLOC_PARAMS {subDeviceId}, 4B */
    uint32_t sub = 0;
    nvkvm_m2_alloc1(s, C, DEV, SUB, 0x2080u, &sub, sizeof(sub), &st);
    qemu_log("nvkvm-gpu[%s] MEMTEST subdevice -> 0x%x\n", s->chip->name, st);
    /* Exercise the reusable data-plane primitive: alloc + map real host GPU vidmem. */
    struct nvkvm_host_map hm;
    if (!nvkvm_m2_host_alloc_map_vidmem(s, C, DEV, MEM, 0x10000, &hm)) {
        qemu_log("nvkvm-gpu[%s] MEMTEST: host alloc+map primitive FAILED\n",
                 s->chip->name);
        return;
    }
    volatile uint32_t *p = (volatile uint32_t *)hm.qva;
    p[0] = 0xc0ffee01u; p[1] = 0xdeadbeefu;
    uint32_t r0 = p[0], r1 = p[1];
    qemu_log("nvkvm-gpu[%s] MEMTEST: *** mmap OK hva=%p  wrote/read 0x%08x 0x%08x "
             "-> %s ***  DATA PLANE PRIMITIVE WORKS\n", s->chip->name, hm.qva, r0, r1,
             (r0 == 0xc0ffee01u && r1 == 0xdeadbeefu) ? "PASS" : "MISMATCH");
    /* M5.3: query the host GPU-phys of this buffer — the value PROMOTE_CTX rewrite
     * will point the host GPU at, so the host GPU operates on the SAME memory the
     * guest sees through the (future) memslot backing. */
    uint64_t hphys = 0; uint32_t aper = 0xff;
    if (nvkvm_m2_host_phys(s, C, MEM, &hphys, &aper)) {
        qemu_log("nvkvm-gpu[%s] MEMTEST: host GPU-phys=0x%llx aperture=%u (0=VIDMEM)"
                 "  <- PROMOTE_CTX target\n", s->chip->name,
                 (unsigned long long)hphys, aper);
    }
    munmap(hm.qva, hm.size);
}

/* M6.0: RAMBlock iterator — record the largest fd-backed (memfd) block as guest RAM. */
static int nvkvm_m2_find_guest_ram(RAMBlock *rb, void *opaque)
{
    NvkvmGpuEmul *s = opaque;
    int fd = qemu_ram_get_fd(rb);
    uint64_t len = qemu_ram_get_used_length(rb);
    if (fd >= 0 && len > s->m2_guest_ram_size) {
        s->m2_guest_ram_fd   = fd;
        s->m2_guest_ram_hva  = qemu_ram_get_host_addr(rb);
        s->m2_guest_ram_size = len;
    }
    return 0;
}

static void nvkvm_gpu_emul_realize(PCIDevice *pci_dev, Error **errp)
{
    NvkvmGpuEmul *s = NVKVM_GPU_EMUL(pci_dev);
    const NvkvmGpuChip *chip = &nvkvm_chip_ga106;
    uint8_t *cfg = pci_dev->config;

    s->chip = chip;
    g_nvkvm_dma_s = s;                    /* M5.15 DIAG: enable DMA-write logging hook */
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
    /*
     * The guest driver currently leaves our MSI-X table disabled, so GSP/event
     * delivery falls back to pci_set_irq().  Advertise INTA# or Linux routes no
     * legacy IRQ at all (/proc/driver/nvidia/... reports IRQ: 0), making those
     * fallback interrupts invisible to the guest.
     */
    pci_set_byte(cfg + PCI_INTERRUPT_PIN, 1);

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

    /* M6.0 (item-4 prereq): locate the guest-RAM memfd so the stub can later mmap any guest
     * GPA + OS_DESCRIPTOR it for host-GPU DMA. Pick the largest fd-backed RAMBlock (the guest
     * RAM when run with -object memory-backend-memfd,share=on). fd=-1 if anon RAM. */
    s->m2_guest_ram_fd = -1;
    nvkvm_handle_table_init(&s->m2_ht);   /* M6.1: fd registry for sharing guest RAM to stub */
    qemu_ram_foreach_block(nvkvm_m2_find_guest_ram, s);
    qemu_log("nvkvm-gpu[%s] M6.0 guest-RAM memfd: fd=%d hva=%p size=0x%llx %s\n",
             chip->name, s->m2_guest_ram_fd, s->m2_guest_ram_hva,
             (unsigned long long)s->m2_guest_ram_size,
             s->m2_guest_ram_fd >= 0 ? "[shareable to stub for item-4]"
                                     : "[anon RAM — add memory-backend-memfd,share=on]");

    /* M5.1: forwarding (m2fwd) is lazy — the per-guest host isolate is created on
     * the first forwarded alloc in the cmdq path (nvkvm_m2_shadow_fwd). */
    /* M5.3: one-shot data-plane proof (QEMU mmaps real host GPU vidmem). */
    if (s->m2fwd) {
        nvkvm_m2_memtest(s);
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
    /* Host-GPU forwarding is the ONLY supported Mode-2 operating mode (there is no
     * pure-emulation-without-host-GPU path).  Default ON; the props remain solely as a
     * DEBUG off-switch for the no-host-GPU fake-the-boot bring-up (M0-M3). */
    DEFINE_PROP_BOOL("m2fwd", NvkvmGpuEmul, m2fwd, true), /* M5: host-GPU forwarding (always on) */
    DEFINE_PROP_BOOL("m2exec", NvkvmGpuEmul, m2exec, true), /* M5.7: execution-plane backing (always on) */
    DEFINE_PROP_UINT64("m2semval", NvkvmGpuEmul, m2semval, 0), /* M5.14 DIAG: ctx-poll sentinel */
    DEFINE_PROP_UINT64("m2sempage", NvkvmGpuEmul, m2sempage, 0x2efbaf000ull), /* M5.14 page */
    DEFINE_PROP_UINT32("m2semforcehigh", NvkvmGpuEmul, m2semforcehigh, 0), /* M8.100 DEBUG: force stuck CE finish high */
    DEFINE_PROP_STRING("m2pbmap", NvkvmGpuEmul, m2pbmap_path), /* M8.11 DEBUG: user-mmap PB VA->GPA map */
    DEFINE_PROP_UINT32("m2mapflags", NvkvmGpuEmul, m2mapflags, 0), /* M8.76 DEBUG: exact map-DMA flags */
    DEFINE_PROP_UINT32("m2mapflags_high", NvkvmGpuEmul, m2mapflags_high, 0), /* M8.76 high/UVM VAs */
    DEFINE_PROP_UINT32("m2pcas", NvkvmGpuEmul, m2pcas, 0), /* M8.89 inline-QMD PCAS injection mode */
    DEFINE_PROP_UINT32("m2refscan", NvkvmGpuEmul, m2refscan, 0), /* M8.94 broad UVM ref scan */
    DEFINE_PROP_UINT32("m2setobj", NvkvmGpuEmul, m2setobj, 0), /* M8.95 synthetic SET_OBJECT */
    DEFINE_PROP_UINT32("m2legacyvas", NvkvmGpuEmul, m2legacyvas, 0), /* M8.97 old c7c0 VAS sweep */
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
