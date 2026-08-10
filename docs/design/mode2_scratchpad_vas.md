# The scratchpad address space — where OUR work lives

**Status:** owner design, 2026-08-10, recorded because it existed only in conversation.
Companion to `mode2_channel_ownership_split.md` (who owns which channel) and
`s1_what_does_it_protect.md` (why the current placement is wrong).

---

## 1. The rule, in one line

> **Guest userspace → passthrough, in the guest's own address space, opaque to us.
> Guest kernel → emulated, in OUR scratchpad.**

★ This is the channel-ownership split expressed in **addressing** terms rather than policy terms,
which is the more durable form: it is checkable by looking at where an object lives.

---

## 2. The scratchpad

- **One scratchpad address space**, belonging to the **system proc** (the guest kernel's client).
  ⊘ It is **not** per-guest-process, and that costs nothing: its customers are **kernel-initiated
  operations** — scrubs, copies, the driver's own housekeeping — and **there is only one guest
  kernel**. Guest userspace never needs it, because userspace is passthrough.
  ⇒ **Per-process isolation is untouched by this.**
- **Map all of GPGA at a fixed offset**, sparse — holes where nothing is backed.
  ⇒ ★★★ **scratchpad VA = offset + GPGA.** The address is a **pure function** of the thing
  addressed, so there is **no allocator and nothing to track**. This is
  `mode2_address_table_of_truth`'s forward-population rule applied to our own memory.
  ★ And laziness buys little: a scrubber reaches most of GPGA anyway, so lazy mapping would add a
  tracking set for no gain — and mapping everything up front removes the idempotence hazard, since
  there is nothing to re-map. (⚠ Re-mapping an occupied address returns `NV_ERR_NO_MEMORY`, which
  cannot be distinguished from genuine exhaustion — see `s1_what_does_it_protect.md`.)
- **An object may live in several address spaces at once.** The same page can be at the guest's VA
  in the guest's space *and* at `offset + GPGA` in ours. Routine, and unobservable to the guest.
- The scratchpad holds **our** ring, pushbuffer, USERD and completion semaphores — room for several.
  ⊘ **None of it is guest-visible, so its shape is ours to choose.** We are not obliged to give our
  own machinery the layout a guest would use.

**A kernel-initiated operation is then:** map (no-op if already), do the work, return.

---

## 3. Completion — the waiter that does not exist yet

An emulated operation that needs real GPU work puts it in the scratchpad and registers its
semaphore with **one waiter over many semaphores** — an epoll-shaped loop, not one blocking wait per
operation. Spin briefly; if it takes too long, arm an OS notification for the outstanding semaphores
so the loop can sleep and resume. On completion, complete the **emulated** semaphore in the guest's
managed buffers and raise the interrupt if one was requested.

★★★ **If an interrupt is registered after the work already completed, it fires immediately** — as a
real GPU does. ⇒ Check-then-wait must be atomic against the wake, the same discipline as a futex
guarding a sleep. ⚠ **This is not a refinement: a lost wakeup on an event that could never be posted
was 12.6 s of a 13 s `cuCtxCreate` — measured.**

⊘ **This architecture does not exist today.** Audit `026374c`: *"there is NO completion-wait
architecture — synchronous inline on the vCPU thread; the whole reactor subsystem is UNREACHED."*
⇒ The owner's design is the missing piece, named.

---

## 4. ⊘ What this does NOT solve — recorded so it is not read as closure

**Two memories.** The framebuffer objects backed at `w228` are **blank and have no CPU view**: the
host GPU reaches the real object, the guest's own accesses still reach the emulator's fabricated
aperture. Under execution the engine would read **zeros** where the guest wrote, and write where the
guest cannot see.

⚠ **Silent in both directions** — no fault, no `Xid`, no status. And ★★★ **the missing CPU view is
simultaneously the defect and the reason the defect would be invisible**: a run in which the engine
dereferenced a blank pool would log identically to a correct one.

★ The C's answer was a **double mapping** — CPU side at the framebuffer address, GPU side FIXED at
the guest VA, **both sharing one host object** — plus a one-time seed copy. `kayfabe` deliberately
took the `gpu_only` shape and named the join as successor: `Request::ExportBacking`, *"already on
the wire"*, not routed to this path.

⇒ **This is the open question, and it is an ordering decision the owner owns:** the CPU view is the
**falsifier** as much as the fix, so it arguably belongs **before** the first boot in which an engine
executes guest methods, not after.
