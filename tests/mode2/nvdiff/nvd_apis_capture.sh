#!/usr/bin/env bash
# nvd_apis_capture.sh — run EVERY stage of nvd_apis under the shim, one trace each,
# then census them for NV_ESC_RM_MAP_MEMORY_DMA.
#
#   ./nvd_apis_capture.sh <outdir> [stages...]
#     stages default: vmm hostreg ipc peer mapped arrays base
#
# Produces, in <outdir>:
#   nvd_apis, nvdiff_shim.so     the built binaries (kept: a re-run must be possible)
#   <stage>.jsonl                the ioctl+mmap trace for that stage
#   <stage>.jsonl.child          the IPC importer's trace (the `ipc` stage only)
#   <stage>.stdout               the program's own transcript, incl. its OK/FAIL/SKIPPED
#   env.txt                      environment + md5s, as recorded at capture time
#   RESULT                       one line per stage, plus START and EXIT terminators
#   CENSUS.txt                   the per-stage 0x57/0x58 table
#
# ★★★ THE TERMINATOR IS LOAD-BEARING (same reason as nvd_fault_run.sh). A killed job and
# a running job are indistinguishable if absence-of-result is the only check, and `143`
# (the work was SIGTERMed) and `124` (the LAUNCHER timed out while the work ran fine)
# arrive as the same word. If RESULT carries no `EXIT ` line, this script did not finish
# and nothing in this directory may be read as a zero.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: nvd_apis_capture.sh <outdir> [stages...]}"
shift || true
STAGES="${*:-vmm hostreg ipc peer mapped arrays base}"
mkdir -p "$OUT"
RES="$OUT/RESULT"
: > "$RES"
say() { echo "$*" | tee -a "$RES"; }

say "START $(date -u +%FT%TZ) host=$(hostname) uid=$(id -u)"

CC="${CC:-cc}"
CFLAGS_CUDA="${CFLAGS_CUDA:-}"
: "${CUDA_INC:=}"
# ⚠ a file named cuda.h is not cuda.h: the PowerMac ADB driver header has the same name
#   and is present on both the bench box and the guest. Check the CONTENT, as
#   nvd_capture.sh does.
if [ "${NVD_MIN_CUDA:-0}" != 1 ]; then
    for d in /usr/local/cuda/include /usr/include /usr/local/include; do
        [ -f "$d/cuda.h" ] && grep -q 'CUDA_SUCCESS' "$d/cuda.h" 2>/dev/null && CUDA_INC="$d" && break
    done
fi
if [ -n "$CUDA_INC" ]; then
    say "BUILD using REAL cuda.h from $CUDA_INC"
    INCFLAG="-I$CUDA_INC"
else
    say "BUILD ⊘ no real cuda.h — using the bundled nvd_cuda_min.h stand-in"
    INCFLAG="-DNVD_NO_CUDA_H -I$HERE"
fi

$CC -shared -fPIC -O2 -o "$OUT/nvdiff_shim.so" "$HERE/nvdiff_shim.c" -ldl -lpthread || {
    say "FATAL shim build failed"; say "EXIT rc=1"; exit 1; }
# ⊘ -ldl: nvd_apis resolves cuMemAdvise / cuMemPrefetchAsync with dlsym, on purpose —
#   see the block comment at resolve_uvm_hints(). Harmless on glibc >= 2.34 where dlsym
#   moved into libc.
$CC -O0 $INCFLAG -o "$OUT/nvd_apis" "$HERE/nvd_apis.c" -lcuda -ldl $CFLAGS_CUDA || {
    say "FATAL nvd_apis build failed (COMPILE or LINK — read the compiler output above)"
    say "     ⊘ this is NOT necessarily a missing libcuda.so dev symlink. Measured 2026-09-07:"
    say "       a plain compile error ('RTLD_DEFAULT' undeclared) reported here as a LINK"
    say "       failure and sent the reader looking for a symlink that was present."
    say "EXIT rc=1"; exit 1; }

# ★★★ SYMBOL-BINDING GATE. A header cannot check itself; the linker's output can.
# Real cuda.h #defines a growing list of entry points onto their _v2 symbols. Binding a
# v1 name instead builds, links, runs and emits a DIFFERENT ioctl stream — silently.
# nvd_apis.c spells every versioned name in full, so this gate is checking that the
# spelling actually resolved and not that a macro happened to be present.
# ⚠ objdump prints `cuMemAlloc_v2@Base` (or `@LIBCUDA_1.0`), never a bare name: an
#   end-anchored match fails on a CORRECTLY bound symbol (measured, nvd_capture.sh).
say "GATE checking the versioned entry points bind their _v2 symbols"
MISSING=
for sym in cuCtxCreate_v2 cuCtxDestroy_v2 cuMemAlloc_v2 cuMemFree_v2 \
           cuMemcpyHtoD_v2 cuMemcpyDtoH_v2 cuMemcpyDtoD_v2 cuDeviceTotalMem_v2 \
           cuMemHostRegister_v2 cuMemHostGetDevicePointer_v2 cuMemAllocHost_v2 \
           cuIpcOpenMemHandle_v2 cuArrayCreate_v2 cuMemcpy2D_v2 cuMemsetD32_v2 \
           cuMemGetInfo_v2; do
    if objdump -R "$OUT/nvd_apis" 2>/dev/null | grep -qE "[[:space:]]$sym(@|$)"; then
        say "   ok   $sym"
    else
        say "   ★★★ NOT BOUND: $sym"; MISSING="$MISSING $sym"
    fi
done
[ -z "$MISSING" ] || { say "FATAL v1 symbols bound instead of _v2 —$MISSING"; say "EXIT rc=1"; exit 1; }

{
  echo "date=$(date -u +%FT%TZ)"
  echo "uname=$(uname -a)"
  echo "driver=$(sed -n 1p /proc/driver/nvidia/version 2>/dev/null || echo NONE)"
  echo "gpu=$(nvidia-smi --query-gpu=name,pci.bus_id --format=csv,noheader 2>/dev/null || echo NONE)"
  echo "devnodes=$(ls -1 /dev/nvidia* 2>/dev/null | tr '\n' ' ')"
  echo "libcuda=$(ldconfig -p 2>/dev/null | grep -m1 libcuda.so.1 || echo none)"
  echo "cuda_inc=${CUDA_INC:-none}"
  echo "stages=$STAGES"
  echo "shim_md5=$(md5sum "$HERE/nvdiff_shim.c" | cut -d' ' -f1)"
  echo "apis_md5=$(md5sum "$HERE/nvd_apis.c" | cut -d' ' -f1)"
  echo "hdr_md5=$(md5sum "$HERE/nvd_cuda_min.h" | cut -d' ' -f1)"
  echo "git_rev=$(cd "$HERE" && git rev-parse HEAD 2>/dev/null || echo unknown)"
} | tee "$OUT/env.txt" | sed 's/^/   /'
# ⚠ a bench claim without its SOURCE REVISION is not a claim (CLAUDE.md). git_rev above
#   is that revision; `unknown` is a legitimate value and must be reported as such rather
#   than dropped.

rc_all=0
for st in $STAGES; do
    f="$OUT/$st.jsonl"
    # ⚠ the `ipc` stage's second process writes ${st}_child.jsonl / ${st}_child.stdout —
    #   named as if it were a stage of its own so the census finds it with no special
    #   case. Clear those too, or a stale child trace from a previous run is silently
    #   re-censused and attributed to this one.
    rm -f "$f" "$OUT/${st}_child.jsonl" "$OUT/${st}_child.stdout"
    say "---- stage $st ----"
    # ⚠ NVDIFF_MAXBUF=65536 is NOT optional. The MANIFEST records that the old default of
    #   8192 truncated 27 records, including every UVM_MAP_EXTERNAL_ALLOCATION (9264 B)
    #   and control 0x20803002 (13344 B) — i.e. exactly the calls a memory-API census
    #   cares about, decoded over a partial buffer.
    NVDIFF_OUT="$f" NVDIFF_MAXBUF="${NVDIFF_MAXBUF:-65536}" \
        LD_PRELOAD="$OUT/nvdiff_shim.so" \
        timeout "${NVD_TIMEOUT:-300}" "$OUT/nvd_apis" "$st" \
        > "$OUT/$st.stdout" 2>&1
    rc=$?
    # ⊘ rc is NOT the verdict: every stage exits 0 by design, and a nonzero here means the
    #   harness (timeout/signal) intervened. Say which.
    case $rc in
      0)   note="" ;;
      124) note='  ⚠ TIMEOUT — the stage was killed by the timeout(1) wrapper; its trace is PARTIAL' ;;
      *)   note="  ⚠ nonzero rc — the stage did not run to completion" ;;
    esac
    say "PROG $st rc=$rc records=$(wc -l < "$f" 2>/dev/null || echo 0)$note"

    # ASSERT the capture is real. An existing file is not a capture; a zero-byte file is
    # a state that needs its own check, not a "not yet".
    if [ ! -s "$f" ]; then
        say "FATAL $st: capture is EMPTY — the shim did not attach"; rc_all=1
    elif ! grep -q '"nr":42' "$f"; then
        say "FATAL $st: no RM_CONTROL (nr=0x2a) — not a real RM stream"; rc_all=1
    fi
    # The program's OWN verdict line. ⊘ This is the one place a string match is correct:
    # it is reading a contract the program prints on purpose, NOT inferring that something
    # happened from the presence of a word. The ioctl question is answered by COUNTING
    # records, in nvd_dma_census.py, never by grepping.
    say "GRADE $st $(grep -m1 -E '^(OK|FAIL|SKIPPED) ' "$OUT/$st.stdout" || echo '(no verdict line — the stage did not reach its end)')"
    if [ -s "$OUT/${st}_child.jsonl" ]; then
        say "CHILD $st records=$(wc -l < "$OUT/${st}_child.jsonl") grade=$(grep -m1 -E '^(OK|FAIL|SKIPPED) ' "$OUT/${st}_child.stdout" 2>/dev/null || echo '(none)')"
    fi
done

# ---------------------------------------------------------------- the measurement
say "---- census ----"
if command -v python3 >/dev/null 2>&1; then
    python3 "$HERE/nvd_dma_census.py" "$OUT" > "$OUT/CENSUS.txt" 2>&1
    crc=$?
    sed 's/^/   /' "$OUT/CENSUS.txt" | tee -a "$RES" >/dev/null
    cat "$OUT/CENSUS.txt"
    [ $crc -ne 0 ] && { say "FATAL census exited rc=$crc"; rc_all=1; }
else
    say "FATAL python3 absent — no census was computed (this is NOT a zero)"
    rc_all=1
fi

say "EXIT rc=$rc_all"
exit $rc_all
