# Mode-2 forwarding model — translate guest *intent*, don't replay privileged GSP internals

Status: governing principle (2026-06-11). Applies to all Mode-2 forwarding code in
`src/qemu/nvkvm_gpu_emul.c` and the host stub/isolate. Read alongside
`mode2_cuctxcreate_resume.md` (§0.3 map-vs-stub) and `mode2_compute_forwarding.md`.

## The thesis (what the "reverse driver" is doing)

In Mode-2 the guest runs the **stock, unmodified NVIDIA kernel driver + UVM** against an
**emulated GPU with a faked GSP**. By the time anything reaches us (BAR/register writes, the
GSP-RPC ring, DMA), the guest's kernel-RM has **already decomposed an unprivileged userspace
intent** (e.g. `cuCtxCreate`, `cuMemAlloc`, a kernel launch) into a stream of low-level
operations — many of them **privileged, GSP-internal** steps that only ever exist *inside* the
kernel-RM → GSP path.

The job of Mode-2 is **not** to replay those low-level steps on the host. It is to **recover the
original userspace-level intent and re-express it as the normal, unprivileged host userspace
operations** — exactly the operations Mode-1 forwards directly. The host's own kernel-RM then
legitimately re-derives all the privileged GSP steps internally. The host runs a **real GR/CUDA
context and real execution** (the whole point: Mode-2 must eventually run Mode-1 apps).

## Correctness criterion

Only the **observable end-states** must match a real system:

- the host kernel/RM state and the **real GPU execution**, and
- what the **guest GPU application** (libcuda and up) observes.

Everything between is free. The guest kernel module is **not a black box we must faithfully
re-execute** — it is a means to an end. It is perfectly correct for some guest-kernel operations
to **complete instantly as fakes** (they are internal side-effects of the guest's RM sequence),
**as long as** the *one* operation that actually carries the work triggers the real host-side
chain, and the final observables are right. Faking an internal side-effect ≠ faking the result.

## Two classes of forwarded operation

1. **Case 1 — the RPC *is* (essentially) the userspace op.** `GSP_RM_ALLOC` carries the same
   `NVOS64` alloc params as a userspace `RM_ALLOC`; `GSP_RM_CONTROL` carries the same cmd+params
   a userspace control would. These re-issue ~1:1 on the host through the isolate.
   **`nvkvm_m2_shadow_fwd` already does this** — it replays the guest's alloc stream on the host
   stub, which is why the host channel / compute object (`NVC7C0`) are created with **real host
   handles** and the host kernel-RM promotes the host channel's GR context itself.

2. **Case 2 — ROUTE_TO_PHYSICAL / GSP-internal controls with no userspace equivalent**
   (e.g. `NV2080_CTRL_CMD_GPU_PROMOTE_CTX` `0x2080012b`). These have no userspace ioctl because
   they only exist inside the kernel-RM → GSP path. **Do not replay them on the host** — an
   unprivileged userspace process issuing one gets `NV_ERR_INSUFFICIENT_PERMISSIONS (0x1b)`.
   Their *effect* is already achieved by the Case-1 forwarding (the host kernel-RM did its own
   PROMOTE_CTX for the host channel when we forwarded the channel/object alloc). Correct handling:
   **ack the guest (satisfy its post-op completion poll), do nothing on the host.**

## The `0x1b` lesson

`NV_ERR_INSUFFICIENT_PERMISSIONS` from the host stub is **not** "we lack a privilege we should
have." Normal userspace RM ops (alloc / map / submit) are fully unprivileged and work. `0x1b`
means **we forwarded at the wrong layer** — we tried to replay a Case-2 (privileged, GSP-internal)
control as a userspace control. The fix is never "gain privilege"; it is "translate back up to the
userspace intent" (usually Case-1 already did it, so: ack-only).

## Implications / current gaps

- **`PROMOTE_CTX` host-forward (M6.4) is wrong** — it is a Case-2 control. It should be ack-only.
  The host channel is already promoted by the host kernel-RM during the forwarded alloc.
- **The real remaining work is the *submission* intent**, not context setup:
  - guest "run this work" = it writes `GP_PUT` / rings its channel doorbell;
  - correct translation = mirror that into the **already-real host channel's** USERD and ring the
    **host** doorbell (the GP_PUT bridge), then let the host GPU's **real completion** (the GPU
    DMA-writing the real semaphore) flow back **unchanged**;
  - the completion semaphore must live in **shared, untrapped, WB-coherent sysmem** (the shared
    `memfd` page the host GPU DMAs to and the guest polls natively) — never a QEMU-trapped or
    emulated-FB page. A completion is a *real host-GPU write*, never a forged value (per the
    map-vs-stub rule). See `mode2_memory_model.md` and `mode2_compute_forwarding.md`.
  - `hostUSERD put=0` on the compute client = the submission intent is not reaching the host
    channel; that is the live keystone, not context promotion.

## Anti-patterns (do not do these)

- Replaying a ROUTE_TO_PHYSICAL / GSP-internal control on the host stub.
- Forging a *completion value* the guest's userspace observes (forge only what is provably
  guest-kernel-internal and content-irrelevant; completions that gate userspace must be real host
  writes — see the CE-scrubber vs compute distinction in `mode2_execfwd_keystone_plan.md`).
- Treating a non-zero host status as "the bug" without first asking whether we should have issued
  that host op at all (Case-2 → we shouldn't).
