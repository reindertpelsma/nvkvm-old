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

# -- video engines: NVENC encode + NVDEC/cuvid.  libnvidia-encode.so depends on
# libnvcuvid.so, so BOTH must be present + version-matched or ffmpeg/NVENC says
# "Cannot load libnvidia-encode.so.1".  (NVENC session InitializeEncoder beyond
# this is a separate deeper forwarder gap — tracked as its own task.) --
for vlib in libnvidia-encode libnvcuvid; do
    if [ -f "$GFXBUNDLE/$vlib.so.$V" ]; then
        sudo cp -f "$GFXBUNDLE/$vlib.so.$V" "$SYS/"
        sudo ln -sf "$vlib.so.$V" "$SYS/$vlib.so.1"
        sudo ln -sf "$vlib.so.1"  "$SYS/$vlib.so"
    fi
done

# -- EGL GBM external platform (#102 modeset): libnvidia-egl-gbm lets the NVIDIA
# EGL stack render via GBM on a DRM card (the virtual KMS head). Its config
# (15_nvidia_gbm.json) ships with the guest userspace but the .so did not.
# Necessary-but-not-yet-sufficient: NVIDIA EGL still fails to init on the
# emulated head's GBM device (deep integration — see docs/PRE_PUBLIC + #102).
# egl-gbm carries its OWN version (not driver $V); copy whatever the bundle has. --
for f in "$GFXBUNDLE"/libnvidia-egl-gbm.so.*; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    sudo cp -f "$f" "$SYS/"
    sudo ln -sf "$b" "$SYS/libnvidia-egl-gbm.so.1"
done

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

# GLVND EGL vendor config for NVIDIA.  Without /usr/share/glvnd/egl_vendor.d/
# 10_nvidia.json, libGLX_nvidia never enters its NVIDIA device-detection path
# (it only sees mesa) and the Vulkan ICD bows out to llvmpipe.  Points at
# libEGL_nvidia.so.0 (staged above).
sudo mkdir -p /usr/share/glvnd/egl_vendor.d
sudo tee /usr/share/glvnd/egl_vendor.d/10_nvidia.json >/dev/null <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_nvidia.so.0"
    }
}
JSON

# Blacklist nouveau: the emulated NVIDIA-id PCI device (nvkvm-gpu, the DRM render
# node's parent) would otherwise have nouveau auto-bind and probe it.  The device
# has no BARs, so nouveau can only fail/noise — keep it off the device entirely.
if [ ! -f /etc/modprobe.d/blacklist-nvkvm-nouveau.conf ]; then
    echo "blacklist nouveau" | sudo tee /etc/modprobe.d/blacklist-nvkvm-nouveau.conf >/dev/null
    echo "stage_guest_libs: blacklisted nouveau (reboot to take effect)"
fi

echo "stage_guest_libs: done ($V)"
