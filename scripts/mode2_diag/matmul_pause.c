/*
 * matmul_pause.c - Mode-2 diagnostic matmul with staging pauses.
 *
 * Self-contained CUDA driver API probe. It sleeps after HtoD and after kernel
 * sync so scripts/mode2_diag/gcup2_pbmap.sh can export live guest mappings
 * before launch and before DtoH.  MATMUL_SKIP_SYNC=1 reads back after the
 * post-launch delay without waiting in cuCtxSynchronize.
 */
#include <dlfcn.h>
#include <math.h>
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

static const char matmul_ptx[] =
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
".visible .entry matmul(\n"
"    .param .u64 p0,\n"
"    .param .u64 p1,\n"
"    .param .u64 p2,\n"
"    .param .u32 p3\n"
")\n"
"{\n"
"    .reg .pred %p<5>;\n"
"    .reg .b32 %r<16>;\n"
"    .reg .f32 %f<6>;\n"
"    .reg .b64 %rd<16>;\n"
"    ld.param.u64 %rd1, [p0];\n"
"    ld.param.u64 %rd2, [p1];\n"
"    ld.param.u64 %rd3, [p2];\n"
"    ld.param.u32 %r1, [p3];\n"
"    mov.u32 %r2, %ntid.y;\n"
"    mov.u32 %r3, %ctaid.y;\n"
"    mov.u32 %r4, %tid.y;\n"
"    mad.lo.s32 %r5, %r3, %r2, %r4;\n"
"    mov.u32 %r6, %ntid.x;\n"
"    mov.u32 %r7, %ctaid.x;\n"
"    mov.u32 %r8, %tid.x;\n"
"    mad.lo.s32 %r9, %r7, %r6, %r8;\n"
"    setp.ge.s32 %p1, %r5, %r1;\n"
"    setp.ge.s32 %p2, %r9, %r1;\n"
"    or.pred %p3, %p1, %p2;\n"
"    @%p3 bra END;\n"
"    mov.f32 %f1, 0f00000000;\n"
"    mov.u32 %r10, 0;\n"
"LOOP:\n"
"    setp.ge.s32 %p4, %r10, %r1;\n"
"    @%p4 bra STORE;\n"
"    mad.lo.s32 %r11, %r5, %r1, %r10;\n"
"    mul.wide.s32 %rd4, %r11, 4;\n"
"    cvta.to.global.u64 %rd5, %rd1;\n"
"    add.s64 %rd6, %rd5, %rd4;\n"
"    ld.global.f32 %f2, [%rd6];\n"
"    mad.lo.s32 %r12, %r10, %r1, %r9;\n"
"    mul.wide.s32 %rd7, %r12, 4;\n"
"    cvta.to.global.u64 %rd8, %rd2;\n"
"    add.s64 %rd9, %rd8, %rd7;\n"
"    ld.global.f32 %f3, [%rd9];\n"
"    fma.rn.f32 %f1, %f2, %f3, %f1;\n"
"    add.s32 %r10, %r10, 1;\n"
"    bra.uni LOOP;\n"
"STORE:\n"
"    mad.lo.s32 %r13, %r5, %r1, %r9;\n"
"    mul.wide.s32 %rd10, %r13, 4;\n"
"    cvta.to.global.u64 %rd11, %rd3;\n"
"    add.s64 %rd12, %rd11, %rd10;\n"
"    st.global.f32 [%rd12], %f1;\n"
"END:\n"
"    ret;\n"
"}\n";

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : env_int("MATMUL_N", 64);
    int sleep_after_htod = env_int("MATMUL_SLEEP_AFTER_HTOD", 20);
    int sleep_after_launch = env_int("MATMUL_SLEEP_AFTER_LAUNCH", 0);
    int sleep_before_dtoh = env_int("MATMUL_SLEEP_BEFORE_DTOH", 20);
    int skip_sync = env_int("MATMUL_SKIP_SYNC", 0);
    if (n <= 0 || n > 4096) {
        fprintf(stderr, "bad N=%d\n", n);
        return 1;
    }

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
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t) = dlsym(h, "cuMemcpyHtoD_v2");
    CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t) = dlsym(h, "cuMemcpyDtoH_v2");
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                               unsigned, unsigned, unsigned, unsigned,
                               CUstream, void **, void **) = dlsym(h, "cuLaunchKernel");
    CUresult (*cuCtxSynchronize)(void) = dlsym(h, "cuCtxSynchronize");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData ||
        !cuModuleGetFunction || !cuMemAlloc || !cuMemcpyHtoD ||
        !cuMemcpyDtoH || !cuLaunchKernel || !cuCtxSynchronize) {
        fprintf(stderr, "missing CUDA symbol\n");
        return 1;
    }

    printf("pid=%d N=%d bytes_per_matrix=0x%zx\n", getpid(), n, (size_t)n * n * sizeof(float));
    fflush(stdout);

    CHK(cuInit(0), 1, "cuInit");
    CUdevice dev;
    CHK(cuDeviceGet(&dev, 0), 1, "cuDeviceGet");
    CUcontext ctx;
    CHK(cuCtxCreate(&ctx, 0, dev), 1, "cuCtxCreate");

    CUmodule mod;
    CUfunction fn;
    CHK(cuModuleLoadData(&mod, matmul_ptx), 1, "cuModuleLoadData");
    CHK(cuModuleGetFunction(&fn, mod, "matmul"), 1, "cuModuleGetFunction");

    size_t bytes = (size_t)n * n * sizeof(float);
    float *a = malloc(bytes);
    float *b = malloc(bytes);
    float *c = calloc(1, bytes);
    if (!a || !b || !c) {
        fprintf(stderr, "host alloc failed\n");
        return 1;
    }
    for (int i = 0; i < n * n; i++) {
        a[i] = (float)(i % 17) * 0.1f;
        b[i] = (float)(i % 13) * 0.2f;
    }

    CUdeviceptr da, db, dc;
    CHK(cuMemAlloc(&da, bytes), 2, "cuMemAlloc A");
    CHK(cuMemAlloc(&db, bytes), 2, "cuMemAlloc B");
    CHK(cuMemAlloc(&dc, bytes), 2, "cuMemAlloc C");
    printf("devptr A=0x%lx B=0x%lx C=0x%lx\n",
           (unsigned long)da, (unsigned long)db, (unsigned long)dc);
    fflush(stdout);

    CHK(cuMemcpyHtoD(da, a, bytes), 2, "cuMemcpyHtoD A");
    CHK(cuMemcpyHtoD(db, b, bytes), 2, "cuMemcpyHtoD B");
    printf("sleep_after_htod=%d\n", sleep_after_htod);
    fflush(stdout);
    sleep(sleep_after_htod);

    void *args[] = { &da, &db, &dc, &n };
    unsigned grid = (unsigned)((n + 31) / 32);
    CHK(cuLaunchKernel(fn, grid, grid, 1, 32, 32, 1, 0, NULL, args, NULL), 3, "cuLaunchKernel");
    printf("sleep_after_launch=%d\n", sleep_after_launch);
    fflush(stdout);
    sleep(sleep_after_launch);
    if (!skip_sync) {
        CHK(cuCtxSynchronize(), 3, "cuCtxSynchronize");
    } else {
        printf("skip cuCtxSynchronize\n");
        fflush(stdout);
    }
    printf("sleep_before_dtoh=%d\n", sleep_before_dtoh);
    fflush(stdout);
    sleep(sleep_before_dtoh);

    CHK(cuMemcpyDtoH(c, dc, bytes), 3, "cuMemcpyDtoH C");
    int bad = 0;
    for (int col = 0; col < n; col++) {
        float acc = 0.0f;
        for (int k = 0; k < n; k++) {
            acc += a[k] * b[k * n + col];
        }
        if (fabsf(c[col] - acc) > 1e-2f) {
            if (bad < 8) {
                fprintf(stderr, "C[0,%d]=%f expected %f\n", col, c[col], acc);
            }
            bad++;
        }
    }
    if (bad) {
        fprintf(stderr, "VERIFY FAIL: %d/%d first-row mismatches\n", bad, n);
        return 4;
    }
    printf("PASS matmul N=%d\n", n);
    return 0;
}
