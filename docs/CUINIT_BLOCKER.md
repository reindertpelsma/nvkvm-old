# cuInit "no device" blocker — host vs gVisor vs nvkvm comparison

This is a forensic breakdown of where each implementation diverges on the
UVM call sequence that cuInit issues during device probe.

## The sequence libcuda issues (verified via strace)

For driver 575.51.03 + CUDA 12.9 libcuda, the relevant tail of the cuInit
syscall trace is identical on every implementation up to UVM:

```
openat("/dev/nvidia-uvm", O_RDWR|O_CLOEXEC) = 9     # primary UVM fd
openat("/dev/nvidia-uvm", O_RDWR|O_CLOEXEC) = 10    # secondary (MM tracker)
ioctl(9,  _IO(0, 0x01), &p_init)                    # UVM_INITIALIZE
ioctl(10, _IO(0, 0x4b), &p_mm_init)                 # UVM_MM_INITIALIZE
ioctl(9,  _IO(0, 0x27), &p_pageable)                # UVM_PAGEABLE_MEM_ACCESS
ioctl(9,  _IO(0, 0x25), ...)                        # UVM_REGISTER_GPU_VASPACE
ioctl(9,  _IO(0, 0x17), ...)                        # UVM_REGISTER_GPU
```

cuInit gates `cuDeviceGetCount > 0` on `UVM_REGISTER_GPU` succeeding,
which requires `UVM_PAGEABLE_MEM_ACCESS` to have run on a properly
initialised primary UVM fd.

## What the kernel UVM driver actually checks

From `/usr/src/nvidia-575.51.03/nvidia-uvm/uvm.c::uvm_api_mm_initialize`:

```c
uvm_file = fget(params->uvmFd);                     // primary fd
if (!uvm_file_is_nvidia_uvm(uvm_file)) → INVALID_ARGUMENT
if (uvm_fd_type(uvm_file, ...) != UVM_FD_VA_SPACE)  → INVALID_ARGUMENT
if (!uvm_va_space_mm_enabled(va_space))             → WARN_NOTHING_TO_DO  (0x00010006)
```

`uvm_va_space_mm_enabled` returns false if:
- the va_space was opened with `UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE`, **or**
- `UVM_CAN_USE_MMU_NOTIFIERS() && uvm_enable_va_space_mm` is false at the
  system level (controlled by a kernel conftest at module build).

`UVM_PAGEABLE_MEM_ACCESS` is wrapped by `UVM_ROUTE_CMD_STACK_INIT_CHECK`,
which fails the call to NV_ERR_ILLEGAL_ACTION (in `params.rmStatus`, not
as syscall errno) if `uvm_fd_va_space(filp)` returns NULL — i.e. if the
filp wasn't UVM_INITIALIZE'd on this exact open.

The kernel UVM ioctl handler never returns `-EBADF` itself; the only
syscall errnos it emits are `-EFAULT` (copy-from/to-user) and `-EAGAIN`
(power-management trylock).

## Host (vanilla)

- libcuda's process opens both UVM fds, calls all three ioctls.
- File ownership: same process, same mm — passes `current->mm == file->mm` checks.
- `UVM_INITIALIZE` succeeds → va_space attached to fd 9.
- `UVM_MM_INITIALIZE` returns NV_WARN_NOTHING_TO_DO (0x10006) because
  this driver build has `UVM_CAN_USE_MMU_NOTIFIERS() = 0`. libcuda
  treats the warning as "MM FD not needed" and continues per the
  driver's documented contract.
- `UVM_PAGEABLE_MEM_ACCESS` succeeds because the same filp (fd 9)
  has a va_space; `params.pageableMemAccess` is set to false (no MM
  tracking) but rmStatus is NV_OK.
- cuInit proceeds; device is enumerated.

## gVisor nvproxy (`pkg/sentry/devices/nvproxy/uvm.go`)

- Everything runs in the sentry process. The application's "fd 9" is a
  gVisor virtual fd that wraps a host fd that the sentry opens at
  `/dev/nvidia-uvm`.
- `uvmMMInitialize` translates `params.UvmFD` from the sentry's
  fd-table token to the corresponding host fd, then invokes the host
  ioctl on the same sentry process.
- Owning mm is the sentry process's mm for both fds; same task calls
  all UVM ioctls.
- Same NV_WARN_NOTHING_TO_DO observed; libcuda handles it.

## Our nvkvm setup (before today)

- QEMU opens `/dev/nvidia-uvm` and passes the fd to the isolate via
  `SCM_RIGHTS`. The file's owning mm is **QEMU's mm**, not the
  isolate's.
- `UVM_INITIALIZE` succeeded (the macro is `NO_INIT_CHECK`, no mm check).
- `UVM_MM_INITIALIZE`: kernel does `fget(params->uvmFd)` in the
  isolate's task → gets QEMU-owned file → `current->mm` mismatch
  internally → returns **0x1f (NV_ERR_INVALID_ARGUMENT)**.
- We masked 0x1f to 0 in QEMU dispatch so libcuda would continue, but
  the kernel UVM driver's internal va_space state for the primary fd
  is still incomplete because MM_INITIALIZE didn't fully establish
  the mm binding.
- Subsequent `UVM_PAGEABLE_MEM_ACCESS` would set
  `params.rmStatus = NV_ERR_ILLEGAL_ACTION` (still ret 0), and libcuda
  fails downstream.

## Our nvkvm setup (after stub-local UVM opens)

- The stub now `openat("/dev/nvidia-uvm")` itself, before seccomp, so
  the file's owning mm matches the stub task.
- `UVM_INITIALIZE` succeeds: va_space attached.
- `UVM_MM_INITIALIZE` returns **0x10006 (NV_WARN_NOTHING_TO_DO)** —
  same as host! The kernel sees both fds in the same mm and just says
  "MM FD not needed."
- We short-circuit `UVM_MM_INITIALIZE` entirely in QEMU now (don't
  forward to the kernel) so the driver state isn't perturbed.
- **`UVM_PAGEABLE_MEM_ACCESS` returns intermittently 0 OR -1 EBADF.**

## Where the intermittent EBADF comes from

This is the open puzzle. Verified by strace + dmesg side-by-side on
the same build:

- The strace ioctl(9, 0x27) returns -1 EBADF, but `nvkvm_ioctl_unlocked`
  (our guest kernel module's `unlocked_ioctl` handler) is **never
  entered** for that call — its `pr_warn` doesn't fire even though it
  does fire for `UVM_INITIALIZE` and `UVM_DEINITIALIZE` on the same fd
  in the same run.
- fd 9 is **not closed** between the calls (no `close(9)` in strace,
  and the subsequent `UVM_DEINITIALIZE` on fd 9 succeeds).
- The error must therefore be `filp->private_data == NULL` at the
  start of `nvkvm_ioctl_unlocked` (line 389 returns `-EBADF` exactly
  there), reached without our `pr_warn` because that's placed after
  the null check.
- Why `private_data` would intermittently be NULL after a successful
  `open()` (which sets it on the `done:` label and always returns 0)
  is the next thing to verify. Suspects, in order:
    1. A use-after-free where `release()` runs and clears
       `private_data` while another fd still holds the file. The two
       UVM opens share an inode; if our `release()` is wired to the
       inode rather than the file, closing the secondary fd would
       free the primary's ctx. (Most likely.)
    2. The isolate's `nvkvm_session` is torn down by a concurrent
       isolate-kill path, which frees ctx through the session.
    3. A virtio response from QEMU racing the UVM call's setup
       overwrites the slot.

## Symbols that confirm the diagnosis above

Run sequence (multiple back-to-back invocations):

```
run 1: cuInit FAILED: 100 (no device — UVM_PAGEABLE_MEM_ACCESS = 0)
run 2: cuInit FAILED: 999 (UVM_PAGEABLE_MEM_ACCESS = -EBADF)
run 3: cuInit FAILED: 999
run 4: cuInit FAILED: 100
run 5: cuInit FAILED: 100
```

dmesg for the 999 runs has UVM_INITIALIZE + UVM_MM_INITIALIZE + UVM_DEINITIALIZE traces only. UVM_PAGEABLE_MEM_ACCESS never reaches our module.

dmesg for the 100 runs has all four UVM ioctls traced.

## Next step

Add a check at the top of `nvkvm_ioctl_unlocked` that logs `filp` /
`ctx` (and the calling tgid) when ctx is NULL, plus instrument
`nvkvm_release` to log which fd it's releasing. That should
positively identify the use-after-free path.
