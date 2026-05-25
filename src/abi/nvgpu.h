/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvgpu.h — Core NVIDIA GPU ABI types
 *
 * Ported from gVisor pkg/abi/nvgpu/nvgpu.go with additions from the
 * open-gpu-kernel-modules headers.
 *
 * These structs describe the kernel-driver ABI for /dev/nvidia*, /dev/nvidiactl
 * and /dev/nvidia-uvm. They must not be modified unless the corresponding
 * driver version support in nvkvm_dispatch.c is also updated.
 */

#ifndef NVGPU_H
#define NVGPU_H

#include <linux/types.h>

/* ── Device numbers ──────────────────────────────────────────────────────── */

#define NV_MAJOR_DEVICE_NUMBER                  195
#define NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE   255
#define NV_MINOR_DEVICE_NUMBER_REGULAR_MAX      15
#define NVIDIA_UVM_PRIMARY_MINOR_NUMBER         0

/* ── Core handle types ───────────────────────────────────────────────────── */

typedef __u32 nvhandle_t;     /* NvHandle — opaque RM object handle  */
typedef __u32 nvclassid_t;    /* NvU32 class ID                      */
typedef __u64 nvp64_t;        /* NvP64 — 64-bit pointer-as-integer   */

#define NV01_NULL_OBJECT  0x00000000U

/* ── RM status codes ─────────────────────────────────────────────────────── */

#define NV_OK                   0x00000000U
#define NV_ERR_NOT_SUPPORTED    0x00000057U

/* ── Object class IDs ────────────────────────────────────────────────────── */

/* Root / client — libnvidia-ml uses class=0 (NV01_ROOT) for root allocation,
 * older apps may use NV01_ROOT_CLIENT=0x41.  Both create a root client. */
#define NV01_ROOT                           0x00000000U  /* same as NV01_NULL_OBJECT */
#define NV01_ROOT_CLIENT                    0x00000041U
/* Devices */
#define NV01_DEVICE_0                       0x00000080U
/* Subdevice */
#define NV20_SUBDEVICE_0                    0x00002080U
/* VA space */
#define FERMI_VASPACE_A                     0x000090F1U
/* Usermode */
#define TURING_USERMODE_A                   0x0000C461U
#define AMPERE_USERMODE_A                   0x0000C561U
#define HOPPER_USERMODE_A                   0x0000C661U
/* Channel groups */
#define KEPLER_CHANNEL_GROUP_A              0x0000A06CU
/* GPFIFO channels */
#define TURING_CHANNEL_GPFIFO_A             0x0000C46FU
#define AMPERE_CHANNEL_GPFIFO_A             0x0000C56FU
#define HOPPER_CHANNEL_GPFIFO_A             0x0000C86FU
/* Compute objects */
#define TURING_COMPUTE_A                    0x0000C4B1U
#define AMPERE_COMPUTE_A                    0x0000C6B1U
#define HOPPER_COMPUTE_A                    0x0000CBB1U
/* DMA copy */
#define TURING_DMA_COPY_A                   0x0000C4B5U
#define AMPERE_DMA_COPY_A                   0x0000C6B5U
#define HOPPER_DMA_COPY_A                   0x0000CBB5U
/* Memory classes */
#define NV01_MEMORY_SYSTEM                  0x0000003DU
#define NV01_MEMORY_LOCAL_USER              0x00000040U
#define NV01_MEMORY_SYSTEM_OS_DESCRIPTOR    0x00000071U
#define NV50_MEMORY_VIRTUAL                 0x000050A0U
/* Events */
#define NV01_EVENT_OS_EVENT                 0x00000079U
/* Context DMA */
#define NV01_CONTEXT_DMA                    0x00000002U
/* Subcontext */
#define FERMI_CONTEXT_SHARE_A               0x00009067U

/* ── RS_ACCESS_MASK ──────────────────────────────────────────────────────── */

struct rs_access_mask {
	__u32 limbs[1];    /* variable length; here just 1 limb for simplicity */
};

/* ── NV_ESC_RM_ALLOC parameter structs ───────────────────────────────────── */

/*
 * NVOS21_PARAMETERS — used when hRightsRequested == 0 / no access mask.
 * From src/common/sdk/nvidia/inc/nvos.h.
 */
struct nvos21_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	nvp64_t    p_alloc_parms;   /* pointer to class-specific alloc struct */
	__u32      status;
	__u32      _pad;            /* trailing alignment pad — sizeof == 32 on x86-64 */
};

/*
 * NVOS64_PARAMETERS — used when hRightsRequested != NULL (newer drivers).
 *
 * Field order matches NVIDIA open-gpu-kernel-modules nvos.h NVOS64_PARAMETERS:
 *   hRoot(4), hObjectParent(4), hObjectNew(4), hClass(4),
 *   pAllocParms(8), pRightsRequested(8),
 *   paramsSize(4), flags(4), status(4), reserved(4)
 * = 48 bytes total. (Cross-checked with gVisor pkg/abi/nvgpu/frontend.go.)
 */
struct nvos64_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	nvp64_t    p_alloc_parms;       /* pointer to class-specific alloc struct */
	nvp64_t    p_rights_requested;  /* pointer to RS_ACCESS_MASK (may be NULL) */
	__u32      alloc_parms_size;    /* size of alloc params buffer            */
	__u32      flags;
	__u32      status;
	__u32      reserved;
};

/* ── NV_ESC_RM_FREE ──────────────────────────────────────────────────────── */

struct nvos00_parameters {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_old;
	__u32      status;
};

/* ── NV_ESC_RM_CONTROL ───────────────────────────────────────────────────── */

struct nvos54_parameters {
	nvhandle_t h_client;
	nvhandle_t h_object;
	__u32      cmd;
	__u32      flags;
	nvp64_t    params;      /* pointer to command-specific param struct */
	__u32      params_size;
	__u32      status;
};

/* ── NV_ESC_RM_DUP_OBJECT ────────────────────────────────────────────────── */

struct nvos55_parameters {
	nvhandle_t h_client;
	nvhandle_t h_parent_client;
	nvhandle_t h_parent;
	nvhandle_t h_object;
	nvhandle_t h_client_src;
	nvhandle_t h_src_parent;
	nvhandle_t h_src_object;
	__u32      flags;
	__u32      status;
};

/* ── NV_ESC_RM_SHARE ─────────────────────────────────────────────────────── */

struct nvos57_parameters {
	nvhandle_t h_client;
	nvhandle_t h_object;
	__u32      share_policy;    /* RS_SHARE_POLICY */
	__u32      status;
};

/* ── NV_ESC_RM_ALLOC_MEMORY ─────────────────────────────────────────────── */

struct nv_ioctl_nvos02_parameters_with_fd {
	nvhandle_t h_root;
	nvhandle_t h_object_parent;
	nvhandle_t h_object_new;
	nvclassid_t h_class;
	__u32      flags;
	__u32      reserved;
	nvp64_t    p_memory;     /* host VA returned by driver           */
	__u64      limit;
	__u32      status;
	__s32      fd;           /* fd to associate with allocation      */
};

/* ── NV_ESC_RM_MAP_MEMORY ────────────────────────────────────────────────── */

struct nv_ioctl_nvos33_parameters_with_fd {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_memory;
	__u32      reserved0;
	__u64      offset;
	__u64      length;
	nvp64_t    p_linear_address;  /* returned: mapped VA             */
	__u32      status;
	__u32      flags;
	__s32      fd;
	__u32      reserved1;
};

/* ── NV_ESC_RM_UNMAP_MEMORY ──────────────────────────────────────────────── */

struct nv_ioctl_nvos34_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_memory;
	__u32      reserved0;
	nvp64_t    p_linear_address;  /* VA to unmap                     */
	__u32      status;
	__u32      flags;
};

/* ── NV_ESC_RM_VID_HEAP_CONTROL ──────────────────────────────────────────── */

struct nvos32_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_vab;
	nvhandle_t h_memory;
	__u32      function;
	__u32      ivcomp;
	__u32      client_type;
	__u32      reserved0;
	nvp64_t    p_memory;
	__u64      u_start;
	__u64      u_size;
	__u64      u_alignment;
	__u32      attr;
	__u32      attr2;
	__u32      format;
	__u32      comp_tag;
	__u32      flags;
	__u32      status;
};

/* ── NV_ESC_RM_MAP_MEMORY_DMA ────────────────────────────────────────────── */

struct nvos46_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_dma;
	nvhandle_t h_memory;
	__u64      offset;
	__u64      length;
	__u32      flags;
	__u32      status;
};

/* ── NV_ESC_RM_UNMAP_MEMORY_DMA ──────────────────────────────────────────── */

struct nvos47_parameters {
	nvhandle_t h_client;
	nvhandle_t h_device;
	nvhandle_t h_dma;
	nvhandle_t h_memory;
	__u32      flags;
	__u32      dma_offset;
	__u32      status;
	__u32      reserved;
};

/* ── NV_ESC_RM_IDLE_CHANNELS ─────────────────────────────────────────────── */

struct nv_ioctl_idle_channels {
	nvp64_t    p_clients;       /* pointer to array of client handles   */
	nvp64_t    p_devices;       /* pointer to array of device handles   */
	nvp64_t    p_channels;      /* pointer to array of channel handles  */
	__u32      num_channels;
	__u32      notify_clients;
	__u32      timeout_us;
	__u32      status;
};

/* ── NV_ESC_CARD_INFO ────────────────────────────────────────────────────── */

#define NV_IOCTL_CARD_INFO_BUS_TYPE_PCI  0x1
#define NV_IOCTL_CARD_INFO_MAX_ENTRIES   32

struct nv_pci_info {
	__u32 domain;
	__u8  bus;
	__u8  slot;
	__u8  function;
	__u8  reserved;
	__u16 vendor_id;
	__u16 device_id;
};

struct nv_ioctl_card_info {
	__u8            valid;
	__u8            reserved[3];
	struct nv_pci_info pci_info;
	__u32           gpu_id;
	__u16           interrupt_line;
	__u8            reserved2[2];
	__u64           reg_address;
	__u64           reg_size;
	__u64           fb_address;
	__u64           fb_size;
	__u32           minor_number;
	__u8            dev_name[14];  /* gVisor NvIoctlCardInfo.DevName is 14 bytes */
	__u8            reserved3[2];
	/* 4 bytes implicit trailing padding → sizeof == 80 on x86-64 */
};

/* ── NV_ESC_CHECK_VERSION_STR ────────────────────────────────────────────── */

#define NV_RM_API_VERSION_STRING_LENGTH  64

struct nv_ioctl_rm_api_version {
	__u32 cmd;
	__u32 reply;
	char  version_string[NV_RM_API_VERSION_STRING_LENGTH];
};

/* ── NV_ESC_SYS_PARAMS ───────────────────────────────────────────────────── */

struct nv_ioctl_sys_params {
	__u64 memory_size;
};

/* ── NV_ESC_ALLOC_OS_EVENT / NV_ESC_FREE_OS_EVENT ───────────────────────── */

struct nv_ioctl_alloc_os_event {
	nvhandle_t h_client;
	nvhandle_t h_device;
	__u32      fd;
	__u32      status;
};

struct nv_ioctl_free_os_event {
	nvhandle_t h_client;
	nvhandle_t h_device;
	__u32      fd;
	__u32      status;
};

/* ── NV_ESC_REGISTER_FD ──────────────────────────────────────────────────── */

struct nv_ioctl_register_fd {
	__s32 ctl_fd;    /* fd for /dev/nvidiactl to associate  */
};

/* ── NV_ESC_WAIT_OPEN_COMPLETE ───────────────────────────────────────────── */

struct nv_ioctl_wait_open_complete {
	__s32 rc;
	__u32 adapter_status;
};

/* ── NV_ESC_NUMA_INFO ────────────────────────────────────────────────────── */

struct nv_ioctl_numa_info {
	__s32  nid;
	__u32  status;
	__u64  memblock_size;
	__u64  numa_mem_addr;
	__u64  numa_mem_size;
};

/* ── NV_ESC_EXPORT_TO_DMABUF_FD ──────────────────────────────────────────── */

struct nv_ioctl_export_to_dmabuf_fd {
	nvhandle_t h_client;
	nvhandle_t h_memory;
	__u64      size;
	__u64      offset;
	__u32      map_type;
	__s32      fd;         /* result: dmabuf fd                     */
	__u32      b_allow_mmap;
	__u32      status;
};

/* ── NV_ESC_ALLOC_CONTEXT_DMA2 ───────────────────────────────────────────── */

struct nv_ioctl_alloc_context_dma2 {
	nvhandle_t h_client;
	nvhandle_t h_parent;
	nvhandle_t h_memory;
	nvclassid_t h_class;
	__u32      flags;
	__u32      attr;
	__u64      va_space;
	__u64      va_base;
	__u64      limit;
	nvhandle_t h_dma;
	__u32      status;
};

/* ── NV0080_ALLOC_PARAMETERS — alloc params for NV01_DEVICE_0 ────────────── */

struct nv0080_alloc_parameters {
	__u32      device_id;
	nvhandle_t h_client_share;
	nvhandle_t h_target_client;
	nvhandle_t h_target_device;
	__u32      flags;
	__u32      _pad0;
	__u64      va_space_size;
	__u64      va_start_internal;
	__u64      va_limit_internal;
	__u32      va_mode;
	__u32      _pad1;
};

/* ── NV2080_ALLOC_PARAMETERS — alloc params for NV20_SUBDEVICE_0 ─────────── */

struct nv2080_alloc_parameters {
	__u32      sub_device_id;
};

/* ── NV00DE_ALLOC_PARAMETERS — alloc params for RM_USER_SHARED_DATA (0xDE) ── */

#define RM_USER_SHARED_DATA 0x000000DEU

/* V545 layout (driver >= 545.23.06): a single uint64. Our target driver
 * 575.51.03 uses this layout. */
struct nv00de_alloc_parameters_v545 {
	__u64 polled_data_mask;
};

/* ── NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101) ────────────────────── */

#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION 0x00000101U

/* NV_DECLARE_ALIGNED(NvP64, 8) fields: 4-byte pad before each 8-byte pointer */
struct nv0000_ctrl_system_get_build_version_params {
	__u32    size_of_strings;
	__u32    _pad0;
	nvp64_t  p_driver_version_buffer;  /* out: e.g. "575.51.03" */
	nvp64_t  p_version_buffer;         /* out: numeric version   */
	nvp64_t  p_title_buffer;           /* out: display title     */
	__u32    changelist_number;        /* out */
	__u32    official_changelist_number; /* out */
};

/* ── Commands with embedded InfoList pointer (NvxxxCtrlXxxGetInfoParams) ── */
/*
 * These RM_CONTROL inner commands share a common preamble:
 *   uint32 info_list_size;  // offset 0
 *   uint32 _pad;            // offset 4
 *   nvp64  info_list;       // offset 8   (pointer to info_list_size * 8 bytes)
 *
 * Each entry is two uint32: (index, data). CUDA pre-fills the index fields and
 * reads the data fields after the call. The guest sanitizer must carry the
 * list contents through the aux slot since the host has no access to guest VAs.
 *
 * Pulled from gVisor pkg/abi/nvgpu/ctrl.go.
 */
#define NV0041_CTRL_CMD_GET_SURFACE_INFO  0x00410110U
#define NV0080_CTRL_CMD_GR_GET_INFO       0x00801104U
#define NV2080_CTRL_CMD_BIOS_GET_INFO     0x20800802U
#define NV2080_CTRL_CMD_GR_GET_INFO       0x20801201U
#define NV2080_CTRL_CMD_FB_GET_INFO       0x20801301U
#define NV2080_CTRL_CMD_BUS_GET_INFO      0x20801802U
#define NVXXX_CTRL_XXX_INFO_ENTRY_SIZE    8U

/* ── NV_ESC_RM_CONTROL command IDs used in tests ─────────────────────────── */

#define NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES 0x00800280U

struct nv0080_ctrl_gpu_get_num_subdevices_params {
	__u32 num_sub_devices;
};

/* ── NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE (0x800289) ──────────────── */

#define NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE 0x00800289U

#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_NONE            0U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_VGX             1U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST_VGPU       2U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST_VSGA       3U
#define NV0080_CTRL_GPU_VIRTUALIZATION_MODE_PASSTHROUGHGUEST 4U

struct nv0080_ctrl_gpu_get_virtualization_mode_params {
	__u32 virtualization_mode;
	__u32 b_is_grid_licensed;
};

/* ── NV2080_CTRL_CMD_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO (0x20800406) ─ */

#define NV2080_CTRL_CMD_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO 0x20800406U

/* ── NV2080_CTRL_CMD_MC_GET_ARCH_INFO (0x20801701) ──────────────────────── */

#define NV2080_CTRL_CMD_MC_GET_ARCH_INFO    0x20801701U

/* ── NV2080_CTRL_CMD_GPU_GET_GID_INFO (0x2080014a) ──────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_GID_INFO    0x2080014aU

/* ── NV2080_CTRL_CMD_GPU_GET_NAME_STRING (0x20800110) ────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_NAME_STRING 0x20800110U

/* ── NV2080_CTRL_CMD_GPU_GET_INFO_V2 (0x20800102) ───────────────────────── */

#define NV2080_CTRL_CMD_GPU_GET_INFO_V2     0x20800102U

/* ── Frontend ioctl numbers (IOC_NR portion only) ────────────────────────── */

#define NV_IOCTL_BASE                           200
#define NV_ESC_CARD_INFO                        (NV_IOCTL_BASE + 0)
#define NV_ESC_REGISTER_FD                      (NV_IOCTL_BASE + 1)
#define NV_ESC_ALLOC_OS_EVENT                   (NV_IOCTL_BASE + 6)
#define NV_ESC_FREE_OS_EVENT                    (NV_IOCTL_BASE + 7)
#define NV_ESC_CHECK_VERSION_STR                (NV_IOCTL_BASE + 10)
#define NV_ESC_ATTACH_GPUS_TO_FD                (NV_IOCTL_BASE + 12)
#define NV_ESC_SYS_PARAMS                       (NV_IOCTL_BASE + 14)
#define NV_ESC_NUMA_INFO                        (NV_IOCTL_BASE + 15)
#define NV_ESC_WAIT_OPEN_COMPLETE               (NV_IOCTL_BASE + 18)

#define NV_ESC_RM_ALLOC_MEMORY                  0x27
#define NV_ESC_RM_FREE                          0x29
#define NV_ESC_RM_CONTROL                       0x2a
#define NV_ESC_RM_ALLOC                         0x2b
#define NV_ESC_RM_DUP_OBJECT                    0x34
#define NV_ESC_RM_SHARE                         0x35
#define NV_ESC_RM_IDLE_CHANNELS                 0x41
#define NV_ESC_RM_VID_HEAP_CONTROL              0x4a
#define NV_ESC_RM_MAP_MEMORY                    0x4e
#define NV_ESC_RM_UNMAP_MEMORY                  0x4f
#define NV_ESC_RM_ALLOC_CONTEXT_DMA2            0x54
#define NV_ESC_RM_MAP_MEMORY_DMA                0x57
#define NV_ESC_RM_UNMAP_MEMORY_DMA              0x58
#define NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO    0x5e
#define NV_ESC_EXPORT_TO_DMABUF_FD              0x70

#endif /* NVGPU_H */
