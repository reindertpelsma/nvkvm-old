# Night job result — 2026-05-26

Three commits pushed:

- `b3b5a29` — strip install_isolate_mapping seccomp path + UVM_MM_INITIALIZE mask. -1332 / +15 lines.
- `47c61c5` — docs/ARCHITECTURE.md + scripts/run_remote_test.sh harness.
- `8ead07e` — docs/CUINIT_BLOCKER.md updated with RM_MAP_MEMORY hypothesis.

## State

- `test_ioctl_fwd`: 48/48 PASS (unchanged — confirmed after every commit).
- `cuinit_test`: returns 1 (cuInit FAILED 100 — no CUDA-capable device).
- Build pipeline: `scripts/run_remote_test.sh {restart, rebuild, both, log [pattern], test_ioctl_fwd, cuinit_test}` works end-to-end.

## Why cuInit still fails

All ioctls succeed individually. libcuda runs through:

```
[...isolate spawn, RM_ALLOC root, lots of RM_CONTROL setup...]
RM_MAP_MEMORY (cmd 0xc038464e)  →  ret=0, nvstatus=0x0
[libcuda does cleanup ioctls and exits]
```

Host trace diverges exactly at RM_MAP_MEMORY — host continues to
UVM_REGISTER_GPU_VASPACE / UVM_REGISTER_GPU and `mmap(/dev/nvidia0, ...)`.
Our guest doesn't.

Hypothesis: RM_MAP_MEMORY returns `pLinearAddress` in the stub's mm
(where the stub will later mmap the nvidia0 region). libcuda reads
that VA in its own (guest userspace) mm, finds nothing valid there,
concludes the device is unusable. Diagnostic logging for
`pLinearAddress` is in place to confirm/refute (last commit didn't
push it — see `nvkvm_isolate_handlers.c` if I left it in dirty state).

## What's next

The "no device" needs the mmap-collapse architectural refactor laid out in:

- `docs/ARCHITECTURE.md` → "Known wrong" items 1–4
- `docs/CUINIT_BLOCKER.md` → "The architectural fix needed"

Concretely:
1. Stub opens nvidia fds (today QEMU does).
2. Stub SCM_RIGHTS the fd to QEMU.
3. On RM_MAP_MEMORY (or any nvidia mmap path): stub mmaps + QEMU mmaps the same fd; QEMU calls `KVM_SET_USER_MEMORY_REGION(QEMU_VA → GPA)`; the GPA gets returned to libcuda disguised as a VA, then guest module's `.mmap` fop does `vm_insert_pfn(guest_VA → GPA)`.

This is multi-day work and crosses too many design calls to do unattended. Whoever picks this up should read `docs/ARCHITECTURE.md` start to finish first.

## Things I did NOT do (intentional)

- Did not touch the stub-local UVM pool (`uvm_local_fds[]`). Still keeps UVM mm-ownership correct on the SCM_RIGHTS path. Will go away when stub becomes the nvidia-fd opener (item 1 above).
- Did not delete the `kvm_vm_fd` discovery in `virtio_nvgpu_device_realize`. Restored after initially removing because the legitimate nvidia0 mmap path in `nvkvm_isolate_handlers.c` uses it.
- Did not push the diagnostic `pLinearAddress` logging in this commit set unless it shows something interesting. If it does, expect a 4th commit with that.
