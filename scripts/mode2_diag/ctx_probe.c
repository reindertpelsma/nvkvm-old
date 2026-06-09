/* Mode-2 cuCtxCreate probe with selectable device-query preamble. */
#include <cuda.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CK(x) do {                                                        \
    CUresult r__ = (x);                                                   \
    const char *s__ = NULL;                                               \
    if (r__ != CUDA_SUCCESS) {                                            \
        cuGetErrorString(r__, &s__);                                      \
        printf("FAIL %s -> %s (%d)\n", #x, s__ ? s__ : "?", r__);       \
        fflush(stdout);                                                   \
        return 1;                                                         \
    }                                                                     \
    printf("ok   %s\n", #x);                                             \
    fflush(stdout);                                                       \
} while (0)

static int run_full_preamble(CUdevice d)
{
    int n = 0;
    char nm[256] = {0};
    int maj = 0, min = 0;
    size_t total = 0;

    CK(cuDeviceGetCount(&n));
    printf("devices=%d\n", n);
    fflush(stdout);
    CK(cuDeviceGetName(nm, sizeof(nm), d));
    printf("name=%s\n", nm);
    fflush(stdout);
    CK(cuDeviceGetAttribute(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d));
    CK(cuDeviceGetAttribute(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d));
    printf("compute=%d.%d\n", maj, min);
    fflush(stdout);
    CK(cuDeviceTotalMem(&total, d));
    printf("totalMem=%zu MiB\n", total >> 20);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    bool full = argc > 1 && strcmp(argv[1], "full") == 0;

    printf("pid=%d mode=%s\n", getpid(), full ? "full" : "minimal");
    fflush(stdout);

    CK(cuInit(0));
    CUdevice d;
    CK(cuDeviceGet(&d, 0));
    if (full && run_full_preamble(d)) {
        return 1;
    }

    CUcontext ctx;
    CK(cuCtxCreate(&ctx, 0, d));
    printf("CTX OK\n");
    fflush(stdout);
    return 0;
}
