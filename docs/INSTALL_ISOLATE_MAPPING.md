# install_isolate_mapping — design

Decided 2026-05-25 with user. This is the chosen architecture for letting
the guest expose isolate-side memory regions (from UVM, RM_MAP_MEMORY,
and similar ioctls) at guest physical addresses without QEMU having to
understand each ioctl's mapping semantics.

## Why this exists

The host nvidia driver — for several ioctls — mutates the calling process's
`mm`: it inserts VMAs, programs page tables, or installs eventfd
notifications keyed on the caller's task. Examples:

- `UVM_MAP_EXTERNAL_ALLOCATION` / `UVM_CREATE_EXTERNAL_RANGE` / `UVM_FREE`
- `UVM_REGISTER_GPU_VASPACE` / `UVM_REGISTER_CHANNEL`
- `NV_ESC_RM_MAP_MEMORY` (the in-process mapping the driver does before
  returning `pLinearAddress`)
- `NV_ESC_RM_ALLOC_OS_EVENT` (event delivery is tied to the caller's
  process)

In our architecture the *stub* is the process making these syscalls. So the
mappings end up in the stub's mm, not in the guest CUDA process's mm. The
guest userspace can't dereference VAs that exist only in the stub.

The chosen solution is **not** for QEMU to probe the stub's mm at fault
time. It's for the guest kernel — which intercepts these ioctls anyway and
knows their input arguments verbatim — to *tell QEMU exactly which (gva,
size) ranges to expose at what GPA*. QEMU validates and forwards the
install command to the stub, which calls
`KVM_SET_USER_MEMORY_REGION`; a seccomp user_notif catches the actual
syscall and lets QEMU re-validate before the kernel commits the call.

The data path is one-way per install/uninstall; there's no question/answer
RPC. The stub never has to inspect anything to figure out what's exposable
— that decision was made by the guest kernel when it intercepted the
ioctl.

## Wire format

### Guest kernel → QEMU (over virtio control queue)

```c
struct nvkvm_req_install_isolate_mapping {
    uint32_t type;             /* NVKVM_REQ_INSTALL_ISOLATE_MAPPING */
    uint32_t reserved;
    uint32_t session_id;
    uint32_t isolate_id;
    uint64_t gva;              /* in the isolate's mm — same number as in
                                  the guest CUDA process's mm */
    uint64_t size;
    uint64_t gpa_target;       /* 0 = let QEMU pick from the mmap window */
    uint32_t prot;             /* PROT_READ | PROT_WRITE (RO map permitted) */
    uint32_t flags;            /* reserved */
};

struct nvkvm_resp_install_isolate_mapping {
    uint32_t type;
    uint32_t status;           /* 0 on success, -errno on failure */
    uint64_t gpa;              /* the GPA QEMU chose (== gpa_target if non-0
                                  and accepted); 0 on failure */
};

struct nvkvm_req_uninstall_isolate_mapping {
    uint32_t type;             /* NVKVM_REQ_UNINSTALL_ISOLATE_MAPPING */
    uint32_t reserved;
    uint32_t session_id;
    uint32_t isolate_id;
    uint64_t gva;
    uint64_t size;             /* must match a prior install */
};
```

**Idempotency.** Multiple installs with the same `(isolate_id, gva,
size)` resolve to the same GPA without doubling the slot. Re-sends are
safe — UVM ioctls can be retried by libcuda, and we want this to be
robust against the guest module accidentally double-sending.

### QEMU → stub (over the existing SOCK_SEQPACKET socket)

```c
#define ISOLATE_CMD_INSTALL_MAPPING   10
#define ISOLATE_CMD_UNINSTALL_MAPPING 11
#define ISOLATE_RESP_MAPPING          0x15

struct isolate_cmd_install_mapping {
    uint32_t type;
    uint32_t slot;        /* the KVM memory slot QEMU pre-allocated */
    uint64_t gva;         /* userspace_addr for KVM_SET_USER_MEMORY_REGION */
    uint64_t size;
    uint64_t gpa;
    uint32_t prot;
    uint32_t flags;
};

struct isolate_cmd_uninstall_mapping {
    uint32_t type;
    uint32_t slot;
    uint64_t gpa;         /* for cross-check */
    uint64_t size;
};

struct isolate_resp_mapping {
    uint32_t type;
    uint32_t status;
};
```

The stub's job:

1. Receive ISOLATE_CMD_INSTALL_MAPPING.
2. `ioctl(kvmfd, KVM_SET_USER_MEMORY_REGION, {.slot, .gpa, .size,
   .userspace_addr=gva, .flags = (prot & PROT_WRITE) ? 0 : KVM_MEM_READONLY})`.
3. Reply ISOLATE_RESP_MAPPING with `status`.

The `kvmfd` arrives via SCM_RIGHTS at spawn (see "Stub spawn changes").

## QEMU's whitelist + seccomp user_notif

The stub's seccomp filter is augmented:

- For `ioctl` syscalls where `args[0] == kvmfd_in_stub` AND
  `args[1] == KVM_SET_USER_MEMORY_REGION` (or `_EXT` variant):
  return `SECCOMP_RET_USER_NOTIF`.
- All other syscalls keep their current allow/deny disposition.

At stub spawn QEMU receives the seccomp notify fd (via
`seccomp(SECCOMP_FILTER_FLAG_NEW_LISTENER, …)` and SCM_RIGHTS). A QEMU
thread `select`s on this fd plus the existing per-isolate socket.

When a user_notif arrives:

```
1. SECCOMP_IOCTL_NOTIF_RECV  → struct seccomp_notif {id, pid, data{nr, instruction_pointer, args[6]}}
2. Read args from the notif. arg2 is the userspace_addr of the
   kvm_userspace_memory_region in the stub.
3. Read the struct from the stub's mm via /proc/<stub-pid>/mem
   (or seccomp_notif_addfd if we want to be fancy; the standard
   way is just to peek the addr).
4. Look up the whitelist entry for (slot, gpa, size, userspace_addr).
   If no exact match: respond with errno=EPERM and log.
5. If match: SECCOMP_IOCTL_NOTIF_SEND with CONTINUE.
6. Wait for the syscall to complete from the kernel side (the
   listener fd will signal completion via SECCOMP_USER_NOTIF_FLAG_CONTINUE
   handling); remove the whitelist entry once the call returns.
```

A few invariants the validation must enforce:

- `slot` matches a slot QEMU reserved for this stub (per-isolate slot
  range; cross-isolate aliasing forbidden).
- `gpa` is inside the mmap window (`NVKVM_MMAP_WIN_GPA_BASE`..`+size`).
- `size` matches the whitelist entry exactly.
- `userspace_addr` matches the whitelist entry exactly.
- A re-read of `/proc/<stub>/maps` shows the VA range is backed by a
  VMA whose path is `/dev/nvidia*` or `/dev/nvidia-uvm` (no anon mappings
  the stub allocated for itself).

This last point is the provenance check from earlier conversations. We
*could* drop it — the install was guest-authorised — but it's cheap
defense in depth against a guest kernel attempting to expose a region
the isolate shouldn't be able to expose (e.g. a memfd QEMU itself
backed but the isolate received). Including it.

## Guest module UVM intercepts

The guest module already intercepts every nvidia ioctl. The new code
in `nvkvm_main.c::nvkvm_ioctl` is:

1. Save IN arguments (gva, size, etc.) before forwarding.
2. Forward to QEMU/stub.
3. If response is success AND the ioctl creates a mapping the guest
   needs to see, issue a follow-up
   `NVKVM_REQ_INSTALL_ISOLATE_MAPPING`. After the install response
   arrives, complete the original ioctl back to userspace.
4. For unmap ioctls (UVM_FREE, UVM_UNMAP_EXTERNAL, etc.), do the
   symmetric: first uninstall the mapping, then forward the unmap to
   the stub.

Ioctls that create mappings the guest must see (initial list — extend
as needed):

| ioctl                                       | nr / cmd       | extracts (gva, size) from              |
|--------------------------------------------|----------------|----------------------------------------|
| UVM_MAP_EXTERNAL_ALLOCATION                | 33             | params.base, params.length             |
| UVM_CREATE_EXTERNAL_RANGE                  | 73             | params.base, params.length             |
| UVM_REGISTER_GPU_VASPACE                   | 25             | mm-wide registration; no VA install (?)|
| UVM_REGISTER_CHANNEL                       | 27             | params.base, params.length             |
| UVM_MAP_DYNAMIC_PARALLELISM_REGION         | 65             | params.base, params.length             |
| UVM_ALLOC_SEMAPHORE_POOL                   | 68             | params.base, params.length             |
| UVM_VALIDATE_VA_RANGE                      | 72             | (read-only; no install)                |
| UVM_FREE                                   | 34             | params.base, params.length (uninstall) |
| UVM_UNMAP_EXTERNAL                         | 66             | params.base, params.length (uninstall) |
| UVM_DESTROY_RANGE_GROUP                    | 24             | tracking only                          |
| NV_ESC_RM_MAP_MEMORY                       | 0x4e           | post-call: pLinearAddress, length      |
| NV_ESC_RM_UNMAP_MEMORY                     | 0x4f           | post-call: pLinearAddress (uninstall)  |
| NV_ESC_RM_ALLOC_OS_EVENT                   | 0xce           | not a VA mapping; eventfd association  |

For RM_MAP_MEMORY specifically: the call to the host driver in the stub
returns `pLinearAddress = host VA in the stub's mm`. The guest module
issues install_isolate_mapping(gva=that_VA, size=length). Subsequent
`mmap(/dev/nvidia0, offset)` calls from CUDA in the guest are still
served by the existing virtio-mmap path; what install_isolate_mapping
adds is that the VA already used by the host driver's internal mapping
also becomes visible at the same GPA.

(For ioctls that don't map memory but only update tracking — e.g.
`UVM_REGISTER_GPU_VASPACE`, `UVM_VALIDATE_VA_RANGE` — no install call
is needed.)

## Stub spawn changes

```
Before exec:
  - socketpair() → sv[0] (QEMU) / sv[1] (stub stdin)
  - QEMU opens /dev/kvm vm fd if not already (cached on first device init)
  - SCM_RIGHTS sv[0] → stub: the kvm vm fd at a known slot
  - QEMU also creates the seccomp notify fd (passed via SCM_RIGHTS too)
  - dup2(sv[1], 0); exec(stub_path)

In stub main, before apply_seccomp():
  - receive kvmfd via SCM_RIGHTS (or it was already passed inherited)
  - open /proc/self/maps O_RDONLY|O_CLOEXEC into a long-lived fd
  - apply seccomp filter with NEW_LISTENER, get notify_fd
  - send notify_fd back to QEMU via the socket
  - install the filter
```

QEMU then watches the notify_fd for ioctl(kvmfd, KVM_SET_USER_MEMORY_REGION,
*) calls, validates each against the install whitelist, and CONTINUE/EPERM
appropriately.

## Test plan

1. **Unit test (in-process, no VM)**: a Go-style table-driven check
   on the QEMU whitelist data structure — add, lookup by (slot, gpa,
   size, va), remove, double-add (idempotent), conflicting add (reject).
2. **Stub-stub integration test**: spawn a stub with the new filter,
   feed it a synthetic ISOLATE_CMD_INSTALL_MAPPING, verify the
   `KVM_SET_USER_MEMORY_REGION` traps to user_notif and QEMU's
   validator returns CONTINUE.
3. **End-to-end with a fake UVM**: in the guest, write a small kmod
   that simulates a UVM_MAP_EXTERNAL_ALLOCATION call; verify the
   install_isolate_mapping RPC fires; verify the GPA window is live
   from inside the guest userspace.
4. **Regression**: `tests/integration/test_ioctl_fwd` must continue
   to pass all 48 checks.
5. **GPT-2**: with everything in place, `cuInit` reaches 0;
   `cuDeviceGetCount` returns 1; `python3 -c "import torch;
   torch.cuda.is_available()"` returns True; a small GPT-2 forward
   pass completes.

## What this does NOT do

- Doesn't change handle-based fd mappings (`mmap(/dev/nvidia0,
  offset)`). The existing virtio-mmap path stays as-is.
- Doesn't change the dispatch for ioctls that don't create mappings.
- Doesn't try to virtualise UVM. The host UVM driver still owns the
  GPU page tables; we only mirror its mm-side VMAs into the guest's
  GPA layout.

## What this enables for follow-up

- Once installs work for RM_MAP_MEMORY responses, the next blocker
  (driver returning different bytes per-process) goes away because
  the stub mm becomes the "canonical" CUDA process for the host
  driver's purposes.
- UVM `cuMemAllocManaged` becomes wireable later by the same
  intercept hook.
