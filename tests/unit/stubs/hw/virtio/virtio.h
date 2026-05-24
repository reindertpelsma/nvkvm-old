/* QEMU virtio stub for unit tests — minimal type definitions */
#ifndef QEMU_VIRTIO_STUB_H
#define QEMU_VIRTIO_STUB_H

#include <stdint.h>
#include <stdlib.h>

typedef struct VirtIODevice    { int _dummy; } VirtIODevice;
typedef struct VirtQueue       { int _dummy; } VirtQueue;
typedef struct VirtQueueElement { int _dummy; } VirtQueueElement;

#define OBJECT_CHECK(t, o, n) ((t *)(o))

/* GLib memory stubs */
#define g_new0(T, n)      ((T *)calloc((size_t)(n), sizeof(T)))
#define g_free(p)         free(p)
#define g_realloc(p, s)   realloc((p), (size_t)(s))
#define g_malloc(s)       malloc(s)

#endif /* QEMU_VIRTIO_STUB_H */
