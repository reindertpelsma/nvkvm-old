# nvkvm-tables-refactor — plan + decision record

Branch: `nvkvm-tables-refactor` (cut from `master` after commit `8ff18f4`).
Goal: replace the two parallel ID spaces (fd_token + handle_id) with a single
authoritative four-table model in QEMU, and remove the `dup()` workaround
that's currently masking a referencing bug.

Priority: getting a ChatGPT-scale LLM running inside the guest. Open driver
only (closed driver is being deprecated). Everything below serves that goal.

---

## 1. Architecture summary

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Guest userspace (libcuda)                                               │
└────────────────┼────────────────────────────────────────────────────────┘
                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Guest kernel (nvkvm-guest.ko) — speaks handle_id only, owns txn_id      │
│   maintains its own (multi-isolate, multi-handle) map for fork-aware    │
│   routing                                                               │
└────────────────┼────────────────────────────────────────────────────────┘
                 ▼ virtio
┌─────────────────────────────────────────────────────────────────────────┐
│ QEMU — four tables, single source of truth                              │
│   1. Handle table     handle_id → {qemu_fd, type, ready, refcount, poll}│
│   2. Isolate table    isolate_id → {pid, comm_fd, alive}                │
│   3. Iso↔Hnd map      M:N  (isolate_id, handle_id) → fd_on_isolate     │
│   4. MMAP table       mmap_id → handle_id, offset, vmm_va, gpa, size... │
│                                                                         │
│   GPA windows (one big sparse memfd per window, sub-allocate)           │
│                                                                         │
│   Allocates handle_id BEFORE telling isolate to open anything           │
│   Wire to isolate carries stub-local fd numbers (not handle_ids)        │
│   Strict cleanup: deps before owner                                     │
└────────────────┼────────────────────────────────────────────────────────┘
                 ▼ unix socket + SCM_RIGHTS (both directions)
┌─────────────────────────────────────────────────────────────────────────┐
│ Stub — table-free                                                       │
│   Obeys "ioctl(fd=N, …)" / "mmap(fd=N, …)" / "open(path)"               │
│   open() reply atomically carries (stub_fd_number + SCM_RIGHTS fd)      │
│   One I/O thread (strict FIFO). Worker pool, clone3, auto-scale, ≥1 idle│
│   Tiny per-process bookkeeping only for WRITE_MEMORY_HANDLE cleanup     │
└─────────────────────────────────────────────────────────────────────────┘
```

### Open ownership

| Device                          | Opener | Why                                       |
|---------------------------------|--------|-------------------------------------------|
| `/dev/nvidiactl`                | stub   | RM control; nvfp identity = stub's process |
| `/dev/nvidia0..N`               | stub   | events want distinct nvfp/fd; multi-client OK on driver |
| `/dev/nvidia-uvm`               | QEMU   | driver enforces "opener does mmap"        |
| memfd_create (GPA backing)      | QEMU   | KVM region installation lives here        |
| eventfd                         | either | no opener-binding; stub for consistency   |

**QEMU always holds an SCM_RIGHTS copy of every stub-opened fd**. `qemu_fd == -1`
window is bounded to the open syscall itself. Single sendmsg from stub
carries (reply, SCM_RIGHTS fd). If isolate dies, kernel struct file stays
alive via qemu_fd; ioctls return EBADF cleanly; close only happens when
guest releases its guest fd.

### ID semantics

- `handle_id`, `isolate_id`, `mmap_id`: all u32, low 12 bits = slot index,
  high 20 bits = generation. Stale ids fail lookup → no fd-reuse confusion.
- Allocated by QEMU only. Stub never invents IDs.
- Wire format QEMU↔stub uses stub-local fd numbers, not handle_ids.
- `txn_id` (u32) chosen by guest kernel, mirrored on every reply. Enables
  out-of-order completion. Track in-flight set as bitmap; never reuse a
  currently-in-flight id.

### Refcount + ready

- Every operation `acquire`s the handle: lookup under lock, check `ready`,
  `refcount++`, release lock.
- Operation runs, then `release`: lock, `refcount--`, signal cv if zero.
- Close: lock, set `ready=false`, wait on cv for `refcount==0`, drop deps,
  free slot.
- **Refcount bracket extends from request reception through reply send**,
  not just dispatch.

### CPU memory paths (in fast-path order)

**0. memfd-passthrough (the dominant case — must be the primary fast path).**
   When guest userspace passes a CPU pointer into an ioctl, and that
   pointer falls within a region already backed by one of our memfd-backed
   GPA windows, *nothing has to move*. We already have a handle for the
   memfd, the stub already has it mapped at a known VA, the guest writes
   already land in the same physical pages the stub sees. The guest module:
     - walks `find_vma(P)` → identifies the GPA window
     - derives (memfd_handle_id, offset_within_handle, size)
     - sends the ioctl request to QEMU carrying the triple; QEMU forwards
       to the stub along with the precomputed stub-side VA
     - stub rewrites the ioctl arg buffer to point at its own VA and calls
   Zero copy, zero allocation, zero coordination. This is what every
   CUDA-allocated buffer hits.

1. **userfaultfd promote (the medium-rare case).**
   Guest userspace allocated a buffer in plain anonymous memory (e.g. via
   `cudaHostRegister` on existing-malloc'd memory). We migrate that GPA
   region into a memfd window once, then the page falls into path (0)
   forever after. Step 5 PoC. If KVM-UFFD doesn't cooperate on 6.8 kernel,
   fallback to brief vCPU pause.

2. **WRITE_MEMORY_HANDLE (the residual edge case).**
   Buffers whose backing memory genuinely can't be swapped — vfio MMIO,
   PCI BAR pass-throughs from other devices, etc. Pointer rewrite in the
   ioctl, buffer bytes copied inline via virtio. Invariant: per-page
   mutex; never two concurrent writes to overlapping range. Stub has a
   tiny post-ioctl cleanup list for these (the only table on stub side).
   Slow but correct. Only path that doesn't hit the kernel struct file
   that's already shared.

### GPA windows

- 128 GiB upfront, one KVM memslot, one memfd backing.
- Sparse sub-allocation: free list of regions sorted by offset, coalesce
  on insert.
- `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)` on free, with
  ±1 adjacency check (look at predecessor and successor free regions,
  punch the entire contiguous gap). Double-punch is a kernel no-op.
- Additional 128 GiB windows lazily on first ENOSPC.
- Max 8 windows = 1 TiB total addressable.

### Cleanup ordering

```
A handle entry may be freed only after:
  - all entries in MMAP table referencing it are freed,
  - all entries in Iso↔Hnd map referencing it are unlinked,
  - poll registrations are deregistered,
  - in-flight refcount has drained to 0.

A window may be destroyed only after:
  - no MMAP entry references it.

An isolate may be closed only after:
  - all its Iso↔Hnd links are unlinked.
```

---

## 2. Race/blocker decision record

### Resolved with action

| Ref | Issue                                          | Decision                                     |
|-----|------------------------------------------------|----------------------------------------------|
| R1  | UFFD-on-KVM-memory                             | Sub-agent PoC. Fall back to vCPU pause if KVM-UFFD has gaps on 6.8. |
| R2  | KVM memslot exhaustion (512 cap)               | One GPA window = one slot. Sub-allocate via memfd offsets. Lazy 2nd window. |
| R3  | Refcount lifecycle bracket                     | `refcount++` at request reception, before queue; `refcount--` after reply leaves QEMU. Unit test for concurrent close + ioctl. |
| R4  | fork() in guest libcuda                        | Lazy clone of handle to child isolate on first use. M:N map supports this. Same behavior as bare metal (works for memfds, undefined for nvidia per upstream). |
| R5  | Memfd ↔ isolate binding                        | MMAP entries link to specific isolate_id; never re-linked. |
| R6  | Isolate kill mid-op                            | Socket EOF → mark all this isolate's handle entries `ready=false`, drain, error in-flight txns as EIO. Handles owned only by dead isolate stay valid via qemu_fd until guest close. |
| R7  | KVM_SET_USER_MEMORY_REGION during live DMA     | RM_UNMAP_MEMORY must precede KVM mutation. Natural ordering; enforce via assertion. |
| R8  | Stub message ordering                          | One I/O thread, strict FIFO. Workers parallel internally. Close-then-open-fd-reuse race impossible. |
| R9  | txn_id exhaustion                              | u32 + bitmap of in-flight. Hours of headroom at realistic rates. |
| R10 | Sub-allocator fragmentation                    | Sorted free list, coalesce on insert. Unit test churn pattern. Free-list-per-size-class is post-MVP. |
| R11 | PUNCH_HOLE vs concurrent write                 | Punch only after guest munmap completes for that GPA range. Hard ordering invariant. |
| R12 | Cross-isolate handle clone race                | Double-checked locking on the M:N link insert. Test for parallel first-use from two child threads. |
| R13 | Refcount bracket boundary                      | Per R3. Documented in code + tested. |
| R14 | Punch-hole 4 KiB granularity                   | Track only mmap allocations in MMAP table. On free, recompute the contiguous free region around the freed range (predecessor + this + successor if in free list) and punch the whole thing. Already-punched ranges are no-ops. Unit test verifies multiple out-of-order frees consolidate correctly. |
| R15 | Stub thread pool size cap                      | Bound to 256 worker threads. Real workloads stay well below. |
| R16 | Memfd hole reuse                               | Accept "max touched simultaneously" as physical RAM usage. Out of scope for now; revisit if reporting matters. |
| R17 | GPA window placement vs guest e820             | Place windows above guest RAM extent. QEMU CLI / config dictates the boundary. Out of scope for now (unbounded). |
| R18 | Multi-window growth notification               | virtio config-change notification → guest module appends to its window list. Standard pattern. |
| R19 | Stub crash mid-WRITE_MEMORY_HANDLE             | Socket EOF, drain, error txn. Target ioctl never runs. Safe. |
| R20 | KVM_RUN exit reason ordering on vCPU-pause     | Signal-based wakeup is idempotent. Standard pattern. |

### Carried forward as task #34 (Step 5)

- UFFD-on-KVM-memory composition needs a standalone PoC. Until then, the
  mmap-from-day-one path is the only supported zero-copy path.

---

## 3. Step-by-step plan

Each step lands as one or a few commits on `nvkvm-tables-refactor`. Don't
move on until the prior step's tests are green.

### Step 1 — Four-table skeleton + unit tests (task #30, IN PROGRESS)

- `src/qemu/nvkvm_tables.h` — typedefs and API.  ✅ done
- `src/qemu/nvkvm_tables.c` — implementation.    ✅ done (review needed)
- `tests/unit/test_tables.c` — pipe()/memfd_create only, no /dev/nvidia*.
- `tests/unit/Makefile` — add target.
- Tests must cover at minimum:
  - alloc/lookup/close, gen-id stale rejection
  - concurrent close vs in-flight ioctl (R3, R13)
  - GPA allocator fragmentation + coalesce (R10)
  - punch-hole adjacency consolidation, double-punch no-op (R14)
  - isolate-kill mid-op (R6)
  - dependency-ordered cleanup refusal (close-handle-while-mmap-exists)
  - cross-isolate handle clone race on first use (R12)
  - generation rollover (smoke test that gen bits work)
- Existing `test_handle.c`, `test_dispatch.c`, `test_frontend.c`,
  `test_isolate.c` must stay passing.
- Integration: `test_ioctl_fwd` must still pass (no behavior change yet).

### Step 2 — txn IDs end-to-end (task #31)

- Guest module: allocate u32 `txn_id` per outbound request, mirror on reply
  match.
- Virtio header carries `txn_id`; today's `req_id` becomes `txn_id` where
  inconsistent.
- QEMU reader thread: enqueue (txn_id, request) to dispatch pool, reply
  copies `txn_id` back.
- Stub: pass `txn_id` through transparently (round-tripped to QEMU; QEMU
  forwards to guest).
- In-flight tracking: u32 bitmap in guest module (and one in QEMU too if
  we ever need to reject duplicate inbound). Reject reuse of a still-
  outstanding id.

### Step 3 — Reverse open ownership + open-with-SCM_RIGHTS atomic reply (task #32)

- New protocol message `NVKVM_REQ_OPEN_ON_ISOLATE` (guest → QEMU →
  isolate):
  1. QEMU `handle_alloc(type)` → preliminary `handle_id`, qemu_fd=-1, !ready.
  2. QEMU sends "open path X for handle_id N" to isolate.
  3. Stub `open(path)` → local fd, `sendmsg(reply_with_local_fd_number +
     SCM_RIGHTS(fd))` atomically.
  4. QEMU `recvmsg` gets both. `handle_attach_qemu_fd(N, fd_from_scm)` →
     ready=true. `iso_hnd_link(isolate_id, N, stub_local_fd)`.
  5. QEMU returns `handle_id` to guest.
- On any failure: `handle_abort_open(N)` rolls back. Guest sees error.
- Delete legacy NVKVM_REQ_OPEN/CLOSE/IOCTL/MMAP/MUNMAP. Delete
  `nvkvm_fd_lookup` + `session->fds`. Delete the dup() workaround in
  `nvkvm_handle.c`. Migrate every dispatch site to `handle_acquire`.
- `nvidia-uvm` and memfd stay opened in QEMU (separate request types).

### Step 4 — Stub becomes table-free + auto-scaling thread pool (task #33)

- Wire format isolate↔QEMU references stub-local fd numbers, not handle_ids.
- Stub has no handle table. Optional small list: "ranges populated by
  WRITE_MEMORY_HANDLE, cleanup after this ioctl".
- One I/O thread reads commands FIFO; worker pool spawned via clone3, no
  TLS, small stack, auto-scales to ≥1 idle, capped at 256, idle exit
  after N seconds.
- Strict FIFO ack of close before any open whose fd number could collide.

### Step 5 — UFFD-on-KVM-memory PoC (task #34, parallel via sub-agent)

- Standalone test: `tests/poc/uffd_kvm.c`. Registers UFFD-WP on a chunk of
  KVM-backed guest RAM, performs heap→memfd promote sequence under guest
  write pressure. Verifies all guest writes during promote are deferred
  and replayed against the new mapping with no data loss.
- If KVM-UFFD on 6.8 has gaps: switch design to vCPU-pause path. Document
  what we hit.
- Independent of Steps 1–4; can develop in parallel.

### Step 6 — Cross-isolate handle clone (task #35)

- Guest module: per-process map of (handle_id → set of isolate_ids it
  knows about).
- On ioctl issued from a process whose isolate doesn't already have the
  handle: send `NVKVM_REQ_CLONE_HANDLE_TO_ISOLATE` to QEMU. QEMU
  SCM_RIGHTS the qemu_fd to the target isolate, target opens new local
  fd, registers in iso_hnd map. Returns the new local fd to QEMU.
- Double-checked locking around the first-use race.
- This is what makes fork() not break for memfd-backed CPU memory.

### Step 7 — Full vast.ai rebuild + cumemalloc on open driver without dup() (task #36)

- Build everything on the vast.ai node.
- Run `cumemalloc_test` inside the guest under open nvidia.ko.
- Keep NVKVM_* printk instrumentation in the open driver source for this
  run (it's our diagnostic surface).
- PASS = the referencing bug is fixed at the root.

### Step 8 — Cleanup (task #37)

- Move the NVKVM_* open-driver patches to a `debug-instrumentation`
  branch. Revert the open-driver source tree on the vast.ai node.
- Write `docs/ARCHITECTURE.md` documenting tables, invariants, txn
  protocol, fork semantics, cleanup ordering.
- Merge `nvkvm-tables-refactor` → `master`.

---

## 4. Test contract (must hold at every step)

- All four tables: `tests/unit/test_tables.c`. No /dev/nvidia* dependency.
- Existing unit tests pass.
- `test_ioctl_fwd` integration test passes after Step 3 lands (it might
  fail mid-Step 3 while migration is in progress; that's allowed but
  must be green by end of Step 3).
- `cumemalloc_test` inside the guest passes by end of Step 7.

---

## 5. Open commitments / "if you forget anything, remember this"

1. **dup() removed**. There is no dedupe of /dev/nvidiactl opens. If
   `pOSInfo != clientOSInfo` re-appears, it's a real referencing bug —
   don't paper over it.
2. **No fd_token**. Single ID space: `handle_id` only. If you see
   `nvkvm_fd_lookup` or `session->fds`, that's dead code from before the
   refactor.
3. **Stub has no handle table**. It receives `(stub_local_fd, op, args)`
   from QEMU and just runs syscalls. The only state stub keeps is
   threads + the optional WRITE_MEMORY_HANDLE cleanup list.
4. **handle_id is allocated in QEMU before stub knows about it**. Stub
   never invents IDs.
5. **QEMU always holds a qemu_fd via SCM_RIGHTS** for every stub-opened
   fd, EXCEPT during the open transaction window (bounded to one
   sendmsg).
6. **GPA placement is QEMU's choice**, dictated to the guest in the
   mmap response. Guest doesn't pick GPAs.
7. **The data path for CPU memory is mmap, not virtio R/W**.
   WRITE/READ_MEMORY_HANDLE exists only for buffers whose backing can't
   be swapped (vfio MMIO etc).

7b. **memfd-passthrough is the dominant fast path**. If a pointer the
    guest passes into an ioctl falls within an already-memfd-backed GPA
    window, *do not copy anything*. Resolve `vma → (handle_id, offset,
    size)` in the guest module, forward to QEMU, QEMU instructs the
    stub to rewrite the ioctl arg to its own VA for that same memfd.
    This is what every CUDA-allocated buffer hits. The userfaultfd
    path and WRITE_MEMORY_HANDLE are fallbacks for pointers that
    *aren't* memfd-backed yet (or can't be).
8. **Closed driver is not a target**. Open driver only. Validate against
   open driver in Step 7.

---

## 6. ChatGPT-scale goal (the actual point)

Everything in this plan exists to get a 7B+ LLM running in the guest.
Order of operations once the refactor is green:

1. cumemalloc_test passes inside guest, open driver, without dup() — gate.
2. cuLaunchKernel: vec_add test inside guest — gate.
3. Small model (llama.cpp Q4 ≤1 GB, or GPT-2 124M) — gate.
4. 7B LLM (Llama-3.1-7B Q4, Mistral-7B-Q4, similar). RTX 3060 12 GB so
   quantized.

If any of (2)/(3)/(4) reveals more forwarding gaps (channel alloc, GR
engine setup, push buffer mapping), file them as their own tasks and
keep the refactor branch clean.

---

## 7. Pointers to existing memory entries

- `event_os_event_paranoid_diag.md` — superseded by rmclient_validate_strict_fix.md
- `event_list_proof.md` — also superseded; root cause was strict-validate, not osUserHandleToKernelPtr
- `rmclient_validate_strict_fix.md` — confirmed 0x23 = NV_ERR_INVALID_CLIENT, the misnomer that misled us for days
- `multi_driver_support.md` — open driver is canonical; closed-driver compat is post-MVP
- `focus_open_driver_then_inference.md` — user's explicit direction: open driver, then inference, then closed driver

---

This document is the canonical plan. If anything in code disagrees with
this doc, the doc wins until updated.
