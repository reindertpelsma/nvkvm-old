# Mode-2 guest recovery recipe (after an overlay rebuild / NVKVM_FRESH)

The Mode-2 overlay holds finely-tuned state. If it's lost, rebuild the guest like this
(see scripts/mode2_diag/cup2_mode2_run_inner.sh). Confirmed 2026-06-06: gets cuInit + cuCtxCreate
working (c7c0 crash fixed); next blocker is first-compute pushbuffer FAULT.

1. Build nvidia.ko + nvidia-uvm.ko from the 9p `ogkm` source (make modules) -> /home/ubuntu/nvmods.
2. `systemctl isolate multi-user.target` then `rmmod nvkvm_guest` — the Mode-1 module squats
   /sys/module/nvidia and blocks loading the real driver (insmod "File exists").
3. Stage GSP firmware: `mount -t 9p nvfw` -> cp into /lib/firmware/nvidia/580.159.04/  (the base
   ships .03 but the .04 driver needs .04, else cuInit 999 "Cannot initialize GSP firmware RM").
4. `modprobe ecdh_generic ecc` (nvidia.ko needs the ecc/ecdh crypto symbols).
5. insmod nvidia.ko (NVreg_EnableGpuFirmware=1) + nvidia-uvm.ko.
6. Device nodes with the CORRECT majors: nvidia0/nvidiactl = 195; nvidia-uvm = DYNAMIC, read from
   /proc/devices (was 237, NOT the hardcoded 235 — wrong major = cuInit 999 before RmInitAdapter).
7. Run cup2 SINGLE (no nvidia-smi first — that double-boots GSP -> "WPR2 already up").

mid-GPU-op fault wedges the guest -> pkill qemu on the host to recover; the host GPU is unaffected.
