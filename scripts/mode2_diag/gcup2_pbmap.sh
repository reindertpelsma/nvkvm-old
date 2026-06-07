#!/usr/bin/env bash
set -euo pipefail

# Dump selected live guest cup2 user pages into a host-side m2pbmap file consumed by:
#   -device nvkvm-gpu-emul,...,m2pbmap=/tmp/m2_pbmap.txt
#
# By default this includes the CUDA driver mappings whose pages can appear in
# CE pushbuffers: /dev/nvidiactl, /dev/nvidia-uvm, and /dev/zero staging maps.
# Additional addresses can be forced with:
#   NVKVM_PBMAP_VAS='0xaddr 0xaddr...'
# If NVUVM_SHADOW=1 is used with nvioctl_trace.so, CUDA_HTOD shadow records
# from NVKVM_UVM_TRACE are also converted into device-VA -> guest-GPA rows.
#
# Output format is one hex tuple per line:
#   <guest userspace/GPU VA> <guest physical address> <size>

out=${1:-/tmp/m2_pbmap.txt}
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

ahead_pages=${NVKVM_PBMAP_AHEAD_PAGES:-5}
forced_vas_arg=${NVKVM_PBMAP_VAS:-}
uvm_trace=${NVKVM_UVM_TRACE:-/tmp/guest_uvm_trace.txt}

ssh vg "sudo python3 - $(printf '%q' "$ahead_pages") $(printf '%q' "$forced_vas_arg") $(printf '%q' "$uvm_trace")" >"$tmp" <<'PY'
import os
import struct
import sys

PAGE = os.sysconf("SC_PAGE_SIZE")
AHEAD_PAGES = int(sys.argv[1], 0)
UVM_TRACE = sys.argv[3]
pid_path = "/tmp/cup2_live.pid"
with open(pid_path, "r", encoding="ascii") as f:
    pid = int(f.read().strip())

all_maps = []
maps = []
eager_windows = []
with open(f"/proc/{pid}/maps", "r", encoding="ascii") as f:
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
try:
    with open(UVM_TRACE, "r", encoding="ascii", errors="replace") as f:
        for line in f:
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
except FileNotFoundError:
    pass

def parse_pushbuffer(data):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
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

with open(f"/proc/{pid}/pagemap", "rb") as pm, open(f"/proc/{pid}/mem", "rb", buffering=0) as mem:
    def gpa_for_va(va):
        try:
            mem.seek(va)
            mem.read(1)         # fault the page in before reading pagemap
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

ssh vh "cat > '$out'" <"$tmp"
printf 'wrote %s with %s ranges\n' "$out" "$(wc -l <"$tmp")"
