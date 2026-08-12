/*
 * nvd_prog.c — the differential workload. ONE program, staged, so host and guest
 * captures are byte-comparable and so a future question can re-run a shorter prefix.
 *
 *   nvd_prog init    cuInit only
 *   nvd_prog dev     + cuDeviceGet / attributes / totalMem
 *   nvd_prog ctx     + cuCtxCreate + cuCtxDestroy          <- the wall
 *   nvd_prog alloc   + cuMemAlloc / cuMemFree
 *   nvd_prog ce      + HtoD/DtoH round trip (this is what cup2 does: ZERO launches)
 *   nvd_prog launch  + one trivial kernel launch from embedded PTX
 *
 * Default stage is "ce" — i.e. the cup2 shape, no kernel launches, because that is
 * what our guest runs. "launch" is the first stage the CE emulator cannot forge.
 *
 * Build: cc -O0 -o nvd_prog nvd_prog.c -lcuda      (no nvcc, no cudart, no cuBLAS)
 */
/* ⊘ -DNVD_NO_CUDA_H selects the bundled stand-in (nvd_capture.sh sets it when the box has
 * libcuda but no toolkit). ⚠ It must be set on BOTH sides of a differential or neither:
 * the two binaries are supposed to be the same program. */
#ifdef NVD_NO_CUDA_H
#include "nvd_cuda_min.h"
#else
#include <cuda.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CK(x) do{ CUresult r_=(x); const char*s_=0; if(r_!=CUDA_SUCCESS){ \
    cuGetErrorString(r_,&s_); printf("FAIL %s -> %s (%d)\n",#x,s_?s_:"?",r_); \
    fflush(stdout); return 1;} else { printf("ok   %s\n",#x); fflush(stdout);} }while(0)

/* sm_52 PTX: out[i] = a[i] + b[i]; portable across Turing/Ampere via JIT. */
static const char *k_ptx =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry vadd(.param .u64 p0, .param .u64 p1, .param .u64 p2)\n"
"{\n"
" .reg .b32 %r<4>; .reg .b64 %rd<10>;\n"
" ld.param.u64 %rd1, [p0]; ld.param.u64 %rd2, [p1]; ld.param.u64 %rd3, [p2];\n"
" cvta.to.global.u64 %rd4, %rd1; cvta.to.global.u64 %rd5, %rd2; cvta.to.global.u64 %rd6, %rd3;\n"
" mov.u32 %r1, %tid.x; mul.wide.u32 %rd7, %r1, 4;\n"
" add.s64 %rd8, %rd4, %rd7; ld.global.u32 %r2, [%rd8];\n"
" add.s64 %rd9, %rd5, %rd7; ld.global.u32 %r3, [%rd9];\n"
" add.s32 %r2, %r2, %r3;\n"
" add.s64 %rd8, %rd6, %rd7; st.global.u32 [%rd8], %r2;\n"
" ret;\n"
"}\n";

enum { S_INIT, S_DEV, S_CTX, S_ALLOC, S_CE, S_LAUNCH };

static int stage_of(const char *s)
{
    if (!s || !strcmp(s, "ce"))     return S_CE;
    if (!strcmp(s, "init"))         return S_INIT;
    if (!strcmp(s, "dev"))          return S_DEV;
    if (!strcmp(s, "ctx"))          return S_CTX;
    if (!strcmp(s, "alloc"))        return S_ALLOC;
    if (!strcmp(s, "launch"))       return S_LAUNCH;
    fprintf(stderr, "unknown stage '%s'\n", s);
    exit(2);
}

int main(int argc, char **argv)
{
    int stage = stage_of(argc > 1 ? argv[1] : NULL);
    CUdevice d;
    CUcontext ctx;
    CUdeviceptr da = 0, db = 0, dc = 0;
    int n = 0, maj = 0, min = 0;
    char nm[256];
    size_t tot = 0;

    printf("STAGE %s\n", argc > 1 ? argv[1] : "ce"); fflush(stdout);

    CK(cuInit(0));
    if (stage == S_INIT) goto done;

    CK(cuDeviceGetCount(&n));
    printf("devices=%d\n", n); fflush(stdout);
    if (n < 1) return 1;
    CK(cuDeviceGet(&d, 0));
    memset(nm, 0, sizeof(nm));
    CK(cuDeviceGetName(nm, sizeof(nm), d));
    printf("name=%s\n", nm); fflush(stdout);
    CK(cuDeviceGetAttribute(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d));
    CK(cuDeviceGetAttribute(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d));
    printf("compute=%d.%d\n", maj, min); fflush(stdout);
    CK(cuDeviceTotalMem(&tot, d));
    printf("totalMem=%zu MiB\n", (size_t)(tot >> 20)); fflush(stdout);
    if (stage == S_DEV) goto done;

    CK(cuCtxCreate(&ctx, 0, d));
    printf("CTX OK\n"); fflush(stdout);
    if (stage == S_CTX) { CK(cuCtxDestroy(ctx)); goto done; }

    CK(cuMemAlloc(&da, 4096));
    printf("MEMALLOC OK\n"); fflush(stdout);
    if (stage == S_ALLOC) { CK(cuMemFree(da)); CK(cuCtxDestroy(ctx)); goto done; }

    {
        unsigned hv = 0xabcd1234, rv = 0;
        CK(cuMemcpyHtoD(da, &hv, 4));
        CK(cuMemcpyDtoH(&rv, da, 4));
        printf("CE rv=0x%x want=0x%x -> %s\n", rv, hv, rv == hv ? "PASS" : "MISMATCH");
        fflush(stdout);
        if (rv != hv) return 1;
    }
    if (stage == S_CE) { CK(cuMemFree(da)); CK(cuCtxDestroy(ctx)); goto done; }

    {
        CUmodule mod; CUfunction fn;
        unsigned ha[32], hb[32], hc[32];
        void *args[3];
        int i, bad = 0;
        for (i = 0; i < 32; i++) { ha[i] = (unsigned)i; hb[i] = (unsigned)(100 + i); }
        CK(cuModuleLoadData(&mod, k_ptx));
        CK(cuModuleGetFunction(&fn, mod, "vadd"));
        CK(cuMemAlloc(&db, 128)); CK(cuMemAlloc(&dc, 128));
        CK(cuMemcpyHtoD(da, ha, 128));
        CK(cuMemcpyHtoD(db, hb, 128));
        args[0] = &da; args[1] = &db; args[2] = &dc;
        CK(cuLaunchKernel(fn, 1, 1, 1, 32, 1, 1, 0, 0, args, 0));
        CK(cuCtxSynchronize());
        CK(cuMemcpyDtoH(hc, dc, 128));
        for (i = 0; i < 32; i++) if (hc[i] != ha[i] + hb[i]) bad++;
        printf("LAUNCH bad=%d c[0]=%u c[31]=%u -> %s\n", bad, hc[0], hc[31],
               bad ? "MISMATCH" : "PASS");
        fflush(stdout);
        CK(cuMemFree(dc)); CK(cuMemFree(db)); CK(cuMemFree(da));
        CK(cuCtxDestroy(ctx));
        if (bad) return 1;
    }

done:
    printf("DONE\n"); fflush(stdout);
    return 0;
}
