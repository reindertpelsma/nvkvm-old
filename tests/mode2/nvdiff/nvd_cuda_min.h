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

/* ★ the versioned seven — see the header comment; getting these wrong is silent */
#define cuDeviceTotalMem cuDeviceTotalMem_v2
#define cuCtxCreate      cuCtxCreate_v2
#define cuCtxDestroy     cuCtxDestroy_v2
#define cuMemAlloc       cuMemAlloc_v2
#define cuMemFree        cuMemFree_v2
#define cuMemcpyHtoD     cuMemcpyHtoD_v2
#define cuMemcpyDtoH     cuMemcpyDtoH_v2

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

#ifdef __cplusplus
}
#endif
#endif /* NVD_CUDA_MIN_H */
