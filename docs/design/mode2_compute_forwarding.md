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

## Data-plane double-mmap — code-grounded mechanism (2026-06-04)

Read of the Mode-1 mmap path (nvkvm_mmap_host.c) settles the "how does QEMU map
the stub's GPU memory" question:
- `nvkvm_mmap_create(nv, hfd, offset, length, prot, flags, &region)` does
  `mmap(NULL, length, prot, flags, hfd->fd, offset)` — i.e. QEMU mmaps a **host
  nvidia fd it holds in its OWN process** at the RM-returned mmap offset, giving a
  host VA backed by real GPU memory.
- `nvkvm_mmap_map_to_guest(nv, region)` then `KVM_SET_USER_MEMORY_REGION`s that
  host VA at a GPA → the guest sees the real GPU memory at that GPA.

Implication for Mode-2: the forwarded channel memory is allocated by the STUB's
RM client (sandboxed). For QEMU to mmap it, QEMU needs (1) its own host fd to
/dev/nvidia0, and (2) the memory accessible from QEMU's client — i.e. **DUP the
stub-allocated object into a QEMU-side RM client** (NV_ESC_RM_DUP_OBJECT), then
RM_MAP_MEMORY on QEMU's fd to get the offset, then `nvkvm_mmap_create` on QEMU's
fd. Mode-1 already runs a QEMU-side handle table + the stub; Mode-2 reuses both.

Concrete Mode-2 data-plane build (the remaining work):
1. QEMU opens its own /dev/nvidia0 (nvkvm_handle_open_nvidia) — for mmap.
2. For each forwarded memory object the guest will mmap (compute-context buffers,
   channel USERD/GPFIFO once RM-allocated), DUP it from the stub's client into a
   QEMU-side client (grant share rights at alloc), RM_MAP_MEMORY on QEMU's fd.
3. nvkvm_mmap_create QEMU's fd at the offset; back the EMULATED FB BAR's fb_pages
   at the guest's GPU-phys (or the GPA window) with that host VA — so the guest's
   BAR1/PRAMIN writes land in real GPU memory and the host GPU reads the guest's
   data. (Double-mmap: one physical buffer, two views.)
4. VAS: for the compute channel, the host channel's VAS must map the guest's GPU-VAs
   (gpFifoOffset etc.); use the channel alloc's gpFifoOffset field (caller-specified,
   IS in params) + PROMOTE_CTX maps to align them.
5. Flip the compute-context allocs/controls to AUTHORITATIVE (return host results);
   keep faking UVM/cuInit.
This is intricate (cross-process DUP + VAS alignment) and is the focused build that
remains; the control plane + the mmap mechanism are now both settled.

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

---

## M5.3 status update (2026-06-04): data-plane primitive PROVEN

Commit 651d860. The host-side half of the double-mmap is validated end-to-end:
QEMU allocs real GA106 vidmem (client/device/subdevice/memory all NV_OK), maps it
(`RM_MAP_MEMORY` on **/dev/nvidiactl** — it is `NV_CTL_DEVICE_ONLY`, escape.c:521 —
with the NVOS33 `fd` field naming the device fd), and `mmap`s the SCM_RIGHTS fd
into QEMU's address space at **offset 0** (nvidia device mmap requires
`vm_pgoff==0`, nv-mmap.c:533; the kernel uses the per-fd `mmap_context`
`rm_create_mmap_context` registered). Wrote/read `0xc0ffee01/0xdeadbeef` through
host BAR1 — byte-exact PASS. See [[mode2-map-memory-control-device]]. Reusable
sequence lives in `nvkvm_m2_memtest()` (gated by `m2fwd`, realize-time).

### The integration that remains (the real M5.3)

The emulated GPU's FB is a **sparse malloc-backed emulation** (`fb_pages`
GHashTable of 4 KB pages, reached via the BAR0 PRAMIN window). The guest's
cuCtxCreate context buffers live at guest-FB-GPAs that the guest's faked RM PMA
assigns; libcuda then CPU-mmaps them (seen in strace at guest VA 0x200xxxxxxx via
nvidia-uvm). Today those pages are malloc'd and **never populated** with real GPU
state → libcuda dereferences garbage → SIGSEGV. To fix:

1. **Reconcile** each guest context buffer (known guest-GPU-phys + size from the
   PROMOTE_CTX side-table, `nvkvm_record_va_map`) with the corresponding **host**
   context buffer the forwarded cuCtxCreate allocated on the real GA106. This
   guest-FB-GPA ↔ host-GPU-mem matching is the hard kernel (two independent RMs
   pick independent physical addresses).
2. **Map** the host context buffer into QEMU via the proven primitive → QEMU VA.
3. **Back** the guest-FB-GPA range with that QEMU VA via a KVM memslot (replace the
   malloc'd `fb_pages` for that range). Then guest-CPU mmaps AND guest-GPU-PTE
   walks that resolve to those FB pages hit real host GPU memory; the host GPU's
   writes during real execution become visible to the guest. This mirrors Mode-1's
   sparse GPA window (nvkvm_isolate_handlers.c:1818) but driven by the emulated
   GPU's FB allocation instead of forwarded guest mmaps.

Reconciliation options to evaluate next: (a) intercept the context-buffer
GSP_RM_ALLOC, force the guest's GPU-phys assignment into a dedicated host-backed
FB window whose GPAs we memslot to the host mappings; (b) post-hoc match by
size+order. Option (a) is cleaner — we control the guest's FB-GPA at alloc time.

### M5.3 progress (2026-06-04, cont'd): primitive is reusable; integration mapped

- `nvkvm_m2_host_alloc_map_vidmem()` (commit e124deb) is the reusable on-demand
  primitive: alloc host vidmem of size N → QEMU VA. memtest regression PASS.
- **Forwarding state today:** `nvkvm_m2_shadow_fwd` forwards GSP_RM_ALLOC (fn=103)
  and FREE (fn=10) to the host. **GSP_RM_CONTROL (fn=76) is NOT forwarded** — so
  PROMOTE_CTX never reaches the host GSP, and the host context buffers are never
  mapped into the GR VAS.
- **cup2 context-buffer trace** (PROMOTE_CTX side-table, guest-FB-phys):
  client 0xc1e00009: va 0x120020000→FB 0x2ef946000 sz 0xea000 (MAIN);
  0x12010a000→FB 0x2efa41000 sz 0x4000; 0x120010000→SYS 0x149140000 sz 0x10000;
  0x120110000→FB 0x2eed80000 sz 0x80000. client 0xc1d0000a (UVM): two more FB bufs.
  These guest-FB-phys are picked by the guest's faked PMA; the host has no matching
  allocation for them.
- **Guest CPU access to vidmem** goes through the emulated BAR1 aperture
  (nvkvm_baraperture_read/write → nvkvm_walk_pdb(bar1_pdb) → fb_pages malloc'd) OR
  via the guest UVM mmap (libcuda derefs 0x200xxxxxxx, fd=nvidia-uvm). The exact
  resolution of the UVM-mapped deref (BAR1 MMIO vs guest RAM vs fault) is the next
  build's first trace target — it determines where the KVM memslot must go.

### Next build (the real M5.3, in order)

1. Trace where libcuda's UVM-mapped context-buffer deref resolves in the emulated
   GPU (add a targeted log / gdb the guest at the SIGSEGV si_addr).
2. Per FB context buffer (from PROMOTE_CTX): `nvkvm_m2_host_alloc_map_vidmem(size)`
   → host VA. Install a KVM memslot over the guest-FB-GPA range backed by that VA
   (Mode-1 GPA-window style; replaces the malloc'd fb_pages for that range).
3. Query host GPU-phys of each (NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR forwarded),
   rewrite the PROMOTE_CTX entries guest-phys→host-phys, and forward PROMOTE_CTX
   (fn=76 0x2080012b) to the host GSP so the host GPU maps the SAME memory.
4. Re-run cup2: the mapped-but-unbacked context buffers now carry real host GPU
   state → cuCtxCreate should pass the NULL-deref crash.

### M5.3 finding (2026-06-04): host-phys is privileged → pivot to VA-based backing

Empirical (commit 279c523): `NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR` returns
NV_ERR_INSUFFICIENT_PERMISSIONS (0x1b) for the unprivileged stub. Querying a
surface's GPU-physical address is a privileged op. Since prod QEMU/stub MUST stay
unprivileged (security model), we **cannot** learn host-phys → the
"rewrite PROMOTE_CTX guest-phys→host-phys" plan is dead for the unprivileged path.

**Pivot — VA-based backing (unprivileged).** `NV_ESC_RM_MAP_MEMORY_DMA` (NVOS46,
escape.c:624) is `NV_CTL_DEVICE_ONLY` but **not** privilege-gated. It maps a memory
object into a VASpace (hDma) at a caller-chosen GPU VA (`dmaOffset` with
`DMA_OFFSET_FIXED_TRUE`), returning the VA — no phys needed, unprivileged. So:

1. Per guest context buffer (PROMOTE_CTX entry: gpuVirtAddr, size): allocate host
   vidmem (`nvkvm_m2_host_alloc_map_vidmem`) and `RM_MAP_MEMORY_DMA` it into the
   forwarded host channel's VAS at dmaOffset = the guest's gpuVirtAddr (FIXED).
   The host GPU executing that channel then reaches the buffer by the same VA.
2. Back the guest's CPU/GPU view of that buffer (guest-FB-phys range, via
   nvkvm_fb_read/write or a memslot) with the same host vidmem mapping (double-mmap).
3. OPEN QUESTION (needs GR-internals research): does the GR engine context switch
   require PROMOTE_CTX with true phys, or does VAS mapping suffice? Read
   open-gpu-kernel-modules kgraphics/GR context-buffer promotion + nouveau before
   building. If phys is unavoidable, fall back to a privileged helper invoked ONLY
   at host setup (out of the per-guest unprivileged path) or host-side authoritative
   context (option A).

Recommended next: confirm the GR-context VA-vs-phys question by source, then wire
RM_MAP_MEMORY_DMA into the channel VAS for the 6 cup2 context buffers + memslot the
guest-FB ranges, and re-run cup2.

### M5.3 BREAKTHROUGH INSIGHT (2026-06-04): GR context is kernel-RM-self-promoted

Source: kernel_graphics_object.c `kgrobjPromoteContext` builds PROMOTE_CTX entries
via `kgrctxPrepareInitializeCtxBuffer` (PA, from the buffer's MEMORY_DESCRIPTOR —
`pEngCtx->pMemDesc`, `pmCtxswBuffer.pMemDesc`) + `kgrctxPreparePromoteCtxBuffer`
(VA). The GR context buffers are **allocated and managed by the kernel RM itself**
during GR-object/channel construction — NOT by guest userspace.

**Consequence (eliminates the privileged-phys problem):** when we forward the GR
object alloc (fn=103, compute class e.g. AMPERE_COMPUTE_A) to the host, the HOST
kernel RM allocates its OWN GR context buffers (host memdescs, host-phys) and issues
its OWN PROMOTE_CTX entirely in-kernel — no unprivileged client ever needs host-phys,
and we do NOT forward or rewrite the guest's PROMOTE_CTX. The guest's PROMOTE_CTX
stays a forge (as today).

**The real remaining data-plane problem (reframed):** mirror the HOST's GR
context-buffer CONTENTS into the guest's view, but ONLY for the buffers libcuda
actually CPU-touches (the crash is libcuda reading a context buffer that holds NULL
where a real pointer belongs). Kernel-only ctxsw buffers (engine ctx, PM) never need
mirroring — the host GPU uses them in-place. libcuda-mapped buffers (patch / global /
bundle CB) DO: those are the double-mmap targets, matched by bufferId.

**Verify next (cup2):** confirm a compute-class GR object alloc is forwarded and the
host RM logs its own context-buffer alloc+promote; then identify which PROMOTE_CTX
bufferIds libcuda CPU-maps (the 0x200xxxxxxx mmaps) and double-mmap only those host
buffers (RM_MAP_MEMORY_DMA into the channel VAS for GPU side + memslot/fb-redirect
for the guest CPU side). This is far smaller than backing all 6 buffers.

### M5.3 decisive test (2026-06-04): m2fwd cup2 — exact forwarding gaps found

Ran cup2 with m2fwd=on (new /tmp/m2_cup2_fwd.sh → /tmp/run_mode2_m2fwd.sh).
cuInit ok, devices=1; cuCtxCreate still SIGSEGVs (NULL-0x38). Forwarded-alloc class
status breakdown (host RM):
  OK:  0x0000 client, 0x0080 device, 0x2080 subdev, 0x2081, 0x402c, 0x90f1 VASPACE,
       0xa06c channelgroup (5 ok/1 err), 0xc56f channel (10 ok/4 err),
       0xc7b5 AMPERE_DMA_COPY_B (8 ok/4 err)
  ERR: 0xc7c0 AMPERE_COMPUTE_B  status=0x57 NV_ERR_OBJECT_NOT_FOUND (parent chan 0x5c000019)
       0xc797 AMPERE_B graphics  status=0x57 OBJECT_NOT_FOUND
       0xc076 (profiler/perf)   status=0x1b INSUFFICIENT_PERMISSIONS ×7 (privileged → likely optional)
       0x007e ×3, 0x902d, 0x9067 ERR

**Root:** the compute GR object (0xc7c0) — the one whose construction triggers the
host kernel's self-promotion — fails OBJECT_NOT_FOUND because its parent channel
chain has gaps (some 0xc56f channels / 0xa06c channelgroups fail). Child objects
(compute, DMA-copy, graphics) cascade. So the host never builds the GR context →
never self-promotes → guest reads unpopulated buffers → cuCtxCreate NULL-deref.

**Next (object-tree chain, in dependency order):** debug why specific 0xa06c
channelgroup + 0xc56f channel allocs ERR on the host (likely a missing parent
handle remap, the channelgroup's VASpace handle, or USERD/instmem the M5.3a forge
zeroed). Once channels construct cleanly, 0xc7c0 should find its parent and
construct → host self-promotes. The 0xc076 privileged profiler objects are a
separate concern (probably skip/forge — not needed for compute). Then revisit the
content-mirroring (double-mmap by bufferId) for libcuda-CPU-touched buffers.

### M5.3 ROOT CAUSE pinned (2026-06-04): channelgroup embedded-handle not translated

Full failure cascade for cuCtxCreate's compute context (the 0x5c0000xx RM-internal
chain under UVM client 0xc1d00003 — note libcuda's OWN 0xcaf00xxx channel chains all
forward OK):
  [43] device   0x5c000002 (parent client 0xc1d00003) -> OK
  [44] subdev   0x5c000003, [45] 0x2081, [46/47] VASPACE 0x5c000007/8 -> OK
  [90] a06c channelgroup (parent device 0x5c000002, obj 0x5c000012) -> st=0x33
       NV_ERR_INVALID_OBJECT_HANDLE  <-- THE ROOT
  [92] c56f channel (parent group 0x5c000012) -> 0x57 OBJECT_NOT_FOUND (group missing)
  [93] c7c0 AMPERE_COMPUTE_B (parent chan 0x5c000019) -> 0x57 (chan missing)

INVALID_OBJECT_HANDLE on the channelgroup = an embedded object handle in its
NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS (e.g. hVASpace) does not resolve on the host.
`nvkvm_m2_shadow_fwd` remaps only h_root + h_object_parent — it copies alloc params
verbatim (auxbuf), so any object handle EMBEDDED in the params reaches the host as a
guest handle. For the 0xcaf00xxx libcuda chains it happens to work (their embedded
refs are within the same forwarded set/verbatim), but the 0x5c0000xx (UVM
RM-internal) channelgroup's embedded handle is wrong on the host. Matches the known
residual "channelgroup VASpace handle INVALID_OBJECT_HANDLE".

**NEXT (precise):** dump the 0xa06c alloc params (add a hex dump in shadow_fwd for
hClass==0xa06c) to find which embedded handle field is the bad one; then translate
embedded handles per-class in shadow_fwd (channelgroup hVASpace; channel hVASpace/
hContextShare/hObjectError; compute-object none). Once the channelgroup constructs,
the channel + compute object should follow, the host builds + self-promotes the GR
context, and we move to mirroring the libcuda-CPU-touched buffers.

### M5.3 ROOT CAUSE CONFIRMED (2026-06-04): GR channelgroup passes hVASpace=0

a06c param dump (NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS: hObjectError@0,
hObjectEccError@4, hVASpace@8, engineType@12):
  OK   (libcuda client 0xcaf00000): hVASpace=0xcaf00005 (explicit), engineType=0x9..0xd
       (= NV2080_ENGINE_TYPE_COPY0..4) — COPY-engine TSGs, all construct.
  FAIL (UVM client, device 0x5c000002): hVASpace=0x00000000, engineType=0x1
       (= NV2080_ENGINE_TYPE_GRAPHICS) -> st=0x33 INVALID_OBJECT_HANDLE.

So the GR-engine TSG — the parent the compute object (0xc7c0) needs — relies on the
device-DEFAULT VASpace (hVASpace=0), which doesn't resolve on the host device as
forwarded; the COPY TSGs pass an explicit handle and succeed. The guest had allocated
FERMI_VASPACE_A 0x5c000007 + 0x5c000008 under that same device (SHADOW[46][47], both
OK on host).

**FIX TO TRY (next tick):** in shadow_fwd, for hClass==0xa06c with hVASpace(@8)==0,
substitute an explicit VASpace handle forwarded under the same device. Candidate:
the GR VASpace among 0x5c000007/0x5c000008. Determine which by checking the VASPACE
alloc params (FERMI_VASPACE_A index/flags) or just try each. If neither works, the
host device may need its default VASpace established (forward a SET_DEFAULT or alloc
a device-global VASpace). Once the GR TSG constructs, channel 0x5c000019 + compute
0x5c00001a should follow → host builds + self-promotes the GR context.

### M5.3 chain progress (2026-06-04): TSG fixed, channel advanced, ctxshare wall

Iterative forward-chain repair of the GR context (0x5c0000xx, UVM client 0xc1d00003):
  0xa06c TSG       0x5c000012:  0x33 -> 0x0   FIXED (substitute hVASpace 0 -> 0x5c000007)
  0x9067 ctxshare  0x5c000013:  0x40 INVALID_STATE  <-- CURRENT WALL
  0xc56f channel   0x5c000019:  0x57 -> 0x1f  (downstream of ctxshare; references it)
  0xc7c0 compute   0x5c00001a:  0x57          (downstream of channel)

The FERMI_CONTEXT_SHARE_A (0x9067, psize=12: hVASpace@0,flags@4,subctxId@8) already
carries a NON-zero hVASpace (our hVASpace=0 substitution did NOT fire), so 0x40
NV_ERR_INVALID_STATE is NOT a handle problem — it's a stateful GR-subcontext failure.
ctxshareConstruct allocates a subcontext within the TSG's GR engine context; on the
host that state isn't established because we forward the guest's RM-internal GR
objects out of their original kernel construction sequence (golden ctx / subcontext
pool / GR engine state are GSP/RM-internal and not replicated by forwarding individual
allocs).

**Architectural question for next session:** forwarding the guest's RM-INTERNAL GR
objects (TSG/ctxshare/channel/compute under 0x5c0000xx) is fighting their stateful
interdependencies. Two directions:
  (A) Make the host device establish a proper default VASpace + GR engine state so
      hVASpace=0 resolves naturally and ctxshare subcontext alloc finds valid state
      (investigate the NV0080 device alloc params + whether a default-VAS / GR
      bootstrap control must be forwarded first).
  (B) DON'T forward the guest's RM-internal GR objects; instead let the HOST RM build
      its own GR context when libcuda's compute object is allocated against a
      host-constructed channel — i.e. forward only libcuda's userspace-visible
      objects and let host kgrctx self-manage (ties to the kernel-self-promotion
      insight). Needs the compute object reparented to a host-built channel.
Dump the 0x9067 params (hVASpace/flags/subctxId values) + the subdevice GR-init
control sequence to decide. INVALID_STATE is a state-ordering problem, not a handle.

### M5.3 ROOT CAUSE — DEFINITIVE (2026-06-04): GR context = UVM externally-owned VASpace

VASpace alloc-param comparison (NV_VASPACE_ALLOCATION_PARAMETERS flags@4):
  libcuda VAS 0xcaf00005 (WORKS):     flags=0x4  = SHARED_MANAGEMENT (normal RM-managed)
  UVM GR VAS 0x5c000007 (SHELL/fail): flags=0x48 = IS_EXTERNALLY_OWNED(BIT3) |
                                                    ENABLE_PAGE_FAULTING(BIT6)

The cuCtxCreate compute context's primary VASpace is a UVM-externally-owned,
fault-enabled VASpace: its page tables are managed by nvidia-uvm.ko, NOT RM. Forwarded
verbatim, the host RM creates the VASpace resource but with an empty/NULL OBJVASPACE
(pVAS) because the external owner — the GUEST UVM — never registers or manages the
HOST RM's VASpace. Hence kernel_ctxshare.c:133 NV_ERR_INVALID_STATE (pVAS==NULL), and
the channel/compute cascade. Handle substitution can't fix this; it's structural.

**This is the UVM-externally-owned-VASpace problem** — the documented hard part of
Mode-2, tied to [[mode2-uvm-residency]]. The path to Mode-2 cuCtxCreate REQUIRES making
the host RM's externally-owned VASpace actually managed: forward the guest's UVM
VASpace-registration operations (UvmRegisterGpuVaSpace / the UVM ioctls that populate
GPU page tables) to a HOST UVM bound to the host's externally-owned VASpace + the host
channel, so the host VASpace gets real page tables. This is exactly the Mode-1 UVM
forwarding ([[uvm-in-qemu]]) but for a Mode-2-guest-driven, host-owned VASpace, and it
intersects the UVM residency design. NOT a quick handle fix — it's the UVM milestone.

So: the GR forward-chain repair (TSG/channel hVASpace) was real and correct, but the
chain ultimately dead-ends at the UVM externally-owned VASpace, which must be solved at
the UVM layer. Next session: scope the UVM VASpace-registration forwarding (which UVM
ioctls the guest issues for 0x5c000007; bind a host UVM to the host VASpace+channel).

### M5.3 KEYSTONE (2026-06-04): Mode-2 compute requires reverse-driver page-table xlate

Complete causal chain for Mode-2 cuCtxCreate, now definitively established:
  cuCtxCreate (Pascal+) builds a UVM-managed primary context
   → its compute object (0xc7c0) lives on a channel whose primary VASpace is
     UVM-externally-owned + fault-enabled (flags=0x48)
   → an externally-owned VASpace's GPU page tables are managed by nvidia-uvm.ko
     (UVM_REGISTER_GPU_VASPACE ioctls + the in-kernel nvUvmInterface), NOT by RM
   → in Mode-2 those UVM ops run ENTIRELY IN-GUEST (guest /dev/nvidia-uvm + guest
     faked RM); they never reach the emulated GPU as forwardable ioctls
   → so the host RM's forwarded externally-owned VASpace is an unmanaged shell
     (NULL OBJVASPACE) → ctxshareConstruct pVAS==NULL → INVALID_STATE → cuCtxCreate fails.

There is NO forwardable-ioctl shortcut: the guest UVM↔RM binding is in-guest and
invisible to the emulated device. The host VASpace can only be made functional by
RECONSTRUCTING its GPU page tables from the guest's observable GPU-side activity:
  - PROMOTE_CTX (already snooped → #2 side-table) gives context-buffer VA↔phys.
  - The guest's GMMU page-table writes are observable via the emulated BAR1 aperture
    (nvkvm_baraperture_write → walk → FB), and via the GPA-window the guest CPU uses
    to build page tables.
  → Translate those guest GPU-VA→guest-phys mappings into host VASpace page-table
    entries pointing at the host backing (the data-plane primitive, now proven), via
    a host UVM bound to the host externally-owned VASpace OR direct RM page-table fills.

This is the **reverse-driver page-table translation** — the documented hard core of
Mode-2 ([[device-sim-verdict]]: "Hardest = guest-PTE↔host DMA xlate"; [[mode2-isolation-cr3-key]]).
It is THE Mode-2 compute keystone milestone, multi-week, and intersects the UVM
residency design ([[mode2-uvm-residency]]) + the prior Mode-1 UVM saga
([[uvm-in-qemu]], [[state-machine-step-e]], [[cuctxcreate-800-pinned]] which hit
RS_ACCESS_DUP_OBJECT / PAGE_TABLE_NOT_AVAIL on exactly this binding).

What this session SETTLED: the data-plane primitive (QEMU↔host-GPU memory) is proven;
the cuCtxCreate wall is fully root-caused to the UVM externally-owned VASpace; the GR
forward-chain handle repairs (TSG/channel hVASpace) are correct and committed; and the
remaining work is precisely scoped to the page-table-translation keystone. Approach
candidates for the keystone (next major milestone, likely worth user steer given scale):
  (1) Host-UVM mirror: stub opens /dev/nvidia-uvm, registers the host externally-owned
      VASpace, and replays guest GPU-VA mappings into it (observed via BAR1/PROMOTE_CTX).
  (2) Direct host page-table fill: QEMU/stub writes the host VASpace's PTEs directly
      from the observed guest mappings (no host UVM), pointing at host backing.
  (1) reuses real UVM machinery (safer, matches residency design); (2) is lower-level.

### M5.3 channel 0x1f diagnosis (2026-06-04): not memdescs; engineType/ctxshare differ

After the EXTERNALLY_OWNED strip (VASpace/TSG/ctxshare all construct), the GR channel
(0xc56f 0x5c000019) is at 0x1f INVALID_ARGUMENT. Compared working libcuda channels vs it:
  memdescs (inst/userd/ramfc/mthd .base) are guest-FB offsets (addressSpace=2/FBMEM) in
  BOTH — libcuda channels construct fine with them, so guest-FB bases are NOT the blocker
  (host RM accepts/ignores them at alloc). Real differences on the UVM channel:
    - engineType@128 = 0x0 (libcuda COPY channels: 0xb/0xc/0xd). The GR channel's TSG is
      engineType=GRAPHICS(1); the channel passes 0 (NULL) — may need to match the TSG.
    - hContextShare@24 = 0x5c000013 explicit (libcuda channels: 0, RM makes legacy default).
    - gpFifoOff@8 = 0x200200000 (a UVM-managed GPU VA, unmapped in the fresh host VASpace).
  Next: try engineType 0->GRAPHICS(1) (track TSG engineType by handle); if not, the
  explicit-ctxshare channel path likely needs the gpFifoOffset VA mapped (RM_MAP_MEMORY_DMA
  into the host VASpace) or specific channel flags. Then compute object should construct.

NOTE the big win this tick: stripping IS_EXTERNALLY_OWNED makes the host build an
RM-managed GR context (VASpace+TSG+ctxshare all OK), SIDESTEPPING the multi-week UVM
keystone for host-side context construction. Remaining for cuCtxCreate-on-host: channel
0x1f -> compute object, then back the guest's CPU-touched context buffers via the data
plane. The guest keeps its UVM view; host uses RM-managed; reconcile at buffer level.

### M5.3 LANDMARK (2026-06-04, commit a1f3edb): full GR compute context constructs on host

The channel 0x1f was MY OWN c56f hVASpace substitution (host dmesg: "TSG channels
can't use an explicit vaspace" + "Both context share and vaspace handles can't be
valid"). REMOVED it (TSG channels inherit the TSG vaspace; never set explicit hVASpace).
Result: the full UVM RM-internal GR compute chain constructs st=0x0 on the real GA106
via Mode-2 forwarding:
  VASpace 0x90f1 -> TSG 0xa06c -> ctxshare 0x9067 -> channel 0xc56f -> AMPERE_COMPUTE_B 0xc7c0
No regression (libcuda COPY channels 0xc56f back to 11 OK, 0xc7b5 8 OK). So with
(a) the EXTERNALLY_OWNED strip and (b) leaving TSG channels' vaspace implicit, the host
RM builds a COMPLETE real shadow GR compute context — the kernel-self-promotion trigger —
WITHOUT the UVM keystone. This validates the SIDESTEP: the host doesn't need the guest's
UVM to manage the GR VASpace; it builds an RM-managed shadow context; reconcile at the
buffer-content level.

### M5.3 NEXT: data-plane backing — reframe of the cuCtxCreate crash

REFRAME: the cuCtxCreate guest crash (NULL deref) is the GUEST reading back its own GR
context buffers that a REAL GPU's GSP/RM would have populated during GR-context init.
Our emulated GPU never runs GR init, so those buffers are zero -> guest derefs NULL.
This is NOT (yet) about forwarding compute *execution*; it's about populating the guest's
context-buffer view with real state. The host shadow context (just constructed) HAS real
context buffers. So the milestone = mirror the host's context-buffer state into the guest's
view (FB pages / sysmem the guest CPU reads), via the proven host-alloc-map primitive.

Concrete sub-steps (this session):
  (a) PINPOINT the exact guest read that returns NULL and crashes cuCtxCreate (faulting VA
      + libcuda backtrace via gdb). Determine if that VA is an FB-backed context buffer
      (observable/interceptable in QEMU via BAR1/PRAMIN) or guest sysmem.
  (b) Read the host self-promoted context-buffer addresses/contents
      (NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO 0x20801219 / GET_CTX_BUFFER_SIZE 0x20801218).
  (c) Mirror host buffer contents into the guest's view so the guest reads real state.

### M5.3 ROOT CAUSE PROVEN (2026-06-04): host-vs-guest byte-compare → data-plane backing

Method: ran the SAME cup2 on the bare-metal host (PASSES: CTX OK, CE PASS) and the guest
(crashes), both under an LD_PRELOAD ioctl tracer (tests/mode2/ioctl_trace.c) decoding
RM_CONTROL/RM_ALLOC in+out. Findings:

1. Class-alloc sequence is IDENTICAL up to the first compute object:
     0x90f1 0x90f1 0x50a0 0x0040x4 0xa06c 0x9067 0x0040 0x003e 0x003e 0xc56f 0xc7c0
   HOST then continues: 0xc7b5 0x003e 0xc56f 0xc7c0 0xc7b5 ... (8 compute+copy channel
   groups) and finishes cuCtxCreate. GUEST crashes right after the FIRST 0xc7c0.
2. The 0xc7c0 RM_ALLOC NVOS64 writeback is BYTE-IDENTICAL host vs guest except the client
   id (0xc1d00277 vs 0xc1d00003) and a stack pointer: status=0 (NV_OK), paramsSize=0,
   tail all zeros. So the ioctl writeback is NOT the bug.
3. NO ioctl occurs between the 0xc7c0 alloc and the SIGSEGV (verified by the tracer) —
   libcuda crashes processing IN-MEMORY state.
4. gdb: crash is a STACK SMASH — in 0x4664c0 -> 0x47acc0 -> 0x497b50 (the chain that
   issues the 0xc7c0 RM_ALLOC), the saved rbp of 0x47acc0's frame is overwritten, so
   on return rbp=0 and the next deref (mov -0x38(%rbp)) faults at 0x466560. The HAL
   dispatch target (*(global+0x48))->[0x560] = 0x47acc0 is a VALID, well-behaved function.
   Full bt: cuCtxCreate_v2 -> 0x2578390 -> 0x25931c5 -> 0x246f533/164/cd32 ->
   0x24299f4 -> 0x2426fbd -> 0x266d49f -> 0x4664c0 -> 0x47acc0 -> 0x497b50.

CONCLUSION (proven): cuCtxCreate-on-Mode-2 crashes because libcuda reads the channel/GR
compute context's MAPPED GPU memory (instance block / USERD / GR "golden" context buffer
— BAR-backed in our emulated FB at e.g. inst.base=0x3330000, userd.base=0x4202000) which
on a real GPU is initialized by RM/GSP during channel+GR-object construction, but on our
faked GPU contains zeros/inconsistent data. libcuda derives a bad size/count from it and
smashes its stack. The host shadow context (real GA106, forwarded) HAS this real state.

=> THE FIX IS THE DATA-PLANE BACKING: mirror the host shadow context's real context-buffer
contents into the guest's BAR-backed context buffers (the proven nvkvm_m2_host_alloc_map_
vidmem primitive + double-mmap), so the guest reads real GPU-initialized state. This is the
multi-week keystone (task #126 "hard part"). NEXT IMPLEMENTATION STEP: enumerate the host
shadow context's buffers via NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO (0x20801219) on the
host subdevice (struct: hUserClient@0,hChannel@4,bufferCount@8,ctxBufferInfo[]@16 each 80B:
alignment@0,size@8,physAddr@32,bufferType@40,aperture@44), map each, and back the guest FB
ranges the channel/GR object reference. Tooling: tests/mode2/{gcup2_gdb,gcup2_hal,gcup2_trace,
ioctl_trace}.* + host-side /tmp/cup2_host (bare-metal reference run).

### M5.3 DATA-PLANE constraint (2026-06-04): GR_GET_CTX_BUFFER_INFO is PRIVILEGED

Implemented step 1 of the data-plane backing: after the compute object (0xc7c0) constructs
on the host shadow context, issue NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO (0x20801219) on
the host shadow subdevice (tracked via m2_subdev[], keyed by GR client) to enumerate the
real host GR context buffers. RESULT: crc=0 but st=0x1b (NV_ERR_INSUFFICIENT_PERMISSIONS),
bufferCount=0. The control is PRIVILEGED — the unprivileged stub cannot read it (same wall
as GET_SURFACE_PHYS_ATTR 0x410103).

IMPLICATION (intersects the hard security constraint "QEMU stays unprivileged in prod"):
the "read host GR context buffers and mirror their contents into the guest" data-plane
approach is BLOCKED on the unprivileged path. The host GR context buffers are kernel-RM-
managed (no userspace handles, privileged enumeration), so unprivileged QEMU cannot read
them directly. The data-plane keystone must therefore be solved one of:
  (A) FORGE the guest-side GSP state so the guest kernel populates libcuda's context
      buffers self-consistently (unprivileged; canonical fake-the-boot). Needs the correct
      values — find via host-vs-guest CONTROL-RESPONSE byte-compare (the tracer captures
      both in+out, unprivileged), since libcuda crashes on a value it stored from an
      earlier control/mapped read.
  (B) An unprivileged host-content path (TBD) — e.g. map host buffers via handles libcuda
      itself creates, not RM-internal GR buffers.
  (C) Privileged helper (rejected by the security constraint except for debugging).
NEXT: host-vs-guest control-response diff to find the divergent forgeable value (approach A).
