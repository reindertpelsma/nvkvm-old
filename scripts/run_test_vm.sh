#!/usr/bin/env bash
# run_test_vm.sh — launch a KVM guest VM with virtio-nvgpu device
#
# This starts a QEMU VM with:
#   - The virtio-nvgpu device patched into QEMU
#   - nvkvm-guest.ko available to load in the guest
#   - A shared directory with the test suite
#
# Prerequisites:
#   - QEMU patched with the virtio-nvgpu device (hw/misc/virtio-nvgpu.c)
#   - nvkvm-guest.ko built in src/guest/
#   - Ubuntu cloud image at /var/lib/libvirt/images/ubuntu-22.04-nvkvm.qcow2

set -euo pipefail

QEMU=${QEMU_BIN:-qemu-system-x86_64}
IMG=/var/lib/libvirt/images/ubuntu-22.04-nvkvm.qcow2
SEED=/tmp/nvkvm-cloud-init/seed.iso
MODULE_DIR=$(realpath "$(dirname "$0")/../src/guest")
TEST_DIR=$(realpath "$(dirname "$0")/../tests")
SSH_PORT=2222

echo "Starting nvkvm test VM..."
echo "SSH will be available at: ssh nvkvm@localhost -p $SSH_PORT"
echo ""

exec "$QEMU" \
    -enable-kvm \
    -m 4G \
    -smp 4 \
    -cpu host \
    \
    -drive file="$IMG",format=qcow2,if=virtio \
    -drive file="$SEED",format=raw,if=virtio,readonly=on \
    \
    -netdev user,id=net0,hostfwd=tcp::"$SSH_PORT"-:22 \
    -device virtio-net-pci,netdev=net0 \
    \
    -device virtio-nvgpu-pci \
    \
    -virtfs local,path="$MODULE_DIR",mount_tag=nvkvm_module,security_model=mapped \
    -virtfs local,path="$TEST_DIR",mount_tag=nvkvm_tests,security_model=mapped \
    \
    -serial stdio \
    -display none \
    "$@"
