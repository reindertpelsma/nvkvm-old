# Mode-2 cuCtxCreate — Resume / Handoff Doc (2026-06-07)

A fresh Claude can resume the Mode-2 `cuCtxCreate` work from this doc alone. It captures the
whole arc of the recent sessions, the current blocker, the diagnostic infrastructure, and the
concrete next step. Repo is accessible; this doc embeds the essential out-of-repo memory facts.

---

## 0. The project in one paragraph

**Mode-2 goal:** run the *stock open NVIDIA driver* (580.159.04) inside a KVM/QEMU guest against a
*fully emulated* GA106 (`nvkvm-gpu-emul`, `src/qemu/nvkvm_gpu_emul.c`), and forward real compute to
the host RTX 3060 through **unprivileged** host ioctls (an "isolate" stub process). North star:
`cuInit` → `cuCtxCreate` → first compute (a matmul). Mode-1 (WSL2-style ioctl forwarding, the
shipping product) must never regress. The hard mechanism is **address virtualization**: the guest's
own `nvidia.ko` builds GPU page tables locally (its PDB lives in guest RAM / emulated vidmem;
`RM_MAP_MEMORY_DMA` is CPU-side and never GSP-forwarded), and QEMU must reconstruct those mappings on
the host so the host GPU executes the guest's channels.

**Standing constraints (do not violate):**
- Host-GPU forwarding is the ONLY supported Mode-2 mode — no pure-emulation path. Flags that enable
  forwarding (`m2fwd`, `m2exec`) default ON; keep only as a debug off-switch.
- Debug constructs must compile out for prod (`#ifdef NVKVM_DEBUG` / `NVKVM_MODE2_DEBUG`). Debug mode
  is NOT a security boundary (trusted-VM only), separate from verbose logging.
- QEMU = the cross-VM / host trust boundary only; the guest kernel emulates ALL intra-VM access rights.
- Run vast.ai GPU tests **strictly serially** (concurrent matmuls / mid-ioctl kills wedge the GPU).
- Commit AND push after each milestone. Branch: `mode-2`. Eventual end goal: per-CR3 isolate (#128).

---

## 1. Current status (what works / what's blocked)

- `cuInit(0)` ✅, `cuDeviceGetCount/Get/GetName/GetAttribute/TotalMem` ✅ (reports "NVIDIA GeForce
  RTX 3060", compute 8.6, 11909 MiB) — all pass in the guest through the forwarder.
- **`cuCtxCreate` ❌ — SIGSEGVs in libcuda.** This is the active blocker.
- The host GPU is healthy throughout (0% idle, recovers cleanly; no permanent wedge).

### THE key reframe (this session)
`cuCtxCreate` does **NOT hang** — it **crashes** (SIGSEGV). Prior notes called it a "hang /
completion-delivery / wait-target" problem; that was wrong — the `timeout: ... dumped core` line was
the *crash* being misread as a timeout kill. The whole "what does libcuda poll for completion"
investigation was chasing a symptom that is never reached. **Fix the crash first.**

### Crash fingerprint (gdb on the guest, deterministic)
```
Thread 1 "cup2" SIGSEGV
pc = libcuda.so + 0x300560   insn: mov -0x38(%rbp),%rax
rbp = 0    si_addr = 0xffffffffffffffc8 (= -0x38)
rsi = 0xc030462b (= NV_ESC_RM_ALLOC ioctl)   r15 = 0x5c00001a (the c7c0 compute-object handle)
```
`rbp` here is **not a frame pointer** — optimized libcuda uses `rbp` as a general register holding an
**object pointer that is NULL**. So an object libcuda expected to exist (keyed by the compute object,
r15) was never created → null deref. The crash happens **right after the `c7c0` (AMPERE_COMPUTE_B)
alloc returns**, before libcuda issues the next alloc.

### What has been RULED OUT (do not re-chase)
1. **Not the c7c0 params writeback.** Forcing the c7c0 GR-object reply `paramsSize=0` (commit M8.1)
   vs the old `=16` makes **zero difference** — identical crash. The guest now progresses *past*
   c7c0 either way. So c7c0's params are not the clobber.
2. **Not an oversized-reply overflow.** Every `fn=103` alloc reply's `paramsSize` equals libcuda's
   *request* size (e.g. c56f channel alloc reply = 368 = request). The ~368-byte run of zeros seen
   on the stack at the crash is libcuda's *own* zeroed `pAllocParms` buffer (a red herring), not a
   kernel writeback overrun.
3. **It is a libcuda-internal divergence**, caused by an earlier RM reply that differs host-vs-guest,
   which makes libcuda compute different state and eventually deref null. (See §3.)

---

## 2. What was built this session (all committed + pushed on `mode-2`)

| Commit | What |
|---|---|
| `93ca53b` | **M5.28 per-channel VAS → Xid 32 ELIMINATED.** |
| `53e7736` | **M8.1** c7c0 reply `paramsSize=0` (revert M7 regression) + crash reframe. |
| `0be03bb` | **M8.2** alloc-reply `paramsSize` diag — ruled out oversized-reply theory. |

### M5.28 per-channel VAS (the headline win)
Each forwarded **GR/compute TSG** (engineType GRAPHICS=1) now gets its **own fresh nvkvm-owned**
`FERMI_VASPACE_A` (+ `NV01_MEMORY_VIRTUAL` mapper) under the channel's forwarded device, instead of
running in the guest's forwarded VAS (which the host RM self-promotes GR ctx into → guest VAs collide
with `st=0x51` → **Xid 32** "corrupt pushbuffer"). Live-validated: **Xid 32 gone (0 occurrences)**,
collisions dropped 94→~11, host GPU healthy, no regression.

Implementation (`src/qemu/nvkvm_gpu_emul.c`):
- struct `m2_cvas[16]` `{client,tsg,hdev,fvas,fvirt,populated}` + `m2_cur_cvas` (init `-1` at
  isolate-ready). Search `M5.28`.
- `nvkvm_m2_cvas_get(client,tsg)` — lazily allocs the fresh VAS under the client's forwarded device.
- `shadow_fwd` `a06c` handler: for engineType==1 substitutes `fvas` into `hVASpace@8` ALWAYS (the
  compute TSGs pass an *explicit* forwarded VAS, so the old `hVASpace==0` gate never fired for them).
  `9067` ctxshare substitutes the same `fvas`.
- `grmapper` has an override: when `m2_cur_cvas>=0` and client matches, return `fvirt` so all FIXED
  `map_dma` route into the fresh VAS.
- doorbell loop sets `m2_cur_cvas` per-channel (match by `c->tsg`), calls `nvkvm_m2_populate_cvas`
  (walk the channel's guest PDB via `chan_own_pdb` → `pt_enum` → map each leaf), resets `-1` after.

**Known limits of M5.28 (future work, NOT the cuCtxCreate blocker):**
- `populate_cvas` is a no-op for the GR channel: its VAS is GSP-managed and not snooped into
  `chan_vas[]`, so `chan_own_pdb` returns 0. Only the reactive maps (in `chan_execute`, routed via
  cvas) populate it. The GR channel is also idle (`gp_put=0`) during cuCtxCreate.
- COPY-engine channels (engineType 0x9–0xc = NV2080_ENGINE_TYPE_COPY0-3, a *different* RM client from
  the GR channel in the same process) still use the legacy forwarded VAS → ~11 `st=0x51` collisions
  (no Xid). **Extending cvas to copy engines HANGS the guest** (PMC_BOOT_0 reset spin) — it redirects
  copy channels off the main guest VAS `0xcaf00005` that the guest driver relies on (the copy TSG
  *constructs* fine — SHADOW status=0, BIND `0xa06c010a` status=0 — but the guest faults downstream).
  Copy channels need a different approach; revert if you try it.

### M8.1 / M8.2 (c7c0 reply)
`src/qemu/nvkvm_gpu_emul.c` around the `if (fn == 103)` block (search `M8.1` / `M8.2`). M8.1 forces
GR-object (`fam>=0xb0 && (lb==0xc0||lb==0x97)`) reply `paramsSize=0`. M8.2 logs every alloc reply's
`paramsSize`. Both are bring-up diagnostics; M8.1 reverts a regression (the M7 "forward real 16B caps"
re-opened the c7c0 clobber that an older commit `1443793` had fixed with `paramsSize=0`).

---

## 3. THE active lead — host-vs-guest RM-reply divergence (in progress)

Tooling: `scripts/mode2_diag/nvioctl_trace.c` — an `LD_PRELOAD` ioctl tracer that decodes
NVOS54 (RM_CONTROL, NR 0x2A) and NVOS21/64 (RM_ALLOC, NR 0x2B) and dumps, post-call:
- `CTRL cmd= psz= status= content=<hex>` (env `NVCONTENT`, default 48 bytes)
- `ALLOC class= psz= status= areply=<hex params> outer=<hex NVOS64>` (env `NVALLOC` 32, `NVOUTER` 64)

Build: `gcc -shared -fPIC -O2 -o nvioctl_trace.so nvioctl_trace.c -ldl`.
Run: `NVALLOC=64 NVOUTER=64 NVTRACE=/tmp/trace.txt LD_PRELOAD=./nvioctl_trace.so <prog>`.

Captured baselines (saved in repo):
- `docs/design/mode2_traces/host_cup2_trace.txt` — native `cup2_host` on the host (full PASS, 100
  allocs, runs the whole compute test incl. CE PASS).
- `docs/design/mode2_traces/guest_cup2_trace.txt` — guest Mode-2 cup2 (crashes at alloc #22 = c7c0).
- `docs/design/mode2_traces/ctrl_divergence.txt` — every CTRL cmd whose first-occurrence content
  differs host-vs-guest.

### What the diff shows
Alloc class sequence is **identical** host-vs-guest through #22 (c7c0); guest crashes before #23
(c7b5). c56f (#21) and c7c0 (#22) have `psz=0` (no kernel writeback) so their `areply` bytes are
libcuda's *own* buffers — and they already diverge (e.g. c56f `flags@20`: host `0x20`, guest `0x00`),
proving **libcuda's internal state diverged earlier** from a faked control reply.

**Dominant divergence pattern (`ctrl_divergence.txt`):** a whole family of NV0000 GPU-enumeration
controls return **`0x00000007` on host vs `0x00010000` on guest** at the gpuId/instance field:
`0x13a, 0x201 (GET_ATTACHED_IDS), 0x202, 0x205, 0x214 (GET_PROBED_IDS), 0x215, 0x288`. These are the
guest's emulated gpuId (`0x10000`) vs the host's real gpuId (`0x07`). Prior notes called the gpuId
difference "benign", but the **consistency** matters: if libcuda mixes an emulated-id value with a
forwarded-from-host value, GPU-by-id lookups can return null → the crash. This is the prime suspect.

Other diffs to weigh:
- `0x800292` (GET_CLASSLIST count): guest `0x61`=97 vs host `0x6b`=107 — numClasses, noted benign.
- `0x20800102` (GPU GET_ENGINES-ish): guest `0x80000011` vs host `0x00000011` — **extra bit
  `0x80000000`**. Worth checking; not obviously benign.
- `0x0080170d`, `0x20801201`, `0x00000101`, `0x00000d04` — pointer / client-handle differences
  (benign: addresses + remapped client `0xc1d00003` vs host `0xc1d005fd`).

### Concrete next step (the fix path)
1. **Decide the gpuId story.** Confirm whether the `0x07` vs `0x10000` mismatch breaks a libcuda
   GPU/device lookup. Easiest test: make the divergent NV0000 enumeration controls return a value
   *consistent* with whatever the guest uses everywhere (or forward them so libcuda always sees the
   host's `0x07`). The existing code already forwards a curated set of GET controls (search the
   `0x906f0101` / `0x0080170d` forward block, ~line 1429, gated `m2fwd`) precisely because faking
   them shifts libcuda's stack/behavior — extend that set to the divergent enumeration controls,
   carefully (NV0000 root-client controls; mind handle/address translation).
2. After each change: rebuild → fresh QEMU boot → run cup2 under gdb (`gcup2_segv.sh`) and check
   whether the crash clears or moves. Use the trace diff to confirm the target control now matches.
3. Once cuCtxCreate stops crashing, the channel-execution path is already de-risked (Xid 32 fixed);
   pick up first-compute (the matmul) — and only then revisit COPY-channel VAS + completion delivery.

Decode the exact cmd meanings from the open driver source on the host (9p: `/usr/src/nvidia-580.159.04`,
or `/root/open-gpu-kernel-modules`) — grep the `NV0000_CTRL_CMD_*` / `NV2080_CTRL_CMD_*` and the
`NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS` `flags` bits rather than guessing from bytes.

---

## 4. Repro recipes & environment

### SSH
- `ssh vg` → guest VM (ubuntu@localhost:2222). `ssh vh` → host (root@77.104.167.149:44850).
- The guest 9p tag `nvkvm_src` → host `/workspace/nvkvm` (mount at `/mnt/nvsrc`). Guest scp DOES
  work for small files (used it this session); large files (>~4 GB) hit 9p EIO.

### Host layout (`vh`)
- `/opt/qemu-src/` — QEMU source. Deploy: `scp src/qemu/nvkvm_gpu_emul.c vh:/opt/qemu-src/hw/misc/`
  then `ssh vh 'cd /opt/qemu-src/build && ninja install'` (writes `/opt/qemu-nvkvm/bin/...`).
- `/tmp/m2launch.sh` — the reliable direct-cmdline launcher (kills old qemu, boots a FRESH VM with
  `-m 8G`, `hostfwd=tcp::2222-:22`, `-device nvkvm-gpu-emul,...,m2fwd=on,m2exec=on`, logs to
  `/tmp/m0_qemu.log`). Restarting QEMU = a fresh guest boot (wipes guest `/tmp`).
- `/tmp/cup2_host` — native host cup2 (real GPU, full PASS) for baselines. Host has nvcc + libcuda
  580.159.04 matching the guest.

### Guest run scripts (in `scripts/mode2_diag/`)
- `cup2_run_m516.sh` — swaps Mode-1 `nvkvm_guest` for the open `nvidia.ko`+`nvidia-uvm.ko`, fixes the
  **DYNAMIC** nvidia-uvm major (read from `/proc/devices`; hardcoding it = `cuInit 999`), symlinks
  libcuda, builds + runs `/tmp/cup2`.
- `gcup2_segv.sh` — same setup, runs cup2 **under gdb** to catch the SIGSEGV (pc/rbp/regs/bt).
- `gcup2_stack.sh` — gdb stack dump on SIGSEGV.
- `gtrace.sh` — builds nvioctl_trace.so + runs cup2 with it → `/tmp/guest_trace.txt`.
- `tests/mode2/cup2.c` — the test: cuInit → device queries → **cuCtxCreate** → cuMemAlloc →
  (host version also does memcpy + a CE PASS check).

### CRITICAL gotchas
- **Fresh QEMU boot per run.** A 2nd `nvidia.ko` load in the *same* QEMU = WPR2-dirty → `cuInit 999`
  (`_kgspBootGspRm: WPR2 is still up`). Always relaunch QEMU before a clean cuCtxCreate run.
- **Stage test files AFTER the m2launch reboot** (it wipes guest `/tmp`). Order: relaunch → wait for
  `ssh vg` → scp cup2.c + scripts → run.
- **cup2 stdout is fully buffered** when redirected to a file → printfs are LOST on crash. Use
  `stdbuf -oL -eL timeout 40 /tmp/cup2` or read the QEMU log to confirm progress.
- A mid-op guest crash can leave the emulated GPU view dirty; a fresh boot fixes it. Host GPU is
  unaffected (`pkill qemu` is always safe).

---

## 5. Architecture notes you'll need

- **Channels:** `c56f`=GPFIFO channel, `a06c`=KEPLER_CHANNEL_GROUP (TSG), `9067`=FERMI_CONTEXT_SHARE,
  `90f1`=FERMI_VASPACE, `0070`=NV01_MEMORY_VIRTUAL, `0080`=Device, `2080`=subdevice,
  `c7c0`=AMPERE_COMPUTE_B, `c7b5`=AMPERE_DMA_COPY_B. Channel engineType@128: GRAPHICS=1,
  COPY0..n=0x9..0x12.
- In `cup2`'s cuCtxCreate the **GR/compute** channel is a separate RM client from the pool of
  **COPY** channels (e.g. this run: GR client `0xc1d00003` TSG `0x5c000012`; COPY client `0xc1d00001`
  TSGs `0xcaf00008/20/36/4c`). The COPY channels are the ones that actually run scrub/init work
  (`gp_put=30`); the GR channel is idle during cuCtxCreate.
- **GSP-RPC functions seen:** `fn=10` poll, `fn=76` GSP_RM_CONTROL, `fn=103` RM_ALLOC, `fn=47/65/70`
  other. Control/alloc reply layout in `resp`: hClient@80, hObject@84/88, cmd@88, status@92,
  paramsSize@96/100, params@112/120 (offsets differ alloc vs control — see the code).
- **GPGA range-table model (the intended end-state address virtualization, user-specified):** GPGA is
  a *range table*; one backing object can occupy MULTIPLE disjoint GPGA ranges, each
  `{gpga_start, len, object, obj_off}`. Resolution is two-level: `VA →(channel PDB walk)→ GPGA
  →(range table)→ (object, obj_off + within_range)→ host backing`. Aperture (sysmem WB / vidmem) is
  per range. The stub's CPU VA need not match the guest userspace VA — only the GPU VA (in the
  channel's GR VAS) must equal the guest's GPU VA. Partial impl exists (`m2_objs`/`m2_gpga`,
  `nvkvm_m2_pt_enum`/`leaf_add`/`leaf_flush`, `back_and_map_sys` zero-copy + `gpga_obj` vidmem).

---

## 6. gpu_emul.c tech debt (TASK #128 — do AFTER first-compute)

`src/qemu/nvkvm_gpu_emul.c` (~5000 lines) has heavy bring-up debt: two exec/doorbell paths
(`nvkvm_chan_execute` vs `nvkvm_m2_exec_doorbell`), two mapping mechanisms (`back_and_map` copy vs
`back_and_map_sys` zero-copy), four address resolvers (content-pick, `bar1_wpg`, `chan_own_pdb`,
`va_map`/PROMOTE_CTX), legacy `m2_fbback` vs half-migrated `m2_objs`/GPGA, and many ungated inline
DIAG probes. Do NOT refactor now — need a GREEN first-compute reference first. Meanwhile: converge on
the chosen single mechanisms (one no-copy map, one PDB-mirror resolver, per-channel O(1) chid ring),
gate new DIAG behind `NVKVM_MODE2_DEBUG`, delete superseded paths instead of leaving "fallbacks".

---

## 7. Relevant out-of-repo memory (key facts; full files in the assistant's memory dir)

- `mode2_c7c0_crash_deterministic.md` — the cuCtxCreate crash fingerprint + the corrected model
  (this doc supersedes/expands it).
- `mode2_first_compute_blocker.md` — long running log of the data-plane discovery (M5.x). Note its
  "cuCtxCreate hangs / completion" framing is the *symptom-after-crash* mistake corrected here.
- `mode2_gpu_emul_refactor_debt.md` — TASK #128 detail (see §6).
- `ssh_aliases.md`, `vast_host_setup.md` — environment (see §4).
- `remote_test_serialization.md` — run GPU tests serially; wedge → `vastai reboot instance ...`.
- `mode2_dma_virtualization_hypothesis.md` — the user's "virtualize the whole address space" reframe;
  existential risk = does the NVIDIA KMD attest real hardware (it does NOT block this on GA106).
- `multi_driver_validated.md` — HOST is on driver 580.159.04 (open). Guest userspace must match.

Task tracker: **#126** = "Mode-2 M5: real compute forwarding (cuCtxCreate → first compute)" is the
active item. #128 = the consolidation refactor (deferred).
