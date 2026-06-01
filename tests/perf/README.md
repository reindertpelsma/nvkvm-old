# tests/perf — host-vs-guest parity harness (PLAN + building blocks)

Goal: a single repeatable runner that proves nvkvm runs real workloads at host
parity, with correct methodology baked in so we never again chase a phantom gap
(see the 2026-06-01 corrections in ../../docs/perf/forwarding_latency_decomposition.md).

## Building blocks already here (committed 2026-06-01)
- `launchstorm.c`     — separates pipelined submit (A) vs launch+sync RT (B) vs
                        empty cuCtxSynchronize (C). Driver API, embedded empty PTX.
- `cuda_api_prof.cpp` — LD_PRELOAD interposer over the CUDA *runtime* API
                        (cudaLaunchKernel/StreamSync/MemcpyAsync...), cycle-accurate,
                        buckets memcpy by kind×size. Build: `g++ -shared -fPIC -O2
                        -I/usr/local/cuda-12.0/include cuda_api_prof.cpp -o x.so -ldl`.
                        Use on real apps: `LD_PRELOAD=x.so <app>`.
- `htod_probe.c`      — HtoD by fresh-vs-reused source, anon-vs-file-backed.
- `dtoh_probe.c`      — DtoH cold/warm/new-buffer + byte-exact check.
(`sp_pingpong.c` predates this; transport ping-pong.)

## Methodology the harness MUST enforce (lessons paid for, do not drop)
1. Capture the HOST baseline in the SAME run — never compare to a remembered number.
2. Steady-state sampling only. For decode/LLM, GPU util must be sampled DURING
   generation, not model load (load shows ~0% GPU; that mistake faked a "14x gap").
3. Guest RAM >= model size + overhead, or model-load HtoD is disk-bound (the 17x
   was guest -m 4G < 4.4GB model; now -m 16G). Assert free RAM at start.
4. Byte-exact correctness alongside throughput (DtoH/HtoD/matmul), not just speed.
5. Warm caches before timing (gguf into page cache; one warm-up copy before loops).
6. Each metric asserts a threshold vs the host run -> regression FAILS loudly.

## Reference numbers (RTX 3060, driver 580, this stack, 2026-06-01) = thresholds
- LLM decode (Qwen2.5-7B Q4_K_M, -ngl 99): guest 64-65 t/s vs host 68 t/s (~95%),
  both 95-98% GPU util / 100% mem during decode.
- matmul 1024^2 fp32: PASS (correctness); ~parity GFLOP/s.
- HtoD large reused: ~12 GB/s guest vs ~13 host. DtoH warm (cached fix): ~8.6 GB/s
  guest vs ~9.7 host. (Pageable DtoH was 0.07 before #94 — guard against regress.)
- empty cuCtxSynchronize: ~0.37us guest (post WB-sysmem) vs 0.36 host.
- model-load HtoD (warm cache, 16G): ~12.5 GB/s vs ~14 host.
- alloc/launch: launch ~3x, alloc tax — bounded; record current as baseline.

## Harness shape (to build in the fresh session)
`tests/perf/run_parity.sh` (or a small C/py driver): ssh the SAME workload to host
(root@vh) and guest (ssh -p 2222 ubuntu@localhost), capture both, check correctness,
print a parity table with PASS/FAIL vs thresholds above. Reuse iter.sh's ssh
patterns and scripts/run_test_vm.sh (now -m 16G). Then point cuda_api_prof.so at a
real-app matrix: llama.cpp (have), then PyTorch / a CUDA sample / vLLM / SD.
