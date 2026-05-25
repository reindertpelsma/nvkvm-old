/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cuinit_test.c — minimal cuInit + cuDeviceGetCount regression test.
 *
 * Build inside the guest after libcuda.so.<host-version> is installed:
 *   gcc -O0 -g -o cuinit_test cuinit_test.c -ldl
 *
 * Pass criteria (a "passing" cuInit means the UVM/RM stack reached a working
 * state in the isolate):
 *   - cuInit(0) returns CUDA_SUCCESS (0)
 *   - cuDeviceGetCount returns 0 and a positive device count
 *
 * Regression bisect notes (see uvm_via_isolate_needed memory file):
 *   - Before UVM-via-isolate: cuInit failed 999 because UVM_PAGEABLE_MEM_ACCESS
 *     hit EBADF after a QEMU-side UVM_MM_INITIALIZE that left the driver
 *     state incoherent.
 *   - After UVM-via-isolate: UVM ioctls run in the isolate process; cuInit
 *     reaches device enumeration but still has known mmap/handle gaps before
 *     a device can be successfully detected.
 *
 * Exit codes:
 *   0  — cuInit OK and device count > 0  (full pass)
 *   2  — cuInit OK but device count == 0 (partial — known regression point)
 *   3  — cuInit failed with a known-blocker error code (track in CI)
 *   1  — cuInit failed unexpectedly OR dlopen failure (real regression)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* Known-blocker error codes. As fixes land, remove entries — any code not in
 * this list is treated as a regression and the test exits with 1. */
#define EXPECTED_BLOCKERS_MAX 4
static int expected_blockers[EXPECTED_BLOCKERS_MAX] = {
    100, /* CUDA_ERROR_NO_DEVICE       — UVM/mmap reaches end of probe         */
    999, /* CUDA_ERROR_UNKNOWN         — intermittent UVM_PAGEABLE_MEM EBADF   */
    0, 0,
};

static int is_expected_blocker(int rc)
{
    for (int i = 0; i < EXPECTED_BLOCKERS_MAX; i++)
        if (expected_blockers[i] != 0 && expected_blockers[i] == rc) return 1;
    return 0;
}

int main(void)
{
    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    int (*cuInit)(unsigned int)              = dlsym(h, "cuInit");
    int (*cuDriverGetVersion)(int*)          = dlsym(h, "cuDriverGetVersion");
    int (*cuDeviceGetCount)(int*)            = dlsym(h, "cuDeviceGetCount");
    int (*cuGetErrorString)(int, const char**) = dlsym(h, "cuGetErrorString");

    if (!cuInit || !cuDeviceGetCount) {
        printf("dlsym failed\n");
        return 1;
    }

    int ver = -1;
    if (cuDriverGetVersion) cuDriverGetVersion(&ver);
    printf("driver version: %d\n", ver);

    int rc = cuInit(0);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuInit FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        if (is_expected_blocker(rc)) {
            printf("  (known blocker — see tests/integration/cuinit_test.c)\n");
            return 3;
        }
        return 1;
    }
    printf("cuInit OK\n");

    int count = -1;
    rc = cuDeviceGetCount(&count);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuDeviceGetCount FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        return is_expected_blocker(rc) ? 3 : 1;
    }
    printf("device count: %d\n", count);
    if (count <= 0) {
        printf("PARTIAL: cuInit OK but no devices visible\n");
        return 2;
    }

    printf("PASS: cuInit OK with %d device(s)\n", count);
    return 0;
}
