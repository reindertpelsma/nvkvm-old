# Property 2 — the subtraction, and why it is the owner's call

> ### STATUS — 2026-08-11 / **LIVE — AWAITING OWNER DECISION**
> **The last blocker on the graphics path that is not addressing.** Properties 1 and 3 are
> discharged and measured. This one is an **architecture change**, it is a **subtraction**, and
> nothing below it can be built without deciding it.
>
> ⊘ **Nothing has ever been forwarded.** `CE-SUBMIT` is 0. This decision does not change that on its
> own — it **unblocks the attempt.**

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
