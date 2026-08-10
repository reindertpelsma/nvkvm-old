# Mode-2 channel ownership: passthrough vs emulated, and how completions cross the line

**Status:** owner ruling, 2026-08-10. Continues `mode2_doorbell_chid.md` (owner, 2026-06-05) and
`mode2_forwarding_model.md`. Written here because this repo already hosts the June ruling it extends;
the `kayfabe` tree should carry a pointer to it.

⚠ **This page records an owner brainstorm plus corrections made against measurement and against
NVIDIA's own driver.** Where a claim is inference it says so. Where the assistant was wrong, it says
that too — several of the corrections below are of the assistant, not of the owner.

---

## 1. The split, and the discriminator

> **Guest-userspace-managed channels are always passthrough.** No exception has been found that we
> need to implement.
>
> **Kernel channels are mostly completely emulated.** Passthrough of a kernel channel is possible
> *only* if every action on it is guaranteed unprivileged and executable against host **userspace**
> — no physical addressing, no privileged registers. That is unlikely for most kernel channels.

★ **The discriminator is who PUSHES the commands** — who writes the pushbuffer and advances `GP_PUT`
— **not** whose memory the work touches, and not whose VA space the operands live in.

This dissolves a question the assistant had posed badly ("which side is UVM on, since its channels
are kernel-allocated but operate on user memory?"). Owner's answer, and it is correct: libcuda and
the kernel driver do not both drive one ring — that would be a data race on `GP_PUT` — so **for any
channel it is unambiguous who pushes**. UVM's channels are built and driven by the guest kernel's UVM
module, so they are **kernel** channels; and because UVM's migration copies use physical addressing,
they also fail the "guaranteed unprivileged" test ⇒ **emulated**.

⇒ The C already implements exactly this boundary, in one line
(`src/qemu/nvkvm_gpu_emul.c:4265`):

> *"**User-CE / GR channels are EXCLUDED (the host executes + releases those for real).**"*

The corresponding split for the objects on a channel (`mode2_doorbell_chid.md`, 2026-06-05):

| object | class | treatment |
|---|---|---|
| pushbuffer, GPFIFO ring, referenced data, completion semaphore | **chid-INDEPENDENT** | `OS_DESCRIPTOR`-pin the guest pages, `map_dma` FIXED into the host channel's VAS **at the guest VAs** |
| USERD (`GP_PUT`), doorbell token | **chid-DEPENDENT** | translate |

---

## 2. The doorbell is one lookup and one store

`mode2_doorbell_chid.md` item 3, and the C's implementation
(`nvkvm_gpu_emul.c:4220`, `:9160`):

```c
uint32_t tok = c->token_valid ? c->host_token : s->m2_gr_token;
stl_le_p((uint8_t *)s->m2_usermode_qva + 0x90, tok);   /* the whole fast path */
```

Receive the token, look it up, ring the translated one, VM-enter. **No inspection, no parse.** The
host's usermode doorbell page is mapped by any CUDA process, so the store needs no privilege. The
VM exit + enter is inherent; the body must be the most optimised short code in the system.

⊘ **Correction of the assistant:** "zero exits if we can dictate the chid" was wrong framing. The exit
is inherent to trapping the write. The named optimisation is different and already recorded: make
USERMODE a `KVM_MEM_READONLY` memslot so **reads and PTIMER are native and only writes fault**.

Only channels we own completely — USERD *and* buffers *and* ring *and* semaphores — take the slow
path of manual inspection, and for those **no direct host ring happens at all**.

---

## 3. Wakeup: three routes, and only one of them is hot

⊘ **Correction of the assistant.** The assistant worried that "every completion costs a GSP RPC
round-trip". That is wrong, and the owner's instinct is right. There are three distinct routes:

1. ★ **A semaphore in memory, polled by the waiter.** This is the hot path and the overwhelming
   majority of real work: **zero interrupts, zero VM exits, nothing for us to do.** An LLM in steady
   state lives here — and a kernel launch costs **zero ioctls**, so the control plane is not involved
   at all after `cuCtxCreate`.
2. **GSP message queue + `POST_EVENT (0x1003)` + the GSP stall vector** → guest ISR → `kgspService`
   drains → `osNotifyEvent` → the registered os-event fd wakes. This is the **blocking-sync /
   os-event** path: context setup, explicit `cuStreamSynchronize` with blocking sync, teardown. **Cold
   by construction**, so its cost is not a parity concern.
3. **Non-stall notifier interrupts** (e.g. notifier 35, which is what makes `nvidia-smi`'s process
   list work). A separate vector from (2); do not conflate them.

⇒ The interrupt round-trip is inevitable *when an interrupt is genuinely required*. The optimisation
is not making the round-trip cheaper — it is that **most completions never need one**, because the
guest is polling a semaphore the hardware wrote.

⚠ **Open, and worth checking rather than assuming:** a measured boot showed `completions: 4 announced,
**179 UNVECTORED (work done, nothing told the guest)**`. Before building more announcement machinery,
establish how many of those 179 any guest actually asked to be told about. Announcing work nobody
requested notification for is the *opposite* of the rule above.

---

## 4. Scheduling: the guest expresses intent; the GPU and GSP schedule

Owner's model: *the host schedules the guest, just as the host schedules vCPUs as threads.* A guest
determining relative priority **among its own channels** is a performance property, not a correctness
one — correct workloads synchronise explicitly rather than depending on scheduler behaviour.

**Owner's question — "does the guest preempt channels and do the context switch, or is that the
GSP/GPU's job?" — answer: it is the GPU's and GSP's job**, which is the outcome the owner preferred.
On Turing+ with GSP-RM:

- The CPU-side driver **allocates** TSGs and channels, sets timeslice and preemption **mode**
  (`NV2080_CTRL_CMD_GR_SET_CTXSW_PREEMPTION_MODE` — WFI vs CILP; we already see this control), and
  calls `GPFIFO_SCHEDULE` to make a channel **resident on a runlist**.
- The **GPU's host/FIFO engine executes the runlist** and performs context switches; **GSP-RM** owns
  runlist submission and scheduling policy.

⇒ The guest never performs a context switch itself. It expresses **intent**, and intent is exactly
what we can accept without honouring precisely. This is what makes host-side scheduling — shared with
other host CUDA processes and other VMs, and therefore unpredictable — **hideable**.

⚠ **The real risk is not scheduling; it is the guest's WATCHDOG.** If the guest believes a channel
should have run and it did not (because the host was busy elsewhere), the guest's RM can declare a
channel timeout and enter robust-channel recovery — which *is* correctness-visible. ⇒ Scheduling
latency is a perf property **until it trips a guest timeout**, at which point it becomes a
correctness one. Track the guest's timeout policy, not its priorities.

---

## 5. The scrubber, and the general shape of an emulated kernel channel

★ **Owner's reframing, and it is the right one:** a scrub means **"ensure future access to this memory
reads blank"**, not "make this memory blank now". That turns one operation into three cases:

| the page is… | correct action | why the guest cannot tell |
|---|---|---|
| **unallocated / unbacked** | **no-op** | the next allocation yields empty pages anyway |
| **in use** | **really scrub it** | the very next read must return zero and the page cannot be deallocated |
| **dangling — no references** | **free it on the host** | the next use allocates a fresh empty page |

All three produce the same guest-observable behaviour. ⇒ **Applies to kernel-initiated scrubs only;
guest-userspace scrubs are passthrough and none of this applies.**

**Two invariants this rests on. State them; do not assume them.**
1. **Our backing is zero on first touch.** If a page we hand the guest is not zeroed, case 1 is a
   data leak, not an optimisation.
2. **Host RM scrubs on allocation.** Case 3 depends on it.

⚠⚠ **The one genuinely dangerous part — signalled, because getting it backwards is a security bug,
not a correctness bug.** Cases 1 and 3 are *optimisations* and case 2 is the *only* one that is always
safe. Misclassifying an in-use page as unbacked or dangling hands the guest **stale contents of
somebody else's memory**. ⇒ **The default must be case 2 (really scrub), with cases 1 and 3 requiring
POSITIVE PROOF of unbackedness or of danglingness.** "We have no record of this page" is not proof of
either — this tree has already had orphan-generation lifetime bugs where our record and reality
disagreed.

### The general pattern for an emulated kernel channel

> We write the semaphore, and optionally send an interrupt if an os-event is set. If a **real** host
> operation is required, we track the **host's** completion separately and bridge it to the guest
> semaphore we manage by hand.

★ This is the general shape, and it makes precise what "manual completion handling" means: **the
guest-facing semaphore is ours to write because the channel is ours; the host-facing completion is a
separate object we observe.** Nothing about it is forgery — forging is writing a **user** channel's
completion, which the host was supposed to release.

⊘ **One constraint the brainstorm did not state, and it is load-bearing: the wait must not run on the
vCPU thread.** "Spin on the semaphore, or an eventfd if it takes too long" is right in shape, but a
doorbell trap runs on the vCPU with the BQL held. A spin or a blocking wait there stalls the guest and
can wedge it. ⇒ The bridge belongs on the reactor (which exists in the tree and has never been
reached), and the doorbell path must return promptly having only *started* the host operation. An
audit found **nine** sites on the doorbell path that can block, only two of which are bounded.

---

## 6. Faults: prevent, don't handle

`mode2_doorbell_chid.md` item 4, owner, 2026-06-05:

> **"Gate the ring** on 'this channel's working set fully mapped' (naive ring today → `cuInit=999`
> because the host faults on unmapped referenced VAs)."

⇒ The design answer to host faults is **do not fault**: refuse to ring until every VA the channel will
reference is mapped in the host channel's VAS. The `kayfabe` port has this gate built and it is
currently **vacuous** — the shell passes an empty working set. Closing it is a **prerequisite** of
passthrough, not a follow-up: under passthrough it is the only thing standing between a hostile ring
and an Xid.

⚠ **Scope, honestly:** prevention covers the **static** working set. It does not cover the guest
changing a mapping under a running channel, nor UVM demand-migration. A host→guest fault **delivery**
path remains undesigned; it is simply not a precondition for first compute.

---

## 7. Standing instruction, from the same June page

> *"**Quarantine** the QEMU-side `nvkvm_chan_execute()` pushbuffer-parse/sema-fake path during the real
> build (**it masks whether the host actually ran the work**)."*

⇒ The `kayfabe` equivalents are the CPU CE executor on the **forwarded** arm, the private GP cursor,
and completion-writing on the forwarded arm. **Quarantine, do not delete** — the emulated arm is
legitimate per §5. Without this, a green run cannot distinguish *"the host ran it"* from *"we faked
it"*, which is precisely the ambiguity that cost this campaign five rungs.
