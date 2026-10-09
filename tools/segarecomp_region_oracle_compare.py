#!/usr/bin/env python3
"""SEG-045-T005 (report-only, aggregates only): compare a broad and a region-selective execution-PC oracle run.

Inputs are PRIVATE local artifacts of two `genesis_startup_bridge.py --execution-coverage` runs (COVERAGE_SUMMARY lines + coverage.bitmap),
the broad identity address report `U`, and the hybrid admission plan used by the selective run. Prints only counts and booleans:
  * the selective run's observed PCs outside K = U ∩ plan ranges (execution-PC escapes; expected 0) -- a falsifier, never an input;
  * observed PCs of the BROAD run outside K (what the plan would have excluded; > 0 means the plan is unsafe for that workload);
  * equality of coverage / final-state / frame-stream digests and typed outcomes between broad and selective.
usage: segarecomp_region_oracle_compare.py --broad-out <bridge stdout> --broad-cov <dir> [--selective-out ... --selective-cov ...] --universe U.txt --plan P
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

BITMAP_BYTES = 1 << 20


def summary(path: str) -> dict:
    for line in open(path, errors="replace"):
        if line.startswith("COVERAGE_SUMMARY "):
            return json.loads(line[len("COVERAGE_SUMMARY "):])
    raise SystemExit("no COVERAGE_SUMMARY")


def observed(directory: str) -> set[int]:
    data = pathlib.Path(directory, "coverage.bitmap").read_bytes()
    if len(data) != BITMAP_BYTES:
        raise SystemExit("coverage bitmap has the wrong size")
    out = set()
    for index, byte in enumerate(data):
        if byte:
            out.update(((index << 3) | bit) << 1 for bit in range(8) if byte >> bit & 1)
    return out


def plan_ranges(path: str) -> list[tuple[int, int]]:
    ranges = []
    for line in open(path):
        match = re.fullmatch(r"range ([0-9a-f]{8}) ([0-9a-f]{8})\n", line)
        if match:
            ranges.append((int(match[1], 16), int(match[2], 16)))
    if not ranges:
        raise SystemExit("plan has no ranges")
    return ranges


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--broad-out", required=True)
    parser.add_argument("--broad-cov", required=True)
    parser.add_argument("--selective-out")
    parser.add_argument("--selective-cov")
    parser.add_argument("--universe", required=True)
    parser.add_argument("--plan", required=True)
    args = parser.parse_args()
    universe = {int(token, 16) for token in open(args.universe).read().split()}
    ranges = plan_ranges(args.plan)
    k = {u for u in universe if any(b <= u < e for b, e in ranges)}
    broad, broad_pcs = summary(args.broad_out), observed(args.broad_cov)
    report = {"broad": {"outcome": broad["outcome"], "frames": broad["frames_published"], "distinct_pcs": len(broad_pcs),
                        "stop_class": broad["stop_class"], "pcs_outside_K": len(broad_pcs - k), "pcs_outside_U": len(broad_pcs - universe),
                        "run_wall_seconds": broad["run_wall_seconds"]},
              "U": len(universe), "K": len(k), "K_over_U": round(len(k) / len(universe), 4)}
    if args.selective_out:
        selective, selective_pcs = summary(args.selective_out), observed(args.selective_cov)
        report["selective"] = {"outcome": selective["outcome"], "frames": selective["frames_published"], "distinct_pcs": len(selective_pcs),
                               "stop_class": selective["stop_class"], "escapes_outside_K": len(selective_pcs - k),
                               "run_wall_seconds": selective["run_wall_seconds"]}
        report["identical"] = {key: broad[key] == selective[key] for key in
                               ("outcome", "frames_published", "stop_class", "distinct_pc_count", "retirements", "dispatches", "coverage_digest",
                                "frame_stream_digest", "final_state_digest")}
        report["pc_sets_equal"] = broad_pcs == selective_pcs
    json.dump(report, sys.stdout, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
