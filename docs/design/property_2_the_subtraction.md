# Property 2 — the subtraction, and why it is the owner's call

> ### STATUS — 2026-08-11 / ⊘⊘ **SUPERSEDED IN ITS EVIDENCE — NO OWNER DECISION IS OWED. READ §0.**
> ⊘ **The decision this document asks for was ALREADY PERFORMED, and measured, the day before
> this document was written.** §2's central claim is false at `b3ecda4`, and its exploit citation
> is a **pre-fix** measurement quoted without its revision.
> ★ A **different, smaller** residual survives — a different object — and it is named in §0.
> **Do not act on §4's question; it is posed about a topology the tree no longer has.**

---

## 0. ⊘⊘ CORRECTION, MEASURED 2026-08-11 at `b3ecda4` — this corrects §2, §3 and §4

### §2's central claim is FALSE at HEAD

> *"★★ And there is exactly ONE host address space per guest address space today, holding all of:
> the guest's channel; the host-backed framebuffer leaves at guest-chosen addresses; **and our own
> ring, cursor block and completion semaphore**."*

**Three separate things in that sentence are wrong**, all readable in the code:

- **TWO host address spaces per guest `Vas`, not one** — `Vas::host_vas`
  (`kayfabe-core/src/gpu.rs:171`) **and** an `ExecutorVas` minted lazily beside it
  (`kayfabe-isolate-host/src/rm.rs:2975`, one per guest range). `free` disposes of both
  (`rm.rs:3285`).
- **The isolate's ring / USERD / completion semaphore are NOT in the guest's space** —
  `ce_copy_outcome` → `executor_vas(key)` → `ce_channel(key, exec)` →
  `alloc_channel_for_isolate(vas: ExecutorVas)` → `alloc_channel_in(vas.range, …)`
  (`rm.rs:4442,4443,3812,3817`). Mapped through `exec.range` and **nothing else**, and
  `ExecutorVas` cannot be *spelled* by a caller holding a guest `Vas` (private field, one mint
  site, plus a `trybuild` case).
- **The "cursor block" is in NO GPU address space at all** — USERD goes to RM as
  `hUserdMemory[0]` and is only ever CPU-mapped. No guest VA can name it under any topology.

### ⊘ And the exploit citation is a PRE-FIX measurement quoted without its revision

§2 cites R30 arm C — *"a copy engine bound to the guest's address space read our own semaphore
payload back"*. That was measured at **`cc5d55c`**. The fix landed at **`254cf38` (2026-08-10)**;
`ae73f6b`, the audit §2's co-location row rests on, is an **ancestor** of it. The same arm was
**re-measured and REFUSED**, with hardware's own word:

```
★  R30 arm C = the guest-bound engine did NOT retire a read of 0x1_20022000
NVRM: Xid 31 … CE0 faulted @ 0x1_20022000, FAULT_PDE ACCESS_TYPE_VIRT_READ
```

re-confirmed again at `b39f95f`. ⇒ ★★★ **This is the *"a ruling's DATE is part of the citation"*
class, committed by me, against a MEASUREMENT rather than a ruling.** §3 calls `ExecutorVas`
*"the precedent that makes it tractable"*: it is **not a precedent — it is the fix**, already
applied to the object §2 names.

### ★ What actually survives — a DIFFERENT object

The residual is the **materialized guest channel's own 64 KiB ring**, via `alloc_channel` →
`alloc_channel_on` → `alloc_channel_at(vas, …, None)` → `RingSource::Ours`, `raw_map_dma`'d into
the guest's range. `executor_vas_separation.md` §6 **excludes it explicitly** (*"stays in the
guest's space by design — it is the guest's channel"*). ⇒ §3's *mechanism* sentence
(`alloc_channel_at` must change) aims at the **right call**; §2's *evidence* aims at an object
that **already moved**.

★★ **And it needs no owner decision and no new address space.** `w230`'s
`alloc_channel_over_guest_ring` (`rm.rs:3791`) builds the channel over the guest's **own** pages,
so `RingOwner::HandedIn` maps *nothing of ours* into the guest's space. It is **built**, with
**one caller — the R31 probe**. Promoting it to the doorbell path removes the residual **as a
side effect of work the execution plane needs anyway**.

### ⊘ Ruling 2 (kernel CE VA spaces need not exist) does NOT dissolve this

Two code facts refute the premise:
1. **Every CE copy this tree can issue is VIRTUAL, by a standing refusal** — `ce_pushbuffer` ORs
   `LAUNCH_SRC_VIRTUAL | LAUNCH_DST_VIRTUAL` (`rm.rs:1930`); the `_PHYSICAL` variants exist only
   as decode-side constants, refused by name (*"nothing in this project's threat model permits
   it"*). ⇒ Kernel CE work needs **a** host VA space. The ruling can delete the **guest's** space,
   never **a** space.
2. **No producer of an executor-space mapping starts anywhere but the guest space** —
   `map_dma_both` runs `raw_map_dma(guest_range, …)` **first** and feeds RM's returned address
   into the shadow. The VMM translation the ruling invokes exists in `AddressTable::resolve`, but
   **no verb consumes it that way.**

⚠ **Applied literally today, ruling 2 turns kernel-channel forwarding OFF**: with no `host_vas`,
`plan_doorbell` returns `FwdFault::NoVas` and `plan_ce` returns `FwdFault::NoHostVas` — hard
refusals. It does not reroute; it refuses.

### ⚠ NEW, `[NOT MEASURED]` — an exposure the separation itself created

`map_dma_both` places every guest publish in the `ExecutorVas` at the **guest-chosen** VA, while
our CE ring sits there at an **RM-chosen** VA. A fixed publish colliding with our ring makes the
shadow map fail, and the verb tears the guest-side map down and returns `PlacementRefused` for a
publish that would otherwise succeed. ⇒ **A guest can locate the isolate's CE ring by binary
search over publish refusals** — in a space it can never read. **Address disclosure, not a read.**
Inferred from `rm.rs:3029-3040`; **no probe has asked hardware.**

---

## 1. The three properties, and where they stand

The graphics execution route was gated on three properties. As of `814b225`:

| | property | status |
|---|---|---|
| **1** | the operands **resolve** | ★ **DISCHARGED, MEASURED.** All three address planes armed simultaneously on a booting tree: 5 of 5 operands accounted for, 24/24 host-backed `placed_as_asked=true`. Predictions pre-registered and committed before the boot |
| **2** | our own machinery is **unreachable** in the guest's graphics address space | ⊘ **OPEN. THIS DOCUMENT.** |
| **3** | a guest-caused GPU fault is **contained** | ★ **DISCHARGED, MEASURED.** A bystander context ran **2 675 519** verified iterations across the attacker's fault: 0 errors, 0 wrong bytes, no escalation, no reboot latch |

---

## 2. ★★★ Why property 2 is not theoretical — it was MEASURED EXPLOITABLE

⊘ It was once argued as *"not exploitable today, and latent."* **That was refuted by measurement**
(R30 arm C, real GA106): a copy engine **bound to the guest's address space read our own semaphore
payload back, value for value** — a number obtainable no other way.

> **The only thing standing between a guest and the isolate's semaphore was that nobody had pointed
> an engine at it.**

★★ **And there is exactly ONE host address space per guest address space today**, holding *all* of:
the guest's channel; the host-backed framebuffer leaves at **guest-chosen** addresses; **and our own
ring, cursor block and completion semaphore.** The separation is **stated in the design docs and
violated in the placement.**

⚠ **The bill is already being paid**: every publish is currently done twice.

---

## 3. What the change actually is

**A subtraction.** Not a feature — the removal of a reachability that exists today.

- **The mechanism**: `alloc_channel_at` must change so the isolate's ring / cursor block / completion
  semaphore are **not placed in the address space the guest's graphics channel can name.**
- **Why it is architecture, not a patch**: it changes *where our own execution machinery lives*, which
  touches the isolate's address-space model — the thing the isolate exists for. ⇒ It is not a line;
  it is a decision about the shape of the plane.

★ **The precedent that makes it tractable**: the same shape has already been separated once
(`ExecutorVas`), and the framebuffer operands are already placed at guest-chosen addresses
successfully (`placed_as_asked=true`, 24/24). **The machinery to place things deliberately exists.**

---

## 4. ⇒ THE QUESTION FOR THE OWNER

**Do we separate the address spaces now, before any graphics execution is attempted?**

| | if YES — separate first | if NO — execute first, separate later |
|---|---|---|
| **cost** | an architecture change on a plane that is otherwise green; delays the first execution attempt | the first attempt happens sooner |
| **risk** | ⊘ none measured — properties 1 and 3 are already discharged without it | ⚠ **the measured read-back becomes reachable by a real workload**, not just by a probe |
| **rework** | none — the separation is a precondition either way | ★ every execution result measured before the separation is measured **in the wrong address-space topology**, and may not survive it |

★★★ **My recommendation: SEPARATE FIRST**, and the reason is not security — it is that
**a result measured in the wrong topology is a result that has to be re-measured.** This campaign has
paid that price repeatedly: the address plane was measured four separate times under configurations
that turned out not to be the shipping one. ⊘ **The security argument is real but secondary** — the
hostile-guest posture is the product's value proposition, and shipping a measured read-back into the
first execution rung would be the wrong order regardless.

⚠ **The counter-argument, stated fairly**: `CE-SUBMIT` has been 0 for the entire campaign, and
**nothing has ever executed.** An architecture change made *before* the first execution is a change
made without the one measurement that would tell us whether the topology matters in practice.
⇒ **If the owner prefers to see something execute first, that is a defensible call** — provided the
first execution rung is explicitly labelled *"measured in the pre-separation topology, to be
re-measured after."*

---

## ✔ Evidence, all measured on a real GA106 and committed
- **Property 1**: `traces/boots/w247/`, with `predictions_recorded_before_the_boot.md`.
- **Property 3**: `traces/boots/w248/`, `SCRIPT_RC=0`, 5 of 5 predictions.
- **The exploit**: R30 arm C, `executor_vas_separation.md`.
- **The co-location**: `s1_what_does_it_protect.md` §3 — one host address space per guest address
  space, everything in it.
