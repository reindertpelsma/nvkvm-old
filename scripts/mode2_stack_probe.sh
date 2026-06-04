#!/bin/bash
# mode2_stack_probe.sh — run cup2 in background, let it block at cuCtxCreate,
# then dump every thread's kernel stack (ground truth for the wait site).
set -u
NVVER=580.159.04
GUESTLIB=/usr/local/nvidia-guest/lib
LIBCUDA="$GUESTLIB/libcuda.so.$NVVER"

dmesg -C 2>/dev/null || true
# preload the ioctl snoop too (if built) so os-event fds are logged in the SAME
# run we decode the pollset of -> lets us correlate os-event fds <-> poll fds.
SNOOP=""
[ -f /tmp/nvioctl_snoop.so ] && SNOOP="/tmp/nvioctl_snoop.so "
LD_PRELOAD="${SNOOP}$LIBCUDA" LD_LIBRARY_PATH="$GUESTLIB" /tmp/cup2 >/tmp/cup2.out 2>/tmp/snoop.err &
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
echo "=== POLL FD DECODE (what fds the do_poll threads actually wait on) ==="
for t in /proc/$CPID/task/*; do
    tid=$(basename "$t")
    w=$(cat "$t/wchan" 2>/dev/null)
    case "$w" in *poll*) ;; *) continue;; esac
    python3 - "$tid" <<'PYEOF'
import sys,struct
tid=sys.argv[1]
try:
    sc=open(f"/proc/{tid}/syscall").read().split()
except Exception as e:
    print(f"tid {tid}: syscall read failed {e}"); sys.exit()
# format: nr arg0 arg1 arg2 arg3 arg4 arg5 sp pc
nr=sc[0]
if nr not in ("7","271"):  # poll / ppoll
    print(f"tid {tid}: syscall nr={nr} (not poll)"); sys.exit()
buf=int(sc[1],16); nfds=int(sc[2],16)
fds=[]
try:
    with open(f"/proc/{tid}/mem","rb") as m:
        m.seek(buf)
        data=m.read(min(nfds,64)*8)
    for i in range(min(nfds,64)):
        fd,ev,rev=struct.unpack_from("<ihh",data,i*8)
        fds.append(f"fd{fd}(ev=0x{ev&0xffff:x})")
except Exception as e:
    print(f"tid {tid}: mem read failed {e}"); sys.exit()
print(f"tid {tid}: poll nfds={nfds} -> {' '.join(fds)}")
PYEOF
done
echo "=== fd links for ALL cup2 fds (identify RM/UVM/eventfd) ==="
for f in $(ls /proc/$CPID/fd 2>/dev/null); do
    l=$(readlink /proc/$CPID/fd/$f 2>/dev/null)
    case "$l" in *nvidia*|*uvm*) echo "fd$f -> $l";; esac
done
echo "=== os-event fds (from snoop, this run) ==="; grep "ALLOC_OS_EVENT" /tmp/snoop.err 2>/dev/null
echo "=== event->fd bindings ==="; grep "RM_ALLOC EVENT" /tmp/snoop.err 2>/dev/null
echo "=== dmesg tail ==="; dmesg | tail -8
kill -9 $CPID 2>/dev/null
