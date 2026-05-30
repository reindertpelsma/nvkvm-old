# Feasibility Study: nvkvm Mode 2 — "GPU Device Simulation" (reverse nvkvm)

Status: research / design only. No source changed.
Author: GPU-virtualization architect (feasibility pass)
Date: 2026-05-30

## 0. Scope and definitions

- **Mode 1 (today).** The guest runs a matching NVIDIA userspace (libcuda) plus the
  nvkvm guest kernel module. The guest module forwards `/dev/nvidia*` *ioctls* over a
  paravirtual virtio link; the host translates and replays them against the real driver.
  Boundary = the **ioctl/RM-API** layer. Guest must be modified.
- **Mode 2 (this study).** Present a **simulated NVIDIA PCI device** to the guest so the
  **stock, unmodified** `open-gpu-kernel-modules` driver loads and drives *our* emulated
  hardware. The host turns the guest driver's hardware/GSP traffic into real operations
  on the host GPU. Boundary = somewhere at/below the **GSP-RPC / BAR0-MMIO** layer.
  Guest is unmodified (the strategic prize: stock Linux driver, ultimately Windows).

This document answers the four questions in the brief and gives a go/no-go.

Evidence is drawn from the on-host `open-gpu-kernel-modules` tree (`ssh vh`), the local
gVisor `nvproxy` ABI model, and public GSP-RM / vGPU documentation. File paths below are
relative to `open-gpu-kernel-modules/` on the vast host unless noted.

---

## 1. Interception boundary — what each option costs

The stock driver's life on Turing+ silicon is, from cold:

1. Read PCI config + **BAR0** (16 MB MMIO register aperture), **BAR1** (framebuffer
   aperture / window), **BAR3** (USERD/doorbell on some chips).
2. Read **fuses and BSI secure scratch** to learn secure-boot state
   (`GPU_REG_RD32(pGpu, NV_FUSE_OPT_SECURE_GSP_DEBUG_DIS)`,
   `NV_PGC6_BSI_SECURE_SCRATCH_14`; `src/.../gsp/arch/turing/kernel_gsp_tu102.c`).
3. **Extract the VBIOS from ROM**, parse **FWSEC**, run it to trigger **FRTS** and set up
   **WPR2** (write-protected region in framebuffer)
   (`kgspInitRm_IMPL`, `kgspExtractVbiosFromRom_HAL`, `kgspParseFwsecUcodeFromVbiosImg`;
   `src/.../gsp/kernel_gsp.c` ~line 3650+).
4. Load the **30–72 MB signed GSP-RM firmware blob** (`gsp_tu10x.bin` 30 MB,
   `gsp_ga10x.bin` 72 MB on this host) into framebuffer, boot the **falcon → SEC2 →
   RISC-V GSP** chain. The HS bootloader's signature is **verified by the silicon itself**
   ("root of trust", per NVIDIA GSP docs). FALCON mailboxes
   (`NV_PGSP_FALCON_MAILBOX0/1`) carry the boot args physical address.
5. After GSP-RM is live, the driver sets up a **shared-memory message-queue pair**
   (command/status) in system memory and rings the **doorbell** by writing
   `NV_PGSP_QUEUE_HEAD(queueIdx)` in BAR0
   (`kgspSetCmdQueueHead_TU102`; `src/.../gsp/message_queue_priv.h`). GSP raises **MSI-X**
   interrupts back. All RM work after this is **GSP-RPC messages**, not raw registers.
6. CUDA work then flows as **channel/USERD/doorbell** submissions plus GSP-RPCs for
   allocation/control.

Three candidate boundaries:

### (a) Full BAR0/MMIO + GSP-RPC ring emulation ("the driver thinks it owns a real GPU")
Must emulate: PCI config space + capabilities; the **entire BAR0 register model** the
driver touches during init (PMC, PBUS, PFB/MMU, PGC6/BSI scratch, FUSE, PGSP falcon,
PFALCON, EMEMC/EMEMD ports, timer, MC interrupt tree); **VBIOS ROM** content and the
**FWSEC/FRTS/WPR2** flow; **falcon/RISC-V boot** handshake; the **doorbell** register;
**MSI-X** delivery; **BAR1** framebuffer aperture; **USERD/doorbell** channel path. Then,
*below* the RPC ring, redirect the GSP-RPC messages to the host.

This is the maximal surface. The driver's `kgspBootstrap_HAL` expects the *real* secure
boot to complete (WPR2 comes up, falcon reports the RISC-V app version, GSP posts
`GSP_INIT_DONE`). You cannot fake that without either (i) the real silicon doing it, or
(ii) re-implementing NVIDIA's secure boot — which is signature-gated by hardware and is a
multi-person-year RE effort with no guarantee (see §3.1). **Verdict: emulating boot in
software is effectively infeasible.**

### (b) Intercept at the GSP-RPC ring only (boot GSP-RM normally, redirect RPCs)
Let the guest driver boot a *real* GSP-RM (so FRTS/WPR/falcon all run on real silicon),
then intercept the **command/status message queues** and **doorbell** and ship the RPC
messages to a host agent. Must emulate: only the **doorbell write trap**, the **queue
head/tail registers**, and **interrupt injection** — *if* you could give the guest a real
GPU to boot on. But a VM that is given a real GPU to boot GSP on **is just VFIO
passthrough** — there is no second GPU to boot per guest on commodity hardware. So option
(b) only makes sense if *one* host GSP-RM is shared by *many* guests. That means the guest
must NOT boot its own GSP; it must believe GSP booted while the host multiplexes a single
real GSP-RM. That is exactly NVIDIA's **vGPU** design, and it is gated on SR-IOV VFs +
the closed `vmioplugin`/`vgpu-manager`. See §2/§4. The RPC header itself
(`rpc_message_header_v03_00` in `generated/g_rpc-message-header.h`) literally calls the
peer *"the vmioplugin & guest RM"* — i.e. this ring was designed to be terminated by
NVIDIA's host-side vGPU plugin, not by us.

### (c) Mediated / SR-IOV partitioning (mdev / VF)
Use the GPU's own SR-IOV: enable VFs on the PF, expose each VF as a virtual GPU via
vfio-mdev. The host `vgpu-manager` boots one GSP-RM per VF (or a partitioned GSP), and the
guest's stock driver talks to its VF. Must emulate: essentially *nothing* in software —
the hardware + NVIDIA's host stack do it. But this **requires SR-IOV-capable silicon**
(Ampere datacenter and up: A100/A40/L40/H100…), the **NVIDIA vGPU host driver license**,
and is unavailable on the GeForce/commodity cards nvkvm targets. `vgpu_unlock`/Open-IOV
can spoof a consumer card as a Tesla to *get past the license check*, but the **SR-IOV VF
hardware path still has to exist**; pre-Ampere it patches the legacy mdev path, and on
Ampere+ it still needs the card's VF capability. It does not create SR-IOV where the
silicon lacks it.

**Boundary conclusion.** The only boundary that is both (i) clean and (ii) does not require
re-implementing hardware secure boot is the **GSP-RPC message ring (b)** — but terminating
that ring for *many guests on one GPU* is precisely vGPU, which needs SR-IOV (c). Full
software emulation (a) founders on GSP boot. There is no commodity-hardware sweet spot in
options (a)/(b)/(c) as literally posed.

---

## 2. The GSP-RPC surface

**Count.** The RPC function table (`src/nvidia/inc/kernel/vgpu/rpc_global_enums.h`,
checked on host) defines **`NV_VGPU_MSG_FUNCTION_NUM_FUNCTIONS = 227`** request types
(0…226, with a handful marked deprecated/reserved) plus **`NV_VGPU_MSG_EVENT_NUM_EVENTS =
0x1023` (35 async events, 0x1000…0x1022)** that GSP posts back. The functionally hot ones
are few and familiar:

- `GSP_RM_CONTROL` (76) — wraps the entire **RM control-command** space (the same
  `NV*_CTRL_*` commands nvproxy/Mode 1 already understand). This is the workhorse.
- `GSP_RM_ALLOC` (103) — wraps `RM_ALLOC` (the class/handle allocator; same `nvos64`
  surface Mode 1 sanitizes).
- `RM_API_CONTROL` (204), `ALLOC_ROOT/MEMORY/VIDMEM/VIRTMEM`, `MAP_MEMORY[_DMA]`,
  `DUP_OBJECT`, `FREE`, `CTRL_GPFIFO_SCHEDULE`, `CTRL_GET_WORK_SUBMIT_TOKEN`,
  `CTRL_GPU_PROMOTE_CTX`, `SET_PAGE_DIRECTORY`, `INVALIDATE_TLB`, the `UVM_PAGING_CHANNEL_*`
  family (160–166), etc.

So the *semantic* surface is **the same RM/control/alloc ABI that Mode 1 and gVisor's
nvproxy already model** — `GSP_RM_CONTROL`/`GSP_RM_ALLOC` are thin RPC envelopes around the
exact `NVOS*`/`NV*_CTRL_*` payloads. **This is the key insight, and it holds**: we already
know this surface.

**Stability / versioning — worse than the ioctl ABI.** The RPC wire format is *not* a
stable contract; it is **co-generated and version-locked to a specific driver build**:

- The header is `rpc_message_header_v03_00` and the whole `g_rpc-*.h` / `g_rpcstructure*`
  family is **generated** (the `g_` prefix). Struct layouts are emitted per build.
- GSP-RM and CPU-RM are **two halves of one compiled RM**. NVIDIA ships them together and
  the public guidance is explicit that vGPU host (GSP/`vgpu-manager`) and guest driver
  must be from **compatible release branches**; cross-branch mixing "fails to load"
  (NVIDIA vGPU User Guide; Event ID 160 "Guest driver is incompatible with host driver").
  The on-host firmware blob is named by full driver version (`580.159.04/gsp_ga10x.bin`),
  and the open-RM CPU side asserts a matching GSP-RM build version at boot.
- Contrast: the *ioctl* ABI Mode 1 rides on is comparatively stable and nvproxy tracks it
  with ~39 discrete versions (`v535_104_05` … `v590_44_01` in `nvproxy/version.go`), each a
  curated diff. The RPC structs churn *at least* as fast and have **less** external
  documentation than the ioctl ABI, because they were never meant to cross a trust
  boundary we control.

**Can host GSP-RM execute guest-originated RPCs?** In principle the payloads are the same
RM operations, but three concrete impedance mismatches block a naive splice:

1. **Client/handle namespace.** RPCs carry `hClient/hObject/hParent` handles. In vGPU these
   live in a **per-guest (per-GFID)** namespace the host plugin maps into the real RM. Mode
   1 already had to solve almost this exact problem (see MEMORY: `rmclient_validate_strict_fix`,
   `hclient_not_fd_scoped`, cross-session handle reach C3/C4). The work is real but *known*.
2. **DMA address translation.** RPC payloads embed **guest-physical addresses** for the
   message queues, page tables (`SET_PAGE_DIRECTORY`, `UPDATE_GPU_PDES`,
   `DMA_FILL_PTE_MEM`, `TRANSLATE_GUEST_GPU_PTES`), USERD, semaphores, and DMA buffers. The
   real GSP/MMU must see **host-physical or IOMMU-mapped** addresses. There is an explicit
   `TRANSLATE_GUEST_GPU_PTES` RPC precisely because the *host* side is expected to fix up
   guest PTEs — i.e. NVIDIA's own design assumes a privileged host translator (the
   vgpu-manager) sits here. We would have to *be* that translator. This is the single
   biggest non-security hard part (§3).
3. **GSP partitioning / GFID.** A shared host GSP-RM tags work by **GFID** (function id).
   Without an SR-IOV VF there is no GFID to present, so a single host GSP-RM has no native
   way to keep N guests' contexts isolated.

---

## 3. Hard unknowns, ranked by risk

### #1 (highest) — GSP firmware boot on a simulated device
GSP-RM is a **30–72 MB signed RISC-V firmware** whose HS bootloader signature is verified
**by the GPU silicon**, after a VBIOS-driven **FWSEC → FRTS → WPR2** sequence that reads
fuses/BSI scratch and drives the falcon/SEC2 chain (`kgspInitRm_IMPL`,
`kgspBootstrap_HAL`, `kernel_gsp_frts_tu102.c`, `kernel_gsp_tu102.c`). You cannot run this
against a software-emulated BAR0: there is no silicon to verify the signature or run the
RISC-V core. Options: (a) let the guest boot GSP on *real* silicon — but that is
passthrough, one GPU per guest; or (b) make the guest believe GSP booted while a host GSP
serves it — that is vGPU and needs a VF. **No commodity software path exists.** This single
fact is close to decisive.

### #2 — Guest-physical ↔ host DMA translation (IOMMU/GPA) for GPU DMA
Every page-table, USERD, semaphore, and DMA pointer in the RPC stream is a **guest** address
that the real GPU MMU/GSP must not see verbatim. We would need to walk and rewrite guest
PDEs/PTEs into host-IOMMU space on the fly (`SET_PAGE_DIRECTORY`, `UPDATE_GPU_PDES`,
`DMA_FILL_PTE_MEM`, `TRANSLATE_GUEST_GPU_PTES`) and keep them coherent as the guest remaps.
Mode 1 sidesteps most of this by replaying high-level ioctls and using a GPA-window mmap
trick (MEMORY: `gpa_window_design`, `nvos56_fake_success`); Mode 2 would re-expose the raw
PTE plumbing. High risk, high effort, but *theoretically* tractable if #1 were solved.

### #3 — Security: the guest driver becomes an untrusted near-raw HW client
In Mode 1 the guest only emits ioctls that the host **sanitizes** (the whole audit history:
handle TOCTOU C1, cross-session reach C3/C4, seccomp, OOB #66, etc.). In Mode 2 the guest
emits **GSP-RPCs that include reg-op lists** (`GPU_EXEC_REG_OPS` 50,
`CTRL_DBG_EXEC_REG_OPS` 134, `CTRL_B0CC_EXEC_REG_OPS` 130), page-directory installs, and
fault-buffer registrations — i.e. near-raw hardware control. We'd have to validate a
**227-function** RPC surface (vs the already-large ioctl surface) where many messages carry
register/PTE arrays. nvproxy is *default-deny* and still finds this hard; a default-deny
RPC allowlist that preserves CUDA functionality is a large, ongoing security project. The
attack surface is strictly larger than Mode 1.

### #4 — Interrupt delivery (MSI-X) and the doorbell/queue protocol
GSP signals completion and async events (`POST_EVENT`, `RC_TRIGGERED`,
`MMU_FAULT_QUEUED`, `GSP_INIT_DONE`) via **MSI-X**; the driver rings work via
`NV_PGSP_QUEUE_HEAD` doorbell writes and polls the status queue. Trapping the doorbell MMIO
and injecting virtual MSI-X into the guest is **standard device-model work** (QEMU does this
for virtio/vfio) and is the *least* novel risk here — but it must be wired to the host
RPC agent's completion events with correct ordering and the queue's seqnum/checksum/auth-tag
discipline (`GSP_MSG_QUEUE_ELEMENT` carries a 16-byte **authTag** + AAD + checksum;
confidential-compute builds **encrypt/authenticate** queue elements, which we could not
forge). Medium risk on non-CC parts, blocking on CC parts.

### #5 — USERD/doorbell channel submission + BAR1 framebuffer aperture
Post-init CUDA submits via **USERD** rings and a **doorbell**; the **BAR1** aperture windows
VRAM into the guest. In a shared-GPU model these must be partitioned per guest and the
guest's BAR1 view mapped to a host VRAM slice with MMU enforcement. Mode 1 already proved
the data path works through the GPA window for memcpy/compute (MEMORY: `cumemcpy_first_pass`,
`ptxjit_version_match`); Mode 2 would re-derive it at the hardware-aperture level. Medium
risk *given* #1/#2 solved; otherwise moot.

(Doorbell MMIO trapping and BAR layout are not in the top 3 because they are well-trodden
device-model problems; the GPU-specific killers are boot (#1), DMA translation (#2), and
the enlarged trust surface (#3).)

---

## 4. Verdict

### Is Mode 2 feasible on commodity (non-SR-IOV) NVIDIA GPUs?
**No — not as "simulate a GPU and let the stock driver boot GSP against it."** The blocker
is structural, not merely laborious:

- GSP-RM is a large signed RISC-V firmware whose boot is **anchored in silicon**
  (hardware signature check, FWSEC/FRTS/WPR2, falcon/RISC-V). You cannot boot it against a
  software BAR0, and you cannot skip it on Turing+ (GSP is mandatory for the open module).
- Sharing **one real GSP-RM across many guests** is exactly NVIDIA vGPU, which is gated on
  **SR-IOV VFs** (Ampere+ datacenter) and the closed `vgpu-manager`/`vmioplugin`. The RPC
  ring's own header names the peer "the vmioplugin" — it was built for NVIDIA's host plugin
  to terminate, not us. `vgpu_unlock`/Open-IOV only defeat the *license/marketing* gate;
  they still ride the card's real VF/mdev hardware path and do not synthesize SR-IOV on
  silicon that lacks it.

So: **Mode 2 in its pure form effectively requires vGPU/SR-IOV-class hardware.** On the
GeForce/commodity GPUs nvkvm targets, a faithful "stock driver over a simulated device" is
not reachable by software alone.

### Is there a meaningful middle path?
Yes — and it is the only sane one: **a stock-driver-compatible *front* that reuses Mode-1
forwarding underneath.** Concretely, build a **paravirtual GSP transport**: expose a small
emulated PCI device whose "GSP" is a **stub that never boots real firmware**; instead it
**terminates the GSP-RPC ring in QEMU** and lowers each `GSP_RM_CONTROL`/`GSP_RM_ALLOC`/etc.
message into the **existing Mode-1 host replay path** (which already sanitizes the identical
`NVOS*`/`NV*_CTRL_*` payloads). The guest still needs a **shim** to (a) skip real GSP boot
and (b) present our queues — i.e. it is *not* a fully stock driver, but it could be a **thin
out-of-tree patch to `open-gpu-kernel-modules`** (force `IS_GSP_CLIENT`-style path to our
emulated GSP) rather than a bespoke module. Benefits: reuses the proven sanitizer, handle
namespace, GPA-window DMA, and CUDA data path; the guest userspace is **fully stock**
(libcuda unmodified). Cost: still a guest-side driver patch, so it does **not** deliver the
"completely unmodified driver" or "Windows guest" prize — Windows has no source to patch and
its GSP path cannot be redirected without the firmware boot we cannot provide.

A genuinely-unmodified-driver path (including Windows) is realistically only obtainable by
**buying into SR-IOV hardware** and either licensing NVIDIA vGPU or doing the Open-IOV-style
unlock on a VF-capable card — at which point nvkvm's paravirtual value proposition mostly
evaporates (you'd be a vGPU deployment, not a novel forwarder).

### Effort estimate (paravirtual-GSP middle path, the only viable build)
- **M0 – Bring-up spike (1.5–2 pm):** emulated PCI device + BAR0 trap + GSP message-queue
  pair + doorbell trap + MSI-X injection in QEMU; loopback an RPC. Prove a patched guest
  open-RM reaches "GSP_INIT_DONE" against our fake GSP without touching silicon.
- **M1 – RPC↔ioctl lowering (2–3 pm):** map `GSP_RM_CONTROL`/`GSP_RM_ALLOC`/alloc/map/free
  onto the existing Mode-1 replay + sanitizer; handle namespace + writeback parity.
- **M2 – DMA/PTE translation (3–4 pm, highest risk):** `SET_PAGE_DIRECTORY`/`UPDATE_GPU_PDES`/
  `TRANSLATE_GUEST_GPU_PTES`/USERD/BAR1 into the GPA-window model; channel submission.
- **M3 – Async events + RC/fault path (1.5 pm):** `POST_EVENT`/`RC_TRIGGERED`/
  `MMU_FAULT_QUEUED` back-channel, seqnum/checksum/authTag discipline (non-CC only).
- **M4 – Security hardening (2–3 pm, ongoing):** default-deny RPC allowlist over 227
  functions incl. reg-op/PTE validators; threat-model the larger surface.
- **M5 – Version tracking (recurring):** regenerate RPC struct bindings per driver build;
  expect churn ≥ the 39-version nvproxy treadmill.

**Total ≈ 11–16 person-months** to a single-GPU, single-(or-few)-guest demo that still
requires a guest-side open-RM patch — and it does **not** unlock Windows or
truly-stock-driver guests. Confidence is low on M2 (DMA/PTE) and on long-term version churn.

### Go / No-Go recommendation
**No-go on pure Mode 2 (simulated device, stock driver booting GSP).** It is blocked by
hardware-anchored GSP boot and is, in the shared-GPU form, a re-implementation of vGPU that
needs SR-IOV silicon nvkvm deliberately avoids.

**Conditional, lower-priority "explore later" on the paravirtual-GSP middle path** *only if*
a concrete customer needs to run a Linux distro's stock-packaged NVIDIA driver (no nvkvm
guest module) and is willing to accept a small out-of-tree open-RM patch. Even then it
re-uses Mode 1 underneath and inherits all of Mode 1's hard-won sanitizer/DMA work, so it is
better framed as a **future front-end for Mode 1**, not a second engine.

**Primary recommendation: keep investing in Mode 1.** Mode 1 already runs real CUDA, 7B-LLM
inference, multi-process, and containers through the forwarder (MEMORY: `llm_7b_inference_done`,
`multi_process_unblocked`, `container_toolkit_works`); its moat is the sanitizer + DMA model,
which Mode 2's only viable variant would have to reuse anyway. The marginal dollar is better
spent finishing Mode 1 hardening (teardown #80, RM-control allowlist per the nvproxy gap
analysis) than chasing a Mode 2 that either needs SR-IOV hardware or still ships a guest
patch.

---

## Appendix — key evidence pointers

- RPC function/event tables (227 fns / 35 events):
  `open-gpu-kernel-modules/src/nvidia/inc/kernel/vgpu/rpc_global_enums.h`.
- RPC message header (generated, per-build): `.../src/nvidia/generated/g_rpc-message-header.h`
  (`rpc_message_header_v03_00`; comment: "communication between the vmioplugin & guest RM").
- GSP message queue + doorbell + authTag: `.../inc/kernel/gpu/gsp/message_queue_priv.h`
  (`GSP_MSG_QUEUE_ELEMENT.authTagBuffer[16]`), `kgspSetCmdQueueHead_TU102`
  (`.../gsp/arch/turing/kernel_gsp_tu102.c`, `GPU_REG_WR32(pGpu, NV_PGSP_QUEUE_HEAD(...))`).
- GSP boot (VBIOS/FWSEC/FRTS/WPR2/falcon, signature): `.../gsp/kernel_gsp.c`
  (`kgspInitRm_IMPL`, `_kgspBootGspRm`, `kgspBootstrap_HAL`, `kgspExtractVbiosFromRom_HAL`,
  `pGspFw->pSignatureData/signatureSize`), `.../gsp/arch/turing/kernel_gsp_frts_tu102.c`.
- Firmware blob sizes (signed, silicon-run): host `/lib/firmware/nvidia/580.159.04/`
  `gsp_tu10x.bin` 30 MB, `gsp_ga10x.bin` 72 MB.
- ioctl ABI version treadmill for comparison: `gvisor/pkg/sentry/devices/nvproxy/version.go`
  (39 versions `v535_104_05`…`v590_44_01`).
- vGPU needs SR-IOV (Ampere+) + version-matched host/guest: NVIDIA vGPU User Guide;
  `vgpu_unlock`/Open-IOV defeat licensing, not the VF hardware requirement.
