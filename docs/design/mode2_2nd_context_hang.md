# Mode-2 #12 — 2nd-CUDA-context hang: root cause (CE completion sema)

Status: diagnosed 2026-06-17 (root cause proven end-to-end via an instrumented
full-source guest driver). The wrap-wedge *layer* is fixed and committed
(`37d15c5`); the CE-completion *layer* documented here is the remaining blocker.

> ### UPDATE 2026-06-19 (cont. 4) — SOURCE + LOG forensics: it's a VAS-RESOLUTION miss (transient-VAS free), NOT sharing/lifecycle; map-loss pinpointed
>
> Read the open RM source (`ce_utils.c`, `channel_utils.c`, `mem_utils_gm107.c`, `video_mem.c`)
> + mined the persisted `cupctx2` QEMU log. Six facts, then the exact map-loss point.
>
> **Architecture (source).** CeUtils is **kernel RM** (CPU-side), via
> `rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL)`; it allocates its **own** `NV01_ROOT` client and
> its **own fresh VAS**, `bUseBar1=TRUE`. The scrub channel buffer (gpfifo+pushbuffer+semas, one
> memdesc) is vidmem by default (`ADDR_FBMEM`). The teardown wait `channelWaitForFinishPayload`
> is a **kernel CPU busy-poll** (`while(READ_CHANNEL_PAYLOAD_SEMA < target)`, 4 s timeout,
> services scrubber interrupts but completion authority = the sema *value*). The sema is read by
> `channelReadChannelMemdesc` → `memmgrMemDescBeginTransfer(USE_BAR1)` → `MEM_RD32(pbCpuVA +
> finishPayloadOffset)` — a **CPU read through BAR1 of the channel buffer's vidmem**, at
> `pbGpuVA + finishPayloadOffset` (`= gpfifo_va + 0x8004`).
>
> **CORRECTION to cont.-era claims — who allocates FB phys.** *We do NOT assign vidmem phys.* The
> **guest's CPU-side PMA is the sole FB allocator**: `vidmemConstruct` → `pmaAllocatePages`
> (unconditional, CPU-side); `NV_RM_RPC_ALLOC_VIDMEM` is gated `!IS_GSP_CLIENT` (a vGPU alias
> step, not the allocator) and our GSP-client guest skips it. Our fake GSP only declares the heap
> *bounds* at boot (`pmaRegisterRegion`); the guest picks every offset within them and writes
> them into PTEs (GPU VAS + BAR1). **One allocator ⇒ a GSP-vs-driver phys overlap is structurally
> impossible** — so the `0x31f0000` "collision" is **not** sharing, **not** temporal reuse, **not**
> a lifecycle bug. It is our resolver guessing.
>
> **Map-loss pinpointed (log).** Hang channel = scrub, client `0xc1e00007`, gpfifo VA
> `0x120064000`, **instance block `0x2efa6e000`**. The mapping is lost at three points:
> 1. **Transient-VAS free.** Guest allocs VASPACE `0x0c` → we capture PDB `0x2efba5000` (L3476) →
>    guest **frees `0x0c`** (L3482) → *then* allocs the channel (L3523). So our recorded `cli_vas`
>    for this client is the dead `0x2efba5000`; every probe FAULTs. The **sibling** scrub channel
>    (`0xc1e00008`, VAS `0x0a`/PDB `0x2efa6c000`) kept its VAS, resolved via `res=cli_vas`, and its
>    finishPayload `0x42006c004` → **sysmem `0x144a48004`** was written correctly. *Same code path,
>    correct result — the only difference is the transient free.*
> 2. **Instance-block PDB reads empty.** `M5.14` reports "PDB empty (GSP-managed)" for these
>    channels — `instblk + RAMIN_PDB_off` is zero in our mirrored FB, so the bulletproof fallback
>    (read the channel's own GMMU root) yields nothing.
> 3. **The forge bypasses VAS resolution** and uses the `bar1_wpg` MRU → `finFB=0x31f8004`, the UVM
>    channel's buffer (`0x1210d0000`/phys `0x31f0000`). Collision → guest reads its real
>    finishPayload elsewhere → `ce_utils.c:349`.
>
> **Fix anchors (both match the address-table directive; retire `bar1_wpg` for finishPayload):**
> (1) **Instance-block PDB** — make `instblk 0x2efa6e000 → PDB` resolve, then
> `finishPayload phys = walk(PDB, gpfifo_va + 0x8004)`; immune to VAS-handle churn, general to
> every channel. (2) **Don't lose the real VAS** — bind the channel to the VAS its `c56f` params
> reference (the `cli_vas` capture that already works for the sibling), don't drop it on the
> transient's free. **Open read-only question gating (1):** *why does RAMIN+0x200 read empty here*
> — wrong offset for this channel class, not mirrored into our FB, or genuinely GSP-populated (our
> fake GSP never wrote it)? That's the next step. (See `[[mode2_address_table_of_truth]]`.)
>
> ### UPDATE 2026-06-18 (cont. 3) — FIX ATTEMPT BENCH-DISPROVEN: the wall is memslot/fb_write INCOHERENCE, and owning-client overlay-release is UNSAFE
>
> Built + bench-ran the agreed first fix (release a root-freed client's GPGA overlays so
> the scrub finishPayload de-aliases to base FB) on `cupctx2` (2-context #12 repro), fresh
> QEMU, forge active. **Result: STILL HANGS** — CTX1 matmul byte-exact PASS, CTX1
> `cuCtxDestroy` returns, then CTX2 `cuCtxCreate` hangs (rc=124). Guest dmesg is unchanged:
> `scrubberDestruct: Timed out` (4 s) + `nvAssertFailedNoLog … lastCompletedPayload ==
> lastSubmittedPayload @ ce_utils.c:349`. Two hard facts fell out:
>
> 1. **The forge fires with the EXACT right values, and the guest still doesn't see them.**
>    45 `#12 FORGE finishPayload` lines on the scrub channel (`client 0xc1e00007`,
>    `gpfifo=0x120064000`): `finFB=0x31f8004` written `0→1→…→8`, then the ring re-fragments
>    mid-run (`b1off 0x128004 → 0xa8004`) and `finFB=0x3138004` written `9→10→…→20+` — a
>    clean monotonic `lastSubmittedPayload`. So the *value* and the *channel* are correct;
>    the **delivery target is not the memory the guest reads.** The guest read is
>    memslot-served (poll-spin still 0), so QEMU's `nvkvm_fb_write` (which routes through
>    `nvkvm_fb_host_overlay` → an overlay `host_qva`, else base `fb_page`) writes a
>    *different* host backing than the KVM memslot the guest's BAR1 read resolves to. This
>    incoherence — not the value, not the offset — is THE remaining wall.
>
> 2. **Owning-client overlay-release is UNSAFE (cross-client sharing), so it was reverted.**
>    The release fired for UVM `client=0xc1d00003`, dropping the overlay for FB `0x3130000`
>    — **but the scrub channel (`client 0xc1e00007`) reads its ring/finishPayload from that
>    same FB phys.** Freeing the backing on the *owner's* free yanks it out from under a
>    still-polling *different* client — exactly the cross-VAS/cross-client sharing the
>    address-table model forbids without a refcount over **all** referencing clients/VAS
>    (the user's "phys freed only when every reference drops"). The naive
>    `m2_objs[].client == fClient` scope violates that. Reverted to the committed baseline
>    (only an explanatory NOTE remains in `ctx_free_drop`).
>
> **Where this leaves the fix (pick one; (b) is the principled "real not fake" path):**
> - **(a) Coherent backing for the scrub ring.** Stop the scrub channel's FB phys from
>   COLLIDING with a UVM client's object (the `M5.24` map-FAIL is the symptom): give the
>   GSP-managed scrub its OWN host object + memslot so its finishPayload page is written and
>   read through the SAME backing. Forge then lands where the guest reads.
> - **(b) Execute the scrub CE on the host.** Forward the GSP-managed scrub channel's
>   pushbuffer (resolve its VAS via the address table, not the `bar1_wpg` heuristic) so the
>   host GPU's real `SET_SEMAPHORE` writes the real finishPayload through the guest-coherent
>   backing. This is `mode2_real_forward_not_fake` + the address-table execution piece.
> - **(c) Memslot-punch.** Exclude the finishPayload page from the non-trapping memslot so
>   the guest's poll TRAPS into `baraperture_read`, where QEMU returns `c->fin_payload`
>   directly (no backing-coherence needed). Smallest blast radius; least "real".
>
> Incidental blockers cleared this session (not #12): the VAST guest auto-upgraded its
> kernel to `6.8.0-124` while `nvmods` are vermagic `6.8.0-117` (`Invalid module format` →
> `cuInit=100`) — pinned grub to 117 + QEMU restart (see `[[mode2_bench_kernel_drift]]`);
> and the `mode2_diag` orchestrators slurp the piped script via a stdin-less inner `ssh`
> in the boot-wait loop (silent truncation) — fixed with `</dev/null`.
>
> ### UPDATE 2026-06-17 — CORRECTED ATTRIBUTION (ground truth wins): the blocker is the finishPayload `0x…6c004`, NOT `0x121000010`
>
> **Correction of a same-day mis-attribution.** An intermediate write-up of this section
> (commit `99bb828`) named `va=0x121000010` (client `0xc1e00007`) as the root cause —
> the backward-aliased write the resolver mis-routes to a foreign VAS. That is a **real but
> SEPARATE** phenomenon (the UVM *tracking* sema, the `#12-L3` line, already contained by the
> `37d15c5` backward-defer). It is **not** what `scrubberDestruct` waits on. The
> **instrumented-guest ground truth** (counters printed from *inside* the guest driver at the
> hanging destruct — stronger than any QEMU-side parse, because only the guest knows what its
> destruct polls) is decisive:
> ```
> destruct #1 PASSES: lastSub=2  hwsema=2  finVa=0x42006c004  bUseBar1=0  (sysmem finishPayload — parser writes it → match)
> destruct #2 HANGS:  lastSub=63 hwsema=0  finVa=0x12006c004  bUseBar1=1  (VIDMEM finishPayload — NEVER written → timeout)
> ```
> **EXACT ROOT CAUSE:** the hanging channel is gpfifo **`0x120064000`** (pbGpuVA
> `0x120000000`, a GSP-managed UVM CE channel). `chan_exec` shows **`picked_pdb=0`** → we
> cannot resolve its pushbuffer VAS, so we *drain* its ring (gp_get 0→63) but **do not parse
> its methods**. Its CE `SET_SEMAPHORE`/release to the **finishPayload sema
> `0x12006c004`** (= pbGpuVA + `finishPayloadOffset 0x6c004`, in **VIDMEM**, `bUseBar1=1`)
> therefore **never executes** → the sema reads 0 → `ceutilsDestruct`→
> `channelWaitForFinishPayload(63)` times out (4 s) → `ce_utils.c:349` → device `StateUnload`
> botched (last-context destroy de-inits the whole GPU) → CTX2 cold `StateLoad` hangs.
> Confirmed both nights: **zero writes to `0x12006c004`** in the QEMU log; the analogous
> *sysmem* sibling gpfifo `0x420064000` DOES get its `CE_SEM_RELEASE → 0x42006c004` and
> passes (`bUseBar1=0`, parser resolves + writes it).
>
> Today's `m2trace` is **consistent** with this once read correctly: a re-mine query for any
> `SET_SEMAPHORE` targeting `0x12006c004` came back **empty** (never parsed), while the
> `0x121000010` releases ARE parsed — i.e. we parse *part* of the channel's ring but miss the
> GP entry carrying the finishPayload release. `picked_pdb=0` is why.
>
> **Why single-context (LLM/cup8) passes:** the finishPayload that goes unwritten is on a
> **vidmem, `bUseBar1=1`** channel that only appears at the *2nd* context's device-reinit
> teardown; the sysmem sibling (`bUseBar1=0`) we DO write covers the 1st.
>
> **The fix targets `0x12006c004`, not `0x121000010`:**
> 1. **Parse the GSP-managed channel's finishPayload release even when `picked_pdb=0`** — via
>    the `bar1_wpg` FB-backing fallback (the guest BAR1-wrote the ring to vidmem we mirror);
>    then `nvkvm_chan_sem_wr32` writes the vidmem sema via `fb_write` (it already handles
>    vidmem). The finishPayload VA→FB mapping is already known (the guest's own
>    `DIAG BAR1 WR off=0x12006c → FB 0x31f006c`). The missing piece is **parsing the
>    release**, not resolving a sema VA. ⇒ `bar1_wpg` is **load-bearing** here — the earlier
>    "pin the own VAS" attempt regressed precisely by *disabling* it.
> 2. **OR forge-complete at teardown** (the "wait if real work, else complete now" rule):
>    finishPayload is at a fixed `+0x8004` offset in the same buffer we already resolve for
>    `GP_PUT`; on a `bUseBar1` scrub channel whose ring we can't fully parse, write
>    `finishPayload = lastSubmittedPayload`. Safe because the scrubbed memory is being freed.
>
> The `0x121000010` foreign-VAS aliasing (MISS=FAULT, address-table population) is still a
> valid cleanup — but it is a **separate** task, not the `#12` destruct blocker. Do not
> conflate them again.

The clean fix is the address table — see `mode2_address_table.md`.

> ### UPDATE 2026-06-18 — forge MECHANISM bench-validated; target-DELIVERY is the remaining wall
>
> Implemented the "complete the no-op scrub" forge (option 2) and ran cupctx2 on the
> vast.ai vh bench (RTX 3060) over two rounds. **The mechanism is validated; the value is
> exact; CTX1 passes; no UVM poison. cupctx2 still hangs (rc=124)** — the forged value never
> reaches the backing the guest actually polls. Precise findings (the value of this round):
>
> - **Forge fires on EXACTLY the right channel/value.** `#12 FORGE finishPayload ch[0]
>   gpfifo=0x120064000 … client=0xc1e00007` — the CeUtils scrub channel from the instrumented
>   ground truth — with a **monotonic, exact** value (`c->fin_payload` = cumulative GPFIFO
>   entries == `lastSubmittedPayload`, since `channelPbInfo.payload = lastSubmitted+1` and one
>   entry per op, `ce_utils.c:611`). Never a backward write ⇒ no `uvm_gpu_semaphore` poison.
>   CTX1 fully passes; the hang is unchanged at CTX2's first `cuCtxCreate`.
> - **The guest reads finishPayload via a non-trapping MEMSLOT — CONFIRMED (round 3).** A
>   poll-spin detector with the LOFB window REMOVED (fire on any address read ≥2000× in a row)
>   fired **0** times during the 4 s timeout, while `DIAG BAR1 WR` fired **1032**. So the
>   finishPayload reads NEVER reach a QEMU trap at all — they are served by a KVM RAM memslot
>   mapped into the guest's BAR1. **QEMU is structurally blind to the poll**, and
>   `nvkvm_fb_write(fin_fb,…)` only reaches it if `fin_fb`'s overlay IS that memslot's host RAM
>   — which M5.16 aliasing breaks. Proof the page is wrong, not just lagging: the resolved page
>   `0x31f8004` reached the **full** count (45) yet the guest still timed out. ⇒ **No QEMU-side
>   read-trap diagnostic can locate the target; resolution must come from the guest's BAR1
>   mapping authority (address table) or a guest-side oracle.**
> - **FB+0x8004 is wrong (buffer is FB-fragmented); BAR1-offset+0x8004 via the BAR1 PTEs is
>   the right primitive but still hits the memslot mismatch.** The ring pages jump
>   `0x31f0000 → 0x3130000` in FB, so `chan_gpfifo_phys+0x8004` lands on a foreign fragment.
>   Resolving through `walk_pdb(bar1_pdb, chan_gpfifo_bar1off + (gpfifo_va&0xfff) + 0x8004)`
>   correctly absorbs the fragmentation — but the guest still reads elsewhere (memslot).
> - **M5.16 cross-channel ALIASING.** The shared `bar1_wpg` pool makes M5.16 sometimes pick
>   the **compute** channel's ring page (`b1off=0xa0000 → FB 0x3130000`, the known compute
>   gpfifo) as the scrub channel's base, so the monotonic sequence splits across the real page
>   and a wrong one — and a forge write can corrupt the compute channel's buffer (a real
>   regression risk; the forge is therefore gated behind `m2trace`, default OFF, NOT a default
>   fix yet).
> - **`M5.24 GPFIFO double-mmap … phys=0x31f0000 sz=0x8000 → map-FAILED`** for this channel,
>   and finishPayload (`+0x8004`) is past the `0x8000` gpfifo span regardless.
>
> **The remaining problem is now precise:** deliver the (correct, exact) forged value to the
> **exact backing the guest polls** (a memslot we don't currently write), and stop M5.16 from
> aliasing other channels' pages into this channel. Both are the "one address table of truth"
> work (`mode2_address_table.md`): per-channel-isolated VA→backing with the guest's BAR1
> mapping as the authority. **Next experiment:** stage the instrumented 580.159.04 guest
 (the oracle that prints the finishPayload CPU-VA + the value it reads each poll) to nail the
> exact read backing in one run, rather than guessing QEMU-side. Forge code lives in
> `nvkvm_chan_execute`'s doorbell loop (the `#12 FORGE` block + `c->fin_payload` +
> `chan_gpfifo_bar1off` + `bar1_wpg[].off`), gated behind `m2trace`.

> ### UPDATE 2026-06-18 (cont.) — ROOT CAUSE FOUND by log forensics: an FB-physical COLLISION (no oracle needed)
>
> Mining the persisted QEMU log (zero extra bench rounds) decoded the layout AND the bug:
> - **BAR1↔FB is contiguous-linear here** (`off 0x120000→FB 0x31f0000`, `off 0x12006x` are the
>   GP ring entries, e.g. `0x20000648 / 0xd801`), so `finishPayload = gpfifo_va + 0x8004 →
>   FB 0x31f8004` IS geometrically correct. The forge writes the right *offset*.
> - **But emulated-FB phys `0x31f0000` is CLAIMED BY TWO DIFFERENT GUEST CHANNELS** — the
>   decisive lines:
>   ```
>   M5.7 back_and_map VA=0x1210d0000 phys=0x31f0000 size=0x10000 client=0xc1d00001  OK PLACED
>   M7  gpga_obj      va=0x1210d0000 gpga=0x31f0000 cpu_qva=0x77a7e5e49000 obj=8
>   M5.24 GPFIFO      va=0x120064000 phys=0x31f0000 sz=0x8000 client=0xc1e00007  -> map-FAILED
>   ```
>   The **CeUtils scrub channel** (`client=0xc1e00007`, VA `0x120064000`) and a **UVM kernel
>   channel** (`client=0xc1d00001`, VA `0x1210d0000`) both resolve to FB phys `0x31f0000`. The
>   scrub channel's `M5.24` double-mmap FAILED *because the UVM channel already owns that phys*.
> - **Consequence:** the finishPayload page `0x31f8004` is inside the UVM channel's `gpga_obj`
>   `[0x31f0000, 0x3200000)`, so `fb_write(0x31f8004)` lands in the **UVM channel's** host RAM
>   (`cpu_qva 0x77a7e5e49000+0x8004`). The guest reads the **scrub** channel's finishPayload
>   from *its* own (different) mapping → never sees the forge. Hang persists.
>
> **This is the page-reuse / free-realloc aliasing the `#12-L3c` note predicted, now proven
> concretely:** when the scrub channel tears down, its vidmem (`0x31f0000`) is reused by the
> UVM channel — but our FB-phys model lets both VAs alias the same emulated page, so a write
> to "the scrub finishPayload" actually hits the other channel's buffer. **No instrumented
> oracle is needed** — the collision is fully visible in the QEMU log.
>
> **THE FIX is squarely the address table (`mode2_address_table.md`): per-channel-isolated
> VA→FB-phys so two live channels can never alias the same emulated-FB page, plus free/realloc
> lifecycle tracking so a torn-down channel's phys isn't silently reused under a still-polling
> owner.** Once VA→backing is per-channel-correct, the (already-exact) forge writes the right
> RAM and the guest's poll completes. The forge is the *completion policy*; the address table
> is the *delivery*. Recommended order: fix the FB-phys collision first (it likely also removes
> the M5.16 aliasing symptom, since pages stop being shared), then re-enable the forge default-on.

> ### UPDATE 2026-06-18 (cont. 2) — mechanism verified in code+log; isolation test narrows it to CROSS-CLIENT
>
> **Step 1 (mechanism, verified):** `bar1_wpg` (our MRU cache of guest-BAR1-written FB pages,
> which M5.16 uses to resolve the GSP-managed scrub ring) is **never invalidated** — grep shows
> only write + read sites, no clear path. And the guest **does** issue the completing action we
> ignore: `INVALIDATE_TLB`-class + `0x20800a6c`/`0x20800a61` FB-flush/membar control RPCs
> (`status=0x0`, we just ack), including right after the channel free. So we trust a stale
> VA/BAR1→FB cache instead of a refcounted GPGA reference, and drop the guest's membar.
>
> **Step 2 (isolation/coherence test `tests/mode2/chshare.c`):** two channels (streams) in the
> SAME client/VAS → **PASS** (each fills its own 64 KiB buffer with a distinct pattern, no
> contamination; + a shared buffer written by ch1 and read by ch2 is coherent). So the backing
> model is correct *within one client*. The #12 collision is therefore **cross-client / cross-VAS**:
> the CeUtils scrub channel (`client 0xc1e00007`) vs a UVM channel (`client 0xc1d00001`) — and
> `bar1_wpg` + the FB-phys overlays are global, not client-isolated.
>
> **Stale-after-free confirmed by timeline:** UVM channel `0x1210d0000` is **freed at log line
> 362099**, but `nvkvm_m2_ctx_free_drop` only drops bookkeeping (chans/chanbuf/devvas/cvas/
> chanvas) — it does **NOT** release the channel's `m2_fbback`/`m2_gpga`/host-object backing or
> invalidate `bar1_wpg`. So the dead UVM channel's overlay at FB `0x31f0000` persists, and the
> scrub channel's finishPayload (collided onto that phys) reads the dead channel's RAM at the
> teardown poll (after 362099) → the 4 s timeout.
>
> **Fix (the address-table lifecycle, per the agreed model):** on channel/object free, release
> that channel's FB-phys backing (refcounted — freed only when the last VAS/BAR mapping drops,
> never while still referenced) and invalidate the matching `bar1_wpg` entries; honor the guest's
> TLB-invalidate/flush as the membar that gates phys reuse; and key resolution per-client so two
> live clients can't alias one emulated-FB page. Then the (already-exact) forge delivers to the
> right, un-collided backing.

## Symptom

A 2nd CUDA context after the 1st tears down hangs. Markers: `CTX1` runs and exits
cleanly; `CTX2` stops at its **first** `cuda.synchronize()` (after `randn`),
never reaching matmul. dmesg at the hang shows, from **CTX1's teardown**:

```
NVRM: scrubberDestruct:  Timed out when waiting for the scrub to complete ...
NVRM: nvAssertFailedNoLog: pCeUtils->lastCompletedPayload == lastSubmittedPayload @ ce_utils.c:349
```

So it is CTX1's teardown (the CE memory scrubber) that wedges shared driver state,
and CTX2 then blocks on its first GPU op. "2nd context hangs" is the *observable*;
"1st context's scrubber teardown never completes" is the *cause*.

## Two distinct layers (don't conflate)

1. **Wrap-wedge (FIXED, committed `37d15c5`):** the UVM tracking semaphores
   (`0x121000xxx`) hit a double-writer → backward jump → `uvm_gpu_semaphore.c:776`
   / `uvm_channel.c:205`. Fixed by exact-GPA + backward-only parser defer.
   Validated: cup8 byte-exact + PyTorch-heavy, no regression.
2. **CE completion at teardown (THIS doc, open):** a *different* sema, in a
   *different* place, surfaced only after layer 1 was fixed.

## Root cause (proven with guest counters)

An **instrumented** guest `nvidia.ko` (built from full 580.159.04 source, with
`NV_PRINTF` at the `ce_utils.c` destruct) printed the decisive values:

```
destruct #1 PASSES: lastSub=2  hwsema=2  finVa=0x42006c004  bUseBar1=0
destruct #2 HANGS:  lastSub=63 hwsema=0  finVa=0x12006c004  bUseBar1=1
```

- The hanging channel is **`gpfifo 0x120064000`** (`pbGpuVA=0x120000000`), a
  **GSP-managed** UVM CE channel. `chan_exec` shows **`picked_pdb=0x0`** — we
  cannot resolve its pushbuffer address space.
- Because the VAS is unresolvable, we **drain its ring** (`gp_get 0→63`) but
  **never parse/execute its methods**, so its CE `SET_SEMAPHORE` to the
  finishPayload sema is never performed.
- That finishPayload sema is **`0x12006c004`** (= `pbGpuVA + finishPayloadOffset
  0x6c004`), in **vidmem**, `bUseBar1=1`. It reads **0** forever
  (`m0_qemu.log` grep: zero writes to `0x12006c004`).
- `ceutilsDestruct` / `scrubberDestruct` (`mem_scrub.c: _isScrubWorkPending` →
  `ceutilsUpdateProgress` → `READ_CHANNEL_PAYLOAD_SEMA`) wait for that sema to
  reach `lastSubmitted=63`, time out (4 s), assert, wedge.

The *passing* destruct #1 channel (`0x420064000`, `bUseBar1=0`) has its
finishPayload in **sysmem**, which our parser *does* write (`CE_SEM_RELEASE
addr=0x42006c004`) — so no hang. The difference is purely resolvable-vs-not.

The whole night's earlier `0x121000xxx` analysis chased the **wrong sema** (that
is the UVM tracking sema; the CeUtils finishPayload is a separate sema in the
channel's own vidmem buffer).

## Why "can't resolve the pushbuffer VAS"

The channel is `bGspOwned` (`kernel_channel.c`); GSP manages its page tables, so
the CPU-side instance-block PDB reads empty and there is no walkable root. The
binding existed at channel-create (the guest knows the channel buffer's phys via
`memdescGetPhysAddr(AT_GPU)`) but the data plane *threw it away* and tried to
reverse-resolve at exec time — which fails for GSP-managed channels. This is the
exact failure the address-table design removes.

## REFINEMENT 2026-06-17 (re-derived from the overnight `m0_qemu.log`)

The "unresolvable VAS" framing above is **imprecise** — corrected here from the
full candidate dump in the overnight trace (`/tmp/m0_qemu.log` on `vh`):

- The CeUtils gpfifo VA `0x120064000` **DOES resolve via PDB walk.** Both captured
  roots of its VAS (`hVASpace=0xcaf00005`: `0x3114000` from RESERVED_PDES and
  `0x3400000` from SET_PAGE_DIRECTORY) agree it maps to **sysmem phys
  `0x165664000`** (`DIAG vas[2]/vas[3] … eva=0x120064000 -> SYS phys=0x165664000`).
  So the gpfifo ring is in **sysmem**, and the walk is correct.
- `picked_pdb=0` is **not** a resolution failure — it is a **content-gate
  artifact.** `nvkvm_chan_execute` only pins a PDB if the GP entry reads
  **non-zero** (`val != 0`). At the moment of the sweep the ring slot reads `0`
  (an idle/empty GP slot — `val=0x00000000`), so the value-gated pin is **skipped**
  and we fall through to the `bar1_wpg` heuristic, which pins the **wrong** (a
  vidmem) pushbuffer page. The gate conflates "ring momentarily empty" with "wrong
  VAS."
- Proof the channel is otherwise parseable: `M5: CE COPY … gpfifo=0x120064000`
  (line ~136257) — when the ring is non-empty we *do* decode its CE methods.
- Aperture split: the **gpfifo is sysmem** (`0x165664000`) but the **finishPayload
  sema is vidmem** (`bUseBar1=1`, guest counters) — different memdescs in the same
  channel. So the sema must be resolved on its **own** VA (not gpfifo+offset into
  sysmem); under the same VAS its VA resolves to the vidmem phys we must write.

**Corrected failure chain:** content-gate false-rejects the (idle) sysmem gpfifo →
`chan_pdb=0` → `bar1_wpg` heuristic pins the wrong (UVM-tracking) ring → the
CeUtils completion `SET_SEMAPHORE` is never parsed → its vidmem finishPayload sema
is never written → scrubberDestruct times out.

**Implication for the address table:** this is *exactly* the false-reject the
forward table removes — a forward binding (`gpfifo_va 0x120064000 → 0x165664000
sys`, recorded at map/FILL_PTE time) carries no content-gate, so an idle ring never
demotes resolution to a heuristic. The sema's own VA resolves the same way. Still
to confirm by one targeted run: (a) the finishPayload sema VA and what each root
resolves it to (phys+aperture), (b) whether a `FILL_PTE_MEM` (0x801802) /
`INVALIDATE_TLB` (RPC fn 200 / ctrl 0x80180c) carries that sema's binding forward,
(c) whether trusting the PDB-walk gpfifo result (drop the content-gate, or gate on
"resolves" not "non-zero") lets the `SET_SEMAPHORE` parse + complete the scrub.

## CONFIRMED on current HEAD 2026-06-17 (cupctx2 repro, dc5c24c)

Reproduced cleanly with `tests/mode2/cupctx2.c` (create→matmul→destroy ×2) on a
fresh boot, `m2cefwd=on`, guest open-580 rebuilt in-guest (vermagic 6.8.0-117):

```
[CTX1] SYNC OK → RESULT bad=0 C[0]=256 → PASS      # ctx1 matmul correct
[CTX1] cuCtxDestroy (fires CeUtils scrubberDestruct) → CTX DESTROY OK
[CTX2] cuCtxCreate...                               # HANGS (rc=124)
NVRM: scrubberDestruct: Timed out waiting for the scrub to complete the pending work.
NVRM: nvAssertFailedNoLog: pCeUtils->lastCompletedPayload == lastSubmittedPayload @ ce_utils.c:349
```

QEMU-side log at teardown nails the mechanism:
- `chan_exec … picked_pdb=0x0 gpfifoVA=0x120064000` — the content-gate false-reject
  (refined cause above) still fires; the channel falls to the `bar1_wpg` heuristic.
- The channel **is** partially parsed under the heuristic — `M5: CE COPY … gpfifo=
  0x120064000` decodes the scrub copies, and `#12-L3c SEMW va=0x121000010 …` writes
  the **UVM tracking sema** (sysmem; here also handled by the instrumented build's
  backdoor, `SEMW-DEFER backdoor owns …`). So the wrap-wedge layer is contained.
- The **finishPayload sema is never written**: the only event touching its region is
  `DIAG BAR1 WR off=0x12006c -> FB 0x31f006c <- 0xd801` (the *guest* CPU initializing
  it). **No `SEMW`/`SET_SEMAPHORE` ever targets `0x12006c…`** — its release method sits
  in a GP entry the heuristic never reaches. So `lastCompletedPayload` stays < 63,
  scrubberDestruct times out, asserts, wedges → CTX2 hangs.

Decisive corollaries for the fix: (a) the finishPayload's **FB backing is known**
(`0x31f006c`, from the guest's own BAR1 write) and the channel's VAS resolves via PDB
walk — so the only thing missing is *reaching + resolving the release method*; (b) the
fix is to stop the content-gate from demoting a PDB-resolvable channel to partial
heuristic parsing, so the **whole** ring (including the finishPayload `SET_SEMAPHORE`)
is parsed and written. This is the forward-binding / drop-the-content-gate change the
address table generalizes. Repro is now turnkey: `cupctx2_run_guest.sh`.

## The fix

**Clean (do this): the address table** (`mode2_address_table.md`). Record the
channel-buffer binding at channel-create; resolve the finishPayload sema by table
hit; no PDB, no `picked_pdb=0`. The bug dissolves with no special case. The same
pushbuffer decoder that parses `SET_SEMAPHORE` also parses the `MEM_OP`
TLB-invalidate that maintains the table — one piece of plumbing serves both.

**Interim QEMU-side alternative:** when a GSP-managed CE channel drains with
`picked_pdb=0`, locate its buffer in FB via the existing `bar1_wpg` fallback
(the GPFIFO and the sema are in the *same contiguous channel buffer* —
`finishPayload = gpfifo_base − 0x64000 + 0x6c004`), and `nvkvm_fb_write` the sema.
Risk to verify: GPU-VA contiguity ≠ physical-FB contiguity; if the FB pages are
not adjacent, scan FB for the sema's backing page instead.

## Guest-side hack attempts — inconclusive (do NOT trust)

A guest-side force-complete (report completion for `bUseBar1` channels in
`ceutilsUpdateProgress` / short-circuit `channelWaitForFinishPayload`) was tried
to demonstrate the fix end-to-end. It **never landed**: the writes hit the same
unresolvable vidmem wall, and a build/NVOC quirk meant `ce_utils.c` edits past the
first instrumentation never took effect at runtime even after `make clean`
(unresolved — next session: objdump `nv-kernel.o`, or full `rm -rf` re-extract).
The diagnosis above does **not** depend on the hack; it is proven by the printed
counters. The hack is not a fix and was abandoned.

## Reproduction / instrumentation pipeline (reusable)

Bench is on the vast host (`vh` build+runtime, `vg` guest). Build an instrumented
guest driver:

1. On `vh`: `git clone --depth 1 --branch 580.159.04
   https://github.com/NVIDIA/open-gpu-kernel-modules` (network reachable from vh).
2. Apply the `0xFFF508` backdoor to `kernel-open/nvidia-uvm/uvm_channel.c` (needed
   to reach the CE layer) + add `NV_PRINTF` probes to `src/.../ce_utils.c`.
3. **Build IN-GUEST** (24.04 / gcc-13 / glibc-2.38 objtool; a `vh` 22.04
   cross-build fails on objtool GLIBC + a `module_layout` CRC mismatch — the guest
   kernel is a 24.04 build). Tar the source to the guest via the writable
   `nvkvm_src` 9p; `make -j2 modules SYSSRC=/lib/modules/$(uname -r)/build` (+ a
   `/swapf` for OOM safety on 4 GiB). ~10 min; loads natively.
4. **Build gotcha:** incremental `make modules` does NOT reliably relink
   `nv-kernel.o` after an `src/nvidia` edit (the RM combines all objects) → always
   `make clean && make modules`, and verify with a *runtime* marker (NV_PRINTF
   literals are invisible to `strings`).
5. Deploy `nvidia.ko` to guest `~/nvmods` (back up `~/nvmods.good` first), fresh
   boot, run `pt_ctx2`, read `dmesg | grep NVKVM-`.

## UPDATE cont. 5 — forge bench-disproven; the "FB backing known" corollary was wrong

A run with the `#12 FORGE` active (markers present, `cupctx2`, 117-pinned guest)
**still hangs**, and the persisted log (`vh:/tmp/m0_qemu.log`, 30 MB) lets us correct
two earlier claims and reframe the fix.

**Verified, unchanged:** our RAMIN PDB offsets are correct — `NV_RAMIN_PAGE_DIR_BASE_LO
= word 128 = byte 0x200`, `_HI = word 129 = 0x204` (ga102 inherits gm107
`dev_ram.h`), matching `NVKVM_RAMIN_PDB_{LO,HI}_OFF`. The instblk path is not
mis-offset; the instblk at `0x2efa6e000` is simply **never populated** — every channel
logs `M5.14 … PDB empty (GSP-managed)` because our fake GSP (which owns instblk
construction in GSP-client mode) never writes RAMIN+0x200. So "read the instblk PDB"
is a dead end for *all* channels, not just this one.

**The channel names no VAS.** `M5.3 DIAG c56f … hVASpace@28=0x00000000` — confirmed
from both source (`hVASpaceId = NV01_NULL_OBJECT`) and log. The only VAS the client
(`0xc1e00007`) ever created (obj `0x0c`, PDB `0x2efba5000`) is **RM-handle-freed at
M5.49 *before* the channel even allocates** (`hVASpace`-less). So `picked_pdb=0` on
every `chan_exec`; there is no handle to bind to.

**The decisive asymmetry (this is the bug, precisely):** the channel has two
semaphores in *different apertures*, and only one resolves —
- host/progress sema, GPU-VA `0x121000000` → **SYSMEM** `0x14444d000`. Resolves
  (`res=translate`, GPA-backed, already in the address table) → the ring drains, the
  guest's progress sema advances.
- finishPayload, GPU-VA `0x12006c004` → **VIDMEM**. Needs the channel's VAS PTEs to
  translate; we have none → VAS-walk through the only candidate PDB (`0x3114000` =
  the UVM client `0xc1d00001`'s VAS, line 5110) **FAULTs**; the resolver falls to the
  `bar1_wpg` MRU heuristic.

**Correction to cont. 4 corollary (a).** The earlier note "finishPayload FB backing is
known = `0x31f006c`, from the guest's own BAR1 write" is **wrong**. BAR1 off `0x12006c`
→ FB `0x31f006c` is the **gpfifo ring entry at byte `0x6c`** (first page) — a
coincidental digit-match with the finishPayload *GPU-VA* `0x12006c004`. They are not
the same thing. finishPayload lives at **channel-buffer offset `0x8004`** (`gpfifo_va +
0x8004`), i.e. BAR1 off `~0x128004`. The captured BAR1 PTEs for this buffer cover only
off `0x120000–0x1200xx` → FB `0x31f0000–0x31f00xx` (the first page). **No captured PTE
— BAR1 or VAS — ever maps the finishPayload page.**

**Why the forge fails.** It computes `finFB = gpfifo_FB(0x31f0000) + 0x8004 =
0x31f8004` — a *contiguity extrapolation* from the first page. The doc's own interim
section flagged the risk ("GPU-VA contiguity ≠ physical-FB contiguity"); this run
**confirms the risk materialized**: the forge wrote `0x31f8004` for the entire run
(counter climbed `0→39+`) and `CeUtils` still read `0` → timeout → assert → wedge.
The buffer is provably **non-contiguous** — the host sema is sysmem and the
finishPayload is vidmem, so they are *separate memdescs*; `gpfifo_FB + 0x8004` is the
wrong page.

**Reframed fix (still the address table, now sharper).** The write authority for the
finishPayload is the CE `SET_SEMAPHORE` method targeting GPU-VA `0x12006c004` in the
channel's VAS — which we cannot resolve. But the guest *also* maps the same memdesc
through **BAR1** for its own CPU read (`memmgrMemDescBeginTransfer(USE_BAR1)` in
`channelReadChannelMemdesc`). Both tables point at the *same* finishPayload FB page.
The BAR1 table is the one we **can** populate authoritatively (the guest writes BAR1
PTEs; we snoop them) — so the principled fix is: **walk the BAR1 page table for the
finishPayload page's BAR1-VA to get its true FB, then write there** — *not* extrapolate
`+0x8004` from the gpfifo. Open sub-question gating this: the guest's `USE_BAR1`
transfer mapping for the finishPayload is **transient** (created per-read during the
poll, torn down after), so at method-execute time there may be no live BAR1 PTE for it.
If so, the table must record the finishPayload memdesc's FB at **channel-buffer
construction** (the `c56f`/memdesc RPC carries the sema memdesc) and resolve by table
hit at drain — exactly the "record the channel-buffer binding at channel-create" clean
fix above, with the emphasis that the *sema* memdesc (not just the gpfifo) must be the
recorded unit, since the two are not contiguous.

Net: the clean address-table fix is unchanged in direction but the *unit of binding*
is corrected — bind each **channel-buffer memdesc separately** (gpfifo, sysmem host
sema, **vidmem finishPayload**), never assume one contiguous span. The `bar1_wpg`
+`+0x8004` interim is disproven and should be retired for finishPayload.

## UPDATE cont. 6 — both cheap hooks closed; (C) sysmem-aperture is guest-hardcoded

Re-converged after re-reading the in-tree `#12 NOTE` (line ~1677) + the open RM
scrubber init. Two QEMU-side "cheap" hooks are **both structurally closed**, and the
clean sysmem angle is **not ours to flip**:

- **Write emulated-FB (the forge): dead.** The guest reads the finishPayload through a
  **non-trapping KVM memslot** whose backing is not coherent with `nvkvm_fb_write`
  (the promoted/GPA-window page ≠ the emulated-FB `g_malloc`), so even a correctly
  *located* write would not be seen. (And cont. 5 showed the forge's *location* is also
  wrong — `gpfifo_FB + 0x8004` assumes a contiguity the sysmem-hostsema/vidmem-fin
  split disproves.)
- **Intercept the poll read: dead.** Same non-trapping memslot — the finishPayload poll
  does **not** trap (BAR1 reads aren't traced; the 100k trapped reads are the BAR0
  PRAMIN/CRASHWIN window, which the guest stops using before the wait). Nothing to hook
  on the read side.
- **(C) make finishPayload land in sysmem: not GSP-controllable.** The aperture split is
  hardcoded in the guest driver: general CeUtils passes `_NO_BAR1_USE_TRUE`
  (`mem_mgr.c:4134`) → `bUseBar1=FALSE` → **sysmem** (this is the sibling that resolves
  and works); the memory scrubber passes `_VIRTUAL_MODE_TRUE` with no `_NO_BAR1_USE`
  (`mem_scrub.c:154`) → `bUseBar1=TRUE` → **vidmem**. `bUseBar1` is purely
  `FLD_TEST_DRF(_NO_BAR1_USE, allocFlags)` — no GPU-cap / GSP input — so our fake GSP
  cannot nudge the scrubber onto the sysmem path without modifying the (unmodified)
  guest driver.

**Net.** There is no patch-sized fix. The crux is singular and unavoidable: **resolve
the GSP-managed, `_VIRTUAL_MODE` scrubber channel's VAS** so its own buffer VA
`0x12006c004` translates to FB. The scrubber issues its CE *copies* through this VAS and
we already forward those (`CE COPY … out=…(phys)`), so the data plane works; only the
channel's *self-referential* finishPayload sema is unresolvable, because the VAS it runs
in has `hVASpace=0`, an empty (GSP-owned) instblk, and a transient VAS handle
(`0x2efba5000`) freed before the channel ran. Once that PDB is known, the fix is either
(A) coherent-write the translated FB through the promoted memslot backing, or (B)
forward the `SET_SEMAPHORE` to the host CE alongside the copies. Both are real work;
neither is a heuristic. This is the address-table directive's "PDB = communication, the
channel must carry its binding" applied to a channel that deliberately discards its
binding — the open question is what authoritative signal *does* carry the scrubber
channel's PDB (candidate: the instblk *would*, if our fake GSP synthesized RAMIN+0x200
from the channel-alloc memdescs at construction — making us populate what real GSP
populates).

## UPDATE cont. 7 — ROOT CAUSE: we key VAS by client; HW keys by PDB (instance block)

The cheap-hook dead-ends in cont. 5/6 were a symptom. The real root cause, found by
walking the *map* failures instead of the sema:

**The `map-FAILED` is deterministic, not racy, and not a host-ioctl error.** Every
`M5.19 fwd-map pushbuffer … client=0xc1e00007 -> map-FAILED` (42×) bails at the **first
line** of `nvkvm_m2_back_and_map_sys`: `hDev = m2_devvas[client].dev; if (!hDev) return
false;` — there is **no device/VAS registered for client `0xc1e00007`** (confirmed:
`M5.7 grmapper: no dev/vas for client 0xc1e00007`; every `M5.49` drop shows
`devvas=0 cvas=0 chanvas=0`). No `M6.5 back_sys …` line is ever emitted for it, proving
it returns *before* any host RM call. The **identical** VA→GPA
(`0x120800000→0x155e00000`) mapped fine under client `0xc1d00001` (`-> MAPPED`), so the
mapping is valid and shareable — just unreachable under the scrub client's key.

**Why the scrub client owns no VAS — the keying divergence (open-RM ground truth).**
`kernel_channel.c:1030`: `pKernelChannel->hVASpace =
pKernelChannel->pKernelCtxShareApi->hVASpace;` — a channel's effective VAS comes from
its **KernelCtxShare** (subcontext, under the TSG), **not** from the channel's own
`hVASpace` param (which is `0` here). With `hVASpace=0` and no explicit ctxshare, the
implicit TSG binds the **device's default VAS**. So the VAS is owned by the
**ctxshare/TSG and identified by its PDB (instance block) — client-independent and
shareable.** Hardware roots every channel's translation at `RAMIN+0x200` (the PDB); the
client handle is irrelevant to translation.

We do the opposite: `m2_devvas[**client**]`. A channel whose VAS is inherited/shared
(not owned by its own client handle) is **invisible** to a per-client lookup → `hDev=0`
→ everything for that channel "doesn't exist." **The 42 `map-FAILED`s and the
unresolvable vidmem finishPayload are the same bug.**

**PDB chase result (cont. 7): the scrub channel's true root is not observable in our
state.** No captured PDB correctly roots its own buffer (`gpfifo 0x120064000 → real FB
0x31f0000`, or the finishPayload): `0x2efba5000` (its freed explicit VAS) FAULTs;
`0x2efa6c000` (sibling) maps to a sparse/wrong page; `0x3114000` (UVM) resolves only the
**sysmem** host-sema; `picked_pdb=0` otherwise. There is **no `PDB=`, `SET_PAGE_DIRECTORY`,
or page-table write** that establishes the device-default VAS for this channel. It is
created GSP-side and our fake GSP never models it, so its root was never materialized in
our world (even though the guest's PMA allocated the page directory — we never associated
that allocation with this VAS).

**Fix (three parts, HW-faithful):**
1. **Key the VAS table by PDB (instance-block root), not by client** — a global
   `pdb → {host device, host VAS, page-table view, isolate}` map.
2. **Resolve a channel's VAS via the ctxshare/TSG → PDB chain** (mirroring
   `kernel_channel.c:1030`), with `hVASpace=0` → the **device-default VAS** — never via
   the client handle. Channels/clients sharing a ctxshare share one entry → one isolate.
3. **Model (or capture) the device-default VAS's PDB**, since cont. 7 shows it is not
   observed for this channel: our fake GSP must create/assign it (as real GSP does) or
   capture the guest PMA's page-directory allocation and bind it to the device-default
   VAS. Then `back_and_map_sys` finds the shared VAS and **reuses** the existing host
   placement (generalize the `mst==0x51` ALREADY-MAPPED path to "shared VAS, reuse"),
   and the finishPayload VA `0x12006c004` walks through the now-known PDB.

This is the address-table-of-truth (`mode2_address_table.md`) stated precisely: one
table per VAS keyed by PDB, channels finding their VAS through the instance-block/
ctxshare chain like silicon — never through the client handle.

## UPDATE cont. 8 — the fix: a QEMU-owned "system VAS" + coherent write (supersedes cont.5)

Co-designed with the user; this supersedes two earlier errors and names the build.

**Correction to cont. 5 (the finishPayload IS contiguous).** cont. 5 claimed the
finishPayload was a *separate, non-contiguous* memdesc. Wrong — it conflated two
different semaphores: the channel's **host/progress sema** (genuinely SYSMEM,
`0x14444d000`, VA `0x121000000`) with the **finishPayload** (VIDMEM, VA `0x12006c004` =
`gpfifo_va + 0x8004`). The instrumented guest's backdoor reported the finishPayload
region at **FB `0x31f8000`** (`= gpfifo_FB 0x31f0000 + 0x8000`), i.e. **contiguous**
with the gpfifo inside the same 64 KB channel-buffer object. The forge's location
(`0x31f8004`) was therefore **correct**; it failed only on coherence (below). The FB was
knowable all along.

**Exhaustive PDB check (settles cont. 7's open question).** No captured GMMU VAS maps
the scrub buffer to its real backing: the sibling's `0x2efa6c000` has VA `0x120064000`
only as a **sparse/identity reservation** (`fb=0x64000 val=0`), `0x3110000`/`0x2efba5000`
FAULT, UVM's `0x3114000` resolves only the sysmem host-sema. The buffer's *real* FB
(`0x31f0000`) is reachable **only via the BAR1 VAS** (`bar1_pdb`, statically
pre-allocated) — because the scrubber is `bUseBar1=TRUE` and the guest reaches it through
BAR1. So: don't chase the absent GMMU channel PDB; the address is already observable.

**Root cause, final form.** `hVASpace == NV01_NULL_OBJECT (0)` → the channel uses the
**device-default VAS** (via ctxshare, `kernel_channel.c:1030`). For kernel-internal /
GSP-managed work that default is a **shared kernel/system address space**, and it is
**GSP-managed — i.e. ours to define.** Today we key per *client* (`m2_devvas[client]`),
so the scrub client (which owns no VAS of its own) is invisible → `no dev/vas` →
map-FAILED + unresolvable finishPayload, all one bug. FB `0x31f0000` (incl. the
finishPayload at `0x31f8004`) is actually backed by UVM client `0xc1d00001`'s `gpga_obj`
obj 8 (`cpu_qva=0x75d18c023000`), **aliased** by the scrub channel — and a (disproven)
`#12 LIFECYCLE release … de-alias` even tore that backing out mid-run.

### The build: a QEMU-owned system VAS, forward-populated, keyed by a minted PDB

1. **`m2_system_vas` (per kernel device).** A QEMU-owned default VAS with a **PDB we
   mint** (we are GSP). One per kernel device (NOT one GPU-wide — avoid cross-device VA
   aliasing). Fields: `{ device, client, pdb_synth, va→fb interval map }`.
2. **Resolve `hVASpace=0` → `m2_system_vas[device]`.** In the channel VAS-resolution
   chain (`nvkvm_chan_own_pdb_rs`), when the channel names no VAS and `cli_vas` doesn't
   resolve, fall to the system VAS for the channel's device. Key the table + host
   isolate/device on the **minted PDB**, not the client — so sibling kernel channels
   share one entry (matches HW: instance-block PDB, client-independent).
3. **Forward-populate from observation.** We already derive `VA 0x120064000 → FB
   0x31f0000` for the scrub channel (`M5.24 GPFIFO double-mmap`, from the BAR1-written
   page). Record that interval into the system VAS regardless of host-placement success.
   Then `VA 0x12006c004 = base + 0x8004 → FB 0x31f8004` falls out by contiguity within
   the 64 KB buffer object. (Address-table directive verbatim: forward-populate, never
   reverse-resolve.)
4. **Give the system-VAS client a device** so `back_and_map_sys` stops bailing at
   `hDev=0`: resolve the scrub client to the system-VAS device, and treat an existing
   placement at the same VA→GPA as **reuse** (generalize the `mst==0x51` ALREADY-MAPPED
   path) instead of a FIXED-map collision → kills the 42× map-FAILED.
5. **Coherent write (separate, smaller fix).** Resolution finds `0x31f8004`; completion
   must write the **host page the guest actually reads**. The guest reads the
   finishPayload via BAR1; per the in-tree note that path is a **non-trapping memslot**
   whose backing ≠ the emulated-FB `g_malloc` page the forge wrote. Fix: route the
   completion write through the **same overlay/memslot backing** (obj-8's `cpu_qva`, via
   `nvkvm_fb_host_overlay`), and **do not de-alias** a shared kernel buffer while a
   channel still references it. Write value = the channel's `lastSubmittedPayload`
   (already tracked as `c->fin_payload`); advance-only, never backward.

### Open questions to settle at implementation (bench-verifiable)
- **Coherence path (load-bearing):** does the guest's BAR1 read of `0x31f8004` route
  through `nvkvm_fb_host_overlay` (then fb_write is already coherent and step 5 is
  trivial), or through a separate non-trapping KVM memslot (then we must write *that*
  backing)? One instrumented read-trap confirms which.
- **PDB minting:** any value is fine for QEMU-internal resolution, but if we later
  **forward** the scrub CE to the host (option B), the host VAS needs real page tables —
  out of scope for the in-QEMU completion, which is all #12 needs.
- **De-alias safety:** ensure the disproven `#12 LIFECYCLE release` de-alias path is gone
  / gated so it can't yank a shared kernel buffer's backing.

This is the address-table-of-truth (`mode2_address_table.md` §13) made concrete for the
`hVASpace=0` kernel case: one QEMU-owned VAS per kernel device, minted PDB, populated by
observation, keyed by PDB — exactly the Rust core's `HashMap<PdbRoot, IntervalMap<…>>`.

---

## UPDATE cont. 9 (2026-06-20) — IMPLEMENTED + BENCHED: #12 is LAYERED. L1+L2 fixed, L3 found.

Built the fix and ran it (`cupctx2`, `NVKVM_M2TRACE=1`, `m12_forge_orch.sh`). The result:
the documented finishPayload root cause is **fixed**, and the hang **moved twice**, exposing
#12 as a *multi-layer GSP re-acquire* problem — each layer a stage of a teardown+reboot the
one-shot fake-boot never exercised.

**Correction to cont. 3/8's premise.** BAR1 is **pure MMIO** (`memory_region_init_io`,
`nvkvm_aperture_ops`) — there is NO memslot over it, so every guest BAR1 read traps
`nvkvm_baraperture_read` and walks the same `bar1_pdb` as `nvkvm_fb_write`. Forge-write and
guest-read are therefore **coherent by construction**; the "memslot-served / no-trap" claim
was a false read of the 2000×-consecutive-spin heuristic (it resets on any interleaved read,
so a real poll loop never trips it). The coherent-backing step is **not needed**.

**Layer 1 — finishPayload split (the real root cause). FIXED, commit `c6b4150`.** The forge
resolved its target FB from the *global* `chan_gpfifo_phys`/`chan_gpfifo_bar1off` (the M5.16
content-heuristic MRU scan over `bar1_wpg`), stomped per doorbell by whichever channel last
decoded plausibly. Persisted-log proof: the *same* scrub channel (`0xc1e00007`,
`gpfifo 0x120064000`) alternated `finFB 0x31f8004 ↔ 0x3138004`, **splitting its monotonic
payload across two FB pages** so the guest's real sema (VA `0x12006c004` → FB `0x31f8004`)
only ever saw a subset and never reached `lastSubmitted` → 4 s `scrubberDestruct` timeout +
assert + wedge. Fix = the address-table principle scoped to one channel: forward-populate the
finishPayload FB **once**, on the first doorbell the channel advances (its just-written ring
page is freshest in `bar1_wpg`, so the MRU scan picks the *right* page), cache it in
`chans[].fin_fb`, **pin it** — never heuristically re-resolve. Verified: 58 forge writes all
to the single page `0x31f8004`; one `FORGE-RESOLVE … (pinned)`; **no scrub timeout/assert;
`[CTX1] CTX DESTROY OK`** (clean teardown — previously asserted+wedged here).

**Layer 2 — GSP re-boot keeps WPR2 down. FIXED, commit `bf36f63`.** With L1 fixed the hang
moves to `[CTX2] cuCtxCreate`. `cuCtxDestroy` of the *last* context sends **fn-47 UNLOADING**
(a GPU-idle release while the module stays loaded — not just rmmod) → `gsp_suspended` + WPR2
down. The next `cuCtxCreate` re-acquires: it **reloads the GSP falcon image** (`DMATRFCMD`)
then `STARTCPU`s to re-boot. But the teardown-phase gate (`teardown = gsp_suspended`) treated
that single re-boot `STARTCPU` as a *bare trailing-teardown* STARTCPU and **kept WPR2 down**,
so the guest waited forever for a `GSP_INIT_DONE` that never came. The gate assumed *two*
post-UNLOADING STARTCPUs (defensive-unload then boot); a re-acquire has only *one*, and it
must boot. Fix: latch `gsp_reloaded` when the guest issues a `DMATRFCMD` transfer **while
suspended** (the unambiguous genuine-reboot signal); `teardown = suspended && !reloaded`.
Verified: a 2nd `M3: GSP STARTCPU → FWSEC ran, WPR2 up` now fires and the re-boot progresses.

**Layer 3 — SEC2 Booter Load on re-acquire (OPEN, next).** After WPR2 re-raises, the guest
re-establishes WPR2 through the **SEC2 Booter** (the "separate SEC2 path we don't model" the
STARTCPU comment already flagged): it DMA-loads the Booter ucode into SEC2 (`0x84011c`
stepping `0x4000,0x4100,…` = IMEM/DMEM; `0x840118` `DMATRFCMD` transfers), runs it (SEC2
`STARTCPU`), then **polls `0x8403c0` / `0x8400f4` for a Booter-done signal our emulation never
produces** → hang. Decisive asymmetry: the **first** boot touched SEC2 (`0x840xxx`) **zero**
times (it used the GSP-falcon FWSEC fake directly); only the **re-acquire** uses SEC2. So the
remaining #12 work is a scoped sub-project: fake the SEC2 Booter Load completion (read the
driver's SEC2 Booter completion check; set the polled status/mailbox to its success values on
SEC2 `STARTCPU`; keep WPR2 up), analogous to the original M3 GSP-boot faking. Rejected
alternative: prevent the fn-47 idle-release to keep the GSP alive across contexts (hacky,
fights the guest driver, doesn't generalise to real teardown).
