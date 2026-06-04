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
