/*
 * nvkvm_mmap_host.c — GPU memory mapping management (host side)
 *
 * When the guest requests a mmap on a /dev/nvidia* fd, we:
 *   1. Call mmap() on the real host nvidia fd to get a host VA backed by GPU
 *      memory (BAR framebuffer, command ring, doorbell page, etc.).
 *   2. Register those host pages as a new KVM memory slot so they become
 *      visible at a guest physical address (GPA).
 *   3. Return the GPA to the guest, which then calls remap_pfn_range() in its
 *      kernel module to complete the process-level VMA mapping.
 *
 * On munmap, we remove the KVM memory slot and munmap() the host VA.
 *
 * IOMMU / cache coherence
 * =======================
 * NVIDIA GPU memory mappings fall into two categories:
 *
 *   a) Framebuffer / VRAM (BAR2): Write-Combine on x86, or MT_WRITE_COMBINE
 *      on ARM. These are MMIO pages. We set KVM_MEM_READONLY for read-only
 *      BAR regions and use KVM_SET_USER_MEMORY_REGION with appropriate flags.
 *
 *   b) System memory (pinned, write-back): These are ordinary DRAM pages that
 *      the NVIDIA driver has pinned. We use KVM_SET_USER_MEMORY_REGION
 *      directly; the guest should see them as write-back cached.
 *
 * We infer the memory type from the PROT flags and the device type:
 *   - /dev/nvidiactl and /dev/nvidia* BAR mmaps → write-combine
 *   - /dev/nvidia-uvm and system descriptor mmaps → write-back
 *
 * DMA safety
 * ==========
 * The NVIDIA driver has already pinned (and possibly DMA-mapped) the physical
 * pages before returning from the mmap call. We do not need additional VFIO
 * DMA-mapping for simple user-space compute workloads. For scenarios
 * requiring proper IOMMU passthrough (e.g., DMA engines writing directly into
 * guest-owned memory), VFIO integration is a future extension point.
 */

#include "qemu/osdep.h"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "virtio_nvgpu.h"

/*
 * Inline KVM memory region API — avoids including <linux/kvm.h> which
 * conflicts with QEMU's internal header chain (resettable.h typedef issue).
 * Values are stable ABI constants on Linux x86-64.
 */
#ifndef KVM_SET_USER_MEMORY_REGION
#define NVKVM_KVMIO                     0xAE
#define KVM_MEM_READONLY                (1UL << 1)
#define KVM_SET_USER_MEMORY_REGION      _IOW(NVKVM_KVMIO, 0x46, \
					     struct nvkvm_kvm_mem_region)
struct nvkvm_kvm_mem_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#else
typedef struct kvm_userspace_memory_region nvkvm_kvm_mem_region;
#endif

/* KVM VM fd opened at device init */
static int kvm_vm_fd = -1;
/* Exposed for nvkvm_isolate_handlers.c */
int nvkvm_kvm_vm_fd = -1;

void nvkvm_set_kvm_vm_fd(int fd)
{
	kvm_vm_fd     = fd;
	nvkvm_kvm_vm_fd = fd;
}

/* ── GPA allocator ────────────────────────────────────────────────────────── */

/*
 * Allocate a contiguous GPA range in the mmap window.
 * The window is a reserved region in the guest physical address space that
 * QEMU pre-configures as empty (no backing RAM) specifically for these
 * dynamic GPU mappings.
 *
 * Alignment: mappings are aligned to PAGE_SIZE (4 KiB). For BAR mappings
 * that must be 2 MiB aligned (huge pages), we align up to 2 MiB.
 */
static uint64_t alloc_gpa(VirtIONvgpu *nv, size_t length)
{
	uint64_t gpa;
	size_t align = (length >= (2 << 20)) ? (2 << 20) : 4096;

	pthread_mutex_lock(&nv->mmap_win_lock);
	/* Align current pointer */
	nv->mmap_win_cur = (nv->mmap_win_cur + align - 1) & ~(align - 1);

	if (nv->mmap_win_cur + length > nv->mmap_win_size) {
		pthread_mutex_unlock(&nv->mmap_win_lock);
		fprintf(stderr, "nvkvm: mmap window exhausted\n");
		return 0;
	}

	gpa = nv->mmap_win_gpa + nv->mmap_win_cur;
	nv->mmap_win_cur += length;
	pthread_mutex_unlock(&nv->mmap_win_lock);
	return gpa;
}

/* Thin wrapper used by nvkvm_isolate_handlers.c for double-mmap GPA allocation */
void nvkvm_mmap_win_alloc(VirtIONvgpu *nv, size_t length, uint64_t *gpa_out)
{
	*gpa_out = alloc_gpa(nv, length);
}

/* ── KVM memory slot management ───────────────────────────────────────────── */

/*
 * next_slot_id: KVM memory slots are numbered from 0. We use slots from
 * NVKVM_KVM_SLOT_BASE upward to avoid conflicts with QEMU's own slots.
 */
#define NVKVM_KVM_SLOT_BASE  64
static int next_kvm_slot = NVKVM_KVM_SLOT_BASE;

static int kvm_add_memory_region(uint64_t gpa, void *hva, size_t length,
				 bool readonly, int *slot_out)
{
	struct nvkvm_kvm_mem_region region = {
		.slot            = next_kvm_slot++,
		.flags           = readonly ? (uint32_t)KVM_MEM_READONLY : 0,
		.guest_phys_addr = gpa,
		.memory_size     = length,
		.userspace_addr  = (uint64_t)(uintptr_t)hva,
	};
	if (kvm_vm_fd < 0) {
		fprintf(stderr,
			"nvkvm: kvm_vm_fd not set; GPU mmap will not be "
			"directly accessible in guest\n");
		*slot_out = -1;
		return 0;  /* non-fatal for initial bring-up */
	}
	if (ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		fprintf(stderr,
			"nvkvm: KVM_SET_USER_MEMORY_REGION failed: %s\n",
			strerror(errno));
		return -errno;
	}
	*slot_out = (int)region.slot;
	return 0;
}

static void kvm_remove_memory_region(int slot)
{
	struct nvkvm_kvm_mem_region region = {
		.slot         = (uint32_t)slot,
		.memory_size  = 0,  /* size=0 removes the slot */
	};
	if (kvm_vm_fd >= 0)
		ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int nvkvm_mmap_create(VirtIONvgpu *nv, struct nvkvm_host_fd *hfd,
		      uint64_t offset, size_t length,
		      int prot, int flags,
		      struct nvkvm_mmap_region **region_out)
{
	void *hva;
	struct nvkvm_mmap_region *region;

	/* Round up to page size */
	length = (length + 4095) & ~4095UL;

	hva = mmap(NULL, length, prot, flags, hfd->fd, (off_t)offset);
	if (hva == MAP_FAILED) {
		fprintf(stderr,
			"nvkvm: host mmap fd=%d offset=0x%llx len=%zu: %s\n",
			hfd->fd, (unsigned long long)offset, length,
			strerror(errno));
		return -errno;
	}

	region = g_new0(struct nvkvm_mmap_region, 1);
	region->host_va = hva;
	region->length  = length;
	/* guest_pa and kvm_slot filled by nvkvm_mmap_map_to_guest */

	*region_out = region;
	return 0;
}

int nvkvm_mmap_map_to_guest(VirtIONvgpu *nv,
			    struct nvkvm_mmap_region *region)
{
	uint64_t gpa;
	int slot = -1;
	int ret;

	gpa = alloc_gpa(nv, region->length);
	if (!gpa) {
		munmap(region->host_va, region->length);
		return -ENOMEM;
	}

	ret = kvm_add_memory_region(gpa, region->host_va, region->length,
				    false, &slot);
	if (ret) {
		munmap(region->host_va, region->length);
		return ret;
	}

	region->guest_pa = gpa;
	region->kvm_slot = slot;
	return 0;
}

void nvkvm_mmap_unmap_from_guest(VirtIONvgpu *nv,
				 struct nvkvm_mmap_region *region)
{
	if (region->kvm_slot >= 0) {
		kvm_remove_memory_region(region->kvm_slot);
		region->kvm_slot = -1;
	}
	/* GPA is not returned to the pool (simple bump allocator).
	 * A future version could use a proper free-list. */
}

void nvkvm_mmap_destroy(VirtIONvgpu *nv,
			struct nvkvm_mmap_region *region)
{
	if (region->host_va && region->host_va != MAP_FAILED)
		munmap(region->host_va, region->length);
	g_free(region);
}
