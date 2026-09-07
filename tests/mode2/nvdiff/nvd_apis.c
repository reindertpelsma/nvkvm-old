/*
 * nvd_apis.c — the WIDE memory-API census workload.
 *
 * ## The question this program exists to answer
 *
 * `NV_ESC_RM_MAP_MEMORY_DMA` — ioctl nr **0x57 = 87** on /dev/nvidia* , RM's
 * `NV04_MAP_MEMORY_DMA` (research_clones/ogkm-580.159.04/.../nvos.h:2166, NVOS46) —
 * appears **ZERO times** across all twelve committed captures in
 * `traces/host_reference_ga106/`. Its sibling `RM_UNMAP_MEMORY_DMA` (0x58 = 88) likewise.
 *
 * ⊘ That zero is an ABSENCE, and this tree has paid for reading an absence as a fact
 * more than once ("an absence is not evidence without a known-positive"; the C oracle's
 * `dlen=0` rows; the serial-log trap). Those twelve captures run exactly one workload —
 * `nvd_prog.c` — whose entire memory vocabulary is `cuCtxCreate`, `cuMemAlloc`, one CE
 * round trip and one launch. RM serves 0x57 in full and it carries a documented
 * client-facing flag set (`NVOS46_FLAGS_*`), so the honest reading of the zero is
 * **"we never exercised the API family that would issue it"**.
 *
 * ⇒ This program walks the client-facing memory surface that `nvd_prog.c` does not,
 * one stage per API family, so a RECORD COUNT can settle it per family.
 * ⚠ It is a NEW program on purpose. `nvd_prog.c` must not change: its md5
 * (`7af4c3ec2b702cf04ae00d68a6596c4d`) is the recorded provenance of every committed
 * reference trace in `traces/host_reference_ga106/MANIFEST.txt`, and editing it would
 * invalidate them.
 *
 * ## Stages, in descending order of how likely they are to reach 0x57
 *
 *   nvd_apis vmm      CUDA VMM: AddressReserve/MemCreate/MemMap/SetAccess/Unmap/Release
 *                     ★ RANK 1 — this API exists precisely for client-managed explicit
 *                     mapping, which is what NV04_MAP_MEMORY_DMA provides.
 *   nvd_apis hostreg  cuMemHostRegister(DEVICEMAP) + cuMemHostGetDevicePointer,
 *                     cuMemAllocHost / cuMemHostAlloc. ★ RANK 2 — placing ordinary
 *                     process pages into the GPU's VAS is the escape's literal job
 *                     (0x4E RM_MAP_MEMORY maps into the CPU's; 0x57 into the device's).
 *   nvd_apis ipc      cuIpcGetMemHandle here + cuIpcOpenMemHandle in a REAL second
 *                     process (self-exec, see run_ipc()).
 *   nvd_apis peer     cuDeviceCanAccessPeer / cuCtxEnablePeerAccess / cuMemcpyPeer.
 *                     SKIPPED with the device count printed when there is only one GPU.
 *   nvd_apis mapped   cuMemAllocManaged + cuMemAdvise + cuMemPrefetchAsync both ways.
 *   nvd_apis arrays   cuArrayCreate / cuMipmappedArrayCreate / cuTexObjectCreate.
 *   nvd_apis base     ★ THE CONTROL — plain cuMemAlloc + HtoD + DtoH + free, i.e. the
 *                     `nvd_prog ce` shape. It MUST be captured in the same run: without
 *                     a known-negative in the same table, "0 in stage X" cannot be told
 *                     from "the counter is broken".
 *   nvd_apis ipcimport <hex>   internal; the second process of the `ipc` stage.
 *
 * ## The grading rule, and it is the whole point
 *
 * Every stage prints exactly one of:
 *   OK       — the family was exercised AND its data check passed. A 0x57 count of zero
 *              from an OK stage is a MEASUREMENT.
 *   FAIL <r> — the family could not be exercised (an API refused, a device pointer was
 *              never obtained, a verify mismatched). ⊘ A 0x57 count from a FAIL stage is
 *              UNMEASURED and must not be tabulated as a zero.
 *   SKIPPED <r> — this hardware cannot run the family at all.
 * and exits 0 regardless: the ioctl census is the measurement, not the CUresult. Every
 * stage also prints its allocation sizes, its pointers and its verify results, so
 * "ran and issued nothing" is distinguishable from "silently did nothing" from the
 * stdout alone, without the trace.
 *
 * Build: cc -O0 -o nvd_apis nvd_apis.c -lcuda -ldl
 *        (add -DNVD_NO_CUDA_H -I. on a box with libcuda but no toolkit)
 * Run:   NVDIFF_OUT=t.jsonl NVDIFF_MAXBUF=65536 LD_PRELOAD=./nvdiff_shim.so ./nvd_apis vmm
 */
/* ⊘ -DNVD_NO_CUDA_H selects the bundled stand-in, exactly as nvd_prog.c does. Unlike
 * nvd_prog.c this program is NOT one half of a host<->guest differential, so the two
 * header modes need not be pinned together — but every entry point it uses is still
 * spelled with its explicit `_v2` suffix where one exists, because a silently bound v1
 * symbol is the failure mode nvd_cuda_min.h's top comment was written for. */
/* ⚠ `_GNU_SOURCE` before ANY libc header: `RTLD_DEFAULT` is a GNU extension and
 * `<dlfcn.h>` hides it otherwise. Measured 2026-09-07 on the w388 bench — without
 * this the build dies with `'RTLD_DEFAULT' undeclared`, and the capture script
 * mis-reports that as a LINK failure against -lcuda, which sends you looking for a
 * missing dev symlink that is not the problem. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef NVD_NO_CUDA_H
#include "nvd_cuda_min.h"
#else
#include <cuda.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/* ---------------------------------------------------------------------------
 * PHASE MARKERS — identical mechanism to nvd_prog.c, and here they carry more weight
 * than they do there.
 *
 * A stage-level count answers "does this FAMILY issue 0x57". It cannot answer "which
 * CALL issued it", and for the VMM stage — six distinct entry points in one lifecycle —
 * that is the interesting half. `_IOW('F', 0x7f, char[32])` on /dev/nvidiactl is not a
 * real NVIDIA escape (RM's escapes are all well below 0x7f), so the driver returns
 * EINVAL and does nothing; the LD_PRELOAD recorder still captures the call with our 32
 * ASCII bytes as its header, giving every later record a named phase.
 *
 * ⊘ If /dev/nvidiactl cannot be opened the markers are silently absent, so the analysis
 * must REFUSE to phase-split an unmarked trace rather than assume one phase. The census
 * script checks for the marker and says which mode it is in.
 * ------------------------------------------------------------------------ */
#define NVD_MARK_IOC _IOW('F', 0x7f, char[32])
static int g_markfd = -1;
static void mark(const char *tag)
{
    char buf[32];
    if (g_markfd < 0)
        return;
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "NVDMARK:%s", tag);
    ioctl(g_markfd, NVD_MARK_IOC, buf);   /* EINVAL expected; the RECORD is the point */
}

static const char *estr(CUresult r)
{
    const char *s = 0;
    cuGetErrorString(r, &s);
    return s ? s : "(no string)";
}

/* CK: only for the setup calls whose failure means there is nothing to measure at all. */
#define CK(x) do{ CUresult r_=(x); if(r_!=CUDA_SUCCESS){ \
    printf("FAIL setup %s -> %s (%d)\n",#x,estr(r_),(int)r_); \
    fflush(stdout); return 1;} }while(0)

/* TRY: for every call inside a stage. A failing CUDA call is data, not an abort — the
 * census is the measurement. Prints the numeric CUresult AND the driver's own string so
 * a report can quote what the process was told rather than paraphrasing it. */
#define TRY(tag, x) ({ CUresult r__ = (x); \
    printf("  TRY %-24s %-46s rc=%d %s\n", tag, #x, (int)r__, estr(r__)); \
    fflush(stdout); r__; })

/* Every stage ends through exactly one of these three, so the census script can grade a
 * capture from stdout with a single per-stage line and never has to guess. */
static int stage_ok(const char *what)
{ printf("OK %s\n", what); printf("DONE\n"); fflush(stdout); return 0; }
static int stage_fail(const char *why)
{ printf("FAIL %s\n", why); printf("DONE\n"); fflush(stdout); return 0; }
static int stage_skip(const char *why)
{ printf("SKIPPED %s\n", why); printf("DONE\n"); fflush(stdout); return 0; }

/* ---------------------------------------------------------------------------
 * ⚠⚠ cuMemAdvise / cuMemPrefetchAsync are resolved with dlsym, and this is not
 * fastidiousness — it is the exact class of silent divergence nvd_cuda_min.h exists to
 * prevent, arriving in a NEW place.
 *
 * CUDA 12.2 introduced `cuMemAdvise_v2` and `cuMemPrefetchAsync_v2` with an INCOMPATIBLE
 * argument list (a `CUmemLocation` struct plus a flags word, in place of a `CUdevice`),
 * and `cuda.h` #defines the short names onto the _v2 symbols from that toolkit forward.
 * So a source-level call to `cuMemPrefetchAsync(...)` means a DIFFERENT ABI depending on
 * which toolkit header the build box happens to have — the binary still links and still
 * runs, and the trace it emits is a different trace.
 *
 * dlsym() on the BARE name binds the v1 entry point on every driver that has ever
 * exported it, independent of any header. If the symbol is absent we say so and the
 * stage reports the fact rather than measuring a zero.
 * ------------------------------------------------------------------------ */
typedef CUresult (*fn_advise_v1)(CUdeviceptr, size_t, int /*CUmem_advise*/, CUdevice);
typedef CUresult (*fn_prefetch_v1)(CUdeviceptr, size_t, CUdevice, CUstream);
static fn_advise_v1   p_memAdvise;
static fn_prefetch_v1 p_memPrefetch;
static void resolve_uvm_hints(void)
{
    p_memAdvise   = (fn_advise_v1)  dlsym(RTLD_DEFAULT, "cuMemAdvise");
    p_memPrefetch = (fn_prefetch_v1)dlsym(RTLD_DEFAULT, "cuMemPrefetchAsync");
    /* ⚠ printed as resolved/MISSING rather than as an address: ISO C has no conversion
     * from a function pointer to void*, and a %p on one is a portability warning for no
     * information gain. What matters is whether the v1 entry point EXISTS. */
    printf("  dlsym cuMemAdvise=%s cuMemPrefetchAsync=%s (v1 ABI, by bare name)\n",
           p_memAdvise ? "resolved" : "MISSING",
           p_memPrefetch ? "resolved" : "MISSING");
    fflush(stdout);
}

/* A device-side write we can read back on the host: proves the mapping was USED, not
 * merely created. Returns 0 on match. */
static int dev_roundtrip(CUdeviceptr dp, size_t bytes, const char *tag)
{
    unsigned *h = (unsigned *)malloc(bytes);
    size_t i, n = bytes / 4;
    unsigned pat = 0xa5c30000u;
    int bad = 0;
    CUresult r1, r2, r3;
    if (!h) return 1;
    for (i = 0; i < n; i++) h[i] = pat + (unsigned)i;
    r1 = cuMemcpyHtoD(dp, h, bytes);
    memset(h, 0, bytes);
    r2 = cuMemcpyDtoH(h, dp, bytes);
    r3 = cuCtxSynchronize();
    for (i = 0; i < n; i++) if (h[i] != pat + (unsigned)i) bad++;
    printf("  ROUNDTRIP %-14s dptr=0x%llx bytes=%zu rc=%d/%d/%d bad=%d first=0x%08x -> %s\n",
           tag, (unsigned long long)dp, bytes, (int)r1, (int)r2, (int)r3, bad, h[0],
           (r1 || r2 || r3 || bad) ? "MISMATCH" : "PASS");
    fflush(stdout);
    free(h);
    return (r1 || r2 || r3 || bad) ? 1 : 0;
}

/* ==========================================================================
 * STAGE base — THE CONTROL.
 *
 * This is the `nvd_prog ce` shape, reproduced here so that every capture of this
 * program contains a known-negative recorded by the SAME instrument, in the SAME run
 * set, on the SAME box. Without it, "vmm issued 0 x 0x57" and "the counter never fires"
 * are the same observation.
 * ========================================================================== */
static int run_base(CUdevice d)
{
    CUcontext ctx = 0;
    CUdeviceptr da = 0;
    size_t free_b = 0, total_b = 0;
    int bad;

    CK(cuCtxCreate(&ctx, 0, d));
    if (TRY("meminfo", cuMemGetInfo_v2(&free_b, &total_b)) == CUDA_SUCCESS)
        printf("  fb free=%zu MiB total=%zu MiB\n", free_b >> 20, total_b >> 20);
    if (TRY("alloc_1MiB", cuMemAlloc(&da, 1 << 20)) != CUDA_SUCCESS) {
        cuCtxDestroy(ctx);
        return stage_fail("base: cuMemAlloc refused, nothing was exercised");
    }
    printf("  da=0x%llx size=%u\n", (unsigned long long)da, 1u << 20);
    bad = dev_roundtrip(da, 1 << 20, "base");
    TRY("free", cuMemFree(da));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    return bad ? stage_fail("base: HtoD/DtoH round trip mismatched")
               : stage_ok("base cuMemAlloc+HtoD+DtoH+cuMemFree (the KNOWN-NEGATIVE control)");
}

/* ==========================================================================
 * STAGE vmm — ★ RANK 1.
 *
 * The CUDA virtual-memory-management API is the one public surface where the client
 * splits reservation, backing and mapping into separate calls it issues itself. That is
 * the same decomposition NV04_MAP_MEMORY_DMA offers (hDevice + hMemory + dmaOffset ->
 * a mapping in the device's DMA address space), so if any CUDA path reaches 0x57 this
 * is the first place to look.
 *
 * Four sub-phases, marked separately so the trace can attribute a hit to a call:
 *   vmm_reserve  cuMemAddressReserve
 *   vmm_create   cuMemCreate                (physical backing, no VA yet)
 *   vmm_map      cuMemMap + cuMemSetAccess  <- the mapping proper
 *   vmm_alias    a SECOND cuMemMap of the SAME handle at a different VA. ★ Deliberate:
 *                one physical allocation reachable at two VAs cannot be expressed by a
 *                single "allocate and map" ioctl; if anything forces an explicit
 *                map-into-VAS escape, aliasing is it.
 *   vmm_export   an EXPORTABLE handle round-tripped through a POSIX fd, which takes the
 *                dup-through-fd route in RM rather than the plain one.
 * ========================================================================== */
static int run_vmm(CUdevice d)
{
    CUcontext ctx = 0;
    CUmemAllocationProp prop;
    CUmemAccessDesc acc;
    CUmemGenericAllocationHandle h = 0, himp = 0;
    CUdeviceptr va = 0, va2 = 0, vae = 0;
    size_t gran = 0, sz;
    int supported = 0, fd_supported = 0, bad = 0, exp_fd = -1;

    CK(cuDeviceGetAttribute(&supported,
        CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED, d));
    CK(cuDeviceGetAttribute(&fd_supported,
        CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED, d));
    printf("  vmm_supported=%d posix_fd_handle_supported=%d\n", supported, fd_supported);
    fflush(stdout);
    if (!supported)
        return stage_skip("vmm: CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED=0");

    CK(cuCtxCreate(&ctx, 0, d));

    memset(&prop, 0, sizeof(prop));
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_NONE;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = (int)d;
    if (TRY("granularity", cuMemGetAllocationGranularity(&gran, &prop,
                               CU_MEM_ALLOC_GRANULARITY_MINIMUM)) != CUDA_SUCCESS)
        { cuCtxDestroy(ctx); return stage_fail("vmm: granularity query refused"); }
    /* round 4 MiB up to the device's minimum granularity — cuMemCreate rejects anything
     * that is not a multiple, and a refusal there would measure our arithmetic. */
    sz = gran ? (((4u << 20) + gran - 1) / gran) * gran : (4u << 20);
    printf("  granularity=%zu  size=%zu\n", gran, sz); fflush(stdout);

    mark("vmm_reserve");
    /* reserve TWICE the size in one shot so the alias sub-phase has a second VA that is
     * known-free; a reservation that fails later would confound the alias measurement. */
    if (TRY("addressreserve", cuMemAddressReserve(&va, sz * 2, 0, 0, 0)) != CUDA_SUCCESS)
        { cuCtxDestroy(ctx); return stage_fail("vmm: cuMemAddressReserve refused"); }
    va2 = va + sz;
    printf("  reserved va=0x%llx va2=0x%llx span=%zu\n",
           (unsigned long long)va, (unsigned long long)va2, sz * 2);
    fflush(stdout);

    mark("vmm_create");
    if (TRY("memcreate", cuMemCreate(&h, sz, &prop, 0)) != CUDA_SUCCESS) {
        TRY("addressfree", cuMemAddressFree(va, sz * 2));
        cuCtxDestroy(ctx);
        return stage_fail("vmm: cuMemCreate refused");
    }
    printf("  handle=0x%llx (physical backing, NO virtual address yet)\n",
           (unsigned long long)h);
    fflush(stdout);

    mark("vmm_map");
    if (TRY("memmap", cuMemMap(va, sz, 0, h, 0)) != CUDA_SUCCESS) bad = 1;
    memset(&acc, 0, sizeof(acc));
    acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    acc.location.id = (int)d;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    if (!bad && TRY("setaccess", cuMemSetAccess(va, sz, &acc, 1)) != CUDA_SUCCESS) bad = 1;

    /* TOUCH IT. A mapping that is created and never used proves nothing about whether
     * the mapping path ran — RM is free to defer. */
    if (!bad) bad |= dev_roundtrip(va, 64 << 10, "vmm_mapped");

    mark("vmm_alias");
    /* ★ the same physical handle at a SECOND virtual address, both live at once. */
    if (!bad) {
        if (TRY("memmap_alias", cuMemMap(va2, sz, 0, h, 0)) == CUDA_SUCCESS) {
            if (TRY("setaccess_alias", cuMemSetAccess(va2, sz, &acc, 1)) == CUDA_SUCCESS) {
                unsigned probe = 0;
                /* write through the FIRST VA, read through the SECOND: the only way this
                 * can pass is if both VAs really resolve to the one allocation. */
                unsigned magic = 0x5eed5eed;
                TRY("alias_htod", cuMemcpyHtoD(va, &magic, 4));
                TRY("alias_dtoh", cuMemcpyDtoH(&probe, va2, 4));
                printf("  ALIAS write@va read@va2 got=0x%08x want=0x%08x -> %s\n",
                       probe, magic, probe == magic ? "PASS" : "MISMATCH");
                fflush(stdout);
                if (probe != magic) bad = 1;
            }
            TRY("unmap_alias", cuMemUnmap(va2, sz));
        } else {
            printf("  ALIAS not established — the alias sub-phase is UNMEASURED, "
                   "the rest of the stage still stands\n");
            fflush(stdout);
        }
    }

    mark("vmm_export");
    /* An EXPORTABLE allocation goes out through a POSIX fd and comes back as a fresh
     * handle, which is a different RM route (object dup across an fd) to the same
     * physical memory. Non-fatal: if the device does not advertise the handle type we
     * say so and the stage is still graded on the main lifecycle. */
    if (fd_supported) {
        CUmemAllocationProp ep = prop;
        CUmemGenericAllocationHandle he = 0;
        ep.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
        if (TRY("memcreate_exp", cuMemCreate(&he, sz, &ep, 0)) == CUDA_SUCCESS) {
            if (TRY("export_to_fd",
                    cuMemExportToShareableHandle(&exp_fd, he,
                        CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0)) == CUDA_SUCCESS) {
                printf("  exported fd=%d\n", exp_fd); fflush(stdout);
                if (TRY("import_from_fd",
                        cuMemImportFromShareableHandle(&himp, (void *)(long)exp_fd,
                            CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR)) == CUDA_SUCCESS) {
                    if (TRY("memaddrreserve_exp",
                            cuMemAddressReserve(&vae, sz, 0, 0, 0)) == CUDA_SUCCESS &&
                        TRY("memmap_exp", cuMemMap(vae, sz, 0, himp, 0)) == CUDA_SUCCESS &&
                        TRY("setaccess_exp",
                            cuMemSetAccess(vae, sz, &acc, 1)) == CUDA_SUCCESS) {
                        (void)dev_roundtrip(vae, 4096, "vmm_imported");
                        TRY("unmap_exp", cuMemUnmap(vae, sz));
                    }
                    if (vae) TRY("addrfree_exp", cuMemAddressFree(vae, sz));
                    TRY("release_imported", cuMemRelease(himp));
                }
                close(exp_fd);
            }
            TRY("release_exportable", cuMemRelease(he));
        }
    } else {
        printf("  export sub-phase SKIPPED (no POSIX-fd handle support on this device)\n");
        fflush(stdout);
    }

    mark("vmm_teardown");
    TRY("unmap", cuMemUnmap(va, sz));
    TRY("release", cuMemRelease(h));
    TRY("addressfree", cuMemAddressFree(va, sz * 2));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    return bad ? stage_fail("vmm: the mapped range did not round-trip")
               : stage_ok("vmm reserve/create/map/setaccess/alias/unmap/release");
}

/* ==========================================================================
 * STAGE hostreg — ★ RANK 2.
 *
 * `cuMemHostRegister(..., CU_MEMHOSTREGISTER_DEVICEMAP)` asks RM to place ordinary,
 * already-existing process pages into the GPU's virtual address space, and
 * `cuMemHostGetDevicePointer` then hands back the device VA it chose. That is what
 * NV04_MAP_MEMORY_DMA is FOR — as opposed to NV04_MAP_MEMORY (0x4E), which is present
 * in every existing capture and maps into the CPU's address space instead.
 *
 * The proof that the mapping was used, and not merely created: the GPU writes through
 * the device pointer (cuMemsetD32) and the HOST buffer is then read directly. If the
 * two do not agree, no device mapping was in play and the stage says so.
 * ========================================================================== */
static int run_hostreg(CUdevice d)
{
    CUcontext ctx = 0;
    void *hbuf = NULL, *hpin = NULL, *halloc = NULL;
    CUdeviceptr dp = 0, dp2 = 0;
    const size_t SZ = 1 << 20;      /* 1 MiB, page-aligned; RM pins whole pages */
    unsigned flags = 0;
    int canmap = 0, bad = 0, sawdev = 0;

    CK(cuDeviceGetAttribute(&canmap, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY, d));
    printf("  can_map_host_memory=%d\n", canmap); fflush(stdout);
    if (!canmap)
        return stage_skip("hostreg: CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY=0");

    CK(cuCtxCreate(&ctx, 0, d));

    /* --- (1) register EXISTING anonymous process memory -------------------------- */
    mark("hostreg_register");
    if (posix_memalign(&hbuf, 4096, SZ) != 0 || !hbuf) {
        cuCtxDestroy(ctx); return stage_fail("hostreg: posix_memalign failed");
    }
    memset(hbuf, 0, SZ);
    printf("  hbuf=%p size=%zu (anonymous, page-aligned, pre-existing)\n", hbuf, SZ);
    fflush(stdout);
    if (TRY("hostregister", cuMemHostRegister_v2(hbuf, SZ,
                CU_MEMHOSTREGISTER_DEVICEMAP | CU_MEMHOSTREGISTER_PORTABLE))
            != CUDA_SUCCESS) {
        free(hbuf); TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("hostreg: cuMemHostRegister refused — family NOT exercised");
    }
    mark("hostreg_getdevptr");
    if (TRY("getdevicepointer",
            cuMemHostGetDevicePointer_v2(&dp, hbuf, 0)) == CUDA_SUCCESS) {
        sawdev = 1;
        printf("  DEVICE POINTER for host page = 0x%llx (host %p)  same_va=%d\n",
               (unsigned long long)dp, hbuf,
               (unsigned long long)dp == (unsigned long long)(size_t)hbuf);
        fflush(stdout);
    }
    TRY("hostgetflags", cuMemHostGetFlags(&flags, hbuf));
    printf("  host flags=0x%x\n", flags); fflush(stdout);

    /* ★ THE USE. The GPU writes 0x1234beef into the first 4096 words through the DEVICE
     * pointer; we then read the HOST buffer with the CPU. A match can only happen if the
     * host pages really were mapped into the GPU's VAS. */
    if (sawdev) {
        mark("hostreg_gpu_write");
        if (TRY("memsetD32_via_devptr",
                cuMemsetD32_v2(dp, 0x1234beefu, 4096)) == CUDA_SUCCESS &&
            TRY("sync", cuCtxSynchronize()) == CUDA_SUCCESS) {
            unsigned *w = (unsigned *)hbuf;
            size_t i; int nbad = 0;
            for (i = 0; i < 4096; i++) if (w[i] != 0x1234beefu) nbad++;
            printf("  GPU-WROTE-HOST-PAGES words=4096 bad=%d w[0]=0x%08x -> %s\n",
                   nbad, w[0], nbad ? "MISMATCH" : "PASS");
            fflush(stdout);
            if (nbad) bad = 1;
        } else bad = 1;
    } else {
        bad = 1;   /* no device pointer => the DEVICEMAP half never happened */
    }
    mark("hostreg_unregister");
    TRY("hostunregister", cuMemHostUnregister(hbuf));
    free(hbuf);

    /* --- (2) cuMemAllocHost: RM allocates the page-locked sysmem itself ----------- */
    mark("hostreg_allochost");
    if (TRY("memallochost", cuMemAllocHost_v2(&halloc, SZ)) == CUDA_SUCCESS) {
        CUdeviceptr da = 0;
        printf("  cuMemAllocHost -> %p\n", halloc); fflush(stdout);
        if (TRY("alloc_dev", cuMemAlloc(&da, SZ)) == CUDA_SUCCESS) {
            unsigned *w = (unsigned *)halloc, v = 0;
            w[0] = 0xfeed0001u;
            TRY("htod_from_pinned", cuMemcpyHtoD(da, halloc, SZ));
            TRY("dtoh_to_stack", cuMemcpyDtoH(&v, da, 4));
            printf("  PINNED-HtoD got=0x%08x want=0x%08x -> %s\n",
                   v, 0xfeed0001u, v == 0xfeed0001u ? "PASS" : "MISMATCH");
            fflush(stdout);
            if (v != 0xfeed0001u) bad = 1;
            TRY("free_dev", cuMemFree(da));
        }
        TRY("memfreehost", cuMemFreeHost(halloc));
    }

    /* --- (3) cuMemHostAlloc(DEVICEMAP|WRITECOMBINED) ------------------------------
     * A third allocator with its own flags; WRITECOMBINED changes the CPU mapping's
     * cacheability, which RM has to express somewhere. */
    mark("hostreg_hostalloc");
    if (TRY("memhostalloc", cuMemHostAlloc(&hpin, SZ,
                CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_WRITECOMBINED))
            == CUDA_SUCCESS) {
        printf("  cuMemHostAlloc(DEVICEMAP|WC) -> %p\n", hpin); fflush(stdout);
        if (TRY("getdevptr_hostalloc",
                cuMemHostGetDevicePointer_v2(&dp2, hpin, 0)) == CUDA_SUCCESS) {
            printf("  device pointer = 0x%llx\n", (unsigned long long)dp2);
            fflush(stdout);
            if (TRY("memsetD32_wc", cuMemsetD32_v2(dp2, 0x0badf00du, 1024))
                    == CUDA_SUCCESS && TRY("sync2", cuCtxSynchronize()) == CUDA_SUCCESS) {
                unsigned *w = (unsigned *)hpin; int nbad = 0; size_t i;
                for (i = 0; i < 1024; i++) if (w[i] != 0x0badf00du) nbad++;
                printf("  GPU-WROTE-HOSTALLOC words=1024 bad=%d -> %s\n",
                       nbad, nbad ? "MISMATCH" : "PASS");
                fflush(stdout);
                if (nbad) bad = 1;
            }
        }
        TRY("memfreehost2", cuMemFreeHost(hpin));
    }

    TRY("ctxdestroy", cuCtxDestroy(ctx));
    return bad ? stage_fail("hostreg: a device-mapped host buffer did not verify")
               : stage_ok("hostreg register/getdevptr/allochost/hostalloc, GPU wrote host pages");
}

/* ==========================================================================
 * STAGE ipc  (+ the internal `ipcimport` second half)
 *
 * ★ Why a real second process, and what an in-process open would and would not measure.
 *
 * `cuIpcGetMemHandle` on its own is an EXPORT: it asks RM to name an existing allocation
 * so another client can find it. The interesting direction is the IMPORT — a DIFFERENT
 * RM client, with a DIFFERENT VASpace, has to make that foreign physical memory
 * addressable, and "map this existing memory object into my device address space" is
 * exactly NV04_MAP_MEMORY_DMA's job description.
 *
 * ⊘ Opening the handle IN the exporting process does NOT exercise that: CUDA short-
 * circuits it (the allocation is already resident in this context's VAS) and typically
 * answers CUDA_ERROR_INVALID_CONTEXT or hands back the original pointer without any new
 * mapping work. We still do it, because the refusal itself is worth one line of trace —
 * but it is recorded as a NEGATIVE probe and never as the measurement.
 *
 * So the stage forks and exec's ITSELF as `ipcimport <128-hex-chars>`. LD_PRELOAD
 * survives exec, so the child is recorded too; the parent points the child's recorder at
 * `<NVDIFF_OUT>.child` so the two traces stay separable (the shim's sequence numbers are
 * per-process and would otherwise collide in one file — a positional differ cannot
 * survive that).
 * ========================================================================== */
static int run_ipc_import(const char *hex)
{
    CUipcMemHandle ih;
    CUdevice d;
    CUcontext ctx = 0;
    CUdeviceptr dp = 0;
    unsigned v = 0;
    size_t i;

    if (!hex || strlen(hex) != 2 * CU_IPC_HANDLE_SIZE)
        return stage_fail("ipcimport: handle argument is not 128 hex chars");
    memset(&ih, 0, sizeof(ih));
    for (i = 0; i < CU_IPC_HANDLE_SIZE; i++) {
        unsigned byte = 0;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
            return stage_fail("ipcimport: handle argument is not hex");
        ((unsigned char *)&ih)[i] = (unsigned char)byte;
    }
    CK(cuInit(0));
    CK(cuDeviceGet(&d, 0));
    CK(cuCtxCreate(&ctx, 0, d));
    printf("  child pid=%d importing a handle exported by pid=%s\n",
           (int)getpid(), getenv("NVD_IPC_PARENT") ? getenv("NVD_IPC_PARENT") : "?");
    fflush(stdout);

    mark("ipc_open");
    if (TRY("ipcopenmemhandle",
            cuIpcOpenMemHandle_v2(&dp, ih, CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS))
            != CUDA_SUCCESS) {
        TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("ipcimport: cuIpcOpenMemHandle refused — import NOT exercised");
    }
    printf("  IMPORTED dptr=0x%llx in a foreign process\n", (unsigned long long)dp);
    fflush(stdout);

    /* Read the pattern the exporter wrote. This is the only thing that distinguishes
     * "a handle was opened" from "the exporter's physical memory is really addressable
     * from this process's VAS". */
    mark("ipc_use");
    TRY("dtoh_from_imported", cuMemcpyDtoH(&v, dp, 4));
    TRY("sync", cuCtxSynchronize());
    printf("  IMPORT-READ got=0x%08x want=0x%08x -> %s\n",
           v, 0xc0ffee42u, v == 0xc0ffee42u ? "PASS" : "MISMATCH");
    fflush(stdout);

    mark("ipc_close");
    TRY("ipcclosememhandle", cuIpcCloseMemHandle(dp));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    return (v == 0xc0ffee42u)
        ? stage_ok("ipcimport open/read/close of a foreign process's allocation")
        : stage_fail("ipcimport: imported pointer did not carry the exporter's data");
}

static int run_ipc(CUdevice d)
{
    CUcontext ctx = 0;
    CUdeviceptr da = 0, dsame = 0;
    CUipcMemHandle ih;
    char hex[2 * CU_IPC_HANDLE_SIZE + 1];
    char self[256], pidbuf[32], childout[512], childerr[544];
    const char *out;
    size_t i;
    const size_t SZ = 2u << 20;
    unsigned magic = 0xc0ffee42u;
    pid_t pid;
    int wstatus = 0, childrc = -1;

    CK(cuCtxCreate(&ctx, 0, d));
    if (TRY("alloc", cuMemAlloc(&da, SZ)) != CUDA_SUCCESS) {
        cuCtxDestroy(ctx); return stage_fail("ipc: cuMemAlloc refused");
    }
    TRY("htod_magic", cuMemcpyHtoD(da, &magic, 4));
    TRY("sync", cuCtxSynchronize());
    printf("  exporter da=0x%llx size=%zu magic=0x%08x\n",
           (unsigned long long)da, SZ, magic);
    fflush(stdout);

    mark("ipc_get");
    memset(&ih, 0, sizeof(ih));
    if (TRY("ipcgetmemhandle", cuIpcGetMemHandle(&ih, da)) != CUDA_SUCCESS) {
        TRY("free", cuMemFree(da)); TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("ipc: cuIpcGetMemHandle refused — family NOT exercised");
    }
    for (i = 0; i < CU_IPC_HANDLE_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", ((unsigned char *)&ih)[i]);
    hex[2 * CU_IPC_HANDLE_SIZE] = 0;
    printf("  IPC handle = %s\n", hex); fflush(stdout);

    /* ⊘ NEGATIVE PROBE, documented as such: opening our own handle. See the block
     * comment — this is expected to be refused or to short-circuit, and either way it
     * does NOT exercise the cross-VASpace mapping the stage is about. */
    mark("ipc_open_selfprobe");
    printf("  -- negative probe: open our OWN handle in the exporting process --\n");
    fflush(stdout);
    if (TRY("selfopen", cuIpcOpenMemHandle_v2(&dsame, ih,
                CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS)) == CUDA_SUCCESS) {
        printf("  self-open SUCCEEDED dptr=0x%llx (same_as_export=%d) — CUDA "
               "short-circuited it; NOT a cross-process mapping\n",
               (unsigned long long)dsame, dsame == da);
        fflush(stdout);
        TRY("selfclose", cuIpcCloseMemHandle(dsame));
    }

    /* --- the real half: a second process ----------------------------------------- */
    mark("ipc_fork");
    memset(self, 0, sizeof(self));
    if (readlink("/proc/self/exe", self, sizeof(self) - 1) <= 0) {
        TRY("free", cuMemFree(da)); TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("ipc: cannot resolve /proc/self/exe for the importer");
    }
    /* ★ The child's artefacts are named `<stage>_child.jsonl` / `<stage>_child.stdout`,
     * i.e. as if they were a stage of their own, so the census tool finds and grades them
     * with no special case. A `.child` SUFFIX would have made the trace's own name
     * unparsable by the same rule that names every other capture — and a capture the
     * analysis silently skips is indistinguishable from a capture that measured zero. */
    out = getenv("NVDIFF_OUT");
    if (!out) out = "./nvdiff.jsonl";
    {
        size_t n = strlen(out);
        if (n > 6 && !strcmp(out + n - 6, ".jsonl"))
            snprintf(childout, sizeof(childout), "%.*s_child.jsonl", (int)(n - 6), out);
        else
            snprintf(childout, sizeof(childout), "%s_child.jsonl", out);
        n = strlen(childout);
        snprintf(childerr, sizeof(childerr), "%.*s.stdout", (int)(n - 6), childout);
    }
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
    printf("  IPC_CHILD_TRACE=%s\n  IPC_CHILD_STDOUT=%s\n", childout, childerr);
    fflush(stdout);

    pid = fork();
    if (pid == 0) {
        /* ⚠ CUDA state is NOT fork-safe, which is why the child touches nothing and
         * execs immediately: exec discards the inherited driver state entirely. */
        char *args[4];
        setenv("NVDIFF_OUT", childout, 1);
        setenv("NVD_IPC_CHILD_STDOUT", childerr, 1);
        setenv("NVD_IPC_PARENT", pidbuf, 1);
        args[0] = self; args[1] = (char *)"ipcimport"; args[2] = hex; args[3] = NULL;
        execv(self, args);
        _exit(127);                     /* exec failed; parent sees rc=127 */
    }
    if (pid < 0) {
        printf("  fork failed: %s\n", strerror(errno)); fflush(stdout);
    } else {
        waitpid(pid, &wstatus, 0);
        childrc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -WTERMSIG(wstatus);
        printf("  IPC_CHILD rc=%d (⚠ read the child's own OK/FAIL line, not this rc)\n",
               childrc);
        fflush(stdout);
    }

    mark("ipc_teardown");
    TRY("free", cuMemFree(da));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    if (pid < 0 || childrc == 127)
        return stage_fail("ipc: the importing process never ran");
    return stage_ok("ipc get-handle here + open/read/close in a second process "
                    "(child trace is the .child file)");
}

/* ==========================================================================
 * STAGE peer
 *
 * Only meaningful with two GPUs. On a single-GPU box this prints SKIPPED with the
 * device count — ⊘ NOT a zero: "peer mapping issues no 0x57" and "there was no second
 * GPU to map from" must never arrive as the same row in the table.
 * ========================================================================== */
static int run_peer(int ndev)
{
    CUdevice d0, d1;
    CUcontext c0 = 0, c1 = 0;
    CUdeviceptr p0 = 0, p1 = 0;
    int can01 = 0, can10 = 0, bad = 0;
    unsigned magic = 0x9e3779b9u, got = 0;
    char msg[64];

    if (ndev < 2) {
        snprintf(msg, sizeof(msg), "peer: n_devices=%d (needs 2)", ndev);
        return stage_skip(msg);
    }
    CK(cuDeviceGet(&d0, 0));
    CK(cuDeviceGet(&d1, 1));
    CK(cuDeviceCanAccessPeer(&can01, d0, d1));
    CK(cuDeviceCanAccessPeer(&can10, d1, d0));
    printf("  can_access 0->1=%d 1->0=%d\n", can01, can10); fflush(stdout);
    if (!can01) {
        snprintf(msg, sizeof(msg), "peer: n_devices=%d but canAccessPeer(0,1)=0", ndev);
        return stage_skip(msg);
    }

    CK(cuCtxCreate(&c0, 0, d0));
    if (TRY("alloc_dev0", cuMemAlloc(&p0, 1 << 20)) != CUDA_SUCCESS) {
        cuCtxDestroy(c0); return stage_fail("peer: alloc on device 0 refused");
    }
    TRY("htod_dev0", cuMemcpyHtoD(p0, &magic, 4));

    CK(cuCtxCreate(&c1, 0, d1));          /* leaves c1 current */
    if (TRY("alloc_dev1", cuMemAlloc(&p1, 1 << 20)) != CUDA_SUCCESS) bad = 1;

    /* ★ THE MAPPING CALL. cuCtxEnablePeerAccess is the moment device 1's context is
     * given addressability over device 0's memory — the peer analogue of "map this
     * memory object into my device address space". */
    mark("peer_enable");
    if (!bad && TRY("enablepeeraccess", cuCtxEnablePeerAccess(c0, 0)) != CUDA_SUCCESS)
        bad = 1;

    mark("peer_copy");
    if (!bad) {
        TRY("memcpypeer", cuMemcpyPeer(p1, c1, p0, c0, 1 << 20));
        TRY("sync", cuCtxSynchronize());
        TRY("dtoh_dev1", cuMemcpyDtoH(&got, p1, 4));
        printf("  PEER-COPY got=0x%08x want=0x%08x -> %s\n",
               got, magic, got == magic ? "PASS" : "MISMATCH");
        fflush(stdout);
        if (got != magic) bad = 1;
        /* ★ and a DIRECT dereference of the peer VA from device 1's context: this is the
         * access that requires the peer mapping to be in device 1's page tables. */
        TRY("dtod_across_peer", cuMemcpyDtoD(p1, p0, 4096));
        TRY("sync2", cuCtxSynchronize());
    }

    mark("peer_disable");
    TRY("disablepeeraccess", cuCtxDisablePeerAccess(c0));
    if (p1) TRY("free_dev1", cuMemFree(p1));
    TRY("ctxdestroy1", cuCtxDestroy(c1));
    TRY("setcurrent0", cuCtxSetCurrent(c0));
    TRY("free_dev0", cuMemFree(p0));
    TRY("ctxdestroy0", cuCtxDestroy(c0));
    return bad ? stage_fail("peer: the peer copy did not verify")
               : stage_ok("peer canAccess/enable/memcpyPeer/dtod/disable");
}

/* ==========================================================================
 * STAGE mapped — managed (unified) memory beyond plain cuMemAlloc.
 *
 * Managed memory is migrated between host and device by the UVM driver, and the
 * residency hints (cuMemAdvise) plus explicit migration (cuMemPrefetchAsync) are the
 * client-visible controls over where the pages live. Every existing capture reaches
 * nvidia-uvm only through cuCtxCreate's registration path; none of them ever moves a
 * managed page on purpose.
 * ⚠ Read the dlsym comment at resolve_uvm_hints() before changing how these are called.
 * ========================================================================== */
static int run_mapped(CUdevice d)
{
    CUcontext ctx = 0;
    CUdeviceptr um = 0;
    const size_t SZ = 4u << 20;
    int managed = 0, concurrent = 0, bad = 0;
    unsigned *w;
    size_t i;
    int nbad = 0;

    CK(cuDeviceGetAttribute(&managed, CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY, d));
    CK(cuDeviceGetAttribute(&concurrent,
        CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS, d));
    printf("  managed_memory=%d concurrent_managed_access=%d\n", managed, concurrent);
    fflush(stdout);
    if (!managed)
        return stage_skip("mapped: CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY=0");

    CK(cuCtxCreate(&ctx, 0, d));
    resolve_uvm_hints();

    mark("managed_alloc");
    if (TRY("allocmanaged",
            cuMemAllocManaged(&um, SZ, CU_MEM_ATTACH_GLOBAL)) != CUDA_SUCCESS) {
        TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("mapped: cuMemAllocManaged refused — family NOT exercised");
    }
    printf("  managed ptr=0x%llx size=%zu (addressable from BOTH sides)\n",
           (unsigned long long)um, SZ);
    fflush(stdout);

    /* CPU writes first, so the pages start resident on the host and the prefetch below
     * has something real to migrate. A prefetch of never-touched pages can be a no-op. */
    w = (unsigned *)(size_t)um;
    for (i = 0; i < SZ / 4; i++) w[i] = 0x11110000u + (unsigned)i;
    printf("  CPU wrote %zu words; w[0]=0x%08x\n", SZ / 4, w[0]); fflush(stdout);

    mark("managed_advise");
    if (p_memAdvise) {
        TRY("advise_preferred", p_memAdvise(um, SZ, CU_MEM_ADVISE_SET_PREFERRED_LOCATION, d));
        TRY("advise_accessedby", p_memAdvise(um, SZ, CU_MEM_ADVISE_SET_ACCESSED_BY, d));
        TRY("advise_readmostly", p_memAdvise(um, SZ, CU_MEM_ADVISE_SET_READ_MOSTLY, d));
    } else {
        printf("  ⊘ cuMemAdvise NOT RESOLVED — the advise sub-phase is UNMEASURED\n");
        fflush(stdout);
    }

    mark("managed_prefetch_to_gpu");
    if (p_memPrefetch) {
        TRY("prefetch_to_dev", p_memPrefetch(um, SZ, d, 0));
        TRY("sync", cuCtxSynchronize());
    } else {
        printf("  ⊘ cuMemPrefetchAsync NOT RESOLVED — migration is UNMEASURED\n");
        fflush(stdout);
    }

    /* GPU writes a marker through the managed pointer while they are device-resident. */
    mark("managed_gpu_write");
    TRY("memsetD32_managed", cuMemsetD32_v2(um, 0x77770000u, 1024));
    TRY("sync2", cuCtxSynchronize());

    mark("managed_prefetch_to_cpu");
    if (p_memPrefetch) {
        TRY("prefetch_to_host", p_memPrefetch(um, SZ, CU_DEVICE_CPU, 0));
        TRY("sync3", cuCtxSynchronize());
    }

    /* Now read on the CPU. The first 1024 words must carry the GPU's marker and the
     * rest must still carry the CPU's pattern — which proves the pages went to the
     * device and came back, rather than never having left. */
    for (i = 0; i < 1024; i++) if (w[i] != 0x77770000u) nbad++;
    for (i = 1024; i < SZ / 4; i++) if (w[i] != 0x11110000u + (unsigned)i) nbad++;
    printf("  MANAGED-ROUNDTRIP bad=%d w[0]=0x%08x w[2048]=0x%08x -> %s\n",
           nbad, w[0], w[2048], nbad ? "MISMATCH" : "PASS");
    fflush(stdout);
    if (nbad) bad = 1;

    mark("managed_free");
    TRY("free", cuMemFree(um));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    return bad ? stage_fail("mapped: managed pages did not survive the migration round trip")
               : stage_ok("mapped allocManaged/advise x3/prefetch to device and back");
}

/* ==========================================================================
 * STAGE arrays — a DIFFERENT allocator.
 *
 * CUDA arrays are not flat VA out of the same heap `cuMemAlloc` uses: they are surfaces
 * with a format, a channel count and (in RM's terms) a page KIND, and the copy engine
 * reaches them through a 2D descriptor rather than a linear pointer. NVOS46 carries a
 * `kindOverride` field and a `NVOS46_FLAGS_PAGE_KIND_OVERRIDE` flag that exist for
 * exactly this class of surface, which is why the family is worth a stage of its own.
 *
 * ⊘ The texture OBJECT is created but not sampled by default. Sampling needs a PTX
 * `tex` instruction and therefore a JIT, and a JIT failure would make this stage's
 * grade a statement about PTX rather than about the mapping path. Set NVD_TEX_SAMPLE=1
 * to attempt it; its outcome is printed and never folded into the stage grade.
 * ========================================================================== */
static const char *k_texptx =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry texrd(.param .u64 t0, .param .u64 o0)\n"
"{\n"
" .reg .b32 %r<8>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1, [t0];\n"
" ld.param.u64 %rd2, [o0];\n"
" cvta.to.global.u64 %rd3, %rd2;\n"
" mov.u32 %r1, 0;\n"
" tex.1d.v4.u32.s32 {%r2,%r3,%r4,%r5}, [%rd1, {%r1}];\n"
" st.global.u32 [%rd3], %r2;\n"
" ret;\n"
"}\n";

static int run_arrays(CUdevice d)
{
    CUcontext ctx = 0;
    CUarray arr = 0, lvl0 = 0;
    CUmipmappedArray mip = 0;
    CUtexObject tex_arr = 0, tex_lin = 0;
    CUDA_ARRAY_DESCRIPTOR ad;
    CUDA_ARRAY3D_DESCRIPTOR a3;
    CUDA_MEMCPY2D cp;
    CUDA_RESOURCE_DESC rd;
    CUDA_TEXTURE_DESC td;
    CUdeviceptr lin = 0;
    const unsigned W = 256, H = 256;
    unsigned *src = NULL, *dst = NULL;
    size_t i, bytes = (size_t)W * H * 4;
    int bad = 0, nbad = 0;

    CK(cuCtxCreate(&ctx, 0, d));
    src = (unsigned *)malloc(bytes);
    dst = (unsigned *)malloc(bytes);
    if (!src || !dst) { cuCtxDestroy(ctx); return stage_fail("arrays: malloc failed"); }
    for (i = 0; i < (size_t)W * H; i++) src[i] = 0x30000000u + (unsigned)i;
    memset(dst, 0, bytes);

    /* --- (1) a plain 2D array + a real HtoA / AtoH round trip -------------------- */
    mark("array_create");
    memset(&ad, 0, sizeof(ad));
    ad.Width = W; ad.Height = H;
    ad.Format = CU_AD_FORMAT_UNSIGNED_INT32; ad.NumChannels = 1;
    if (TRY("arraycreate", cuArrayCreate_v2(&arr, &ad)) != CUDA_SUCCESS) {
        free(src); free(dst); TRY("ctxdestroy", cuCtxDestroy(ctx));
        return stage_fail("arrays: cuArrayCreate refused — family NOT exercised");
    }
    printf("  array=%p %ux%u fmt=U32 ch=1 bytes=%zu\n", (void *)arr, W, H, bytes);
    fflush(stdout);

    mark("array_copy");
    memset(&cp, 0, sizeof(cp));
    cp.srcMemoryType = CU_MEMORYTYPE_HOST; cp.srcHost = src; cp.srcPitch = W * 4;
    cp.dstMemoryType = CU_MEMORYTYPE_ARRAY; cp.dstArray = arr;
    cp.WidthInBytes = W * 4; cp.Height = H;
    if (TRY("memcpy2d_HtoA", cuMemcpy2D_v2(&cp)) != CUDA_SUCCESS) bad = 1;
    memset(&cp, 0, sizeof(cp));
    cp.srcMemoryType = CU_MEMORYTYPE_ARRAY; cp.srcArray = arr;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST; cp.dstHost = dst; cp.dstPitch = W * 4;
    cp.WidthInBytes = W * 4; cp.Height = H;
    if (TRY("memcpy2d_AtoH", cuMemcpy2D_v2(&cp)) != CUDA_SUCCESS) bad = 1;
    TRY("sync", cuCtxSynchronize());
    for (i = 0; i < (size_t)W * H; i++)
        if (dst[i] != 0x30000000u + (unsigned)i) nbad++;
    printf("  ARRAY-ROUNDTRIP words=%zu bad=%d dst[0]=0x%08x dst[last]=0x%08x -> %s\n",
           (size_t)W * H, nbad, dst[0], dst[(size_t)W * H - 1],
           nbad ? "MISMATCH" : "PASS");
    fflush(stdout);
    if (nbad) bad = 1;

    /* --- (2) a mipmapped array: several levels behind one object ----------------- */
    mark("mipmap_create");
    memset(&a3, 0, sizeof(a3));
    a3.Width = W; a3.Height = H; a3.Depth = 0;
    a3.Format = CU_AD_FORMAT_UNSIGNED_INT32; a3.NumChannels = 1; a3.Flags = 0;
    if (TRY("mipmappedarraycreate",
            cuMipmappedArrayCreate(&mip, &a3, 4)) == CUDA_SUCCESS) {
        printf("  mipmapped array=%p levels=4\n", (void *)mip); fflush(stdout);
        if (TRY("mipmapgetlevel",
                cuMipmappedArrayGetLevel(&lvl0, mip, 0)) == CUDA_SUCCESS) {
            printf("  level0 array=%p\n", (void *)lvl0); fflush(stdout);
            memset(&cp, 0, sizeof(cp));
            cp.srcMemoryType = CU_MEMORYTYPE_HOST; cp.srcHost = src; cp.srcPitch = W * 4;
            cp.dstMemoryType = CU_MEMORYTYPE_ARRAY; cp.dstArray = lvl0;
            cp.WidthInBytes = W * 4; cp.Height = H;
            TRY("memcpy2d_HtoMip0", cuMemcpy2D_v2(&cp));
            TRY("sync_mip", cuCtxSynchronize());
        }
    } else {
        printf("  mipmap sub-phase UNMEASURED (create refused)\n"); fflush(stdout);
    }

    /* --- (3) texture objects, over the array AND over linear device memory ------- */
    mark("texobj_create");
    memset(&td, 0, sizeof(td));
    td.addressMode[0] = CU_TR_ADDRESS_MODE_CLAMP;
    td.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
    td.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
    td.filterMode = CU_TR_FILTER_MODE_POINT;
    td.flags = CU_TRSF_READ_AS_INTEGER;

    memset(&rd, 0, sizeof(rd));
    rd.resType = CU_RESOURCE_TYPE_ARRAY;
    rd.res.array.hArray = arr;
    if (TRY("texobjcreate_array",
            cuTexObjectCreate(&tex_arr, &rd, &td, NULL)) == CUDA_SUCCESS) {
        printf("  texobj(array) = 0x%llx\n", (unsigned long long)tex_arr);
        fflush(stdout);
    }

    if (TRY("alloc_linear", cuMemAlloc(&lin, bytes)) == CUDA_SUCCESS) {
        TRY("htod_linear", cuMemcpyHtoD(lin, src, bytes));
        memset(&rd, 0, sizeof(rd));
        rd.resType = CU_RESOURCE_TYPE_LINEAR;
        rd.res.linear.devPtr = lin;
        rd.res.linear.format = CU_AD_FORMAT_UNSIGNED_INT32;
        rd.res.linear.numChannels = 1;
        rd.res.linear.sizeInBytes = bytes;
        if (TRY("texobjcreate_linear",
                cuTexObjectCreate(&tex_lin, &rd, &td, NULL)) == CUDA_SUCCESS) {
            printf("  texobj(linear over 0x%llx) = 0x%llx\n",
                   (unsigned long long)lin, (unsigned long long)tex_lin);
            fflush(stdout);
        }
    }

    /* optional: actually SAMPLE the linear texture. Graded separately on purpose. */
    if (tex_lin && getenv("NVD_TEX_SAMPLE")) {
        CUmodule mod = 0; CUfunction fn = 0; CUdeviceptr outp = 0;
        unsigned got = 0; void *args[2];
        mark("texobj_sample");
        if (TRY("moduleload_tex", cuModuleLoadData(&mod, k_texptx)) == CUDA_SUCCESS &&
            TRY("getfunction_tex", cuModuleGetFunction(&fn, mod, "texrd")) == CUDA_SUCCESS &&
            TRY("alloc_out", cuMemAlloc(&outp, 4)) == CUDA_SUCCESS) {
            args[0] = &tex_lin; args[1] = &outp;
            TRY("launch_tex", cuLaunchKernel(fn, 1, 1, 1, 1, 1, 1, 0, 0, args, 0));
            TRY("sync_tex", cuCtxSynchronize());
            TRY("dtoh_tex", cuMemcpyDtoH(&got, outp, 4));
            printf("  TEXSAMPLE got=0x%08x want=0x%08x -> %s (NOT part of the stage grade)\n",
                   got, 0x30000000u, got == 0x30000000u ? "PASS" : "MISMATCH");
            fflush(stdout);
            TRY("free_out", cuMemFree(outp));
        } else {
            printf("  TEXSAMPLE unavailable (JIT/module refused) — not graded\n");
            fflush(stdout);
        }
    }

    mark("arrays_teardown");
    if (tex_lin) TRY("texobjdestroy_linear", cuTexObjectDestroy(tex_lin));
    if (tex_arr) TRY("texobjdestroy_array", cuTexObjectDestroy(tex_arr));
    if (lin)     TRY("free_linear", cuMemFree(lin));
    if (mip)     TRY("mipmappedarraydestroy", cuMipmappedArrayDestroy(mip));
    TRY("arraydestroy", cuArrayDestroy(arr));
    TRY("ctxdestroy", cuCtxDestroy(ctx));
    free(src); free(dst);
    return bad ? stage_fail("arrays: the HtoA/AtoH round trip did not verify")
               : stage_ok("arrays create/2D-copy-both-ways/mipmap/texobj array+linear");
}

/* ------------------------------------------------------------------ dispatch */
enum { S_VMM, S_HOSTREG, S_IPC, S_IPCIMPORT, S_PEER, S_MAPPED, S_ARRAYS, S_BASE };

static const struct { const char *name; int id; } STAGES[] = {
    { "vmm",       S_VMM },
    { "hostreg",   S_HOSTREG },
    { "ipc",       S_IPC },
    { "ipcimport", S_IPCIMPORT },   /* internal: the second half of `ipc` */
    { "peer",      S_PEER },
    { "mapped",    S_MAPPED },
    { "arrays",    S_ARRAYS },
    { "base",      S_BASE },
};

static int stage_of(const char *s)
{
    size_t i;
    if (!s) return S_BASE;          /* the CONTROL is the safe default */
    for (i = 0; i < sizeof(STAGES) / sizeof(STAGES[0]); i++)
        if (!strcmp(s, STAGES[i].name)) return STAGES[i].id;
    fprintf(stderr, "unknown stage '%s'; known:", s);
    for (i = 0; i < sizeof(STAGES) / sizeof(STAGES[0]); i++)
        fprintf(stderr, " %s", STAGES[i].name);
    fprintf(stderr, "\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *sname = argc > 1 ? argv[1] : "base";
    int stage = stage_of(argc > 1 ? argv[1] : NULL);
    CUdevice d;
    int n = 0, maj = 0, min = 0;
    char nm[256];
    size_t tot = 0;

    /* ⚠ The IPC importer gets its OWN stdout file before it prints anything. Sharing the
     * parent's fd 1 would put TWO `OK ...` verdict lines in one transcript, and any rule
     * for picking "the" verdict out of that file (first? last?) would be a guess that
     * silently mis-grades one of the two processes. */
    if (stage == S_IPCIMPORT) {
        const char *co = getenv("NVD_IPC_CHILD_STDOUT");
        if (co && !freopen(co, "w", stdout))
            fprintf(stderr, "WARN child could not redirect stdout to %s\n", co);
    }

    /* STAGE first, always, before any CUDA call: a capture whose stdout does not open
     * with this line was truncated or never started, and that must be visible. */
    printf("STAGE %s\n", sname);
    printf("pid=%d nvdiff_out=%s\n", (int)getpid(),
           getenv("NVDIFF_OUT") ? getenv("NVDIFF_OUT") : "(unset -> ./nvdiff.jsonl)");
    fflush(stdout);

    g_markfd = open("/dev/nvidiactl", O_RDWR);
    printf("MARKFD %d%s\n", g_markfd,
           g_markfd < 0 ? "  ⊘ NO MARKERS — the trace must NOT be phase-split" : "");
    fflush(stdout);
    mark("stage_begin");

    if (stage == S_IPCIMPORT)
        return run_ipc_import(argc > 2 ? argv[2] : NULL);

    CK(cuInit(0));
    CK(cuDeviceGetCount(&n));
    printf("devices=%d\n", n); fflush(stdout);
    if (n < 1)
        return stage_skip("no CUDA devices");
    CK(cuDeviceGet(&d, 0));
    memset(nm, 0, sizeof(nm));
    CK(cuDeviceGetName(nm, sizeof(nm), d));
    CK(cuDeviceGetAttribute(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d));
    CK(cuDeviceGetAttribute(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d));
    CK(cuDeviceTotalMem(&tot, d));
    printf("name=%s compute=%d.%d totalMem=%zu MiB\n", nm, maj, min, (size_t)(tot >> 20));
    fflush(stdout);

    mark("stage_body");
    switch (stage) {
    case S_VMM:     return run_vmm(d);
    case S_HOSTREG: return run_hostreg(d);
    case S_IPC:     return run_ipc(d);
    case S_PEER:    return run_peer(n);
    case S_MAPPED:  return run_mapped(d);
    case S_ARRAYS:  return run_arrays(d);
    case S_BASE:    return run_base(d);
    }
    return stage_fail("unreachable stage dispatch");
}
