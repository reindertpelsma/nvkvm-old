# Mode-2 M3 — GSP-RPC emulation spec (the keystone)

Goal: make `RmInitAdapter` succeed by emulating just enough of the GSP-RM
message protocol to post **GSP_INIT_DONE**, so the stock driver believes it has
a live GPU. Entry state in [[mode2_m2_results.md]]. Structures below are from
the open 580.159.04 source (host `/root/open-gpu-kernel-modules` @ 580.159.04).

## How the driver and GSP share memory

`message_queue_cpu.c` allocates one **shared sysmem region** (`pSharedMemDesc`):

```
shared region (guest RAM, GPA = libos region 'pa'):
  [ command queue ]  CPU->GSP, size commandQueueSize (default 0x40000 = 256KB)
  [ status  queue ]  GSP->CPU, size statusQueueSize   (= pStatusQueue = base + cmdSize)
```

Each queue is an `msgq` SPSC ring whose backing store begins with headers
(`msgq/msgq_priv.h`):

```c
#define MSGQ_VERSION 0
typedef struct {            // 32 bytes, written by the TX side
    NvU32 version;          // = 0
    NvU32 size;             // backing-store bytes, page aligned
    NvU32 msgSize;          // entry size, power-of-2, >=16 (GSP_MSG_QUEUE_ELEMENT_SIZE_MIN)
    NvU32 msgCount;         // number of entries
    NvU32 writePtr;         // msg id of next slot (TX increments)
    NvU32 flags;            // 1 => "i want to swap RX"
    NvU32 rxHdrOff;         // offset of msgqRxHeader from backing-store start
    NvU32 entryOff;         // offset of entries from backing-store start
} msgqTxHeader;
typedef struct { NvU32 readPtr; } msgqRxHeader;  // written by the RX side
```

- **Command queue**: CPU is TX. The driver's `msgqTxCreate` writes the cmd-queue
  txHeader and entries; GSP is RX (reads readPtr/writes... no — GSP writes the
  cmd-queue rxHeader.readPtr to ack consumption).
- **Status queue**: GSP is TX. **We (fake GSP) must write the status-queue
  txHeader** (msgqInit/msgqTxCreate-equivalent) so the driver's `msgqRxLink`
  (in `GspStatusQueueInit`) stops spinning, then write message elements and bump
  writePtr.

`msgqRxLink` spinning for the GSP to init its TX header is the **observed stall**
(BAR0 0xbb0080 liveness poll alongside). Note `GspStatusQueueInit` uses a near
NV_U32_MAX timeout if `IS_EMULATION` — so it waits ~forever.

## How we learn the GPA

`kgspProgramLibosBootArgsAddr_TU102` writes the **LibOS init-args descriptor
GPA** to mailboxes:
```
GPU_REG_WR32(NV_PGSP_FALCON_MAILBOX0 0x110040, lo32(addr));
GPU_REG_WR32(NV_PGSP_FALCON_MAILBOX1 0x110044, hi32(addr));
```
The descriptor is an array (max 4096 entries) of
(`libos_init_args.h`):
```c
typedef struct {
    NvU64 id8;   // id tag (ascii-ish), identifies the region
    NvU64 pa;    // physical address (GPA)
    NvU64 size;  // bytes
    NvU8  kind;  // NONE / CONTIGUOUS / RADIX3
    NvU8  loc;   // NONE / SYSMEM / FB
} LibosMemoryRegionInitArgument;   // (padded)
```
QEMU captures the two mailbox writes → boot-args GPA → `pci_dma_read` the array
→ the message-queue shared region is the SYSMEM/CONTIGUOUS entry (match by id8;
confirm by size == commandQueueSize+statusQueueSize). `pa` = command-queue base;
status queue = `pa + commandQueueSize`.

## The message element

`message_queue_priv.h`:
```c
typedef struct GSP_MSG_QUEUE_ELEMENT {
    NvU8  authTagBuffer[16];   // CC only — zero (CC is OFF)
    NvU8  aadBuffer[16];       // CC only — zero
    NvU32 checkSum;            // 32-bit XOR/sum over the element must be 0
    NvU32 seqNum;              // sequence number (matches CPU expectation)
    NvU32 elemCount;           // # of msgSize elements this message spans
    rpc_message_header_v rpc;  // 8-aligned; { header_version, signature,
                               //   length, function, rpc_result, rpc_result_private,
                               //   sequence, cpuRmGfid, ... , data[] }
} GSP_MSG_QUEUE_ELEMENT;
```
GSP_INIT_DONE = an element whose `rpc.function = NV_VGPU_MSG_EVENT_GSP_INIT_DONE`,
`rpc.rpc_result = NV_OK`, body = `rpc_init_done_v17_00` (the negotiated message
version). `_kgspRpcRecvPoll` special-cases GSP_INIT_DONE (kernel_gsp.c:2324-2329).

## Implementation steps (Rust core + thin C shell)

1. **[C shell] Capture + read.** Trap MAILBOX0/1 writes → boot-args GPA;
   `pci_dma_read` the libos region array; locate the queue region; read the
   command-queue txHeader + the init-RPC element(s) the driver posted. LOG it
   all. (M3-step-1, verifiable now — proves the GPA path + gives ground truth.)
2. **[Rust] Init the status-queue TX header** at status-queue base
   (version=0, size, msgSize=GSP_MSG_QUEUE_ELEMENT_SIZE_MIN, msgCount, writePtr=0,
   rxHdrOff, entryOff) so `msgqRxLink` succeeds.
3. **[Rust] Decode the command queue.** Parse each GSP_MSG_QUEUE_ELEMENT's
   rpc_message_header → the NV_VGPU_MSG_* the driver sent (kgspSendInitRpcs:
   SET_REGISTRY etc.). Ack via the cmd-queue rxHeader.readPtr.
4. **[Rust] Post GSP_INIT_DONE.** Encode the element (zero authTag/aad, correct
   seqNum, elemCount, rpc header with function=GSP_INIT_DONE result=NV_OK,
   compute checkSum so the running sum is 0), write at the status-queue write
   slot, bump txHeader.writePtr.
5. **[C shell] Interrupt.** Raise the emulated MSI-X ([[mode2_interrupt_delivery]])
   so `_kgspRpcRecvPoll` wakes; also answer the GSP-RM heartbeat mailbox.

When `_kgspRpcRecvPoll` consumes GSP_INIT_DONE, `kgspWaitForRmInitDone` returns
NV_OK → `RmInitAdapter` succeeds → **the stock driver believes the GPU is live.**
That is the M3 proof-of-concept gate; M4 then triages the post-init RPC stream
and shims it into the Mode-1 core.

## Caveats / open

- **Checksum + seqNum** exact algorithm: read `GSPRPC`/`message_queue_cpu.c`
  `_gspMsgQueueCheckSum` / sequence handling before encoding (must match or the
  driver rejects the element).
- **Confidential Compute OFF** → authTag/aad unused (don't encrypt).
- **msgq ring semantics**: reuse the `msgq` library logic (`src/common/shared/msgq`)
  for pointer math rather than reimplementing — port to Rust.
- This is per-(device) state; multi-GPU keeps it per-instance.
