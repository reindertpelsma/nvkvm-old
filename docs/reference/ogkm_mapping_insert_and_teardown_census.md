# ogkm mapping inserts and their teardown routes — where a GPU VA gets recorded, and how it is removed

> **STATUS — LIVE, 2026-08-13.** Measured read-only against
> `research_clones/ogkm-580.159.04/` (**version-specific: 580.159.04**). All nvoc HAL bindings
> resolved for **GA106 + GSP-client** from the generated dispatch tables, not from the `.c` alone.
> Scope: **RM proper** (`src/nvidia/`). The **UVM plane** (`kernel-open/nvidia-uvm/`) and the
> **GSP RPC surface census** were dispatched to parallel lanes; where their results are not folded
> in below, §7 says so explicitly and marks the gap.
>
> Answers the owner's question of 2026-08-12: *"does ogkm issue rm ioctls and then automatically
> insert mappings in its own bookkeeping, if so which ones. iterate them, all sites, so we also
> know the cleanup routes"*.

---

## 0. THE DIRECT ANSWER

**YES.** Every RM operation that establishes a GPU VA records it in RM's own bookkeeping, in up to
four parallel structures at once.

But the count that matters is not the number of insert sites. It is this:

| | count | |
|---|---|---|
| distinct bookkeeping structures | **4** in RM proper (+ UVM's own, separate) | §3 |
| insert call sites across them | **~14** | §3 |
| **PTE-writing choke points** | **exactly 2** | §2 |
| **GSP-crossing insert sites on a GA106 GSP-client** | ⊘ **ZERO** | §2 |
| CPU-RM-local (invisible to a GSP) insert sites | **all of them** | §2 |

★★★★★ **On a GA106 GSP client, the entire user-visible GPU-VA mapping plane is CPU-RM-local. Not
one of these sites sends an RPC to the GSP.** The client RM owns the VA space, owns the mapping
bookkeeping, and **writes the GMMU page tables itself**. The GSP is told the **page-directory base**
and nothing else about what is mapped inside it.

⇒ **This is the structural answer to the owner's *"is it PDB or RM?"* fork: it is RM, and RM is on
the guest side of our boundary.** A GSP emulator that watches only the wire will see **none** of
these mappings. See §2 for the citation chain and §6 for what that means for the CE-operand fault.

⊘ **Do not over-read this.** It says our *populate source (1)* — bind-time RPC/ioctl bindings —
**structurally cannot cover the user-VA mapping plane on a GSP client**. It does **not** by itself
prove the `0x1_20000000` CE source operand is unmapped; a sibling lane's `NV0080_CTRL_CMD_DMA_GET_PTE_INFO`
probe answers that empirically and should be believed over this inference. §6 states the hypothesis
and its falsifier.

---

## 1. ★★★ THE TEARDOWN ROUTES — kernel-guaranteed vs best-effort

This is the half that no empirical probe can supersede, and the half the owner's standing rule
depends on: *"if we need some kind of pinning that we can unpin it or that we have a cleanup
mechanism later."*

### 1.1 The kernel-guaranteed backstop, end to end

There is **one** guaranteed teardown chain, and it is anchored on **file-descriptor release**, not
on anything userspace chooses to do:

```
close(fd)  /  process death  /  SIGKILL
  └─ nvidia_close                          kernel-open/nvidia/nv.c:2208   (.release = nvidia_close, nv.c:245)
      └─ nvidia_close_callback                                    nv.c:2123
          └─ rm_cleanup_file_private
              └─ RmFreeUnusedClients            src/nvidia/arch/nvalloc/unix/src/osapi.c:2914 (call), :453 (def)
                  └─ pRmApi->DisableClients   → rmapiDisableClientsWithSecInfo   rmapi/alloc_free.c:1587
                      └─ serverMarkClientListDisabled            resserv/src/rs_server.c:995
                          └─ serverFreeClient(bDisableOnly=NV_TRUE)          rs_server.c:1019
              └─ serverFreeDisabledClients                        osapi.c:2918 / rs_server.c:1046
                  └─ serverFreeResourceTree                                  rs_server.c:1100
                      └─ clientFreeResourceTree → clientFreeResource_IMPL    resserv/src/rs_client.c
                          ├─ clientUnmapResourceRefMappings   rs_client.c:832   ← CPU mappings
                          ├─ _clientUnmapBackRefMappings      rs_client.c:833
                          ├─ _clientUnmapInterMappings        rs_client.c:836   ← ★ GPU-VA mappings
                          └─ _clientUnmapInterBackRefMappings rs_client.c:837   ← ★ GPU-VA mappings
```

★ **Why this is kernel-guaranteed, in ogkm's own words** — `osapi.c:463-468`, the comment directly
above the loop in `RmFreeUnusedClients`:

> *"The 'nvfp' pointer uniquely identifies an open instance in kernel space and the kernel interface
> layer guarantees that we are not called before the associated nvfp descriptor is closed. We can
> thus safely free abandoned clients with matching 'nvfp' pointers."*

The Linux kernel forces `.release` on every open fd at process teardown, including under `SIGKILL`.
⇒ **Every route in §1.2 that terminates in `clientFreeResource_IMPL` fires unconditionally.**

⚠ **ONE CAVEAT, and it is a real one for a mirror: the free may be DEFERRED.**
`osapi.c:2917-2918` runs the immediate free **only** under `if (!pSys->bUseDeferredClientListFree)`;
otherwise the actual `serverFreeResourceTree` is punted to a worker (`osapi.c:2924`, *"Start the
deferred free callback if necessary"*). ⇒ **Teardown is guaranteed to HAPPEN but not guaranteed to
have happened by the time the fd is closed.** A mirror that keys "unjoin" on fd-close will unjoin
*before* RM does. Key on the observable teardown, or tolerate the window.

### 1.2 Route table — every teardown, marked

| # | What it removes | Teardown site | Reached from | **Guarantee** |
|---|---|---|---|---|
| T1 | `CLI_DMA_MAPPING_INFO` from `VirtualMemory::pDmaMappingList` (btree) | `intermapDelDmaMapping` — `rmapi/mapping_list.c:291`, unlink at `:326` | `virtmemUnmapFrom_IMPL` `mem_mgr/virtual_mem.c:1788` | ★ **KERNEL-GUARANTEED** via T2 |
| T2 | `RsInterMapping` from `pMapperRef->interMappings` | `refRemoveInterMapping` — `resserv/src/rs_resource.c:656`; driver `serverInterUnmapMapping` `rs_server.c:2358` | **(a)** explicit `NV_ESC_RM_UNMAP_MEMORY_DMA` (0x58, `escape.c:640`); **(b)** `_clientUnmapInterMappings` `rs_client.c:836`; **(c)** `_clientUnmapInterBackRefMappings` `rs_client.c:837` | (a) best-effort · **(b)(c) ★ KERNEL-GUARANTEED** |
| T3 | The GMMU PTEs + the VAS mapping-tree node | `gvaspaceUnmap_IMPL` — `mem_mgr/gpu_vaspace.c:2252`; record removed at `:2274` (`_gvaspaceMappingRemove`), PTEs at `:2284/:2289` (`mmuWalkSparsify` / `mmuWalkUnmap`) | `dmaFreeMap` → `dmaUpdateVASpace_*` → `virt_mem_allocator_gm107.c:2373` — called from `virtmemUnmapFrom_IMPL:1773` | ★ **KERNEL-GUARANTEED** via T2 |
| T4 | Whole-VAS sweep: **every** `GVAS_MAPPING` still in a VA block | `gpu_vaspace.c:1763-1772` — `btreeEnumStart` over `pVASBlock->pMapTree`, `gvaspaceUnmap` per node, **looping until the tree is empty** | VAS block free / `gvaspaceDestruct` (`:914-922`, `eheapTraverse` then `eheapDestruct`) | ★★ **KERNEL-GUARANTEED — this is the VAS-teardown backstop** |
| T5 | `VA_LIST` entry for an engine ctx buffer (`ENGINE_CTX_DESCRIPTOR::vaList`, `globalCtxBufferVaList[]`, `*.vAddrList`) | `vaListRemoveVa` — `mem_mgr/vaddr_list.c:430`; driver `kgraphicsUnmapCtxBuffer` `gpu/gr/kernel_graphics.c:2037` (paired with `dmaUnmapBuffer_HAL`) | `shrkgrctxDetach_IMPL` → `kgrctxUnmapBuffers_HAL` `kernel_graphics_context.c:3699`, gated `if (!kgrctxShouldCleanup(...)) return;` `:3691` | ★ **KERNEL-GUARANTEED** (channel free cascade) — **but see the gate below** |
| T6 | The `VA_LIST` container itself | `vaListDestroy` × 10 — `kernel_graphics_context.c:3619-3632` | `shrkgrctxDestructUnicast_IMPL` `:3602` | ★ **KERNEL-GUARANTEED** |
| T7 | VA range reservation in the VAS eheap | `pGVAS->pHeap->eheapFree` — `gpu_vaspace.c:1816`, `:3434` | `gvaspaceFree` / VAS destruct | ★ **KERNEL-GUARANTEED** |
| T8 | `RsCpuMapping` (CPU BAR mapping — **not** a GPU VA; listed for completeness) | `clientUnmapMemory_IMPL` `rs_client.c:877`; sweep `clientUnmapResourceRefMappings` `rs_client.c:1144` | `clientFreeResource_IMPL:832`; also `rs_server.c:1298` | ★ **KERNEL-GUARANTEED** |

★★ **T5's gate is the one to read carefully.** `kgrctxShouldCleanup` resolves **unconditionally** to
`kgrctxShouldCleanup_KERNEL` (macro, `generated/g_kernel_graphics_context_nvoc.h:771` — no chip
variance), whose whole body is `return gpuIsClientRmAllocatedCtxBufferEnabled(pGpu);`
(`kernel_graphics_context.c:2489-2495`). On a GSP client that is **`NV_TRUE`**
(`gpu_registry.c:153-156`, enclosing `else if (IS_GSP_CLIENT(pGpu) || RMCFG_FEATURE_PLATFORM_GSP)`
taken when the `RMClientRmAllocatedCtxBuffer` regkey is absent). ⇒ **On our target the gate is OPEN
and T5 fires.** On a non-GSP monolithic driver it would be closed and physical RM would do the
unmapping instead. **This teardown is configuration-dependent, and the configuration is what makes
it ours.**

### 1.3 What is **best-effort** — do not build on these

| Route | Why best-effort |
|---|---|
| `NV_ESC_RM_UNMAP_MEMORY_DMA` (0x58) — `arch/nvalloc/unix/src/escape.c:640` | A userspace ioctl. **Issues nothing under `SIGKILL`.** |
| `NV_ESC_RM_FREE` on an individual `VirtualMemory` / `Memory` object | Same. |
| UVM's `UVM_UNMAP_EXTERNAL` / `UVM_FREE` | Same class — userspace-initiated. (UVM's own file-release backstop is a separate question; see §7.) |

⇒ ★★★ **This confirms the prior owner ruling, from ogkm's source rather than from inference: an
unjoin must key on the KERNEL's teardown — routes T2(b)/T2(c) via `clientFreeResource_IMPL`, with
T4 (the whole-VAS `pMapTree` sweep) as the backstop — and never on a userspace free.** Both of
those are reachable with no cooperation from the guest process.

---

## 2. ★★★ GSP-CROSSING vs CPU-RM-LOCAL — the decisive column

### 2.1 The switch: `bSplitVasManagementServerClientRm` defaults to TRUE on a GSP client

`src/nvidia/src/kernel/gpu/gpu_registry.c:171-186`. Enclosing condition, quoted in full:

```c
if ((pGpu->bSriovEnabled && !gpuIsWarBug200577889SriovHeavyEnabled(pGpu)) ||
    RMCFG_FEATURE_PLATFORM_GSP || IS_GSP_CLIENT(pGpu))
{
    if (osReadRegistryDword(pGpu, NV_REG_STR_RM_SPLIT_VAS_MGMT_SERVER_CLIENT_RM, &data32) == NV_OK)
        pGpu->bSplitVasManagementServerClientRm = (data32 == ..._ENABLED);
    else
        pGpu->bSplitVasManagementServerClientRm = NV_TRUE;      // ← :181, the default
}
```

The regkey `"RMSplitVasMgmtServerClientRm"` (`interface/nvrm_registry.h:1231`) is not set by
default. ⇒ **`gpuIsSplitVasManagementServerClientRmEnabled()`** (`generated/g_gpu_nvoc.h:5456-5458`,
a plain field read) **is TRUE on a GA106 GSP client.**

### 2.2 What that switch does to `NV_ESC_RM_MAP_MEMORY_DMA`

`virtmemConstruct_IMPL`, `mem_mgr/virtual_mem.c:458-470` — enclosing `if (IS_VIRTUAL(pGpu) || IS_GSP_CLIENT(pGpu))`:

```c
bRpcAlloc = !(gpuIsSplitVasManagementServerClientRmEnabled(pGpu) ||
              (bSriovFull && (bBar1VAS || pVirtualMemory->bFlaVAS)));
```

On a bare-metal GSP client `bSriovFull` is false, so **`bRpcAlloc = !TRUE = NV_FALSE`**, stored at
`:587`. Consequences in `virtmemMapTo_IMPL`:

| line | condition | on GA106 GSP-client | effect |
|---|---|---|---|
| `virtual_mem.c:1421` | `if (!pMemory->bRpcAlloc \|\| gpuIsSplitVas...Enabled(pGpu))` (sysmem/EGM/fabric arm) | **TRUE** | `dmaAllocMap` runs **locally** (`:1426`); mapping registered locally (`:1430`) |
| `virtual_mem.c:1463` | same predicate (`ADDR_FBMEM` arm) | **TRUE** | `dmaAllocMap` runs **locally** (`:1466`); registered at `:1472` |
| `virtual_mem.c:1520` | `if (pMemory->bRpcAlloc)` — the **only** guard on `NV_RM_RPC_MAP_MEMORY_DMA` (`:1522`) | ⊘ **FALSE** | ★★★ **NO RPC IS SENT** |
| `virtual_mem.c:1850` | `if (pMemory->bRpcAlloc && ...)` — the **only** guard on `NV_RM_RPC_UNMAP_MEMORY_DMA` (`:1863`) | ⊘ **FALSE** | ★★★ **NO RPC ON UNMAP EITHER** |

⚠ Note the comments at `:1456` and `:1491` — `// !IS_VIRTUAL(pGpu) && !IS_GSP_CLIENT(pGpu)` —
which annotate the local-mapping branch as *not* taken on a GSP client. **Those comments are stale
with respect to the code they annotate.** The `|| gpuIsSplitVasManagementServerClientRmEnabled(pGpu)`
disjunct was added to both predicates and re-opens the branch. Reading the comment instead of the
predicate inverts the answer. (Same class as this campaign's *"the `.c` you read is not the code
that runs"*, one layer up: **the comment you read is not the predicate that runs**.)

### 2.3 Who writes the page tables — and it is not the GSP

`dmaAllocMapping` binds to **`dmaAllocMapping_GM107`** on GA106: the nvoc dispatch
(`generated/g_virt_mem_allocator_nvoc.c:299-307`) installs the no-op `_46f6a7` variant **only** for
`ChipHal: T234D | T264D` (Tegra), and `dmaAllocMapping_GM107` in the `else`. Chain:

```
dmaAllocMapping_GM107      gpu/mem_mgr/arch/maxwell/virt_mem_allocator_gm107.c:317
  └─ dmaUpdateVASpace_*  → gvaspaceMap                                        :2605
      └─ gvaspaceMap_IMPL                        mem_mgr/gpu_vaspace.c:2190
          ├─ _gvaspaceMappingInsert  :2230   ← the bookkeeping insert (§3, family D)
          └─ mmuWalkMap              :2239   ← ★ the MMU walker runs HERE, in CLIENT RM
              └─ memmgrMemWrite      gpu/mmu/gmmu_walk.c:813   ← the actual PTE store
```

★★★ **`mmuWalkMap` is called in client RM with no RPC anywhere in the path.** The PTEs are written
by `memmgrMemWrite` from the CPU side. **The GSP never sees the mapping.**

⚠ **Scope this correctly.** `gvaspaceMap_IMPL:2219` guards the bookkeeping insert with
`if (!flags.bRemap)`; the remap path (MODS compression release, Windows BAR1 clobber) skips the
insert and only rewrites PTEs. Neither applies to us, but a sweep keyed on `_gvaspaceMappingInsert`
would miss remaps.

### 2.4 The GSP-crossing column, per family

| family | structure | GSP-crossing on GA106 GSP-client? | what makes it so |
|---|---|---|---|
| **D** | `GVAS_BLOCK::pMapTree` + the PTEs | ⊘ **CPU-RM-LOCAL** | `mmuWalkMap` at `gpu_vaspace.c:2239`, no RPC in path |
| **A** | `VirtualMemory::pDmaMappingList` | ⊘ **CPU-RM-LOCAL** | `bRpcAlloc == NV_FALSE` ⇒ `virtual_mem.c:1520` false |
| **B** | `RsResourceRef::interMappings` | ⊘ **CPU-RM-LOCAL** | pure resserv bookkeeping, `rs_server.c:2266`; no RPC |
| **C** | `VA_LIST` (engine ctx buffers) | ⊘ **CPU-RM-LOCAL** | `bClientRmAllocatedCtxBuffer == NV_TRUE` (`gpu_registry.c:153-156`) ⇒ **client** RM allocates *and* maps ctx buffers |
| **E** | VAS eheap reservations | ⊘ **CPU-RM-LOCAL** | `pGVAS->pHeap`, `gpu_vaspace.c:588-595` |
| — | **UVM's own page-tree writes** | ⊘ **not even RM-local** | see §5 / §7 |

★ **What DOES cross to the GSP** is the *page-directory base and the server's own reserved PDEs* —
`NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES` (`ctrl90f1.h:268`, handled at
`gpu_vaspace.c:4405 / :4431`), plus channel-allocation RPCs carrying the VAS. ⇒ **The GSP learns
WHERE the page tables are. It never learns WHAT IS IN THEM.**

---

## 3. THE INSERT SITES — bounded enumeration

Small enough to list in full (~14). Grouped by structure, **ranked by the §2 column** — all
CPU-RM-local, so ranked by proximity to the page tables instead.

### Family D — `GVAS_BLOCK::pMapTree` — **THE CHOKE POINT** (1 site)

| trigger | insert | teardown | paired? |
|---|---|---|---|
| Any RM GPU-VA map: `NV_ESC_RM_MAP_MEMORY_DMA`, ctx-buffer maps, BAR1/BAR2 setup, HWPM streamout | `_gvaspaceMappingInsert` — called at `gpu_vaspace.c:2230`, body `:4735`, `btreeInsert` at `:4781` | `_gvaspaceMappingRemove` `:2274` (body `:4824`) **and** the whole-block sweep `:1763-1772` | ★ **YES**, doubly |

★★★ **Every RM-side GPU VA in the driver funnels through this one function.** If we mirror exactly
one thing, mirror this. Its teardown is paired *twice over*: a per-range remove and an
enumerate-until-empty sweep at VAS-block free (T4) — which is precisely the "VAS-teardown backstop"
the owner's ruling asks for, already present in the source we are mirroring.

### Family A — `VirtualMemory::pDmaMappingList` (btree keyed by DMA offset range) — 5 sites

Store: `mapping_list.c:276` `btreeInsert(pNode, &pVirtualMemory->pDmaMappingList)`; node keyed
`[RM_ALIGN_DOWN(dmaOffset, alignment) .. RM_ALIGN_UP(dmaOffset+size, alignment)-1]` (`:241-243`).
Read back by `CliGetDmaMappingInfo` (`:391`) — **the known-positive for this sweep, and it is
reachable**: used on the error-notifier path at `gpu/fifo/kernel_channel.c:2033`, and at
`:4027`, `disp/disp_sw.c:146`, `mem_mgr/method_notification.c:312,581`,
`gr/kernel_sm_debugger_session_ctrl.c:118`, `rmapi/nv_gpu_ops.c:5915`.

| # | insert site | when | teardown | paired? |
|---|---|---|---|---|
| A1 | `virtual_mem.c:1430` | sysmem / EGM / fabric map | `:1603` on error; `:1788` on unmap | ★ YES |
| A2 | `virtual_mem.c:1472` | vidmem (`ADDR_FBMEM`) map | same | ★ YES |
| A3 | `virtual_mem.c:1541` | after `NV_RM_RPC_MAP_MEMORY_DMA` — **dead on GA106 GSP-client** (§2.2) | same | ★ YES (but unreached) |
| A4 | `virtual_mem.c:1793` | partial-unmap **left** remainder re-register | `:1821` on error; later `:1788` | ★ YES |
| A5 | `virtual_mem.c:1802` | partial-unmap **right** remainder re-register | `:1829` on error; later `:1788` | ★ YES |

⚠ **A4/A5 are the shape that breaks a naive mirror.** A partial `UNMAP_MEMORY_DMA` does not remove
one record — it removes the record and **re-inserts up to two new ones** for the surviving
remainders, plus a third throwaway (`pDmaMappingInfoUnmap`, `:1754`) that is freed at `:1813`. A
mirror keyed on "unmap ⇒ delete" corrupts state here. Same split logic mirrored in resserv at
`rs_server.c:2323/:2333`.

### Family B — `RsResourceRef::interMappings` (+ two back-ref lists) — 5 sites

Insert `refAddInterMapping` — `resserv/src/rs_resource.c:613`, appends to **three** lists at once:
`pMapperRef->interMappings` (`:627`), `pMappableRef->interBackRefsMappable` (`:632`),
`pContextRef->interBackRefsContext` (`:642`).

| # | insert site | when | teardown | paired? |
|---|---|---|---|---|
| B1 | `rs_server.c:2266` (`serverInterMap`) | every `MAP_MEMORY_DMA`, **before** `resMapTo` | `:2294` on error; `:2358`; `rs_client.c:836/837` | ★ YES |
| B2 | `rs_server.c:2323` | partial-unmap left remainder | `:2349` / `:2358` | ★ YES |
| B3 | `rs_server.c:2333` | partial-unmap right remainder | `:2352` / `:2358` | ★ YES |
| B4 | `mem_mgr/mem_fabric.c:216` | fabric-memory map | `:167`, `:280` | ★ YES |
| B5 | `mem_mgr/mem_multicast_fabric.c:2655` | multicast-fabric map | `:2495`, `:2718` | ★ YES |

★ B1 is the **outermost** hook: it fires for every inter-resource map before RM-specific code runs,
and `rs_client.c:1074-1076` asserts all three lists are empty at ref destruction — **resserv already
enforces the leak check we would otherwise have to write.**

### Family C — `VA_LIST` (RM-internal engine context buffers) — 3 sites

Store: `mem_mgr/vaddr_list.c:335` `vaListAddVa`, refcounted (`:371` re-add path). Containers:
`ENGINE_CTX_DESCRIPTOR::vaList`, `KernelGraphicsContextUnicast::globalCtxBufferVaList[]`,
`*.vAddrList`.

| # | insert site | when | teardown | paired? |
|---|---|---|---|---|
| C1 | `gr/kernel_graphics.c:2009` (in `kgraphicsMapCtxBuffer_IMPL:1913`) | GR ctx buffer mapped into a channel VAS | `kgraphicsUnmapCtxBuffer` `:2037` (`vaListRemoveVa`) via T5/T6 | ★ YES |
| C2 | `fifo/kernel_channel.c:3763` (in `kchannelSetEngineContextMemDesc_IMPL:3692`) | engine ctx memdesc bound to channel | channel free cascade | ★ YES |
| C3 | `fifo/kernel_channel.c:3942` (in `kchannelMapEngineCtxBuf`) | engine ctx buffer VA cached | `:3910` find / channel free | ★ YES |

★★★ **C is DISABLED for a CUDA process.** `kgrctxMapCtxBuffers_IMPL`, `kernel_graphics_context.c:1613-1615`:

```c
pGVAS = dynamicCast(pKernelChannel->pVAS, OBJGVASPACE);
if (gvaspaceIsExternallyOwned(pGVAS))
    return NV_OK;                    // ← RM maps NOTHING
```

Same early-out at `:1884` and `:2072`; `kgraphicsMapCtxBuffer_IMPL:1934` outright **asserts**
`!gvaspaceIsExternallyOwned(pGVAS)`. Under CUDA the VAS is **externally owned by UVM**, so on that
path family C never runs and **UVM establishes those VAs itself**. See §5.

### Family E — VAS eheap VA reservations — ~2 sites

`pGVAS->pHeap->eheapAlloc` — `gpu_vaspace.c:1559` (`gvaspaceAlloc`), `:767` / `:4599` / `:4628`
(reserved ranges). Teardown `eheapFree` `:1816`, `:3434`; container destroyed `:914-922`.
This is **VA-range ownership**, not VA→backing, and is listed for completeness. ★ Paired.

---

## 4. UNPAIRED INSERTS AND UNPAIRED TEARDOWNS

⚠ Both are findings. Here is what the sweep found, and what it would have missed.

**No unpaired insert was found in families A–E.** Every insert site above has a reachable teardown
and, for A/B, an error-path teardown as well. That is a stronger result than expected and it is
worth stating plainly: **RM's mapping bookkeeping is symmetric by construction, and resserv asserts
it** (`rs_client.c:1074-1076`).

**Near-misses / asymmetries worth recording:**

1. ⊘ **`virtmemDestruct_IMPL` does NOT walk `pDmaMappingList`.** `virtual_mem.c:652-717` frees the
   memdesc and the heap allocation and **never touches the btree**. The dma mappings are removed
   *earlier*, by resserv's `_clientUnmapInterMappings` (`rs_client.c:836`) driving
   `virtmemUnmapFrom`, before `objDelete` (`rs_client.c:~880`) reaches the destructor.
   ⇒ **The teardown is real but lives in a different layer than the insert.** A mirror that hooks
   "VirtualMemory freed" and expects to see mapping removals there will see none.

2. ⊘ **`_virtmemCopyConstruct` deliberately drops the mapping list on `DUP_OBJECT`.**
   `virtual_mem.c:254-255`: `// Mappings do not follow virtual memory object` /
   `pDstVirtualMemory->pDmaMappingList = NULL;`
   ⇒ **`NV_ESC_RM_DUP_OBJECT` on a `VirtualMemory` is NOT a mapping-insert site.** It duplicates the
   *object* and the VA *reservation* refcount (`vaspaceIncAllocRefCnt`, `:264`, under
   `if (bIncAllocRefCnt)` where `bIncAllocRefCnt = pSrcVirtualMemory->bReserveVaOnAlloc && !pSrcMemory->bRpcAlloc`
   at `:247`), **not** the mappings. Anyone expecting DUP to publish mappings will find zero and
   must not read that zero as "no mappings exist".
   ★★ **RESOLVED, and the resolution is itself the lesson: there is NO `vaspaceDecAllocRefCnt`.**
   `vaspaceIncAllocRefCnt` → `gvaspaceIncAllocRefCnt_IMPL` (`gpu_vaspace.c:1943-1961`) does one
   thing: `pVASpaceBlock->refCount++` on the eheap block (`:1959`). The **decrement has no function
   of its own** — it is inline at the top of the VA-block free path, `gpu_vaspace.c:1741-1745`:
   ```c
   if (pMemBlock->refCount > 1) { pMemBlock->refCount--; return NV_OK; }   // ← early-out, block survives
   ```
   ⇒ **The pair is symmetric in behaviour and asymmetric in NAME.** A name-based sweep
   (`grep Inc… / grep Dec…`) reports a leak here that does not exist. ⚠ This is the exact shape the
   brief warned about — *a sweep is scoped to what it names* — and it fired inside this very
   document's first draft. **Recorded as a false-positive class, not as a defect.**

3. ⚠ **`_gvaspaceMappingRemove` re-inserts.** `gpu_vaspace.c:4876` and `:4884` call
   `_gvaspaceMappingInsert` from *inside* the remove, to re-record the surviving head/tail of a
   partially-unmapped range (and `:4912` re-inserts the original node on the multi-GPU path). ⇒
   **"remove" is not monotone.** Any mirror driven off remove events must handle re-insertion, or a
   partial unmap will silently drop the surviving remainders from our table.

**No dead teardown was found**, with one qualification: the RPC-side teardown
`NV_RM_RPC_UNMAP_MEMORY_DMA` (`virtual_mem.c:1863`) is **unreachable on a GA106 GSP client**
because its guard `pMemory->bRpcAlloc` is false (§2.2). It is live for the vGPU-guest configuration.
⇒ **Dead for us, not dead in general** — a version/configuration-scoped finding, not a defect.

---

## 5. WHAT THIS SWEEP DELIBERATELY DOES NOT COVER — and why it matters most

★★★ **Under CUDA, the VA space is EXTERNALLY OWNED, and families A/C are bypassed for the buffers
that matter.**

`gvaspaceIsExternallyOwned_IMPL` (`gpu_vaspace.c:2024`) gates:

| site | behaviour when externally owned |
|---|---|
| `kernel_graphics_context.c:1614-1615` | `kgrctxMapCtxBuffers` returns `NV_OK` having mapped **nothing** |
| `kernel_graphics_context.c:1884`, `:2072` | same early-out |
| `kernel_graphics.c:1934` | hard assert — `kgraphicsMapCtxBuffer` **may not be called** |
| `falcon/kernel_falcon.c:167`, `:218` | ctx-buffer map skipped |
| `fifo/kernel_channel.c:2200`, `:3920` | channel/ctx VA handling diverges |

⇒ **There are exactly TWO GMMU page-table writers on this system:**
1. **RM's MMU walker** — `mmuWalkMap` via `gvaspaceMap_IMPL:2239`, for RM-owned VA spaces.
2. **UVM's own page-tree code**, for externally-owned VA spaces — **which does not go through any
   structure in §3 and does not appear in this census at all.**

For a CUDA workload, writer (2) dominates. **That is the single most important scoping statement in
this document**, and it is why the UVM lane's result (§7) is required before this census can be
called complete.

---

## 6. ⇒ WHICH ROW COULD BE "THE SOURCE THAT DID NOT FIRE" FOR THE CE OPERAND

Live context: the wall reproduces in 82 ioctls with host faults at **`0x1_20000000`** (a CE `src`
operand) and **`0x7_00100000`**, both `FAULT_PTE / ACCESS_TYPE_VIRT_READ`, and the same binary
passes natively.

**Ranked candidates, most to least likely:**

1. ★★★ **UVM's externally-owned-VAS page-table writes (§5, writer 2).** Not in any RM structure;
   never an RPC; never on our wire. If the failing workload is CUDA/UVM-backed, this is the prime
   suspect **by construction**, and no amount of RM-side mirroring reaches it.
2. ★★★ **Family D — `gvaspaceMap_IMPL` (§3).** RM's own walker writes the PTE from the CPU with no
   RPC. **A GSP emulator sees nothing.** If the CE source operand was mapped by an RM path
   (`MAP_MEMORY_DMA` from a non-UVM client, or an RM-internal buffer), this is where it happened and
   where we are blind.
3. ★★ **Family A/B — `NV_ESC_RM_MAP_MEMORY_DMA` (§3).** The user-visible trigger for (2).
   Observable as an **ioctl** but **not** as an RPC.
4. ★ **Family C — engine ctx buffers.** Only if the VAS is *not* externally owned.

⚠ ⊘ **This is a hypothesis, not a conclusion, and I am explicitly not asserting it.** Three
hypotheses on this campaign have been refuted in two days. **The falsifier is cheap and already in
flight**: the sibling `NV0080_CTRL_CMD_DMA_GET_PTE_INFO` probe at the exact faulting addresses. If
it reports `0x1_20000000` **mapped**, then the fault is *not* a missing mapping, candidates 1–4 are
all wrong, and this section is void — while §1 (teardown) and §2 (the GSP-crossing structure) stand
regardless.

★ What survives either outcome: **populate source (1) — "bind-time RPC/ioctl bindings" — cannot be
complete on a GSP-client architecture, because the bindings it names do not cross to the GSP.** Our
only wire-visible signal for an RM-established mapping is **source (2), the observed page-table
write itself** (`memmgrMemWrite`, `gmmu_walk.c:813`) — which is exactly what
`mode2_address_table.md` §5 already measured as the surviving source when both invalidate transports
read zero. ⇒ **This census independently corroborates that §5 correction from ogkm's source, by a
different route.**

---

## 7. ⊘ WHAT I COULD NOT DETERMINE, AND WHAT WOULD DETERMINE IT

1. ⊘ **The UVM plane is not enumerated here.** `kernel-open/nvidia-uvm/` — `uvm_ext_gpu_map`,
   `uvm_va_range`/`uvm_va_block`, `uvm_user_channel`, `uvm_mem_map_gpu_*`, page-tree ranges — was
   dispatched to a parallel lane whose result had not returned when this document was written.
   **§5 makes it the highest-value missing piece.** ⇒ *What would determine it:* that lane's table,
   folded in as family F, with the same GSP-crossing and kernel-guarantee columns. Specifically
   needed: **does `uvm_release` (file-release) tear down external mappings unconditionally**, making
   UVM's teardown kernel-guaranteed like RM's?
2. ⊘ **The full `NV_RM_RPC_*` / `ROUTE_TO_PHYSICAL` surface** was likewise dispatched and is not
   folded in. §2 establishes the *negative* (no RPC on the mapping path) from the call-site guards,
   which is the load-bearing direction; the *positive* inventory of what else crosses is not here.
3. ✔ **CLOSED during this pass** — `vaspaceIncAllocRefCnt`'s decrement is inline at
   `gpu_vaspace.c:1741-1745`, not a separate function. See §4 item 2.
4. ⊘ **BAR1/BAR2 and `kbus` mapping bookkeeping** (`gpu/bus/arch/*/kern_bus_*.c`) is out of scope.
   It uses family D underneath (`kern_bus_gm107.c:3435` calls `dmaAllocMapping_HAL`), so it is
   covered *transitively*, but its own per-BAR records are not enumerated.
5. ⊘ **P2P / BAR1-P2P mappings** (`dmaAllocBar1P2PMapping_HAL`, `virtual_mem.c:1292`;
   freed `:1596`, `:1711`) are noted as paired but not analysed. Not on a single-GPU GA106 path.
6. ⊘ **Confidential-computing and MIG variants** were not resolved. Several sites carry
   `swizzId` / `KMIGMGR_SWIZZID_INVALID` parameters; MIG changes VAS ownership.

### What this enumeration would MISS, stated plainly

- **Anything that writes PTEs without going through `gvaspaceMap_IMPL`** — i.e. all of UVM's
  externally-owned-VAS path (§5), and the `flags.bRemap` path inside `gvaspaceMap` itself
  (`:2219`), which rewrites PTEs while skipping the bookkeeping insert.
- **Mappings established before RM bookkeeping exists** — early boot / BAR2 bootstrap
  (`gpu/mmu/bar2_walk.c`) uses a separate walker.
- **Any site reached only under a HAL variant not bound on GA106.** I resolved `dmaAllocMapping`,
  `kgrctxUnmapBuffers`, and `kgrctxShouldCleanup` against the generated dispatch tables; I did
  **not** resolve every function named above. A `_gv100`/`_PHYSICAL`/`_46f6a7` variant elsewhere in
  these paths could be dead on our target and I would not have noticed.
- **Anything whose insert is spelled differently.** This sweep keyed on `btreeInsert`,
  `listAppendNew`, `vaListAddVa`, `refAddInterMapping`, `eheapAlloc`. **A structure using a
  different container idiom would be invisible to it**, and an absence here must not be read as a
  measured zero.

### Sweep integrity — known-positives, both PASSED

- ★ Absent-`_IMPL` control: `grep -rn 'kchannelCtrlCmdGetClassEngineid_IMPL'` → found at
  `gpu/fifo/kernel_channel.c:2923` **and** in the nvoc dispatch table
  `generated/g_kernel_channel_nvoc.c:254`. **PASS.**
- ★ Reachability control: `grep -rn 'CliGetDmaMappingInfo'` → found the definition
  (`rmapi/mapping_list.c:391`), the declaration (`inc/kernel/rmapi/mapping_list.h:176`), and **7
  distinct call sites** including the error-notifier path at `gpu/fifo/kernel_channel.c:2033`
  (inside the `2019-2075` range named in the brief). **PASS.**

---

## 8. ONE-LINE SUMMARY FOR THE DESIGN

**Mirror `gvaspaceMap_IMPL` / `gvaspaceUnmap_IMPL` (family D), key the unjoin on
`clientFreeResource_IMPL` (T2b/T2c) with the `pMapTree` whole-block sweep (T4) as the backstop,
and accept that on a GSP-client architecture NONE of it is visible to us as an RPC — so our only
wire-side signal remains the observed page-table write.**
