# Mode-2 execution-forward keystone — implementation plan (2026-06-10)

Branch: `consolidation`. Goal: get the host GPU to actually RUN the guest's GR context-init work so a
REAL completion fires, the guest's `MC_SERVICE_INTERRUPTS` poll is satisfied for real, and
`cuCtxCreate` completes — then drive `matmul` end-to-end. No faking; verified by numeric correctness.

This plan follows the governing rules in `mode2_cuctxcreate_resume.md §0.3` (map-vs-stub) and §0.7
(real completion via reused Mode-1 #127 poll), and the user's interrupt principle: *userspace polls,
not interrupts → for any op a real host GPU would interrupt on, QEMU polls the host fd and forwards.*

## Status entering this plan
- ✅ M8.4: cuCtxCreate `rbp=0` crash FIXED + verified (GR-alloc reply rbp-restore).
- ✅ M5.30: UVM/GR VAS root captured from `SET_PAGE_DIRECTORY` (0x801813) — the missing PDB source.
- ⛔ Blocker: `nvkvm_m2_exec_doorbell` (M5.9) fires 0× for the GR channel because
  `nvkvm_m2_populate_cvas` → `nvkvm_chan_own_pdb` returns 0 (GR-VAS root not resolvable). So the GR
  work is never mapped into the host VAS, never run, never completes → `MC_SERVICE_INTERRUPTS` spins.

## Design decision: authoritative PDB from the instance block (not the snoop chain)

The page-table CONTENTS live in our FB/GPGA (CPU-RM writes PDEs/PTEs via BAR2 — incremental MMIO,
always current). Only the ROOT pointer is missing, because in GSP-client mode the channel instance
block's `NV_RAMIN_PAGE_DIR_BASE` is written by GSP, not the CPU-RM — and we are GSP. Therefore:

**When we capture a VAS root (SET_PAGE_DIRECTORY 0x801813, and/or VASPACE_COPY_SERVER_RESERVED_PDES
0x90f10106), write it into the emulated channel instance block's PAGE_DIR_BASE field** (the GSP job),
and make PDB resolution read it from the instblk (as `nvkvm_bar2_translate` already does for BAR2).
This gives one authoritative source of truth and removes the fragile `client→m2_devvas→chan_vas[]`
lookup that returns 0. Keep `chan_vas[]` as a fallback during bring-up.

## Steps (each = a commit + fresh-boot verification; m2exec-gated; DEBUG logs gated)

### Step 1 — Resolve the GR-VAS root authoritatively (unblock populate_cvas)
1a. Add one DEBUG log in `nvkvm_chan_own_pdb` dumping the resolution chain (chan_client, matched
    m2_devvas vas, chan_vas pdb) so we SEE why it returns 0 for 0xc1d00003 (timing vs absence).
1b. On `SET_PAGE_DIRECTORY` (M5.30) capture, also write `physAddress` into the owning channel's
    emulated instance block `PAGE_DIR_BASE` (and record aperture). Add a `nvkvm_chan_instblk_pdb()`
    that reads it back (mirror of `nvkvm_bar2_translate`'s instblk path).
1c. Make `nvkvm_chan_own_pdb` try, in order: instblk PDB → M5.30 captured root for the client's VAS →
    existing chan_vas[] scan. Return the first that yields a valid walk.
- **VERIFY:** fresh boot, `populate_cvas` no longer logs `no own PDB`; it logs `runs=N backed=M` with
  M>0 (leaves enumerated + backed). No regression to boot/cuInit. COMMIT.

### Step 2 — Confirm the GR working set is mapped into the host VAS (link 2)
2a. With populate_cvas running, confirm the GPGA double-mmap (`nvkvm_m2_leaf_flush` →
    `nvkvm_m2_gpga_obj`/`back_and_map`) maps the GR ctx buffers + GPFIFO + pushbuffers + completion
    semaphore into the host channel VAS at the guest VAs (st=0x0, not 0x51 self-promote collision).
2b. Cross-check against the M5.31 PROMOTE_CTX buffer list (MAIN/PATCH/etc.) — every referenced leaf
    resolves in the host VAS.
- **VERIFY:** QEMU log shows each GR leaf mapped st=0x0; no `dmaAllocMapping` faults for GR. COMMIT.
  (Do NOT ring yet.)

### Step 3 — Host os-event plumbing: reuse Mode-1 #127 poll (link 5/6, before ring)
3a. Ensure the guest's GR-completion OS-event (NV01_EVENT_OS_EVENT, snooped fn=103 class 0x0079) is
    forwarded to a host eventfd via the isolate (Mode-1 already does this).
3b. Arm that host eventfd in the isolate poll set via `nvkvm_isolate_poll` (the #127 ABI). On
    `ISOLATE_RESP_POLL_EVENT`, deliver to the EMULATED guest via GSP `POST_EVENT` (nvkvm_gpu_emul
    M8.38) — NOT VQ_EVT (stock guest has no nvkvm module).
3c. RACE-GUARD (user requirement): the host eventfd must be level-readable until consumed so a
    completion that fires between arming and ppoll is not lost. Fable-verify this property + that
    POST_EVENT delivery can't drop a wake (mirror #127's "re-arm + re-fire" recovery).
- **VERIFY (no ring yet):** arming + a manual host signal delivers a POST_EVENT to the guest (unit-
  style). COMMIT.

### Step 4 — Ring the host doorbell (link 3) — THE WEDGE-RISK STEP, gate it
4a. Gate the ring on "GR working set fully mapped for this channel" (step 2 complete) — a premature
    ring faults the host GPU → cuInit 999 / host wedge.
4b. On the guest GR doorbell: translate vChid → host work-submit token, write the host USERMODE
    doorbell (primitives exist: M5.8 doorbell_setup). Keep `chan_execute` faking OFF for GR under
    m2exec so a green can ONLY come from the host.
- **VERIFY:** `ssh vh nvidia-smi` shows real GR utilization during the run; the completion semaphore
  transitions 0→nonzero by a GPU write (CRASHWIN), NOT by QEMU. Host not wedged. COMMIT.
  Recovery if wedged: `rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia; modprobe nvidia` (or
  `vastai reboot instance`).

### Step 5 — MC_SERVICE_INTERRUPTS satisfied for real → cuCtxCreate completes
With steps 3–4, the host completion fires → #127 poll → POST_EVENT → guest services the interrupt →
`MC_SERVICE_INTERRUPTS` returns serviced → cuCtxCreate returns.
- **VERIFY:** `ctx_probe`/`cup2` prints `CTX OK` with NO uprobe bridge, NO pbmap, NO credit-shortcut.
  COMMIT. (This is the real cuCtxCreate milestone.)

### Step 6 — matmul end-to-end + correctness gate (the legitimacy proof)
`matmul_pause 8` → `matmul_pause 64` → small LLM. The result must be NUMERICALLY CORRECT and host
nvidia-smi must show util. Correct = every simulate we did was legitimate (kernel-internal only);
wrong/idle = something was faked → revert per §0.3. COMMIT the milestone.

## Guardrails (carry the whole way)
- Each step: committed, builds, fresh-boot no-regression, m2exec/DEBUG-gated, deleted if wrong.
- Serial GPU only; kill stale QEMU/stub; fresh boot per clean run; stage test files AFTER boot.
- NEVER accept a green guest log alone — verify real host work (nvidia-smi util + GPU-written sema).
- If a fix would fake a value the guest userspace consumes, STOP (slop signal) and surface it.
- Use Fable 5 for bounded byte/ABI/race analysis; verify its output against hardware before building
  (its byte-diff was right; its open-ended source-trace once over-committed to a wrong theory).

## PROGRESS LOG (2026-06-10, autonomous run)

- **Step 1 DONE (with caveats):** M5.32 — `chan_own_pdb` now also tries the channel's own
  `hVASpace` vs `chan_vas[]` (M5.30-populated) + the instblk PDB. HW: `populate_cvas` resolves
  `pdb=0x3114000` and walks **26 leaves** (was `no own PDB` bail). Two caveats:
  - **Flaky:** resolution is timing-dependent — some boots the VAS isn't captured before
    `populate_cvas` runs → `no own PDB` again. **TODO: make deterministic** by having fake-GSP write
    the captured SET_PAGE_DIRECTORY root into the channel instance block (`PAGE_DIR_BASE`, RAMIN+0x200)
    so `chan_pdb` is always authoritative (Step 1b, not yet done).
  - **`backed=0` is BENIGN:** the GR working set is **already mapped** at `0xc7c0`-alloc time
    (M6.5 sysmem leaves return `st=0x51 ALREADY-MAPPED`; M7 R2 vidmem `gpu_mapped=1 st=0x0`). So
    Step 2 (map the working set) is effectively already satisfied — NOT the blocker.
- **Real frontier = Step 4 (the RING).** The host GPU has the buffers mapped but is **never told to
  run the work** — `exec_doorbell` doesn't ring → `nvidia-smi` shows **0% util** → no real GR
  completion → `MC_SERVICE_INTERRUPTS` hangs (or flakily self-terminates into a HOLLOW `CTX OK` with
  no compute). cuCtxCreate "passing" today is a hollow pass; the real proof is matmul, which needs
  the host to actually execute. So the next move is Steps 3+4: poll the completion (reuse #127) and
  **ring the host doorbell** (wedge-risk — gate on working-set-mapped, keep `chan_execute` faking OFF).
- **M8.4 (crash fix) remains solid + verified.** M5.30 + M5.32 committed.

## Stop-and-report forks
- Step 4 ring wedges repeatedly / needs `vastai reboot` → report.
- A required completion turns out NOT to come from a host-pollable fd (host wouldn't interrupt) →
  rule question, report.
- matmul (step 6) is wrong despite a real host run → content-coherence fork, report.
