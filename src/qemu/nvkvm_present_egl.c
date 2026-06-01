/*
 * nvkvm_present_egl.c — host-side capture of a guest-composited scanout buffer
 * (#107 present path C).
 *
 * The guest's virtual KMS head flips an NVIDIA scanout bo; the stub PRIME-exports
 * it as a host dma-buf and QEMU receives the fd (#106).  Here QEMU imports that
 * dma-buf as a GL texture on the host GPU (NVIDIA EGL detiles block-linear
 * automatically) and reads it back to CPU memory so it can be written to a file
 * or, later, handed to a QemuConsole (dpy_gl_scanout_dmabuf) / NVENC.
 *
 * QEMU is the trusted VMM, so it may open the host render node for its own
 * display compositing — this is distinct from the per-guest sandboxed stub.
 *
 * Reuses QEMU's ui/egl-helpers.c (egl_init / egl_dmabuf_import_texture /
 * egl_fb_read), the same path virtio-gpu uses, so headless GBM + DRM modifiers
 * are handled for us.
 */
#include "qemu/osdep.h"

#ifdef CONFIG_OPENGL
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/egl-helpers.h"
#include "ui/dmabuf.h"
#include "qapi/error.h"
#include "nvkvm_log.h"

#include "nvkvm_present_egl.h"

/* One EGL display/context for the whole VMM, lazily created on the first
 * present, current on the thread that first calls in (QEMU's virtio TX thread —
 * present is dispatched inline there).  -1 = not tried, 0 = failed (give up),
 * 1 = ready. */
static int nvkvm_egl_state = -1;

static bool nvkvm_present_egl_ensure(void)
{
    if (nvkvm_egl_state == 1) {
        return true;
    }
    if (nvkvm_egl_state == 0) {
        return false;   /* already failed once — don't spam retries */
    }

    Error *err = NULL;
    /* Headless: GBM rendernode platform, surfaceless context (no X needed). */
    if (!egl_init("/dev/dri/renderD128", DISPLAY_GL_MODE_ON, &err)) {
        fprintf(stderr, "nvkvm present: egl_init failed: %s\n",
                err ? error_get_pretty(err) : "(unknown)");
        error_free(err);
        nvkvm_egl_state = 0;
        return false;
    }
    if (!qemu_egl_has_dmabuf()) {
        fprintf(stderr, "nvkvm present: host EGL lacks dma_buf import\n");
        nvkvm_egl_state = 0;
        return false;
    }
    nvkvm_egl_state = 1;
    fprintf(stderr, "nvkvm present: host EGL ready (dma-buf import)\n");
    return true;
}

/* Write a DisplaySurface (pixman ARGB32 / BGRA bytes) to a binary PPM (P6). */
static int nvkvm_write_ppm(const char *path, DisplaySurface *s)
{
    int w = surface_width(s), h = surface_height(s);
    int stride = surface_stride(s);
    const uint8_t *data = surface_data(s);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -errno;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = data + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            /* egl_fb_read gives GL_BGRA bytes → B,G,R,A in memory. */
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    return 0;
}

int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path)
{
    if (!nvkvm_present_egl_ensure()) {
        return -ENOTSUP;
    }
    if (width == 0 || height == 0) {
        return -EINVAL;
    }

    /* qemu_dmabuf_new dups nothing — it takes ownership of fd? No: it stores fd
     * and qemu_dmabuf_free()/close() manage it.  We pass a dup so our caller's
     * fd lifetime is independent. */
    int dup_fd = dup(dmabuf_fd);
    if (dup_fd < 0) {
        return -errno;
    }
    QemuDmaBuf *buf = qemu_dmabuf_new(width, height, stride, 0, 0,
                                      width, height, fourcc, modifier,
                                      dup_fd, false, false);
    if (!buf) {
        close(dup_fd);
        return -ENOMEM;
    }

    int ret = 0;
    egl_dmabuf_import_texture(buf);
    uint32_t tex = qemu_dmabuf_get_texture(buf);
    if (!tex) {
        fprintf(stderr, "nvkvm present: dmabuf import produced no texture\n");
        ret = -EIO;
        goto out;
    }

    egl_fb fb = { 0 };
    egl_fb_setup_for_tex(&fb, width, height, tex, false);

    DisplaySurface *s = qemu_create_displaysurface(width, height);
    if (!s) {
        ret = -ENOMEM;
        goto out_fb;
    }
    egl_fb_read(s, &fb);          /* glReadPixels texture → CPU (BGRA) */
    ret = nvkvm_write_ppm(out_path, s);
    qemu_free_displaysurface(s);

out_fb:
    egl_fb_destroy(&fb);
out:
    egl_dmabuf_release_texture(buf);
    qemu_dmabuf_close(buf);
    qemu_dmabuf_free(buf);
    return ret;
}

#else /* !CONFIG_OPENGL */
#include "nvkvm_present_egl.h"
int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path)
{
    (void)dmabuf_fd; (void)width; (void)height; (void)stride;
    (void)fourcc; (void)modifier; (void)out_path;
    return -ENOTSUP;
}
#endif
