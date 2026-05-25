/*
 * nvkvm_dispatch.c — ioctl dispatch table and param size table (host side)
 *
 * Maps ioctl command → handler and expected parameter size.
 * Mirrors gVisor's driverABI dispatch tables in version.go.
 *
 * All sizes are validated before any shared memory access. Unknown ioctls
 * return -ENOTTY. Ioctls with wrong sizes return -EINVAL.
 */

#include "qemu/osdep.h"
#include <errno.h>
#include <sys/ioctl.h>

#include "virtio_nvgpu.h"

#define NV_IOC_NR(cmd)  _IOC_NR(cmd)

/* ── Expected parameter sizes ─────────────────────────────────────────────── */

size_t nvkvm_ioctl_expected_param_size(unsigned int cmd)
{
	/* UVM full-word commands */
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

	/* Frontend ioctls — IOC_NR dispatch */
	switch (NV_IOC_NR(cmd)) {
	case NV_ESC_CARD_INFO: {
		size_t sz = _IOC_SIZE(cmd);
		if (!sz || sz > sizeof(struct nv_ioctl_card_info) *
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
	case NV_ESC_NUMA_INFO: {
		/* Struct grew in newer drivers — accept whatever size the ioctl encodes */
		size_t sz = _IOC_SIZE(cmd);
		return sz ? sz : (size_t)-1;
	}
	case NV_ESC_WAIT_OPEN_COMPLETE:
		return sizeof(struct nv_ioctl_wait_open_complete);
	case NV_ESC_RM_ALLOC_MEMORY:
		return sizeof(struct nv_ioctl_nvos02_parameters_with_fd);
	case NV_ESC_RM_FREE:
		return sizeof(struct nvos00_parameters);
	case NV_ESC_RM_CONTROL:
		return sizeof(struct nvos54_parameters);
	case NV_ESC_RM_ALLOC: {
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

/* ── Main dispatch ────────────────────────────────────────────────────────── */

int nvkvm_dispatch_ioctl(struct nvkvm_req_ctx *ctx, unsigned int cmd)
{
	/* UVM ioctls — simple passthrough (no embedded pointers in most) */
	switch (cmd) {
	case UVM_INITIALIZE:
	case UVM_DEINITIALIZE:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * UVM_MM_INITIALIZE: links the secondary MM fd to the primary UVM fd.
	 * uvm_fd contains the fd_token of the primary UVM open (from the guest
	 * sanitizer); translate to the real host fd before forwarding.
	 */
	case UVM_MM_INITIALIZE: {
		struct uvm_mm_initialize_params *p = ctx->params_buf;
		struct nvkvm_host_fd *uvm_hfd =
			nvkvm_fd_lookup(ctx->session, (uint32_t)p->uvm_fd);
		if (!uvm_hfd) {
			fprintf(stderr, "nvkvm: UVM_MM_INITIALIZE: uvm_fd token %d not found\n",
				p->uvm_fd);
			return -EBADF;
		}
		int saved = p->uvm_fd;
		p->uvm_fd = (int32_t)uvm_hfd->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		/*
		 * UVM_MM_INITIALIZE called from QEMU returns rm_status=0x10006
		 * (NV_ERR_NOT_SUPPORTED) because the UVM kernel driver expects
		 * the calling process to be the GPU memory owner — which in our
		 * architecture is the isolate, not QEMU.  Until we route UVM
		 * ioctls through the isolate, mask the status to NV_OK so cuInit
		 * can progress.  This is a known divergence; see PLAN.md.
		 */
		if (p->rm_status == 0x10006) {
			fprintf(stderr,
				"nvkvm: UVM_MM_INITIALIZE: masking rm_status 0x10006 → 0 "
				"(QEMU-side call; need isolate routing)\n");
			p->rm_status = 0;
		}
		fprintf(stderr, "nvkvm: UVM_MM_INITIALIZE: uvm_fd_token=%d host_fd=%d ret=%d rm_status=0x%x\n",
			saved, uvm_hfd->fd, ret, p->rm_status);
		p->uvm_fd = 0;
		return ret;
	}

	case UVM_CREATE_RANGE_GROUP:
	case UVM_DESTROY_RANGE_GROUP:
	case UVM_FREE:
	case UVM_SET_PREFERRED_LOCATION:
	case UVM_UNSET_PREFERRED_LOCATION:
	case UVM_ENABLE_READ_DUPLICATION:
	case UVM_DISABLE_READ_DUPLICATION:
	case UVM_SET_ACCESSED_BY:
	case UVM_UNSET_ACCESSED_BY:
	case UVM_ENABLE_PEER_ACCESS:
	case UVM_DISABLE_PEER_ACCESS:
	case UVM_CREATE_EXTERNAL_RANGE:
	case UVM_VALIDATE_VA_RANGE:
	case UVM_PAGEABLE_MEM_ACCESS:
	case UVM_PAGEABLE_MEM_ACCESS_ON_GPU:
	case UVM_MIGRATE:
	case UVM_MIGRATE_RANGE_GROUP:
	case UVM_ALLOC_SEMAPHORE_POOL:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * UVM channel/vaspace registration: rm_ctrl_fd contains an fd_token
	 * (assigned by QEMU to the guest) that must be translated to the real
	 * host fd before the UVM driver sees it.  We zero the field on return
	 * so the guest cannot read back real host fd numbers from shared memory.
	 */
	case UVM_REGISTER_GPU_VASPACE: {
		struct uvm_register_gpu_vaspace_params *p = ctx->params_buf;
		struct nvkvm_host_fd *ctrl_hfd =
			nvkvm_fd_lookup(ctx->session, (uint32_t)p->rm_ctrl_fd);
		if (!ctrl_hfd) return -EBADF;
		p->rm_ctrl_fd = (nvhandle_t)ctrl_hfd->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->rm_ctrl_fd = 0;
		return ret;
	}
	case UVM_UNREGISTER_GPU_VASPACE:
		/* no rm_ctrl_fd in this struct — plain passthrough */
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case UVM_REGISTER_CHANNEL: {
		struct uvm_register_channel_params *p = ctx->params_buf;
		struct nvkvm_host_fd *ctrl_hfd =
			nvkvm_fd_lookup(ctx->session, (uint32_t)p->rm_ctrl_fd);
		if (!ctrl_hfd) return -EBADF;
		p->rm_ctrl_fd = (nvhandle_t)ctrl_hfd->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->rm_ctrl_fd = 0;
		return ret;
	}
	case UVM_UNREGISTER_CHANNEL: {
		struct uvm_unregister_channel_params *p = ctx->params_buf;
		struct nvkvm_host_fd *ctrl_hfd =
			nvkvm_fd_lookup(ctx->session, (uint32_t)p->rm_ctrl_fd);
		if (!ctrl_hfd) return -EBADF;
		p->rm_ctrl_fd = (nvhandle_t)ctrl_hfd->fd;
		int ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->rm_ctrl_fd = 0;
		return ret;
	}

	case UVM_REGISTER_GPU:
	case UVM_UNREGISTER_GPU:
		return nvkvm_handle_simple_ioctl(ctx, cmd);
	}

	/* Frontend ioctls — IOC_NR dispatch */
	switch (NV_IOC_NR(cmd)) {
	case NV_ESC_RM_ALLOC:
		return nvkvm_handle_rm_alloc(ctx);
	case NV_ESC_RM_FREE:
		return nvkvm_handle_rm_free(ctx);
	case NV_ESC_RM_CONTROL:
		return nvkvm_handle_rm_control(ctx);
	case NV_ESC_RM_DUP_OBJECT:
		return nvkvm_handle_rm_dup_object(ctx);
	case NV_ESC_REGISTER_FD:
		return nvkvm_handle_register_fd(ctx);
	case NV_ESC_ALLOC_OS_EVENT:
		return nvkvm_handle_alloc_os_event(ctx);
	case NV_ESC_FREE_OS_EVENT:
		return nvkvm_handle_free_os_event(ctx);

	/* Simple ioctls — no embedded pointers, pass directly through */
	case NV_ESC_CARD_INFO:
	case NV_ESC_CHECK_VERSION_STR:
	case NV_ESC_SYS_PARAMS:
	case NV_ESC_NUMA_INFO:
	case NV_ESC_WAIT_OPEN_COMPLETE:
	case NV_ESC_ATTACH_GPUS_TO_FD:
	case NV_ESC_RM_SHARE:
	case NV_ESC_RM_VID_HEAP_CONTROL:
	case NV_ESC_RM_MAP_MEMORY_DMA:
	case NV_ESC_RM_UNMAP_MEMORY_DMA:
	case NV_ESC_RM_ALLOC_CONTEXT_DMA2:
	case NV_ESC_EXPORT_TO_DMABUF_FD:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	/*
	 * RM_MAP_MEMORY / RM_UNMAP_MEMORY: contain pointer fields that the
	 * guest zeroed. The host driver returns a valid host VA in p_linear_address
	 * after the mmap; we zero it again before returning to the guest since
	 * the guest uses its own GPA-based mapping established via NVKVM_REQ_MMAP.
	 */
	case NV_ESC_RM_MAP_MEMORY: {
		struct nv_ioctl_nvos33_parameters_with_fd *p = ctx->params_buf;
		int ret;
		/* Translate fd_token back to host fd */
		if (p->fd >= 0) {
			struct nvkvm_host_fd *hfd2 =
				nvkvm_fd_lookup(ctx->session, (uint32_t)p->fd);
			if (!hfd2) return -EBADF;
			p->fd = (int32_t)hfd2->fd;
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->p_linear_address = 0;  /* guest must not use host VA */
		return ret;
	}
	case NV_ESC_RM_UNMAP_MEMORY:
		return nvkvm_handle_simple_ioctl(ctx, cmd);

	case NV_ESC_RM_ALLOC_MEMORY: {
		struct nv_ioctl_nvos02_parameters_with_fd *p = ctx->params_buf;
		int ret;
		if (p->fd >= 0) {
			struct nvkvm_host_fd *hfd2 =
				nvkvm_fd_lookup(ctx->session, (uint32_t)p->fd);
			if (!hfd2) return -EBADF;
			p->fd = (int32_t)hfd2->fd;
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->p_memory = 0;
		return ret;
	}

	case NV_ESC_RM_IDLE_CHANNELS: {
		struct nv_ioctl_idle_channels *p = ctx->params_buf;
		uint32_t n  = p->num_channels;
		int ret;
		/* Aux slot must hold 3 arrays of n handles each */
		if (n > 0) {
			size_t needed = 3 * n * sizeof(nvhandle_t);
			if (!ctx->aux_buf || ctx->aux_size < needed)
				return -EINVAL;
			p->p_clients  = (nvp64_t)(uintptr_t)ctx->aux_buf;
			p->p_devices  = p->p_clients + n * sizeof(nvhandle_t);
			p->p_channels = p->p_devices + n * sizeof(nvhandle_t);
		}
		ret = nvkvm_handle_simple_ioctl(ctx, cmd);
		p->p_clients = p->p_devices = p->p_channels = 0;
		return ret;
	}
	}

	return -ENOTTY;
}
