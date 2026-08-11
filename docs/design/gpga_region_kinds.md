# GPGA region kinds — the core design

> ### STATUS — 2026-08-11 / **LIVE — OWNER DESIGN, not yet measured**
> **Owner's model, stated in conversation 2026-08-11 and written up here verbatim in substance.**
> ⊘ **This is a DESIGN STATEMENT, not a measurement.** Nothing may be retired on its authority
> until a boot says so. What *is* measured is recorded in §3 and §4 below, separately marked.

---

## 1. The model

**A GPGA region is exactly ONE of four kinds, and the kind is DECIDED WHEN THE REGION IS
ALLOCATED — not derived later from whatever happens to be present.**

| kind | backed by | created when |
|---|---|---|
| **unallocated** | nothing | the default for untouched pages; a miss is a fault |
| **fake framebuffer** | our emulated store | ★ **ONLY when the guest KERNEL builds an internal channel we emulate** — it is where *we* manage that channel's pushbuffer / USERD / ring / semaphore |
| **real GPU memory** | a real allocation (`NV_ESC_RM_MAP_MEMORY` / `NV_ESC_RM_ALLOC_MEMORY`) | ★★★ **a guest USERSPACE mapping request against an unallocated region** |
| **DMA to guest physical** | `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` | guest RAM the GPU must be able to reach |

### ★ The awkward case, and its clean answer

If fake framebuffer must ever appear at a **real GPU virtual address** — the scratchpad, for
instance — **route it through `OS_DESCRIPTOR`.** Then all three parties are simultaneously correct:

- **we** know the bytes are fabricated,
- **the guest** believes it is device memory,
- **the real GPU** believes it is host RAM it was told to DMA into.

⇒ **Nobody is lied to in a way that matters**, and no party has to be told a fact it can disprove.

---

## 2. What unprivileged guest userspace may see

**Exactly four surfaces, and the fake framebuffer is not among them:**

1. `NV_ESC_RM_MAP_MEMORY` / `NV_ESC_RM_ALLOC_MEMORY`
2. `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR`
3. the **UVM** ioctls that unify CPU VA and GPU VA — including the simulated aperture, under which
   **the guest always believes the GPU is DMAing to host memory**
4. the **one doorbell page** every userspace process maps and writes its token into — **read native,
   write trapped** so the token can be translated

★★ Every one of these is either **host memory that already has a CPU mapping** or **a single page we
trap**. ⇒ None needs a mechanism NVIDIA refuses on this hardware. **The fiction stays entirely below
the userspace boundary.**

⚠ **A guest kernel would almost never publish our fake framebuffer to an unprivileged process** — it
is internal privileged RAM and channels. If we ever observe it doing so, that observation is the
finding.

---

## 3. ✔ MEASURED — why this is not merely tidier

**The taxonomy already exists in the code, but it is DERIVED and it grew one distinction at a time,
each added when a specific confusion bit.**

- `Representability` (`kayfabe-fwd`) is **computed from what happens to be present** at an address —
  `HostBacked` / `Fabricated` / `Untracked`. ⇒ **`Fabricated` is simply what you get when nothing
  decided a kind.**
- Its own doc for the host-backed case reads: *"⚠ **AND the object must be the range's ONLY
  memory**… A host object that merely *exists* at the address is not enough: `PublishVidmem` puts one
  at a VA whose bytes the guest goes on reading and writing through the emulated framebuffer."*
- `BackingBytes::{SoleBacking, ShadowsGuestMemory}` (`kayfabe-mmu`) was added **2026-08-11** as a
  **runtime check** for exactly that state.

⇒ ★★★ **In this model that state is unrepresentable.** A region is one kind. There is no *"real
object at an address the guest also reaches through the fiction"*, because a region that is real is
not fake. **The check becomes unnecessary rather than merely passing.**

★ This is the same distinction the tree discovered empirically, stated **once**, **declaratively**,
at **allocation time**, instead of re-derived at every use.

---

## 4. ✔ MEASURED — what it predicts about the standing wall

The eight walling doorbells are **guest userspace** (`proc 2`). Their **queue is backed by the fake
framebuffer** (`V:0x1024000`) while their command buffers are in host memory (`S:`).

⊘ **Two readings were proposed and BOTH were wrong** — the orchestrator's `(a) we advertise it that
way` and `(b) the driver always does this`. **The answer is neither:**

> ★★★ **Nothing decided that ring should be fabricated. `Fabricated` is the default for any touched
> GPGA page.**

⇒ Under this model, **a userspace mapping request against an unallocated region becomes real GPU
memory**, and there is no fiction on that path at all. ⚠ **`[NOT MEASURED]` — this predicts the fix;
it does not demonstrate it.** The deciding experiment is what the code does today when guest
userspace asks to map an unallocated GPGA region.

---

## 5. ⇒ What this changes

- **Region kind becomes declared state**, consulted rather than inferred. Every site that currently
  calls `representability_of` instead reads the kind.
- **`BackingBytes` becomes redundant** — kept only as a transitional assertion.
- **The scratchpad stops needing a special mechanism**; it is fake framebuffer routed through
  `OS_DESCRIPTOR`.
- ⊘ **The emulated framebuffer stops being a shared surface** and becomes what it was always meant
  to be: **private storage for channels we emulate on the guest kernel's behalf.**

Related: `mode2_gpga_memfd.md` (the owner's sparse-memfd sketch — ⚠ its motivation is narrowed by
this doc: sharing the fiction matters only for the kernel-channel case),
`property_2_the_subtraction.md`, `is_passthrough_the_only_correct_route.md`.
