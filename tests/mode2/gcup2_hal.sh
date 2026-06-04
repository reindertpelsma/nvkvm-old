#!/bin/bash
# Guest-side: break at the HAL dispatch in the cuCtxCreate crash function and
# inspect the call-target function pointer (*(rax+0x560)) to see if it's valid
# libcuda .text or garbage/NULL. Anchored to exported cuVDPAUCtxCreate (crash fn =
# cuVDPAUCtxCreate+0xc0f00; HAL call site = +0xc0ecf; earlier HAL call = +0xc0e9f).
set +e
NVMODS=/home/ubuntu/nvmods
sudo systemctl isolate multi-user.target 2>/dev/null; sleep 2
sudo rmmod nvidia_uvm nvidia 2>/dev/null
sudo modprobe ecdh_generic ecc 2>/dev/null
sudo mknod /dev/nvidia0 c 195 0 2>/dev/null; sudo mknod /dev/nvidiactl c 195 255 2>/dev/null
sudo mknod /dev/nvidia-uvm c 235 0 2>/dev/null; sudo mknod /dev/nvidia-uvm-tools c 235 1 2>/dev/null
sudo chmod 666 /dev/nvidia* /dev/nvidia-uvm* 2>/dev/null
sudo dmesg -C
sudo insmod "$NVMODS/nvidia.ko" NVreg_EnableGpuFirmware=1 NVreg_RegistryDwords="RmGspBootRetryAttempts=1" 2>&1 | tail -1
sudo insmod "$NVMODS/nvidia-uvm.ko" 2>&1 | tail -1
sudo chmod 666 /dev/nvidia-uvm* 2>/dev/null
rm -f /tmp/cup2; nvcc -g -o /tmp/cup2 /tmp/cup2.c -lcuda 2>&1 | head -2
cat > /tmp/hal.gdb <<'GDB'
set pagination off
set confirm off
set breakpoint pending on
break cuCtxCreate_v2
commands
  silent
  set $b = (unsigned long)&cuVDPAUCtxCreate
  break *($b + 0xc0e60) if $rdi != 0 && *(unsigned long*)$rdi != 0
  commands
    silent
    set $a0 = $rdi
    set $p1 = *(unsigned long*)($a0 + 8)
    printf "FN-ENTRY a0=%p a0[8]=%p", $a0, $p1
    if $p1 > 0x1000
      set $saved = *(unsigned long*)($p1 + 0x40)
      printf " a0[8][0x40]=%p", $saved
      if $saved > 0x1000
        printf " *(saved+0xd78)=%p", *(unsigned int*)($saved + 0xd78)
      end
    end
    printf " rbx=%p rbx[0x9488]=%p\n", $rbx, *(unsigned long*)($rbx + 0x9488)
    continue
  end
  continue
end
handle SIGSEGV stop nopass
run
echo \n==CRASH==\n
printf "pc=%p rbp=%p\n", $pc, $rbp
echo "-- which mapping is rbx, and the saved ptr? check /proc/PID/maps offsets --"
info proc mappings
GDB
echo "=== gdb HAL inspect ==="
sudo timeout 90 gdb -batch -nx -x /tmp/hal.gdb /tmp/cup2 2>&1 | grep -vE "Reading symbols|no debugging symbols|Thread|New Thread|^\[" | head -80
echo "=== dmesg ==="
sudo dmesg | grep -aiE "segfault|NVRM|Xid" | tail -4
