#!/usr/bin/env python3
"""SEG-044-T005 DIAGNOSTIC (never a production route, never credited as the SEG-031 planner's result).

Writes a `segarecomp.m68k_hybrid_admission_plan.v1` whose admitted set is `H = C ∩ U`, where `C` is a source-derived executable
universe (`segarecomp.m68k_source_universe.v1`, see tools/segarecomp_source_map_extract.py and ADR 0093) and `U` is the broad
identity set from `segarecomp emit-general-startup-bridge-c --immutable-aot-address-report`.

It exists only because the unchanged SEG-031 planner cannot consume a 24k-entry island (ADR 0093 T004: `broad_analysis_incomplete`):
the plan is a hypothesis about the source authority, and the production seam (`--immutable-rom-aot-admission` /
`segarecomp build --admission-plan`) still validates it fail-closed (digest, universe fingerprint, structural closure over
fixed successors, call continuations and machine roots). Dynamic-control completeness is the external source authority's claim and
is checked only by the complete execution-PC oracle (a falsifier, not a proof).

usage: segarecomp_source_universe_plan.py --universe <file> --address-report <file> --rom-sha256 <hex> --output <plan>
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys

SHA = re.compile(r"[0-9a-f]{64}")


def read_universe(text: str, rom_sha256: str) -> set[int]:
    lines = text.split("\n")
    if (len(lines) < 8 or lines[0] != "segarecomp.m68k_source_universe.v1" or lines[1] != f"rom_sha256 {rom_sha256}"
            or lines[-1] != "" or lines[-2] != "end"):
        raise SystemExit("invalid source universe")
    count = int(lines[5].split(" ", 1)[1]) if lines[5].startswith("entries ") else -1
    entries = lines[6:-2]
    if count != len(entries) or count <= 0:
        raise SystemExit("invalid source universe")
    values = [int(entry, 16) for entry in entries if re.fullmatch(r"[0-9a-f]{8}", entry)]
    if len(values) != count or values != sorted(set(values)) or any(value & 1 for value in values):
        raise SystemExit("invalid source universe")
    return set(values)


def build_plan(universe_text: str, address_report: str, rom_sha256: str) -> tuple[str, dict]:
    if not SHA.fullmatch(rom_sha256):
        raise SystemExit("invalid rom sha256")
    source = read_universe(universe_text, rom_sha256)
    broad = sorted({int(token, 16) for token in address_report.split()})
    digest = hashlib.sha256("".join(f"{address:08x}\n" for address in broad).encode()).hexdigest()
    admitted = [address for address in broad if address in source]
    if not admitted:
        raise SystemExit("empty admission")
    ranges: list[list[int]] = []
    previous_kept = False
    for address in broad:
        if address not in source:
            previous_kept = False
            continue
        if previous_kept:
            ranges[-1][1] = address + 2
        else:
            ranges.append([address, address + 2])
        previous_kept = True
    text = (f"segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 {rom_sha256}\nuniverse_sha256 {digest}\nstrategy hybrid\n"
            + "".join(f"range {begin:08x} {end:08x}\n" for begin, end in ranges) + "end\n")
    return text, {"broad_u": len(broad), "source_c": len(source), "admitted_h": len(admitted), "source_outside_broad": len(source - set(broad)),
                  "ranges": len(ranges)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--universe", required=True)
    parser.add_argument("--address-report", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    text, summary = build_plan(open(args.universe).read(), open(args.address_report).read(), args.rom_sha256)
    with open(args.output, "w", newline="\n") as sink:
        sink.write(text)
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
