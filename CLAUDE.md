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
  `mode2_uvm_residency.md`, `nvidia_gpu_internals.md`.
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

## Working notes (conventions, not commands)

- Mode-2 dev loop runs on a remote GPU host (vast.ai): edit locally → sync to the host build tree
  → rebuild QEMU → fresh-boot the guest → run. The emulated GSP's WPR2 state only resets on a full
  QEMU restart, so **each clean run needs a fresh boot**, and GPU tests run **strictly serially**.
- The repo also carries persistent agent memory under
  `/root/.claude/projects/-workspace-nvidia-gpu-passthrough/memory/` (index `MEMORY.md`) with the
  live state of in-flight work — consult it for "where things are right now."
