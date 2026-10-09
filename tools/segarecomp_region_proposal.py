#!/usr/bin/env python3
"""SEG-045 (ADR 0094): the FROZEN generic executable-region proposal policy `FLOW8-D` (report-only; never a production route).

Writes `segarecomp.m68k_executable_regions.v1`, an *assumption* that all executable ROM code lies inside the ranges. It is consumed only
by `segarecomp emit-general-startup-bridge-c --immutable-aot-region-proposal`, whose C++ kernel prunes it structurally and emits an
ordinary hybrid admission plan that the unchanged production validator checks.

Inputs (all generic, none title- or label-specific; runtime coverage is NEVER an input):
  * `--page-structure-report`: per-2-KiB-bin counts from `segarecomp ... --page-structure-report` (identities and *flow terminators*
    = BRA / direct JMP / RTS / RTE / RTR / JMP ea, classified by the C++ MC68000 control-successor owner; this tool classifies nothing);
  * `--direct-control-address-report`: instruction addresses of the precise static direct-control discovery from the machine roots.

Policy (frozen on Sonic 1 alone before any other title was processed; see ADR 0094 section 3):
  page size   = 8 KiB (4 KiB failed the calibration criterion; 2 KiB/8 KiB were the only sensitivity sizes allowed)
  a page is selected when   flow_terminators / (page_bytes / 2) >= 0.010   OR   it contains a direct-control-discovery address;
  adjacent selected pages merge into one range. No halo, no bridging, no per-title exclusion or addition.
"""
from __future__ import annotations

import argparse
import re
import sys

PAGE_BYTES = 8192            # FROZEN
FLOW_DENSITY_MIN = 0.010     # FROZEN: flow terminators per aligned word
BIN_BYTES = 2048             # fixed by the C++ report
SHA = re.compile(r"[0-9a-f]{64}")


def parse_bins(text: str) -> dict[int, tuple[int, int]]:
    lines = text.split("\n")
    if not lines or lines[0] != f"segarecomp.m68k_page_structure.v1 bin_bytes {BIN_BYTES}" or lines[-1] != "":
        raise SystemExit("invalid page structure report")
    bins: dict[int, tuple[int, int]] = {}
    for line in lines[1:-1]:
        match = re.fullmatch(r"([0-9a-f]{8}) ([0-9]+) ([0-9]+)", line)
        if not match:
            raise SystemExit("invalid page structure report line")
        start, identities, terminators = int(match[1], 16), int(match[2]), int(match[3])
        if start % BIN_BYTES or start // BIN_BYTES in bins or terminators > identities:
            raise SystemExit("invalid page structure report line")
        bins[start // BIN_BYTES] = (identities, terminators)
    return bins


def parse_addresses(text: str) -> list[int]:
    values = [int(token, 16) for token in text.split()]
    if any(value < 0 or value > 0xFFFFFF for value in values):
        raise SystemExit("invalid direct-control address")
    return values


def select_pages(bins: dict[int, tuple[int, int]], seeds: list[int], rom_size: int) -> list[int]:
    per_page = PAGE_BYTES // BIN_BYTES
    words = PAGE_BYTES // 2
    page_count = (rom_size + PAGE_BYTES - 1) // PAGE_BYTES
    selected = set()
    for page in range(page_count):
        terminators = sum(bins.get(page * per_page + offset, (0, 0))[1] for offset in range(per_page))
        if terminators / words >= FLOW_DENSITY_MIN:
            selected.add(page)
    selected |= {address // PAGE_BYTES for address in seeds if address < rom_size}
    return sorted(selected)


def build_regions(rom_sha256: str, rom_size: int, pages: list[int]) -> str:
    if not SHA.fullmatch(rom_sha256) or rom_size <= 0 or rom_size % 2:
        raise SystemExit("invalid rom identity")
    runs: list[list[int]] = []
    for page in pages:
        begin, end = page * PAGE_BYTES, min((page + 1) * PAGE_BYTES, rom_size)
        if runs and runs[-1][1] == begin:
            runs[-1][1] = end
        else:
            runs.append([begin, end])
    if not runs:
        raise SystemExit("empty region proposal")
    return ("segarecomp.m68k_executable_regions.v1\n" f"rom_sha256 {rom_sha256}\n"
            + "".join(f"range {begin:08x} {end:08x}\n" for begin, end in runs) + "end\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--page-structure-report", required=True)
    parser.add_argument("--direct-control-address-report", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--rom-size", type=int, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    bins = parse_bins(open(args.page_structure_report).read())
    seeds = parse_addresses(open(args.direct_control_address_report).read())
    pages = select_pages(bins, seeds, args.rom_size)
    text = build_regions(args.rom_sha256, args.rom_size, pages)
    with open(args.output, "w", newline="\n") as sink:
        sink.write(text)
    print({"policy": "FLOW8-D", "page_bytes": PAGE_BYTES, "flow_density_min": FLOW_DENSITY_MIN, "pages": len(pages),
           "region_bytes": sum(min((p + 1) * PAGE_BYTES, args.rom_size) - p * PAGE_BYTES for p in pages),
           "rom_bytes": args.rom_size}, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
