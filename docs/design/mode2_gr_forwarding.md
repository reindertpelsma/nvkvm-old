# Mode-2 Phase B: forward GR/compute to a real host GPU (host parity)

Decision (user, 2026-06-03): **do B.** Pure-emulation Mode-2 is complete through
boot + GSP-RPC + BAR2/GMMU + CE + interrupts; it stalls at the GR (compute) engine
golden-context bootstrap because GR state is produced by FECS/GPCCS microcode on
real silicon. Phase B runs GR/compute on a real host GPU while the guest keeps
running its stock NVIDIA driver against the emulated device. Goal: CUDA at host
parity. QEMU stays UNPRIVILEGED.

## Premise & the lucky alignment
- Host (vast `vh`) has a real **GA106** (RTX 3060) + nvidia 580 driver.
- We advertise the guest as **GA106** too → same chip, same context/golden format,
  same class IDs. Driver versions must match (guest 580.159.04 ↔ host 580.x).
- The address-translation core (nvkvm_walk_pdb: guest GR-VA → GPA/FB, aperture-aware,
  2M/64K/4K) and channel tracking (gpfifo/USERD/instblk/doorbell) are already built.

## Architecture: "host-backed GR channel, guest-driven"
The guest RM is authoritative — it builds its own channel/VAS/context buffers in
guest memory against the emulated device. We do NOT mirror all of that. Instead:
- QEMU (the emulated device) talks to an UNPRIVILEGED host helper (reuse the
  Mode-1 stub model: it holds the real RM client/device/subdevice/GR-context fds
  and issues the host nvidia ioctls; QEMU never touches /dev/nvidia* directly).
- The host helper sets up a real GR/compute channel + context on the host GPU.
- The guest's GR pushbuffer submissions (doorbell → GPFIFO → methods, already
  parsed for CE) are REPLAYED on the host channel.
- Data stays in **guest RAM (GPAs)**; the host channel's VAS maps guest RAM
  (RM OS-descriptor of the guest-RAM HVA — Mode-1's mechanism, unprivileged), so
  guest GR-VA → GPA → host-GPU-VA. The host GPU reads kernels/inputs and writes
  outputs directly into guest RAM. No bounce.

## Golden context: content doesn't matter for BOOT
Critical simplification: the guest's golden-context buffer content is only USED at
context-RESTORE time (when a context actually runs on GR). We forward all real GR
execution to the host (whose own golden context is valid), so **the guest never
runs its own GR engine** — its golden buffer can be garbage. For BOOT we only need
to SIGNAL the golden-capture COMPLETION the driver's 4s poll waits on (same shape
as the CE semaphore fix). So B1 = signal the GR ctxsw/golden completion; we do NOT
need to produce a real golden image.

## Incremental phases (each testable)
- **B0 — design + transport** (this doc). Transport decision: reuse the Mode-1
  unprivileged stub as the host RM proxy (it already forwards RM ioctls); add a
  QEMU↔stub control path for Mode-2 GR ops. (Confirm the stub can be driven from
  the QEMU device process unprivileged.)
- **B1 — golden completion → RmInitAdapter SUCCEEDS** (immediate blocker): find the
  exact signal the golden-context capture's 4s `_threadNodeCheckTimeout` polls
  (after the GR-object GSP_RM_ALLOC on the golden channel) and provide it. Likely a
  GSP-RPC reply field, a notifier/semaphore beyond the channel sema, or a FECS
  status the driver reads. Then the stock driver fully loads + `nvidia-smi`
  enumerates. (No host GPU needed yet — boot-only.)
- **B2 — address bridge**: map guest RAM (a GR context's GPAs) into a host GR
  context's VAS via the stub (OS-descriptor). Prove the host GPU can read a guest
  buffer and write a result back into guest RAM (a host-side memcpy via CE on the
  host channel, verified by the guest reading the result).
- **B3 — first forwarded compute**: replay a real compute pushbuffer (cuLaunchKernel
  → SET_OBJECT(compute) + kernel launch methods) from the guest's GR channel onto
  the host channel; verify kernel output in guest RAM. First real CUDA kernel.
- **B4 — parity hot path**: direct-map the GR channel's USERD/doorbell (host GPU
  MMIO via stub forwarded mmap into guest GPA) so submissions hit real HW with no
  trap (per docs/design/mode2_memory_model.md). Removes the per-submit replay tax.
- **B5 — matrix**: run the 20-app compute/graphics matrix at host parity.

## Open questions to resolve as we go
- Exact GR golden-capture completion signal (B1 — investigate next).
- Stub interface for Mode-2 (raw RM alloc/control/map/submit) vs a new helper.
- Context-buffer/VAS ownership: does the host context use the guest's context
  buffers (mapped) or its own (then we bridge only data buffers)? Start with host
  owning its context, guest data buffers mapped in.
- Driver-version exact match (guest vs host) for class/ABI parity.

## Reuses
- nvkvm_walk_pdb / nvkvm_chan_translate (VA→phys), channel tracking, doorbell,
  the GSP-RPC shim, the CE method parser (extend for GR/compute classes), and the
  Mode-1 stub (unprivileged host RM proxy + OS-descriptor guest-RAM mapping).
