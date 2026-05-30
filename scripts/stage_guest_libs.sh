#!/bin/bash
# stage_guest_libs.sh — version-match the guest's NVIDIA userspace to the host
# driver.  Runs INSIDE the guest (over 9p at /mnt/nvkvm).  Kept as a standalone
# script (not inline in run_remote_test.sh) so shell variables expand on the
# guest, not the local shell across the nested ssh host "ssh guest '...'" hops.
#
# Two dirs matter and BOTH must match the host driver or NVML/libcuda refuse to
# init ("Driver/library version mismatch" / cuInit 803):
#   - /usr/lib/x86_64-linux-gnu      : where NVML/nvidia-smi resolve libnvidia-ml
#   - /usr/local/nvidia-guest/lib    : where CUDA apps resolve libcuda (on the
#                                      ld.so path via nvidia-guest.conf, AHEAD of
#                                      the system dir) + allocator + ptxjit
#
# The bundle dir defaults to the host's current driver bundle; override with $1.
set -u
GFXBUNDLE="${1:-/mnt/nvkvm/host-libs-580}"
SYS=/usr/lib/x86_64-linux-gnu
CUDADIR=/usr/local/nvidia-guest/lib

V=$(ls "$GFXBUNDLE"/libcuda.so.* 2>/dev/null | sed "s#.*/libcuda.so.##" | head -1)
if [ -z "$V" ]; then
    echo "stage_guest_libs: no libcuda in $GFXBUNDLE" >&2
    exit 1
fi
echo "stage_guest_libs: staging $V from $GFXBUNDLE"

# -- system dir: NVML for nvidia-smi (+ libcuda for completeness) --
sudo cp -f "$GFXBUNDLE/libnvidia-ml.so.$V" "$SYS/" 2>/dev/null
sudo ln -sf "libnvidia-ml.so.$V"           "$SYS/libnvidia-ml.so.1"
sudo cp -f "$GFXBUNDLE/libcuda.so.$V"      "$SYS/" 2>/dev/null
sudo ln -sf "libcuda.so.$V"                "$SYS/libcuda.so.1"
sudo rm -f "$SYS/libcuda.so.575.51.03" "$SYS/libnvidia-ml.so.575.51.03"

# -- canonical CUDA dir: libcuda + allocator + ptxjit (what apps actually load) --
sudo cp -f "$GFXBUNDLE/libcuda.so.$V"                  "$CUDADIR/" 2>/dev/null
sudo cp -f "$GFXBUNDLE/libnvidia-allocator.so.$V"      "$CUDADIR/" 2>/dev/null
sudo cp -f "$GFXBUNDLE/libnvidia-ptxjitcompiler.so.$V" "$CUDADIR/" 2>/dev/null
sudo ln -sf "libcuda.so.$V"                  "$CUDADIR/libcuda.so.1"
sudo ln -sf "libnvidia-allocator.so.$V"      "$CUDADIR/libnvidia-allocator.so.1"
sudo ln -sf "libnvidia-ptxjitcompiler.so.$V" "$CUDADIR/libnvidia-ptxjitcompiler.so.1"
sudo rm -f "$CUDADIR/libcuda.so.575.51.03" \
           "$CUDADIR/libnvidia-allocator.so.575.51.03" \
           "$CUDADIR/libnvidia-ptxjitcompiler.so.575.51.03"

# version-matched nvidia-smi binary
[ -f "$GFXBUNDLE/nvidia-smi-580" ] && \
    sudo cp -f "$GFXBUNDLE/nvidia-smi-580" /usr/local/bin/nvidia-smi && \
    sudo chmod +x /usr/local/bin/nvidia-smi

sudo ldconfig
echo "stage_guest_libs: done ($V)"
