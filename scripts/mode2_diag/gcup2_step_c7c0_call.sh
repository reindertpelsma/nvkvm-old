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
cat > /tmp/cup2_step_c7c0.gdb <<'GDB'
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
printf "pc=%p rbp=%p rsp=%p rax(vtbl)=%p callee=%p rbx=%p r13=%p r15=%p\n", $pc,$rbp,$rsp,$rax,$callee,$rbx,$r13,$r15
x/gx $rax+0x560
x/12i $callee
x/24gx $rsp
si
echo \n==== ENTERED CALLEE ====\n
printf "pc=%p rbp=%p rsp=%p saved_caller_rbp=%p saved_caller_rsp=%p\n", $pc,$rbp,$rsp,$caller_rbp,$caller_rsp
x/32i $pc
bt 8
finish
echo \n==== AFTER CALLEE RETURN ====\n
printf "pc=%p rbp=%p rsp=%p rax=%p eax=0x%x saved_caller_rbp=%p\n", $pc,$rbp,$rsp,$rax,$eax,$caller_rbp
x/gx $caller_rbp-0x38
x/24gx $rsp
continue
echo \n==== STOPPED ====\n
printf "pc=%p rbp=%p rsp=%p si_addr=%p\n", $pc,$rbp,$rsp,$_siginfo._sifields._sigfault.si_addr
info registers
GDB
echo "=== cup2 step c7c0 call under gdb ==="
LD_LIBRARY_PATH="/usr/local/nvidia-guest/lib:/lib/x86_64-linux-gnu" \
  sudo -E timeout 120 gdb -batch -nx -x /tmp/cup2_step_c7c0.gdb /tmp/cup2 2>&1 | \
  grep -vE "Reading symbols|no debugging symbols|^\[New Thread|^\[Thread" | head -260
echo "=== dmesg ==="; sudo dmesg | grep -aiE "segfault|NVRM|Xid|trap" | tail -12
