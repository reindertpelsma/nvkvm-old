# ⊘ CORRECTION to `mode2_scratchpad_vas.md` — the customer did NOT evaporate

**Status:** owner correction, 2026-08-11. ⚠ **Read this before acting on the parent doc or on
`w232`'s finding.** I shelved the scratchpad on a misreading and the owner caught it within minutes.

---

## 1. What I got wrong

`w232` found an owner design of **2026-08-07** (`ce_executor_tree.md`) saying:

> *"★ It is **NOT** needed for the scrubber — the CPU branch **reaches** both operands."*

⇒ I read that as *"the scratchpad has no customer"* and shelved it.

⊘ **That line answers CORRECTNESS. The owner's question was PERFORMANCE:**

> *"How do you then do a kernel-initiated CE that has one operand in physical RAM and one in real GPU
> RAM, since most performant is doing real CE? Same for scrub in GPU space if the page is still
> referenced. **There must be a channel with a VA on unprivileged to execute that.**"*

★★★ **"Reaches" is not "reaches fast enough."** And under the standing bar — *"a product ready for
production, not a research paper"* — **throughput is a requirement, not a nice-to-have.**

---

## 2. ★★ The C measured this exact cost, and named this exact fix

`mode2_userbuf_vidmem_passthrough.md`: the emulated copy engine moved **~95–107 MB/s** where the real
one does **~7 GB/s** — **≈70×** — because every byte funnelled through a CPU loop
(`nvkvm_gpu_emul.c:3755-3844`, 4 bytes at a time). Its stated fix: **(B) forward the CE to the host
GPU.**

⇒ **The C hit the owner's problem and named the owner's answer.** A CPU-side scrubber is not
*incorrect*; it is *unshippable* at framebuffer scale.

★ **And the second case is sharper: scrubbing GPU memory that is STILL REFERENCED.** It cannot be
discarded — zeros must be written *through something that can reach it*. CPU-side means the aperture
window: slow **and** size-limited. ⇒ Real copy engine, or nothing.

---

## 3. ★★★ It does NOT violate the boundary — I collapsed two different things

`w232` established a real, deliberate boundary (`ce_executor_tree.md`, `[measured]` E10, 2026-08-07):

> *"The **CPU branch** cannot execute in the isolate … `ce_copy(Ours)` must keep refusing there …
> it is the security boundary **refusing to leak guest memory into the sandbox**, working as designed."*

⊘ **That forbids a CPU memcpy inside the isolate. It says nothing about a GPU channel inside the
isolate** — driving real GPU channels **is the isolate's entire job**, and under a real CE the bytes
never enter the isolate's address space at all: **the GPU moves them.**

| shape | verdict |
|---|---|
| scratchpad + **real CE** (GPU moves the bytes) | ★ **compatible with the boundary** |
| scratchpad + **CPU memcpy** in the isolate | ⊘ **exactly what the boundary forbids** |

⇒ Collapsing these is what produced my wrong conclusion.

---

## 4. The corrected status

| piece | verdict |
|---|---|
| **separation** — our ring/USERD/semaphore out of any guest-nameable space | ★ **BUILT** (`8776992`), and the hazard was **measured exploitable** first (`R30 arm C`) |
| **scratchpad for real kernel-initiated CE** | ★★ **NEEDED — for throughput, which is a product requirement.** I was wrong to shelve it |
| **scratchpad for CPU copies** | ⊘ correctly refused; that is the boundary |
| **the addressing scheme** — `scratchpad VA = offset + GPGA`, sparse, no allocator | ★ **still the right design**, and it is what makes *both* operands nameable in one space: host RAM via `OS_DESCRIPTOR`, GPU memory via its object |

---

## 5. ⚠ Sequencing — right, needed, and NOT next

The user-process path is stuck at `RING-VA-UNBOUND`, and that is the **compute** road: `cuCtxCreate`
→ `cup2` → first arithmetic. The scratchpad is the **kernel** road, and it is a **throughput**
requirement rather than a correctness blocker for reaching compute at all.

⇒ **Sequenced after the guest executes something** — with one falsifiable exception worth measuring
cheaply: **if the CPU-side scrubber is slow enough to stall init itself**, it becomes a blocker rather
than a performance item. That is measurable from a boot we already take.

⊘ **And do not build it before something needs it.** Building a mechanism for a problem we do not have
is the same shape as the vacuous gate the owner stopped one day earlier: *establish the need, then
build.*
