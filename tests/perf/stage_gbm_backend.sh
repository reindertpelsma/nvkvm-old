#!/bin/bash
# stage_gbm_backend.sh — stage the NVIDIA GBM backend so gbm_create_device() on
# the virtual nvidia-drm card0 selects NVIDIA (not Mesa dri/llvmpipe).
# The backend IS libnvidia-allocator; Mesa libgbm loads <drmdriver>_gbm.so by
# name, so nvidia-drm_gbm.so must exist in the gbm backends dir.
set -u
B="${1:-/mnt/nvkvm/host-libs-580}"
SYS=/usr/lib/x86_64-linux-gnu
V=$(ls "$B"/libnvidia-allocator.so.* 2>/dev/null | sed "s#.*/libnvidia-allocator.so.##" | grep -E '^[0-9]' | head -1)
if [ -z "$V" ]; then echo "no libnvidia-allocator in $B"; exit 1; fi
sudo cp -f "$B/libnvidia-allocator.so.$V" "$SYS/"
sudo ln -sf "libnvidia-allocator.so.$V" "$SYS/libnvidia-allocator.so.1"
sudo ln -sf "../libnvidia-allocator.so.$V" "$SYS/gbm/nvidia-drm_gbm.so"
sudo ldconfig
echo "staged GBM backend (allocator $V)"
ls -la "$SYS/gbm/"
