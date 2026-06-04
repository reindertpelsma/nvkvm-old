# Mode-2 compute forwarding (M5) — implementation spec

Status: design, code-accurate, 2026-06-04. This is the buildable plan for M5
("first real compute"): make cuCtxCreate and real kernels work by forwarding the
guest's RM control plane to a real host GPU and backing all GPU memory with real
hardware. Supersedes the high-level M5 bullet in [[mode2-plan]].

## STATUS 2026-06-04: control-plane forwarding WORKS; data-plane is the decision

The shadow-forward harness (commits 4d37233→2e1bd54) drove the guest's REAL RM
stream onto the live GA106 through every reconciliation layer, each empirically
guided by the host kernel's own dmesg:
- clients remapped (0xc1xxxxxx collide with host clients → 0xdeadNNNN) [M5.1b]
- FREE forwarded; error-notifier dropped [M5.1c]
- channel USERD/instance: zero hUserdMemory[0] → host RM allocates them [M5.3a]
=> the FULL object tree, **including channels (class 0xc56f), now constructs on
the real GPU** for the primary client. Two known residuals: non-kernel-priv
forwarded clients can't skip the memory scrubber (chid exhaustion for the compute
/RM-internal clients), and a channel-group VASpace handle for some clients.

### The remaining work is the DATA PLANE, and it forks (a decision)
Everything above is the control plane (object tree). Real compute needs the
data plane: the guest must actually RUN on the host channels. Two paths, and the
choice has large effort implications — flagging it as the architectural decision:

- **(a) Continue op-by-op forwarding + double-mmap (authoritative).** We're far
  along (tree + channels forward). Remaining: flip shadow→authoritative (return
  host results to the guest); **VAS reconciliation** — make the host channel's
  VAS map the SAME GPU-VAs the guest chose (gpFifoOffset 0x121010000, pushbuffers
  0x120000000…) so the guest's GPFIFO contents (which reference guest GPU-VAs)
  are valid on the host GPU; double-mmap the host channel's RM-allocated
  USERD/GPFIFO into the guest's emulated FB at the guest's offsets so the guest's
  GP_PUT drives real silicon; resolve the scrubber/priv + vaspace residuals. The
  hard part is VAS reconciliation — the guest's and host's CPU-RM each pick their
  own VA/FB layout, and bridging them per-buffer is the long tail's tail.
- **(b) Clean host context + forward only compute.** Keep faking the control
  plane (cuInit already works), maintain OUR OWN clean host GR/compute context
  (built once by QEMU), copy its golden context image into the guest's emulated
  context buffers (fixes the cuCtxCreate crash without mirroring allocs), and
  forward only the COMPUTE pushbuffers with guest-GPU-VA→host-GPU-VA translation
  (the PROMOTE_CTX side-table) + double-mmap of the compute buffers. Shorter
  reconciliation (only buffer addresses, not the whole object tree + VAS), but
  needs the golden-image copy + matching the guest's context config + compute
  replay.

COMPLICATION (found 2026-06-04): in GSP-RM the VA→phys maps for the cuInit/UVM
channels are filled GSP-side — there is NO forwardable MAP_MEMORY_DMA (fn=14) in
the stream (only fn=76/103/10). So path (a)'s VAS reconciliation can NOT be done
by forwarding the guest's maps for those channels (they don't exist as RPCs). It
CAN for the compute/GR context (PROMOTE_CTX carries the VA↔phys). So:
- Path (a) works cleanly for the COMPUTE channel (PROMOTE_CTX gives the VA maps)
  but for the UVM/RM-internal channels we'd have to reconstruct the host VAS some
  other way (e.g. let the host RM map at the same gpFifoOffset via the channel
  alloc's gpFifoOffset field, which IS in the params, + accept RM's other layout).
- Path (b) sidesteps the UVM-channel VAS entirely: keep faking them (cuInit
  already works that way via the forge), copy a captured host GR golden-context
  image into the guest's context buffer to clear the cuCtxCreate crash, and
  forward only the compute channel (which HAS PROMOTE_CTX maps). The golden image
  is mostly VA-independent GR pipeline state, so a one-time capture+replay is
  plausible; verify it doesn't embed context-specific VAs.

SYNTHESIS — the HYBRID (clearest path, resolves the fork): the two paths aren't
exclusive. The cuCtxCreate crash (gdb RE in [[mode2-promote-ctx-and-uvm-wall]]) is
libcuda dereferencing NULL from an RM_ALLOC that returned faked/zero data — so
flipping THAT alloc (and the compute context's tree) to AUTHORITATIVE (return the
host's real result + double-mmap its buffers) directly fixes it. And the compute
context IS the path that has PROMOTE_CTX maps, so its VAS reconciles cleanly.
Meanwhile the UVM/cuInit channels (the ones with GSP-internal maps) STAY FAKED via
the working forge — we never need to forward them for compute. So:
  - Control plane for cuInit / UVM channels: keep faking (forge) — already works.
  - Compute context (cuCtxCreate's GR ctx + compute channel + its memory):
    forward AUTHORITATIVELY (return host results to the guest) + double-mmap the
    buffers, using the proven M5.0–M5.3a machinery + the PROMOTE_CTX VA maps.
This is selective authoritative forwarding: fake the hard (UVM) parts, forward the
compute parts where we have everything needed. It reuses all the M5.x work, fixes
cuCtxCreate at its actual cause, and sidesteps the UVM-channel VAS problem.
NEXT BUILD: identify the compute-context alloc/control/channel set (the 0xc1e…
CUDA client's GR ctx + compute channel + PROMOTE_CTX'd buffers), forward those
authoritatively, double-mmap their memory into the emulated FB, keep everything
else faked. Then cup2 cuCtxCreate→cuMemAlloc→memcpy should pass.

(superseded recommendation kept for history:) lean (b) for the FASTEST path
(it reuses the working forge for the hard UVM channels and only forwards the
compute channel, whose maps we already have via PROMOTE_CTX), with (a)'s
machinery (now proven) kept for the compute channel's object tree. But this is a
real strategic fork with large effort either way — the USER's call, given the
GSP-internal-map complication makes neither obviously dominant. Control-plane
forwarding (object tree + channels) is DONE and reusable in both.

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

### Handles — objects verbatim, CLIENTS need a map (M5.1a finding)
CORRECTED by the M5.1a shadow-forward test (2026-06-04): RM_ALLOC is
caller-chooses-handle for OBJECTS under a client (device/subdevice/vaspace/channel:
0xcaf…/0x5c… — these are scoped to their client's handle space and forward
verbatim fine). But CLIENT handles do NOT: forwarding the guest's client handles
(0xc1e00004 …) verbatim mostly fails with **NV_ERR_INSERT_DUPLICATE_NAME (0x19)** —
they collide with PRE-EXISTING host RM clients (persistenced/desktop live in the
same global 0xc1xxxxxx namespace), and every object under a failed client then
cascades to NV_ERR_INVALID_CLIENT (0x23). (A few guest client handles that happen
to be free on the host succeed — confirming it's a collision, not a format bug.)
So the reconciliation layer is: **let the host RM ASSIGN the client handle**
(NV01_ROOT/NV01_ROOT_CLIENT, pass hObjectNew=0, capture the returned handle), keep
a guest-client → host-client map, and translate the client refs (hRoot in NVOS64,
hClient in NVOS54, and any hParent that names a client) on every forwarded op.
Object handles within a client stay verbatim. GPU-phys reconciliation (channel
instanceMem.base etc.) is the NEXT layer after this — the M5.1a run couldn't reach
it because the client allocs failed first.

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

## Integration prerequisite (M5.0) — stand up the forwarding backend in Mode-2

VERIFIED 2026-06-04: the Mode-2 VM instantiates ONLY `-device nvkvm-gpu-emul`
(run_mode2_vm.sh:76; its own header comment line 12: "NO virtio-nvgpu / nvkvm-gpu
identity device — Mode-2 forwards nothing yet"). So the Mode-1 forwarding backend
(`VirtIONvgpu` singleton: isolate table, handle table, sparse GPA window, KVM-slot
allocator) is **NOT initialized** — `nvkvm_get_global_device()` returns NULL and
every reuse function above is unreachable. M5's true first step is to provide that
backend. Two options:
- **(A) Factor the backend out of `VirtIONvgpu`** into a standalone
  `nvkvm_forward_backend` (isolate table + handle table + mmap/GPA-window state +
  KVM fd) that `nvkvm_gpu_emul`'s realize() initializes directly — no virtio
  device. Cleanest for Mode-2 (no spurious virtio-nvgpu in the guest).
- **(B) Also instantiate a (headless) virtio-nvgpu backend** alongside the
  emulated GPU purely for its infrastructure, ignoring its guest-facing virtqueue.
  Faster to wire, but adds a guest-visible device we don't want.
Recommend (A). Also resolve **GPA-window sharing**: Mode-1's GPA window is its own
512 GiB KVM memslot; Mode-2's emulated GPU has its own FB BAR + the
[[gpa-window-design]] window. Decide whether forwarded host buffers install into
the emulated GPU's BAR-backed window or a dedicated Mode-2 forward window
(MAP_FIXED slices either way). This is the one piece needing attended design
before coding M5.1.

## Build increments (each commit-and-test; keep forge path as fallback)

0. **M5.0 — backend init (above).** Factor `nvkvm_forward_backend` out of
   `VirtIONvgpu`; initialize it in `nvkvm_gpu_emul` realize(); decide GPA-window
   sharing. Gate behind a device property (default OFF).
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
