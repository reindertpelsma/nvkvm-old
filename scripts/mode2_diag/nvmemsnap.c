/*
 * nvmemsnap.so — LD_PRELOAD trap+snapshot of NVIDIA mmap region CONTENTS, for host-vs-guest
 * data-plane divergence diffing (the surface the ioctl tracer can't see: what the GPU/kernel
 * DMA-writes into sysmem/USERD/ctx buffers that libcuda reads back, e.g. the rbp=0 crash).
 *
 * Three kinds of CPU-visible GPU memory are captured (NVIDIA establishes mappings 3 ways):
 *   (1) mmap(2) on an nvidia fd            -> readlink(/proc/self/fd/N) to identify.
 *   (2) NV_ESC_RM_MAP_MEMORY ioctl         -> register the returned pLinearAddress (NVOS33).
 *   (3) anonymous RW mmap MARKED by ioctl  -> libcuda mmaps anon host-CPU-RAM then marks it for
 *       GPU DMA via an ioctl (OS_DESCRIPTOR / UVM register) carrying that VA. We track all anon
 *       RW maps in an O(1) hash set and PROMOTE one when an nvidia ioctl's params point at it.
 *
 * We snapshot all registered regions pre+post each RM_ALLOC of NVSNAP_CLASS (default: every RM
 * ioctl if NVSNAP_ALL=1) and at exit. The guest crashes right AFTER allocating 0xc7c0, so the
 * 'post' snapshot at 0xc7c0 captures the exact bytes libcuda is about to read — diff that against
 * the host's. Run 3x host + 3x guest; nvmemsnap_diff.py applies the 3x3 noise filter (a byte is
 * flagged only if STABLE within host runs AND within guest runs AND host!=guest).
 *
 *   build: gcc -shared -fPIC -O2 -o nvmemsnap.so nvmemsnap.c -ldl -lpthread
 *   run:   LD_PRELOAD=./nvmemsnap.so NVSNAP=/tmp/snap.txt NVSNAP_CLASS=0xc7c0 [NVSNAP_ALL=1] \
 *          [NVSNAP_MAX=65536] <prog>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>

static int   (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

#define MAXR 1024
static struct { void *addr; size_t len; int prot; off_t off; char path[80]; } regs[MAXR];
static int nregs;

/* anon RW maps -> O(1) hash set of page-aligned bases (host-CPU-RAM marked for DMA via ioctl) */
#define ANONH (1u << 16)
static struct { uintptr_t base; size_t len; } anonh[ANONH];   /* base==0 => empty slot */

static FILE *lg;
static int   snap_seq;
static unsigned long trig_class = 0;
static int   snap_all;
static size_t snap_max = 65536;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static long n_ioctl, n_mmap, nanon, n_trig;
static int  memfd = -1;                 /* /proc/self/mem — fault-safe reads */
static void snapshot(const char *tag, uint32_t cmd, uint32_t cls);

/* fault-safe read of `n` bytes at `addr` from our own address space; returns bytes read.
 * pread(/proc/self/mem) returns -1/short on unmapped pages instead of SIGSEGV. */
static size_t read_mem(void *dst, uintptr_t addr, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(memfd, (char *)dst + got, n - got, (off_t)(addr + got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}

__attribute__((constructor)) static void init(void)
{
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_mmap  = dlsym(RTLD_NEXT, "mmap");
    memfd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    const char *p = getenv("NVSNAP");
    lg = p ? fopen(p, "w") : stderr;
    const char *c = getenv("NVSNAP_CLASS"); if (c) trig_class = strtoul(c, NULL, 0);
    snap_all = getenv("NVSNAP_ALL") ? 1 : 0;
    const char *m = getenv("NVSNAP_MAX"); if (m) snap_max = strtoul(m, NULL, 0);
}
__attribute__((destructor)) static void fini(void)
{
    if (!lg) return;
    snapshot("exit", 0, 0);
    fprintf(lg, "SUMMARY ioctls=%ld mmaps=%ld nvregs=%d anon=%ld triggers=%ld\n",
            n_ioctl, n_mmap, nregs, nanon, n_trig);
    fflush(lg);
}

static int is_nv(const char *p) { return p && strstr(p, "nvidia"); }

static void anon_add(uintptr_t base, size_t len)
{
    unsigned h = (unsigned)((base >> 12) & (ANONH - 1));
    for (unsigned i = 0; i < ANONH; i++) {
        unsigned s = (h + i) & (ANONH - 1);
        if (anonh[s].base == 0 || anonh[s].base == base) {
            anonh[s].base = base; anonh[s].len = len; return;
        }
    }
}
static size_t anon_find(uintptr_t v)   /* returns len if v is a known anon base, else 0 */
{
    unsigned h = (unsigned)((v >> 12) & (ANONH - 1));
    for (unsigned i = 0; i < ANONH; i++) {
        unsigned s = (h + i) & (ANONH - 1);
        if (anonh[s].base == 0) return 0;
        if (anonh[s].base == v) return anonh[s].len;
    }
    return 0;
}

static void register_region(void *addr, size_t len, int prot, off_t off, const char *path)
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nregs; i++)
        if (regs[i].addr == addr && regs[i].len == len) { pthread_mutex_unlock(&lk); return; }
    if (nregs < MAXR) {
        regs[nregs].addr = addr; regs[nregs].len = len; regs[nregs].prot = prot; regs[nregs].off = off;
        strncpy(regs[nregs].path, path, sizeof(regs[0].path) - 1);
        fprintf(lg, "REG #%d path=%s off=0x%lx len=0x%zx prot=0x%x (after %ld ioctls)\n",
                nregs, path, (long)off, len, prot, n_ioctl);
        nregs++;
    }
    pthread_mutex_unlock(&lk);
}

/* scan up to `n` bytes of a param buffer for an 8-byte value == a known anon base -> promote it.
 * Reads via /proc/self/mem so a wrong/oversized psz can never fault us. */
static void scan_for_anon(uintptr_t bufaddr, size_t n)
{
    if (!bufaddr) return;
    if (n > 4096) n = 4096;                      /* marking structs are small; bound the scan */
    static unsigned char tmp[4096];
    pthread_mutex_lock(&lk);
    size_t got = read_mem(tmp, bufaddr, n);
    for (size_t o = 0; o + 8 <= got; o += 8) {
        uintptr_t v; memcpy(&v, tmp + o, 8);
        if (v && anon_find(v)) {
            size_t len = anon_find(v);
            pthread_mutex_unlock(&lk);
            register_region((void *)v, len, PROT_READ | PROT_WRITE, 0, "[anon-dma]");
            pthread_mutex_lock(&lk);
        }
    }
    pthread_mutex_unlock(&lk);
}

#define SNAPBUF (1u << 20)
static unsigned char snapbuf[SNAPBUF];
static void snapshot(const char *tag, uint32_t cmd, uint32_t cls)
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nregs; i++) {
        if (!(regs[i].prot & PROT_READ)) continue;
        size_t n = regs[i].len; if (n > snap_max) n = snap_max; if (n > SNAPBUF) n = SNAPBUF;
        size_t got = read_mem(snapbuf, (uintptr_t)regs[i].addr, n);  /* fault-safe */
        fprintf(lg, "SNAP %s seq=%d cmd=0x%x class=0x%x path=%s off=0x%lx len=0x%zx got=0x%zx prot=0x%x h=",
                tag, snap_seq, cmd, cls, regs[i].path, (long)regs[i].off, regs[i].len, got, regs[i].prot);
        for (size_t k = 0; k < got; k++) fprintf(lg, "%02x", snapbuf[k]);
        fprintf(lg, "\n");
    }
    snap_seq++;
    fflush(lg);
    pthread_mutex_unlock(&lk);
}

void *mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
    __atomic_fetch_add(&n_mmap, 1, __ATOMIC_RELAXED);
    void *r = real_mmap(a, len, prot, flags, fd, off);
    if (r == MAP_FAILED) return r;
    if (fd >= 0) {
        char lnk[64], tgt[80]; tgt[0] = 0;
        snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(lnk, tgt, sizeof(tgt) - 1);
        if (n > 0) tgt[n] = 0;
        if (is_nv(tgt)) register_region(r, len, prot, off, tgt);
    }
    if ((flags & MAP_ANONYMOUS) && (prot & PROT_READ) && (prot & PROT_WRITE)) {
        pthread_mutex_lock(&lk); anon_add((uintptr_t)r, len); nanon++; pthread_mutex_unlock(&lk);
    }
    return r;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    unsigned nr = req & 0xff, type = (req >> 8) & 0xff, size = (req >> 16) & 0x3fff;
    uint32_t cls = 0, cmd = 0; void *pp = NULL; uint32_t psz = 0;
    if (type == 0x46 && arg) {
        if (nr == 0x2B) {
            cls = *(uint32_t *)((char *)arg + 12);
            pp  = (void *)(uintptr_t)(*(uint64_t *)((char *)arg + 16));
            psz = *(uint32_t *)((char *)arg + 24);
        } else if (nr == 0x2A) cmd = *(uint32_t *)((char *)arg + 8);
    }
    __atomic_fetch_add(&n_ioctl, 1, __ATOMIC_RELAXED);
    int trig = snap_all || (trig_class && nr == 0x2B && cls == trig_class);
    if (trig) { __atomic_fetch_add(&n_trig, 1, __ATOMIC_RELAXED);
                snapshot("pre", cmd ? cmd : cls, cls); }
    int rc = real_ioctl(fd, req, arg);
    if (type == 0x46 && arg) {
        /* OS_DESCRIPTOR (host-CPU-RAM marked for GPU DMA) carries the anon VA in the RM_ALLOC
         * params buffer pp (kernel-validated to psz bytes -> safe to read). We deliberately do
         * NOT scan `arg` by _IOC_SIZE: libcuda often passes a struct smaller than the ioctl's
         * encoded size, so reading `size` bytes overreads and faults. */
        if (pp && psz) scan_for_anon((uintptr_t)pp, psz);
        if (nr == 0x4e) {                                   /* NV_ESC_RM_MAP_MEMORY (NVOS33) */
            uint64_t va = 0, len = 0;
            read_mem(&va, (uintptr_t)arg + 32, 8);          /* pLinearAddress */
            read_mem(&len, (uintptr_t)arg + 24, 8);         /* length */
            if (va && len && len <= (1ull << 30))
                register_region((void *)(uintptr_t)va, (size_t)len,
                                PROT_READ | PROT_WRITE, 0, "[rm-map-mem]");
        }
    }
    if (trig) snapshot("post", cmd ? cmd : cls, cls);
    return rc;
}
