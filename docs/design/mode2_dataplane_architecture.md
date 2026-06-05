# Mode-2 data-plane architecture (consolidated plan)

Status: design, 2026-06-05. Synthesizes the user's bookkeeping-table brainstorm with
the verified doorbell/chid findings (docs/design/mode2_doorbell_chid.md). This is the
buildable plan for the Mode-2 data plane: how guest GPU memory, the doorbell, and
GPU<->CPU DMA are backed by real host GPU resources via UNPRIVILEGED host ioctls.

## Core principle (Mode-1 proven): forward, don't emulate

Anything the guest *userspace* touches on the GPU — USERD, GPFIFO, pushbuffers, compute
data, context buffers — is **real host GPU memory**, double-mmapped into the guest, and
all allocation/mapping is **forwarded to unprivileged host nvidia ioctls** (the isolate/
stub). We do NOT interpret or emulate guest GPU work. Mode-1 proved this is feasible and
fast. If we find ourselves trapping a guest-userspace data range, that's a smell — those
ranges are meant to be forwarded, not trapped. (Interpretation/replay = the rejected
"approach B"; never go there.)

## Two planes

1. **Memory plane** — GPU-physical-backed (FB / sysmem): USERD, GPFIFO, pushbuffers,
   data, context buffers. Backed by real host GPU memory; CPU access via BAR memslots;
   GPU access via per-channel page tables. *Bookkeeping below.*
2. **Register plane** — MMIO registers, NOT GPU-physical: the USERMODE page (doorbell +
   PTIMER). Handled as a "special" object: read-only memslot (native reads), write-fault
   handler (doorbell = chid-translate + forward). See doorbell doc §13.

The older brainstorm's one flaw was putting the doorbell in GPU-physical space; corrected
— it's a register-plane special object. Everything else of the brainstorm holds and is
adopted below.

## Bookkeeping structures (adopted from the brainstorm)

### GPGA table — Guest GPU-Physical Address -> backing object
The emulated GPU's physical address space (FB) is pure bookkeeping. Every RM/object alloc
that consumes GPU-physical space gets a valid guest-GPU-physical address (GPGA) registered
here. Page-granular (4K) lookup GPGA -> range:
```
struct gpga_page_range { u64 gpga_addr; u64 size_pages; gpu_memory_object *target;
                         u64 offset_in_target; bool readable; bool writable; /*CPU bits*/ };
```
(= the address-virtualization #2 side-table, [[mode2-address-virtualization]].)

### gpu_memory_object — the backing descriptor
```
struct gpu_memory_object { gpu_mem_mode_t mode;          // special | general | physical
                           fault_handler_fn fault;       // NULL if none (e.g. doorbell write handler)
                           int nvkvm_handle;             // -1 unset (isolate/stub handle)
                           nvidia_handle_t nvidia_handle;}; // host RM handle, reachable via nvkvm_handle
```
- mode `physical`: a single static object representing real host GPU memory; BAR ranges
  reference it via gpga_page_range (the double-mmap target).
- mode `special`: register pages (doorbell/USERMODE, BAR registers) — often RO-mapped +
  a write fault handler.
- gpu_emul references objects by **nvkvm handles**, not raw nvidia fds.

### PDB table — per channel/context GPU-VA -> GPGA (or sysmem DMA)
Separate from GPGA (kept per channel/context, simple paging):
```
struct pdb_range { ctx_id channel; u64 va_addr; u64 gpga_addr; u64 va_size; };
```
Two leaf kinds: (1) -> GPGA (vidmem), (2) -> sysmem/CPU DMA (GPA). This is the VAS layer;
GPU->CPU DMA falls out of (2) by forwarding the guest's maps as host RM_MAP_MEMORY_DMA at
the SAME GPU VA (caller-fixed, unprivileged).

## CPU -> GPU: BAR memslots + lazy demand-fault

Per BAR region the guest configures on our PCIe device, install KVM memslots. Most GPU-
physical is never CPU-touched, so default to **trap** and back **lazily**:
- Reserve a host-side region (mmap, PROT_NONE) with a QEMU fault handler; KVM_SET_USER_
  MEMORY_REGION it at the BAR-offset GPAs where GPGA ranges should appear.
- On guest CPU access (fault): look up the GPGA -> gpu_memory_object; if it has a valid
  backend, **mmap it now** (RM_MAP_MEMORY on the stub's fd -> host VA, the proven Mode-1
  primitive) and install/refresh the memslot at that GPA. Multiple BAR offsets can alias
  one physical object. Keep memslots in sync when GPGA/BAR pages change.
- Register-only pages (no GPGA: doorbell, control regs) are separate `special` objects,
  usually fully trapped or RO-mapped.

## The doorbell (register plane) — see mode2_doorbell_chid.md §13

USERMODE page = `special` object, **RO memslot backed by the host USERMODE mapping**:
reads native (PTIMER nanosecond clock free), writes fault -> handler. The handler is the
chid namespace translator: {runlist, guest vChid} -> {runlist, host sChid} -> write host
doorbell. Adaptive RW(zero-trap)/RO(trap-write); see §13 + the chid namespace analysis.

## chid namespace (PID-namespace model)

Guest allocates base-0 vChids in its own namespace; host has system sChids. Translation is
a boundary function (like kernel PID-ns translation): real SR-IOV does it in the VF
doorbell HW (zero-trap); we do it in the doorbell write handler (trap). No host->guest
"chid taken" signal exists or should (it would break the namespace). Reservation of a host
sChid block is the privileged-PF job in real SR-IOV; unprivileged we self-reserve via the
forwarded FIXED channel allocs + handle collisions by translation. Identity (vChid==sChid,
single-tenant/no-collision) enables RW/zero-trap doorbell.

## GPU -> CPU DMA

Inherent in the PDB layer: a channel's GPU-VA -> sysmem leaf is forwarded as a host
RM_MAP_MEMORY_DMA into the host channel's VAS at the same GPU VA, backed by the guest-RAM
GPA (KVM/GPA-window). The host GPU then DMAs to the same memory the guest CPU sees.

## Build order

1. Stand up the forward backend in gpu_emul (isolate/handle/mmap from VirtIONvgpu) — M5.0.
2. GPGA table + gpu_memory_object + lazy BAR memslot fault-mmap (CPU->GPU memory plane).
3. Forward object/memory allocs -> populate GPGA; forward controls.
4. PDB per-channel forward (RM_MAP_MEMORY_DMA at guest VAs) — GPU VAS + GPU->CPU DMA.
5. USERMODE RO memslot + doorbell write handler (chid translate + forward); chid table.
6. cuCtxCreate -> first compute (the scrubber/ctx channels run on host; semaphore fires).

## Security (unchanged)

QEMU/stub unprivileged; only unprivileged nvidia ioctls; one isolate per guest userspace
process; apply Mode-1 allowlists/sanitizers before forwarding ([[access-model-split]]).

## Execution-plane build status (2026-06-05) — primitives PROVEN, assembly remaining

All execution-path primitives are built (gated behind the `m2exec` device prop, default
off = zero regression) and validated on the RTX 3060 / GA106 host:

- **map_dma FIXED** (`nvkvm_m2_map_dma`, NVOS46 V580): place a host memory object into a
  host VASpace at a chosen GPU VA. KEY: `hDma` must be an **NV01_MEMORY_VIRTUAL (0x0070)**
  mapper (`nvkvm_m2_alloc_virtmem`), NOT a raw FERMI_VASPACE_A (only virtual_mem.c
  implements MapTo). Proven into the live GR VASpace 0x5c000007.
- **ctx buffers already host-resident:** FIXED-mapping the PROMOTE_CTX va_map VAs returns
  0x51 NV_ERR_NO_MEMORY = already mapped (host RM self-promoted its GR ctx at the SAME guest
  VAs). Do NOT re-map them.
- **GPFIFO double-mmap:** resolve the GPFIFO guest-FB phys by walking the guest GR PDB
  (VA 0x200200000 -> FB 0xe0200000), `back_and_map` registers the FB overlay + FIXED-maps it
  into 0x5c000007 (st=0). Guest GP writes now land in the host channel's GPFIFO. USERD was
  already double-mmapped (M5.4).
- **doorbell primitives:** alloc host AMPERE_USERMODE_A (0xc561) + RM_MAP_MEMORY + mmap;
  fetch the GR channel work-submit token via NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN
  (0xc36f0108) -> token=0xc. Ringing = write token to usermode_qva + 0x90.

### Remaining final assembly (the cuCtxCreate keystone) — DO ATTENTIVELY
Order, each a checkpoint (the RING is the only wedge-risk step — keep it last):
1. **Forward channel schedule** (safe): `NVA06C_CTRL_CMD_GPFIFO_SCHEDULE` (0xa06c0101) on the
   GR TSG (0x5c000012; track it like m2_gr_channel), params `NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS`
   {bEnable=1}. The host TSG isn't scheduled today (schedule is a control; shadow_fwd only
   forwards allocs/frees).
2. **Map pushbuffers** (safe): at the guest doorbell, walk the guest GPFIFO entries
   [gp_get,gp_put) -> pushbuffer VAs -> resolve phys (guest PDB walk) -> `back_and_map` each
   (double-mmap + FIXED into 0x5c000007). Also double-mmap the completion-semaphore buffer so
   the guest's PRAMIN poll sees the HOST GPU's write.
3. **Disable chan_execute faking** under m2exec (so a green guest can ONLY come from the host).
4. **RING** (wedge-risk): on the guest doorbell write the token (0xc) to usermode_qva+0x90.
5. **VERIFY ON HOST**: `ssh vh nvidia-smi` must show utilization/process AND the completion
   semaphore must be written by the GPU (not QEMU). Guest-green + host-idle = emulated, FAIL.
   See [[mode2-real-forward-not-fake]]. If the host GPU wedges (100% util / no procs): reload
   the host driver (rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia; modprobe nvidia).

## cuCtxCreate blocker RE-DIAGNOSED (2026-06-05) — it's a GR-VAS page-table poll, not channels

CRASHWIN data (m2exec run): when cuCtxCreate hangs, the guest RM busy-loops (~13.7k iters)
manually WALKING/SCANNING its own GR VAS page tables via PRAMIN — PDE chain
0x2f3392000->0x2efbc3000->4000->5000 (PD0 dual-PDE: SMALL half @0x2efbc5000 = 0 / not
installed; BIG half @0x2efbc5008 -> 0x2efbc6000), then a swath of big-PT entries
(0x2efbc6188..6360). The walk TARGET page is never read; there are NO channel-USERD (gva!=0)
polls. So it is the guest KERNEL polling GR-VAS PAGE-TABLE STATE, not libcuda polling a
channel completion. => the busy worker channels (client 0xc1d00001, scrubber gpfifo
0x1210d0000) are UNRELATED to this hang; the M5.9 multi-channel channel-forward would NOT
clear cuCtxCreate. The real gap is GR-VAS page-table POPULATION (the guest awaits a mapping/
state GSP would install) — or an event the guest waits on before installing it. The
exec-forward primitives (map_dma/double-mmap/USERMODE/token/schedule) remain correct and
reusable, but must target populating the guest's GR page tables / the awaited mapping, not
channel rings, for cuCtxCreate. See memory mode2_cuctxcreate_pagetable_poll.

## cuCtxCreate crash access-path RESOLVED (2026-06-05) — un-backed CPU-mmap of RM sysmem

gdb + strace at the rbp=0 SIGSEGV: ALL guest open/ioctl/mmap SUCCEED (no failed fd; the
gdb rdi=-1 was a mid-computation value). The crash is libcuda dereferencing a NULL it READ
from a CPU-mmap'd RM buffer. Pre-crash pattern = RM_MAP_MEMORY (ioctl NR 0x4e / NVOS33) +
mmap(MAP_SHARED|MAP_FIXED) on /dev/nvidia0 and /dev/nvidiactl:
  mmap(0x200200000, 2 MiB,  /dev/nvidia0)     GPFIFO (FB; covered by m2_fbback via BAR1)
  mmap(0x200400000, 64 MiB, /dev/nvidiactl)   large RM region   <- NOT FB; uncovered
  mmap(0x77f2f2ddf000, 4 KiB,/dev/nvidiactl)  small RM struct (last mmap pre-crash)
The /dev/nvidiactl maps are RM SYSTEM/heap memory (NV_CTL device), NOT GPU FB -> NOT covered
by the FB/PRAMIN m2_fbback overlay -> content is UN-BACKED (zeros) -> libcuda reads a NULL
struct ptr -> rbp=0 deref. This is the access path the FB-read CRASHWIN probe couldn't see
(direct CPU mmap, not a PRAMIN/BAR access).

NEXT BUILD: back the RM_MAP_MEMORY+mmap'd buffers with REAL host content. On the guest's
RM_MAP_MEMORY (NR 0x4e), make the guest CPU mapping resolve to the forwarded host object's
real memory (double-mmap: we already forward the alloc; map the host object + overlay the
guest mmap target — including the /dev/nvidiactl sysmem maps, not just FB). Pin which mapped
buffer holds the NULL (instrument the 0x4e handler + the mmap GPA, or LD_PRELOAD-log the bytes
libcuda reads from 0x200400000 / 0x77f2f2ddf000 pre-crash). See memory
mode2_cuctxcreate_pagetable_poll. Supersedes the "multi-channel forward" and "page-table
population" next-steps (both were symptoms, not the crash).

## cuCtxCreate root cause CONFIRMED (2026-06-05): un-backed CPU mmap; fix = item-2 memory plane

gdb memory dumps at the crash: both GPU-mapped CPU regions libcuda reads are ALL ZEROS —
0x200200000 (GPFIFO, /dev/nvidia0) and 0x200400000 (64 MiB, /dev/nvidiactl). The crash is a
method dispatch through a zeroed structure (`*global -> +0x48 -> vtable -> call *0x560`) on the
GR channel -> rbp corrupted -> SIGSEGV. With m2exec=on the GPFIFO backing FIRES (m2_fbback at
FB 0xe0200000) but the region STAYS ZERO -> the guest's CPU mmap does NOT route through the FB
overlay (it uses the BAR1 mapping / possibly guest-RAM, which resolves elsewhere than the
channel-vaspace FB addr the overlay was keyed on). MECHANISM, not timing (backing ran before
crash; 133 doorbells). RULED OUT this session as the cause: channels, GR page-table population,
faked controls (fn=76), failed fds — all red herrings/symptoms.

THE FIX (singular) = plan item-2 CPU->GPU memory plane: back the guest's CPU mmaps of GPU
objects with the forwarded host objects' real memory. Immediate next step: instrument
nvkvm_baraperture_read to log the bar1_pdb resolve of VA 0x200200000 (and whether libcuda's
read traps there) -> decide BAR1-overlay vs KVM-memslot-over-guest-RAM -> back the CPU-mmap'd
regions. See memory mode2_cuctxcreate_pagetable_poll.

## MECHANISM RESOLVED: guest-RAM, not BAR1 — fix = Mode-1 GPA-window memslot (2026-06-05)

Code smoking gun: emulated BAR1 = 256 MiB (bar1_size=256<<20, MMIO). libcuda's CPU mmaps are
at 8 GiB+ VAs (0x200200000, 0x200400000) -> cannot be BAR1 accesses; CRASHWIN logged no BAR1
access. So the guest driver maps these GPU objects to GUEST RAM (no real VRAM), CPU reads
don't trap, and the m2_fbback overlay can never cover them (why m2exec leaves them zero).

FIX (singular, plan item-2) = back the guest-RAM GPA of each CPU-mmap'd GPU object with the
forwarded host object's real memory via a KVM memslot (KVM_SET_USER_MEMORY_REGION) — Mode-1's
GPA-window mechanism (docs: gpa_window_design). Intercept the guest object alloc + CPU-map to
learn the guest-RAM GPA + size, RM_MAP_MEMORY the host object -> host VA, install the memslot.
Reuses Mode-1 GPA-window code; NOT the FB overlay. This is the one remaining cuCtxCreate fix.

## cuCtxCreate DIAGNOSIS CONVERGED (2026-06-05): un-backed SYSMEM GR-context buffers

LD_PRELOAD mapshim correlated the crash buffers to RM objects (by RM_MAP_MEMORY len):
2 MiB GPFIFO = hMem 0x5c000014, 64 MiB = 0x5c000016, 4 KiB (last mmap before crash) =
0x5c000018 (client 0xc1d00003). NONE are in the SHADOW (GSP_RM_ALLOC fn=103) list -> the
guest RM allocated them LOCALLY (NVOS32/VidHeapControl, ioctl 0x2a; not forwarded) with no
GSP-RPC. Combined with 0 BAR1 reads -> these are SYSMEM (guest RAM) GR-context buffers the
guest CPU-RM manages itself (no GSP cooperation), CPU-mapped by libcuda, that the GPU should
fill (golden GR context / DMA) but doesn't (no host execution, no DMA forwarding) -> libcuda
reads zeros -> NULL deref -> rbp=0 SIGSEGV.

FIX = the full Mode-2 data plane (multi-week keystone): the guest-RM-managed sysmem GR
objects must be backed by host memory the host GPU fills — forward the guest's sysmem GPU
mappings (RM_MAP_MEMORY_DMA) so the host GPU DMAs into the guest RAM (GPU->CPU DMA, item-4) +
forward channel execution so the GPU runs the GR-context fill. Diagnosis fully converged:
channels (no), page-table poll (symptom), faked controls (not the direct filler), fds (ok);
the EXACT buffers + their local-sysmem-alloc path are now identified. See memory
mode2_cuctxcreate_pagetable_poll.

## item-4 hard prerequisite (2026-06-05): shared guest-RAM memfd so the stub can OS_DESCRIPTOR it

To back the guest's sysmem GR buffers, the host nvidia driver (STUB process) must
OS_DESCRIPTOR-register the guest RAM. But the stub is a separate process; QEMU's guest RAM
(anon mmap) isn't in its address space. Mode-1 works only because the guest nvkvm MODULE
allocates in a SHARED GPA-window (memfd); the stock Mode-2 driver allocates in ordinary guest
RAM with no cooperation. So item-4's prerequisite:
  1. back Mode-2 guest RAM with -object memory-backend-memfd; pass the fd to the emul device.
  2. share the fd to the stub (SCM_RIGHTS); stub mmaps it (any guest GPA -> stub VA).
  3. OS_DESCRIPTOR(memfd + gpa_offset, size) primitive (NV01_MEMORY_SYSTEM_OS_DESCRIPTOR 0x71).
  4. for each GR-VA->guest-GPA sysmem mapping (va_map sys=true / PROMOTE_CTX + more), OS_DESCRIPTOR
     + RM_MAP_MEMORY_DMA FIXED at the GR VA into the host GR VAS.
  5. forward channel execution so the host GPU DMA-fills the buffers libcuda reads.
The host-GPU-DMA-to-shared-RAM step is Mode-1-proven (partly de-risks the DMA-virt concern);
the work is the shared-RAM plumbing + GR-mapping enumeration + execution. Multi-week keystone.

## cuCtxCreate crash — CORRECTED root cause (2026-06-05 late): unfilled VIDMEM ctx buffer via BAR1

Boot-free host-vs-guest LD_PRELOAD diff (shims in tests/mode2/shims/) + guest hObject<->class
correlation DEFINITIVELY corrects the earlier "un-backed sysmem" diagnoses above — those were
WRONG. Findings:

- Host (real GA106, **same open KMD 580.159.04 + same libcuda** as guest) cuCtxCreate: allocates
  client/device/subdevice/vaspace then the per-channel set, does 25 RM_MAP_MEMORY but only ONE
  MAP_SHARED mmap (the 2MiB GPFIFO, which stays ALL-ZERO), and PASSES. A zero CPU buffer is not
  fatal.
- Guest replays the host's alloc sequence IDENTICALLY through 0xc56f (channel) + 0xc7c0 (compute),
  then SIGSEGVs (rbp=0) exactly where the host does its next 0xc7b5 (DMA copy).
- The crash buffers are **VIDMEM**: hMem 0x5c000016 (64MiB) and 0x5c000018 (4KiB) are alloc
  **class 0x3e = NV01_MEMORY_LOCAL_USER**, ret=0 (alloc SUCCEEDS). Only the GPFIFO (0x5c000014) is
  class 0x40 sysmem — sysmem on the host too. NEITHER side mmap()s the 0x3e buffers -> libcuda
  reads them via the **BAR1 aperture**, not a CPU mmap.
- HOST BAR1 read -> real GPU vidmem -> GPU-written **golden GR context** (non-zero) -> works.
  GUEST BAR1 read -> emulated vidmem (m2_fbback/guest-RAM) -> **ZERO** (never filled) -> NULL deref.

So the singular cuCtxCreate blocker = the emulated GPU's VIDMEM is never filled with the golden GR
context that the real GPU writes. FIX = CPU->GPU memory plane (item-2) for vidmem-via-BAR: the
guest's BAR1 read of the vidmem GR ctx (0x5c000016) must resolve to the host's real golden-context
vidmem. The host HAS the golden ctx in host vidmem (self-promoted from the forwarded channel/
compute allocs), but 0x5c000016 is allocated GUEST-LOCALLY (not in the fn=103 SHADOW stream) so
there's no host counterpart handle. Options: (A) host GPU fills it — OS_DESCRIPTOR 0x5c000016's
emulated-vidmem (guest-RAM) backing + map into the host GR VAS + trigger golden-ctx load
(execution); (B) copy host golden-ctx content into 0x5c000016's m2_fbback backing (needs to name
the host ctx buffer; GET_CTX_BUFFER_INFO was privileged 0x1b on the unprivileged stub). The CPU-RM
regkey RMInstLoc* (force aperture) does NOT change it — aperture is already vidmem; the problem is
content, not location. Supersedes ALL prior sysmem/page-table/channel diagnoses for this crash.

## REFACTOR PLAN (2026-06-05, user-directed): replace fb_pages with the GPGA/gpu_memory_object model

Decision (user): the ad-hoc memory backing is the core flaw and must be replaced wholesale,
not patched. Point-fixes (M5.7/M6.5/M6.6) kept dead-ending on it. Evidence it's the foundation,
not a logic bug: at the cuCtxCreate rbp=0 SIGSEGV, the libcuda dispatch chain is INTACT
(global->A(heap)->B(libcuda data)->FP=valid libcuda function with a normal prologue) — so it is
NOT a corrupt function pointer from a wrong control value; the corruption is downstream of the
broken memory model. Also: 0 BAR1 reads in the crash window, so the buffer libcuda faults on is
reached via a path the current fb_pages/m2_fbback design does not even cover.

WHY fb_pages is fatal: emulated "vidmem" = g_malloc0 pages in a hash (nvkvm_fb_page). Coherent
for guest-only access (BAR1/PRAMIN/BAR2 -> our handlers), but the real host GPU CANNOT touch it.
So nothing the host GPU must produce (golden GR ctx, compute output, HW semaphores) can ever land
where the guest reads it. Every GPU-physical byte that must cross to the real GPU has to live in
REAL host-GPU memory (a host RM object, double-mmapped), not malloc.

### Target model (from the bookkeeping section above)
- `gpu_memory_object { mode(special|general|physical); fault_handler; int nvkvm_handle;
  nvidia_handle; void *cpu_qva; uint64_t host_va_in_gr_vas; ... }` — one descriptor per real
  backing (a host RM_ALLOC, double-mmapped: cpu_qva for QEMU/guest-CPU view, mapped into the host
  GR VAS at the guest VA for the host-GPU view).
- GPGA page table: `gpga_page_range { u64 gpga_addr; u64 size_pages; gpu_memory_object *target;
  u64 offset_in_target; bool readable, writable; }` — page-granular GPGA -> (object, offset).
- `nvkvm_fb_read/write` resolve fb_addr(=GPGA) -> gpga_page_range -> object->cpu_qva+offset.
  fb_pages becomes the FALLBACK only for GPGAs with no real backing (pure guest bookkeeping that
  the host GPU never needs); everything GR/channel/ctx is real-backed.

### Sequenced increments (each gated by m2exec, each a commit + no-regression boot)
- R1. Introduce structs + a gpga table keyed by page; `nvkvm_m2_gpga_lookup(fb_addr)`. Route
  nvkvm_fb_read/write through it (miss => current fb_pages path => zero behavior change). COMMIT.
- R2. `nvkvm_m2_gpga_back(va, fb_addr, size)`: alloc ONE blank host vidmem gpu_memory_object,
  CPU-map it (cpu_qva), map_dma it into the host GR VAS at `va`, register gpga_page_range(s) for
  [fb_addr, fb_addr+size). Both views = one host object => coherent. Replaces back_and_map's
  split FB-overlay-vs-map. COMMIT.
- R3. SOLVE 0x51: do NOT let the host self-promote its GR ctx (which collides). Either (a) before
  the host constructs the ctx, pre-map OUR objects at the ctx VAs so the host adopts them; or
  (b) intercept PROMOTE_CTX and substitute our gpu_memory_object handles so the host promotes
  OURS. Goal: every GR-ctx VA is backed by an object we own (CPU+GPU). COMMIT.
- R4. Drive R2 from the guest PDB walk (M6.5 enumerator) for BOTH apertures: each leaf {VA, GPGA,
  sys} -> gpga_back. Lazy variant: on a BAR1/PRAMIN miss in the crash path, back on demand. COMMIT.
- R5. Re-test cuCtxCreate. Expect: libcuda's faulting buffer now resolves to real host memory the
  host GPU also sees; rbp=0 should clear or move. VERIFY host nvidia-smi for real work
  ([[mode2-real-forward-not-fake]]). COMMIT.

### Risks / notes
- fb_pages is used by boot/page-table/channel paths; R1 must be a pure pass-through (miss=fb_pages)
  to avoid breaking boot. Gate everything on m2exec.
- The hard core is unchanged by the refactor: connecting the guest buffer to host-GPU-written
  content (R3 0x51 + R4 VA mapping). The refactor makes that connection EXPRESSIBLE (one object,
  both views) instead of impossible (malloc). It does not by itself prove the host GPU fills the
  ctx — that's R5 + the execution/doorbell plane (still the DMA-virt gate).

## REFACTOR perf rules (user, 2026-06-05): trap only rare triggers; hot paths native via memslots

Two refinements that apply across the refactor — the guiding rule is "trap only the rare control/
trigger events; make every hot path native via KVM memslots."

### BAR0 is THREE tiers (not two)
boot/GSP/PMC/control regs can't come from host ioctls (privileged/root-only) -> fully simulated,
agreed. But split BAR0 by access pattern:
  1. CONSTANT regs (chip id, fused caps, invariant read-only config): KVM_MEM_READONLY memslot
     PRE-WRITTEN with the constant values -> reads native (no exit), writes trap/ignored. Kills
     the exit cost of the many constant-register polls during init.
  2. DYNAMIC/logic regs (GFW_BOOT progress, WPR2 state machine, the GSP-RPC doorbell 0x110c00,
     anything needing emulation logic): MMIO-emulated, both directions trap.
  3. USERMODE + PTIMER window: host-mapped RO (real host regs) -> native reads (real PTIMER),
     doorbell writes trap (chid translate -> host doorbell). (See doorbell §15.)
  CAVEAT: classify carefully — some "config" regs change during boot (GFW_BOOT, WPR2); those stay
  in tier 2. Only TRULY invariant data goes in the tier-1 RO-constant memslot.

### PDB tables: never trap per-access; walk live + re-sync on trigger
Trapping every page-table read/write is a hot-path killer. Instead:
  - Back the PDB/FB memory with a real object as a NORMAL RW memslot -> the guest reads/writes its
    page tables NATIVELY, untrapped.
  - Keep NO separate copy. WALK the live RAM-backed tables on-demand (via cpu_qva) only when we
    actually need a resolution: backing a new buffer, or a doorbell/exec. Always reads current
    state; no per-write trap, no staleness.
  - Use the TLB-INVALIDATE as the proactive re-sync hook (the guest MMU invalidate register write
    — rare, we DO trap it) ONLY when we must react to a change: tear down a memslot for an
    unmapped range, or install one for a newly-mapped GPGA. Steady-state PTE r/w is never trapped.

Consistent rule: trap rare triggers (invalidate, doorbell, dynamic regs); hot (PTIMER reads, PDB
r/w, backed GPGA access) is native via memslots.
