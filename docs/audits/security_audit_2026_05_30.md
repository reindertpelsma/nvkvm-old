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

### H-B — stub SIGSEGV handler cannot recover → worker/CPU DoS  ❌→✅ FIXED (round 2)
`sigsegv_handler` recorded `si_addr` and returned without fixing the trap
context, so a faulting instruction re-executed forever (unkillable loop pinning a
worker + a host core). (Round 2 R2-H1 caught that this was first mislabeled
"FIXED" here while no fix existed.) **Fix (actual):** `sigsegv_handler` now
`stub_exit(139)` — a stub SIGSEGV is a stub bug (the nvidia driver's own bad
accesses return -EFAULT, not SIGSEGV; the normal path never faults), so terminate
the isolate cleanly (async-signal-safe exit_group) instead of looping; QEMU's
reader then signals pending callers -ECONNRESET. longjmp recovery was rejected as
fragile under -O2 freestanding.

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

## Round 2 (verification + deeper sweep)

Round-2 agents re-verified round-1 fixes (C-1/H-A/M-A all confirmed complete &
not bypassable) and swept deeper (QEMU integer/memory safety; stub seccomp
completeness + signal safety; QEMU↔stub protocol under a malicious-stub model).

### R2-H1 — H-B was mislabeled FIXED; now actually fixed  ✅
See H-B above. The round-1 doc claimed a longjmp fix that did not exist in the
code. Now genuinely fixed via `stub_exit(139)` on a stub SIGSEGV.

### R2-H2 — malicious-stub UAF in QEMU via duplicate txn_id IOCTL response  ❌→✅ FIXED
`nvkvm_isolate.c` reader looked up the pending entry under the lock then dropped
it before `recv()`-ing into `p->param_buf`. A compromised stub echoing the same
txn_id twice could make response #2 re-find `p` and write into it after response
#1 woke the caller, which removes+destroys its stack-allocated `pending` and
returns → UAF write in QEMU. **Fix:** the reader now claims+removes `p` from the
pending list under the lock at lookup, so a duplicate txn_id finds nothing
(drained); the single live response stays safe (caller can't wake until
`p->done`, set after the recv).

### R2-M1 — SCM_RIGHTS fd leak on non-OPEN_DEVICE responses  ❌→✅ FIXED
Only OPEN_DEVICE consumed an ancillary fd; a malicious stub attaching a fd to any
other response type leaked it into QEMU's fd table (fd-exhaustion DoS). **Fix:**
the reader closes any received SCM_RIGHTS fd on every non-OPEN_DEVICE response.

### R2-L1 — pruned 6 vestigial seccomp allowlist entries  ❌→✅ FIXED
clone (clone3 is used), set_robust_list, madvise, lseek, pread64, readlinkat had
no freestanding caller. Removed to shrink the post-RCE syscall surface.

### R2-L2 — apply_seccomp ignored TSYNC positive (sync-failure) return  ❌→✅ FIXED
TSYNC reports a per-thread sync failure as a positive tid and applies the filter
to nothing; the caller checked only `< 0`. Now treats any non-zero as fatal.

### R2-L3 — INTERRUPT txn TOCTOU (intra-VM)  ⏳ residual
A late SIGUSR1 can -EINTR the worker's *next* txn if the target finished first.
Intra-VM/self-inflicted; sibling of L-5. Fix later by re-checking the txn after
entering the handler or carrying an epoch.

Round-2 confirmed CLEAN (no new findings): slot_blob bounding, UVM/REALIZE/
READ_HOST_FILE/#66-admin/#73-interrupt bounds, handle table, sparse arithmetic,
guest-trust-of-QEMU (guest copies back using its own sizes; gpa_base validated).

## Round 3 (handlers + concurrency)

Round-3 agent verified R2-H1/H2/M1 are correctly in place, and found:

### C-1 — sock_fd close/reuse race: KILL (TX thread) vs in-flight IOCTL (pool worker)  ❌→✅ FIXED
`nvkvm_isolate_kill` set `iso->sock_fd=-1`+`close()` under `iso->lock` while the
thread-pool IOCTL path sends under `write_lock` (a different mutex, a different
thread). A guest issuing IOCTL_ON_ISOLATE + KILL_ISOLATE on one isolate could
race the close against a worker's send → write isolate bytes into a recycled fd.
**Fix:** kill tears the fd down under `write_lock` (set -1, then close after
unlock); the worker snapshots `sock_fd` under `write_lock` and skips (-EPIPE) if
-1. Serialized; no close()+reuse window.

### N-1 — WRITE/READ_MEMORY_HANDLE missing TYPE_MEMORY check  ❌→✅ FIXED
Both did `pwrite/pread(h->fd,…)` checking only `fd>=0`; a guest could pass a
TYPE_NVIDIA handle (open /dev/nvidia*/eventfd) and drive read/write fops + an
arbitrary offset against a device fd. **Fix:** require
`h->type == NVKVM_HANDLE_TYPE_MEMORY` in both (→ EBADF otherwise).

### N-2 — MMAP_ON_ISOLATE page-align wrap  ❌→✅ FIXED
`len=(req->length+4095)&~4095` was computed before the bound check; a length near
SIZE_MAX wrapped to a small page-multiple. **Fix:** reject
`req->length==0 || >sparse_size` before the round-up.

### C-2 — KILL_ISOLATE 500 ms nanosleep stalled the whole TX thread  ❌→✅ FIXED
Every teardown blocked all virtio processing for 0.5 s (CREATE/KILL loop = VM
throughput DoS). **Fix:** poll `waitpid(WNOHANG)` in 10 ms steps and break as
soon as the stub (already sent EXIT) exits — typical stall ~10 ms; SIGKILL only
on budget overrun (no premature mid-ioctl kill, no added GPU-wedge risk).

Round-3 CLEAN elsewhere: OPEN_NVIDIA_HANDLE / OPEN_MEMORY_HANDLE / CLOSE_HANDLE /
CREATE/KILL_ISOLATE / COPY/CLOSE_HANDLE_ON_ISOLATE / POLL/UNPOLL / READ_HOST_FILE
/ REALIZE all validated; handle-table fd lifetime (acquire_fd dup-under-lock)
sound; the other lock domains (iso_mmap/sparse/kvm-slot/admin/client_allow) have
no inversion (MMAP/MUNMAP/REALIZE are TX-thread-only).
