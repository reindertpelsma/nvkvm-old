# Mode-2 compute forwarding (M5) — implementation spec

Status: design, code-accurate, 2026-06-04. This is the buildable plan for M5
("first real compute"): make cuCtxCreate and real kernels work by forwarding the
guest's RM control plane to a real host GPU and backing all GPU memory with real
hardware. Supersedes the high-level M5 bullet in [[mode2-plan]].

## Why (the gate)

cuCtxCreate crashes because the GR/context buffers are never populated by real
GPU work — forging GSP-RPC completions ([[mode2-promote-ctx-and-uvm-wall]]) gets
cuInit + device enumeration but not a usable context. The context state must be
produced by a real GPU. So we stop faking the RM control plane and **forward it**.

## Core architecture: forward the RM control plane, fake only the boot/GSP

Today the fake GSP ECHOES every GSP_RM_ALLOC (fn=103) / GSP_RM_CONTROL (fn=76).
M5 splits these into three buckets (M4 classification):
- **fake/static** — GSP boot, GET_GSP_STATIC_INFO, INTERNAL_* GR static info,
  intr table: keep replaying captured GA106 answers (no host GPU needed).
- **forward** — real object allocs (device/subdevice/VASPACE/channel/CE/compute/
  memory) and their controls: translate to the equivalent host RM ioctl and run
  it on a **real host GPU** via a per-guest isolate.
- **emulate** — a few that must be answered locally for the emulated front
  (doorbell/USERD plumbing) — minimize these.

The guest's stock RM still runs and thinks the fake GSP did the work; in reality
the per-guest host isolate did it on the real GPU. This is the Mode-1 forwarding
model moved one level down — from the userspace-ioctl boundary (Mode-1) to the
RM↔GSP RPC boundary (Mode-2) — reusing the same stub/isolate/mmap stack.

### GSP-RPC → RM-ioctl translation is a near-direct field re-pack
- `GSP_RM_ALLOC` body {hClient@80, hParent@84, hObject@88, hClass@92,
  paramsSize@100, params@112} → `NV_ESC_RM_ALLOC` (NVOS64) {hRoot, hObjectParent,
  hObjectNew, hClass, pAllocParms, paramsSize}. Forward params as the aux blob.
- `GSP_RM_CONTROL` body {hClient@80, hObject@84, cmd@88, paramsSize@96,
  params@120} → `NV_ESC_RM_CONTROL` (NVOS54) {hClient, hObject, cmd, params,
  paramsSize}. Forward params as aux.
- `FREE`(10) → `NV_ESC_RM_FREE`.

### Verbatim handles — no handle translation needed
RM_ALLOC is caller-chooses-handle (the caller supplies hObjectNew). The per-guest
isolate is DEDICATED to one guest, so the guest's handle namespace (hClient
0xc1d…/0xc1e…, objects 0x5c…/0xcaf…) can be used **as-is** on the host RM. Forward
the guest's chosen handles verbatim → the host builds the identical object tree
with identical handles → guest↔host handle spaces coincide → zero translation.
(Distinct from Mode-1, which translated; Mode-2's dedicated-isolate-per-guest lets
us skip it. Re-verify no collision with stub-internal handles.)

### Memory backing = double-mmap; channel structures then run NATIVELY
When the guest allocates GPU memory (NV01_MEMORY_LOCAL_USER vidmem / sysmem /
os-descriptor) the forwarded host RM_ALLOC creates **real** host GPU memory. The
guest maps it (RM_MAP_MEMORY) → host returns a mappable fd/offset → we
`nvkvm_mmap_create` + `nvkvm_mmap_map_to_guest` to install that real host buffer
into the guest **GPA window** at the GPA the guest expects (Mode-1 machinery).
Consequence: the channel's USERD/GPFIFO/pushbuffer are **real host GPU memory
mapped into the guest**. When the guest writes GP_PUT to USERD (through the
double-mmap, hitting the real host USERD), the **real host GPU runs the work** —
no method re-modeling, no pushbuffer replay. The emulated doorbell/USERD path
becomes a thin nudge (or a no-op if the mapped doorbell suffices). This is the
crux that makes "first real compute" tractable: we never interpret GR/compute
methods, we let real silicon execute the guest's own pushbuffers.

### GPU-VA verbatim too
The guest chooses GPU-VAs (gpFifoOffset, MAP_MEMORY_DMA VA). RM_MAP_MEMORY_DMA is
caller-specifies-VA, so the forwarded map uses the **same** GPU-VA in the host
VAS → guest GPU-VA == host GPU-VA. The [[mode2-address-virtualization]] side-table
(PROMOTE_CTX) stays as the bookkeeping/validation layer and for the emulated
front's own reads; forwarded compute uses host-native VAs that match.

## Reuse surface (exact, from the Mode-1 stack — all virtio-independent)

Call directly from `nvkvm_gpu_emul.c` (same QEMU process):
- `VirtIONvgpu *nvkvm_get_global_device(void)` — the device singleton (isolate
  table, handle table, mmap/GPA-window state).
- `int nvkvm_isolate_create(table, session_id, nv, &isolate_id)` — per-guest
  isolate (spawns the sandboxed stub child; SEQPACKET socket; reader thread).
- `int nvkvm_isolate_ioctl(table, isolate_id, handle_id, cmd, param_buf,
  param_size, aux_buf, aux_size, flags, &nvstatus, &fault_addr)` — **forward an
  RM ioctl to the host GPU**; param/aux are plain QEMU-process buffers; returns
  retval + NvStatus. (nvkvm_isolate.h:270; NOT coupled to virtio.)
- `nvkvm_handle_open_nvidia(table, session_id, dev_id, flags, &handle_id)` /
  `nvkvm_handle_get` / `nvkvm_handle_acquire_fd` — open host /dev/nvidia*,
  translate handle→host fd.
- `nvkvm_mmap_create(nv, hfd, offset, length, prot, flags, &region)` +
  `nvkvm_mmap_map_to_guest(nv, region)` — double-mmap a host GPU buffer into the
  guest GPA window; `region->guest_pa` is the GPA. `nvkvm_mmap_destroy` to free.
- `nvkvm_gpa_to_vmm_va(nv, gpa, size)` — GPA → QEMU VA (for the emulated front to
  read forwarded buffers, e.g. snoop USERD/GPFIFO).
- Wire protocol to the stub: `struct isolate_cmd_ioctl` / `isolate_resp_ioctl`
  (src/common/nvkvm_isolate_proto.h) — already handled by nvkvm_isolate_ioctl.

Refactor flag: `nvkvm_req_ioctl_on_isolate()` (nvkvm_isolate_handlers.c) bakes the
security allowlists onto the virtio path. The emulated GPU must apply the SAME
gates (frontend NR / alloc class / control cmd / cross-VM hClient) before calling
`nvkvm_isolate_ioctl()` — factor the gate checks into a shared helper and call it
from both. Do NOT forward ungated.

## Build increments (each commit-and-test; keep forge path as fallback)

1. **M5.1 — isolate + root client.** On the guest's first forwardable
   GSP_RM_ALLOC (NV01_ROOT_CLIENT), lazily `nvkvm_isolate_create` a per-guest
   isolate, open a host /dev/nvidiactl handle, forward the alloc. Verify the stub
   creates the client (host nvstatus==0). Gate the whole forwarder behind a device
   property (default OFF) so the working forge path is untouched until ready.
2. **M5.2 — object tree.** Forward device/subdevice/VASPACE/channel-group/channel/
   CE/compute-class allocs + their controls (PROMOTE_CTX, GPFIFO_SCHEDULE, etc.).
   Build the fake/forward classification table (extend M4). Verify the host builds
   the full context object tree; cuCtxCreate should stop crashing once the GR
   context is a real host allocation.
3. **M5.3 — memory double-map.** Forward memory allocs + RM_MAP_MEMORY /
   MAP_MEMORY_DMA; back each with `nvkvm_mmap_create`/`map_to_guest` at the guest
   GPA. Now context buffers, USERD, GPFIFO, pushbuffers are real host GPU memory.
4. **M5.4 — first compute.** Guest writes GP_PUT through the mapped USERD → real
   host GPU executes. Forward/emulate the doorbell as a nudge. Target: cup2's
   `cuCtxCreate → cuMemAlloc → cuMemcpyHtoD/DtoH` round-trip PASS byte-exact.
5. **Managed memory** — per [[mode2-uvm-residency]] (host managed alloc behind the
   GPA window + quiescent guest UVM); spike the cudaMallocManaged round-trip.

## Security (unchanged invariants)

- QEMU stays **unprivileged**; only unprivileged nvidia ioctls on the host
  ([[access-model-split]]). No host root.
- **One sandboxed isolate per guest userspace process** ([[isolate-architecture]]);
  contexts of one process coalesce. Process identity = guest userspace address
  space ([[mode2-isolation-cr3-key]]).
- Apply the Mode-1 allowlists/sanitizers before forwarding; the guest is
  untrusted. Guest data that could go OOB only if the guest *kernel* broke its
  contract → still validate (per the input-validation policy in [[mode2-plan]]).

## Risks to validate during the build

- Stub-internal handle collisions with the guest's verbatim handles (namespace).
- Whether RM accepts the guest's chosen GPU-VAs verbatim in the host VAS (VA-space
  layout differences) — fall back to translation via the side-table if not.
- The emulated doorbell vs. a real mapped work-submit doorbell — may need a nudge
  RPC or a trapped MMIO that pings the host channel.
- cuCtxCreate's first forwarded alloc that previously crashed (object 0x5c00001a)
  — confirm it now succeeds with real backing.
