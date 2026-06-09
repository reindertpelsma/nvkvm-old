/*
 * simple_store_pause.c - Mode-2 diagnostic kernel launch probe.
 *
 * Launches a one-thread PTX kernel that stores a fixed u32 into a CUDA
 * allocation.  SIMPLE_SKIP_SYNC=1 lets us read the allocation after a delay
 * without waiting in cuCtxSynchronize, separating execution from completion
 * accounting.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfn_st *CUfunction;
typedef struct CUstream_st *CUstream;

#define CHK(call, code, msg) do { \
    CUresult r_ = (call); \
    if (r_) { \
        fprintf(stderr, "%s failed: %d\n", msg, r_); \
        return code; \
    } \
    printf("ok   %s\n", msg); \
    fflush(stdout); \
} while (0)

static const char store_ptx[] =
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
".visible .entry store_magic(\n"
"    .param .u64 p0\n"
")\n"
"{\n"
"    .reg .b64 %rd<3>;\n"
"    .reg .b32 %r<2>;\n"
"    ld.param.u64 %rd1, [p0];\n"
"    cvta.to.global.u64 %rd2, %rd1;\n"
"    mov.u32 %r1, 0x12345678;\n"
"    st.global.u32 [%rd2], %r1;\n"
"    ret;\n"
"}\n";

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

int main(void)
{
    int sleep_after_launch = env_int("SIMPLE_SLEEP_AFTER_LAUNCH", 3);
    int skip_sync = env_int("SIMPLE_SKIP_SYNC", 0);

    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) {
        h = dlopen("libcuda.so", RTLD_NOW);
    }
    if (!h) {
        fprintf(stderr, "no libcuda: %s\n", dlerror());
        return 1;
    }

    CUresult (*cuInit)(unsigned) = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice *, int) = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice) = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuModuleLoadData)(CUmodule *, const void *) = dlsym(h, "cuModuleLoadData");
    CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *) = dlsym(h, "cuModuleGetFunction");
    CUresult (*cuMemAlloc)(CUdeviceptr *, size_t) = dlsym(h, "cuMemAlloc_v2");
    CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t) = dlsym(h, "cuMemcpyDtoH_v2");
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                               unsigned, unsigned, unsigned, unsigned,
                               CUstream, void **, void **) = dlsym(h, "cuLaunchKernel");
    CUresult (*cuCtxSynchronize)(void) = dlsym(h, "cuCtxSynchronize");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData ||
        !cuModuleGetFunction || !cuMemAlloc || !cuMemcpyDtoH ||
        !cuLaunchKernel || !cuCtxSynchronize) {
        fprintf(stderr, "missing CUDA symbol\n");
        return 1;
    }

    printf("pid=%d simple_store skip_sync=%d sleep_after_launch=%d\n",
           getpid(), skip_sync, sleep_after_launch);
    fflush(stdout);

    CHK(cuInit(0), 1, "cuInit");
    CUdevice dev;
    CHK(cuDeviceGet(&dev, 0), 1, "cuDeviceGet");
    CUcontext ctx;
    CHK(cuCtxCreate(&ctx, 0, dev), 1, "cuCtxCreate");

    CUmodule mod;
    CUfunction fn;
    CHK(cuModuleLoadData(&mod, store_ptx), 1, "cuModuleLoadData");
    CHK(cuModuleGetFunction(&fn, mod, "store_magic"), 1, "cuModuleGetFunction");

    CUdeviceptr dp;
    CHK(cuMemAlloc(&dp, 4096), 2, "cuMemAlloc");
    printf("devptr=0x%lx\n", (unsigned long)dp);
    fflush(stdout);

    void *args[] = { &dp };
    CHK(cuLaunchKernel(fn, 1, 1, 1, 1, 1, 1, 0, NULL, args, NULL), 3, "cuLaunchKernel");
    printf("sleep_after_launch=%d\n", sleep_after_launch);
    fflush(stdout);
    sleep(sleep_after_launch);

    if (!skip_sync) {
        CHK(cuCtxSynchronize(), 3, "cuCtxSynchronize");
    } else {
        printf("skip cuCtxSynchronize\n");
        fflush(stdout);
    }

    uint32_t got = 0;
    CHK(cuMemcpyDtoH(&got, dp, sizeof(got)), 3, "cuMemcpyDtoH");
    printf("RESULT got=0x%08x want=0x12345678 -> %s\n",
           got, got == 0x12345678u ? "PASS" : "FAIL");
    return got == 0x12345678u ? 0 : 4;
}
