#!/usr/bin/env bash
# build_qemu.sh — clone QEMU 9.2, patch virtio-nvgpu into it, and build a
#                 minimal KVM-only QEMU binary at /opt/qemu-nvkvm.
#
# Idempotent: if /opt/qemu-nvkvm/bin/qemu-system-x86_64 already exists the
# script prints a message and exits successfully.

set -euo pipefail

QEMU_VERSION="9.2.0"
QEMU_SRC="/opt/qemu-src"
QEMU_PREFIX="/opt/qemu-nvkvm"
REPO_ROOT="$(realpath "$(dirname "$0")/..")"

# ── Guard: already built ───────────────────────────────────────────────────
if [ -x "$QEMU_PREFIX/bin/qemu-system-x86_64" ]; then
    echo "INFO: $QEMU_PREFIX/bin/qemu-system-x86_64 already exists — skipping build."
    exit 0
fi

echo "=== nvkvm QEMU build ==="
echo "QEMU version : $QEMU_VERSION"
echo "Source tree  : $QEMU_SRC"
echo "Install path : $QEMU_PREFIX"
echo ""

# ── 1. Install build dependencies ─────────────────────────────────────────
echo "[1/9] Installing build dependencies..."
apt-get update -q
apt-get install -y \
    ninja-build \
    meson \
    libglib2.0-dev \
    libpixman-1-dev \
    python3 \
    git \
    libslirp-dev \
    pkg-config \
    libattr1-dev \
    `# modeset present path (#102): OpenGL + headless EGL scanout` \
    libepoxy-dev \
    libgbm-dev \
    libegl-dev \
    libdrm-dev

# ── 2. Clone QEMU 9.2 stable ──────────────────────────────────────────────
if [ ! -d "$QEMU_SRC" ]; then
    echo "[2/9] Cloning QEMU $QEMU_VERSION..."
    git clone --depth=1 --branch "v${QEMU_VERSION}" \
        https://gitlab.com/qemu-project/qemu.git "$QEMU_SRC"
else
    echo "[2/9] QEMU source already present at $QEMU_SRC — skipping clone."
fi

# ── 3. Copy nvkvm QEMU source files into hw/misc/ ─────────────────────────
echo "[3/9] Copying nvkvm QEMU source files to $QEMU_SRC/hw/misc/..."
cp "$REPO_ROOT/src/qemu/"*.c "$QEMU_SRC/hw/misc/"
cp "$REPO_ROOT/src/qemu/"*.h "$QEMU_SRC/hw/misc/"

# ── 4. Copy ABI / common headers into hw/misc/nvkvm_inc/ ──────────────────
echo "[4/9] Copying ABI and common headers to $QEMU_SRC/hw/misc/nvkvm_inc/..."
mkdir -p "$QEMU_SRC/hw/misc/nvkvm_inc"
cp "$REPO_ROOT/src/abi/nvgpu.h"          "$QEMU_SRC/hw/misc/nvkvm_inc/"
cp "$REPO_ROOT/src/abi/uvm.h"            "$QEMU_SRC/hw/misc/nvkvm_inc/"
cp "$REPO_ROOT/src/common/nvkvm_proto.h" "$QEMU_SRC/hw/misc/nvkvm_inc/"
# Linux type shim: replaces <linux/types.h> in the QEMU user-space build
# to avoid conflicts with QEMU's own type setup in qemu/osdep.h.
cp "$REPO_ROOT/src/qemu/nvkvm_linux_types.h" \
   "$QEMU_SRC/hw/misc/nvkvm_inc/linux_types_compat.h"

# ── 5. Fix include paths in the copied files ──────────────────────────────
echo "[5/9] Fixing include paths in copied files..."
# virtio_nvgpu.h uses relative paths like ../../src/common/nvkvm_proto.h
# that are correct relative to src/qemu/ but wrong inside hw/misc/.
# Rewrite them to use the local nvkvm_inc/ sub-directory.
sed -i \
    's|"../../src/common/nvkvm_proto.h"|"nvkvm_inc/nvkvm_proto.h"|g' \
    "$QEMU_SRC/hw/misc/virtio_nvgpu.h"
sed -i \
    's|"../../src/abi/nvgpu.h"|"nvkvm_inc/nvgpu.h"|g' \
    "$QEMU_SRC/hw/misc/virtio_nvgpu.h"
sed -i \
    's|"../../src/abi/uvm.h"|"nvkvm_inc/uvm.h"|g' \
    "$QEMU_SRC/hw/misc/virtio_nvgpu.h"
# Replace <linux/types.h> in nvkvm_inc headers with our QEMU-compatible shim
# to avoid conflicts with QEMU's own qemu/osdep.h type setup.
sed -i \
    's|#include <linux/types.h>|#include "linux_types_compat.h"|g' \
    "$QEMU_SRC/hw/misc/nvkvm_inc/"*.h

# ── 6. Patch hw/misc/meson.build ─────────────────────────────────────────
echo "[6/9] Patching $QEMU_SRC/hw/misc/meson.build..."

MESON_BUILD="$QEMU_SRC/hw/misc/meson.build"

# Only patch once (idempotent).
if ! grep -q 'virtio_nvgpu.c' "$MESON_BUILD"; then
    # Insert the nvkvm block before the final line of the file.
    # We use a Python one-liner to keep things portable and avoid sed
    # multi-line headaches.
    python3 - "$MESON_BUILD" <<'PYEOF'
import sys

path = sys.argv[1]
with open(path, 'r') as fh:
    lines = fh.readlines()

nvkvm_block = """\

nvkvm_inc = include_directories('nvkvm_inc')

system_ss.add(when: ['CONFIG_VIRTIO'], if_true: files(
  'virtio_nvgpu.c',
  'virtio_nvgpu_pci.c',
  'nvkvm_dispatch.c',
  'nvkvm_frontend.c',
  'nvkvm_objects.c',
  'nvkvm_mmap_host.c',
))
"""

# Insert the block before the very last non-empty line.
insert_pos = len(lines)
for i in range(len(lines) - 1, -1, -1):
    if lines[i].strip():
        insert_pos = i
        break

lines.insert(insert_pos, nvkvm_block)

with open(path, 'w') as fh:
    fh.writelines(lines)

print("  meson.build patched successfully.")
PYEOF
else
    echo "  meson.build already contains virtio_nvgpu.c — skipping patch."
fi

# ── 6b. Patch hw/virtio/virtio.c — extend virtio_device_names table ──────────
# QEMU's virtio_device_names[] in virtio.c has entries only up to ID ~41.
# Our device type is 50, so we must extend the table; otherwise
# virtio_id_to_name() asserts "device_id < G_N_ELEMENTS(virtio_device_names)".
VIRTIO_C="$QEMU_SRC/hw/virtio/virtio.c"
if ! grep -q 'virtio-nvgpu' "$VIRTIO_C"; then
    # The table ends with a line like: [VIRTIO_ID_GPIO] = "virtio-gpio",
    # We append our entry right after it (before the closing brace).
    python3 - "$VIRTIO_C" <<'PYEOF'
import sys, re

path = sys.argv[1]
with open(path, 'r') as fh:
    text = fh.read()

# Find the closing brace of virtio_device_names[] and insert before it.
# The array ends with a line that is just "};" (possibly with leading spaces).
insert_marker = '};\n'
entry = '    [50] = "virtio-nvgpu",\n'
# Only insert once; guard already checked above.
idx = text.rfind(insert_marker)
if idx == -1:
    print("  ERROR: could not find end of virtio_device_names[]", file=sys.stderr)
    sys.exit(1)
text = text[:idx] + entry + text[idx:]
with open(path, 'w') as fh:
    fh.write(text)
print("  virtio.c patched successfully.")
PYEOF
else
    echo "  virtio.c already contains virtio-nvgpu entry — skipping patch."
fi

# ── 7. Configure QEMU ─────────────────────────────────────────────────────
echo "[7/9] Configuring QEMU (target: x86_64-softmmu, KVM only)..."
cd "$QEMU_SRC"
./configure \
    --target-list=x86_64-softmmu \
    --enable-kvm \
    --disable-werror \
    --disable-sdl \
    --disable-gtk \
    --enable-opengl \
    `# opengl pulls in the egl-headless display + dpy_gl_scanout_dmabuf, the` \
    `# host-aligned present path (#102). virglrenderer stays off — the nvkvm` \
    `# present path scans out the guest render target's own dma-buf directly,` \
    `# it does not use virtio-gpu GL virgl.` \
    --disable-virglrenderer \
    --disable-vnc \
    --prefix="$QEMU_PREFIX"

# ── 8. Build ──────────────────────────────────────────────────────────────
echo "[8/9] Building QEMU with ninja -j$(nproc)..."
ninja -j"$(nproc)"

# ── 9. Install ────────────────────────────────────────────────────────────
echo "[9/9] Installing to $QEMU_PREFIX..."
ninja install

echo ""
echo "=== Build complete ==="
echo "Binary: $QEMU_PREFIX/bin/qemu-system-x86_64"
