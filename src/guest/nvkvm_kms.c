// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_kms.c — guest-emulated virtual KMS head (#102 modeset Piece 1).
 *
 * A vkms-style virtual display integrated into nvkvm's OWN nvidia-drm device,
 * so the render node and the scanout head are the SAME DRM device — no cross-
 * device PRIME. Pure intra-VM state: ZERO host display calls (we never forward
 * NVKMS; see docs/design/virtual_modeset.md).
 *
 * Headless first: the virtual CRTC accepts atomic commits / page-flips and
 * completes their flip events, but performs no real scanout (there is no
 * physical connector). A compositor/desktop can run on this head and render via
 * the GPU render node. Later (Piece 1 host present) the flipped buffer's dma-buf
 * is exported to QEMU for a host window with host-paced vblank.
 *
 * Scope is deliberately minimal: one connector (fixed 1080p), one CRTC, one
 * primary plane, atomic helpers. No multi-head / HDCP / overlays.
 */
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_vblank.h>
#include <drm/drm_atomic.h>
#include <drm/drm_edid.h>      /* drm_add_modes_noedid */
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>  /* #102 present path: fb geometry/format */
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "nvkvm.h"

#define NVKVM_KMS_W   1920
#define NVKVM_KMS_H   1080
#define NVKVM_KMS_HZ  60

struct nvkvm_kms {
	struct drm_connector            conn;
	struct drm_simple_display_pipe  pipe;
	struct hrtimer                  vblank;   /* software vblank source       */
	ktime_t                         period;   /* 1/refresh                    */
};

/* ── Software vblank (vkms-style): an hrtimer drives the CRTC vblank at a fixed
 * refresh so page-flips pace + complete. Headless has no real scanout timing;
 * later (host present) this slaves to the host window's actual vblank. ──────── */
static enum hrtimer_restart nvkvm_vblank_fn(struct hrtimer *t)
{
	struct nvkvm_kms *kms = container_of(t, struct nvkvm_kms, vblank);
	drm_crtc_handle_vblank(&kms->pipe.crtc);
	hrtimer_forward_now(t, kms->period);
	return HRTIMER_RESTART;
}

/* ── Connector: a single fixed mode, no EDID ─────────────────────────────── */
static int nvkvm_conn_get_modes(struct drm_connector *conn)
{
	/* One preferred mode at the virtual panel's fixed resolution. */
	return drm_add_modes_noedid(conn, NVKVM_KMS_W, NVKVM_KMS_H);
}

/* A virtual panel is always present: report connected on every probe so the
 * connector advertises its mode and compositors will drive it. */
static enum drm_connector_status
nvkvm_conn_detect(struct drm_connector *conn, bool force)
{
	(void)conn; (void)force;
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs nvkvm_conn_helper_funcs = {
	.get_modes = nvkvm_conn_get_modes,
};

static const struct drm_connector_funcs nvkvm_conn_funcs = {
	.detect                 = nvkvm_conn_detect,
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.reset                  = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

/* ── Display pipe (CRTC + primary plane + encoder) ───────────────────────── */
static int nvkvm_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	hrtimer_start(&kms->vblank, kms->period, HRTIMER_MODE_REL);
	return 0;
}

static void nvkvm_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	hrtimer_cancel(&kms->vblank);
}

static void nvkvm_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_framebuffer *fb = pipe->plane.state ? pipe->plane.state->fb : NULL;

	(void)old_state;

	/* Present path (#106) — the host buffer behind this scanout frame. A real
	 * compositor (weston) flips an NVIDIA bo, which surfaces here as one of our
	 * proxy GEMs carrying the stub handle + owning isolate. Notify QEMU so it
	 * exports that buffer's host dma-buf and routes it to the host display /
	 * codec. Best-effort: a failed present must not stall the flip completion
	 * (we still arm the vblank event below). A plain shmem dumb fb (modetest)
	 * is not a proxy → nothing to present. */
	if (fb) {
		__u32 stub_handle = 0;
		struct nvkvm_fd_ctx *fctx = NULL;

		if (nvkvm_fb_stub_handle(fb, &stub_handle, &fctx)) {
			int pret = nvkvm_virtio_present(
				fctx, stub_handle, fb->width, fb->height,
				fb->pitches[0],
				fb->format ? fb->format->format : 0,
				fb->modifier);
			if (pret)
				pr_info_ratelimited(
					"nvkvm present: export failed %d (flip %ux%u stub_handle=0x%x)\n",
					pret, fb->width, fb->height, stub_handle);
			else
				pr_info_ratelimited(
					"nvkvm present: flip %ux%u pitch=%u fmt=0x%08x mod=0x%llx stub_handle=0x%x → exported\n",
					fb->width, fb->height, fb->pitches[0],
					fb->format ? fb->format->format : 0,
					(unsigned long long)fb->modifier,
					stub_handle);
		}
	}
	/* Headless: no real scanout. Pace the flip completion to the software
	 * vblank so a compositor renders at the refresh rate, not unbounded. */
	if (event) {
		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs nvkvm_pipe_funcs = {
	.update         = nvkvm_pipe_update,
	.enable_vblank  = nvkvm_pipe_enable_vblank,
	.disable_vblank = nvkvm_pipe_disable_vblank,
};

static const uint32_t nvkvm_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

/*
 * Present path (#102/#109): the head must accept the buffers a real NVIDIA
 * client renders. NVIDIA scanout surfaces are BLOCK-LINEAR (tiled in VRAM), not
 * linear — advertising only LINEAR makes AddFB2 reject them (EINVAL), so a
 * compositor composits but can never flip. Advertise the canonical NVIDIA
 * 16Bx2 block-linear scanout family (GOB heights 0..5) plus LINEAR so gbm /
 * compositors negotiate a modifier we accept.
 *
 * We do NOT read these pixels in the guest (they live in host VRAM, tiled); the
 * head only holds the buffer + its modifier and forwards (stub_handle, modifier,
 * geometry) to QEMU, where the host NVIDIA driver imports the exported dma-buf
 * (detiling on the GPU) for the present path. So "accepting" the modifier is
 * correct support, not a fake — the host honours the real layout.
 */
static const uint64_t nvkvm_pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0),
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1),
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2),
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3),
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4),
	DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5),
	DRM_FORMAT_MOD_INVALID
};

/* ── Mode config ─────────────────────────────────────────────────────────── */
static const struct drm_mode_config_funcs nvkvm_kms_mode_funcs = {
	.fb_create     = drm_gem_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Set up the virtual head on `ddev`. Called from nvkvm_drm_init BEFORE
 * drm_dev_register (mode objects must exist before the device goes live). */
int nvkvm_kms_init(struct drm_device *ddev)
{
	struct nvkvm_kms *kms;
	int ret;

	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;
	ddev->mode_config.min_width  = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width  = NVKVM_KMS_W;
	ddev->mode_config.max_height = NVKVM_KMS_H;
	ddev->mode_config.funcs      = &nvkvm_kms_mode_funcs;

	/* Flip events need vblank bookkeeping even though we complete them
	 * immediately (drm_crtc_send_vblank_event reads the vblank state). */
	ret = drm_vblank_init(ddev, 1);
	if (ret)
		return ret;

	kms = drmm_kzalloc(ddev, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;

	hrtimer_init(&kms->vblank, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kms->vblank.function = nvkvm_vblank_fn;
	kms->period = ns_to_ktime(NSEC_PER_SEC / NVKVM_KMS_HZ);

	drm_connector_helper_add(&kms->conn, &nvkvm_conn_helper_funcs);
	ret = drm_connector_init(ddev, &kms->conn, &nvkvm_conn_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(ddev, &kms->pipe, &nvkvm_pipe_funcs,
					   nvkvm_pipe_formats,
					   ARRAY_SIZE(nvkvm_pipe_formats),
					   nvkvm_pipe_modifiers, &kms->conn);
	if (ret)
		return ret;

	drm_mode_config_reset(ddev);
	pr_info("nvkvm: virtual KMS head ready (%dx%d, 1 connector/crtc)\n",
		NVKVM_KMS_W, NVKVM_KMS_H);
	return 0;
}
