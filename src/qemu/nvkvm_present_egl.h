/*
 * nvkvm_present_egl.h — host-side dma-buf capture for the present path (#107).
 */
#ifndef NVKVM_PRESENT_EGL_H
#define NVKVM_PRESENT_EGL_H

#include <stdint.h>

/*
 * Import the dma-buf `dmabuf_fd` (a guest-composited scanout buffer, geometry
 * as given) as a host GL texture, read it back, and write it to `out_path` as a
 * binary PPM.  Does not take ownership of dmabuf_fd (dups internally).
 * Returns 0 on success, -errno otherwise (-ENOTSUP if built without OpenGL or
 * host EGL/dma-buf import is unavailable).
 */
int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path);

#endif /* NVKVM_PRESENT_EGL_H */
