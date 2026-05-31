# Virtual Modeset & Host-Aligned Present Path

Status: DESIGN (2026-05-31)
Owner: nvkvm graphics milestone (#82)
Related: `signal_interrupt_delivery.md`, `../audits/nvproxy_gap_analysis.md`

## TL;DR

We do **not** forward `/dev/nvidia-modeset` (NVKMS) to the host. Display on nvkvm
is split into three pieces with very different cost and reuse:

1. **Present path** (host-aligned): guest render-target → host dma-buf → QEMU
   window → host-compositor direct-scanout, with **host-paced vblank** fed back
   to the guest. *Mode-agnostic — reused verbatim by a future reverse driver
   (Mode 2). Build this first.*
2. **Virtual-KMS device**: emulate the DRM/KMS ioctl surface inside the nvkvm
   guest module (no host display calls). Mode-1 only; scope minimally; it is the
   cheapest harness for driving real rendering engines through the full stack.
3. **NVKMS forwarding**: forward `/dev/nvidia-modeset` to the host. **Rejected** —
   largest/gnarliest attack surface in the driver AND made redundant by Mode 2.

Headless (render-node-only) stays the default for server/microVM guests; it
already works (surfaceless EGL: RTX 3060 @ 632 FPS, 2048², 800-iter shader).

## Why modeset is virtualizable

The render/compute engine *produces* pixels (forwarded already: RM + render
node). The display engine that modeset drives only *consumes* a finished
framebuffer and DMAs it to a connector — it does **no compute**. With no physical
connector, scanout has no destination, so everything NVKMS does (pixel clocks,
PLLs, encoder/EDID/HDCP, the display channel) is irrelevant. What remains is
**bookkeeping + timing**, which the guest can synthesize. Mainline precedent:
`vkms` (Virtual KMS) — a complete software DRM/KMS driver with no display HW.

## Mode 1 ↔ Mode 2 convergence (why present-path-first)

```
MODE 1 (nvkvm forwarding):  guest virtual-KMS ─┐
                                                ├─▶ dma-buf ─▶ QEMU window ─▶ host compositor
MODE 2 (reverse driver):    stock guest nvidia ─┘     (THE PRESENT PATH — shared)
   modeset → QEMU reverse driver emulates GSP
   display methods → produces a scanout surface ─┘
```

The **producer** differs per mode (forward NVKMS vs. emulate vkms vs. emulate GSP
display RPCs). The **consumer/present path is identical**. It is the keystone both
arches stand on: a reverse driver that renders frames the host cannot display is
worthless. Therefore the present path is built first and validated independently
of any modeset code (drive it with virtio-gpu or a synthetic dma-buf producer).

## Piece 1 — Present path (build first)

Flow:

```
guest renders ─▶ swapchain image (NVIDIA GEM surface; render path already forwarded)
   │ virtual-KMS PAGE_FLIP / ATOMIC_COMMIT
   ▼
PRIME_HANDLE_TO_FD  ── export render target as a host dma-buf (rides GEM path)
   ▼
QEMU UI: dpy_gl_scanout_dmabuf()  ── existing plumbing (virtio-gpu uses it today)
   ▼
host present via -display gtk,gl=on | egl-headless | dbus(PipeWire)
   ▼
host compositor (KWin/Mutter): if VM window fullscreen/unobscured AND buffer
   format+modifier scanout-compatible → assigns to a HW plane = GPU display
   engine reads the guest-rendered buffer directly (zero copy, no recomposition).
   ▼
host present-complete / frame-callback ─▶ QEMU ─▶ guest virtual-KMS flip-complete
   = guest vblank slaved to host refresh (real vsync, no tearing).
```

Key points:
- **We never hand the guest a host KMS plane** — planes are a privileged host
  resource owned by the host DRM master. We only make the buffer *eligible* for
  the host compositor's existing direct-scanout optimization.
- **The only thing that crosses to the host for display is `PRIME_HANDLE_TO_FD`**
  (dma-buf export). It rides the render/GEM path, not a display path.
- **Host-aligned vblank**: the guest virtual CRTC has no real timing; slave its
  vblank/flip-complete to the host window's present cadence. This is the
  "align modeset to the window" idea — the guest tracks the host's actual refresh.

New work for Piece 1 (small — most of the consumer exists in QEMU):
- (a) export nvkvm's render target as a host dma-buf;
- (b) feed it to `dpy_gl_scanout_dmabuf` (reuse);
- (c) plumb host present-completion back to the guest as a vblank/flip event.

### Caveat: same-GPU vs cross-GPU (laptops)

- Host compositor on the **same** NVIDIA GPU we forward → intra-GPU dma-buf
  share, near-zero-copy; direct-scanout-to-plane available.
- **Laptop iGPU + dGPU** (compositor on iGPU) → cross-GPU dma-buf import (PRIME
  cross-import works). Direct-scanout-to-plane only if the dGPU drives the panel;
  otherwise one cross-GPU copy. Still fast. Must be handled explicitly.

## Piece 2 — Virtual-KMS device (minimal, Mode-1 only)

Emulate a `vkms`-style DRM/KMS device in the nvkvm guest module: one virtual head
(connector + CRTC + primary plane), fake EDID (e.g. 1920×1080@60), atomic commit,
ADDFB2, PAGE_FLIP, PRIME export, synthesized vblank slaved to host (Piece 1).

Scope **ruthlessly**: no multi-head, no HDCP, no swap-groups, no overlay-plane
zoo. Its job is to be the cheapest harness that drives real present-based apps
(vkmark KMS, glmark2-drm, kmscube, a compositor) through the full stack so we can
find the next round of RM-semantic bugs — plus a modest real feature (Linux
desktop-in-VM). Accept it is partly throwaway under Mode 2.

## Piece 3 — NVKMS forwarding (REJECTED as target; LIVE as interim — REMOVE)

**Current status (2026-05-31):** NVKMS forwarding is *live* in the code
(commit a895f95, added to unblock `vkCreateDevice`). The wrapper ioctl
`0xC0106D00` is forwarded with only an outer-ioctl gate — **no inner-`cmdType`
allowlist** (security audit 2026-05-31 finding G-1). Practical severity is LOW
(unprivileged sandboxed stub + NVKMS modeset-ownership/CAP_SYS_ADMIN gating in
the kernel + headless hosts have no display), but it widens reachable kernel
parser surface, violating our default-deny principle. **Action: remove the
forward path entirely once Piece 2 (guest virtual head) lands; interim, gate to
the minimal cmdTypes the UMD needs.** The text below is the target rationale.


Forwarding `/dev/nvidia-modeset` is the worst option on both axes:
- **Attack surface**: ~70 NVKMS command types (HDCP, EDID parse, mode validation,
  embedded-pointer/fd translation) — a large, under-audited surface, and the host
  display engine is *shared* hardware across guests (a guest must never program
  pixel clocks or read another tenant's scanout).
- **Redundant under Mode 2**: the stock guest driver does modeset itself; the
  reverse driver terminates it at the GSP/register level. Building NVKMS
  forwarding means building a huge surface that Mode 2 deletes.

Fits the existing access-model split: QEMU = host/cross-VM boundary only; the
guest kernel emulates all intra-VM state. A virtual head is pure intra-VM state.

## ioctl surface reference

### `/dev/nvidia-modeset` (NVKMS) — single wrapper, NOT a DRM device

`NVKMS_IOCTL_CMD = _IOWR('m',0,{u32 cmdType; u32 size; u64 address;})` = `0xC0106D00`.
`address` → per-command payload (embedded RM handles + fds). Command set
(ground truth: `nvkms-api.h` in open-gpu-kernel-modules), grouped:

| Group | Commands | Purpose |
|---|---|---|
| Device | ALLOC_DEVICE, FREE_DEVICE | open NVKMS device, head/layer caps |
| Topology | QUERY_DISP, QUERY_CONNECTOR_STATIC/DYNAMIC, QUERY_DPY_STATIC/DYNAMIC | connectors, EDID, connection state |
| Mode | VALIDATE_MODE[_INDEX], SET_MODE | validate + program head mode/viewport/layers |
| Surfaces | REGISTER/UNREGISTER_SURFACE, GRANT/ACQUIRE/RELEASE_SURFACE | bind GPU surface (RM handle + pitch/format/block-linear) to display |
| Present | FLIP, SET_LAYER_POSITION, SET_CURSOR_IMAGE, MOVE_CURSOR | page-flip + cursor + overlay layers |
| Sync/events | REGISTER_DEFERRED_REQUEST_FIFO, ENABLE_VBLANK_SYNC_OBJECT, notifiers | vblank/flip-done signalling |
| Sharing/perms | GRANT/ACQUIRE/REVOKE_PERMISSIONS, swap-groups, framelock | multi-client / multi-GPU display |

(Not forwarded. Listed for completeness / Mode-2 reference.)

### `/dev/dri/card0` (DRM KMS, `nvidia-drm modeset=1`) — type `'d'` (0x64)

Emulated by Piece 2 (virtual head). The **only** cross-to-host ioctl is PRIME.

| Group | ioctls | Purpose |
|---|---|---|
| Caps/identity | VERSION, GET_CAP, SET_CLIENT_CAP(ATOMIC,UNIVERSAL_PLANES) | driver name (identity check) + feature negotiation |
| Master | SET_MASTER, DROP_MASTER, GET/AUTH_MAGIC | compositor owns modeset |
| Enumerate | MODE_GETRESOURCES, GETCONNECTOR, GETENCODER, GETCRTC, GETPLANE[RESOURCES] | discover CRTCs/connectors/planes |
| Properties | OBJ_GETPROPERTIES, GETPROPERTY, GETPROPBLOB, CREATE/DESTROY_PROPBLOB | atomic props, EDID/mode blobs |
| Framebuffer | MODE_ADDFB2, RMFB, DIRTYFB | wrap GEM handle(s) as scanout FB (format + modifiers + pitches) |
| Modeset/flip | MODE_SETCRTC, PAGE_FLIP, **MODE_ATOMIC** | legacy + atomic commit (planes+crtc+flip+fences) |
| Cursor | MODE_CURSOR2 | hw cursor plane |
| Vblank | WAIT_VBLANK, CRTC_QUEUE/GET_SEQUENCE | timing slaved to host |
| Dumb | CREATE/MAP/DESTROY_DUMB | simple FBs (cursor/fallback) |
| **PRIME** | **PRIME_HANDLE_TO_FD**, FD_TO_HANDLE | **dma-buf export/import — the one primitive that crosses to host** |
| GEM | GEM_CLOSE, GEM_OPEN/FLINK | handle lifecycle |

### nvidia-drm private ioctls (`'d'`, nr ≥ 0x40 — `nvidia-drm-ioctl.h`)

This is the **render-node** surface — already forwarded for Vulkan (renderD128):
GET_DEV_INFO, DMABUF_SUPPORTED, GEM_IMPORT_NVKMS_MEMORY, GEM_ALLOC/EXPORT_NVKMS_MEMORY,
GEM_IMPORT_USERSPACE_MEMORY, GEM_MAP_OFFSET, GEM_EXPORT_DMABUF_MEMORY,
FENCE_CONTEXT_CREATE / GEM_FENCE_ATTACH, GET_CLIENT_CAPABILITY, GEM_IDENTIFY_OBJECT.

### Surface split summary

- **Render (DONE)**: nvidia-drm private GEM ioctls + GET_DEV_INFO + VERSION on renderD128.
- **Virtual modeset (emulate in guest, zero host calls)**: the DRM KMS table.
- **Crosses to host for display**: only `PRIME_HANDLE_TO_FD` (rides render/GEM path).

## Roadmap / sequencing

1. **Present path (Piece 1)** — now. Validate with virtio-gpu / synthetic dma-buf
   producer first (no nvkvm modeset code), then wire nvkvm render output in.
2. **Minimal virtual-KMS (Piece 2)** — as the real-app validation harness + Linux
   desktop-in-VM feature. Scope tight.
3. **Reverse driver (Mode 2)** — NOT now. Gate behind a research spike on the one
   unsolved core (guest-PTE↔host-DMA translation) before committing 10–16 pm.
   By then the present path (its riskiest UX surface) already exists.

## Security notes

- Never forward NVKMS; never expose host KMS planes/CRTCs to the guest.
- `PRIME_HANDLE_TO_FD` is the only display-related cross-boundary primitive —
  audit it as a buffer-descriptor-sharing surface (guest-controlled GEM handle →
  host dma-buf): validate ownership/lifetime, no cross-session/cross-guest reach.
- The graphics delta (new RM alloc classes, DRM/GEM ioctls, guest+stub fd
  translations) added default-ALLOW surface and should get a targeted audit
  before the present path builds on top of it.
