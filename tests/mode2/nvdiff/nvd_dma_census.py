#!/usr/bin/env python3
"""
nvd_dma_census.py — how many times did each workload issue NV_ESC_RM_MAP_MEMORY_DMA?

    usage: nvd_dma_census.py <capture.jsonl[.zst] | directory> [more...]

## The question

`NV_ESC_RM_MAP_MEMORY_DMA` is ioctl nr **0x57 = 87** on /dev/nvidia*, RM's
`NV04_MAP_MEMORY_DMA` (NVOS46, nvos.h:2166) — "map this memory object into a device's
DMA address space". Its sibling `RM_UNMAP_MEMORY_DMA` is **0x58 = 88** (NVOS47).
Both appear **zero times** in every capture committed under
`traces/host_reference_ga106/`, which run only `nvd_prog.c` — a workload whose entire
memory vocabulary is `cuMemAlloc` plus one CE copy. `nvd_apis.c` exists to walk the rest
of the client-facing memory surface; this script is the counter that reads its output.

## The rules this obeys, each of which this tree has paid for

★ **COUNT RECORDS, NEVER GREP.** A count of decoded records is the measurement. Searching
  the raw file for a string cannot distinguish "the escape was issued" from "those digits
  appeared inside a parameter blob", and cannot count at all.
★ **AN ABSENCE IS NOT EVIDENCE WITHOUT A KNOWN-POSITIVE.** Two of them are printed on
  every run and must both be checked before any zero in this table is believed:
    - the CONTROL row (`base`), which is the shape that already measured zero — if it
      ever shows a nonzero 0x57 the instrument, not the API, has changed; and
    - the counters for 0x4E `RM_MAP_MEMORY` / 0x4F, which are NOT zero in any real CUDA
      program. A run where those are also zero is a broken decode, not a quiet driver.
★ **A ZERO FROM A STAGE THAT DID NOT RUN IS NOT A ZERO.** Each row carries the stage's
  own `OK` / `FAIL` / `SKIPPED` verdict, read from its sibling `.stdout`. Rows that are
  not `OK` are printed as `UNMEASURED` in the verdict column and are excluded from the
  summary line.
⊘ It is a USERSPACE census, at the ioctl boundary. It says nothing about what the guest
  kernel or the GSP do below it, and nothing at all about the doorbell/pushbuffer plane —
  `traces/host_reference_ga106/MANIFEST.txt` measured that `ce` and `launch` are
  byte-identical, i.e. everything after cuCtxCreate is invisible here.
"""
import os
import sys
import struct
from collections import Counter, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvdiff import load, unhex, ESC          # noqa: E402  (path juggling first)

MAP_DMA = 0x57      # NV_ESC_RM_MAP_MEMORY_DMA    -- the question
UNMAP_DMA = 0x58    # NV_ESC_RM_UNMAP_MEMORY_DMA  -- its sibling
MAP_MEM = 0x4E      # NV_ESC_RM_MAP_MEMORY        -- the CPU-side analogue, a known-POSITIVE
UNMAP_MEM = 0x4F
MARK_NR = 0x7F      # nvd_apis.c / nvd_prog.c phase marker (not an NVIDIA escape)

# NVOS46_PARAMETERS, ogkm-580.159.04/src/common/sdk/nvidia/inc/nvos.h:2167.
# ⚠ `dmaOffset` is NV_ALIGN_BYTES(8) and follows three NvV32s, so it sits at 48, not 44.
# Decoding it at the wrong offset would print a plausible wrong address, which is worse
# than printing nothing.
NVOS46 = [("hClient", 0, "I"), ("hDevice", 4, "I"), ("hDma", 8, "I"),
          ("hMemory", 12, "I"), ("offset", 16, "Q"), ("length", 24, "Q"),
          ("flags", 32, "I"), ("flags2", 36, "I"), ("kindOverride", 40, "I"),
          ("dmaOffset", 48, "Q"), ("status", 56, "I")]
# NVOS47_PARAMETERS, nvos.h:2195.
NVOS47 = [("hClient", 0, "I"), ("hDevice", 4, "I"), ("hDma", 8, "I"),
          ("hMemory", 12, "I"), ("flags", 16, "I"), ("dmaOffset", 24, "Q"),
          ("size", 32, "Q"), ("status", 40, "I")]


def fields(buf, layout):
    out = OrderedDict()
    for name, off, fmt in layout:
        sz = struct.calcsize("<" + fmt)
        if off + sz <= len(buf):
            out[name] = struct.unpack_from("<" + fmt, buf, off)[0]
    return out


def marker_tag(rec):
    """The 32 ASCII bytes a phase marker carries, or None."""
    if rec.get("nr") != MARK_NR or rec.get("dev", "").startswith("nvidia-uvm"):
        return None
    raw = unhex(rec.get("hpre", ""))
    if not raw.startswith(b"NVDMARK:"):
        return None
    return raw[8:].split(b"\x00")[0].decode("ascii", "replace")


def grade_of(path):
    """The stage's OWN verdict line, from its sibling .stdout.

    ⊘ This is a string match, and it is the one place one is correct: the program prints
    exactly one of these lines on purpose, as a contract. The ioctl question above is
    answered by counting records and never by searching text.
    """
    cand = path.replace(".jsonl.zst", ".stdout").replace(".jsonl", ".stdout")
    if os.path.exists(cand):
        with open(cand, errors="replace") as fh:
            for line in fh:
                if line[:3] == "OK " or line[:5] == "FAIL " or line[:8] == "SKIPPED ":
                    return line.rstrip("\n")
        # ⊘ Not the same as a failure. `nvd_prog.c`'s captures have no verdict contract at
        # all, so their rows are informative and simply cannot be auto-graded. Saying
        # "UNMEASURED" about them would be a wrong claim about a good capture.
        return "NO-VERDICT-CONTRACT (a capture from some other workload?)"
    return "NO-STDOUT (cannot be graded)"


def census_one(path):
    recs = load(path)
    counts = Counter()
    phase = None
    hits = []          # (index, nr, phase, decoded fields)
    n_ioctl = 0
    n_marks = 0
    trunc = 0
    for i, r in enumerate(recs):
        if r.get("t") != "ioctl":
            continue
        if r.get("trunc"):
            trunc += 1
        tag = marker_tag(r)
        if tag is not None:
            phase = tag
            n_marks += 1
            continue
        n_ioctl += 1        # ⚠ REAL ioctls only: a marker is our own instrumentation and
                            #   counting it here would inflate every stage by its phases
        nr = r.get("nr")
        counts[nr] += 1
        if nr in (MAP_DMA, UNMAP_DMA):
            buf = unhex(r.get("hpost") or r.get("hpre") or "")
            hits.append((i, nr, phase,
                         fields(buf, NVOS46 if nr == MAP_DMA else NVOS47)))
    return {
        "path": path, "recs": len(recs), "ioctls": n_ioctl, "marks": n_marks,
        "trunc": trunc, "counts": counts, "hits": hits, "grade": grade_of(path),
    }


def stage_name(path):
    b = os.path.basename(path)
    for suf in (".jsonl.zst", ".jsonl.gz", ".jsonl"):
        if b.endswith(suf):
            return b[:-len(suf)]
    return b


def expand(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            for f in sorted(os.listdir(a)):
                if f.endswith((".jsonl", ".jsonl.zst", ".jsonl.gz")):
                    out.append(os.path.join(a, f))
        else:
            out.append(a)
    return out


def main(argv):
    paths = expand(argv[1:])
    if not paths:
        sys.stderr.write(__doc__)
        return 2

    rows = [census_one(p) for p in paths]

    print("NV_ESC_RM_MAP_MEMORY_DMA CENSUS  (nr 0x57=87 map, 0x58=88 unmap)")
    print("=" * 108)
    print("%-22s %8s %8s %6s  %6s %6s  %6s %6s  %s" % (
        "stage", "records", "ioctls", "marks",
        "0x57", "0x58", "0x4E", "0x4F", "verdict"))
    print("-" * 108)
    measured_hits = 0
    measured_rows = 0
    ungraded = []
    for r in rows:
        g = r["grade"]
        ok = g.startswith("OK ")
        gradeable = g.startswith(("OK ", "FAIL ", "SKIPPED "))
        if ok:
            verdict = g
            measured_rows += 1
            measured_hits += r["counts"][MAP_DMA]
        elif gradeable:
            verdict = "UNMEASURED <- " + g
        else:
            verdict = g            # no contract to grade against; the counts still stand
            ungraded.append(stage_name(r["path"]))
        print("%-22s %8d %8d %6d  %6d %6d  %6d %6d  %s" % (
            stage_name(r["path"]), r["recs"], r["ioctls"], r["marks"],
            r["counts"][MAP_DMA], r["counts"][UNMAP_DMA],
            r["counts"][MAP_MEM], r["counts"][UNMAP_MEM],
            verdict[:60]))
        if r["trunc"]:
            print("      !! %d records TRUNCATED — raise NVDIFF_MAXBUF (65536 is the "
                  "documented floor); a short buffer is UNMEASURED, not short" % r["trunc"])
        if r["marks"] == 0:
            print("      ⊘ NO phase markers in this capture (/dev/nvidiactl not opened?) — "
                  "hits below cannot be attributed to a call, and the trace must NOT be "
                  "phase-split by guessing")

    print("-" * 108)

    # ---- the two known-positives, checked and stated rather than assumed --------------
    base = [r for r in rows if stage_name(r["path"]) == "base"]
    if not base:
        print("⊘ NO CONTROL ROW. `base` was not captured, so every zero above is "
              "uncorroborated: 'this API issues no 0x57' and 'the counter never fires' "
              "are the same observation without it. Re-run including the base stage.")
    else:
        b = base[0]
        print("CONTROL  base: 0x57=%d 0x58=%d  (the nvd_prog `ce` shape; the twelve "
              "committed reference captures measure 0 here)"
              % (b["counts"][MAP_DMA], b["counts"][UNMAP_DMA]))
        if b["counts"][MAP_DMA]:
            print("  ★★★ THE CONTROL IS NOT ZERO. Something changed in the INSTRUMENT or "
                  "the driver, not in the API families under test. Stop and explain this "
                  "before reading any other row.")
    live = sum(r["counts"][MAP_MEM] for r in rows)
    print("KNOWN-POSITIVE  0x4E RM_MAP_MEMORY total across all rows = %d" % live)
    if live == 0:
        print("  ★★★ ZERO. Every real CUDA program maps memory for the CPU, so a zero "
              "here means the DECODE is broken, not that the driver was quiet. Every "
              "0x57 zero above is then meaningless.")

    print("-" * 108)
    if ungraded:
        print("NOTE  %d row(s) carry no OK/FAIL/SKIPPED contract and are excluded from the "
              "verdict below: %s. Their COUNTS are still valid; only the auto-grade is "
              "absent." % (len(ungraded), " ".join(ungraded)))
    if measured_rows == 0:
        print("VERDICT  NOTHING WAS MEASURED — no stage reported OK.")
    elif measured_hits == 0:
        print("VERDICT  %d stage(s) ran to an OK verdict and NONE of them issued "
              "NV_ESC_RM_MAP_MEMORY_DMA. That is a measurement, bounded by the stage list "
              "actually run — not a statement about CUDA as a whole." % measured_rows)
    else:
        print("VERDICT  ★★★ %d NV_ESC_RM_MAP_MEMORY_DMA record(s) across %d OK stage(s). "
              "The escape IS reachable from userspace CUDA." % (measured_hits, measured_rows))

    # ---- every hit, decoded ----------------------------------------------------------
    any_hit = False
    for r in rows:
        if not r["hits"]:
            continue
        any_hit = True
        print("")
        print("== %s — %d MAP/UNMAP_MEMORY_DMA record(s)" % (stage_name(r["path"]),
                                                             len(r["hits"])))
        for i, nr, phase, f in r["hits"]:
            print("  [%6d] %-20s phase=%s" % (i, ESC.get(nr, "nr_0x%02x" % nr),
                                              phase if phase else "(unmarked)"))
            print("           " + "  ".join(
                "%s=0x%x" % (k, v) for k, v in f.items()))
    if not any_hit:
        print("")
        print("(no 0x57/0x58 records to decode)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
