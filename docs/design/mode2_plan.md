# Mode-2 "Reverse Driver" — Implementation Plan (Phase 1)

Status: PLAN. Kicked off 2026-06-03 after the attestation spike returned **GO**
([[mode2_attestation_spike_GO]], `docs/research/mode2_attestation_spike.md`).

## Why Mode-2 (and why now)

Mode-1 forwards guest NVIDIA ioctls to the real driver running on the **host**;
the guest has only a fake DRM head. That split-brain is the root of every
display headache we hit (wrong-card rendering, scanout freezes, nvidia-modeset
coupling, no early-boot/text-VT target) and caps us at "Linux guest with our
guest module."

Mode-2 inverts it: the guest runs the **real, stock NVIDIA driver** against an
**emulated NVIDIA GPU** we present from QEMU. We "fake the boot" (never run a
real GSP), translate the addresses the driver programs, and forward real compute
to the host GPU via the existing Mode-1 core. Consequences:

- **Any stock OS** (Linux open *or* closed driver; Windows later) — no guest agent.
- **Display is free**: the emulated GPU exposes a normal framebuffer/VGA, so
  BIOS/grub/plymouth/text-VT/desktop all scan out trivially at every stage.
- The spike proved the **one** existential risk (silicon attestation) is a
  non-issue for GeForce in default mode: the driver verifies no silicon secret;
  all bring-up gates are software-mirrorable register/mailbox/RPC values.

The Mode-1 present/console/readback work (commit `eaf90fc`) is **mode-agnostic**
and reused as Mode-2's host-side display sink — not stranded.

## Architecture

```
   guest (stock NVIDIA driver, unmodified)
        │  MMIO / config / DMA to emulated GPU
        ▼
   QEMU: emulated NVIDIA GPU PCI device  ── nvkvm_gpu_emul.c (NEW)
     ├─ PCI config: vendor 0x10DE, real device ID, class 0x030000, MSI-X
     ├─ BAR0  register aperture (MMIO)  → boot-register state machine (checks #1–#7)
     ├─ BAR1  FB/aperture window        → address-virtualization layer
     ├─ GSP-RPC endpoint                → consumes NV_VGPU_MSG_* from sysmem rings,
     │                                     posts responses (GSP_INIT_DONE first)
     └─ address-virtualization layer    → record every GPA/GPU-phys/bus addr the
                                           driver programs; translate → host/stub
        │  shim RPCs + translated memory ops
        ▼
   Mode-1 core (stub + ioctl forwarding)  → REAL host GPU (compute executes here)
```

Reference chip: **GA106 (RTX 3060)** to match the host GPU, so the HAL the
driver selects, the register family (ampere/ga102 `swref`), and the downstream
forwarding all describe the same silicon. Register defs live in
`research_clones/ogkm/src/common/inc/swref/published/ampere/ga102/`.

Driver to target first: the **open** kernel modules (instrumentable, register
defs public). Closed Linux driver and Windows are later (same trust model per
the spike; different exact poll/RPC set).

## Phases & milestones

`fake-the-boot` (M0–M3) needs **NO real GPU** — pure emulation. So it escapes the
singleton GPU-host serialization ([[orchestration_model]]) and iterates fast on
any KVM box.

- **M0 — Driver probes our device (observe).** Emulated PCI device with a BAR0
  that logs every register access; boot a guest with the stock open driver bound
  (matches on PCI ID). Deliverable: the exact `RmInitAdapter` register-access
  trace up to first stall + an annotated "what the driver wants" map. No
  responses yet beyond PCI enumeration + `NV_PMC_BOOT_0` chip ID.
- **M1 — Reach GSP bootstrap.** Answer checks #1 (`GFW_BOOT`=COMPLETED + PLM
  lowered) and #2 (`HWCFG2._RISCV`=ENABLE) so the driver identifies the chip and
  enters `kgspBootstrap_TU102`/GA10x. Capture the next stall (FWSEC/booter IMEM/
  DMEM writes + halt/mailbox poll).
- **M2 — Fake the boot to RISCV ACTIVE.** ACK FWSEC/Booter: report falcon halt +
  `MAILBOX0`=NV_OK (#4), `RISCV_STATUS._ACTIVE_STAT`=ACTIVE (#3), `WPR2_ADDR_HI`
  nonzero (#5). Record (don't honor) LibOS-boot-args / WPR-meta GPAs. Driver now
  believes the GSP processor started.
- **M3 — sysmem message queue + `GSP_INIT_DONE` (KEYSTONE).** Implement the
  command/status ring transport in the GPA the driver hands us
  (`GspStatusQueueInit`); consume `kgspSendInitRpcs`; post `GSP_INIT_DONE`
  (`rpc_init_done_v17_00`) with `rpc_result`=NV_OK (#6,#7). `RmInitAdapter`
  returns success → **the stock driver believes it has a live GPU.** This is the
  proof-of-concept gate for the whole approach.
- **M4 — RPC surface triage.** Enumerate the post-INIT_DONE `NV_VGPU_MSG_*`
  stream; classify each: (a) static/emulated answer, (b) shim into a Mode-1 RM
  ioctl, (c) needs real GPU. Build the shim dispatch skeleton. (Mitigation from
  spike §5.1: forward into the Mode-1 core, don't re-model GSP-RM.)
- **M5 — First real compute.** Wire enough RPCs + address translation + memory
  ops into the Mode-1 core to enumerate the GPU (`nvidia-smi`) and run one
  trivial compute op end-to-end through the emulated front + real back.
- **M6 — Display for free.** Expose a generic framebuffer/VGA scanout so early
  boot, text VT, and the desktop all present via the Mode-1 host console
  (`eaf90fc`) with zero compositor gymnastics — closing the loop that motivated
  the pivot.

M0–M3 = "does fake-the-boot actually work against a stock driver" (the risk).
M4–M5 = the large, known RPC/forwarding long-tail. M6 = the display win.

## Isolation model (carried over from Mode-1, with a Mode-2 process key)

Mode-1's host security boundary is **one sandboxed isolate per guest userspace
process** ([[isolate_architecture]], [[access_model_split]]): the host-side
execution of a guest process's GPU work runs in a sandbox holding only that
process's GPU resources, so a hostile/buggy guest process cannot reach another's
GPU memory on the host. **Mode-2 MUST preserve this**, even though the guest now
runs one stock driver with no guest agent.

Granularity (user-specified, 2026-06-03):
- **One isolate per guest userspace process.** A process may hold many GPU
  contexts/channels — they all share that process's single isolate.
- **An isolate per context is acceptable** (over-isolation is always safe), but
  the *required* grouping is per process: contexts of the same process coalesce.
- **Process identity = the guest userspace address space**, NOT thread. QEMU
  cannot reliably distinguish guest kernel threads, so "same guest userspace VM
  (address space) ⇒ same process ⇒ same isolate" is the valid criterion.

The Mode-2 signal QEMU keys on (where Mode-1 used a guest-module pid/mm tag, now
unavailable): **the vCPU `CR3` at the trapping MMIO access** — the doorbell /
USERD / submission write (§6/§7 of [[nvidia_gpu_internals|nvidia_gpu_internals.md]]).
Same CR3 ⇒ same guest address space ⇒ same process ⇒ route the forwarded work to
that process's isolate. Properties:
- Userspace submission (the hot path) always traps from the owning process's
  address space, so CR3 is present and correct at exactly the moment we need it.
- Guest **kernel threads** share the kernel CR3 and do not ring the usermode
  doorbell, so they never blur a userspace process key; kernel-initiated RM
  traffic (boot, GSP-RPC setup) maps to a dedicated **system isolate**.
- RM clients / VA-spaces / channel-groups created via GSP-RPC are attributed to
  the CR3 that allocated them, binding each context to its process's isolate at
  creation time (so even non-doorbell control paths route correctly).

Open: confirm CR3 is observable at every relevant MMIO exit (it is, via the vCPU
state at the KVM MMIO exit) and define the CR3→isolate table lifecycle (process
exit = guest frees its mappings → reap the isolate, reusing Mode-1's reaper).

## Language: Rust core, thin C shell

Mode-2 is the right place to introduce Rust (decision 2026-06-03):

- **C — QEMU device shell only.** PCI config, BAR/MMIO read-write traps, MSI-X,
  KVM memory-region wiring. This must integrate with QEMU's C device model, and
  QEMU 9.2 has no first-class Rust device support, so Rust here is FFI pain for
  pure plumbing. The shell stays as small as possible: it traps and hands raw
  buffers/offsets to the core.
- **Rust — the logic core**, built as a `no_std`/static library behind a narrow,
  well-typed C ABI. Owns everything that parses **untrusted guest-supplied
  input on the host**: the GSP-RPC `NV_VGPU_MSG_*` decoder, the address-space
  virtualization + page-table (radix/PDE/PTE) walks, the address translation
  tables, and the RPC→Mode-1-ioctl shim. This is the new host attack surface and
  the keeper logic — memory-safety bugs here are catastrophic, so it is Rust from
  the start (no C→Rust rewrite later).
- **Converges with the stub→Rust rewrite.** Per-process (CR3-keyed) translation
  + compute forwarding runs inside the sandboxed isolate, in Rust — untrusted
  parsing is then *both* memory-safe (Rust) *and* sandbox-contained (isolate).
  System-level pieces (boot-register model, GSP-RPC transport) are the QEMU C
  shell calling the Rust core; per-process channel submission/translation routes
  to the Rust stub isolate ([[mode2_isolation_cr3_key]]).

Boundary rule of thumb: if it touches a QEMU API → C shell; if it interprets a
byte the guest controls → Rust core.

## Key decisions / open questions

- **Emulate vs capture-first:** iterate M0→M3 by booting the open driver against
  the emulated device and adding responses where it stalls (printk + BAR0 log is
  the ground truth). Running the open driver against the *real* GPU with heavy
  printk (spike's suggestion) is a parallel reference if a register trace is
  ambiguous — needs the GPU host.
- **MSI/interrupts:** the driver expects interrupts (e.g. GSP→CPU doorbell).
  Start polled where possible; add MSI-X assertion for the message-queue
  doorbell at M3.
- **Self-consistent identity (spike §5.2):** `NV_PMC_BOOT_0`, HWCFG, PMC boot
  regs, and PCI IDs must all describe GA106. Build a single chip-descriptor.
- **Any open-driver GPU, eventually (user directive 2026-06-03).** GA106 is only
  the *bring-up* reference (it matches the dev host, so the trace and the
  downstream forwarding describe the same silicon). The end goal is to emulate
  **any NVIDIA GPU the open kernel module supports** — every RTX/Ada/Hopper/
  Blackwell part. The architecture is built for this from the start:
  - All silicon-specific identity lives in the `NvkvmGpuChip` descriptor
    (`nvkvm_gpu_emul.c`): PCI IDs, `PMC_BOOT_0/42`, BAR sizes. Adding a chip =
    adding a table row; the device is selected to match (or be told to mimic)
    the host GPU it forwards to.
  - The **register answers** for the boot state machine are mostly arch-stable:
    the GFW/GSP path routes through the shared `_TU102` HAL for everything
    Turing-and-newer, so the GFW_BOOT/PLM/RISCV/mailbox offsets are common.
    Where an arch diverges (Ada/Hopper/Blackwell scratch layouts, CC), the
    descriptor carries per-arch register tables — same dispatch, different data.
  - The **GSP firmware + RPC ABI** are per-driver-version, not per-chip; matching
    the in-guest driver version (we run 580.159.04 to match the host) covers it.
    Converges with Mode-1's `abi_profile` auto-detect ([[multi_driver_validated]]).
  - Multi-GPU (N emulated functions, each bound to a host GPU by BDF) is the
    orthogonal axis already required ([[mode2_perf_dma_multigpu]]); per-instance
    state + per-chip descriptor compose: each function picks its own chip row.
  PoC proceeds on GA106; generalize to a chip table once fake-the-boot →
  GSP_INIT_DONE works on the reference part.
- **Confidential Compute stays OFF** (spike #8/§5.3) — never advertise CC.
- **Closed driver / Windows** deferred (spike §5.4): same attestation conclusion,
  unverified poll/RPC set.

## First concrete task (M0)

`src/qemu/nvkvm_gpu_emul.c` — a QEMU PCI device:
1. PCI config: vendor 0x10DE, device 0x2503 (GA106 / RTX 3060), class 0x030000,
   subsystem, capabilities (PM, MSI-X, PCIe).
2. BAR0: 16 MiB MMIO region; read/write handler that logs `(offset, size,
   value)` and, for now, returns the GA106 `NV_PMC_BOOT_0` on offset 0 and 0 for
   everything else.
3. BAR1: 256 MiB prefetchable FB aperture (stub).
4. Boot guest, bind stock open driver, capture the BAR0 access trace.

Scaffolded in this repo; build-wired into the host QEMU tree like the Mode-1
device. The boot-register state machine (M1/M2) is stubbed in the same file
behind a `reg_read` switch keyed on the offsets enumerated in the spike table.
