/*
 * nvkvm_drm_allowlist.h — default-deny allowlist for nvidia-drm (render node)
 * ioctls forwarded from the guest to the host /dev/dri/renderD128.
 *
 * DRM ioctls are _IOC_TYPE == 'd' (0x64).  nvidia private commands live at
 * DRM_COMMAND_BASE (0x40) + DRM_NVIDIA_* (see kernel-open/nvidia-drm/
 * nvidia-drm-ioctl.h).  Like the RM control + frontend allowlists, this is the
 * host/cross-VM attack-surface boundary: only the compute/render-relevant
 * render-node ioctls are permitted.  DISPLAY/MODESET/permissions surfaces
 * (NVKMS import/alloc, CRTC CRC, grant/revoke permissions, connector/dpy id)
 * are deliberately excluded — a render node should never drive them, and they
 * are privileged on the host.
 *
 * NOTE: VERSION (nr 0x00) is synthesized guest-side and normally never reaches
 * QEMU; it is allowed here only for robustness.
 */
#ifndef NVKVM_DRM_ALLOWLIST_H
#define NVKVM_DRM_ALLOWLIST_H

#include <stdbool.h>

#define NVKVM_DRM_COMMAND_BASE 0x40

static inline bool nvkvm_drm_nr_allowed(unsigned nr)
{
	switch (nr) {
	/* generic DRM */
	case 0x00: /* DRM_IOCTL_VERSION (synthesized guest-side) */
	case 0x09: /* DRM_IOCTL_GEM_CLOSE */
	/* nvidia private (DRM_COMMAND_BASE + DRM_NVIDIA_*) */
	case NVKVM_DRM_COMMAND_BASE + 0x03: /* GET_DEV_INFO (enumeration key) */
	case NVKVM_DRM_COMMAND_BASE + 0x04: /* FENCE_SUPPORTED */
	case NVKVM_DRM_COMMAND_BASE + 0x05: /* PRIME_FENCE_CONTEXT_CREATE */
	case NVKVM_DRM_COMMAND_BASE + 0x06: /* GEM_PRIME_FENCE_ATTACH */
	case NVKVM_DRM_COMMAND_BASE + 0x08: /* GET_CLIENT_CAPABILITY */
	case NVKVM_DRM_COMMAND_BASE + 0x0f: /* DMABUF_SUPPORTED */
	/*
	 * Audit G-3: GEM_IMPORT_USERSPACE_MEMORY (0x02), GEM_MAP_OFFSET (0x0a),
	 * GEM_EXPORT_DMABUF_MEMORY (0x0d) and GEM_IDENTIFY_OBJECT (0x0e) are
	 * deliberately NOT allowed.  They carry raw guest VAs / mint mappings
	 * with no guest-VA marshalling — a guest VA forwarded to the host
	 * render node is pinned in the stub's address space (stub-heap info
	 * disclosure).  The legitimate guest DRM proxy (nvkvm_drm.c) never
	 * issues them (its ioctl table wires only GET_DEV_INFO / DMABUF_SUPPORTED
	 * / SEMSURF_FENCE_* + GEM_CLOSE), so denying them is regression-free.
	 * EXPORT_DMABUF (0x0d) will be re-added WITH marshalling when the
	 * dma-buf present path lands (docs/design/virtual_modeset.md Piece 1).
	 */
	/* Semaphore-surface fences — render-path GPU synchronisation primitives
	 * (pair with NV_SEMAPHORE_SURFACE); NOT display/permissions.  The Vulkan
	 * ICD uses them for cross-queue/cross-process sync (#84). */
	case NVKVM_DRM_COMMAND_BASE + 0x14: /* SEMSURF_FENCE_CTX_CREATE */
	case NVKVM_DRM_COMMAND_BASE + 0x15: /* SEMSURF_FENCE_CREATE */
	case NVKVM_DRM_COMMAND_BASE + 0x16: /* SEMSURF_FENCE_WAIT */
	case NVKVM_DRM_COMMAND_BASE + 0x17: /* SEMSURF_FENCE_ATTACH */
		return true;
	default:
		return false;
	}
}

#endif /* NVKVM_DRM_ALLOWLIST_H */
