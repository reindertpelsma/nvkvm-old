# Milestones

## v0.1 — first LLM inference through nvkvm (2026-05-28, eb6e16f)

End-to-end CUDA pipeline working:

- `cuInit / cuDeviceGet / cuCtxCreate / cuMemAlloc / cuMemcpyHtoD / cuMemcpyDtoH` — all green
- `cuMemcpy` round-trip byte-exact up to **256 MB**
- `cuLaunchKernel` — `vector_add` and **1024×1024 fp32 GEMM** (1B FMAs, CPU-verified)
- `cuModuleLoadData` — PTX JIT works (libnvidia-ptxjitcompiler must match libcuda version)
- Multi-process within one VM boot — cumemalloc + vec_add + matmul interleaved
- **Qwen2.5-0.5B-Instruct GGUF via llama.cpp** answers "Q: What is the capital of France?" → "Paris"
  - 298 tok/s prompt, 29.6 tok/s generation
  - all 25 layers offloaded to GPU (`-ngl 99`)
  - built with `-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86` on guest RTX 3060

Host environment: vast.ai RTX 3060, NVIDIA open kmd 575.51.03, Ubuntu 22.04 + KVM/QEMU + nvkvm-guest.ko in an Ubuntu 24.04 guest VM. No driver patches required for production runtime (debug printks are stripped before release).

## Known issues at v0.1

- Security audit (2026-05-28) flagged cross-session handle-table reach and stub seccomp disabled-for-debug. Tracked separately; functional path is solid, multi-tenant claim is not yet honest. See memory/security_audit_2026_05_28.md.
