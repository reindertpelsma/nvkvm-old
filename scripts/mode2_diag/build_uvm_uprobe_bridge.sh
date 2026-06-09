#!/bin/bash
# Build and load the Mode-2 debug kernel uprobe bridge inside the guest.
set -euo pipefail

LIBCUDA=${LIBCUDA:-/usr/local/nvidia-guest/lib/libcuda.so.580.159.04}
LIBC=${LIBC:-/lib/x86_64-linux-gnu/libc.so.6}
SRC=${SRC:-/tmp/nvkvm_uvm_uprobe_bridge.c}
BUILD=${BUILD:-/tmp/nvkvm_uvm_uprobe_bridge_build}
MAX_BYTES=${MAX_BYTES:-4194304}
CMD_SHADOW=${CMD_SHADOW:-1}
CMD_SCAN_PAGES=${CMD_SCAN_PAGES:-128}
CMD_VA_LIMIT=${CMD_VA_LIMIT:-0x1000000000}
CMD_RANGE_MAX=${CMD_RANGE_MAX:-67108864}
CMD_BG_SCAN_PAGES=${CMD_BG_SCAN_PAGES:-128}
CMD_SCAN_PERIOD_MS=${CMD_SCAN_PERIOD_MS:-100}
CMD_SEED_PAGES=${CMD_SEED_PAGES:-64}
CMD_STRIDE_SEED_PAGES=${CMD_STRIDE_SEED_PAGES:-32}
CMD_PRE_STRIDE_SEED_PAGES=${CMD_PRE_STRIDE_SEED_PAGES:-64}
CMD_STRIDE_STEP=${CMD_STRIDE_STEP:-0x200000}
CMD_STRIDE_WINDOW_PAGES=${CMD_STRIDE_WINDOW_PAGES:-64}
CMD_NEIGHBOR_PAGES=${CMD_NEIGHBOR_PAGES:-64}
CMD_REPORT_ONCE=${CMD_REPORT_ONCE:-1}

if [ ! -f "$SRC" ]; then
    echo "missing source: $SRC" >&2
    exit 1
fi
if [ ! -f "$LIBCUDA" ]; then
    echo "missing libcuda: $LIBCUDA" >&2
    exit 1
fi
if [ ! -f "$LIBC" ]; then
    echo "missing libc: $LIBC" >&2
    exit 1
fi

sym_off() {
    local lib=$1 sym=$2
    readelf -Ws "$lib" | awk -v s="$sym" '$8 == s && !found { val = "0x"$2; found = 1 } END { if (found) print val }'
}

HTOD=$(sym_off "$LIBCUDA" cuMemcpyHtoD)
HTOD_V2=$(sym_off "$LIBCUDA" cuMemcpyHtoD_v2)
IOCTL=$(readelf -Ws "$LIBC" | awk '$8 ~ /^ioctl(@@|$)/ && !found { val = "0x"$2; found = 1 } END { if (found) print val }')
if [ -z "$HTOD" ] && [ -z "$HTOD_V2" ]; then
    echo "could not find cuMemcpyHtoD symbols in $LIBCUDA" >&2
    exit 1
fi
if [ -z "$IOCTL" ]; then
    echo "could not find ioctl symbol in $LIBC" >&2
    exit 1
fi

rm -rf "$BUILD"
mkdir -p "$BUILD"
cp "$SRC" "$BUILD/nvkvm_uvm_uprobe_bridge.c"
cat > "$BUILD/Makefile" <<'EOF'
obj-m += nvkvm_uvm_uprobe_bridge.o
EOF

make -C "/lib/modules/$(uname -r)/build" M="$BUILD" modules
sudo rmmod nvkvm_uvm_uprobe_bridge 2>/dev/null || true
sudo insmod "$BUILD/nvkvm_uvm_uprobe_bridge.ko" \
    libcuda_path="$LIBCUDA" \
    libc_path="$LIBC" \
    htod_off="$HTOD" \
    htod_v2_off="$HTOD_V2" \
    ioctl_off="$IOCTL" \
    max_bytes="$MAX_BYTES" \
    cmd_shadow="$CMD_SHADOW" \
    cmd_scan_pages="$CMD_SCAN_PAGES" \
    cmd_va_limit="$CMD_VA_LIMIT" \
    cmd_range_max="$CMD_RANGE_MAX" \
    cmd_bg_scan_pages="$CMD_BG_SCAN_PAGES" \
    cmd_scan_period_ms="$CMD_SCAN_PERIOD_MS" \
    cmd_seed_pages="$CMD_SEED_PAGES" \
    cmd_stride_seed_pages="$CMD_STRIDE_SEED_PAGES" \
    cmd_pre_stride_seed_pages="$CMD_PRE_STRIDE_SEED_PAGES" \
    cmd_stride_step="$CMD_STRIDE_STEP" \
    cmd_stride_window_pages="$CMD_STRIDE_WINDOW_PAGES" \
    cmd_neighbor_pages="$CMD_NEIGHBOR_PAGES" \
    cmd_report_once="$CMD_REPORT_ONCE"

echo "loaded nvkvm_uvm_uprobe_bridge htod=$HTOD htod_v2=$HTOD_V2 ioctl=$IOCTL max_bytes=$MAX_BYTES cmd_bg_scan_pages=$CMD_BG_SCAN_PAGES cmd_scan_period_ms=$CMD_SCAN_PERIOD_MS cmd_seed_pages=$CMD_SEED_PAGES cmd_stride_seed_pages=$CMD_STRIDE_SEED_PAGES cmd_pre_stride_seed_pages=$CMD_PRE_STRIDE_SEED_PAGES cmd_stride_step=$CMD_STRIDE_STEP cmd_stride_window_pages=$CMD_STRIDE_WINDOW_PAGES cmd_neighbor_pages=$CMD_NEIGHBOR_PAGES cmd_report_once=$CMD_REPORT_ONCE"
