#!/bin/bash
# Build and load the Mode-2 debug kernel uprobe bridge inside the guest.
set -euo pipefail

LIBCUDA=${LIBCUDA:-/usr/local/nvidia-guest/lib/libcuda.so.580.159.04}
SRC=${SRC:-/tmp/nvkvm_uvm_uprobe_bridge.c}
BUILD=${BUILD:-/tmp/nvkvm_uvm_uprobe_bridge_build}
MAX_BYTES=${MAX_BYTES:-4096}

if [ ! -f "$SRC" ]; then
    echo "missing source: $SRC" >&2
    exit 1
fi
if [ ! -f "$LIBCUDA" ]; then
    echo "missing libcuda: $LIBCUDA" >&2
    exit 1
fi

sym_off() {
    local sym=$1
    readelf -Ws "$LIBCUDA" | awk -v s="$sym" '$8 == s && !found { val = "0x"$2; found = 1 } END { if (found) print val }'
}

HTOD=$(sym_off cuMemcpyHtoD)
HTOD_V2=$(sym_off cuMemcpyHtoD_v2)
if [ -z "$HTOD" ] && [ -z "$HTOD_V2" ]; then
    echo "could not find cuMemcpyHtoD symbols in $LIBCUDA" >&2
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
    htod_off="$HTOD" \
    htod_v2_off="$HTOD_V2" \
    max_bytes="$MAX_BYTES"

echo "loaded nvkvm_uvm_uprobe_bridge htod=$HTOD htod_v2=$HTOD_V2 max_bytes=$MAX_BYTES"
