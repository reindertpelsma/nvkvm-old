# CLAUDE.md — repository navigation

This file is a **map**, not a rulebook. It points to the authoritative docs; read those for
detail. Keep it short and navigational — do not inline implementation specifics that will rot.

## What this project is

`nvkvm` — WSL2-style NVIDIA GPU **ioctl/RPC forwarding** for KVM/QEMU guests on commodity
hardware, so an unprivileged host process drives a real host GPU on behalf of a guest. Two modes:

- **Mode 1** — guest forwards its NVIDIA *userspace ioctls* to a host stub that replays them on the
  host `/dev/nvidia*`. Mature: CUDA, multi-process, graphics/Vulkan, NVENC, ~host parity. Must not
  regress.
- **Mode 2** — guest runs the **stock** NVIDIA kernel driver against an **emulated GPU + faked
  GSP** (`src/qemu/nvkvm_gpu_emul.c`); we recover the guest's intent and forward *real* compute to
  the host. North star: `cuCtxCreate → first compute → matmul`, numerically correct. In progress.

Start here: `README.md`, `PLAN.md`, `docs/ARCHITECTURE.md`, `docs/MILESTONES.md`.

## ★★★ THIS REPO'S ROLE CHANGED (2026-07-29) — read before you touch anything

This is the **research artifact**. The **product** is now `kayfabe`, a clean-slate Rust
rewrite at **`/workspace/nvkvm-rs`** (GitHub `reindertpelsma/kayfabe`). This repo is not
history and is not dead — it is the **standing differential oracle** for that rewrite, and it
is maintained for exactly that purpose.

Why it still matters: it was **rebuilt from source on fresh hardware on 2026-07-29** and
reproduced `cuCtxCreate → 2048² matmul` at **`bad=0 maxerr=0` on a STOCK, unpatched guest**
(ladder: `cup2` → `cupctx2_min` (#12) → `cup8` → `cup8_iter` (#13)). It is the only
implementation a real NVIDIA driver has ever accepted end-to-end, so it can answer questions
no amount of Rust-side testing can.

**New, and load-bearing:**
- ★ **`traces/mode2_c_reference/`** — the committed §6 replay captures (~11 MB zstd, dense,
  `n_errors=0`). `cap1_coldboot_hermetic` (359 062 records) is the **only trace a replay can be
  CLOSED over**; `cap2b` is the negative trace (378 GSP elements read out of arbitrary guest
  RAM and answered `NV_OK` — the guest-reachable defect, now a fixture). **These are the
  durable artifact**: a bootable C on a rented box is not.
- **`src/qemu/nvkvm_m2_rec.{c,h}`** — the recorder. Property `m2rec` (+ `m2recfile`,
  `m2recmask`), default OFF. ★ Never reuse `m2_trace` for capture: it is *not* observationally
  neutral (it re-arms an O(n) audit and the crashwin DIAG family). **Never sample or cap** —
  the consumer's `diff()` is positional, so a drop shifts every later index.
- **`docs/BENCH_REBUILD_NOTES.md`** — the authoritative first-person rebuild log. ★ **Read it
  before rebuilding a bench; do not re-derive it.**
- **`scripts/mode2_diag/bench_boot.sh` / `bench_wait.sh`** — the correct boot + wait sequence,
  with the traps encoded inline. `rec_capture.sh` / `rec_dump.py` drive and decode captures.

**Three operational traps that cost real cycles — all measured:**
- ★★★ **The serial log is NOT where the driver's output is.** Measured 2026-08-01:
  `grep -ci nvrm /workspace/bench/run_*_serial.log` returns **0** for every boot of that night,
  while older boots (`run_t135a_serial.log`) contain it. The guest driver is `modprobe`d over
  ssh *after* boot, so its `dmesg` goes to whoever ran the command and **nowhere else** — six
  consecutive rung claims had their only evidence inside a session transcript. The serial log
  still exists, is ~70 KB, is freshly timestamped and is named after the boot, so **every
  signal says the evidence is there**; only grepping for the content shows it is not.
  ⇒ Persist `dmesg` to `run_<tag>_dmesg.log` beside the serial log, and **assert it is
  non-empty and contains `NVRM`** — a harness that writes an empty file and exits 0 is worse
  than none, because the file's existence reads as capture.
- ★★ **`pgrep` fails in BOTH directions, and both are now measured.** `pgrep -x qemu-system-x86_64`
  **can never match** (`/proc/PID/comm` truncates to 15 chars ⇒ `qemu-system-x86`), so any "verify
  none running" built on it passes **vacuously**. Use `pgrep -x qemu-system-x86` **and**
  `ss -tln | grep 2223`.
  ⇒ ★★ And the mirror image, measured **three times on 2026-08-10**: `pgrep -f <literal>`
  **always matches the asker**, because the pattern is in the searching command's own
  `/proc/PID/cmdline`. Twice it made a **finished** boot read as still-running for minutes.
  `boot_capture.sh` documents this *inside* the script; it applies just as much to anyone
  driving the bench from outside it. Fix = the bracket trick: `pgrep -f '[b]uild_qom_shim'`.
  ⚠ Note the two failures are opposite: one **never** fires, one **always** does — so a waiter
  and a "nothing is running" check need different fixes, and neither is safe by default.
- ★★ ⊘ **CORRECTED 2026-08-08 — this trap is HARNESS-SPECIFIC and does NOT apply to the Rust
  bench.** The `~/.ssh/config` mapping `localhost`/`127.0.0.1` to the guest key matters only for
  the ~30 `scripts/mode2_diag/*_host.sh` that run a **bare** `ssh -p 2223 ubuntu@localhost`.
  ⊘ **On the kayfabe bench `vh` there is no such file and none is needed** — `gssh_nv` reaches the
  guest as **`ubuntu@192.168.77.2` over the tap**, not `localhost:2223`. Repeating the ssh-config
  advice there sends people to fix a file that was never in the path.
  ★ What DOES still hold, and is what actually reads as "never booted": the guest needs
  **~20–25 s** to reach a login prompt, `-serial file:` output **lags**, and ⊘ **`nvktap0` does
  not survive a host reboot** while QEMU requires it to pre-exist — a guest can be sitting at a
  login prompt while the harness reports it never answered. A slow boot is not a crash.

⚠ **Any bench claim must carry the SOURCE REVISION it was measured at.** The bench silently
served a binary built from `862c7c2` for weeks — every newer revision failed
`-Werror=redundant-decls` on a duplicate forward declaration — so results attributed to HEAD
were not HEAD's.

## Source layout

- `src/qemu/` — the QEMU-side device. `nvkvm_gpu_emul.c` = Mode-2 emulated GPU + fake GSP; also the
  Mode-1 forwarding stack (`virtio_nvgpu.*`, isolate API). This is the big one.
- `src/stub/` — the unprivileged **host stub / isolate** that issues real host RM ioctls.
- `src/guest/` — guest kernel module (Mode-1 path).
- `src/abi/`, `src/common/` — NVIDIA ABI structs / shared helpers.
- `tests/` — `mode2/`, `integration/`, `perf/`, `abi_parity/`, `security/`, `unit/`.
- `scripts/` — `run_mode2_vm.sh` (boot a Mode-2 guest), `mode2_iter.sh` (build→boot→test on host),
  `run_test_vm.sh` (Mode-1), `mode2_diag/`, vast.ai helpers.

## Design docs (`docs/design/`)

- **Forwarding model (read first for Mode-2):** `mode2_forwarding_model.md` — translate guest
  *intent* to unprivileged host userspace ops; never replay privileged GSP-internal controls;
  correctness = observable end-states only.
- **★ Address table (the data-plane core, read with the forwarding model):** `mode2_address_table.md`
  — one authoritative per-VAS VA→GPGA table, forward-populated, never reverse-resolved; the table
  IS the guest's TLB; miss = fault.
  ⚠ **Two CO-EQUAL populate sources, and NOT "RPC + read-at-invalidate"** — that phrasing was
  refuted by §5's ★ CORRECTION (2026-07-22, audit S3) and must not be repeated: on the **Mode-2
  GSP-emulated compute path both invalidate transports measured ZERO** (`INVALIDATE_TLB` RPC
  fn=200 = 0; `MEM_OP`/`MMU_TLB_INVALIDATE` pushbuffer method = 0), as did `DMA_FILL_PTE_MEM`.
  The sources are **(1)** bind-time RPC/ioctl bindings and **(2)** the **observed CE page-table
  write**, attributed by destination-FB-address → owning PDB and latched at the **CE release
  semaphore** — the commit point that *replaces* the absent invalidate. Read-at-invalidate still
  governs the kernel/UVM/RM paths, where the transports do appear. `mode2_2nd_context_hang.md`
  = the #12 bug it dissolves (GSP-managed CE channel finishPayload, root-caused).
- Mode-2 compute path: `mode2_compute_forwarding.md`, `mode2_gr_forwarding.md`,
  `mode2_cuctxcreate_resume.md` (+ `_problem.md`), `mode2_execfwd_keystone_plan.md`.
- Mode-2 internals: `mode2_memory_model.md`, `mode2_address_virtualization.md`,
  `mode2_bar2_mmu.md`, `mode2_device_data_model.md`, `mode2_m3_gsp_rpc.md`,
  `mode2_dataplane_architecture.md`, `mode2_doorbell_chid.md`, `mode2_interrupt_delivery.md`,
  `mode2_uvm_residency.md`, `mode2_multiprocess_isolate.md` (per-process page-table-publication
  isolate — the multi-process/#14 design, deferred to the Rust rewrite), `nvidia_gpu_internals.md`.
- Feasibility / strategy: `device_simulation_feasibility.md`, `mode2_plan.md`.
- Mode-1 / shared: `mode1_poll_relay_plan.md`, `command_buffer.md`, `gpa_window_pci_bar.md`,
  `async_event_delivery.md`, `virtual_modeset.md`, `signal_interrupt_delivery.md`.

## Other docs

- Security: `docs/SECURITY_MODEL.md`, `docs/HARDENING_PLAN.md`, `docs/audits/`.
- Status/plans: `docs/MILESTONES.md`, `docs/PARITY_PLAN.md`, `docs/PRE_PUBLIC_CHECKLIST.md`,
  `docs/REFACTOR_PLAN.md`.
- Agent workflow & token-cost strategy: `docs/WORKFLOW_STRATEGY.md` (serialize the bench, fan out
  read-only analysis, keep `MEMORY.md` lean, treat each debug episode as restartable-from-disk).
- Kernel patches applied to the guest driver for Mode-2 bring-up: `docs/kernel_patches/`.
- Reference / RE notes: `docs/reference/`, `docs/research/`, `notes/`.

## Reference material (read before guessing driver semantics)

- gVisor `nvproxy` (vendored under `gvisor/`) — the canonical ioctl-dispatch / pointer-translation
  reference.
- Open NVIDIA kernel modules under `research_clones/` (e.g. `ogkm/`) — ground truth for RM
  semantics; the open driver is stricter, treat it as canonical. Both closed + open drivers must
  work.

## Reference traces (the oracle's output — `traces/mode2_c_reference/`)

| capture | records | hermetic? | what it is for |
|---|---|---|---|
| `cap1_coldboot_hermetic` | 359 062 | **yes** (`m2fwd=off m2exec=off m2romregs=off`) | the only **closeable** replay; PCI enumerate → 139 821 PROM/VBIOS reads → FWSEC/WPR2 → LibOS args → msgq → `GSP_INIT_DONE` |
| `cap2_stalequeue_negative` | 886 999 | no | the `WPR2-already-up` chain |
| `cap2b_stalequeue_nofn47` | 862 940 | no | ★ **the real negative** — 378 GSP elements parsed from arbitrary guest RAM, answered `NV_OK` |
| `cap3_matmul_forwarding` | 532 824 | no | decision planes; `cup8` at `bad=0 maxerr=0` |

★★★ **FIFTH LIMIT, and it is different in kind — the oracle is POSITIVELY WRONG here, not blind**
(measured 2026-08-01 against a real GA106, `../nvkvm-rs/traces/real_ga106/`). The captured control
table `src/qemu/mode2_initctrl_ga106.h` has **56 rows, of which 11 (19.6%) carry `dlen = 0`** — the
reply body was never captured. **Every `dlen=0` row checked against real hardware is CONTRADICTED**
(`0x20802a08`, `0x20802a06`, `0x2080017e`, `0x20800af3`, `0x20800a4b`, `0x20800aac`), while **every
row carrying a body matches BYTE FOR BYTE**.
⇒ `0x20802a08` (`CE_GET_FAULT_METHOD_BUFFER_SIZE`) decodes from its empty row as **size 0**; a real
GA106 returns **20480**. RM DMAs CE fault records into a buffer of exactly that size, so trusting the
empty row was a **buffer overrun with a hardware writer**, not merely a wrong number.
⊘ **An empty capture is evidence of NOTHING, not evidence of emptiness.** Treat `dlen=0` as
*unmeasured* and refuse it; do not decode it to zeros. ★ And note how it survived: a gate demanding
a `C:` citation was **satisfied** by a row that cited the empty body *as corroboration* — **citing
the oracle is not the oracle being right.** A citation gate checks a claim is *sourced*, never that
the source says what the claim says.
★ This **scopes** the oracle rather than devaluing it: the 45 rows with bodies matched exactly. The
oracle is trustworthy precisely where it captured something.

★ **Four further measured limits before trusting any diff** (full text in
`../nvkvm-rs/docs/design/c_rust_trace_differential.md`): the **completion plane has NO C
oracle** — the C *forges* completions, so a green diff says nothing about it; **the diff can
never be green end-to-end** because the C has no refusal vocabulary; **forwarding-mode traces
are non-hermetic by construction** (`pci_dma_map` is an uninstrumented channel — the host GPU
DMAs into guest RAM behind every recorder); and `IrqRaise == 1` across the whole of `cap1`
with **zero** `IRQSCLR` writes, so event delivery is gated off after `INIT_DONE`.

## Working notes (conventions, not commands)

- Mode-2 dev loop runs on a remote GPU host (vast.ai): edit locally → sync to the host build tree
  → rebuild QEMU → fresh-boot the guest → run. The emulated GSP's WPR2 state only resets on a full
  QEMU restart, so **each clean run needs a fresh boot**, and GPU tests run **strictly serially**.
- The repo also carries persistent agent memory under
  `/root/.claude/projects/-workspace-nvidia-gpu-passthrough/memory/` (index `MEMORY.md`) with the
  live state of in-flight work — consult it for "where things are right now."
