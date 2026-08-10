#!/usr/bin/env bash
# nvd_capture.sh — build the shim + workload and record N runs.
#
# Identical on the real host and inside a Mode-2 guest; that symmetry is the
# whole point (one instrument, two subjects).
#
#   ./nvd_capture.sh <outdir> [stage] [runs]
#     stage : init|dev|ctx|alloc|ce|launch   (default ce == the cup2 shape)
#     runs  : how many repetitions          (default 2, which gives a noise floor)
#
# Produces  <outdir>/<stage>_r<N>.jsonl  plus  <outdir>/<stage>_r<N>.stdout
# and asserts each capture is NON-EMPTY and contains at least one RM_CONTROL --
# a harness that writes an empty file and exits 0 is worse than none.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: nvd_capture.sh <outdir> [stage] [runs]}"
STAGE="${2:-ce}"
RUNS="${3:-2}"
mkdir -p "$OUT"

CC="${CC:-cc}"
CFLAGS_CUDA="${CFLAGS_CUDA:-}"
: "${CUDA_INC:=}"
for d in /usr/local/cuda/include /usr/include /usr/local/include; do
    [ -f "$d/cuda.h" ] && CUDA_INC="$d" && break
done
[ -n "$CUDA_INC" ] || { echo "FATAL: no cuda.h found"; exit 1; }

echo "== build (cuda.h from $CUDA_INC)"
$CC -shared -fPIC -O2 -o "$OUT/nvdiff_shim.so" "$HERE/nvdiff_shim.c" -ldl -lpthread || exit 1
$CC -O0 -I"$CUDA_INC" -o "$OUT/nvd_prog" "$HERE/nvd_prog.c" -lcuda $CFLAGS_CUDA || {
    echo "FATAL: could not link -lcuda (need libcuda.so dev symlink)"; exit 1; }

echo "== environment"
{
  echo "date=$(date -u +%FT%TZ)"
  echo "uname=$(uname -a)"
  echo "driver=$(cat /proc/driver/nvidia/version 2>/dev/null | head -1)"
  echo "devnodes=$(ls -1 /dev/nvidia* 2>/dev/null | tr '\n' ' ')"
  echo "libcuda=$(ldconfig -p 2>/dev/null | grep -m1 libcuda.so.1 || echo none)"
  echo "stage=$STAGE runs=$RUNS"
  echo "shim_md5=$(md5sum "$HERE/nvdiff_shim.c" | cut -d' ' -f1)"
  echo "prog_md5=$(md5sum "$HERE/nvd_prog.c" | cut -d' ' -f1)"
} | tee "$OUT/env_$STAGE.txt"

rc_all=0
for i in $(seq 1 "$RUNS"); do
    f="$OUT/${STAGE}_r${i}.jsonl"
    rm -f "$f"
    echo "== run $i -> $f"
    NVDIFF_OUT="$f" NVDIFF_MAXBUF="${NVDIFF_MAXBUF:-8192}" \
        LD_PRELOAD="$OUT/nvdiff_shim.so" \
        timeout "${NVD_TIMEOUT:-300}" "$OUT/nvd_prog" "$STAGE" \
        > "$OUT/${STAGE}_r${i}.stdout" 2>&1
    rc=$?
    echo "   prog rc=$rc  records=$(wc -l < "$f" 2>/dev/null || echo 0)"
    tail -3 "$OUT/${STAGE}_r${i}.stdout" | sed 's/^/   | /'
    # ASSERT the capture is real. An existing file is not a capture.
    if [ ! -s "$f" ]; then
        echo "   FATAL: capture is EMPTY -- the shim did not attach"; rc_all=1; continue
    fi
    if ! grep -q '"nr":42' "$f"; then
        echo "   FATAL: no RM_CONTROL (nr=0x2a) in capture -- not a real RM stream"; rc_all=1
    fi
    [ $rc -ne 0 ] && rc_all=$rc
done
echo "== done rc=$rc_all"
exit $rc_all
