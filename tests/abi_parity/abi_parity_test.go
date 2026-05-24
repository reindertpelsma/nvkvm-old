// abi_parity_test.go — ABI struct size parity tests
//
// Verifies that the C structs in src/abi/ match the sizes expected by the
// NVIDIA driver. These are the same checks gVisor does in
// pkg/sentry/devices/nvproxy/nvproxy_driver_parity_test.go and
// pkg/sentry/devices/nvproxy/nvproxy_test.go, but expressed for our
// C header definitions via cgo.
//
// Adapted from gVisor's nvproxy tests (copyright 2024 The gVisor Authors,
// Apache-2.0). See CREDITS for full attribution.
//
// Run:
//   cd tests/abi_parity
//   go test -v ./...

package abi_parity_test

// #cgo CFLAGS: -I../../src
//
// #include "abi/nvgpu.h"
// #include "abi/uvm.h"
// #include "common/nvkvm_proto.h"
// #include <stddef.h>
//
// // Sizes of all structs we need to verify
// size_t size_nvos21     = sizeof(struct nvos21_parameters);
// size_t size_nvos64     = sizeof(struct nvos64_parameters);
// size_t size_nvos00     = sizeof(struct nvos00_parameters);
// size_t size_nvos54     = sizeof(struct nvos54_parameters);
// size_t size_nvos55     = sizeof(struct nvos55_parameters);
// size_t size_nvos57     = sizeof(struct nvos57_parameters);
// size_t size_nvos32     = sizeof(struct nvos32_parameters);
// size_t size_nvos46     = sizeof(struct nvos46_parameters);
// size_t size_nvos47     = sizeof(struct nvos47_parameters);
//
// size_t size_nv_ioctl_card_info    = sizeof(struct nv_ioctl_card_info);
// size_t size_nv_ioctl_register_fd  = sizeof(struct nv_ioctl_register_fd);
// size_t size_nv_ioctl_alloc_os_ev  = sizeof(struct nv_ioctl_alloc_os_event);
// size_t size_nv_ioctl_free_os_ev   = sizeof(struct nv_ioctl_free_os_event);
// size_t size_nv_ioctl_rm_api_ver   = sizeof(struct nv_ioctl_rm_api_version);
// size_t size_nv_ioctl_sys_params   = sizeof(struct nv_ioctl_sys_params);
// size_t size_nv_ioctl_numa_info    = sizeof(struct nv_ioctl_numa_info);
// size_t size_nv_ioctl_wait_open    = sizeof(struct nv_ioctl_wait_open_complete);
// size_t size_nv_map_memory_fd      = sizeof(struct nv_ioctl_nvos33_parameters_with_fd);
// size_t size_nv_unmap_memory       = sizeof(struct nv_ioctl_nvos34_parameters);
// size_t size_nv_alloc_memory_fd    = sizeof(struct nv_ioctl_nvos02_parameters_with_fd);
// size_t size_nv_idle_channels      = sizeof(struct nv_ioctl_idle_channels);
// size_t size_nv_alloc_ctx_dma2     = sizeof(struct nv_ioctl_alloc_context_dma2);
// size_t size_nv_export_dmabuf      = sizeof(struct nv_ioctl_export_to_dmabuf_fd);
//
// size_t size_uvm_init              = sizeof(struct uvm_initialize_params);
// size_t size_uvm_deinit            = sizeof(struct uvm_deinitialize_params);
// size_t size_uvm_mm_init           = sizeof(struct uvm_mm_initialize_params);
// size_t size_uvm_reg_gpu           = sizeof(struct uvm_register_gpu_params);
// size_t size_uvm_unreg_gpu         = sizeof(struct uvm_unregister_gpu_params);
// size_t size_uvm_reg_gpu_vaspace   = sizeof(struct uvm_register_gpu_vaspace_params);
// size_t size_uvm_unreg_gpu_vaspace = sizeof(struct uvm_unregister_gpu_vaspace_params);
// size_t size_uvm_reg_channel       = sizeof(struct uvm_register_channel_params);
// size_t size_uvm_unreg_channel     = sizeof(struct uvm_unregister_channel_params);
// size_t size_uvm_create_rg         = sizeof(struct uvm_create_range_group_params);
// size_t size_uvm_destroy_rg        = sizeof(struct uvm_destroy_range_group_params);
// size_t size_uvm_set_rg            = sizeof(struct uvm_set_range_group_params);
// size_t size_uvm_free              = sizeof(struct uvm_free_params);
// size_t size_uvm_migrate           = sizeof(struct uvm_migrate_params);
// size_t size_uvm_set_pref_loc      = sizeof(struct uvm_set_preferred_location_params);
// size_t size_uvm_unset_pref_loc    = sizeof(struct uvm_unset_preferred_location_params);
// size_t size_uvm_set_accessed_by   = sizeof(struct uvm_set_accessed_by_params);
// size_t size_uvm_unset_accessed_by = sizeof(struct uvm_unset_accessed_by_params);
// size_t size_uvm_peer_access       = sizeof(struct uvm_enable_peer_access_params);
// size_t size_uvm_create_ext_range  = sizeof(struct uvm_create_external_range_params);
// size_t size_uvm_validate_va       = sizeof(struct uvm_validate_va_range_params);
// size_t size_uvm_pageable          = sizeof(struct uvm_pageable_mem_access_params);
// size_t size_uvm_semaphore         = sizeof(struct uvm_alloc_semaphore_pool_params);
//
// size_t size_nvkvm_hdr             = sizeof(struct nvkvm_hdr);
// size_t size_nvkvm_req_open        = sizeof(struct nvkvm_req_open);
// size_t size_nvkvm_req_close       = sizeof(struct nvkvm_req_close);
// size_t size_nvkvm_req_ioctl       = sizeof(struct nvkvm_req_ioctl);
// size_t size_nvkvm_req_mmap        = sizeof(struct nvkvm_req_mmap);
// size_t size_nvkvm_req_munmap      = sizeof(struct nvkvm_req_munmap);
// size_t size_nvkvm_resp_open       = sizeof(struct nvkvm_resp_open);
// size_t size_nvkvm_resp_ioctl      = sizeof(struct nvkvm_resp_ioctl);
// size_t size_nvkvm_resp_mmap       = sizeof(struct nvkvm_resp_mmap);
// size_t size_nvkvm_shm_ctrl        = sizeof(struct nvkvm_shm_ctrl);
import "C"

import (
	"testing"
)

// sizeCase describes a struct whose size must match a known constant.
// Known sizes are taken from gVisor's nvgpu package and open-gpu-kernel-modules
// headers as of driver version 535.
type sizeCase struct {
	name     string
	gotSize  uintptr
	wantSize uintptr
}

func checkSizes(t *testing.T, cases []sizeCase) {
	t.Helper()
	for _, tc := range cases {
		if tc.gotSize != tc.wantSize {
			t.Errorf("%-45s: size=%d, want=%d (delta=%+d)",
				tc.name, tc.gotSize, tc.wantSize,
				int(tc.gotSize)-int(tc.wantSize))
		}
	}
}

// TestFrontendStructSizes verifies that our C struct definitions for the
// NVIDIA frontend ioctl parameter structs are the correct size.
//
// These sizes have been stable since ~R525. If they drift, ioctl forwarding
// will silently pass the wrong amount of data to the host driver.
//
// Sizes sourced from:
//   - gVisor pkg/abi/nvgpu/frontend.go (SizeBytes() calls)
//   - open-gpu-kernel-modules src/common/sdk/nvidia/inc/nvos.h
func TestFrontendStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		// Core RM parameter structs
		{"nvos21_parameters (RM_ALLOC v1)", uintptr(C.size_nvos21), 28},
		{"nvos64_parameters (RM_ALLOC v2)", uintptr(C.size_nvos64), 48},
		{"nvos00_parameters (RM_FREE)", uintptr(C.size_nvos00), 16},
		{"nvos54_parameters (RM_CONTROL)", uintptr(C.size_nvos54), 32},
		{"nvos55_parameters (RM_DUP_OBJECT)", uintptr(C.size_nvos55), 36},
		{"nvos57_parameters (RM_SHARE)", uintptr(C.size_nvos57), 16},
		{"nvos32_parameters (RM_VID_HEAP_CONTROL)", uintptr(C.size_nvos32), 88},
		{"nvos46_parameters (RM_MAP_MEMORY_DMA)", uintptr(C.size_nvos46), 40},
		{"nvos47_parameters (RM_UNMAP_MEMORY_DMA)", uintptr(C.size_nvos47), 32},

		// Card info and system
		{"nv_ioctl_card_info", uintptr(C.size_nv_ioctl_card_info), 80},
		{"nv_ioctl_register_fd", uintptr(C.size_nv_ioctl_register_fd), 4},
		{"nv_ioctl_alloc_os_event", uintptr(C.size_nv_ioctl_alloc_os_ev), 16},
		{"nv_ioctl_free_os_event", uintptr(C.size_nv_ioctl_free_os_ev), 16},
		{"nv_ioctl_rm_api_version", uintptr(C.size_nv_ioctl_rm_api_ver), 72},
		{"nv_ioctl_sys_params", uintptr(C.size_nv_ioctl_sys_params), 8},
		{"nv_ioctl_numa_info", uintptr(C.size_nv_ioctl_numa_info), 40},
		{"nv_ioctl_wait_open_complete", uintptr(C.size_nv_ioctl_wait_open), 8},

		// Memory management
		{"nv_ioctl_nvos33_parameters_with_fd (MAP_MEMORY)", uintptr(C.size_nv_map_memory_fd), 56},
		{"nv_ioctl_nvos34_parameters (UNMAP_MEMORY)", uintptr(C.size_nv_unmap_memory), 40},
		{"nv_ioctl_nvos02_parameters_with_fd (ALLOC_MEMORY)", uintptr(C.size_nv_alloc_memory_fd), 56},
		{"nv_ioctl_idle_channels", uintptr(C.size_nv_idle_channels), 40},
		{"nv_ioctl_alloc_context_dma2", uintptr(C.size_nv_alloc_ctx_dma2), 64},
		{"nv_ioctl_export_to_dmabuf_fd", uintptr(C.size_nv_export_dmabuf), 40},
	})
}

// TestUVMStructSizes verifies that UVM ioctl parameter structs are correctly
// sized. These are more variable across driver versions than frontend structs.
func TestUVMStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		{"uvm_initialize_params", uintptr(C.size_uvm_init), 16},
		{"uvm_deinitialize_params", uintptr(C.size_uvm_deinit), 8},
		{"uvm_mm_initialize_params", uintptr(C.size_uvm_mm_init), 24},
		{"uvm_register_gpu_params", uintptr(C.size_uvm_reg_gpu), 32},
		{"uvm_unregister_gpu_params", uintptr(C.size_uvm_unreg_gpu), 24},
		{"uvm_register_gpu_vaspace_params", uintptr(C.size_uvm_reg_gpu_vaspace), 32},
		{"uvm_unregister_gpu_vaspace_params", uintptr(C.size_uvm_unreg_gpu_vaspace), 24},
		{"uvm_register_channel_params", uintptr(C.size_uvm_reg_channel), 56},
		{"uvm_unregister_channel_params", uintptr(C.size_uvm_unreg_channel), 40},
		{"uvm_create_range_group_params", uintptr(C.size_uvm_create_rg), 16},
		{"uvm_destroy_range_group_params", uintptr(C.size_uvm_destroy_rg), 16},
		{"uvm_set_range_group_params", uintptr(C.size_uvm_set_rg), 32},
		{"uvm_free_params", uintptr(C.size_uvm_free), 24},
		{"uvm_migrate_params", uintptr(C.size_uvm_migrate), 48},
		{"uvm_set_preferred_location_params", uintptr(C.size_uvm_set_pref_loc), 40},
		{"uvm_unset_preferred_location_params", uintptr(C.size_uvm_unset_pref_loc), 24},
		{"uvm_set_accessed_by_params", uintptr(C.size_uvm_set_accessed_by), 40},
		{"uvm_unset_accessed_by_params", uintptr(C.size_uvm_unset_accessed_by), 40},
		{"uvm_enable_peer_access_params", uintptr(C.size_uvm_peer_access), 40},
		{"uvm_create_external_range_params", uintptr(C.size_uvm_create_ext_range), 24},
		{"uvm_validate_va_range_params", uintptr(C.size_uvm_validate_va), 24},
		{"uvm_pageable_mem_access_params", uintptr(C.size_uvm_pageable), 8},
		{"uvm_alloc_semaphore_pool_params", uintptr(C.size_uvm_semaphore), 32},
	})
}

// TestProtocolStructSizes verifies the wire protocol structs are the expected
// size. These are critical for the guest/host ABI — both sides must agree.
func TestProtocolStructSizes(t *testing.T) {
	checkSizes(t, []sizeCase{
		{"nvkvm_hdr (must be exactly 8)", uintptr(C.size_nvkvm_hdr), 8},
		{"nvkvm_req_open", uintptr(C.size_nvkvm_req_open), 8},
		{"nvkvm_req_close", uintptr(C.size_nvkvm_req_close), 8},
		{"nvkvm_req_ioctl (must be exactly 32)", uintptr(C.size_nvkvm_req_ioctl), 32},
		{"nvkvm_req_mmap", uintptr(C.size_nvkvm_req_mmap), 32},
		{"nvkvm_req_munmap", uintptr(C.size_nvkvm_req_munmap), 8},
		{"nvkvm_resp_open", uintptr(C.size_nvkvm_resp_open), 8},
		{"nvkvm_resp_ioctl", uintptr(C.size_nvkvm_resp_ioctl), 16},
		{"nvkvm_resp_mmap", uintptr(C.size_nvkvm_resp_mmap), 24},
		{"nvkvm_shm_ctrl (must fit in one page)", uintptr(C.size_nvkvm_shm_ctrl), 80},
	})
}

// TestIoctlNumberConstants verifies that our ioctl number constants are
// consistent with each other (they must all fit in the expected bit ranges).
func TestIoctlNumberConstants(t *testing.T) {
	cases := []struct {
		name string
		nr   int
	}{
		{"NV_ESC_CARD_INFO",            int(C.NV_ESC_CARD_INFO)},
		{"NV_ESC_REGISTER_FD",          int(C.NV_ESC_REGISTER_FD)},
		{"NV_ESC_ALLOC_OS_EVENT",       int(C.NV_ESC_ALLOC_OS_EVENT)},
		{"NV_ESC_FREE_OS_EVENT",        int(C.NV_ESC_FREE_OS_EVENT)},
		{"NV_ESC_CHECK_VERSION_STR",    int(C.NV_ESC_CHECK_VERSION_STR)},
		{"NV_ESC_SYS_PARAMS",           int(C.NV_ESC_SYS_PARAMS)},
		{"NV_ESC_NUMA_INFO",            int(C.NV_ESC_NUMA_INFO)},
		{"NV_ESC_WAIT_OPEN_COMPLETE",   int(C.NV_ESC_WAIT_OPEN_COMPLETE)},
		{"NV_ESC_RM_FREE",              int(C.NV_ESC_RM_FREE)},
		{"NV_ESC_RM_CONTROL",           int(C.NV_ESC_RM_CONTROL)},
		{"NV_ESC_RM_ALLOC",             int(C.NV_ESC_RM_ALLOC)},
		{"NV_ESC_RM_DUP_OBJECT",        int(C.NV_ESC_RM_DUP_OBJECT)},
		{"NV_ESC_RM_MAP_MEMORY",        int(C.NV_ESC_RM_MAP_MEMORY)},
		{"NV_ESC_RM_UNMAP_MEMORY",      int(C.NV_ESC_RM_UNMAP_MEMORY)},
	}

	for _, tc := range cases {
		if tc.nr <= 0 || tc.nr > 0xFF {
			t.Errorf("%-30s: ioctl NR %d out of [1,255] range", tc.name, tc.nr)
		}
	}
}

// TestUVMIoctlNumbersDistinct verifies that no two UVM command numbers collide.
func TestUVMIoctlNumbersDistinct(t *testing.T) {
	cmds := map[uint32]string{
		uint32(C.UVM_INITIALIZE):           "UVM_INITIALIZE",
		uint32(C.UVM_DEINITIALIZE):         "UVM_DEINITIALIZE",
		uint32(C.UVM_CREATE_RANGE_GROUP):   "UVM_CREATE_RANGE_GROUP",
		uint32(C.UVM_DESTROY_RANGE_GROUP):  "UVM_DESTROY_RANGE_GROUP",
		uint32(C.UVM_REGISTER_GPU_VASPACE): "UVM_REGISTER_GPU_VASPACE",
		uint32(C.UVM_UNREGISTER_GPU_VASPACE): "UVM_UNREGISTER_GPU_VASPACE",
		uint32(C.UVM_REGISTER_CHANNEL):     "UVM_REGISTER_CHANNEL",
		uint32(C.UVM_UNREGISTER_CHANNEL):   "UVM_UNREGISTER_CHANNEL",
		uint32(C.UVM_REGISTER_GPU):         "UVM_REGISTER_GPU",
		uint32(C.UVM_UNREGISTER_GPU):       "UVM_UNREGISTER_GPU",
		uint32(C.UVM_FREE):                 "UVM_FREE",
		uint32(C.UVM_MIGRATE):              "UVM_MIGRATE",
		uint32(C.UVM_MM_INITIALIZE):        "UVM_MM_INITIALIZE",
	}
	// If the map was constructed without collision the count is correct
	if len(cmds) != 13 {
		t.Errorf("UVM command number collision detected: map has %d entries, expected 13", len(cmds))
	}
}
