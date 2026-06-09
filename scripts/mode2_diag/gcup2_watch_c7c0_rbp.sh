#!/bin/bash
set +e
NVMODS=/home/ubuntu/nvmods
sudo systemctl isolate multi-user.target 2>/dev/null; sleep 2
sudo rmmod nvidia_uvm nvidia nvkvm_guest 2>/dev/null
sudo modprobe ecdh_generic ecc 2>/dev/null
sudo sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1
sudo dmesg -C
sudo insmod "$NVMODS/nvidia.ko" NVreg_EnableGpuFirmware=1 NVreg_RegistryDwords="RmGspBootRetryAttempts=1" 2>&1 | tail -1
sudo insmod "$NVMODS/nvidia-uvm.ko" 2>&1 | tail -1
UVM_MAJ=$(awk '$2=="nvidia-uvm"{print $1}' /proc/devices)
sudo mknod /dev/nvidia0 c 195 0 2>/dev/null; sudo mknod /dev/nvidiactl c 195 255 2>/dev/null
sudo rm -f /dev/nvidia-uvm /dev/nvidia-uvm-tools
sudo mknod /dev/nvidia-uvm c "$UVM_MAJ" 0 2>/dev/null; sudo mknod /dev/nvidia-uvm-tools c "$UVM_MAJ" 1 2>/dev/null
sudo chmod 666 /dev/nvidia* /dev/nvidiactl 2>/dev/null
sudo ln -sf /usr/local/nvidia-guest/lib/libcuda.so.580.159.04 /lib/x86_64-linux-gnu/libcuda.so.1
which gdb >/dev/null 2>&1 || sudo apt-get install -y gdb >/dev/null 2>&1
rm -f /tmp/cup2; nvcc -g -o /tmp/cup2 /tmp/cup2.c -lcuda 2>&1 | head -3
cat > /tmp/cup2_watch_c7c0_rbp.gdb <<'GDB'
set pagination off
set confirm off
set disassembly-flavor intel
handle SIGSEGV stop nopass
set disable-randomization on
start
echo \n==== LIBCUDA MAPS ====\n
info sharedlibrary libcuda
break *0x7ffff266652f
continue
echo \n==== BEFORE CALL ====\n
set $caller_rbp = $rbp
set $caller_rsp = $rsp
set $callee = *(void**)($rax+0x560)
set $saved_rbp_slot = $rsp - 0x10
printf "pc=%p rbp=%p rsp=%p callee=%p saved_slot=%p r13=%p r15=%p\n", $pc,$rbp,$rsp,$callee,$saved_rbp_slot,$r13,$r15
x/8gx $saved_rbp_slot
x/24i $callee
watch *(unsigned long*)$saved_rbp_slot
continue
echo \n==== SAVED-RBP SLOT WRITE #1 ====\n
printf "pc=%p rbp=%p rsp=%p saved_slot=%p\n", $pc,$rbp,$rsp,$saved_rbp_slot
x/8gx $saved_rbp_slot
x/12i $pc-0x10
bt 8
continue
echo \n==== SAVED-RBP SLOT WRITE #2 OR STOP ====\n
printf "pc=%p rbp=%p rsp=%p saved_slot=%p\n", $pc,$rbp,$rsp,$saved_rbp_slot
x/8gx $saved_rbp_slot
x/24i $pc-0x20
bt 10
continue
echo \n==== STOPPED ====\n
printf "pc=%p rbp=%p rsp=%p si_addr=%p\n", $pc,$rbp,$rsp,$_siginfo._sifields._sigfault.si_addr
info registers
GDB
echo "=== cup2 watch c7c0 saved-rbp slot under gdb ==="
LD_LIBRARY_PATH="/usr/local/nvidia-guest/lib:/lib/x86_64-linux-gnu" \
  sudo -E timeout 120 gdb -batch -nx -x /tmp/cup2_watch_c7c0_rbp.gdb /tmp/cup2 2>&1 | \
  grep -vE "Reading symbols|no debugging symbols|^\[New Thread|^\[Thread" | head -360
echo "=== dmesg ==="; sudo dmesg | grep -aiE "segfault|NVRM|Xid|trap" | tail -12
