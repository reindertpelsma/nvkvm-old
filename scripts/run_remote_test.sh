#!/bin/bash
# Run integration tests on the remote vast.ai host's VM.
# Assumes:
#   - SSH host config alias `vasthost` resolved here as ssh -p 44850 root@77.104.167.149
#   - VM SSH at port 2222 on the vast.ai host
#   - 9p mount tag `nvkvm_src` exposing the repo root to the guest
#
# Usage:
#   scripts/run_remote_test.sh test_ioctl_fwd
#   scripts/run_remote_test.sh cuinit_test
#   scripts/run_remote_test.sh both
#   scripts/run_remote_test.sh rebuild        # rebuild qemu+stub+module, then test
#   scripts/run_remote_test.sh restart        # kill QEMU, restart, wait for VM
#   scripts/run_remote_test.sh log <pattern>  # grep /tmp/qemu.log on the host
set -e

HOST_SSH="ssh -p 44850 root@77.104.167.149"
GUEST_SSH="ssh -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=5 ubuntu@localhost"

cmd="${1:-both}"
shift || true

wait_for_vm() {
    until $HOST_SSH "$GUEST_SSH echo VM_OK 2>/dev/null" 2>/dev/null | grep -q VM_OK; do
        sleep 4
    done
}

case "$cmd" in
    restart)
        $HOST_SSH 'kill -9 $(pgrep qemu-system) $(pgrep nvkvm_stub) 2>/dev/null; sleep 3
                   rm -f /tmp/qemu.log
                   nohup bash /workspace/nvkvm/scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 & echo PID=$!'
        echo "Waiting for VM..."
        wait_for_vm
        echo "VM ready."
        ;;

    rebuild)
        echo "Syncing source to remote..."
        rsync -avz -e "ssh -p 44850" --exclude '.git' --exclude 'host-libs' \
            /workspace/nvidia-gpu-passthrough/ root@77.104.167.149:/workspace/nvkvm/ > /dev/null
        echo "Rebuilding QEMU + stub..."
        $HOST_SSH '
            cp /workspace/nvkvm/src/qemu/*.c /workspace/nvkvm/src/qemu/*.h /opt/qemu-src/hw/misc/ 2>/dev/null
            cd /opt/qemu-src/build && ninja qemu-system-x86_64 2>&1 | tail -3
            cd /workspace/nvkvm/src/stub && gcc -O2 -g -std=c11 -fPIE -Wall -Wextra \
                -Wno-unused-parameter -pie -Wl,-z,relro,-z,now \
                -o /tmp/nvkvm_stub_new nvkvm_stub.c -lpthread 2>&1 | tail -3
            kill -9 $(pgrep qemu-system) $(pgrep nvkvm_stub) 2>/dev/null
            sleep 2
            cp /opt/qemu-src/build/qemu-system-x86_64 /opt/qemu-nvkvm/bin/qemu-system-x86_64
            cp /tmp/nvkvm_stub_new /usr/lib/nvkvm/nvkvm_stub
            rm -f /tmp/qemu.log
            nohup bash /workspace/nvkvm/scripts/run_test_vm.sh > /tmp/qemu.log 2>&1 & echo PID=$!
        '
        echo "Waiting for VM..."
        wait_for_vm
        echo "Rebuilding guest module + tests in VM..."
        $HOST_SSH "$GUEST_SSH '
            sudo rmmod nvkvm_guest 2>/dev/null
            sudo mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576 nvkvm_src /mnt/nvkvm 2>/dev/null
            cd /mnt/nvkvm/src/guest && sudo make -s 2>&1 | tail -1
            sudo insmod /mnt/nvkvm/src/guest/nvkvm-guest.ko 2>&1 | head -1
            mkdir -p /tmp/build/abi && cp /mnt/nvkvm/src/abi/*.h /tmp/build/abi/
            gcc -O0 -g -Wall -I/tmp/build -o /tmp/test_ioctl_fwd /mnt/nvkvm/tests/integration/test_ioctl_fwd.c
            gcc -O0 -g -o /tmp/cuinit_test /mnt/nvkvm/tests/integration/cuinit_test.c -ldl
            sudo cp /mnt/nvkvm/host-libs/libcuda.so.575.51.03 /usr/lib/x86_64-linux-gnu/ 2>/dev/null
            sudo ln -sf libcuda.so.575.51.03 /usr/lib/x86_64-linux-gnu/libcuda.so.1
            echo READY
        '"
        ;;

    test_ioctl_fwd|ioctl|fwd)
        $HOST_SSH "$GUEST_SSH 'timeout 30 /tmp/test_ioctl_fwd 2>&1 | tail -3'"
        ;;

    cuinit_test|cuinit)
        $HOST_SSH "$GUEST_SSH 'timeout 25 /tmp/cuinit_test 2>&1; echo \"exit=\$?\"'"
        ;;

    both)
        echo "=== test_ioctl_fwd ==="
        $HOST_SSH "$GUEST_SSH 'timeout 30 /tmp/test_ioctl_fwd 2>&1 | tail -3'"
        echo
        echo "=== cuinit_test ==="
        $HOST_SSH "$GUEST_SSH 'timeout 25 /tmp/cuinit_test 2>&1; echo \"exit=\$?\"'"
        ;;

    log)
        pattern="${1:-}"
        if [ -z "$pattern" ]; then
            $HOST_SSH 'tail -50 /tmp/qemu.log'
        else
            $HOST_SSH "grep -aE '$pattern' /tmp/qemu.log | tail -30"
        fi
        ;;

    *)
        echo "Usage: $0 {restart|rebuild|test_ioctl_fwd|cuinit_test|both|log [pattern]}"
        exit 1
        ;;
esac
