// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_ioctl.c — ioctl parameter size table and sanitizer
 *
 * Two responsibilities:
 *   1. Return the expected fixed parameter size for a known ioctl command so
 *      the caller can copy exactly that many bytes from userspace.
 *   2. Sanitize the parameter blob before it goes into shared memory:
 *        - Replace any embedded guest VA pointers with 0 (the host will supply
 *          its own pointers after translating auxiliary data from the aux slot).
 *        - Translate session-local fd_token values embedded in structs (e.g.
 *          NV_ESC_REGISTER_FD, NV_ESC_ALLOC_OS_EVENT) to the real host FD
 *          tokens the host expects.
 *
 * The design mirrors gVisor nvproxy's per-ioctl handler table in version.go,
 * but simplified to a two-pass approach (size lookup + sanitize) rather than
 * individual copy-in/copy-out wrappers, since the guest kernel module doesn't
 * need to interpret the semantic content of most structs — it just needs to
 * ensure no raw guest pointers leak to the host.
 */

#include <linux/errno.h>
#include <linux/ioctl.h>
#include "nvkvm.h"

/* ── Ioctl number extraction ─────────────────────────────────────────────── */

/* IOC_NR gives the function-code portion of the ioctl command */
#define NV_IOC_NR(cmd)  _IOC_NR(cmd)

/* ── Parameter size table ─────────────────────────────────────────────────── */

/*
 * Returns the expected byte size of the ioctl parameter struct, or
 * (size_t)-1 for unknown commands.
 *
 * Only the IOC_NR portion is used for frontend ioctls, consistent with how
 * the NVIDIA driver ignores IOC_TYPE.
 *
 * UVM ioctls use the full 32-bit command word because they have their own
 * numbering scheme that starts at 0x30000001.
 */
size_t nvkvm_ioctl_param_size(unsigned int cmd)
{
	/* UVM ioctls — identified by full command word */
	switch (cmd) {
	case UVM_INITIALIZE:
		return sizeof(struct uvm_initialize_params);
	case UVM_DEINITIALIZE:
		return sizeof(struct uvm_deinitialize_params);
	case UVM_MM_INITIALIZE:
		return sizeof(struct uvm_mm_initialize_params);
	case UVM_REGISTER_GPU:
		return sizeof(struct uvm_register_gpu_params);
	case UVM_UNREGISTER_GPU:
		return sizeof(struct uvm_unregister_gpu_params);
	case UVM_REGISTER_GPU_VASPACE:
		return sizeof(struct uvm_register_gpu_vaspace_params);
	case UVM_UNREGISTER_GPU_VASPACE:
		return sizeof(struct uvm_unregister_gpu_vaspace_params);
	case UVM_REGISTER_CHANNEL:
		return sizeof(struct uvm_register_channel_params);
	case UVM_UNREGISTER_CHANNEL:
		return sizeof(struct uvm_unregister_channel_params);
	case UVM_CREATE_RANGE_GROUP:
		return sizeof(struct uvm_create_range_group_params);
	case UVM_DESTROY_RANGE_GROUP:
		return sizeof(struct uvm_destroy_range_group_params);
	case UVM_SET_RANGE_GROUP:
		return sizeof(struct uvm_set_range_group_params);
	case UVM_MAP_EXTERNAL_ALLOCATION:
		return sizeof(struct uvm_map_external_allocation_params);
	case UVM_FREE:
		return sizeof(struct uvm_free_params);
	case UVM_MIGRATE:
		return sizeof(struct uvm_migrate_params);
	case UVM_SET_PREFERRED_LOCATION:
		return sizeof(struct uvm_set_preferred_location_params);
	case UVM_UNSET_PREFERRED_LOCATION:
		return sizeof(struct uvm_unset_preferred_location_params);
	case UVM_SET_ACCESSED_BY:
		return sizeof(struct uvm_set_accessed_by_params);
	case UVM_UNSET_ACCESSED_BY:
		return sizeof(struct uvm_unset_accessed_by_params);
	case UVM_ENABLE_PEER_ACCESS:
		return sizeof(struct uvm_enable_peer_access_params);
	case UVM_DISABLE_PEER_ACCESS:
		return sizeof(struct uvm_disable_peer_access_params);
	case UVM_CREATE_EXTERNAL_RANGE:
		return sizeof(struct uvm_create_external_range_params);
	case UVM_VALIDATE_VA_RANGE:
		return sizeof(struct uvm_validate_va_range_params);
	case UVM_PAGEABLE_MEM_ACCESS:
		return sizeof(struct uvm_pageable_mem_access_params);
	case UVM_ALLOC_SEMAPHORE_POOL:
		return sizeof(struct uvm_alloc_semaphore_pool_params);
	}

	/* Frontend ioctls — dispatch on IOC_NR only */
	switch (NV_IOC_NR(cmd)) {
	case NV_ESC_CARD_INFO:
		/*
		 * NV_ESC_CARD_INFO takes an array; size is encoded in IOC_SIZE.
		 * Cap at the maximum array size.
		 */
		{
			size_t sz = _IOC_SIZE(cmd);
			if (sz == 0 || sz > sizeof(struct nv_ioctl_card_info) *
			    NV_IOCTL_CARD_INFO_MAX_ENTRIES)
				return (size_t)-1;
			return sz;
		}
	case NV_ESC_REGISTER_FD:
		return sizeof(struct nv_ioctl_register_fd);
	case NV_ESC_ALLOC_OS_EVENT:
		return sizeof(struct nv_ioctl_alloc_os_event);
	case NV_ESC_FREE_OS_EVENT:
		return sizeof(struct nv_ioctl_free_os_event);
	case NV_ESC_CHECK_VERSION_STR:
		return sizeof(struct nv_ioctl_rm_api_version);
	case NV_ESC_SYS_PARAMS:
		return sizeof(struct nv_ioctl_sys_params);
	case NV_ESC_NUMA_INFO:
		return sizeof(struct nv_ioctl_numa_info);
	case NV_ESC_WAIT_OPEN_COMPLETE:
		return sizeof(struct nv_ioctl_wait_open_complete);
	case NV_ESC_RM_ALLOC_MEMORY:
		return sizeof(struct nv_ioctl_nvos02_parameters_with_fd);
	case NV_ESC_RM_FREE:
		return sizeof(struct nvos00_parameters);
	case NV_ESC_RM_CONTROL:
		return sizeof(struct nvos54_parameters);
	case NV_ESC_RM_ALLOC:
		/*
		 * The kernel accepts both NVOS21 and NVOS64 formats; the size
		 * is in IOC_SIZE.
		 */
		{
			size_t sz = _IOC_SIZE(cmd);
			if (sz == sizeof(struct nvos21_parameters) ||
			    sz == sizeof(struct nvos64_parameters))
				return sz;
			return (size_t)-1;
		}
	case NV_ESC_RM_DUP_OBJECT:
		return sizeof(struct nvos55_parameters);
	case NV_ESC_RM_SHARE:
		return sizeof(struct nvos57_parameters);
	case NV_ESC_RM_VID_HEAP_CONTROL:
		return sizeof(struct nvos32_parameters);
	case NV_ESC_RM_MAP_MEMORY:
		return sizeof(struct nv_ioctl_nvos33_parameters_with_fd);
	case NV_ESC_RM_UNMAP_MEMORY:
		return sizeof(struct nv_ioctl_nvos34_parameters);
	case NV_ESC_RM_MAP_MEMORY_DMA:
		return sizeof(struct nvos46_parameters);
	case NV_ESC_RM_UNMAP_MEMORY_DMA:
		return sizeof(struct nvos47_parameters);
	case NV_ESC_RM_IDLE_CHANNELS:
		return sizeof(struct nv_ioctl_idle_channels);
	case NV_ESC_RM_ALLOC_CONTEXT_DMA2:
		return sizeof(struct nv_ioctl_alloc_context_dma2);
	case NV_ESC_EXPORT_TO_DMABUF_FD:
		return sizeof(struct nv_ioctl_export_to_dmabuf_fd);
	}

	return (size_t)-1;
}

/* ── Pointer / FD sanitizer ──────────────────────────────────────────────── */

/*
 * nvkvm_sanitize_ioctl_params — zero out guest VA pointers and rewrite
 * embedded FD numbers to the host-facing token values.
 *
 * The host backend will reconstruct any necessary secondary buffer pointers
 * from the aux slot contents; it must never receive a raw guest VA.
 *
 * Rules:
 *   - NvP64 / pointer-sized fields that contain guest VAs: zero them.
 *     The host will supply valid host pointers.
 *   - FD fields (int32) that reference another open nvkvm device: translate
 *     to the fd_token the host understands.
 *   - Everything else passes through unchanged.
 *
 * For ioctls with a secondary buffer pointer (NV_ESC_RM_CONTROL's params
 * pointer, NV_ESC_RM_ALLOC's alloc-params pointer, etc.) the guest places the
 * secondary buffer in the aux slot before this call; here we just zero the
 * pointer. The host will reconstruct it from the aux slot.
 */
int nvkvm_sanitize_ioctl_params(struct nvkvm_fd_ctx *ctx,
				unsigned int cmd,
				void *buf, size_t size)
{
	if (!buf || size == 0)
		return 0;

	switch (NV_IOC_NR(cmd)) {

	case NV_ESC_RM_ALLOC: {
		if (size == sizeof(struct nvos64_parameters)) {
			struct nvos64_parameters *p = buf;
			p->p_alloc_parms       = 0;  /* in aux slot           */
			p->p_rights_requested  = 0;  /* in aux slot if non-NULL */
		} else if (size == sizeof(struct nvos21_parameters)) {
			struct nvos21_parameters *p = buf;
			p->p_alloc_parms = 0;        /* in aux slot           */
		}
		break;
	}

	case NV_ESC_RM_CONTROL: {
		struct nvos54_parameters *p = buf;
		p->params = 0;               /* secondary buf in aux slot */
		break;
	}

	case NV_ESC_RM_ALLOC_MEMORY: {
		struct nv_ioctl_nvos02_parameters_with_fd *p = buf;
		p->p_memory = 0;             /* host fills this in        */
		/*
		 * p->fd is a guest fd token; translate to host fd_token.
		 * A value of -1 means "no fd" and passes through unchanged.
		 */
		if (p->fd >= 0) {
			struct file *f = fget(p->fd);
			if (!f)
				return -EBADF;
			{
				struct nvkvm_fd_ctx *other =
					f->private_data;
				if (!other) {
					fput(f);
					return -EBADF;
				}
				p->fd = (__s32)other->fd_token;
			}
			fput(f);
		}
		break;
	}

	case NV_ESC_RM_MAP_MEMORY: {
		struct nv_ioctl_nvos33_parameters_with_fd *p = buf;
		p->p_linear_address = 0;     /* host fills this in        */
		if (p->fd >= 0) {
			struct file *f = fget(p->fd);
			if (!f)
				return -EBADF;
			{
				struct nvkvm_fd_ctx *other =
					f->private_data;
				if (!other) {
					fput(f);
					return -EBADF;
				}
				p->fd = (__s32)other->fd_token;
			}
			fput(f);
		}
		break;
	}

	case NV_ESC_RM_UNMAP_MEMORY: {
		struct nv_ioctl_nvos34_parameters *p = buf;
		p->p_linear_address = 0;     /* host fills in from its map table */
		break;
	}

	case NV_ESC_RM_VID_HEAP_CONTROL: {
		struct nvos32_parameters *p = buf;
		p->p_memory = 0;
		break;
	}

	case NV_ESC_RM_IDLE_CHANNELS: {
		struct nv_ioctl_idle_channels *p = buf;
		/* These three are pointers to arrays; moved to aux slot */
		p->p_clients  = 0;
		p->p_devices  = 0;
		p->p_channels = 0;
		break;
	}

	case NV_ESC_REGISTER_FD: {
		struct nv_ioctl_register_fd *p = buf;
		if (p->ctl_fd >= 0) {
			struct file *f = fget(p->ctl_fd);
			if (!f)
				return -EBADF;
			{
				struct nvkvm_fd_ctx *other =
					f->private_data;
				if (!other) {
					fput(f);
					return -EBADF;
				}
				p->ctl_fd = (__s32)other->fd_token;
			}
			fput(f);
		}
		break;
	}

	case NV_ESC_ALLOC_OS_EVENT: {
		struct nv_ioctl_alloc_os_event *p = buf;
		/*
		 * p->fd is an eventfd; translate to the fd_token of the
		 * corresponding nvkvm device FD the event is associated with.
		 * If it's not a nvkvm FD, replace with a sentinel and let the
		 * host handle it (the host will allocate its own eventfd and
		 * relay events back via VQ_EVT).
		 */
		if (p->fd != (unsigned)-1) {
			struct file *f = fget(p->fd);
			if (!f)
				return -EBADF;
			{
				struct nvkvm_fd_ctx *other =
					f->private_data;
				p->fd = other ? other->fd_token : (__u32)-1;
			}
			fput(f);
		}
		break;
	}

	case NV_ESC_FREE_OS_EVENT: {
		struct nv_ioctl_free_os_event *p = buf;
		if (p->fd != (unsigned)-1) {
			struct file *f = fget(p->fd);
			if (!f)
				return -EBADF;
			{
				struct nvkvm_fd_ctx *other =
					f->private_data;
				p->fd = other ? other->fd_token : (__u32)-1;
			}
			fput(f);
		}
		break;
	}

	/* All other ioctls contain no pointer or FD fields; pass through. */
	default:
		break;
	}

	return 0;
}
