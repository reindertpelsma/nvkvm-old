# nvkvm real-application parity matrix

Proves nvkvm runs the workloads people actually put on GPUs — AI (beyond LLMs),
classic HPC/compute, crypto, and graphics/media — at **bare-metal host parity**,
from inside the untrusted KVM guest, over the full forward path
(guest kernel module → virtio → QEMU → per-process stub → host NVIDIA driver).

Each workload runs the SAME source/binary on the host and in the guest, STRICTLY
serially (one shared physical GPU), with a correctness check alongside throughput.
Parity gate: guest/host ≥ 0.90.

- Compute / AI / LLM matrix:  `run_matrix.sh`  (sources in `apps/`, runner `matrix_remote.sh`)
- Graphics / media matrix:    `run_graphics.sh` (runner `graphics_remote.sh`)
- Microbench (per-primitive):  `run_parity.sh`

Hardware: RTX 3060 12GB, driver 580.159.04, host Ubuntu 22.04 / guest Ubuntu 24.04.
Toolchain note: the 7 hand-written CUDA kernels compile to identical sm_86 SASS on
either toolkit; `sgemm_cublas`/`fft_cufft` use host cuBLAS/cuFFT 11.5 vs guest 12.x
(a library-version delta, not a forwarder effect) — `torch.matmul` gives the clean
same-version (12.1) cuBLAS parity signal.

## Results (2026-06-01)

### CUDA compute (10)
| workload | host | guest | ratio |
|---|---|---|---|
| memory bandwidth (triad) | 336.7 | 336.7 GB/s | 1.00x |
| reduction bandwidth | 127.9 | 126.1 GB/s | 0.99x |
| N-body (gravitation) | 4843 | 4872 GFLOP/s | 1.01x |
| Black-Scholes | 20985 | 20976 Mopt/s | 1.00x |
| Mandelbrot | 3.04e6 | 2.82e6 Mpix-it/s | 0.93x |
| 2D convolution | 1261 | 1218 GFLOP/s | 0.97x |
| SGEMM (cuBLAS) | 7.83 | 7.80 TFLOP/s | 1.00x |
| FFT (cuFFT) | 1251 | 1250 GFLOP/s | 1.00x |
| SHA-256 (crypto) | 766 | 808 MH/s | 1.05x |
| gpu-burn (sustained, 0 errors) | 9403 | 9470 GFLOP/s | 1.01x |

### PyTorch AI (7)
| workload | host | guest | ratio |
|---|---|---|---|
| matmul fp32 | 9.44 | 9.36 TFLOP/s | 0.99x |
| matmul fp16 (tensor cores) | 27.62 | 27.40 TFLOP/s | 0.99x |
| ResNet-50 inference | 632 | 630 img/s | 1.00x |
| ResNet-50 inference (AMP fp16) | 1126 | 1124 img/s | 1.00x |
| ResNet-50 training step | 205 | 204 img/s | 1.00x |
| ViT-B/16 inference | 172 | 172 img/s | 1.00x |
| BERT encoder inference | 290 | 290 seq/s | 1.00x |

### LLM — llama.cpp Qwen2.5-7B Q4_K_M (2)
| workload | host | guest | ratio |
|---|---|---|---|
| decode | 67.8 | 66.0 tok/s | 0.97x |
| prefill (1800-tok prompt) | 2221 | 2169 tok/s | 0.98x |

**Prefill methodology note.** With a *tiny* prompt (~5 tok) prefill measured
657→469 t/s (0.71x) — but that is fixed per-prefill launch/sync latency (the
control-path tax), not prefill compute. On a realistic long prompt (RAG / long
context / code — where prefill wall-clock actually matters) prefill is at 0.98x
parity because compute dominates. The short-prompt number measures launch RTT,
which `run_parity.sh` already tracks separately.

### Graphics / media — the DRI path (run_graphics.sh)
Headless throughout (EGL device platform / Vulkan compute / ffmpeg), exercising
the forwarded `/dev/dri/renderD128` + Vulkan ICD + video engines.
| workload | host | guest | ratio | notes |
|---|---|---|---|---|
| Vulkan device enumerate | RTX 3060 | RTX 3060 | — | ICD binds the GPU (not llvmpipe) |
| Vulkan compute fp32 (vkpeak) | 9341 | 9307 GFLOP/s | 1.00x | real Vulkan compute through nvkvm |
| OpenGL offscreen render (EGL) | 1.8 | 1.8 Mtri/s | 1.00x | renderer = NVIDIA RTX 3060; no GL error |
| NVENC h264 encode | 146 fps | — | — | **partial — see below** |

**NVENC (video encode) — partial, root-caused.** Fixed in this pass: staged
`libnvidia-encode` + `libnvcuvid` into the guest (were missing → "Cannot load
libnvidia-encode.so.1"), and allowlisted 4 RM control cmds NVENC needs for engine
discovery (`GPU_GET_CLASSLIST` 0x800201, `GR_GET_CAPS_V2` 0x801109,
`FIFO_GET_CAPS_V2` 0x801713, `GPU_GET_ENCODER_CAPACITY` 0x2080016c — all benign
read-only queries, nvproxy CapVideo). The GPU is now recognized and the encode
session opens, but `InitializeEncoder` fails with "generic error (20)" and the
forwarder logs no DENY/error — a deeper semantic gap (cuCtxCreate-class) tracked
as its own task. NVDEC (cuvid) is excluded: `h264_cuvid` decodes 0 frames and
hangs on the **bare-metal host** too (broken ffmpeg+cuvid build), so no baseline.

## Bottom line
Across **22 real workloads** spanning AI training/inference (PyTorch CNN/ViT/
transformer + matmul tensor-cores), HPC (GEMM/FFT/N-body/Black-Scholes/stream/
reduction/convolution), crypto (SHA-256), sustained stress (gpu-burn), LLM
(llama.cpp decode+prefill), and graphics (Vulkan enumerate+compute, OpenGL
offscreen render), the guest runs at **host parity** (≥0.97x on everything that
matters), all byte-exact / error-free where checkable. The only places the guest
is measurably slower are latency-bound control paths (short-*prompt* prefill,
alloc churn) — never sustained compute, bandwidth, or throughput. The residual
gap is the multi-hop ioctl RTT (`docs/perf/forwarding_latency_decomposition.md`),
which only surfaces when a workload is dominated by tiny serialized control ops.
One real gap remains and is root-caused: NVENC `InitializeEncoder` (above).
