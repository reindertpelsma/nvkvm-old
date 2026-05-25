# nvkvm Architecture

This document describes the current architecture of nvkvm — a project that
forwards CUDA workloads from a KVM guest VM out to the host's real NVIDIA
GPU, without disabling the host's GPU usage. It records the *current*
shape of the system, the constraints that drove it, the things we know
are wrong and need to change, and the things we know are right but
haven't built yet.

If you are reading this to onboard, start here, then read
`docs/CUINIT_BLOCKER.md` for the deepest known issue.

## Goal

Run unmodified CUDA workloads (ultimately, ChatGPT-scale model inference)
inside a KVM guest VM, while the host process tree continues to use the
GPU for display + other apps. The host's nvidia driver remains loaded
and unmodified.

## Three-tier process model

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │ guest VM (Linux + nvkvm-guest.ko + libcuda)                          │
  │                                                                       │
  │   libcuda → /dev/nvidiactl /dev/nvidia0 /dev/nvidia-uvm               │
  │     │                                                                  │
  │     │  ioctl/mmap                                                      │
  │     ▼                                                                  │
  │   nvkvm-guest.ko ── virtio-nvgpu ── (TX/RX queues, shm slots) ─┐     │
  │                                                                  │     │
  └──────────────────────────────────────────────────────────────────│─────┘
                                                                     │
  ┌──────────────────────────────────────────────────────────────────┴─────┐
  │ host: QEMU process (one per VM)                                          │
  │                                                                          │
  │   virtio_nvgpu device                                                    │
  │     │  dispatch: nvkvm_req_*                                             │
  │     ▼                                                                    │
  │   nvkvm_isolate_handlers.c ────────────────┐                            │
  │                                              │                            │
  │   KVM_SET_USER_MEMORY_REGION (QEMU's mm     │ SOCK_SEQPACKET             │
  │   only — kernel enforces kvm->mm equality)  │ + SCM_RIGHTS               │
  │                                              ▼                            │
  └──────────────────────────────────────────────│────────────────────────────┘
                                                 │
                                                 │  per-isolate
                                                 │
  ┌──────────────────────────────────────────────┴────────────────────────────┐
  │ host: stub process (one per guest mm/isolate)                              │
  │                                                                            │
  │   nvkvm_stub (sandboxed)                                                   │
  │     │  ioctl/mmap on locally-opened nvidia fds                             │
  │     ▼                                                                      │
  │   /dev/nvidiactl /dev/nvidia0 /dev/nvidia-uvm (real nvidia driver)         │
  │                                                                            │
  │   Stub's VA layout deliberately mirrors the guest's userspace VAs so       │
  │   pointer fields in nvidia ioctl structs (e.g. nvos54.params,              │
  │   UVM_MAP_EXTERNAL_ALLOCATION.base) dereference correctly inside the       │
  │   nvidia driver.                                                           │
  └────────────────────────────────────────────────────────────────────────────┘
```

The stub is *the* talker to the nvidia kernel driver. QEMU is the trusted
boundary that owns KVM and orchestrates the stub. The guest never talks
to nvidia directly — it talks to a virtio device that looks like nvidia.

## Constraint table (verified)

| Operation | mm enforcement | Where it has to run |
|-----------|----------------|---------------------|
| `KVM_SET_USER_MEMORY_REGION` | Strict `kvm->mm == current->mm` (verified by `tests/integration/kvm_sparse_test.c`) | QEMU |
| RM ioctls (`nvidiactl`, `nvidia0`: NV_ESC_*) | None observed | Anywhere |
| RM mmap on `nvidia0` | None observed (probably) | Anywhere |
| UVM_INITIALIZE | None (NO_INIT_CHECK macro). va_space.mm is set to current->mm here. | Same process throughout the UVM lifetime |
| UVM_MM_INITIALIZE | None on calling task; uses va_space.mm | Optional — returns NV_WARN_NOTHING_TO_DO (0x10006) on this driver build, libcuda handles it |
| Other UVM ioctls (INIT_CHECK) | Just need uvm_fd_va_space(filp) non-null | Same process that ran UVM_INITIALIZE |
| UVM VA-based ioctls (UVM_CREATE_EXTERNAL_RANGE, UVM_MAP_EXTERNAL_ALLOCATION, UVM_FREE, UVM_MIGRATE…) | base/length is interpreted in current->mm | Same process that mmap'd the VA |
| UVM mmap | If MM tracking enabled: strict `va_space.mm == current->mm`. On this build: not enforced. | Same process that ran UVM_INITIALIZE (recommended) |

The hard constraints are: KVM regions in QEMU's mm; UVM lifecycle in one
task. Everything else is flexible.

## Two key invariants (verified by integration tests)

1. **KVM accepts sparse memory regions**.
   `tests/integration/kvm_sparse_test.c` allocates 8 GiB `MAP_NORESERVE`,
   calls `KVM_SET_USER_MEMORY_REGION` on it, and runs a tiny VM that touches
   one page. The host kernel demand-faults each accessed page; the guest
   never sees a fault. **Consequence**: UVM-managed memory (lazy pages
   via HMM/mmu_notifier or just sparse anon) can be installed as a KVM
   region without pre-faulting.

2. **`kvm->mm == current->mm` is enforced with `-EIO`**.
   Same test: a `clone(CLONE_FILES)` child sharing the kvm_fd but with
   its own mm gets EIO from KVM_SET_USER_MEMORY_REGION. **Consequence**:
   the stub cannot call this syscall via seccomp `USER_NOTIF | CONTINUE`
   — the syscall runs in the stub's task whose mm differs from QEMU's.
   KVM region installs must go through a stub→QEMU RPC, not a syscall
   trap.

## What the stub does today

- Forks from QEMU at first isolate-create. Inherits no nvidia fds.
- Pre-opens `/dev/nvidia-uvm` a few times before seccomp (`uvm_local_fds[]`)
  so the stub's mm owns the UVM file when ioctls run on it.
- Receives `nvidiactl` / `nvidia0` fds via SCM_RIGHTS from QEMU (legacy —
  these would ideally also be stub-opened; see "Known wrong" below).
- Applies a seccomp allow-list filter; everything outside the list returns
  EPERM. The list is small (read, write, recvmsg, sendmsg, ioctl, mmap,
  mprotect, munmap, ppoll, close, exit_group, sigaction, sigreturn, futex,
  clone, set_robust_list, madvise, lseek, pread64, openat for the UVM
  pre-opens, plus a few others).
- Workers dequeue ioctl jobs from a queue, look up fd by handle_id,
  patch embedded fd fields in UVM ioctls (UVM_MM_INITIALIZE.uvm_fd,
  UVM_REGISTER_GPU_VASPACE.rm_ctrl_fd, etc.) from handle_id to local fd,
  call `ioctl()`, send the response.
- Worker threads do mmap on nvidia fds when the stub gets an
  `ISOLATE_CMD_MMAP` from QEMU. Currently these are at QEMU-chosen GVAs
  via `MAP_FIXED`.

## What QEMU does today

- Listens on virtio queues; dispatches `NVKVM_REQ_*` messages.
- Holds a handle table (`struct nvkvm_handle_table`) mapping handle_id to
  the host-side fd it opened on the guest's behalf.
- For session/isolate management, spawns/kills stubs.
- For *mmap* on `nvidia0` only: opens the fd, mmaps it into QEMU's mm,
  calls `KVM_SET_USER_MEMORY_REGION(QEMU_VA → GPA)`, then asks the stub
  to ALSO mmap at the guest VA so the nvidia driver knows about the
  mapping. This is the "double mmap" model; it stays as-is for nvidia0
  for now.

## Known wrong (we know it, we just haven't fixed it)

These are the items we'd fix in the next refactor:

1. **QEMU opens `nvidia0`, `nvidiactl`, `nvidia-uvm` first and SCM_RIGHTS them to the stub.**
   This was the original design. We've since established that the stub
   should be the opener (so the file's owning mm matches the calling
   task, which is important for UVM and conjecturally for some RM
   operations on future drivers). The stub-local UVM pool is a partial
   fix for UVM specifically. The right answer is: stub opens, stub
   SCM_RIGHTS *up* to QEMU when QEMU needs the fd (for KVM region
   installs).

2. **No `nvidia_uvm_mmap` RPC.**
   Right now the guest's `mmap(/dev/nvidia-uvm, ...)` doesn't really
   route to UVM. To support managed memory we need a virtio RPC where the
   guest tells QEMU "mmap this UVM fd at offset X for size Y at GPA Z",
   QEMU does mmap + KVM_SET_USER_MEMORY_REGION + any required UVM VA
   ioctls, returns GPA to the guest. The guest module then
   `vm_insert_pfn`'s the guest VA → GPA.

3. **VA-based UVM ioctls (UVM_MAP_EXTERNAL_ALLOCATION, UVM_FREE,
   UVM_MIGRATE, UVM_SET_PREFERRED_LOCATION, UVM_CREATE_EXTERNAL_RANGE,
   UVM_REGISTER_CHANNEL when it includes a VA) need to run in QEMU**,
   because the `base` VA is a QEMU-mm address once we move UVM mmap to
   QEMU. Today the stub does these and they only work for the no-VA
   cases.

4. **No fd-translation layer in QEMU for UVM ioctls' embedded
   `rm_ctrl_fd`.** When VA-based UVM ioctls move to QEMU, QEMU will see
   `rm_ctrl_fd = guest_token`. Needs a translation step: token →
   handle_id → QEMU's local RM fd. (Stub already does the equivalent
   handle_id→local-fd translation today.)

5. **Stub seccomp filter is permissive.** It's an allow-list, but the
   list is wider than necessary. Per `isolate_hardening_todo.md`: drop
   to a minimal set, restrict openat (USER_NOTIF + validator in QEMU,
   or pre-open O_PATH refs + restrict openat to AT_EMPTY_PATH reopens),
   namespace isolation (user, mount, pid, net, ipc, uts), drop all caps,
   no_new_privs, suid_dumpable=0.

## Known *right* but not built yet

These are agreed design decisions that haven't landed:

- **memfd-backed CPU mmap path** for `cuMemHostAlloc`-style shared CPU
  buffers (your "flow 2"). Future RPC: guest asks for "shared CPU buffer
  of size N at GPA X", QEMU creates a memfd, mmaps into QEMU's mm, KVM
  region installed, fd SCM_RIGHTS'd to stub for symmetric access.

- **HMM-mode UVM**. The driver's `UVM_CAN_USE_MMU_NOTIFIERS()` conftest
  is false because it looks for a renamed kernel callback. Forcing it
  true via a rebuilt nvidia-uvm.ko would unlock UVM-on-any-mmap (i.e.
  memfd-backed UVM). Major effort (custom driver build), big payoff
  (cleaner architecture, smaller fault surface). Out of scope until we
  hit a real reason to.

## Current cuInit status

- `tests/integration/test_ioctl_fwd`: 48/48 PASS — the RM ioctl
  forwarding pipeline is solid.
- `tests/integration/cuinit_test`: returns 1 (cuInit FAILED 100 — no
  CUDA-capable device detected). cuInit reaches device enumeration but
  libcuda reports no devices. Investigation pending; see
  `docs/CUINIT_BLOCKER.md`.

## Files to know

- `src/abi/` — nvidia ABI structs (ioctl param types, status codes).
  Mostly transcribed from gVisor's nvgpu package, kept in C.
- `src/common/` — virtio + isolate protocol headers (request/response
  structs shared by guest, QEMU, stub).
- `src/guest/` — `nvkvm-guest.ko` Linux kernel module.
- `src/qemu/` — patches to QEMU that add `virtio-nvgpu` device.
- `src/stub/` — sandboxed userspace stub binary that does the actual
  nvidia ioctls.
- `tests/integration/test_ioctl_fwd.c` — end-to-end RM ioctl test.
- `tests/integration/cuinit_test.c` — minimal cuInit / cuDeviceGetCount
  test; the "is the GPU usable" smoke test.
- `tests/integration/kvm_sparse_test.c` — host-only test proving KVM
  sparse-region semantics and the kvm->mm strict requirement.
- `scripts/run_remote_test.sh` — wrapper that ssh's to the vast.ai host
  to rebuild + run tests. Single command: `rebuild`, `test_ioctl_fwd`,
  `cuinit_test`, `both`, `log <pattern>`, `restart`.

## Reference setup (vast.ai)

Tested on:
- Host: vast.ai instance with RTX 3060 + NVIDIA driver 575.51.03 + kernel
  6.8.0-59-generic
- Guest: Ubuntu 24.04 cloud image, kernel 6.8.0-117-generic (or
  whatever's latest; module is rebuilt against the running kernel)
- The 9p mount tag `nvkvm_src` exposes the repo root to the guest

See `scripts/run_test_vm.sh` for the QEMU command line.
