/* nvioctl_snoop.c — LD_PRELOAD shim to capture the NVIDIA frontend allocs that
 * matter for Mode-2 os-event delivery, straight from libcuda's ioctls (no guest
 * kernel rebuild).  Decodes:
 *   NV_ESC_RM_ALLOC (NR 0x2B): NVOS21/64 {hRoot@0, hObjectParent@4, hObjectNew@8,
 *       hClass@12} — prints when hClass==NV01_EVENT_OS_EVENT(0x0079) or any
 *       event class (0x0005/0x0079/0x007e) so we learn libcuda's real
 *       (hClient,hEvent) for the blocking-sync wait.
 *   NV_ESC_ALLOC_OS_EVENT (NR 206): {hClient@0, hDevice@4, fd@8} — the os-event
 *       fd libcuda's poll() blocks on.
 *   NV_ESC_RM_CONTROL (NR 0x2A): NVOS54 {hClient@0, hObject@4, cmd@8} — prints
 *       EVENT_SET_NOTIFICATION (0x20800301) to see which notifier is armed.
 * Build: gcc -shared -fPIC -O2 -o nvioctl_snoop.so nvioctl_snoop.c -ldl */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/ioctl.h>

#ifndef _IOC_TYPE
#include <asm/ioctl.h>
#endif

static int (*real_ioctl)(int, unsigned long, ...);

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; void *arg;
    va_start(ap, req); arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");

    unsigned type = _IOC_TYPE(req), nr = _IOC_NR(req), sz = _IOC_SIZE(req);
    int is_evt_alloc = 0; uint32_t e_hClass = 0, e_hRoot = 0, e_hNew = 0;
    if (type == 'F' && arg) {
        uint32_t *p = (uint32_t *)arg;
        if (nr == 0x2B) {                 /* NV_ESC_RM_ALLOC (NVOS21/64) */
            uint32_t hRoot = p[0], hParent = p[1], hNew = p[2], hClass = p[3];
            if (hClass == 0x0079u || hClass == 0x0005u || hClass == 0x007eu) {
                fprintf(stderr, "[SNOOP] RM_ALLOC EVENT hClass=0x%04x hRoot(client)=0x%08x "
                        "hParent=0x%08x hObjectNew(event)=0x%08x sz=%u\n",
                        hClass, hRoot, hParent, hNew, sz);
                is_evt_alloc = 1; e_hClass = hClass; e_hRoot = hRoot; e_hNew = hNew;
            }
        } else if (nr == 206) {           /* NV_ESC_ALLOC_OS_EVENT */
            fprintf(stderr, "[SNOOP] ALLOC_OS_EVENT hClient=0x%08x hDevice=0x%08x fd=%d\n",
                    p[0], p[1], (int)p[2]);
        } else if (nr == 0x29) {          /* NV_ESC_RM_FREE (NVOS00) */
            fprintf(stderr, "[SNOOP] RM_FREE hRoot=0x%08x hParent=0x%08x hObjectOld=0x%08x\n",
                    p[0], p[1], p[2]);
        } else if (nr == 0x2A) {          /* NV_ESC_RM_CONTROL (NVOS54) */
            uint32_t hClient = p[0], hObject = p[1], cmd = p[2];
            if (cmd == 0x20800301u || cmd == 0x20800302u || cmd == 0x730190u) {
                fprintf(stderr, "[SNOOP] RM_CONTROL cmd=0x%08x hClient=0x%08x hObject=0x%08x\n",
                        cmd, hClient, hObject);
            }
        }
        fflush(stderr);
    }
    int rc = real_ioctl(fd, req, arg);
    if (is_evt_alloc) {                   /* status: NVOS64(48)@40, NVOS21(32)@28 */
        uint32_t *p = (uint32_t *)arg;
        uint32_t status = (sz >= 48) ? p[10] : p[7];
        fprintf(stderr, "[SNOOP]  -> EVENT alloc 0x%08x class=0x%04x status=0x%x rc=%d\n",
                e_hNew, e_hClass, status, rc);
        fflush(stderr);
    }
    return rc;
}
