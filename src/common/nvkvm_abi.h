/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_abi.h — per-driver-version ABI profile (multi-driver support, #81).
 *
 * The NVIDIA RM/UVM ABI is mostly stable across open-driver versions, but a
 * handful of struct sizes/offsets change at known version boundaries (mirrors
 * gVisor nvproxy's version-keyed tables; see docs/audits/abi_profile_spec.md).
 * Rather than hardcode the 575 layout, every version-variant site consults a
 * profile selected from the host driver's major version.
 *
 * Shared by all three components (guest kernel module, QEMU device, freestanding
 * stub), so it uses only plain ints + static inlines — no libc, no kernel-only
 * or QEMU-only API.
 *
 * Selection is deterministic from the version MAJOR, so QEMU and the guest
 * independently parse the same `NV_ESC_CHECK_VERSION_STR` string and arrive at
 * the same profile; QEMU additionally stamps the profile id into each
 * ISOLATE_CMD_IOCTL (the `reserved` field) so the stub uses the matching
 * offsets without parsing anything.
 *
 * Values verified against the open-gpu-kernel-modules / nvproxy structs for
 * each family.  The 570 profile is byte-identical to nvkvm's historical 575
 * hardcodes (575 shares 570's layouts), so existing 575 behavior is unchanged.
 */
#ifndef NVKVM_ABI_H
#define NVKVM_ABI_H

enum nvkvm_abi_id {
	NVKVM_ABI_535 = 535,   /* LTSB baseline: pre-V550 UVM, base channel/vaspace */
	NVKVM_ABI_570 = 570,   /* == 575 layouts: V550 UVM, V570 channel, pre-580 */
	NVKVM_ABI_580 = 580,   /* V580 VASPACE + V580 NVOS46 (each +8 bytes)        */
};

struct nvkvm_abi_profile {
	unsigned id;                 /* enum nvkvm_abi_id                         */

	/* UVM ioctl param sizes / embedded-fd offset (V550 grew the per-GPU
	 * attribute array from 1 to 256 entries → +9180 bytes). */
	unsigned uvm_map_ext_size;   /* UVM_MAP_EXTERNAL_ALLOCATION params size   */
	unsigned uvm_map_ext_fd_off; /* rm_ctrl_fd offset inside that struct      */
	unsigned uvm_sem_pool_size;  /* UVM_ALLOC_SEMAPHORE_POOL params size      */

	/* RM_ALLOC class-specific alloc-param sizes (the guest forwards exactly
	 * this many bytes of libcuda's p_alloc_parms). */
	unsigned chan_alloc_size;    /* {TURING,AMPERE,HOPPER}_CHANNEL_GPFIFO_A   */
	unsigned vaspace_alloc_size; /* FERMI_VASPACE_A                           */
	unsigned mem_alloc_size;     /* NV50_MEMORY_VIRTUAL / LOCAL_USER / SYSTEM */
	unsigned nv00de_alloc_size;  /* RM_USER_SHARED_DATA                       */

	/* Frontend NVOS46 (NV_ESC_RM_MAP_MEMORY_DMA, NR 0x57): V580 grew it by 8
	 * (Flags2 + KindOverride), moving the status field. */
	unsigned nvos46_size;        /* NVOS46 total size                         */
	unsigned nvos46_status_off;  /* offset of the status u32 in NVOS46        */
};

/* Profile table.  Index by enum; keep all compiled in (nvproxy-style). */
static const struct nvkvm_abi_profile nvkvm_abi_profiles[] = {
	{
		.id = NVKVM_ABI_535,
		.uvm_map_ext_size = 84,    .uvm_map_ext_fd_off = 68,
		.uvm_sem_pool_size = 68,
		.chan_alloc_size = 360,    /* base NV_CHANNEL_ALLOC_PARAMS (no TPCConfigID) */
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 128,     /* base ≈ V545 here; 535 not HW-validated */
		.nv00de_alloc_size = 8,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		.id = NVKVM_ABI_570,       /* == 575 (current default) */
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 368,    /* NV_CHANNEL_ALLOC_PARAMS_V570 */
		.vaspace_alloc_size = 48,
		.mem_alloc_size = 128,     /* NV_MEMORY_ALLOCATION_PARAMS_V545 */
		.nv00de_alloc_size = 8,
		.nvos46_size = 56,         .nvos46_status_off = 48,
	},
	{
		.id = NVKVM_ABI_580,
		.uvm_map_ext_size = 9264,  .uvm_map_ext_fd_off = 9248,
		.uvm_sem_pool_size = 9248,
		.chan_alloc_size = 368,
		.vaspace_alloc_size = 56,  /* NV_VASPACE_ALLOCATION_PARAMETERS_V580 (+Pasid) */
		.mem_alloc_size = 128,
		.nv00de_alloc_size = 8,
		.nvos46_size = 64,         .nvos46_status_off = 56, /* NVOS46_V580 (+Flags2,KindOverride) */
	},
};

/* Parse the leading integer (major) of an "MMM.mm.pp" version string. */
static inline unsigned nvkvm_abi_parse_major(const char *vs)
{
	unsigned m = 0;
	if (!vs)
		return 0;
	while (*vs >= '0' && *vs <= '9') {
		m = m * 10u + (unsigned)(*vs - '0');
		vs++;
	}
	return m;
}

/* Map a profile id to its table entry; defaults to 570 (== 575). */
static inline const struct nvkvm_abi_profile *nvkvm_abi_by_id(unsigned id)
{
	unsigned i;
	for (i = 0; i < sizeof(nvkvm_abi_profiles) / sizeof(nvkvm_abi_profiles[0]); i++)
		if (nvkvm_abi_profiles[i].id == id)
			return &nvkvm_abi_profiles[i];
	return &nvkvm_abi_profiles[1]; /* 570/575 default */
}

/* Map a host driver major version to a profile id. */
static inline unsigned nvkvm_abi_id_for_major(unsigned major)
{
	if (major <= 565)
		return NVKVM_ABI_535;   /* pre-V550 families fold onto the 535 baseline */
	if (major >= 580)
		return NVKVM_ABI_580;
	return NVKVM_ABI_570;           /* 570 / 575 */
}

/* Convenience: profile from a version string. */
static inline const struct nvkvm_abi_profile *nvkvm_abi_for_version(const char *vs)
{
	return nvkvm_abi_by_id(nvkvm_abi_id_for_major(nvkvm_abi_parse_major(vs)));
}

#endif /* NVKVM_ABI_H */
