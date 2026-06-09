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
cat > /tmp/cup2_detail.gdb <<'GDB'
set pagination off
set confirm off
set disassembly-flavor intel
handle SIGSEGV stop nopass
run
echo \n==== FAULT ====\n
printf "si_addr = %p\n", $_siginfo._sifields._sigfault.si_addr
printf "pc=%p sp=%p rbp=%p\n", $pc, $sp, $rbp
echo \n==== LIBCUDA MAPS ====\n
info sharedlibrary libcuda
info proc mappings
echo \n==== REGISTERS ====\n
info registers
echo \n==== STACK ====\n
x/48gx $rsp
echo \n==== DISASM NEAR PC ====\n
x/80i $pc-0x100
echo \n==== CANDIDATE OBJECTS ====\n
printf "rbx=%p r12=%p r13=%p r14=%p r15=%p\n", $rbx, $r12, $r13, $r14, $r15
if $rbx != 0
  x/32gx $rbx
end
if $r12 != 0
  x/32gx $r12
end
if $r13 != 0
  x/32gx $r13
end
if $r14 != 0
  x/32gx $r14
end
GDB
echo "=== cup2 detail under gdb ==="
LD_LIBRARY_PATH="/usr/local/nvidia-guest/lib:/lib/x86_64-linux-gnu" \
  sudo -E timeout 120 gdb -batch -nx -x /tmp/cup2_detail.gdb /tmp/cup2 2>&1 | \
  grep -vE "Reading symbols|no debugging symbols|^\[New Thread|^\[Thread" | head -260
echo "=== dmesg ==="; sudo dmesg | grep -aiE "segfault|NVRM|Xid|trap" | tail -12
