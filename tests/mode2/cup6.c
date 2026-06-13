// cup6.c — DISCRIMINATOR for the cup5 bulk-copy slowness: UC cacheability vs
// per-page demand-fault/residency overhead. Same alloc as cup5, but:
//   - times HtoD TWICE to the SAME buffer. UC writes stay slow forever; a
//     first-touch fault/residency cost makes copy #2 jump to memory speed.
//   - holds the buffer + sleeps so the harness can read the LITERAL memtype
//     from /sys/kernel/debug/x86/pat_memtype_list (write-back vs uncached-minus
//     vs write-combining) while the mapping is live.
// Build: gcc -O2 -o cup6 cup6.c -lcuda
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CK(x) do{ CUresult r=(x); if(r){ const char*s=0; cuGetErrorString(r,&s); \
  printf("FAIL %s -> %s (%d)\n",#x,s?s:"?",(int)r); fflush(stdout); return 1; } }while(0)
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (double)t.tv_sec + (double)t.tv_nsec*1e-9; }
int main(void)
{
    size_t MB = getenv("CUP6_MB") ? (size_t)atoi(getenv("CUP6_MB")) : 64;
    size_t N = MB << 20;
    CK(cuInit(0));
    CUdevice d; CK(cuDeviceGet(&d,0));
    CUcontext c; CK(cuCtxCreate(&c,0,d));
    CUdeviceptr dp; CK(cuMemAlloc(&dp,N));
    printf("alloc %zuMB dp=0x%llx pid=%d OK\n", MB, (unsigned long long)dp, (int)getpid());
    fflush(stdout);
    unsigned char *h = malloc(N);
    if(!h){ printf("FAIL host malloc\n"); return 1; }
    for(size_t i=0;i<N;i++) h[i]=(unsigned char)((i*131u+7u)&0xff);

    // copy #1 (first touch — pays any fault/residency cost)
    double t0=now(); CK(cuMemcpyHtoD(dp,h,N)); CK(cuCtxSynchronize()); double t1=now();
    printf("HtoD#1 %.3fs (%.1f MB/s)\n", t1-t0, (double)MB/(t1-t0)); fflush(stdout);
    // copy #2 (same buffer — already resident if it was a first-touch cost)
    double t2=now(); CK(cuMemcpyHtoD(dp,h,N)); CK(cuCtxSynchronize()); double t3=now();
    printf("HtoD#2 %.3fs (%.1f MB/s)\n", t3-t2, (double)MB/(t3-t2)); fflush(stdout);
    // copy #3 (third, confirm steady state)
    double t4=now(); CK(cuMemcpyHtoD(dp,h,N)); CK(cuCtxSynchronize()); double t5=now();
    printf("HtoD#3 %.3fs (%.1f MB/s)\n", t5-t4, (double)MB/(t5-t4)); fflush(stdout);
    printf("INTERPRET: #2/#3 >> #1 => per-page first-touch fault/residency; "
           "#1~#2~#3 (all slow) => persistent (UC / per-copy overhead)\n");
    fflush(stdout);
    printf("HOLDING buffer 25s — read pat_memtype_list now\n"); fflush(stdout);
    sleep(25);
    return 0;
}
