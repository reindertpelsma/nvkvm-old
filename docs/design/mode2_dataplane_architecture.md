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
