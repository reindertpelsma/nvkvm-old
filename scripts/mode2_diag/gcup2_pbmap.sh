#!/usr/bin/env bash
set -euo pipefail

# Dump selected live guest cup2 user pages into a host-side m2pbmap file consumed by:
#   -device nvkvm-gpu-emul,...,m2pbmap=/tmp/m2_pbmap.txt
#
# By default this includes the CUDA driver mappings whose pages can appear in
# CE pushbuffers: /dev/nvidiactl, /dev/nvidia-uvm, and /dev/zero staging maps.
# Additional addresses can be forced with:
#   NVKVM_PBMAP_VAS='0xaddr 0xaddr...'
# Fast pause-phase refreshes can preserve prior rows while adding exactly the
# pages needed for DtoH staging:
#   NVKVM_PBMAP_MERGE_EXISTING=1 NVKVM_PBMAP_SCAN_MAPS=0 \
#   NVKVM_PBMAP_ZERO_ALL_PAGES=1 NVKVM_PBMAP_FAULT_ZERO_WRITE=1
# If NVUVM_SHADOW=1 is used with nvioctl_trace.so, CUDA_HTOD shadow records
# from NVKVM_UVM_TRACE are also converted into device-VA -> guest-GPA rows.
#
# Output format is one hex tuple per line:
#   <guest userspace/GPU VA> <guest physical address> <size>

out=${1:-/tmp/m2_pbmap.txt}
tmp=$(mktemp)
old_tmp=$(mktemp)
trap 'rm -f "$tmp" "$old_tmp"' EXIT

ahead_pages=${NVKVM_PBMAP_AHEAD_PAGES:-64}
forced_vas_arg=${NVKVM_PBMAP_VAS:-}
uvm_trace=${NVKVM_UVM_TRACE:-/tmp/guest_uvm_trace.txt}
pid_file=${NVKVM_CUP2_PID_FILE:-/tmp/cup2_live.pid}
dmesg_uvm=${NVKVM_PBMAP_DMESG_UVM:-1}
fault_zero_write=${NVKVM_PBMAP_FAULT_ZERO_WRITE:-0}
scan_maps=${NVKVM_PBMAP_SCAN_MAPS:-1}
zero_all_pages=${NVKVM_PBMAP_ZERO_ALL_PAGES:-0}
merge_existing=${NVKVM_PBMAP_MERGE_EXISTING:-0}
allow_empty=${NVKVM_PBMAP_ALLOW_EMPTY:-0}

ssh vg "sudo python3 - $(printf '%q' "$ahead_pages") $(printf '%q' "$forced_vas_arg") $(printf '%q' "$uvm_trace") $(printf '%q' "$pid_file") $(printf '%q' "$dmesg_uvm") $(printf '%q' "$fault_zero_write") $(printf '%q' "$scan_maps") $(printf '%q' "$zero_all_pages")" >"$tmp" <<'PY'
import os
import subprocess
import struct
import sys

PAGE = os.sysconf("SC_PAGE_SIZE")
AHEAD_PAGES = int(sys.argv[1], 0)
UVM_TRACE = sys.argv[3]
pid_path = sys.argv[4]
DMESG_UVM = sys.argv[5] not in ("0", "false", "False", "no", "No")
FAULT_ZERO_WRITE = sys.argv[6] not in ("0", "false", "False", "no", "No")
SCAN_MAPS = sys.argv[7] not in ("0", "false", "False", "no", "No")
ZERO_ALL_PAGES = sys.argv[8] not in ("0", "false", "False", "no", "No")
with open(pid_path, "r", encoding="ascii") as f:
    pid = int(f.read().strip())

def cmdline_for(pid):
    try:
        raw = open(f"/proc/{pid}/cmdline", "rb").read()
    except OSError:
        return ""
    return raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()

def child_pids(pid):
    out = []
    try:
        raw = open(f"/proc/{pid}/task/{pid}/children", "r", encoding="ascii").read()
    except OSError:
        raw = ""
    for tok in raw.split():
        try:
            out.append(int(tok))
        except ValueError:
            pass
    return out

def has_nvidia_maps(pid):
    try:
        with open(f"/proc/{pid}/maps", "r", encoding="ascii") as f:
            data = f.read()
    except OSError:
        return False
    return ("/dev/nvidia" in data or
            "libcuda.so" in data or
            "/usr/local/nvidia-guest" in data)

def select_cuda_pid(root):
    if not os.path.exists(f"/proc/{root}"):
        return root
    seen = set()
    queue = [root]
    candidates = []
    while queue:
        cur = queue.pop(0)
        if cur in seen:
            continue
        seen.add(cur)
        cmd = cmdline_for(cur)
        score = 0
        if any(name in cmd for name in (
                "cup2_pause", "simple_store_pause", "matmul_pause")):
            score += 4
        if has_nvidia_maps(cur):
            score += 2
        if score:
            candidates.append((score, cur, cmd))
        queue.extend(child_pids(cur))
    if candidates:
        candidates.sort(reverse=True)
        chosen = candidates[0][1]
        if chosen != root:
            print(f"using child CUDA pid {chosen} instead of wrapper pid {root}",
                  file=sys.stderr)
        return chosen
    return root

pid = select_cuda_pid(pid)
proc_maps = f"/proc/{pid}/maps"
proc_pagemap = f"/proc/{pid}/pagemap"
proc_mem = f"/proc/{pid}/mem"
if not os.path.exists(proc_maps):
    print(f"process {pid} exited before pbmap export", file=sys.stderr)
    sys.exit(75)

all_maps = []
maps = []
eager_windows = []
zero_maps = []
with open(proc_maps, "r", encoding="ascii") as f:
    for line in f:
        fields = line.split()
        lo_s, hi_s = fields[0].split("-", 1)
        lo, hi = int(lo_s, 16), int(hi_s, 16)
        path = " ".join(fields[5:]) if len(fields) >= 6 else ""
        all_maps.append((lo, hi, path))
        if ("/dev/nvidiactl" in path or
                "/dev/nvidia-uvm" in path or
                path.startswith("/dev/zero")):
            maps.append((lo, hi))
            if path.startswith("/dev/zero"):
                eager_windows.append(lo)
                zero_maps.append((lo, hi))

forced_vas = []
for tok in sys.argv[2].replace(",", " ").split():
    try:
        forced_vas.append(int(tok, 0))
    except ValueError:
        pass

for addr in forced_vas:
    for lo, hi, _path in all_maps:
        if lo <= addr < hi and (lo, hi) not in maps:
            maps.append((lo, hi))

def in_maps(addr):
    return any(lo <= addr < hi for lo, hi in maps)

def in_zero_maps(addr):
    return any(lo <= addr < hi for lo, hi in zero_maps)

want_pages = set()

def add_page(addr):
    if addr and in_maps(addr):
        want_pages.add(addr & ~(PAGE - 1))

def add_command_window(addr):
    base = addr & ~(PAGE - 1)
    for i in range(AHEAD_PAGES):
        add_page(base + i * PAGE)

def parse_kv_line(line):
    vals = {}
    for tok in line.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        vals[k] = v
    return vals

def parse_int(v):
    return int(v, 0)

htod_shadows = []
uvm_external = []
seen_uvm_external = set()

def add_uvm_external(base, length):
    if not base or not length:
        return
    row = (base, length)
    if row in seen_uvm_external:
        return
    seen_uvm_external.add(row)
    uvm_external.append(row)

def parse_trace_lines(lines):
    for line in lines:
        if "UVM_MAP_EXTERNAL " in line:
            line = line[line.index("UVM_MAP_EXTERNAL "):]
            vals = parse_kv_line(line)
            try:
                base = parse_int(vals["base"])
                length = parse_int(vals["len"])
            except (KeyError, ValueError):
                continue
            add_uvm_external(base, length)
            continue
        if not line.startswith("CUDA_HTOD "):
            continue
        vals = parse_kv_line(line)
        shadow = vals.get("shadow", "")
        if not shadow.startswith("0x"):
            continue
        try:
            dst = parse_int(vals["dst"])
            bytes_n = parse_int(vals["bytes"])
            shadow_va = parse_int(shadow)
            shadow_len = parse_int(vals.get("len", vals["bytes"]))
        except (KeyError, ValueError):
            continue
        if dst and shadow_va and bytes_n and shadow_len:
            htod_shadows.append((dst, bytes_n, shadow_va, shadow_len))

try:
    with open(UVM_TRACE, "r", encoding="ascii", errors="replace") as f:
        parse_trace_lines(f)
except FileNotFoundError:
    pass

if DMESG_UVM:
    try:
        dmesg = subprocess.check_output(["dmesg"], text=True, errors="replace")
        parse_trace_lines(dmesg.splitlines())
    except (OSError, subprocess.CalledProcessError):
        pass

for base, length in uvm_external:
    end = base + length
    for lo, hi, _path in all_maps:
        if hi <= base or lo >= end:
            continue
        row = (lo, hi)
        if row not in maps:
            maps.append(row)
    add_command_window(base)

def parse_pushbuffer(data):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    for i in range(1, len(words)):
        prev = words[i - 1]
        cur = words[i]
        candidates = (
            (prev << 32) | cur,
            (cur << 32) | prev,
            ((prev & 0xffff) << 32) | cur,
            ((cur & 0xffff) << 32) | prev,
            ((prev & 0x7fffffff) << 32) | cur,
            ((cur & 0x7fffffff) << 32) | prev,
        )
        for va in candidates:
            if va >= 0x700000000000:
                add_command_window(va)
    ce_sem_addr = 0
    host_sem_addr = 0
    cr_sem_addr = 0
    w = 0
    while w < len(words):
        hdr = words[w]
        w += 1
        secop = (hdr >> 29) & 0x7
        cnt = (hdr >> 16) & 0x1fff
        maddr = (hdr & 0xfff) << 2
        if secop not in (1, 3, 5) or cnt == 0 or cnt > 0x400:
            continue
        for j in range(cnt):
            if w >= len(words):
                return
            d = words[w]
            w += 1
            m = maddr if secop == 3 else maddr + j * 4
            if m == 0x240:
                ce_sem_addr = (ce_sem_addr & 0xffffffff) | ((d & 0x01ffffff) << 32)
            elif m == 0x244:
                ce_sem_addr = (ce_sem_addr & ~0xffffffff) | d
            elif m == 0x300:
                if ((d >> 3) & 0x3) and ce_sem_addr:
                    add_page(ce_sem_addr)
            elif m == 0x5c:
                host_sem_addr = (host_sem_addr & ~0xffffffff) | (d & 0xfffffffc)
            elif m == 0x60:
                host_sem_addr = (host_sem_addr & 0xffffffff) | (d << 32)
            elif m == 0x6c:
                if (d & 0x7) == 1 and host_sem_addr:
                    add_page(host_sem_addr)
            elif m == 0x1b00:
                cr_sem_addr = (cr_sem_addr & 0xffffffff) | ((d & 0xff) << 32)
            elif m == 0x1b04:
                cr_sem_addr = (cr_sem_addr & ~0xffffffff) | d
            elif m == 0x1b0c:
                if (d & 0x3) == 0 and cr_sem_addr:
                    add_page(cr_sem_addr)

with open(proc_pagemap, "rb") as pm, open(proc_mem, "r+b", buffering=0) as mem:
    def gpa_for_va(va):
        try:
            mem.seek(va)
            b = mem.read(1)     # fault the page in before reading pagemap
            if FAULT_ZERO_WRITE and in_zero_maps(va) and b:
                # MAP_SHARED /dev/zero can read through the global zero page
                # without allocating a real PFN.  QEMU needs an actual guest
                # page to DMA DtoH results into, so write the same byte back.
                mem.seek(va)
                mem.write(b)
        except OSError:
            return None
        pm.seek((va // PAGE) * 8)
        raw = pm.read(8)
        if len(raw) != 8:
            return None
        entry = struct.unpack("<Q", raw)[0]
        if not (entry & (1 << 63)):
            return None
        pfn = entry & ((1 << 55) - 1)
        if pfn == 0:
            return None
        return pfn * PAGE + (va & (PAGE - 1))

    for va in eager_windows:
        add_command_window(va)

    for va in forced_vas:
        add_command_window(va)

    if ZERO_ALL_PAGES:
        for lo, hi in zero_maps:
            for va in range(lo, hi, PAGE):
                add_page(va)

    if SCAN_MAPS:
        for lo, hi in maps:
            for va in range(lo, hi, PAGE):
                try:
                    mem.seek(va)
                    data = mem.read(PAGE)
                except OSError:
                    continue
                if not data or not any(data):
                    continue
                add_command_window(va)
                parse_pushbuffer(data)

    page_rows = []
    for va in sorted(want_pages):
        gpa = gpa_for_va(va)
        if gpa is None:
            continue
        page_rows.append((va, gpa))

    ranges = []
    cur_va = cur_gpa = cur_len = None
    for va, gpa in page_rows:
        if cur_va is not None and va == cur_va + cur_len and gpa == cur_gpa + cur_len:
            cur_len += PAGE
        else:
            if cur_va is not None:
                ranges.append((cur_va, cur_gpa, cur_len))
            cur_va, cur_gpa, cur_len = va, gpa, PAGE
    if cur_va is not None:
        ranges.append((cur_va, cur_gpa, cur_len))

    for dst, _bytes_n, shadow_va, shadow_len in htod_shadows:
        for off in range(0, shadow_len, PAGE):
            gpa = gpa_for_va(shadow_va + off)
            if gpa is None:
                continue
            size = min(PAGE, shadow_len - off)
            ranges.append((dst + off, gpa, size))

for va, gpa, size in ranges:
    print(f"{va:x} {gpa:x} {size:x}")
PY

is_true() {
    case "${1:-}" in
        1|true|True|yes|Yes|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

if is_true "$merge_existing"; then
    ssh vh "cat '$out' 2>/dev/null || true" >"$old_tmp" || true
    cat "$tmp" >>"$old_tmp"
    awk 'NF >= 3 { k = $1 " " $2 " " $3; if (!seen[k]++) print $1, $2, $3 }' \
        "$old_tmp" >"$tmp"
fi

rows=$(wc -l <"$tmp")
if [ "$rows" -eq 0 ] && ! is_true "$allow_empty"; then
    printf 'no ranges for %s; preserving existing pbmap\n' "$out"
    exit 75
fi

ssh vh "cat > '$out'" <"$tmp"
printf 'wrote %s with %s ranges\n' "$out" "$rows"
