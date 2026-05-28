/*
 * nvkvm_ioctl_dump.c — LD_PRELOAD that captures NVIDIA ioctl arg buffers
 * before AND after the call, dumping the bytes (using _IOC_SIZE for the
 * exact length, so we never read past the struct).
 *
 *   pre  ioctl[<seq>] fd=<n> cmd=0x.... size=N: aa bb cc dd ...
 *   post ioctl[<seq>] fd=<n> cmd=0x.... ret=<r> size=N: aa bb cc dd ...
 *
 * Only logs NVIDIA-family ioctls (_IOC_TYPE == 'F' or 'u' for UVM, 'P'
 * for nvidia-uvm-tools), so the noise from other ioctls is filtered out.
 *
 * Set NVKVM_DUMP=<path> to redirect output to a file instead of stderr.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>

static int (*real_ioctl)(int, unsigned long, ...);
static FILE *out;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long seq_ctr;

static void open_out(void)
{
	if (out) return;
	const char *p = getenv("NVKVM_DUMP");
	if (p && *p) {
		FILE *f = fopen(p, "w");
		if (f) { setvbuf(f, NULL, _IOLBF, 0); out = f; return; }
	}
	out = stderr;
}

static int is_nvidia_fd(int fd)
{
	char buf[64];
	char link[128];
	snprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(buf, link, sizeof(link)-1);
	if (n <= 0) return 0;
	link[n] = 0;
	return strstr(link, "nvidia") != NULL;
}

static void hexdump(const void *p, size_t n, char *out_buf, size_t out_cap)
{
	const unsigned char *b = p;
	size_t pos = 0;
	for (size_t i = 0; i < n && pos + 4 < out_cap; i++) {
		pos += snprintf(out_buf + pos, out_cap - pos,
				"%02x%s", b[i],
				((i+1) % 4 == 0 && i+1 != n) ? "_" : "");
	}
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	open_out();

	unsigned int type = _IOC_TYPE(req);
	int log = (type == 'F' || type == 'u' || type == 0xc4 || type == 0xc5);

	unsigned long seq = 0;
	size_t sz = _IOC_SIZE(req);
	char buf_pre[1024]  = {0};
	char buf_post[1024] = {0};

	if (log && arg && sz > 0 && sz <= 256) {
		if (!is_nvidia_fd(fd)) log = 0;
	}

	if (log && arg && sz > 0 && sz <= 256) {
		pthread_mutex_lock(&out_lock);
		seq = ++seq_ctr;
		hexdump(arg, sz, buf_pre, sizeof(buf_pre));
		fprintf(out, "pre  ioctl[%lu] fd=%d cmd=0x%lx size=%zu: %s\n",
			seq, fd, req, sz, buf_pre);
		pthread_mutex_unlock(&out_lock);
	}

	int r = real_ioctl(fd, req, arg);

	if (log && arg && sz > 0 && sz <= 256) {
		pthread_mutex_lock(&out_lock);
		hexdump(arg, sz, buf_post, sizeof(buf_post));
		fprintf(out, "post ioctl[%lu] fd=%d cmd=0x%lx ret=%d size=%zu: %s\n",
			seq, fd, req, r, sz, buf_post);
		/* For RM_CONTROL (NV_ESC_RM_CONTROL = 0x2a), the inner param
		 * buffer is pointed to by NVOS54.params (offset 16, 8 bytes);
		 * NVOS54.params_size at offset 24 (4 bytes).  Dump up to 128
		 * bytes of that buffer so we can see what the kernel wrote. */
		if (_IOC_TYPE(req) == 'F' && _IOC_NR(req) == 0x2a && sz >= 32) {
			const uint8_t *b = arg;
			uintptr_t pp;  uint32_t ps;
			memcpy(&pp, b + 16, sizeof(pp));
			memcpy(&ps, b + 24, sizeof(ps));
			if (pp != 0 && ps > 0 && ps <= 256) {
				char aux[1024] = {0};
				hexdump((const void *)pp, ps, aux, sizeof(aux));
				fprintf(out, "      ctrl[%lu] inner-params (%u B): %s\n",
					seq, ps, aux);
			}
		}
		pthread_mutex_unlock(&out_lock);
	}
	return r;
}
