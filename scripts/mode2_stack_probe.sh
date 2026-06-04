#!/bin/bash
# mode2_stack_probe.sh — run cup2 in background, let it block at cuCtxCreate,
# then dump every thread's kernel stack (ground truth for the wait site).
set -u
NVVER=580.159.04
GUESTLIB=/usr/local/nvidia-guest/lib
LIBCUDA="$GUESTLIB/libcuda.so.$NVVER"

dmesg -C 2>/dev/null || true
LD_PRELOAD="$LIBCUDA" LD_LIBRARY_PATH="$GUESTLIB" /tmp/cup2 >/tmp/cup2.out 2>&1 &
CPID=$!
echo "cup2 pid=$CPID"
# wait until it has printed past totalMem (i.e. entered cuCtxCreate)
for i in $(seq 1 20); do
    grep -q "totalMem" /tmp/cup2.out 2>/dev/null && break
    kill -0 $CPID 2>/dev/null || { echo "cup2 exited early"; cat /tmp/cup2.out; exit 1; }
    sleep 1
done
sleep 6   # let it settle into the blocking wait
echo "=== cup2.out so far ==="; cat /tmp/cup2.out
echo "=== per-thread kernel stacks ==="
for t in /proc/$CPID/task/*; do
    tid=$(basename "$t")
    st=$(cat "$t/stack" 2>/dev/null)
    cm=$(cat "$t/comm" 2>/dev/null)
    # only show threads parked in nvidia/rm/poll/wait frames
    if echo "$st" | grep -qiE "nv|rm_|os_|poll|wait|sema|gsp|UVM|uvm"; then
        echo "--- tid=$tid comm=$cm wchan=$(cat $t/wchan 2>/dev/null) ---"
        echo "$st"
    fi
done
echo "=== all wchans ==="
for t in /proc/$CPID/task/*; do echo "$(basename $t): $(cat $t/comm) wchan=$(cat $t/wchan 2>/dev/null)"; done
echo "=== dmesg tail ==="; dmesg | tail -15
kill -9 $CPID 2>/dev/null
