# mode2_diag — Mode-2 host-vs-guest ioctl diagnostic

Find where the guest (Mode-2, m2fwd) diverges from a native host run, to pinpoint
cuCtxCreate (and later) bugs without guessing.

## nvioctl_trace.so  (LD_PRELOAD ioctl tracer — no driver changes)
Decodes every NVIDIA RM ioctl: NVOS54 (RM_CONTROL, NR 0x2A) and NVOS21/64 (RM_ALLOC, NR 0x2B),
logging `cmd`/`class`, `paramsSize` (= the buffer libcuda allocated), `status`, and params ptr.
Runs identically on the guest test and a native host test.

    gcc -shared -fPIC -O2 -o nvioctl_trace.so nvioctl_trace.c -ldl
    # host:  LD_PRELOAD=./nvioctl_trace.so NVTRACE=/tmp/host_trace.txt  ./cup2_host
    # guest: LD_PRELOAD=./nvioctl_trace.so NVTRACE=/tmp/guest_trace.txt ./cup2

Then diff (normalize out ASLR params= addrs):
    sed -E 's/ params=0x[0-9a-f]+//' host_trace.txt  | grep -E 'CTRL|ALLOC' > h.norm
    sed -E 's/ params=0x[0-9a-f]+//' guest_trace.txt | grep -E 'CTRL|ALLOC' > g.norm
    diff g.norm h.norm
A control whose `status` differs (guest NV_OK vs host NOT_SUPPORTED) = a faked-success bug:
the host skips the param copyout, the guest copies garbage over libcuda's buffer.

## report.py  — joins the guest dmesg (instrumented driver) with the QEMU RPC log

## Wins so far (2026-06-06)
- This diff found NV2080_CTRL_CMD_GPU_QUERY_ECC_STATUS (0x2080012f) returning fake NV_OK on the
  guest vs NOT_SUPPORTED on the no-ECC GeForce host -> fixed (QEMU returns 0x56).
- And NVLINK_GET_NVLINK_STATUS (0x20803002) likewise (no NVLink on GeForce).
