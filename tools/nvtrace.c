// SPDX-License-Identifier: GPL-2.0
/*
 * nvtrace.c — LD_PRELOAD ioctl/mmap interposer for byte-exact host-vs-guest
 * comparison of the NVIDIA RM ioctl stream (#84 Vulkan enumeration gap).
 *
 * Dumps ONLY defined bytes — never reads past a struct into uninitialised
 * stack noise:
 *   - the NVOS54 (RM_CONTROL, frontend nr 0x2a) inner params, exactly
 *     paramsSize bytes (app-defined), both BEFORE (input) and AFTER (kernel
 *     writeback) the real ioctl;
 *   - the control command number + status (semantic fields, not the pointer
 *     field, so logs diff cleanly across runs);
 *   - every mmap on an nvidia/dri fd (fd-device, offset, len, prot, result)
 *     so we can confirm all GPU mappings install on both sides.
 *
 * Build:  cc -O2 -fPIC -shared -o nvtrace.so nvtrace.c -ldl
 * Run:    LD_PRELOAD=./nvtrace.so NVTRACE=/tmp/g.log \
 *         VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
 *         vulkaninfo --summary
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static int   (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static FILE *logf;

static void lazy(void)
{
	if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!real_mmap)  real_mmap  = dlsym(RTLD_NEXT, "mmap");
	if (!logf) {
		const char *p = getenv("NVTRACE");
		logf = p ? fopen(p, "w") : stderr;
		if (!logf) logf = stderr;
	}
}

/* Resolve fd -> device basename; returns 1 for nvidia/dri devices. */
static int devname(int fd, char *buf, size_t n)
{
	char l[64];
	snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
	ssize_t r = readlink(l, buf, n - 1);
	if (r < 0) r = 0;
	buf[r] = 0;
	return strstr(buf, "/dev/nvidia") != NULL || strstr(buf, "/dev/dri/") != NULL;
}

/* Short device tag (nvidiactl / nvidia0 / renderD128 / card0). */
static const char *tag(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

static void hexdump(const char *label, const void *p, uint32_t n)
{
	const unsigned char *b = p;
	if (!p || n == 0) { fprintf(logf, "    %s: <none>\n", label); return; }
	if (n > 4096) { fprintf(logf, "    %s: <%u bytes, capped>\n", label, n); n = 4096; }
	fprintf(logf, "    %s[%u]: ", label, n);
	for (uint32_t i = 0; i < n; i++) fprintf(logf, "%02x", b[i]);
	fputc('\n', logf);
}

int ioctl(int fd, unsigned long request, ...)
{
	lazy();
	va_list ap; va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	char dev[80];
	int track = devname(fd, dev, sizeof dev);
	unsigned type = _IOC_TYPE(request), nr = _IOC_NR(request), size = _IOC_SIZE(request);

	/* NVOS54 RM_CONTROL: hClient@0 hObject@4 cmd@8 flags@12 params@16(ptr)
	 * paramsSize@24 status@28.  Read only these defined fields. */
	void    *cparams = NULL;
	uint32_t csize = 0, cctrl = 0;
	int is_ctrl = (track && type == 'F' && nr == 0x2a && arg && size >= 32);
	if (is_ctrl) {
		memcpy(&cctrl,   (char *)arg + 8,  4);
		memcpy(&cparams, (char *)arg + 16, 8);
		memcpy(&csize,   (char *)arg + 24, 4);
	}

	/* RM_ALLOC (nr 0x2b, NVOS21/64): hClass@12 (shared prefix). */
	uint32_t aclass = 0;
	if (track && type == 'F' && nr == 0x2b && arg && size >= 16)
		memcpy(&aclass, (char *)arg + 12, 4);
	/* NVOS64 (size 48): inner alloc params at pAllocParms@16, size@32. Dump
	 * exactly that many bytes (no stack noise) to catch writeback diffs. */
	void    *aparms = NULL;
	uint32_t apsize = 0;
	if (track && type == 'F' && nr == 0x2b && arg && size == 48) {
		memcpy(&aparms, (char *)arg + 16, 8);
		memcpy(&apsize, (char *)arg + 32, 4);
	}

	if (track) {
		fprintf(logf, "IOCTL %-10s nr=0x%02x size=%u", tag(dev), nr, size);
		if (is_ctrl) fprintf(logf, " ctrl=0x%08x psize=%u", cctrl, csize);
		if (nr == 0x2b) fprintf(logf, " class=0x%04x apsize=%u", aclass, apsize);
		fputc('\n', logf);
		/* Full outer struct (defined _IOC_SIZE bytes only) + control/alloc inner. */
		if (arg && size) hexdump("inS ", arg, size);
		if (is_ctrl && cparams) hexdump("in  ", cparams, csize);
		if (aparms && apsize) hexdump("aIN ", aparms, apsize);
	}

	int ret = real_ioctl(fd, request, arg);

	if (track) {
		if (is_ctrl) {
			uint32_t status = 0;
			memcpy(&status, (char *)arg + 28, 4);
			fprintf(logf, "  -> ret=%d status=0x%x\n", ret, status);
		} else {
			fprintf(logf, "  -> ret=%d\n", ret);
		}
		if (arg && size) hexdump("outS", arg, size);
		if (is_ctrl && cparams) hexdump("out ", cparams, csize);
		if (aparms && apsize) hexdump("aOUT", aparms, apsize);
		fflush(logf);
	}
	return ret;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	lazy();
	void *r = real_mmap(addr, len, prot, flags, fd, off);
	if (fd >= 0) {
		char dev[80];
		if (devname(fd, dev, sizeof dev)) {
			fprintf(logf, "MMAP  %-10s off=0x%lx len=0x%zx prot=0x%x flags=0x%x -> %p%s\n",
				tag(dev), (unsigned long)off, len, prot, flags, r,
				r == MAP_FAILED ? " FAILED" : "");
			fflush(logf);
		}
	}
	return r;
}
