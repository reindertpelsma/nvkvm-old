# Mode-2 #12 — 2nd-CUDA-context hang: root cause (CE completion sema)

Status: diagnosed 2026-06-17 (root cause proven end-to-end via an instrumented
full-source guest driver). The wrap-wedge *layer* is fixed and committed
(`37d15c5`); the CE-completion *layer* documented here is the remaining blocker.

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
> - **The guest reads finishPayload via a non-trapping MEMSLOT, not the FB page we write.**
>   In the run, `DIAG BAR1 RD` fired **0** times while `DIAG BAR1 WR` fired **1032** — the
>   guest *writes* the channel region through the trapping BAR1 path but *reads* finishPayload
>   from a RAM memslot. Our `nvkvm_fb_write(fin_fb,…)` never reaches that backing. Proof the
>   page is wrong, not just lagging: the resolved page `0x31f8004` reached the **full** count
>   (45) yet the guest still timed out.
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
> (the oracle that prints the finishPayload CPU-VA + the value it reads each poll) to nail the
> exact read backing in one run, rather than guessing QEMU-side. Forge code lives in
> `nvkvm_chan_execute`'s doorbell loop (the `#12 FORGE` block + `c->fin_payload` +
> `chan_gpfifo_bar1off` + `bar1_wpg[].off`), gated behind `m2trace`.

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
