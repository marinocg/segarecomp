#!/usr/bin/env python3
"""SEG-104-T001/T006 (ADR 0244, 0247): compare a generated-native run's CD-DA artifact with the independent reference's, aggregate only.

Inputs
  --pcm              the product's canonical stream (`--cdda-pcm`): interleaved L,R signed 16-bit little-endian, 44.1 kHz.
  --reference-pcm    the private Genesis Plus GX driver's CD-DA samples (ORACLE_CDDA_RAW): the same format, taken after the reference's fader and before
                     its output-volume scaling (unity in both), one sample frame per consumed CD-DA sample.
  --report           the product run's JSON report (its `cdda` block: sectors, frames, first/last vertical-blank onset, runs).
  --reference-log    the reference's CDD log (ORACLE_CDDLOG): a line `<frame> <samples> <toc index> <lba> 30 <total>` for every audio read that
                     played, so the frames in which the reference played audio and how many samples it consumed there.
  --product-log      optional: the product's `--cdda-log` (run starts / interruptions with their frame).

Checks (all aggregate; nothing printed contains a sample value, an LBA or a track number):
  * exact source-PCM equality: the longest common prefix of the two streams in sample frames, whether it covers the whole shorter stream, and the
    length difference (the reference consumes by video frame, the product by drive sector, so the tail differs by less than one frame's worth);
  * playback intervals: the reference's maximal runs of consecutive playing frames versus the product's runs and onsets: counts, the first and
    last onset, and the signed start and end differences in frames.
usage: sega_cd_cdda_compare.py --pcm A --reference-pcm B --report R.json --reference-log L [--product-log P] [--frame-offset N]
"""
import argparse
import hashlib
import json
import os
import re
import sys


def lcp(a, b, chunk=1 << 20):
    n = min(len(a), len(b))
    i = 0
    while i < n:
        j = min(n, i + chunk)
        if a[i:j] != b[i:j]:
            lo, hi = i, j
            while lo < hi:  # first differing byte inside the chunk
                mid = (lo + hi) // 2
                if a[i:mid + 1] == b[i:mid + 1]:
                    lo = mid + 1
                else:
                    hi = mid
            return lo
        i = j
    return n


def reference_runs(path):
    frames = []
    for line in open(path):
        t = line.split()
        if len(t) == 6 and t[4] == "30":
            frames.append(int(t[0]))
    runs = []
    for f in frames:
        if runs and f <= runs[-1][1] + 1:
            runs[-1][1] = max(runs[-1][1], f)
        else:
            runs.append([f, f])
    return runs, len(frames)


def product_runs(path):
    starts, stops = [], []
    for line in open(path):
        m = re.match(r"(start|stop|jump|track) frame=(\d+)", line)
        if not m:
            continue
        if m.group(1) in ("start", "jump"):
            starts.append(int(m.group(2)))
        elif m.group(1) == "stop":
            stops.append(int(m.group(2)))
    return starts, stops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcm", required=True)
    ap.add_argument("--reference-pcm", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--reference-log", required=True)
    ap.add_argument("--product-log")
    ap.add_argument("--frame-offset", type=int, default=0, help="reference frame index minus product vertical-blank onset count (0: the same numbering)")
    args = ap.parse_args()
    ours = open(args.pcm, "rb").read()
    ref = open(args.reference_pcm, "rb").read()
    n = lcp(ours, ref)
    shorter = min(len(ours), len(ref))
    report = json.load(open(args.report))
    cdda = report.get("cdda")
    out = {"product_frames": len(ours) // 4, "reference_frames": len(ref) // 4, "common_prefix_frames": n // 4,
           "prefix_covers_shorter_stream": n == shorter and shorter > 0, "length_difference_frames": (len(ref) - len(ours)) // 4,
           "prefix_sha256": hashlib.sha256(ours[:n]).hexdigest() if n else None,
           "product_digest_matches_report": cdda is not None and hashlib.sha256(ours).hexdigest() == cdda.get("pcm_sha256")}
    runs, played_calls = reference_runs(args.reference_log)
    out["reference_runs"] = len(runs)
    out["reference_playing_frames"] = played_calls
    if cdda is not None:
        out["product_sectors"] = cdda["sectors"]
        out["product_first_onset"] = cdda["first_frame"]
        out["product_last_onset"] = cdda["last_frame"]
        if runs:
            out["start_difference_frames"] = runs[0][0] - args.frame_offset - cdda["first_frame"]
            out["end_difference_frames"] = runs[-1][1] - args.frame_offset - cdda["last_frame"]
    if args.product_log:
        starts, stops = product_runs(args.product_log)
        out["product_run_starts"] = len(starts)
        out["product_run_stops"] = len(stops)
        out["run_start_differences_frames"] = sorted({r[0] - args.frame_offset - s for r, s in zip(runs, starts)}) if len(runs) == len(starts) else None
    ok = out["prefix_covers_shorter_stream"] and out["product_digest_matches_report"]
    out["exact"] = bool(ok)
    print(json.dumps(out, indent=1, sort_keys=True))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
