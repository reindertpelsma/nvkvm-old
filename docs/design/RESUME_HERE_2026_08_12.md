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
> > ### ⊘⊘ CORRECTION, 2026-08-12 — **IT IS THE PIN THAT NEEDS MORE SOURCES, NOT THE JOIN.**
> > Read this before acting on the sentence above; that sentence names the wrong plane and a
> > rung built on it **cannot fire**.
> >
> > `[measured, traces/boots/w263/run_w263_ring_qemu.log, all 8 channels, BOTH arms]` the
> > pushbuffer VAs resolve **`pb=S:0x3d45f000` … `S:0x3e25f000`** — and `CeResolve::tag`'s own
> > doc is the authority on that letter: *"`V` = this device's framebuffer, **`S` = guest
> > RAM**, `P` = peer"*. ⇒ **The pushbuffer pages are in GUEST RAM, not the framebuffer.**
> > (The `Vidmem` that later readings attach to these addresses is the **ring's** aperture,
> > `rng=V:0x1024000`, and the `FwdFault::PushbufferAperture{va:GpuVa(8592179200)}` beside it
> > decodes to `0x200224000` — the ring's VA, not a pushbuffer's.)
> >
> > `kayfabe_rt::ceutils::resolve_leaf_of` answers `(Site::GuestRam, **None**)` for a sysmem
> > resolution **by construction**, and says why in its own comment: *"it is not this source's
> > to join: **the guest-RAM pin owns that plane**."* A third join source would have been
> > handed eight guest-RAM addresses, printed eight refusals, and joined nothing.
> >
> > ★★★ **The mechanism was already built and had no source.**
> > `SharedDoorbell::pin_ring_guest_ram` is the complete chain — VA → address table → GPA →
> > aperture check → the hypervisor's stated layout → file offset → one `OS_DESCRIPTOR` per
> > contiguous run, mapped **FIXED at the guest's own VA**. It is asked about exactly **one**
> > address, the ring's, which is in Vidmem, so on `w263` it refused all eight `NOT IN GUEST
> > RAM` — **by name and correctly**. ⇒ *The pin has never pinned one byte on a live guest,
> > and not because it is broken.* The addresses in the aperture it serves are on the same log
> > line, eight of them, and nothing presents them.
> >
> > ⇒ Leg 4 keeps its shape — *the primitive works, the source list is short* — and changes
> > its verb: **give the PIN a second source.** Built on branch `leg-4-pushbuffer-pin`
> > (`KAYFABE_GUEST_PUSHBUF=pin`); pre-registration `docs/design/w264_pushbuffer_pin_prereg.md`
> > in the `nvkvm-rs` tree.
> >
> > ⚠ The *hardware* half of the w263 reading is untouched: the eight `Xid 31 FAULT_PDE
> > ACCESS_TYPE_VIRT_READ` are real, and `FAULT_PDE` is exactly what a pin at those VAs would
> > install a directory entry for. Only the named mechanism was wrong.
> >
> > ### ⊘⊘⊘ CORRECTION, 2026-08-12 — **LEG 4 IS DONE, AND THE WALL IS NOW ON THE COMPLETION
> > PLANE.** Read this before acting on the `w264` block below; that block's *"the next rung"*
> > has been RUN. `traces/boots/w265/RESULT.md` (rev `2f02621`, 2 arms, real GA106).
> >
> > ★★★★★ **NO CODE WAS WRITTEN. The populate source was ALREADY BUILT and `w264` ran with it
> > OFF** — `KAYFABE_PT_WITNESS_EXEC`. `w261`/`w262` armed it; `w263_run.sh` and `w264_run.sh`
> > silently dropped it, and all four `w264` arms say `EXEC-WITNESS DISARMED` in their own logs.
> > ⇒ The ninth consecutive lane whose brief's premise was already answered — and the sharpest
> > instance yet: `execution_plane_increments.md` §16.98.1 diagnosed **this exact class about
> > this exact flag two rungs earlier** (*"a correct default is not a handoff … must be named in
> > the CONSUMER's preconditions, not only in the producer's rationale"*) — and recorded it in
> > the **producer's** doc, which is the failure mode the sentence describes.
> >
> > **One variable, `off`→`on`, measured:**
> > - the table LEARNED the leaves — `pdb=0x201000` rows **5 → 13 348**, `wit` **0 → 37**,
> >   `wit_sample` `[]` → **`[0x201000,0x202000,0x203000,0x204000]`** = *exactly* the four
> >   page-table pages the descent calls `byEXEC#104…#107`;
> > - `PB-PIN … MISS` **8 → 0**, resolved-in-guest-RAM **0 → 8**, **`PINNED` 0 → 8** — the
> >   guest-RAM pin has placed bytes on a live guest **for the first time**;
> > - `NOT-IN-GUEST-RAM = 0` ⇒ the **`miss = fault` invariant HELD**; the fix adds a *writer* to
> >   the witness, never a lookup path, so residue still cannot bind.
> >
> > ★★★★★ **AND THE EIGHT `Xid` AT THE EIGHT PUSHBUFFER VAs ARE GONE:**
> > ```
> > off: ENGINE CE3_PBDMA0 HUBCLIENT_ESC @ 0x2_02c00000 (8 distinct VAs) ACCESS_TYPE_VIRT_READ
> > on:  ENGINE CE3        HUBCLIENT_CE1 @ 0x2_0440f000 (ONE address)    ACCESS_TYPE_VIRT_WRITE
> > ```
> > Front-end → engine; method-fetch client → **data** client; the pinned pages → a new page;
> > **READ → WRITE**. ⇒ **The PBDMA fetched the pushbuffer, parsed its methods, and the copy
> > engine began EXECUTING them.**
> > ★★★ **`0x2_0440f000` is the COMPLETION SEMAPHORE PAGE** — eight channels'
> > `SET_REPORT_SEMAPHORE` targets, `0x20440ff80 … 0x20440fff0` at 16-byte stride, all
> > `site=GuestRam`. **Leg 5 has arrived as a hardware fault at a named address, and the fix is
> > isomorphic to the one just landed, on ONE page.**
> > ⚠ `CUP2_RC = 124` on both arms, **pre-registered at zero movement** (fifth consecutive lane
> > to predict zero and measure zero — still right: no table fix retires a semaphore nothing
> > submits). `CE-SUBMIT → RETIRED` still `0`.
> > ⊘ **Costs, unpaid:** `unwitnessed` rose **6275 → 19 874** beside `bound` **6275 → 19 615**
> > (the gate opened *partway*); **255 `StraddlesLiveBinding` refusals** (0 on `off`).
> > ⊘ **Not attributable to the PIN** — the arm changed 13 343 bindings, so the `Xid` move
> > belongs to the **arm**.
> > ⊘⊘ **AND THE INSTRUMENT LESSON, which cost the most:** `grep -c Xid` read **8 on both
> > arms**. ★★★ **A COUNT CANNOT SEE A SUBSTITUTION** — five facts changed and a magnitude saw
> > none of them. When a fix is expected to **move** a wall rather than remove it, the identity
> > is the instrument. `w265_grade.sh` now carries `Xid` ENGINE/CLIENT/DISTINCT-ADDRS/ACCESS-TYPE
> > as scorecard rows.
> >
> > ### ★★★★★ MEASURED, `w264` (4 arms, real GA106, rev `a4c46bb`) — **AND LEG 4 IS NEITHER**
> > `traces/boots/w264/RESULT.md`. The pin was built, armed, and asked about **exactly the
> > eight addresses hardware faults on**. The **address table answered `Miss` on all eight**,
> > while the **descent on the same log line resolves each to guest RAM** (`pb=S:0x41539000
> > …`), and `NOT-IN-GUEST-RAM = 0` on every row.
> > ⇒ ★ **The two resolvers disagree about EXISTENCE, not about aperture.** Leg 4 is not *"join
> > the pages"* and not *"pin the pages"* — it is **the address table's POPULATE side never
> > learning the pushbuffer leaves**. The consumer is built and correct; the authority was
> > never told. ⊘ `miss = fault` means it cannot be papered over at the consumer.
> > **The next rung's address list:** `pdb 0x201000`, VAs `0x202400000 0x202600000 0x202800000
> > 0x202a00000 0x202c00000 0x202e00000 0x203000000 0x203200000`. Open question to split
> > first: *never learned* vs *learned and pruned before we asked* — a `Miss` does not
> > separate them; start at `PT-DECODE`'s `bound=6275 unwitnessed=6275`.
> > ⚠ `CUP2_RC = 124` on all four arms, **pre-registered at zero movement**, and `CE-SUBMIT →
> > RETIRED` still `0` — leg 5 is unbuilt, so this rung could not have moved it.
> > ★★ Second result: the four-arm ladder (one variable per step) **discharges `w263`'s own
> > qualification** — `PushbufferAperture 0→9` belongs to legs **A2+B** (`join`→`ring`), not to
> > the pushbuffer plane, and the FB join **alone** (`base`→`join`) moves no doorbell-level
> > number at all. ⊘ Still unseparated: **leg B vs leg A2** — `ring` and `pin` both carry B, so
> > this campaign cannot attribute the fetch, and doing so needs a boolean on
> > `plan_engine_object`'s public signature (leg B's arming is inherited by construction).
>
> **5 — the COMPLETION PATH.** `CE-SUBMIT → RETIRED` was **0 on both `w262` arms**, as it has been
> in ~127 logs.
>
> > ### ⊘⊘ CORRECTION, 2026-08-12 — **LEG 5 NOW HAS AN ADDRESS, AND HARDWARE NAMED IT.**
> > `w265`'s `on` arm faults `ENGINE CE3 HUBCLIENT_CE1 @ 0x2_0440f000 ACCESS_TYPE_VIRT_WRITE`,
> > ×8. That page holds **eight channels' `SET_REPORT_SEMAPHORE` targets**
> > (`0x20440ff80 … 0x20440fff0`, 16-byte stride, all `site=GuestRam`). ⇒ Leg 5 is still
> > **unbuilt**, but it is no longer *unlocated*: the copy engine is now **trying to complete**
> > and faulting on the write. ★ The primitive it needs is `pin_guest_ram` — the one that just
> > placed 8 pushbuffer runs — pointed at **one** page. ⊘ `CE-SUBMIT → RETIRED` is still `0`;
> > nothing here submits, and an attempted semaphore write is not a retirement.
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
