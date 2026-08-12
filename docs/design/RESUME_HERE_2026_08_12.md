# RESUME HERE — cold-start state, 2026-08-12 (supersedes the 08-11 handoff)

> ### STATUS — 2026-08-12 / **LIVE — THIS IS THE HANDOFF DOC.** Read before anything else.
> Supersedes `RESUME_HERE_2026_08_11.md`, which is marked SUPERSEDED-BY at its head. Everything
> needed to continue is here or linked from here. ⚠ **The night-run cron prompts still name the
> 08-11 doc**; they cannot be edited from a later session, so that doc's first line redirects here.

---

## 1. WHERE THE NORTH STAR IS

**`cuCtxCreate` still hangs. `CUP2_RC = 124`.** `CE-SUBMIT → RETIRED` has never printed.

★★★ **But the wall is now decomposed into three legs, and two are built and hardware-witnessed.**

| leg | what it is | state |
|---|---|---|
| **A — the RING** | the host GR channel is **born** over the guest's GPFIFO | ✔ **BUILT + WITNESSED** (`w261`, `w262`) |
| **B — the CURSOR** | `GP_PUT` is a word the **guest** advances ⇒ the guest's USERD, handed to RM **at creation** | ◐ **IN FLIGHT**, branch `leg-b-userd-adoption-at-creation` |
| **C — the DOORBELL** | trap, translate guest token → host token, ring | ✔ **BUILT** `b734995`, default-off behind `KAYFABE_GR_ROUTE` |

> ### ⊘⊘ CORRECTION, same day — **THE STOOL HAS FIVE LEGS, NOT THREE.** My three-leg model was
> useful and **incomplete**, and w262b's measurement is what found the rest. Four and five are
> **UNBUILT**:
>
> **4 — the PUSHBUFFER PAGES.** The guest's GPFIFO entries name VAs *outside* the leaf we join.
> `[measured, w262b]` `gp[0]` names `0x200400000`, `0x200800000` … while leg A1 joins the
> `0x200200000` leaf. **Nothing joins them.** ⇒ The ring is reachable and *the work it points at
> is not*. ★ Tractable: it is the **same mechanism as A1** — the join needs more sources.
>
> **5 — the COMPLETION PATH.** `CE-SUBMIT → RETIRED` was **0 on both `w262` arms**, as it has been
> in ~127 logs.
>
> ⇒ **`w263` is therefore pre-registered at ZERO movement, and that is a prediction against the
> rung's own optimism** — necessary-not-sufficient, exactly the shape `w260` measured for the
> supply side. ⚠ **A zero here still does not indict the passthrough model**; it indicts my
> arithmetic about how many legs there were.

**Why nothing has moved yet, and why that is expected**: a channel born `RingSource::Ours(None)`
has its `gpFifoOffset` on **our** ring and its `GP_PUT` in **our** USERD, and `submit_entry`
refuses a handed-in ring **by name**. ⇒ `GP_PUT == GP_GET` forever until **all three** legs are
present. Every zero-movement result so far was **pre-registered as zero**.

⚠ **The standing debt is NOT discharged and must not be read as discharged.** A one-legged stool
falling over is not evidence against stools. ★ **The leg-B boot is the first with all three legs,
and therefore the first where a `CUP2_RC` change is genuinely possible.**

---

## 2. WHAT LANDED 2026-08-11 → 08-12

- **Leg C** (`b734995`) — the doorbell route. ⊘ There was **no server to build**:
  `SharedDevice::doorbell` was never copy-engine-specific; the missing wiring was **one arm in one
  `if`**. Token translation is a **plain field read**, not a map lookup.
- **Leg A1** — the FB join gets a **second source**, the channel's own ring, **and it runs before
  the birth**. The join's only prior driver is the **operand** census, and *a ring is not an operand
  of the methods it carries*, so the ring was never presented.
- **Leg A2** — the production birth path names the guest's ring; **crosses the IPC wire** (the
  adapter runs in the child, so a trait-only change would be dead on the one path a boot exercises).
  **Arming is inherited** from the supply side, so a disarmed build is `None` by construction.
- **`w261`** — 24 × `GR-RING-JOIN` at `fb_phys=0x1000000`, including the walling channel.
- **`w262`** — ★ `GR-BIRTH … adopt=GUEST-RING`, **16 armed / 0 disarmed**, on client `0xc1d0000c`.
  The disarmed zero is **measured**: 24 `adopt=DECLINED` lines prove the path was consulted.
- **Display scoped** (`docs/design/display_plane_scoping.md`) — ★ `NVA083_GRID_DISPLAYLESS` is a
  **first-class NVKMS HAL**, 885 lines, `.coreChannelDma = { }`, **zero EVO channels**.
  ⇒ Emulating the display engine is **dominated**: 1–3 days vs 3–6 months.
- **`CLAUDE.md` FIFTH LIMIT corrected** — the oracle table is **11 empty / 16 truncated / 29
  complete**, not "11 empty, 45 good". The trustworthy predicate is `dlen >= psize`.

---

## 3. ★★★ WHAT LEG B ACTUALLY IS — corrected twice, both times by a doc that already existed

⊘ **It is NOT a missing `hUserdMemory` arm, and it is NOT `mem_phys`.** Adjudicated in
`docs/design/userd_is_not_the_ring.md` (STATUS LIVE 2026-08-11) — **a day before two lanes
re-derived the same decision from a false premise.**

- RM **permits** guest-supplied USERD unambiguously: `OsDescMemory` is **explicitly special-cased**
  so client pages can back USERD. No aperture gate, no RM-allocation requirement. Hard constraints
  are the whole list: **512 B, 512 B-aligned, non-VPR, phys < 2^40**.
- ★ The GSP path **cuts for passthrough**: CPU-RM resolves handle → memdesc → physaddr **locally**
  and ships GSP a physical descriptor.
- ⊘ `mem_phys`-has-no-producer is **CONFIRMED but IRRELEVANT** — RM looks `hUserdMemory[0]` up in
  **the caller's own client**, so forwarding the guest's handle was never the mechanism.
- ★★★ The guest's USERD is **already decoded** (`ChannelUserdWire`, version-keyed, live).
  `kayfabe-rmrpc/src/lib.rs:1379` names its own gap: *"⊘ Read by no decision."*

⇒ **Leg B is a MISSING CONSUMER, in the same shape as leg A1**: join the USERD's leaf, hand RM an
`OS_DESCRIPTOR` over it **at creation**. ⚠ **Never at first doorbell — RM zeroes a caller-supplied
USERD**, so late adoption wipes the cursor that caused the doorbell.

---

## 4. ⊘ WHAT IS STILL UNMEASURED

1. **That the adopted ring is FETCHED.** Nothing reads `GP_GET`. *"Adopted"* ≠ *"fetched"*.
2. **The GR channel's own cursor vs its own birth** — nothing joins a BAR1 offset to a channel.
3. **Leg B's ordering claim** — an observation cannot separate *right* from *wrong and lucky*;
   needs fault injection. The favourable reading is an **indication, not a proof** (44 advances
   dropped, and the predicate had a false positive).
4. **`NOT-ASKED = 0`** on both arms has **no live known-positive** — it rests on a unit test.
5. The three `w260` unknowns (install→bind ordering, the release path, attempt-once) — **still
   need fault injection**, still unmeasurable by a green boot.

---

## 5. ⚠ TRAPS ADDED SINCE THE 08-11 DOC — all measured, several hit after being warned

- ★★★★★ **A recorder that buffers and dumps at teardown reports ORDER correctly and TIME not at
  all.** `nvkvm_bar1_record` stores 16 entries, prints once at teardown ⇒ **every** log says
  *"GP_PUT came 178 s after the alloc"* — a constant dressed as a measurement, and it is the
  **favourable** answer. ⇒ **Ask when the LINE was emitted, not when the event happened.**
  ⚠ A broken instrument that always agrees with you is worse than one that always disagrees.
- ★★★ **A gate's PROSE is not its ASSERTION.** `guest_ring_census.rs:168` reads *"exactly one
  caller"* and asserts a **definition** count; it stayed green through the rung that gave the verb
  its first production caller. A pre-registration **promised** to fix it and did not.
- ★★ **A `#[cfg(not(feature = ...))]` sibling with an empty body is a silent no-op.** Assert the
  feature from a positive signal the armed path emits.
- ★★ **A mutation that does not apply is a green indistinguishable from a test that catches
  nothing.** Assert `count == 1` before writing.
- ★★ **`git worktree list`, not mtime, decides whether a tree is live.** A depth-limited `find
  -newermt` scan reported the **active** 18 GB tree as idle.
- ★★ **`git push origin <branch>` reports `Everything up-to-date` truthfully about a branch you are
  not on.** Check `git branch --show-current` first and `git rev-parse HEAD origin/<branch>` after.

---

## 6. NEXT RUNGS, ORDERED

1. ◐ **Leg B** — in flight. The first three-legged boot.
2. **Read `GP_GET`** — the difference between *adopted* and *fetched*. Nothing does today.
3. **Fault injection** for §4's items 3 and 5 — a green boot cannot reach them **by construction**.
4. **Merge to `master`.** ⚠ Deliberately held: `master` is still `e758778`. Merging an
   **unwitnessed** capability is the mechanism that produced the eight built-and-unwired
   capabilities already on the orphan list. Legs A1/A2 are now witnessed and eligible.
5. **Display** — decide posture A (displayless, 1–3 days) vs B (virtual display). ★ The deciding
   experiment is *"does Vulkan/EGL survive `NVKMS_ALLOC_DEVICE → NO_HARDWARE_AVAILABLE`?"* — one
   boot, two strong sources disagree.
6. ⚠ **Confirm the app count with the owner**: both `realapp_matrix.md` and the owner's own
   `PRODUCT_POSITIONING.md` say **22**, and **no 30-app list exists in either tree**.

---

## 7. HONEST LEDGER

**Every lane dispatched in this window refuted something in its brief, and in each case the
refutation was worth more than the work it replaced.** The pattern is unchanged and one-directional:
**I assume things are missing when they are already built, or already answered.** Six instances of
re-deriving an answered question — the worst being a lane that reached a **correct conclusion from a
false premise**, which reads as corroboration rather than duplication.

⇒ **Weight my estimates of remaining work as upper bounds**, and before dispatching anything run
`git log --all --oneline --grep=<topic>` in **both** trees and read the doc's dated STATUS block.
