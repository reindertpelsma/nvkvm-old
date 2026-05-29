/*
 * nvkvm_ctrl_allowlist.h — default-deny RM control-command allowlist (#76).
 *
 * Generated, not hand-written. Provenance:
 *   - gVisor nvproxy 575-ABI compUtil-tagged control cmds (the CUDA-compute
 *     surface; graphics/video/profiling/fabric-only rows EXCLUDED) —
 *     docs/audits/nvproxy_control_allowlist.md
 *   - UNION our empirically-observed known-good set (cuInit/matmul/vec_add/
 *     big_memcpy/ioctl_fwd/nvidia-smi) — docs/audits/empirical_control_cmds.md
 *   - PLUS NV0080_CTRL_CMD_GPU_GET_VGX_CAPS (0x0080028e) and _GET_BRAND_CAPS
 *     (0x00800294): benign read-only caps queries libcuda issues at init that
 *     nvproxy's app never exercised.
 *
 * Two RULE-BASED passthroughs are handled in code, NOT this table (they cover
 * future cmds we haven't observed): GSP-legacy mask (cmd & 0x8000) and the
 * NV2081_BINAPI class ((cmd >> 16) == 0x2081) — both GSP-routed, no app
 * pointers. A 1 MiB inner-params size cap is also enforced in code.
 *
 * Anything not matched here or by those rules is DENIED (NV_ERR_NOT_SUPPORTED),
 * matching nvproxy's posture. This is a HOST/cross-VM attack-surface control
 * (reg-ops/HWPM/debug/fabric/power fall out automatically); it lives in QEMU
 * because the guest kernel module is untrusted.
 */
#ifndef NVKVM_CTRL_ALLOWLIST_H
#define NVKVM_CTRL_ALLOWLIST_H
#include <stdint.h>

static const uint32_t nvkvm_ctrl_allowlist[] = {
	0x00000101u,
	0x00000102u,
	0x00000127u,
	0x0000012bu,
	0x00000136u,
	0x0000013au,
	0x000001f0u,
	0x00000201u,
	0x00000202u,
	0x00000204u,
	0x00000205u,
	0x00000214u,
	0x00000215u,
	0x00000216u,
	0x0000021bu,
	0x00000275u,
	0x00000279u,
	0x0000027bu,
	0x00000288u,
	0x00000289u,
	0x00000290u,
	0x00000a04u,
	0x00000d01u,
	0x00000d04u,
	0x00410110u,
	0x00800201u,
	0x00800280u,
	0x00800288u,
	0x00800289u,
	0x0080028bu,
	0x0080028eu,
	0x00800292u,
	0x00800294u,
	0x00801307u,
	0x00801402u,
	0x0080170du,
	0x00801806u,
	0x0080180du,
	0x00801909u,
	0x00de0001u,
	0x00f80103u,
	0x00fd0101u,
	0x00fd0102u,
	0x00fd0104u,
	0x00fd0105u,
	0x20800102u,
	0x20800110u,
	0x20800111u,
	0x20800119u,
	0x2080012fu,
	0x20800131u,
	0x20800133u,
	0x2080013fu,
	0x20800142u,
	0x20800145u,
	0x20800146u,
	0x2080014au,
	0x2080014bu,
	0x20800156u,
	0x20800157u,
	0x20800170u,
	0x2080018bu,
	0x2080018du,
	0x2080018eu,
	0x20800195u,
	0x208001a3u,
	0x20800301u,
	0x20800403u,
	0x20800406u,
	0x20800407u,
	0x20800513u,
	0x20800802u,
	0x2080110bu,
	0x20801201u,
	0x20801210u,
	0x20801218u,
	0x2080121bu,
	0x20801227u,
	0x2080122au,
	0x2080122bu,
	0x20801230u,
	0x20801303u,
	0x20801357u,
	0x20801358u,
	0x20801701u,
	0x20801702u,
	0x20801801u,
	0x20801802u,
	0x20801803u,
	0x20801823u,
	0x2080182au,
	0x2080182bu,
	0x2080200au,
	0x20802068u,
	0x20802209u,
	0x2080220cu,
	0x20802210u,
	0x20802a03u,
	0x20802a0au,
	0x20803001u,
	0x20803002u,
	0x20803125u,
	0x20803601u,
	0x20803801u,
	0x20808159u,
	0x20808162u,
	0x2080852eu,
	0x2080852fu,
	0x2080a612u,
	0x2080a618u,
	0x20810108u,
	0x208f1105u,
	0x503c0102u,
	0x503c0104u,
	0x503c0105u,
	0x83de0309u,
	0x83de030cu,
	0x83de0310u,
	0x906f0101u,
	0x906f0102u,
	0x90e60102u,
	0xa06c0101u,
	0xa06c0103u,
	0xa06c0105u,
	0xa06f0103u,
	0xc36f0108u,
	0xc56f010bu,
	0xcb330101u,
	0xcb330104u,
	0xcb33010bu,
	0xcb33010cu,
};
#define NVKVM_CTRL_ALLOWLIST_N \
	(sizeof(nvkvm_ctrl_allowlist) / sizeof(nvkvm_ctrl_allowlist[0]))

#endif /* NVKVM_CTRL_ALLOWLIST_H */
