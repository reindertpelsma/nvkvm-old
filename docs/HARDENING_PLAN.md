# nvkvm Isolate Lockdown + ioctl-Boundary Hardening — LIVING PLAN

> **READ THIS FIRST AFTER ANY CONTEXT COMPACTION.** This is the authoritative,
> live plan for the security-hardening work on branch `security-fixes`. Update
> the status markers as you go. Re-read before each phase.

## Why (the thesis)
The **isolate/stub is the security boundary**. Ioctls executed *in the stub*
inherit the nvidia kernel's per-RM-client / per-fd object isolation — a guest
can only touch objects its own stub created. The ioctl-verification work
(Phases 1–5) **relies on the stub actually being an isolate**. So Phase 0
(lockdown) comes first; if the stub isn't contained, the rest is futile.

The guest is untrusted. QEMU (vmm) is privileged. The stub runs guest-driven
nvidia ioctls. We must contain the stub so a stub RCE cannot escape to the host.

## Status legend: [ ] todo  [~] in progress  [x] done  [!] blocked

---

## Phase 0 — Isolate lockdown (FOUNDATION, do first)

Target: a stub that is namespaced, capability-less, seccomp-filtered, and
chrooted into an empty read-only tmpfs — far stronger than today (seccomp was
effectively OFF in the libc test stub: "apply_seccomp defined but not used").

Split of responsibility (SYNTHESIZED — refined from the original split):
- **QEMU's forked child does ALL namespace + mount setup** (it has libc → easy
  uid_map writes, error handling, failure logging), then fexecve's the stub
  already-contained. Sequence in the child, before exec:
  1. `unshare(CLONE_NEWUSER)`; write `/proc/self/uid_map` `0 <vmm_uid> 1`,
     `setgroups=deny`, `/proc/self/gid_map` `0 <vmm_gid> 1` → **ns-root**
     (rootless: full caps inside the userns, unprivileged on host; NO ambient
     caps needed).
  2. `unshare(CLONE_NEWPID|NEWNET|NEWIPC|NEWUTS)`; `fork()` so the stub is PID 1
     in the new pid ns.
  3. Open `/dev` `O_PATH|O_DIRECTORY` dirfd; dup it to a known fd (e.g. 4) so
     the stub uses `openat(4, "nvidiaX", O_RDWR)` after the mount ns.
  4. `unshare(CLONE_NEWNS)`; mount tmpfs RO; `pivot_root`+`chroot`+`chdir` into
     empty root.
  5. `PR_SET_NO_NEW_PRIVS` (early — only blocks gaining privs).
  6. `fexecve(mfd)` → stub.
- **Stub (no-libc, raw syscalls) does only the tail**: `PR_SET_DUMPABLE=0`;
  **drop all caps LATE** (bounding set + capset) — after any privileged setup;
  **two-phase seccomp**: loose during its own init, then a TIGHT steady-state
  filter dropping unshare/mount/pivot_root/ptrace/socket/etc. (main loop needs
  only ~{ioctl,mmap,munmap,sendmsg,recvmsg,futex,close,openat,exit}).
- Devices: stub opens /dev/nvidia* on demand via `openat(dev_dirfd, ...)`.
  (Stub currently opens by path in OPEN_DEVICE; switch to openat(dev_dirfd).)
- **Fail closed**: lack of namespaces/seccomp ⇒ refuse to start unless explicit
  `NVKVM_STUB_NO_{NS,SECCOMP}=1` / QEMU flag.
- **Containment self-test** (debug build): after lockdown, attempt to (a) open a
  path outside the empty root, (b) see other PIDs, (c) open a socket, (d) regain
  a cap — assert each FAILS. Proves the isolate is real.

Sub-steps (each is a deploy+test cycle on vast.ai; test = single matmul + 2/4
concurrent + nvidia-smi must all still pass):

- [x] **0.0 Stub build decision.** DONE — no-libc seccomp-ON stub builds + passes single/2/4-conc/nvidia-smi end-to-end. The committed `security-fixes` stub is
  no-libc (C7). Recent integration testing used the *libc* stub (master-based
  /tmp/nvkvm-slot) with seccomp OFF. FIRST: build the security-fixes no-libc
  stub (`make -C src/stub`), deploy it, confirm single matmul + 2/4 concurrent
  + nvidia-smi pass with seccomp ON. This de-risks the foundation. If no-libc
  build/integration is broken, decide: fix it, or consolidate on libc stub.
  Update deploy scripts to build the chosen stub.
- [x] **0.1 seccomp ON + allowlist audit.** DONE — seccomp called unconditionally; no SIGSYS across full run, allowlist sufficient. Confirm apply_seccomp is actually
  called (not gated off). Run with strace/seccomp-log to enumerate every
  syscall the stub needs (ioctl, mmap, recvmsg/sendmsg, futex, clone3, openat,
  close_range, memfd, etc.) and the new lockdown syscalls (unshare, mount,
  pivot_root, capset, prctl, openat). Widen allowlist as needed. Verify pass.
- [x] **0.2+0.3+0.4(net/ipc/uts) DONE (A1, commit pending).** userns(rootless 0->euid)+net/ipc/uts ns+no_new_privs+dumpable=0+all-caps-dropped, in QEMU child, fail-closed. Verified: CapEff=0, NoNewPrivs=1, user/net/ipc/uts isolated; single/2/4-conc/nvidia-smi pass. REMAINING in 0.4: pid ns (needs fork-for-PID1 + host-pid reporting).
- [ ] **0.2b (superseded label) no_new_privs + dumpable=0 + drop all caps** (stub, after opening
  device handles). Test.
- [ ] **0.3 user namespace** (QEMU child: unshare(CLONE_NEWUSER), write
  uid_map/gid_map mapping 0→vmm_uid, deny setgroups). Test rootless spawn.
- [x] **0.4 pid/net/ipc/uts namespaces** DONE — via clone(CLONE_NEWUSER|NEWPID|NEWNET|NEWIPC|NEWUTS) from QEMU (no double-fork; clone returns the stub's host pid directly; parent writes the rootless single-line uid/gid map gated by a sync pipe). Verified: stub is NSpid 1, all 5 ns isolated, caps=0, seccomp=2. Single/2/4-conc/nvidia-smi pass.
- [ ] **0.4-old pid/net/ipc/uts namespaces** (QEMU: unshare(NEWPID|NEWNET|NEWIPC|
  NEWUTS) then fork so stub is PID 1). Bonus: nvidia host-kernel calls now see
  an empty pid ns → no other-process leakage. Test.
- [ ] **0.5 empty mount namespace** (stub: capture dev O_PATH dirfd; unshare
  NEWNS; mount tmpfs; pivot_root + chroot + chdir; switch OPEN_DEVICE to
  openat(dev_dirfd, ...)). Test.
- [ ] **0.6 fail-closed flags** + final re-test: single + 2/4 concurrent +
  nvidia-smi on the fully-hardened model. Commit milestone.

Relevant files: `src/qemu/nvkvm_isolate.c` (fork/exec spawn ~line 415-520),
`src/stub/nvkvm_stub.c` (main ~line 1700+, apply_seccomp ~1353/1374,
OPEN_DEVICE handler ~1150+), `src/stub/stub_freestanding.h` (syscall macros).
Refs memory: [[isolate_hardening_todo]], [[security_audit_2026_05_28]] (C6),
[[vast_host_setup]].

---

## Phase 1 — Inventory pass  [ ]
Log every forwarded cmd + WHERE it executes (stub vs QEMU-direct) over a full
CUDA + nvidia-smi run. Produce the QEMU-direct danger list (the unscoped
surface). Output: a table in this doc.

## Phase 2 — nvidia-smi PID translation + response scrubbing  [ ]
nvidia-smi enumerates processes via RM ioctl (NV2080_CTRL_CMD_GPU_GET_PIDS /
GET_PID_INFO) — confirmed NOT a /proc scan (strace showed zero /proc/<pid>
opens). Returned PIDs are HOST stub PIDs. Translate stub-host-pid → guest-tgid
in the response using the isolate↔session↔mm/tgid map (H2). Discard/scrub any
process the guest must not see. Same for per-process mem/util. Verify
nvidia-smi shows the guest's own PIDs.

## Phase 3 — Schema table + default-deny  [ ]
Per-cmd descriptor table: `cmd → {runs_in, fields:[{off,size,kind}]}`,
kind ∈ {fd, handle, pid, gva_ptr, access_mask, count, opaque}. Validate/
translate per field. **No descriptor ⇒ not forwardable from QEMU** (stub-only).
Move every movable QEMU-direct cmd into the stub. Genuine exceptions (UVM binds
to mm; mmap/KVM-region installs) keep explicit schemas.

## Phase 4 — Access-model simulation  [ ]
Guest kernel module enforces guest-local /dev/nvidia* uid/gid/mode ("all" =
all-in-VM, never host-wide). RM-level sharing (RS_ACCESS/share-mask/DUP_OBJECT,
the ownMask machinery from cuctxcreate_800_pinned) brokered by QEMU within one
VM (same session↔mm); cross-VM denied.

## Phase 5 — UVM teardown audit  [ ]
Verify UVM_FREE / UVM_UNMAP_EXTERNAL translate the guest VA correctly and tear
down consistently: (a) VA→mapping translation, (b) KVM memslot removal,
(c) sparse-window anon-backing restore (recycling), (d) UVM kernel state free.
Suspected gap: we have REALIZE (create) but teardown may leak GPA windows.
Refs: [[gpa_window_design]], [[nvos56_fake_success]], [[state_machine_step_e]].

---

## Working rules (user directive)
- Do phases continuously: commit a milestone, immediately continue the next; no
  pause needed between milestones. Only stop when genuinely stuck.
- Tight timeouts on all remote commands; detect hangs fast.
- Test each Phase-0 sub-step (single + 2/4 concurrent + nvidia-smi) before moving on.
- Keep this doc live: update status markers + findings as you go.
