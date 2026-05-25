/* Minimal cuInit test — directly calls cuInit via dlopen */
#include <stdio.h>
#include <dlfcn.h>

int main(void)
{
    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    int (*cuInit)(unsigned int) = dlsym(h, "cuInit");
    int (*cuDriverGetVersion)(int*) = dlsym(h, "cuDriverGetVersion");
    int (*cuDeviceGetCount)(int*) = dlsym(h, "cuDeviceGetCount");
    int (*cuGetErrorString)(int, const char**) = dlsym(h, "cuGetErrorString");

    if (!cuInit || !cuDeviceGetCount) {
        printf("dlsym failed\n"); return 1;
    }

    int ver = -1;
    if (cuDriverGetVersion) cuDriverGetVersion(&ver);
    printf("driver version: %d\n", ver);

    int rc = cuInit(0);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuInit FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        return rc;
    }
    printf("cuInit OK\n");

    int count = -1;
    rc = cuDeviceGetCount(&count);
    if (rc != 0) {
        const char *msg = NULL;
        if (cuGetErrorString) cuGetErrorString(rc, &msg);
        printf("cuDeviceGetCount FAILED: %d (%s)\n", rc, msg ? msg : "<no string>");
        return rc;
    }
    printf("device count: %d\n", count);
    return 0;
}
