# Mode-2 address virtualization — the reverse-driver core

Status: design, 2026-06-03 (from user brainstorm). This is THE core of the
reverse driver: converting the guest's GPU address maps back to host-userspace
GPU VAs / host memory. It is the layer the whole Mode-2 thesis rests on, and the
current `UVM_REGISTER_GPU` blocker ([[mode2-cuinit-sm-order-fix]]) is the first
place it's strictly required.

## Core principle: GPU-physical is OUR bookkeeping

The "GPU physical address space" the guest kernel driver sees — and the BAR
addresses of our emulated GPU — exist **only between the guest kernel module and
the QEMU device extension**. Neither the host kernel, the host GPU, nor guest
userspace ever observes it. So we define its semantics entirely; it is pure
bookkeeping. We are free to lazily allocate it, categorize it, and map each
page wherever is correct.

## The two translation chains we follow

Both start at a **guest GPU virtual address** and end at a **VMM (QEMU) VA**;
both pass through the same intermediate bookkeeping layers
(GPU-VA → GPU-phys → guest-phys/GPA). They differ only in what the final GPA
denotes:

1. **MMIO / PCIe-BAR maps** (e.g. USERD, doorbell, register windows):
   GPU-VA → (walk the guest's virtualized PDB) → GPU-phys → GPA **inside our
   emulated PCIe BAR window** → the matching BAR sub-range(s) → `mmap`/install.
   Multiple BAR ranges may match one VA span (if the physical pages are split).

2. **GPU DMA into CPU memory** (pushbuffers, GPFIFO rings, semaphores, UVM
   managed buffers, HtoD/DtoH staging):
   GPU-VA → (walk PDB) → GPU-phys → GPA **in an ordinary KVM memory slot**
   (usually guest RAM). Iterate the KVM memslots to map GPA → VMM VA, then
   read/write or OS-descriptor it into the host context's VAS.

The walk is a **range translation**: a set of contiguous GPU-VA pages goes in;
follow the PDB/PTEs; a set of (possibly more, if fragmented) physical page
ranges comes out. Batch adjacent pages — it is fast and the common case is large
contiguous spans.

**Output of a translation** (per the user): a tuple
`(host context/channel, stub mmap-fd with its GPU-VA, host VMM VA)`. We then run
the unprivileged nvidia alloc/`mmap` for the stub/isolate/host-context GPU-VA →
the GPA, i.e. install the real backing under the guest's chosen address.

## GPU-physical page categories

Every emulated GPU-phys page is in exactly one state:

1. **Special** — kernel doorbells / control regs. A small fixed set. Fully
   trapped, or read-only with write-traps, or read-write-but-tracked. (See the
   doorbell model in [[mode2-plan]]: kernel doorbells trap; userspace work-submit
   doorbells direct-map after channel-create.)
2. **Read-only constant** — VBIOS, chip-ID data, placeholder GSP firmware. Served
   from the captured blobs.
3. **Unallocated** — no context owns it, not allocated. R/W trapped; first touch
   promotes it. (Default state of the lazily-allocated FB.)
4. **Zero-unallocated** (optional) — like unallocated but reads as zeros, write
   traps. Lets debug tools that dump the whole FB read fast zeros without
   per-page allocation (map a single preallocated 16–128 MiB zero region,
   repeated over the untouched GPA range). Possibly overkill.
5. **Unassigned** — a real RM-allocated buffer with data written, but no GPU
   context uses it yet. Rare; correctness-only. Small ones live in host memory.
6. **Assigned** — a real `mmap` of the host GPU at a specific host-userspace
   context/isolate, installed at the guest PCIe GPA. The hot, real path
   (= the double-mmap + GPA-window forwarding, [[mode2-matmul-requirements]] §C/§D).
7. **Inaccessible** — outside the FB region; illegal on real HW → fault.

### The "unassigned" simplification (clear-on-assign)

Unassigned is the tricky state: if a real GPU context later maps that GPU-phys,
we must have preserved the bytes. Two options:
- Back unassigned by **anonymous RM allocations** that can be *transferred* into
  a real context on assign (move the GPU object).
- **OR** lean on the fact that the **NVIDIA driver always clears memory when
  assigning it to a new context** (never hands a context stale garbage). If that
  holds, we can simply **ignore writes to unallocated** memory: nothing the guest
  writes to not-yet-assigned GPU-phys ever needs to survive into a context,
  because the driver will wipe it on assign. This also cleanly answers *when can
  we free* stub-side RM objects: assigned→unassigned always wipes on re-assign,
  so it can never carry data back; it becomes unallocated and is freed on the
  host. **Preferred** (assume clear-on-assign) unless a host driver is observed
  doing the weird stale-transfer thing.

## Lazy allocation & capacity

We lazily allocate GPU-phys backing on first touch (the FB is sparse). Because of
that we can **advertise the full VRAM size even though the host also uses some
VRAM**; if a real backing allocation later fails, report it as a PDB-mapping
failure (or the HW-faithful out-of-memory the guest would see). Watch for this
when sizing FB vs. host availability.

## How this resolves the current blocker

`UVM_REGISTER_GPU` allocates a UVM GPFIFO channel; its GPFIFO ring lives at
GPU-VA `0x121010000`. cuInit hangs because `chan_exec` reads that ring as 0:
- The ring is chain #2 (GPU DMA into CPU memory): GPU-VA → PDB walk → GPU-phys →
  GPA (guest RAM) where the guest wrote the entry.
- We fail at the PDB walk: we don't have the UVM channel's VAS root PDB (it's
  not among the `0x90f10106` snoops, and the instance block is GSP-managed/empty),
  so we can't follow GPU-VA → GPU-phys, and the content-pick heuristic finds no
  snooped VAS that maps it.

So the immediate need is **capture every VAS's root PDB** (especially the
device-default VAS used by `hVASpace=0` channels), then walk → GPU-phys → GPA via
the KVM memslots (chain #2). The PDB is bookkeeping the guest kernel module
established and communicates to us; we must snoop *all* the places it does so
(not just `VASPACE_COPY_SERVER_RESERVED_PDES`), or have the guest module report
it explicitly. Once the walk resolves, the GPFIFO/pushbuffer/semaphore read
correctly and the init push completes (emulate-completion bucket); real compute
channels then use category-6 "assigned" forwarding.

## Capture strategy: #2 primary (map-call side-table), #1 only for correctness

Refined 2026-06-03 (user). Two ways to obtain the guest's GPU-VA → physical
mappings:

- **#2 — reconstruct from the map/alloc calls (PRIMARY).** As the guest issues
  the RM ops that *establish* a mapping (channel alloc's gpFifoOffset, MAP_MEMORY
  / MAP_MEMORY_DMA, UVM maps), record `GPU-VA span → physical (GPU-phys/GPA) →
  owning context/channel` in a side-table. `chan_translate` (and the forwarder)
  consult this table directly — **no page-table walk / VAS-root needed.** This is
  better because it inherently ties the virtualized GPU-physical to a context
  (the unit of forwarding/isolation). **This is also the fix for the current
  UVM-channel blocker**: capture the op that placed the GPFIFO at GPU-VA
  0x121010000 → its physical, instead of trying to walk the device-default VAS
  we can't root.
- **#1 — intercept PTE writes (CORRECTNESS-ONLY, possibly skippable).** Needed
  only for the "unassigned" case: GPU-phys written with data *before* any context
  maps it (anonymous RM/UVM data later transferred into a context). **If the
  guest never writes a GPU-phys range before it is mapped to a context (always
  alloc→map→write, never write→map), #1 is unnecessary** and we rely on
  clear-on-assign. Worth verifying empirically; early evidence supports skipping:
  `DMA_FILL_PTE_MEM` is never used in our run, and CPU-RM PTE writes go via
  PRAMIN (already visible). Action: instrument for a write to a GPU-phys range
  that precedes its first map-to-context; if it never fires, drop #1.

## Implementation order (proposed)

1. **Map-call side-table (#2)** — snoop the RM ops that establish GPU-VA →
   physical mappings (channel alloc gpFifoOffset, MAP_MEMORY/MAP_MEMORY_DMA, UVM
   maps), record `GPU-VA span → physical → context` in a table; `chan_translate`
   consults it (no VAS-root/page-table walk). Unblocks the UVM channel: capture
   what placed the GPFIFO at 0x121010000. Also instrument the write-before-map
   check to confirm #1 is skippable.
2. **Range translator** — keep the PDB-walk fallback for VASes we *can* root
   (already works for snooped channels), but the side-table is authoritative;
   batch GPU-VA spans → physical ranges, FB/sysmem apertures.
3. **Category state machine** — track GPU-phys pages (1–7); default unallocated,
   clear-on-assign simplification; lazy FB.
4. **Chain #1 (BAR/MMIO install)** and **chain #2 (GPA via KVM slots)** wired to
   the stub: assigned pages become real host-context mmaps installed at the
   guest GPA (the double-mmap + GPA window).

Step 1 unblocks cuInit/UVM (and is the primary capture); 3–4 are the parity
compute path. PTE-interception (#1) only if the write-before-map check fires.
