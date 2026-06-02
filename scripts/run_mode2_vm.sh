#!/usr/bin/env bash
# run_mode2_vm.sh — boot a guest with the Mode-2 emulated NVIDIA GPU
# (nvkvm-gpu-emul) and the STOCK open NVIDIA driver, to capture the BAR0
# register-access trace (M0) and iterate the fake-the-boot state machine.
#
# Non-destructive: uses -snapshot so the base qcow2 is never modified (host
# disk is tight; we cannot afford a copy).  Driver build happens on a tmpfs in
# the guest and the open-driver source is shared read-only over 9p.
#
# Differences from the Mode-1 run_test_vm.sh:
#   - q35 machine (real PCIe, like a GeForce) instead of i440fx.
#   - NO virtio-nvgpu / nvkvm-gpu identity device — Mode-2 forwards nothing yet.
#   - Adds -device nvkvm-gpu-emul on the PCIe root complex.
#   - SSH on 2223 (so it can coexist with a Mode-1 VM on 2222).
#   - QEMU log (-D) captures the BAR0 trace from the device's qemu_log() calls.
#
# Env overrides: QEMU_BIN, MEM, SMP, EXTRA_QEMU_ARGS.
set -euo pipefail

QEMU="${QEMU_BIN:-/opt/qemu-nvkvm/bin/qemu-system-x86_64}"
IMG="/opt/nvkvm-guest/ubuntu-24.04.qcow2"
SEED="/opt/nvkvm-guest/seed.iso"
OGKM="/root/open-gpu-kernel-modules"      # open driver source (575.51.03)
SSH_PORT="${SSH_PORT:-2223}"
QLOG="${QLOG:-/tmp/m0_qemu.log}"
SERIAL="${SERIAL:-/tmp/m0_serial.log}"
MEM="${MEM:-8G}"
SMP="${SMP:-4}"

[ -f "$IMG" ]  || { echo "ERROR: $IMG missing"; exit 1; }
[ -f "$SEED" ] || { echo "ERROR: $SEED missing"; exit 1; }

rm -f "$QLOG" "$SERIAL"

echo "Mode-2 VM:"
echo "  QEMU     : $QEMU"
echo "  Image    : $IMG  (snapshot — base preserved)"
echo "  SSH      : localhost:$SSH_PORT  (ubuntu)"
echo "  QEMU log : $QLOG   (BAR0 trace)"
echo "  Serial   : $SERIAL"
echo ""

# -d unimp,guest_errors enables the global logfile so the device's qemu_log()
# BAR0 trace lands in $QLOG.
exec "$QEMU" \
    -machine q35,accel=kvm \
    -cpu host \
    -m "$MEM" \
    -smp "$SMP" \
    -snapshot \
    \
    -drive file="$IMG",format=qcow2,if=virtio \
    -drive file="$SEED",format=raw,if=virtio,readonly=on \
    \
    -netdev user,id=net0,hostfwd=tcp::"$SSH_PORT"-:22 \
    -device virtio-net-pci,netdev=net0 \
    \
    `# Mode-2 emulated NVIDIA GPU — the device under test, behind a PCIe root` \
    `# port so it enumerates as a real express endpoint (like a GeForce).` \
    -device pcie-root-port,id=rp0,chassis=0,slot=0 \
    -device nvkvm-gpu-emul,bus=rp0 \
    \
    `# Open driver source (RO) + repo, both over 9p.` \
    -virtfs local,path="$OGKM",mount_tag=ogkm,security_model=mapped,readonly=on \
    -virtfs local,path=/workspace/nvkvm,mount_tag=nvkvm_src,security_model=mapped \
    \
    -serial file:"$SERIAL" \
    -D "$QLOG" -d unimp,guest_errors \
    -display none \
    ${EXTRA_QEMU_ARGS:-}
