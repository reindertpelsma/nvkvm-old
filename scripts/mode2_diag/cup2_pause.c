/* Mode-2 CUDA dataplane probe with pauses around the CE round-trip. */
#include <cuda.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define CK(x) do {                                                        \
    CUresult r = (x);                                                     \
    const char *s = NULL;                                                 \
    if (r != CUDA_SUCCESS) {                                              \
        cuGetErrorString(r, &s);                                          \
        printf("FAIL %s -> %s (%d)\n", #x, s ? s : "?", r);              \
        fflush(stdout);                                                   \
        return 1;                                                         \
    }                                                                     \
    printf("ok   %s\n", #x);                                             \
    fflush(stdout);                                                       \
} while (0)

int main(void)
{
    printf("pid=%d\n", getpid());
    fflush(stdout);

    CK(cuInit(0));
    int n = 0;
    CK(cuDeviceGetCount(&n));
    printf("devices=%d\n", n);
    fflush(stdout);
    if (n < 1) {
        return 1;
    }

    CUdevice d;
    CK(cuDeviceGet(&d, 0));
    char nm[256] = {0};
    CK(cuDeviceGetName(nm, sizeof(nm), d));
    printf("name=%s\n", nm);
    fflush(stdout);

    int maj = 0, min = 0;
    CK(cuDeviceGetAttribute(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d));
    CK(cuDeviceGetAttribute(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d));
    printf("compute=%d.%d\n", maj, min);
    fflush(stdout);

    size_t tot = 0;
    CK(cuDeviceTotalMem(&tot, d));
    printf("totalMem=%zu MiB\n", tot >> 20);
    fflush(stdout);

    CUcontext ctx;
    CK(cuCtxCreate(&ctx, 0, d));
    printf("CTX OK\n");
    fflush(stdout);

    CUdeviceptr dp;
    CK(cuMemAlloc(&dp, 4096));
    printf("MEMALLOC OK 0x%llx\n", (unsigned long long)dp);
    fflush(stdout);

    unsigned hv = 0xabcd1234;
    unsigned rv = 0;
    CK(cuMemcpyHtoD(dp, &hv, 4));
    printf("HTOD OK dp=0x%llx sleeping-before-dtoh\n", (unsigned long long)dp);
    fflush(stdout);
    unsigned sleep_secs = 60;
    const char *sleep_env = getenv("NVKVM_CUP2_DTOH_SLEEP_SECS");
    if (sleep_env && *sleep_env) {
        unsigned v = (unsigned)strtoul(sleep_env, NULL, 0);
        if (v) {
            sleep_secs = v;
        }
    }
    sleep(sleep_secs);

    CK(cuMemcpyDtoH(&rv, dp, 4));
    printf("CE rv=0x%x want=0x%x -> %s\n", rv, hv, rv == hv ? "PASS" : "MISMATCH");
    fflush(stdout);
    sleep(30);
    printf("DONE\n");
    fflush(stdout);
    return 0;
}
