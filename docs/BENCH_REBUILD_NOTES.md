# nvkvm Mode-2 bench rebuild status

---
## 2026-07-29 — "Mode-2 guest boot failure" on the freshly rebuilt bench: ★ THERE WAS NO BOOT FAILURE

Reported symptom: "QEMU exits cleanly during guest boot, /tmp/m0_serial.log stops at ~4.1 s,
the guest never reaches userspace and never opens SSH on 2223." Every part of that turned out
to be an artefact of how the bench was being *observed*. Ladder is green (see bottom).

**What was actually true.** The QEMU from the "failed" boot was still running 2 h 34 min later.
Its serial log ran all the way to `cloud-final.service` / `cloud-init.target` / a
`nvkvm-guest login:` prompt at t=21.9 s. The forwarding path was up on every boot (M5.1 isolate
ready, M6.1 SHARED, MEMTEST PASS, M5.3 rc=0). Kernel was the pinned 6.8.0-117-generic and
/home/ubuntu/nvmods held all four .ko.

**Root cause of "never opens SSH": a missing ssh identity on the BENCH HOST, not the guest.**
`/root/.ssh` on the bench had `guest_key`/`guest_key.pub` (the keypair baked into seed.iso's
`ssh_authorized_keys`) but **no default identity** — no `id_ed25519`, no `id_rsa`, no
`~/.ssh/config`. Every one of the ~30 harness scripts under `scripts/mode2_diag/*_host.sh`
runs a **bare** `ssh -p 2223 ubuntu@localhost` with no `-i`, so ssh offered no key at all and
the guest answered `Permission denied (publickey,password)`. Read as "the guest never came up".
Proof: with `-i /root/.ssh/guest_key` the exact same live guest logged in immediately.

FIX (bench host, one time per rebuild) — `/root/.ssh/config`:

    Host guest vg
        HostName 127.0.0.1
        Port 2223
        User ubuntu
        IdentityFile /root/.ssh/guest_key
        StrictHostKeyChecking no
        UserKnownHostsFile /dev/null
        LogLevel ERROR

    Host localhost 127.0.0.1
        IdentityFile /root/.ssh/guest_key
        StrictHostKeyChecking no
        UserKnownHostsFile /dev/null
        LogLevel ERROR

The second block is the load-bearing one: it makes every existing bare
`ssh -p 2223 ubuntu@localhost` / `scp -P 2223` in the repo work unmodified. **Add this to the
guest-disk phase of any future rebuild** — it is as much a part of "the bench works" as the
qcow2 is. `chmod 600 /root/.ssh/config`.

**Root cause of "stops at 4.1 s": looking too early.** This guest needs ~20-25 s to reach a
login prompt, and QEMU's `-serial file:` output is buffered/lazy, so a peek a few seconds in
shows the log frozen mid kernel-init (`clk: Disabling unused clocks`, `RAS: Correctable Errors
collector initialized`, ...) with no further growth. Measured on a fresh overlay this run: the
log sat at t=7.9 s and did not move for ~30 s, then jumped straight to a login prompt at
t=20.9 s. Do not conclude "hang" from a log that has merely not caught up.

**★ TRAP: the documented verify step is a guaranteed false negative.**
`pgrep -x qemu-system-x86_64` and `pgrep -a qemu-system-x86_64` match `/proc/PID/comm`, which
the kernel truncates to 15 chars — `qemu-system-x86`. The `_64` form can therefore **never**
match. Measured with a live QEMU (pid 476684, up 2 h 34 m):

    $ pgrep -a qemu-system-x86_64   -> (nothing)
    $ pgrep -x qemu-system-x86_64   -> rc=1
    $ pgrep -x qemu-system-x86      -> 476684

So "verify pgrep is empty before launching" always passes, you launch a second QEMU, it loses
the race for the hostfwd port and dies with `Could not set up host forwarding rule
'tcp::2223-:22'`. **Verify with `pgrep -x qemu-system-x86`** (and independently with
`ss -tln | grep 2223`, which does not lie). The `[4]` bracket form is still required for
`pkill -f` so it does not match its own command line.

**Not confirmed: the SIGHUP theory.** Launching `bash /root/boot_mode2.sh >log 2>&1 &` from an
ssh command that then returns did NOT kill QEMU — it survived the session teardown and booted
to a login prompt. `run_mode2_vm.sh` `exec`s QEMU with `-display none` and no controlling tty.
Detached launch is still the right habit, but a non-detached launch is not what was breaking
this bench.

**Also ruled out** (all checked, all clean): no assert/segfault/abort/OOM; QEMU exit status
never observed because QEMU never exited; PCI enumeration and MSI-X fine (the blacklist is
baked in and the emulated GA106 at 00:07.0 is pristine); 9p mounts fine; `-no-reboot`
semantics irrelevant (not passed); host driver healthy 580.159.04 throughout, 0 Xid.

**Cosmetic noise worth knowing:** with `-d unimp,guest_errors` the q35 machine's unused
ich9-ahci emits a steady `ahci: IRQ#2 level:1` into /tmp/m0_qemu.log — 13929 of 13938 lines in
one 2.5 h run. Harmless, but it buries the nvkvm markers; always
`grep -a 'nvkvm-gpu\|M5\.\|MEMTEST'` rather than `tail` the QEMU log.

**Added to the repo this session:** `scripts/mode2_diag/bench_boot.sh` (kill -> real verified
wait on the truncated comm AND on port 2223 -> fresh overlay -> detached launch) and
`scripts/mode2_diag/bench_wait.sh` (block for ssh, then print the M5.1/M6.1/MEMTEST/M5.3
markers, and tell you to suspect ssh AUTH if the serial log ends at a login prompt). Use these
two as SEPARATE commands instead of hand-rolling the restart.

LADDER RESULT 2026-07-29 — ALL GREEN, one fresh boot each, host 580.159.04 open, guest STOCK
(unpatched; `mode2_uvm_complete_proof.patch` was NOT needed):
- cup2 rc=0 — cuInit / RTX 3060 compute 8.6 11909 MiB / cuCtxCreate / cuMemAlloc /
  CE HtoD+DtoH `rv=0xabcd1234` byte-exact PASS
- cupctx2_min rc=0 — CTX1 create+destroy OK, CTX2 create+destroy OK, VERDICT PASS (#12 stays fixed)
- cup8 rc=0 — 2048² matmul `bad=0 maxerr=0 C[0]=2048` VERDICT PASS
- cup8_iter rc=0 — ITER0 512 / ITER1 1024 / ITER2 1536 / ITER3 2048 / ITER4 768, all
  `bad=0 maxerr=0`, VERDICT PASS (iters=5 fails=0) (#13 stays fixed)
Bench is unblocked for #90 (C reference traces) and #47.

---
## REBUILD 2026-07-19 (box #45305458 @ 70.30.158.46:27130) — ✅ COMPLETE (all baselines green)
Fresh BLANK vast box (RTX 3060 GA106, host 575.51.03, kernel 6.8.0-59, /dev/kvm present,
21 cores / 49GB / 138G free). Goal: single-process baseline GREEN at emulator source = 862c7c2
(local HEAD c861451 = 862c7c2 + 2 docs-only commits; nvkvm_gpu_emul.c byte-identical to 862c7c2).
NOTE: /workspace/bench-archive did NOT survive on this box — VBIOS rsync'd from local
(/workspace/bench-archive/ga106_vbios.rom md5 48df40a04432aca6a35bee2785857eba).

Phase status (this rebuild):
- [x] 0. Repo rsync'd to /workspace/nvkvm; VBIOS -> /opt/nvkvm-guest/ga106_vbios.rom (md5 OK)
- [x] 1. Host apt deps DONE (added qemu-utils cloud-image-utils genisoimage swtpm; box is Ubuntu 22.04/jammy host)
- [x] 2. DRIVER DECISION = **575-host + 580-source** (task fallback path).
      The 580.159.04 .run REFUSED to install (`--silent`) because the vast box ships the 575
      driver via apt/dpkg ("installed through packages") and the .run cancels on an alternate
      installation; no override flag exists in `--help`. Purging the apt 575 set on a rented GPU
      box risks bricking host GPU access, so per the runbook fallback I kept 575 RUNNING on the
      host and staged the 580 open SOURCE for the guest build (ABI auto-detect handles 575-host
      per multi_driver_validated: "575.51.03 -> profile 570: matmul PASS"). Reloaded 575
      (nvidia-smi = 575.51.03, healthy) after the failed install, then `--extract-only`'d 580.
      Staged from the extracted tree:
        - /usr/src/nvidia-580.159.04/  = kernel-open source (nv-kernel.o_binary present) [ogkm 9p]
        - /usr/lib/firmware/nvidia/580.159.04/{gsp_ga10x,gsp_tu10x}.bin              [nvfw 9p]
        - /workspace/nvkvm/host-libs-580/{libcuda,ptxjitcompiler,allocator,nvvm}.so.580.159.04
          + cuda.h (cudart 12.6 redist, CUDA_VERSION 12060)  [pulled into guest via nvkvm_src 9p]
- [x] 3. Build QEMU DONE — /opt/qemu-nvkvm/bin/qemu-system-x86_64 lists m2fwd/m2exec/m2cefwd.
      TWO MORE build_qemu.sh bugs found+fixed (committed):
      (a) step-5 sed used '|' as BOTH s-delimiter and regex-alternation (common|abi) -> "unknown
          option to s". Fixed: delimiter -> '#'.
      (b) step-6b virtio.c patch regex required a trailing comma after "virtio-gpio", but in
          QEMU 9.2.0 [VIRTIO_ID_GPIO] is the LAST initializer entry with NO trailing comma.
          Fixed: made comma optional in match, emit our entries with the comma.
- [x] 4. Build stub DONE — make -C src/stub nvkvm_stub (156656 B) -> install /usr/lib/nvkvm/nvkvm_stub.
- [~] 5. Guest disk IN PROGRESS:
      - base = ubuntu 24.04.4 noble cloudimg -> /opt/nvkvm-guest/ubuntu-24.04.qcow2 (+30G, 33.5G virt)
      - seed.iso built with cloud-localds; user-data (plain #cloud-config, NOT base64 this time —
        no passwd-escaping issue since keys are ssh-ed25519 one-liners) has BOTH pubkeys
        (local root@test-real-amd64-host for `ssh vg`; vh root@ubuntu for harness `ssh -p 2223`),
        ssh_pwauth:true + ubuntu:nvkvm fallback, and BAKES /etc/modprobe.d/nvkvm-blacklist.conf.
      - provisioning boot: used the nvkvm qemu (no distro qemu on box) as a PLAIN boot on port 2222
        (no -device nvkvm-gpu-emul), shares nvkvm_src + ogkm 9p. Launch via
        /opt/nvkvm-guest/boot_provision.sh with `setsid ... </dev/null & disown` + wait loop in ONE
        ssh session (short-nohup gets orphaned — ssh_aliases gotcha).
      - kernel 6.8.0-117: apt-installed image+headers+modules(+extra)+build-essential; GRUB_DEFAULT
        pinned to the 117 advanced menuentry id; apt-mark hold on the 4 kernel pkgs; unattended-
        upgrades removed. Rebooted -> `uname -r` = 6.8.0-117-generic CONFIRMED.
      - 580 open modules: mounted ogkm 9p, cp -a to /root/nv580src, `make -j4 modules
        SYSSRC=/lib/modules/6.8.0-117/build` (4G swap added). NVBUILD_RC=0.
        vermagic=6.8.0-117-generic, version 580.159.04. Staged 4 .ko ->
        /home/ubuntu/nvmods/{nvidia,nvidia-uvm,nvidia-modeset,nvidia-drm}.ko (ubuntu:ubuntu).
      - guest userspace staged: /usr/local/nvidia-guest/lib/{libcuda,ptxjitcompiler,allocator,
        nvvm}.so.580.159.04 (+ .so.1/.so symlinks via ldconfig, ld.so.conf.d entry) + cuda.h
        -> /usr/include/cuda.h (CUDA_VERSION 12060).
      - Clean `poweroff` to persist base qcow2. run_mode2_vm.sh boots this base with a persistent
        mode2-overlay.qcow2 + shares ogkm/nvfw/nvkvm_src.
   [x] 5. Guest disk DONE.
- [~] 6. Mode-2 smoke cup2 — FIRST ATTEMPT FAILED on host=575, DRIVING A HOST-DRIVER SWAP TO 580.
      cup2 boots the Mode-2 VM fine: emulated GA106 at 00:07.0, host isolate spawned, MEMTEST
      PASS, OS_DESCRIPTOR guest-RAM pin rc=0. cuInit OK, RTX 3060 detected (compute 8.6, 11909MiB),
      cuDeviceTotalMem OK — then HANGS at cuCtxCreate's CE path -> **rc=124 (timeout), DETERMINISTIC
      across 2 fresh boots**. QEMU log = wall of `DIAG vas[N] hvas=.. pdb=.. eva=.. -> FAULT`
      (260-600 faults) during the CE MEMSET/COPY setup = the address-table VA->phys resolution
      MISSES. cup2 busy-polls (State=Rl, 100% CPU, NOT D-state; guest dmesg CLEAN, no Xid; host GPU
      healthy). => This is the host-driver-version dependency: the known-good baseline (862c7c2) was
      validated with **host driver 580.159.04**; on host 575 the emulator's CE VAS resolution faults
      and cuCtxCreate hangs. DECISION REVISED: must install 580 on the host after all. The .run
      refused earlier due to the apt-managed 575 driver -> now purging the apt 575 set + installing
      the 580 .run (mechanics per multi_driver_validated).
      DID IT: rmmod nvidia*, `apt-get purge` the whole nvidia-driver-575/dkms/utils/libnvidia-*575
      set (kept container-toolkit + nvidia-modprobe, harmless), then
      `sh NVIDIA-580.run --silent --no-x-check --no-nouveau-check --dkms -m=kernel-open` -> RC=0.
      **HOST NOW ON 580.159.04** (nvidia-smi healthy, open modules loaded, RTX 3060 responsive).
      /usr/src/nvidia-580.159.04 + /usr/lib/firmware/nvidia/580.159.04 (guest 9p shares) intact.
      >>> DRIVER DECISION FINAL = **580.159.04 INSTALLED ON HOST** (matches known-good baseline).
   [x] 6. Mode-2 smoke cup2 rc=0 ON HOST=580 — cuInit OK, RTX 3060 (8.6, 11909MiB), cuCtxCreate OK,
      cuMemAlloc OK, CE HtoD/DtoH byte-exact (rv=0xabcd1234 -> PASS). CONFIRMS the host-driver-version
      dependency: 575 hung at cuCtxCreate CE (VAS faults); 580 passes. (Early transient VAS faults are
      normal — the address table is forward-populated, miss-before-populate = fault by design.)
      GUEST GOTCHA: each fresh overlay lacks /usr/lib/x86_64-linux-gnu/libcuda.so (unversioned, needed
      by `gcc -lcuda`); re-add `ln -sf /usr/local/nvidia-guest/lib/libcuda.so.580.159.04
      /usr/lib/x86_64-linux-gnu/libcuda.so; ldconfig` after each fresh boot (or bake into base).
- [~] 7. Baseline (each FRESH boot, host=580):
      [x] cupctx2_min (#12) rc=0 — CTX1 create+destroy OK, CTX2 create+destroy OK, VERDICT PASS
          (2 contexts). #12 fix (f5bb32f in 862c7c2) confirmed. 0 faults this boot.
      [x] cup8 rc=0 — 2048^2 matmul byte-exact (bad=0 maxerr=0) VERDICT PASS (host GR matmul at scale)
      [x] cup8_iter (#13) rc=0 — ITER0 N=512 PASS, ITER1 N=1024 PASS, ITER2 N=1536 PASS (the old
          #13 hang point), ITER3 N=2048 PASS, ITER4 N=768 PASS. VERDICT PASS (iters=5 fails=0).
   [x] 7. Baseline DONE — ALL THREE GREEN on host=580. Host GPU healthy (0% util, 1 MiB), no Xid.

=== REBUILD COMPLETE 2026-07-19 — ALL SUCCESS CRITERIA MET ===
cup2 rc=0 (smoke) | cupctx2_min rc=0 (#12, 2 ctx) | cup8 rc=0 (2048^2 byte-exact) |
cup8_iter rc=0 (#13, 5/5 iters). Driver decision FINAL = 580.159.04 installed on the host
(575 host deterministically hangs cuCtxCreate's CE VAS resolution; 580 passes — host-driver-version
dependency is real and must match the known-good 580 baseline).
WORKING BOOT+TEST (host): pkill -9 -f "qemu-system-x86_6[4]"; sleep 5; verify none;
  rm -f /opt/nvkvm-guest/mode2-overlay.qcow2;
  NVKVM_M2CEFWD=1 nohup bash /workspace/nvkvm/scripts/run_mode2_vm.sh >/tmp/m0_qemu.log 2>&1 & disown;
  wait for ssh -p 2223; then stage tests from nvkvm_src 9p (/mnt/nvsrc) + re-add libcuda.so symlink
  (ln -sf /usr/local/nvidia-guest/lib/libcuda.so.580.159.04 /usr/lib/x86_64-linux-gnu/libcuda.so;
  ldconfig) and run scripts/mode2_diag/<test>_run_guest.sh. FRESH boot per GPU test.

GOTCHA (this rebuild): the FIRST provision-boot launch died because the heredoc that wrote
/tmp/boot_provision.sh was in the SAME command as `pkill -9 -f qemu-system-x86_64` — pkill's regex
matched (and the session churn meant) the script never got written, so setsid launched a nonexistent
path. Fix: write the boot script to a PERSISTENT path (/opt/nvkvm-guest/boot_provision.sh) in a
SEPARATE command from any pkill, use the `[4]` regex, then launch.

Prior rebuild log (box 18577, for reference) preserved below.
---

## PRIOR REBUILD (box 70.30.158.46:18577), driver 575.51.03, kernel 6.8.0-59.
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
