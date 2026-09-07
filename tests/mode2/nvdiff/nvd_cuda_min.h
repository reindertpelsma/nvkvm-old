/*
 * nvd_cuda_min.h — a MINIMAL stand-in for <cuda.h>, for hosts that have libcuda
 * but no CUDA toolkit.
 *
 * ## Why this exists
 *
 * `nvd_prog.c` needs `cuda.h` only for prototypes and two enum values. The bench box
 * `vh` ships `libcuda.so.580.159.04` and **no toolkit**, and the only `cuda.h` on the
 * filesystem is the PowerMac ADB driver header — the exact decoy
 * `cup2_hook_deadline.sh` warns about ("3 of 5 hits in this guest are the PowerMac ADB").
 * Without this file the differential cannot be captured on the box whose GPU and driver
 * actually match the guest's.
 *
 * ## ⚠⚠ THE ONLY THING THAT CAN GO WRONG HERE, AND IT IS SILENT
 *
 * Real `cuda.h` does not declare `cuCtxCreate`; it declares `cuCtxCreate_v2` and
 * `#define`s the short name onto it. **Seven of the entry points this workload uses are
 * versioned that way.** A hand-written header that declares the SHORT names binds the
 * v1 symbols, which are different functions with different ABIs — the program still
 * builds, still links, still runs, and emits a DIFFERENT ioctl stream. A differential
 * built on that is comparing two things neither of which is what CUDA does.
 *
 * ⇒ The `#define`s below are the load-bearing half of this file, not the prototypes.
 * ⇒ And they are not trusted: `nvd_capture.sh` greps the built binary's dynamic
 *   relocations and REFUSES to run unless every one of the seven `_v2` names is bound.
 *   A header cannot check itself; the linker's output can.
 *
 * Versioning read off the CUDA driver API (unchanged since CUDA 3.2):
 *   cuDeviceTotalMem, cuCtxCreate, cuCtxDestroy, cuMemAlloc, cuMemFree,
 *   cuMemcpyHtoD, cuMemcpyDtoH   -> _v2
 *   cuInit, cuGetErrorString, cuDeviceGetCount, cuDeviceGet, cuDeviceGetName,
 *   cuDeviceGetAttribute, cuModuleLoadData, cuModuleGetFunction, cuLaunchKernel,
 *   cuCtxSynchronize             -> unversioned
 */
#ifndef NVD_CUDA_MIN_H
#define NVD_CUDA_MIN_H

#include <stddef.h>

typedef int                 CUresult;
typedef int                 CUdevice;
typedef unsigned long long  CUdeviceptr;
typedef struct CUctx_st    *CUcontext;
typedef struct CUmod_st    *CUmodule;
typedef struct CUfunc_st   *CUfunction;
typedef struct CUstream_st *CUstream;

#define CUDA_SUCCESS 0
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76
/* ★ Capability attributes the wide-API stages GATE on (nvd_apis.c). A stage that runs
 * an API the device does not support measures the refusal, not the API — so each of
 * these is queried and printed before its family is exercised, and the stage prints
 * SKIPPED rather than a zero. Numbers from CUdevice_attribute in cuda.h; they are ABI
 * and have never been renumbered. */
#define CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY 19
#define CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING 41
#define CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY 83
#define CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS 89
#define CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED 102
#define CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED 103

/* ★ the versioned seven — see the header comment; getting these wrong is silent */
#define cuDeviceTotalMem cuDeviceTotalMem_v2
#define cuCtxCreate      cuCtxCreate_v2
#define cuCtxDestroy     cuCtxDestroy_v2
#define cuMemAlloc       cuMemAlloc_v2
#define cuMemFree        cuMemFree_v2
#define cuMemcpyHtoD     cuMemcpyHtoD_v2
#define cuMemcpyDtoH     cuMemcpyDtoH_v2
/* ★ an EIGHTH, added for the fault stages: cuMemcpyDtoD is versioned too. The other
 * fault-stage entry points (cuCtxSetCurrent, cuMemAddressReserve/Free) are NOT — they
 * postdate the CUDA 3.2 renaming and carry no _v2 alias. Checked against libcuda's
 * dynamic symbol table on the bench, not from memory. */
#define cuMemcpyDtoD     cuMemcpyDtoD_v2

#ifdef __cplusplus
extern "C" {
#endif

CUresult cuInit(unsigned int);
CUresult cuGetErrorString(CUresult, const char **);
CUresult cuDeviceGetCount(int *);
CUresult cuDeviceGet(CUdevice *, int);
CUresult cuDeviceGetName(char *, int, CUdevice);
CUresult cuDeviceGetAttribute(int *, int, CUdevice);
CUresult cuDeviceTotalMem(size_t *, CUdevice);
CUresult cuCtxCreate(CUcontext *, unsigned int, CUdevice);
CUresult cuCtxDestroy(CUcontext);
CUresult cuCtxSynchronize(void);
CUresult cuMemAlloc(CUdeviceptr *, size_t);
CUresult cuMemFree(CUdeviceptr);
CUresult cuMemcpyHtoD(CUdeviceptr, const void *, size_t);
CUresult cuMemcpyDtoH(void *, CUdeviceptr, size_t);
CUresult cuModuleLoadData(CUmodule *, const void *);
CUresult cuModuleGetFunction(CUfunction *, CUmodule, const char *);
CUresult cuLaunchKernel(CUfunction, unsigned, unsigned, unsigned,
                        unsigned, unsigned, unsigned, unsigned,
                        CUstream, void **, void **);
/* fault stages */
CUresult cuMemcpyDtoD(CUdeviceptr, CUdeviceptr, size_t);
CUresult cuCtxSetCurrent(CUcontext);
CUresult cuMemAddressReserve(CUdeviceptr *, size_t, size_t, CUdeviceptr,
                             unsigned long long);
CUresult cuMemAddressFree(CUdeviceptr, size_t);

/* ★ The virtual-memory-management (VMM) API. It is the ONLY way found to hand CUDA a
 * pointer it accepts as a device allocation while the GPU page tables hold no mapping
 * for it — i.e. an "invalid CE address" that survives libcuda's own bounds check.
 * ⚠ These structs are ABI, and getting them wrong is the silent-failure shape this
 * header already exists to warn about. Layout is the CUDA 11+ definition; every call's
 * return code is printed by the caller, so a mismatch shows up as a refusal at
 * cuMemCreate rather than as a wrong measurement. */
typedef unsigned long long CUmemGenericAllocationHandle;
#define CU_MEM_ALLOCATION_TYPE_PINNED      0x1
#define CU_MEM_HANDLE_TYPE_NONE            0x0
#define CU_MEM_LOCATION_TYPE_DEVICE        0x1
#define CU_MEM_ACCESS_FLAGS_PROT_READWRITE 0x3
#define CU_MEM_ALLOC_GRANULARITY_MINIMUM   0x0
#define CU_MEM_ALLOC_GRANULARITY_RECOMMENDED 0x1
/* ★ added 2026-09-07 for nvd_apis.c's `vmm` stage: an EXPORTABLE VMM handle takes a
 * different RM route (the allocation is dup'd through an fd) than an unexported one. */
#define CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR 0x1
#define CU_MEM_ACCESS_FLAGS_PROT_NONE      0x0
#define CU_MEM_ACCESS_FLAGS_PROT_READ      0x1

typedef struct CUmemLocation_st { int type; int id; } CUmemLocation;
typedef struct CUmemAllocationProp_st {
    int          type;                  /* CUmemAllocationType       */
    int          requestedHandleTypes;  /* CUmemAllocationHandleType */
    CUmemLocation location;
    void        *win32HandleMetaData;
    struct {
        unsigned char  compressionType;
        unsigned char  gpuDirectRDMACapable;
        unsigned short usage;
        unsigned char  reserved[4];
    } allocFlags;
} CUmemAllocationProp;
typedef struct CUmemAccessDesc_st {
    CUmemLocation location;
    int           flags;                /* CUmemAccess_flags */
} CUmemAccessDesc;

CUresult cuMemGetAllocationGranularity(size_t *, const CUmemAllocationProp *, int);
CUresult cuMemCreate(CUmemGenericAllocationHandle *, size_t,
                     const CUmemAllocationProp *, unsigned long long);
CUresult cuMemMap(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle,
                  unsigned long long);
CUresult cuMemSetAccess(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
CUresult cuMemUnmap(CUdeviceptr, size_t);
CUresult cuMemRelease(CUmemGenericAllocationHandle);
CUresult cuMemExportToShareableHandle(void *, CUmemGenericAllocationHandle, int,
                                      unsigned long long);
CUresult cuMemImportFromShareableHandle(CUmemGenericAllocationHandle *, void *, int);

/* =====================================================================================
 * ★★★ THE WIDE-API SURFACE — added 2026-09-07 for `nvd_apis.c`.
 *
 * ## Why any of this is here
 *
 * `NV_ESC_RM_MAP_MEMORY_DMA` (ioctl nr 0x57) appears ZERO times across all twelve
 * committed captures in `traces/host_reference_ga106/`. That is an ABSENCE, and this
 * tree has paid repeatedly for reading an absence as a fact: those traces run only
 * `nvd_prog.c`, whose entire memory vocabulary is `cuMemAlloc` + one CE copy. RM serves
 * the escape in full and it carries a documented client-facing flag set (NVOS46_FLAGS_*
 * above the struct at nvos.h:2166), so the honest reading of the zero is "we never
 * exercised the API family that would issue it", not "CUDA never issues it".
 * ⇒ These declarations exist so ONE program can walk the whole client-facing memory
 * surface — VMM, host-registered sysmem, IPC, peer, managed, arrays/textures — and let
 * a record COUNT settle the question.
 *
 * ## ⚠⚠ THE SPELLING RULE FOR EVERYTHING BELOW, AND IT DIFFERS FROM THE BLOCK ABOVE
 *
 * The file's original entries carry `#define cuMemAlloc cuMemAlloc_v2` so callers may
 * write the short name. That works, but it makes the stand-in and real `cuda.h` agree
 * only as long as BOTH carry the same alias — the exact silent-divergence this header's
 * top comment warns about, and the list of aliases has grown twice since (CUDA 11.1
 * versioned `cuIpcOpenMemHandle`; CUDA 12.2 versioned `cuMemAdvise` and
 * `cuMemPrefetchAsync` with a DIFFERENT argument list).
 * ⇒ Everything added below is declared under its FULL, EXPLICIT symbol name and must be
 *   CALLED that way. Real `cuda.h` declares its prototypes through the same macro
 *   expansion, so `cuArrayCreate_v2(...)` compiles against both headers and binds one
 *   unambiguous symbol; and if a name is wrong the LINK fails, which is a loud failure
 *   rather than a wrong measurement. No new `#define` aliases are added here on purpose.
 * ⊘ Two entry points are deliberately NOT declared here — `cuMemAdvise` and
 *   `cuMemPrefetchAsync`. CUDA 12.2 gave both a `_v2` with an incompatible signature
 *   (`CUmemLocation` in place of `CUdevice`), so which ABI a source-level call means
 *   depends on the toolkit's version, i.e. on the build box. `nvd_apis.c` resolves those
 *   two with `dlsym()` on the BARE names, which is the v1 ABI on every driver that has
 *   ever shipped them. See the comment at their use site.
 * ===================================================================================== */

/* --- host (sysmem) memory: register / page-locked alloc ------------------------------
 * ★ RANK-2 CANDIDATE for 0x57. `CU_MEMHOSTREGISTER_DEVICEMAP` asks RM to place ordinary
 * process pages into the GPU's virtual address space — which is literally what
 * NV04_MAP_MEMORY_DMA is for ("map memory into a device's DMA address space"), as
 * opposed to NV04_MAP_MEMORY (0x4E) which maps into the CPU's. */
#define CU_MEMHOSTREGISTER_PORTABLE   0x01
#define CU_MEMHOSTREGISTER_DEVICEMAP  0x02
#define CU_MEMHOSTREGISTER_IOMEMORY   0x04
#define CU_MEMHOSTREGISTER_READ_ONLY  0x08
#define CU_MEMHOSTALLOC_PORTABLE      0x01
#define CU_MEMHOSTALLOC_DEVICEMAP     0x02
#define CU_MEMHOSTALLOC_WRITECOMBINED 0x04
CUresult cuMemHostRegister_v2(void *, size_t, unsigned int);
CUresult cuMemHostUnregister(void *);
CUresult cuMemHostGetDevicePointer_v2(CUdeviceptr *, void *, unsigned int);
CUresult cuMemHostGetFlags(unsigned int *, void *);
CUresult cuMemAllocHost_v2(void **, size_t);
CUresult cuMemHostAlloc(void **, size_t, unsigned int);
CUresult cuMemFreeHost(void *);

/* --- IPC ----------------------------------------------------------------------------
 * CU_IPC_HANDLE_SIZE is 64 and the handle is passed BY VALUE, so its size is ABI. */
#define CU_IPC_HANDLE_SIZE 64
typedef struct CUipcMemHandle_st { char reserved[CU_IPC_HANDLE_SIZE]; } CUipcMemHandle;
#define CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS 0x1
CUresult cuIpcGetMemHandle(CUipcMemHandle *, CUdeviceptr);
CUresult cuIpcOpenMemHandle_v2(CUdeviceptr *, CUipcMemHandle, unsigned int);
CUresult cuIpcCloseMemHandle(CUdeviceptr);

/* --- peer access --------------------------------------------------------------------- */
CUresult cuDeviceCanAccessPeer(int *, CUdevice, CUdevice);
CUresult cuCtxEnablePeerAccess(CUcontext, unsigned int);
CUresult cuCtxDisablePeerAccess(CUcontext);
CUresult cuMemcpyPeer(CUdeviceptr, CUcontext, CUdeviceptr, CUcontext, size_t);

/* --- unified / managed memory --------------------------------------------------------
 * ⊘ cuMemAdvise / cuMemPrefetchAsync are NOT declared — see the spelling rule above. */
#define CU_MEM_ATTACH_GLOBAL 0x1
#define CU_MEM_ATTACH_HOST   0x2
#define CU_DEVICE_CPU        ((CUdevice)-1)
#define CU_MEM_ADVISE_SET_READ_MOSTLY          1
#define CU_MEM_ADVISE_SET_PREFERRED_LOCATION   3
#define CU_MEM_ADVISE_SET_ACCESSED_BY          5
CUresult cuMemAllocManaged(CUdeviceptr *, size_t, unsigned int);

/* --- arrays, mipmapped arrays, texture objects ---------------------------------------
 * A different allocator (RM's "surface" path, CU_AD_FORMAT_* / block-linear kinds)
 * rather than the flat-VA heap `cuMemAlloc` uses, so plausibly a different mapping path.
 * ⚠ These are ABI structs. Layouts are the CUDA 11+/12.x definitions; every call's
 * return code is printed by the caller, so a mismatch surfaces as a refusal at
 * cuArrayCreate/cuTexObjectCreate rather than as a quietly wrong measurement. */
typedef struct CUarray_st          *CUarray;
typedef struct CUmipmappedArray_st *CUmipmappedArray;
typedef unsigned long long          CUtexObject;

#define CU_AD_FORMAT_UNSIGNED_INT8  0x01
#define CU_AD_FORMAT_UNSIGNED_INT16 0x02
#define CU_AD_FORMAT_UNSIGNED_INT32 0x03
#define CU_AD_FORMAT_FLOAT          0x20

#define CU_MEMORYTYPE_HOST    0x01
#define CU_MEMORYTYPE_DEVICE  0x02
#define CU_MEMORYTYPE_ARRAY   0x03
#define CU_MEMORYTYPE_UNIFIED 0x04

#define CU_RESOURCE_TYPE_ARRAY           0
#define CU_RESOURCE_TYPE_MIPMAPPED_ARRAY 1
#define CU_RESOURCE_TYPE_LINEAR          2
#define CU_RESOURCE_TYPE_PITCH2D         3

#define CU_TR_ADDRESS_MODE_CLAMP  1
#define CU_TR_FILTER_MODE_POINT   0
#define CU_TRSF_READ_AS_INTEGER   0x01

typedef struct CUDA_ARRAY_DESCRIPTOR_st {
    size_t       Width;
    size_t       Height;
    unsigned int Format;        /* CUarray_format  */
    unsigned int NumChannels;
} CUDA_ARRAY_DESCRIPTOR;

typedef struct CUDA_ARRAY3D_DESCRIPTOR_st {
    size_t       Width;
    size_t       Height;
    size_t       Depth;
    unsigned int Format;        /* CUarray_format  */
    unsigned int NumChannels;
    unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;

typedef struct CUDA_MEMCPY2D_st {
    size_t       srcXInBytes, srcY;
    unsigned int srcMemoryType; /* CUmemorytype */
    const void  *srcHost;
    CUdeviceptr  srcDevice;
    CUarray      srcArray;
    size_t       srcPitch;
    size_t       dstXInBytes, dstY;
    unsigned int dstMemoryType; /* CUmemorytype */
    void        *dstHost;
    CUdeviceptr  dstDevice;
    CUarray      dstArray;
    size_t       dstPitch;
    size_t       WidthInBytes;
    size_t       Height;
} CUDA_MEMCPY2D;

typedef struct CUDA_RESOURCE_DESC_st {
    unsigned int resType;       /* CUresourcetype */
    union {
        struct { CUarray hArray; } array;
        struct { CUmipmappedArray hMipmappedArray; } mipmap;
        struct { CUdeviceptr devPtr; unsigned int format; unsigned int numChannels;
                 size_t sizeInBytes; } linear;
        struct { CUdeviceptr devPtr; unsigned int format; unsigned int numChannels;
                 size_t width; size_t height; size_t pitchInBytes; } pitch2D;
        struct { int reserved[32]; } reserved;
    } res;
    unsigned int flags;
} CUDA_RESOURCE_DESC;

typedef struct CUDA_TEXTURE_DESC_st {
    unsigned int addressMode[3];   /* CUaddress_mode */
    unsigned int filterMode;       /* CUfilter_mode  */
    unsigned int flags;
    unsigned int maxAnisotropy;
    unsigned int mipmapFilterMode; /* CUfilter_mode  */
    float        mipmapLevelBias;
    float        minMipmapLevelClamp;
    float        maxMipmapLevelClamp;
    float        borderColor[4];
    int          reserved[12];
} CUDA_TEXTURE_DESC;

CUresult cuArrayCreate_v2(CUarray *, const CUDA_ARRAY_DESCRIPTOR *);
CUresult cuArray3DCreate_v2(CUarray *, const CUDA_ARRAY3D_DESCRIPTOR *);
CUresult cuArrayDestroy(CUarray);
CUresult cuMipmappedArrayCreate(CUmipmappedArray *, const CUDA_ARRAY3D_DESCRIPTOR *,
                                unsigned int);
CUresult cuMipmappedArrayGetLevel(CUarray *, CUmipmappedArray, unsigned int);
CUresult cuMipmappedArrayDestroy(CUmipmappedArray);
CUresult cuTexObjectCreate(CUtexObject *, const CUDA_RESOURCE_DESC *,
                           const CUDA_TEXTURE_DESC *, const void * /*CUDA_RESOURCE_VIEW_DESC*/);
CUresult cuTexObjectDestroy(CUtexObject);
CUresult cuMemcpy2D_v2(const CUDA_MEMCPY2D *);

/* --- odds and ends the stages need to make their work OBSERVABLE ---------------------
 * ⚠ Each of these exists so a stage can print a value that distinguishes "this API ran
 * and issued no 0x57" from "this API silently did nothing". They are not decoration. */
CUresult cuMemsetD32_v2(CUdeviceptr, unsigned int, size_t);
CUresult cuMemGetAddressRange_v2(CUdeviceptr *, size_t *, CUdeviceptr);
CUresult cuMemGetInfo_v2(size_t *, size_t *);
CUresult cuPointerGetAttribute(void *, int, CUdeviceptr);
CUresult cuCtxGetDevice(CUdevice *);
CUresult cuStreamSynchronize(CUstream);
#define CU_POINTER_ATTRIBUTE_MEMORY_TYPE 2
#define CU_POINTER_ATTRIBUTE_DEVICE_POINTER 3

#ifdef __cplusplus
}
#endif
#endif /* NVD_CUDA_MIN_H */
