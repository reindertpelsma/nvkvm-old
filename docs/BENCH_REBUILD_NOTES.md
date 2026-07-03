# nvkvm Mode-2 bench rebuild status

Started: fresh vast.ai host (70.30.158.46:18577), driver 575.51.03, kernel 6.8.0-59.
Goal: rebuild Mode-2 bench to reproduce bug #12.

## Phases
- [x] 1. Host deps DONE
- [x] 2. Host driver 580.159.04 open DONE (nvidia-smi 580, license Dual MIT/GPL, /usr/src/nvidia-580.159.04 + firmware present)
- [ ] 3. Build QEMU (nvkvm-gpu-emul)
- [ ] 4. Build stub
- [ ] 5. Guest disk build (kernel 6.8.0-117, nvidia 580 open modules, libcuda)
- [ ] 6. Mode-2 smoke test (cuInit)
- [ ] 7. Reproduce #12 (cupctx2_min CTX2 hang)

## Log
- Phase 1 STARTED: apt deps.

- Phase 1+2 DONE. host-libs-580 bundle staged. DKMS tree flat (nvidia/nv-kernel.o_binary + top Kbuild), buildable, shared as ogkm 9p tag.
- Phase 3 STARTED: build QEMU (bg).

## Phase 3 (QEMU) DONE - success criterion 1 MET
- /opt/qemu-nvkvm/bin/qemu-system-x86_64 built; -device nvkvm-gpu-emul,? lists m2fwd/m2exec/m2cefwd/etc.
- build_qemu.sh had 3 bugs (had to fix in deployed /opt/qemu-src tree):
  1. meson configure failed: missing python3-tomli/python3-venv (apt install fixed).
  2. script runs `ninja` from /opt/qemu-src but build.ninja is in ./build -> ran ninja from /opt/qemu-src/build manually.
  3. header copy incomplete: nvkvm .c/.h include ../../src/common/{nvkvm_abi,nvkvm_isolate_proto,nvkvm_ring}.h + abi/*, script only copied 3 headers & rewrote 3 include patterns. Copied ALL into nvkvm_inc/ and sed-rewrote every ../../src/{common,abi}/*.h -> nvkvm_inc/.
  4. virtio.c patch inserted [50]="virtio-nvgpu" into the LAST `};` (virtio_device_info TypeInfo) not virtio_device_names[]. Fixed: put entry after [VIRTIO_ID_GPIO].

## Phase 4 (stub) DONE
- make -C src/stub nvkvm_stub -> install -D /usr/lib/nvkvm/nvkvm_stub (156656 bytes).

## Phase 5 (guest disk) IN PROGRESS
- ubuntu-24.04.qcow2 (+30G) booting on distro qemu (port 2222), key auth works. kernel currently 6.8.0-124 -> must pin to 6.8.0-117.
- seed.iso gotcha: first user-data had literal backslashes in passwd (heredoc \$ escaping) -> cloud-init failed key install. Rewrote via base64, new instance-id, fresh qcow2 -> keys work.

## Phase 5 (guest disk) DONE - criterion 3 MET
- guest ubuntu-24.04.qcow2: kernel PINNED 6.8.0-117 (grub default + apt-hold + unattended-upgrades disabled).
- nvidia 580.159.04 OPEN modules built in-guest vs 6.8.0-117 headers, vermagic 6.8.0-117, staged /home/ubuntu/nvmods/{nvidia,nvidia-uvm,nvidia-modeset,nvidia-drm}.ko
- libcuda 580.159.04 staged /usr/local/nvidia-guest/lib/ (+ libnvidia-allocator, ptxjitcompiler; ld.so.conf.d ahead of system) + libcuda.so dev symlink for -lcuda.
- cuda.h (12.6 cudart redist) -> /usr/include/cuda.h so gcc/nvcc can build tests.
- built .run in-guest (no 9p in plain boot); guest powered off clean to persist qcow2.
- NOTE: run_mode2_vm.sh boots a PERSISTENT overlay mode2-overlay.qcow2 on this base + shares ogkm=/usr/src/nvidia-580.159.04, nvfw=firmware.

## Phase 6 (Mode-2 smoke) DONE - criterion 4 MET
- Boot: pkill qemu; rm mode2-overlay.qcow2; NVKVM_M2CEFWD=1 nohup bash scripts/run_mode2_vm.sh (SSH 2223, ssh vg works).
- Emulated GA106 at 00:07.0; host isolate spawned + MEMTEST data-plane PASS; guest booted 6.8.0-117.
- Loaded stock /home/ubuntu/nvmods (unbind distro nvidia from 00:07.0 first), mknod nodes, insmod nvidia.ko + nvidia-uvm.ko (uvm_maj=234).
- cup2 rc=0: cuInit OK, 1 dev RTX 3060 compute 8.6 11909MiB, cuCtxCreate OK, cuMemAlloc OK, CE HtoD/DtoH byte-exact PASS.

## Phase 7 (repro #12) IN PROGRESS
- Need FRESH boot (GSP WPR2) before cupctx2_min.

## Phase 7 (repro #12) DONE - criterion 5 MET  ***BUG #12 REPRODUCED***
cupctx2_min output (ITERS=2, timeout 180s):
  CUPCTX2_MIN iters=2 (create->destroy only, NO compute)
  [CTX1] cuCtxCreate...
  [CTX1] CTX OK
  [CTX1] cuCtxDestroy...
  [CTX1] CTX DESTROY OK
  [CTX2] cuCtxCreate...      <-- HANGS HERE
  === cupctx2_min exit rc=124 (124=timeout/hang) ===
=> CTX1 create/destroy OK, CTX2 cuCtxCreate hangs = the #12 2nd-context hang, exactly as MEMORY predicts.

## KEY GOTCHA FOUND (cost several runs):
- run_mode2_vm.sh uses `exec qemu`; a stale Mode-2 qemu from a prior boot kept port 2223 +
  held the base qcow2, so "fresh" boots silently landed on the OLD wedged guest (distro nvidia
  had auto-bound+been unbound -> "Failed to enable MSI-X / No interrupts" -> cuInit=101).
  FIX: before each fresh boot, `pkill -9 qemu-system-x86_64; sleep 5; VERIFY ps shows no
  qemu-system` (kill by explicit PID if pkill races), THEN rm overlay + launch.
- Baked distro-nvidia blacklist into base qcow2 (/etc/modprobe.d/nvkvm-blacklist.conf) so the
  emulated GPU at 00:07.0 is pristine (no MSI-X wedge) when our hand-loaded nvmods first attach.

## WORKING BOOT + REPRO COMMANDS (host):
  pkill -9 qemu-system-x86_64; sleep 5; (verify no qemu); rm -f /opt/nvkvm-guest/mode2-overlay.qcow2
  NVKVM_M2CEFWD=1 nohup bash /workspace/nvkvm/scripts/run_mode2_vm.sh >/tmp/m0_qemu_launch.log 2>&1 &
  # wait for ssh -p 2223 ubuntu@localhost   (or `ssh vg` from the dev box)
  ssh -p 2223 ubuntu@localhost bash -s < /tmp/repro12_clean.sh   # loads nvmods, builds+runs cupctx2_min
  (repro12_clean.sh staged at host /tmp/repro12_clean.sh; smoke test = /tmp/mode2_smoke.sh runs cup2 rc=0)

## ALL SUCCESS CRITERIA 1-5 MET.
