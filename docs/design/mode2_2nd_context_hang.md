# Mode-2 #12 — 2nd-CUDA-context hang: root cause (CE completion sema)

Status: diagnosed 2026-06-17 (root cause proven end-to-end via an instrumented
full-source guest driver). The wrap-wedge *layer* is fixed and committed
(`37d15c5`); the CE-completion *layer* documented here is the remaining blocker.

> ### UPDATE 2026-06-17 (bench-proven, DEFINITIVE) — it's a missing address-table entry + foreign-VAS aliasing, NOT synthesis
>
> Two earlier framings here were **wrong** and are retracted: (a) the narrow "pin the
> channel's own VAS" fix — bench-insufficient; (b) the "GSP-composed empty ring, must
> synthesize the completion" conclusion — an **artifact**: pinning a non-zero PDB *disabled*
> the `bar1_wpg` fallback, so the channel showed "0 methods" only in that one experimental
> run. On **normal HEAD** the channel parses fine. The real cause, proven by an
> `m2trace` capture + log mining on normal HEAD:
>
> **The stuck channel is `client=0xc1e00007` (a UVM CE channel, gpfifo `0x120064000`).** It
> releases its completion semaphore to **`va=0x121000010`** with **incrementing payloads
> 1,2,3,4…** — a perfectly healthy guest-side release; the methods ARE parsed
> (`m=0x0240/0x0244 d=0x21000010 /0x0248`). The bug is purely **where we write it**:
> ```
> #12-L3c SEMW va=0x121000010 phys=0x12bb86010(sys) old=0x33 new=0x1 client=0xc1e00007 res=translate chan_pdb=0x3114000
> #12-L3c PROBE cli_vas[0] client=0xc1e00007 pdb=0x2efba5000 -> FAULT        ← its OWN VAS has NO mapping for the sema
> #12-L3c PROBE chan_vas[2] client=0xc1d00001 pdb=0x3114000  -> 0x12bb86010  ← a FOREIGN client's VAS aliases the VA
> ```
> `0xc1e00007`'s real mapping for `0x121000010` is **absent from our table** (its own VAS
> faults; the shared `0xc1e00008` VAS resolves it to garbage `0x1000010`). So the write
> falls back (`res=translate`) to a **foreign** client's VAS (`0xc1d00001`, pdb `0x3114000`)
> that aliases the same VA onto a *different* phys (`0x12bb86010`) holding `0xc1d00001`'s own
> sema (already `0x33`). `new=1` < `0x33` ⇒ **backward** ⇒ the `#12-L3` backward-defer
> (`37d15c5`) refuses the write ⇒ `0xc1e00007`'s completion never advances ⇒ its teardown
> wait (`ceutilsDestruct`→`channelWaitForFinishPayload`) times out ⇒ device `StateUnload`
> botched ⇒ CTX2 cold `StateLoad` hangs.
>
> **Chain:** missing table entry → foreign-VAS aliasing fallback → backward write → deferred → wedge.
>
> Contrast the *working* sibling `client=0xc1e00008`/gpfifo `0x420064000`: its finishPayload
> `0x42006c004` resolves under its OWN VAS (`res=cli_vas pdb=0x2efa6c000`) and writes cleanly.
> And it explains the LLM/cup8 passing: a single context has no *second* UVM client whose
> sema VA aliases onto a foreign, higher-valued sema page, so the backward-defer never fires.
>
> **The fix is the address table, exactly as designed:**
> 1. **Populate** `va=0x121000010` → `0xc1e00007`'s *own* sema-pool phys (the missing entry).
>    Source: the RPC/UVM map that establishes the semaphore-pool mapping (PDB-read can't —
>    its own captured root `0x2efba5000` faults on it). This is the open implementation
>    question: *which* transport maps UVM's `uvm_gpu_semaphore` pool, and snoop it.
> 2. **Never alias to a foreign client's VAS** on a sema-write miss — per the directive's
>    "MISS = FAULT, no backwards/heuristic resolve." The `res=translate` foreign-VAS
>    fallback IS the bug; with (1) in place it becomes dead, and on a true miss it must
>    fault, not write someone else's page.
>
> So: **no synthesis.** The guest releases the completion correctly; we mis-route it to an
> aliased foreign page because one mapping is absent and the resolver guesses instead of
> faulting. See `mode2_address_table.md` (this is its canonical motivating bug).

The clean fix is the address table — see `mode2_address_table.md`.

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
