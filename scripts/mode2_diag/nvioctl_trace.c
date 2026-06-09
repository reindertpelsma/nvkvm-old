/* nvioctl_trace.so — LD_PRELOAD ioctl tracer for NVIDIA RM ioctls.
 * Decodes NVOS54 (RM_CONTROL, NR 0x2A) and NVOS21/64 (RM_ALLOC, NR 0x2B) so a guest vs
 * host run can be diffed to find the divergent ioctl (status/paramsSize/sequence).
 * No driver changes; runs identically on guest cup2 and host cup2_host.
 *   build:  gcc -shared -fPIC -O2 -o nvioctl_trace.so nvioctl_trace.c -ldl
 *   run:    LD_PRELOAD=./nvioctl_trace.so NVTRACE=/tmp/trace.txt <prog>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int (*real_ioctl)(int, unsigned long, ...);
static FILE *lg;
typedef unsigned long long CUdeviceptr;
typedef int CUresult;

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static unsigned patch_from_hex_file(void *dst, unsigned max, const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned n = 0;
    int hi = -1;
    if (!f) return 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        int v = hexval(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= max) break;
            ((unsigned char *)dst)[n++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    fclose(f);
    return n;
}
__attribute__((constructor)) static void init(void){
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    const char *p = getenv("NVTRACE");
    lg = p ? fopen(p, "w") : stderr;
}

static size_t round_up_page(size_t n)
{
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = ps > 0 ? (size_t)ps : 4096u;
    if (n > (size_t)-1 - (page - 1)) {
        return 0;
    }
    return (n + page - 1) & ~(page - 1);
}

static int uvm_shadow_enabled(void)
{
    const char *e = getenv("NVUVM_SHADOW");
    return e && e[0] && e[0] != '0';
}

static void trace_htod_shadow(CUdeviceptr dst, const void *src, size_t bytes, CUresult ret)
{
    if (!lg || !uvm_shadow_enabled() || ret != 0 || !src || !bytes) {
        return;
    }
    const char *md = getenv("NVUVM_SHADOW_MAX");
    size_t max = md ? strtoull(md, NULL, 0) : (16u << 20);
    if (bytes > max) {
        fprintf(lg, "CUDA_HTOD dst=0x%llx bytes=0x%zx shadow=SKIP max=0x%zx ret=%d\n",
                (unsigned long long)dst, bytes, max, ret);
        fflush(lg);
        return;
    }
    size_t len = round_up_page(bytes);
    if (!len) {
        return;
    }
    void *shadow = mmap(NULL, len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shadow == MAP_FAILED) {
        fprintf(lg, "CUDA_HTOD dst=0x%llx bytes=0x%zx shadow=MAP_FAILED len=0x%zx ret=%d\n",
                (unsigned long long)dst, bytes, len, ret);
        fflush(lg);
        return;
    }
    memcpy(shadow, src, bytes);
    fprintf(lg, "CUDA_HTOD dst=0x%llx bytes=0x%zx shadow=0x%llx len=0x%zx ret=%d",
            (unsigned long long)dst, bytes, (unsigned long long)(uintptr_t)shadow, len, ret);
    size_t dump = bytes < 16 ? bytes : 16;
    if (dump) {
        fprintf(lg, " data=");
        for (size_t i = 0; i < dump; i++) {
            fprintf(lg, "%02x", ((const unsigned char *)shadow)[i]);
        }
    }
    fprintf(lg, "\n");
    fflush(lg);
}

CUresult cuMemcpyHtoD_v2(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount)
{
    static CUresult (*real_fn)(CUdeviceptr, const void *, size_t);
    if (!real_fn) {
        real_fn = dlsym(RTLD_NEXT, "cuMemcpyHtoD_v2");
    }
    CUresult r = real_fn ? real_fn(dstDevice, srcHost, ByteCount) : 999;
    trace_htod_shadow(dstDevice, srcHost, ByteCount, r);
    return r;
}

CUresult cuMemcpyHtoD(CUdeviceptr dstDevice, const void *srcHost, unsigned int ByteCount)
{
    static CUresult (*real_fn)(CUdeviceptr, const void *, unsigned int);
    if (!real_fn) {
        real_fn = dlsym(RTLD_NEXT, "cuMemcpyHtoD");
    }
    CUresult r = real_fn ? real_fn(dstDevice, srcHost, ByteCount) : 999;
    trace_htod_shadow(dstDevice, srcHost, ByteCount, r);
    return r;
}

CUresult cuMemcpyDtoH_v2(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount)
{
    static CUresult (*real_fn)(void *, CUdeviceptr, size_t);
    if (!real_fn) {
        real_fn = dlsym(RTLD_NEXT, "cuMemcpyDtoH_v2");
    }
    CUresult r = real_fn ? real_fn(dstHost, srcDevice, ByteCount) : 999;
    if (lg) {
        fprintf(lg, "CUDA_DTOH src=0x%llx bytes=0x%zx dst=0x%llx ret=%d",
                (unsigned long long)srcDevice, ByteCount,
                (unsigned long long)(uintptr_t)dstHost, r);
        if (r == 0 && dstHost && ByteCount) {
            size_t dump = ByteCount < 16 ? ByteCount : 16;
            fprintf(lg, " data=");
            for (size_t i = 0; i < dump; i++) {
                fprintf(lg, "%02x", ((const unsigned char *)dstHost)[i]);
            }
        }
        fprintf(lg, "\n");
        fflush(lg);
    }
    return r;
}

CUresult cuMemcpyDtoH(void *dstHost, CUdeviceptr srcDevice, unsigned int ByteCount)
{
    static CUresult (*real_fn)(void *, CUdeviceptr, unsigned int);
    if (!real_fn) {
        real_fn = dlsym(RTLD_NEXT, "cuMemcpyDtoH");
    }
    CUresult r = real_fn ? real_fn(dstHost, srcDevice, ByteCount) : 999;
    if (lg) {
        fprintf(lg, "CUDA_DTOH src=0x%llx bytes=0x%x dst=0x%llx ret=%d\n",
                (unsigned long long)srcDevice, ByteCount,
                (unsigned long long)(uintptr_t)dstHost, r);
        fflush(lg);
    }
    return r;
}

int ioctl(int fd, unsigned long req, ...){
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void*); va_end(ap);
    unsigned nr = req & 0xff, type = (req >> 8) & 0xff;
    uint32_t cmd=0, psz=0, hcls=0; uint64_t pptr=0;
    if (type == 0x46 && arg) {
        if (nr == 0x2A) { cmd=*(uint32_t*)((char*)arg+8);  pptr=*(uint64_t*)((char*)arg+16); psz=*(uint32_t*)((char*)arg+24); }
        else if (nr == 0x2B) { hcls=*(uint32_t*)((char*)arg+12); pptr=*(uint64_t*)((char*)arg+16); psz=*(uint32_t*)((char*)arg+24); }
    }
    unsigned char pre_alloc[256];
    unsigned pre_alloc_n = 0;
    if (type == 0x46 && nr == 0x2B && pptr) {
        const char *ad = getenv("NVALLOC"); unsigned an = ad ? (unsigned)atoi(ad) : 32;
        if (an > sizeof(pre_alloc)) an = sizeof(pre_alloc);
        if (an) {
            memcpy(pre_alloc, (void *)(uintptr_t)pptr, an);
            pre_alloc_n = an;
        }
    }
    int r = real_ioctl(fd, req, arg);
    if (type == 0x46 && nr == 0x2B && pptr && pre_alloc_n &&
        getenv("NVRESTORE_GR_ALLOC")) {
        uint32_t lb = hcls & 0xffu, fam = (hcls >> 8) & 0xffu;
        if (fam >= 0xb0u && (lb == 0xc0u || lb == 0x97u)) {
            memcpy((void *)(uintptr_t)pptr, pre_alloc, pre_alloc_n);
        }
    }
    if (type == 0x46 && nr == 0x2A && pptr && r == 0) {
        uint32_t status = *(uint32_t*)((char*)arg+28);
        if (status == 0 && getenv("NVPATCH_GPUFLAGS") &&
            (cmd == 0x00000202u || cmd == 0x00000205u) && psz >= 8) {
            *(uint32_t*)((char*)(uintptr_t)pptr + 4) |= 1u; /* NV0000_CTRL_GPU_ID_INFO_IN_USE */
        }
        const char *classlist = getenv("NVCLASSLIST_HEX_FILE");
        if (status == 0 && classlist && cmd == 0x00800292u && psz) {
            patch_from_hex_file((void *)(uintptr_t)pptr, psz, classlist);
        }
    }
    if (type == 0 && nr == 33 && arg && lg) { /* UVM_MAP_EXTERNAL_ALLOCATION */
        uint64_t base = rd64((char *)arg + 0);
        uint64_t len  = rd64((char *)arg + 8);
        uint64_t off  = rd64((char *)arg + 16);
        int32_t rmfd  = (int32_t)rd32((char *)arg + 9248);
        uint32_t hcli = rd32((char *)arg + 9252);
        uint32_t hmem = rd32((char *)arg + 9256);
        uint32_t st   = rd32((char *)arg + 9260);
        fprintf(lg, "UVM_MAP_EXTERNAL base=0x%llx len=0x%llx off=0x%llx "
                    "rmfd=%d hClient=0x%08x hMemory=0x%08x rm_status=0x%x ret=%d req=0x%lx\n",
                (unsigned long long)base, (unsigned long long)len,
                (unsigned long long)off, rmfd, hcli, hmem, st, r, req);
        fflush(lg);
    }
    if (type == 0x46 && nr == 0x57 && arg && lg) { /* NV_ESC_RM_MAP_MEMORY_DMA */
        unsigned iosz = (unsigned)((req >> 16) & 0x3fffu);
        unsigned status_off = iosz >= 64 ? 56u : 48u;
        unsigned dma_off = iosz >= 64 ? 48u : 40u;
        uint32_t hcli = rd32((char *)arg + 0);
        uint32_t hdev = rd32((char *)arg + 4);
        uint32_t hdma = rd32((char *)arg + 8);
        uint32_t hmem = rd32((char *)arg + 12);
        uint64_t off = rd64((char *)arg + 16);
        uint64_t len = rd64((char *)arg + 24);
        uint32_t flags = rd32((char *)arg + 32);
        uint32_t flags2 = iosz >= 64 ? rd32((char *)arg + 36) : 0;
        uint32_t kind = iosz >= 64 ? rd32((char *)arg + 40) : 0;
        uint64_t dma = rd64((char *)arg + dma_off);
        uint32_t st = rd32((char *)arg + status_off);
        fprintf(lg, "MAPDMA iosz=%u hClient=0x%08x hDevice=0x%08x "
                    "hDma=0x%08x hMemory=0x%08x off=0x%llx len=0x%llx "
                    "flags=0x%08x flags2=0x%08x kind=0x%08x dmaOffset=0x%llx "
                    "status=0x%x ret=%d req=0x%lx\n",
                iosz, hcli, hdev, hdma, hmem, (unsigned long long)off,
                (unsigned long long)len, flags, flags2, kind,
                (unsigned long long)dma, st, r, req);
        fflush(lg);
        return r;
    }
    if (type == 0x46 && lg) {
        if (nr == 0x2A) fprintf(lg, "CTRL  cmd=0x%08x psz=%-6u status=0x%-4x params=0x%llx",
                                cmd, psz, rd32((char*)arg+28), (unsigned long long)pptr);
        else if (nr == 0x2B) fprintf(lg, "ALLOC class=0x%08x psz=%-6u status=0x%-4x params=0x%llx",
                                hcls, psz, rd32((char*)arg+28), (unsigned long long)pptr);
        else { fprintf(lg, "IOCTL nr=0x%02x ret=%d\n", nr, r); fflush(lg); return r; }
        /* dump first N bytes of the params buffer (post-call) so guest vs host CONTENT can be
         * diffed: a control returning real data on the host but NV_OK+zeros on the guest shows up
         * as content=00.. vs nonzero. NVCONTENT env = N (default 48, 0=off). */
        const char *cd = getenv("NVCONTENT"); unsigned cn = cd ? (unsigned)atoi(cd) : 48;
        if (cn > 1024) cn = 1024;
        /* For ALLOC the kernel writes the CLASS param size back even when libcuda passes psz=0
         * (e.g. GR object 0xc7c0 -> 16B caps). Dump a fixed NVALLOC bytes from pAllocParms so the
         * forwarded GR-alloc reply can be compared host-vs-guest. NVALLOC env = N (default 32). */
        if (nr == 0x2B) {
            const char *ad = getenv("NVALLOC"); unsigned an = ad ? (unsigned)atoi(ad) : 32;
            if (an > 256) an = 256;
            if (an && pptr) {
                if (pre_alloc_n) {
                    fprintf(lg, " apre=");
                    unsigned pn = pre_alloc_n < an ? pre_alloc_n : an;
                    for (unsigned i = 0; i < pn; i++) fprintf(lg, "%02x", pre_alloc[i]);
                }
                fprintf(lg, " areply=");
                for (unsigned i = 0; i < an; i++) fprintf(lg, "%02x", ((unsigned char *)(uintptr_t)pptr)[i]);
            }
            /* Also dump the OUTER NVOS64 struct (arg) post-call so the outer-struct layout /
             * copyout can be diffed host-vs-guest — the rbp-clobber is an alloc-reply ABI
             * mismatch, likely in the outer struct, not the params buffer. NVOUTER env = N (default 64). */
            const char *od = getenv("NVOUTER"); unsigned on = od ? (unsigned)atoi(od) : 64;
            if (on > 256) on = 256;
            if (on && arg) {
                fprintf(lg, " outer=");
                for (unsigned i = 0; i < on; i++) fprintf(lg, "%02x", ((unsigned char *)arg)[i]);
            }
        } else if (cn && pptr) {
            unsigned lim = (psz < cn) ? psz : cn;
            fprintf(lg, " content=");
            for (unsigned i = 0; i < lim; i++) fprintf(lg, "%02x", ((unsigned char *)(uintptr_t)pptr)[i]);
        }
        fprintf(lg, "\n"); fflush(lg);
    }
    return r;
}
