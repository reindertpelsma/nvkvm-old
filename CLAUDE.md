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

**Two operational traps that cost real cycles — both measured:**
- ★★ `pgrep -x qemu-system-x86_64` **can never match** (`/proc/PID/comm` truncates to 15 chars
  ⇒ `qemu-system-x86`), so any "verify none running" built on it passes **vacuously**. Use
  `pgrep -x qemu-system-x86` **and** `ss -tln | grep 2223`.
- ★★ The bench needs a `~/.ssh/config` mapping `localhost`/`127.0.0.1` to the guest key,
  because ~30 `scripts/mode2_diag/*_host.sh` run a **bare** `ssh -p 2223 ubuntu@localhost`.
  Without it a perfectly healthy guest reads as "never booted". The guest also needs
  **~20–25 s** to reach a login prompt and `-serial file:` output **lags** — a slow boot is not
  a crash.

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
  — one authoritative per-VAS VA→GPGA table, forward-populated (RPC + PDB-read-at-invalidate),
  never reverse-resolved; the table IS the guest's TLB; miss = fault. `mode2_2nd_context_hang.md`
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

★ **Four measured limits before trusting any diff** (full text in
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
