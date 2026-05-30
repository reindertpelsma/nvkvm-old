# nvkvm security audit — 2026-05-30 (post #73/#27/#37/#65/#66)

Three adversarial read-only audits over the boundaries that changed since the
2026-05-29 audit. Attacker model: full control of the guest kernel; and,
separately, code execution inside the stub. Findings below; status updated as
fixed.

## CRITICAL

### C-1 — stub seccomp filter never covers the worker threads  ❌→✅ FIXED
`src/stub/nvkvm_stub.c`: the 16 `clone3` workers are spawned *before*
`apply_seccomp()`, which calls `seccomp(SECCOMP_SET_MODE_FILTER, 0, …)` with
**no `TSYNC`**. seccomp binds per-thread, so the filter lands on the reader
thread only — **every worker (which runs all attacker-influenced ioctl
handling) runs with no seccomp at all**: free `execve`/`ptrace`/`mprotect(RX)`/
`open` outside the sandbox. Nullifies the entire §4 seccomp pillar (mount-ns/
userns/cap-drop still apply, so not a total escape, but the syscall-surface and
W^X guarantees are void on the threads that matter).
**Fix:** set `PR_SET_NO_NEW_PRIVS` and install the filter **before** the worker
spawn loop so workers inherit it (seccomp + no_new_privs propagate across clone).

## HIGH

### H-A — OOB heap write in GET_PID_INFO fixup (#66)  ❌→✅ FIXED
`nvkvm_isolate_handlers.c` `nvkvm_req_ioctl_on_isolate` GET_PID_INFO post-pass:
writes `result`@`off+8` (4B) and `sum`@`off+16` (8B) for each entry, but the
pre-pass only validated `off+4 <= aux_size`. Guest sends `aux_size=84, count=2`
(both fields guest-controlled) → entry i=1 at off=80 passes the `off+4` check,
the writeback stores 8 bytes at off+16=96..104 — ~20 bytes past the slot, a
guest-driven OOB write in the privileged QEMU process.
**Fix:** require the FULL entry to fit (`off + NVKVM_PIDINFO_STRIDE <= aux_size`)
in both the pre-pass truncation and the writeback.

### H-B — stub SIGSEGV handler cannot recover → worker/CPU DoS  ❌→✅ FIXED
`sigsegv_handler` records `si_addr` and returns without fixing the trap context,
so a faulting instruction re-executes forever (unkillable loop pinning a worker
+ a host core). Reachable when a stub-side embedded-pointer rewrite derefs a bad
pointer (offsets derived from guest sizes). 16 such faults wedge the isolate.
**Fix:** make faults recoverable — longjmp to a per-worker recovery point set
before the ioctl/rewrite, abort the txn with -EFAULT.

### H-1 — GPA sparse window is a no-free bump allocator (host DoS)  ⏳ #61
`nvkvm_sparse_gpa_alloc` only advances `sparse_cur`; no free. munmap/kill never
return GPA. A guest looping mmap/munmap (or cuMemAlloc/Free) exhausts the 128
GiB window irrecoverably → all GPU mmaps fail, VM GPU wedges; affects even a
long-lived well-behaved guest. **Fix:** free-list keyed by the gpa/len already
stored in `iso_mmap_tbl`; free on munmap + isolate kill.

### H-2 — QEMU `nvkvm_session` structs never destroyed (unbounded leak)  ⏳ #61
No `TAILQ_REMOVE` of a session anywhere; `nvkvm_handle_close_session` /
`nvkvm_isolate_kill_session` have zero callers. Each short-lived guest process
leaks a session + lists + 2 mutexes in QEMU forever. **Fix:** session-destroy
hook on last-isolate-kill.

### H-3 — no host-side reaper: adversarial teardown leaks fds/RM/isolates  ⏳ #61
`nvkvm_isolate_kill` never unrefs the handles the isolate held; cleanup relies
on the guest sending CLOSE_HANDLE_ON_ISOLATE first. A guest that kills (or goes
silent) without closing leaves handles at `isolate_refcount>0` → `_close`
returns EBUSY forever → leaked /dev/nvidia* fds + RM objects + GPU memory, and
the isolate process itself. No timeout/reaper. **Fix:** on kill, walk + release
the dead isolate's handles + iso_mmap entries + GPA; per-VM caps; idle reaper.

## MEDIUM

### M-A — non-`'F'`-type ioctls bypass every QEMU allowlist  ❌→✅ FIXED
All frontend default-deny gates guard on `_IOC_TYPE(cmd)=='F'` and the UVM
schema only on `dev_id==UVM`. A non-UVM handle + a cmd with `_IOC_TYPE != 'F'`
skips *all* gates and forwards the raw guest cmd to `ioctl()`. The kmd dispatches
on `_IOC_NR` (see the IOCTL-NR-collision memory), so this can reach a denied
escape. **Fix:** deny any non-'F' cmd on a non-UVM handle before forwarding.

### M-B — worker embedded-pointer rewrites trust guest-derived offsets
Several rewrites (InfoList/BUILD_VERSION/channellist/UVM@9248) write into
param/aux at offsets from guest sizes with looser guards than the access; feeds
H-B. Centralize a bounds check before each write. (Mitigated once H-B fails-safe.)

### M-C — M-2 blind `param_buf+16` aux-pointer write for any aux ioctl
Reconfirmed: the +16 host-VA write fires for any cmd with aux, clobbering 8
bytes of param for cmds whose struct lacks a +16 pointer. Gate on the cmd table.

### M-D — stub→QEMU response framing desync is attacker-driven
A compromised stub can announce param_size with no follow-up datagram (desync)
or echo another txn_id (intra-VM cross-request injection, bounded by param_cap).
Single-tenant only. Fix: single-datagram IOCTL response (iovec), fatal on
size-mismatch.

### M-E — iso_mmap_tbl entries leak on isolate kill (table exhaustion)  ⏳ #61
8192-entry global table freed only by explicit munmap; kill never scans it.
Map-then-kill loop exhausts it. Fix with H-1 (free on kill).

### M-F — H-3 hClient allowlist fail-open while empty + never shrinks
`client_allow` gate guarded by `client_allow_n>0` (default-open before first
record; narrow window) and grows-only (freed clients stay allowed). Real
containment rests on the kernel reach-gate; this DiD layer is weaker than its
comment. Fix: default-deny independent of count; prune on teardown.

## LOW / informational

- **L-1 cross-VM containment sound** — per-VM QEMU process; handle table,
  client_allow, iso_mmap_tbl, sparse window, admin client all per-VM. DUP gate +
  kernel reach-gate: no cross-tenant reach found.
- **L-2 #66 admin client lifecycle OK** — per-VM, lazy, freed at unrealize;
  GET_PID_INFO only queries validated own-isolate host tids (no arbitrary pid);
  no cross-tenant leak. (The OOB H-A is a buffer bug, not pid confusion.)
- **L-3 handle table** — bounded indexing, stale-id recheck, C-2 dup-under-lock
  sound; no UAF.
- **L-4 TOCTOU on UVM embedded-fd translation** — `handle_get` then read `->fd`
  without dup (vs C-2's acquire on the target fd); intra-VM, tiny window. Harden
  by dup-under-lock for the embedded fd fields too.
- **L-5 interrupt_txn (#73)** — 32-bit txn wrap could mis-interrupt after 2^32
  txns (intra-VM, unreachable in one boot); carry the epoch.
- **L-6 dead tombstones** — `nvkvm_dispatch.c`/`nvkvm_frontend.c` reachable only
  under `#if 0`; delete to avoid confusion.

## #61 verdict (UVM/resource teardown)
- **Clean exit / SIGKILL:** correct for fd + RM-object reclaim (guest `.release`
  runs on fd teardown regardless), BUT leaks GPA-window space (H-1) + session
  structs (H-2) unconditionally.
- **Adversarial skip:** NOT contained — no host reaper (H-3, M-E). This is the
  documented H-4 residual, broader than thought (touches the clean path too).
- Root cause: teardown + resource accounting are entirely guest-driven with no
  host-side per-VM ledger/reaper/caps. Minimum multi-tenant fix: per-session
  ledger (handles + isolates + GPA ranges + iso_mmap tokens) released on
  kill/destroy/idle, plus per-VM caps.
