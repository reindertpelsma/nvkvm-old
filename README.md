# nvkvm — NVIDIA GPU Passthrough for KVM Guests

WSL2-style NVIDIA GPU ioctl forwarding for KVM/QEMU VMs on commodity hardware,
without vGPU licensing or full PCI passthrough.

## What this is

On Windows, WSL2 gives Linux VMs GPU access via GPU-PV: the Windows NVIDIA
WDDM driver mediates all GPU work, and the Linux guest uses userspace stubs +
`/dev/dxg` instead of `nvidia.ko`. There is no equivalent for Linux-host KVM.

**nvkvm** implements the same model for Linux/KVM:

```
Guest VM
  App → libcuda.so (unmodified NVIDIA userspace)
        ↓ open/ioctl/mmap on /dev/nvidiactl, /dev/nvidia0, /dev/nvidia-uvm
  nvkvm-guest.ko (guest Linux kernel module)
        ↓ virtio-nvgpu transport (shared memory + virtqueues)
  QEMU virtio-nvgpu device (host)
        ↓ pointer translation + handle validation
  /dev/nvidiactl, /dev/nvidia0, /dev/nvidia-uvm (host)
        ↓
  NVIDIA driver + physical GPU
```

CUDA, cuDNN, PyTorch, etc. work unmodified inside the guest.

## Key design decisions

- **Ioctl forwarding, not vGPU**: no NVIDIA vGPU license required
- **No PCI passthrough**: GPU is shared across VMs and host
- **gVisor nvproxy as reference**: `pkg/sentry/devices/nvproxy/` implements
  the same forwarding for process sandboxing; we adapt it for VM boundaries
- **Two security boundaries**: QEMU validates guest kernel; guest kernel
  validates guest userspace. Neither trusts the other.
- **Zero-copy mmap path**: GPU memory pages are mapped directly into guest
  physical address space via `KVM_SET_USER_MEMORY_REGION`, not copied
- **Session isolation**: each guest process gets its own RM client scope;
  nvidia-smi in the guest only sees guest processes

## Project structure

```
src/
  common/nvkvm_proto.h      — virtio-nvgpu wire protocol
  abi/                      — NVIDIA driver ABI structs (ported from gVisor)
  guest/                    — nvkvm-guest.ko Linux kernel module
    nvkvm_main.c            — char device registration, open/release/ioctl/mmap
    nvkvm_ioctl.c           — param size table + sanitizer (zeroes guest pointers)
    nvkvm_virtio.c          — virtio frontend, shared memory, sync send/recv
    nvkvm_mmap.c            — mmap request → GPA window → remap_pfn_range
    nvkvm_session.c         — per-process session management
  qemu/                     — QEMU virtio-nvgpu device backend
    virtio_nvgpu.c          — virtio device, request dispatch
    nvkvm_dispatch.c        — ioctl command → handler + param size validation
    nvkvm_frontend.c        — RM alloc/free/control handlers with ptr translation
    nvkvm_objects.c         — RM object dependency graph (mirrors gVisor object.go)
    nvkvm_mmap_host.c       — host mmap + KVM_SET_USER_MEMORY_REGION
gvisor/                     — reference implementation (read-only)
tests/
  integration/test_basic.py — CUDA + PyTorch smoke tests
scripts/
  vastai_find_node.sh       — find KVM+GPU nodes on vast.ai
  vastai_setup.sh           — setup script for test node
  run_test_vm.sh            — launch QEMU with virtio-nvgpu device
```

## How ioctl forwarding works

1. Guest userspace calls `ioctl(fd, NV_ESC_RM_CONTROL, &params)`
2. `nvkvm_ioctl()` in the guest kernel module:
   - Validates `param_size` against `nvkvm_ioctl_param_size(cmd)` table
   - Copies params from userspace to kernel
   - **Sanitizes**: zeroes embedded pointer fields (guest VA → 0), translates
     embedded fd numbers from guest fd → `fd_token`
   - Writes params blob to a shared memory slot
   - Posts `NVKVM_REQ_IOCTL` to VQ_TX and blocks
3. QEMU `nvkvm_tx_handler()`:
   - Re-validates `param_size` against its own table
   - Validates `fd_token` → `session_id` ownership
   - **Substitutes** host pointers: secondary buffers from aux slot replace
     zeroed pointer fields
   - Calls `ioctl(host_fd, cmd, params)`
   - Restores zeroed pointer fields, writes result to VQ_RX
4. Guest wakes up, copies updated params back to userspace

## How mmap works

1. Guest userspace calls `mmap(addr, len, prot, MAP_SHARED, fd, offset)`
2. Guest kernel module forwards `NVKVM_REQ_MMAP` with offset + length
3. QEMU:
   - Calls `mmap(NULL, len, prot, MAP_SHARED, host_fd, offset)` → host VA
   - Allocates a GPA from the mmap window (BAR 1 reserved region)
   - Calls `KVM_SET_USER_MEMORY_REGION(gpa, hva, len)` → GPU pages now
     visible at that GPA
   - Returns GPA to guest
4. Guest kernel module calls `remap_pfn_range(vma, gpa >> PAGE_SHIFT)` →
   GPU pages mapped into process VA

The result: process reads/writes go directly to GPU memory (BAR or pinned
system RAM) with no bounce buffering.

## Building

### Guest kernel module
```bash
cd src/guest
make KDIR=/lib/modules/$(uname -r)/build
# Produces: nvkvm-guest.ko
```

### QEMU device
```bash
# Patch the QEMU source tree:
cp src/qemu/virtio_nvgpu.{c,h} <qemu>/hw/misc/
cp src/qemu/nvkvm_*.c          <qemu>/hw/misc/
# Add to <qemu>/hw/misc/meson.build:
#   system_ss.add(when: 'CONFIG_VIRTIO_NVGPU', if_true: files('virtio_nvgpu.c', ...))
# Configure and build QEMU normally.
```

## Testing

```bash
# Find a vast.ai node with KVM + GPU:
./scripts/vastai_find_node.sh

# Set up the node:
ssh root@<node> 'bash -s' < scripts/vastai_setup.sh

# Launch a test VM:
./scripts/run_test_vm.sh

# Inside the guest, load the module and run tests:
sudo insmod /mnt/nvkvm_module/nvkvm-guest.ko
python3 /mnt/nvkvm_tests/integration/test_basic.py -v
```

## Reference: gVisor nvproxy

The core ioctl-forwarding logic is heavily inspired by gVisor's nvproxy
(`pkg/sentry/devices/nvproxy/`). Key files to read for understanding the
design:

- `frontend.go` — `rmAlloc`, `rmControl`, `rmFree` with pointer translation
- `object.go` — RM object dependency graph
- `version.go` — versioned ABI dispatch tables
- `uvm.go` / `uvm_mmap.go` — UVM ioctl + mmap handling

The primary difference: gVisor translates pointers within one process address
space (sentry VA = host VA), while nvkvm crosses a VM boundary (guest PA →
host VA via KVM memory slots).

## Limitations / TODO

- [ ] UVM fd translation in `REGISTER_GPU_VASPACE` / `REGISTER_CHANNEL`
- [ ] Async event relay: host eventfd → VQ_EVT → guest poll wake
- [ ] QEMU BAR exposure for shared memory and mmap window
- [ ] Multi-GPU support (tested with 1 GPU)
- [ ] Driver version negotiation (currently pinned to detected host version)
- [ ] Checkpoint/restore (gVisor captures alloc params for this)
- [ ] VFIO integration for strict IOMMU passthrough on DMA operations
