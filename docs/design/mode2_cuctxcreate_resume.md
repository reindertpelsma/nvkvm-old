# Mode-2 cuCtxCreate / UVM Dataplane Resume (2026-06-07)

This is the current handoff for branch `mode-2`. A fresh session with repo access and the `vh`/`vg`
SSH aliases can resume from this document alone.

## 0. Project Context

Mode-2 runs the stock open NVIDIA driver 580.159.04 inside a KVM/QEMU guest against the emulated
GA106 device `nvkvm-gpu-emul` in `src/qemu/nvkvm_gpu_emul.c`. QEMU forwards real GPU work to the
host RTX 3060 through the unprivileged isolate/stub path.

Standing constraints:

- Host GPU forwarding is the only supported Mode-2 path. Keep `m2fwd` and `m2exec` default-on.
- Host GPU tests are strictly serial. Kill old QEMU/stub processes before a new run.
- Use a fresh QEMU boot for each clean CUDA run. A second `nvidia.ko` load in the same VM commonly
  hits dirty GSP/WPR state and returns `cuInit 999`.
- The emulated GPU must be on q35 root slot `addr=0x7`.
- Debug plumbing is allowed for bring-up, but production fixes must not depend on LD_PRELOAD or a
  trusted guest userspace. Debug code should eventually be gated behind `NVKVM_MODE2_DEBUG`.
- If committing another milestone, update this file first.

## 1. Current Status

Guest CUDA now reaches:

- `cuInit(0)` PASS.
- Device query path PASS (`RTX 3060`, compute 8.6, 11909 MiB).
- `cuCtxCreate` PASS.
- `cuMemAlloc` PASS.
- A 4-byte `cuMemcpyHtoD` / `cuMemcpyDtoH` round-trip PASS when run with the debug UVM shadow bridge:

```text
ok   cuCtxCreate(&ctx, 0, d)
CTX OK
ok   cuMemAlloc(&dp, 4096)
MEMALLOC OK 0x704c9e200000
ok   cuMemcpyHtoD(dp, &hv, 4)
HTOD OK dp=0x704c9e200000 sleeping-before-dtoh
ok   cuMemcpyDtoH(&rv, dp, 4)
CE rv=0xabcd1234 want=0xabcd1234 -> PASS
DONE
```

The important reframe: the old active blocker was `cuCtxCreate` crashing after `c7c0`. That is no
longer the current blocker. With the live `m2pbmap` bridge, `cuCtxCreate` gets through and the next
real blocker is UVM external-allocation backing for CE data movement.

Without the new debug shadow, the final DtoH path had already narrowed to:

- Destination staging sysmem resolved through pbmap.
- Source `dp` faulted because it is a UVM external allocation that QEMU does not own.

The passing run proves that if QEMU can resolve the device pointer source to coherent backing, the
existing local CE copy path writes the correct bytes into the guest DtoH staging page.

## 2. Last Run Proof

Artifacts:

- `docs/design/mode2_traces/guest_uvm_shadow_trace.txt`
- `docs/design/mode2_traces/qemu_ce_shadow_pass.txt`
- `docs/design/mode2_traces/pbmap_shadow_row.txt`

Guest trace:

```text
UVM_MAP_EXTERNAL base=0x704c9e200000 len=0x200000 off=0x0 rmfd=10 hClient=0xc1d00003 hMemory=0x5c00007f rm_status=0x0 ret=0 req=0x21
CUDA_HTOD dst=0x704c9e200000 bytes=0x4 shadow=0x704cbefe3000 len=0x1000 ret=0 data=3412cdab
CUDA_DTOH src=0x704c9e200000 bytes=0x4 dst=0x7ffe32ed23b0 ret=0 data=3412cdab
```

pbmap row generated from the HtoD shadow:

```text
704c9e200000 10d9be000 1000
```

QEMU CE proof:

```text
nvkvm-gpu[GA106] M5:   COPY[0] src 0x10d9be000(sys)=0xabcd1234 -> dst 0x155ce7100(sys)
nvkvm-gpu[GA106] M5: CE COPY in=0x704c9e200000(virt) out=0x704ca2800100(virt) bytes=4 const=0x0
```

Host dmesg still showed `dmaAllocMapping_GM107: can't alloc VA space for mapping` and Xid 32 from
other high-UVM CE packets during the run. Treat the PASS as a proof of the missing UVM source
backing, not a production-clean first-compute milestone.

## 3. What Is Implemented Locally

Tracked local changes:

- `src/qemu/nvkvm_gpu_emul.c`
  - Adds `m2pbmap=/tmp/m2_pbmap.txt` device property and reloadable VA-to-GPA table.
  - CE virtual address resolution checks pbmap before channel page-table translation.
  - CE copy-fault logging now includes virtual address, resolved aperture, physical address, and
    phys-mode fields.
  - Per-channel VAS and pbmap target mapping changes from the previous session are still in this
    dirty tree.

- `scripts/mode2_diag/nvioctl_trace.c`
  - Existing RM ioctl tracing remains.
  - Adds `UVM_MAP_EXTERNAL_ALLOCATION` logging.
  - Adds opt-in CUDA memory-copy wrappers. With `NVUVM_SHADOW=1`, successful `cuMemcpyHtoD` calls
    copy the bytes into an anonymous guest page and log `CUDA_HTOD dst=... shadow=...`.
  - Logs `CUDA_DTOH` result bytes for confirmation.

- `scripts/mode2_diag/gcup2_pbmap.sh`
  - Exports live guest user pages for `/dev/nvidiactl`, `/dev/nvidia-uvm`, and `/dev/zero` staging
    maps.
  - Parses `CUDA_HTOD` records from `NVKVM_UVM_TRACE` (default `/tmp/guest_uvm_trace.txt`) and emits
    synthetic `<device VA> <shadow guest GPA> <size>` rows.

- `scripts/mode2_diag/cup2_pause.c`
  - Paused CUDA probe that sleeps after HtoD so the live pbmap exporter can catch shadow/staging pages
    before DtoH.

Do not confuse the debug shadow with a production fix. It is a controlled proof that source backing
is the missing piece.

## 4. What Is Ruled Out

- The old `c7c0` / rbp SIGSEGV line is no longer the live failure. `cuCtxCreate` now completes in the
  current dirty tree.
- The NV0000 gpuId divergence and `0x20800102` high bit lead were already mostly ruled out after
  root-slot `addr=0x7` and response normalization.
- The final DtoH mismatch was not a destination staging problem. `/dev/zero` and command-window pbmap
  coverage fixed the destination.
- The UVM allocation handle `hMemory=0x5c00007f` is guest RM/UVM state. QEMU has no matching
  shadow-forwarded host object for it, so "map the existing hMemory on the host" is not currently a
  valid fix path.

## 5. Active Next Step

Replace the LD_PRELOAD shadow proof with a real Mode-2 UVM external-allocation bridge.

Concrete path:

1. Capture `UVM_MAP_EXTERNAL_ALLOCATION` information through a guest-kernel or VMM-visible reporting
   path, not through user LD_PRELOAD:
   `<base, len, hClient, hMemory, offset>`.
2. Add a QEMU side table for UVM external ranges. The table must associate guest device VA ranges
   with coherent backing that the CE resolver can read/write.
3. Populate that backing on HtoD. The debug shadow did this by mirroring `cuMemcpyHtoD` bytes into
   guest RAM; the production path needs the equivalent state from the real UVM/RM migration/copy
   operation.
4. Make CE resolution use the UVM side table before falling back to pbmap/channel translation, or map
   the backing into the forwarded host channel VAS if the operation must execute on the host GPU.
5. Investigate the remaining high-UVM CE packets that still cause host `dmaAllocMapping_GM107` spam
   and Xid 32. They are not required for the 4-byte debug PASS, but they are not production-clean.

## 6. Repro Recipe

Host and guest:

- `ssh vh` is the Vast host with the GPU and QEMU.
- `ssh vg` is the guest through QEMU user networking.

Deploy QEMU:

```bash
scp -q src/qemu/nvkvm_gpu_emul.c vh:/opt/qemu-src/hw/misc/nvkvm_gpu_emul.c
ssh vh 'cd /opt/qemu-src/build && ninja install'
```

Kill stale host processes:

```bash
ssh vh 'bash -s' <<'SH'
pids=$(ps -eo pid=,args= | awk '/[q]emu-system-x86_64/ {print $1}')
[ -n "$pids" ] && kill -9 $pids
pids=$(ps -eo pid=,args= | awk '/[n]vkvm_stub/ {print $1}')
[ -n "$pids" ] && kill -9 $pids
SH
```

Direct QEMU launch used for the PASS:

```bash
/opt/qemu-nvkvm/bin/qemu-system-x86_64 \
  -machine q35,accel=kvm,memory-backend=pcram \
  -object memory-backend-memfd,id=pcram,size=8G,share=on \
  -cpu host -m 8G -smp 4 \
  -drive if=none,id=hd0,file=/opt/nvkvm-guest/mode2-overlay.qcow2,format=qcow2 \
  -device virtio-blk-pci,drive=hd0,addr=0x9 \
  -drive if=none,id=seed,file=/opt/nvkvm-guest/seed.iso,format=raw,readonly=on \
  -device virtio-blk-pci,drive=seed,addr=0xa \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,addr=0x2 \
  -device nvkvm-gpu-emul,addr=0x7,vbios=/opt/nvkvm-guest/ga106_vbios.rom,m2fwd=on,m2exec=on,m2pbmap=/tmp/m2_pbmap.txt \
  -virtfs local,path=/usr/src/nvidia-580.159.04,mount_tag=ogkm,security_model=mapped,readonly=on \
  -virtfs local,path=/usr/lib/firmware/nvidia/580.159.04,mount_tag=nvfw,security_model=mapped,readonly=on \
  -virtfs local,path=/workspace/nvkvm,mount_tag=nvkvm_src,security_model=mapped \
  -serial file:/tmp/m0_serial.log -D /tmp/m0_qemu.log -d unimp,guest_errors -display none
```

Guest setup after every fresh boot:

```bash
scp -q scripts/mode2_diag/cup2_pause.c scripts/mode2_diag/nvioctl_trace.c vg:/tmp/
ssh vg 'bash -s' <<'SH'
set -euo pipefail
NVMODS=/home/ubuntu/nvmods
sudo systemctl isolate multi-user.target 2>/dev/null || true
sleep 2
sudo rmmod nvidia_uvm nvidia nvkvm_guest 2>/dev/null || true
sudo modprobe ecdh_generic ecc 2>/dev/null || true
sudo sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1 || true
sudo dmesg -C || true
sudo insmod "$NVMODS/nvidia.ko" NVreg_EnableGpuFirmware=1 NVreg_RegistryDwords="RmGspBootRetryAttempts=1" 2>&1 | tail -1 || true
sudo insmod "$NVMODS/nvidia-uvm.ko" 2>&1 | tail -1 || true
UVM_MAJ=$(awk '$2=="nvidia-uvm"{print $1}' /proc/devices)
sudo mknod /dev/nvidia0 c 195 0 2>/dev/null || true
sudo mknod /dev/nvidiactl c 195 255 2>/dev/null || true
if [ -n "$UVM_MAJ" ]; then
  sudo rm -f /dev/nvidia-uvm /dev/nvidia-uvm-tools
  sudo mknod /dev/nvidia-uvm c "$UVM_MAJ" 0 2>/dev/null || true
  sudo mknod /dev/nvidia-uvm-tools c "$UVM_MAJ" 1 2>/dev/null || true
fi
sudo chmod 666 /dev/nvidia* /dev/nvidiactl 2>/dev/null || true
sudo ln -sf /usr/local/nvidia-guest/lib/libcuda.so.580.159.04 /lib/x86_64-linux-gnu/libcuda.so.1
gcc -O0 -g -o /tmp/cup2 /tmp/cup2_pause.c -I/usr/include -L/usr/lib/x86_64-linux-gnu/stubs -lcuda
gcc -shared -fPIC -O2 -o /tmp/nvioctl_trace.so /tmp/nvioctl_trace.c -ldl
SH
```

Launch the debug-shadow run:

```bash
ssh vg 'bash -s' <<'SH'
rm -f /tmp/cup2_live.out /tmp/cup2_live.pid /tmp/guest_uvm_trace.txt
GUESTLIB=/usr/local/nvidia-guest/lib
LIBCUDA=$GUESTLIB/libcuda.so.580.159.04
(NVTRACE=/tmp/guest_uvm_trace.txt NVUVM_SHADOW=1 NVALLOC=64 NVOUTER=64 \
 LD_PRELOAD=/tmp/nvioctl_trace.so:$LIBCUDA LD_LIBRARY_PATH=$GUESTLIB \
 stdbuf -oL -eL /tmp/cup2 > /tmp/cup2_live.out 2>&1 & echo $! > /tmp/cup2_live.pid)
SH
```

Refresh pbmap during the pre-DtoH sleep:

```bash
for i in $(seq 1 55); do
  NVKVM_PBMAP_AHEAD_PAGES=16 NVKVM_UVM_TRACE=/tmp/guest_uvm_trace.txt \
    scripts/mode2_diag/gcup2_pbmap.sh /tmp/m2_pbmap.txt
  ssh vg 'tail -n 10 /tmp/cup2_live.out; grep -E "CUDA_HTOD|CUDA_DTOH|UVM_MAP_EXTERNAL" /tmp/guest_uvm_trace.txt | tail -n 8'
  ssh vg 'grep -q "CE rv=" /tmp/cup2_live.out' && break
  sleep 2
done
```

## 7. Architecture Notes

- Channels: `c56f` = GPFIFO channel, `a06c` = channel group/TSG, `9067` = context share, `90f1` =
  VASPACE, `0070` = memory virtual, `c7c0` = compute, `c7b5` = copy.
- GR/compute channels and COPY channels use different RM clients during `cuCtxCreate`. COPY channels
  do most of the scrub/init work.
- Guest CPU-side UVM mappings are not visible to QEMU through GSP RPCs. The debug trace sees them
  only because it hooks the guest userspace `ioctl` and CUDA API.
- The intended end state is still a range-table model:
  `guest GPU VA -> channel PDB -> GPGA/range table -> backing object + offset`.
- Do not refactor `nvkvm_gpu_emul.c` yet. It still has duplicate doorbell/exec paths, multiple
  resolvers, and debug probes. Save cleanup for TASK #128 after first-compute is production-clean.

## 8. Supporting Traces

Previously committed:

- `docs/design/mode2_traces/host_cup2_trace.txt`
- `docs/design/mode2_traces/guest_cup2_trace.txt`
- `docs/design/mode2_traces/guest_root7_trace.txt`
- `docs/design/mode2_traces/ctrl_divergence.txt`

Added for this milestone:

- `docs/design/mode2_traces/guest_uvm_shadow_trace.txt`
- `docs/design/mode2_traces/qemu_ce_shadow_pass.txt`
- `docs/design/mode2_traces/pbmap_shadow_row.txt`
