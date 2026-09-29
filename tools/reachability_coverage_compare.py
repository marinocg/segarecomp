#!/usr/bin/env python3
"""SEG-026-T001: compare observed execution coverage O against a reachability challenger D (report-only).

Inputs are PRIVATE local artifacts that may contain exact commercial-derived PCs:
  --coverage-dir   directory written by `tools/genesis_startup_bridge.py --execution-coverage`
                   (coverage.bitmap: one bit per even 24-bit PC; witnesses.txt: first-entry witnesses)
  --challenger     private JSON written by `segarecomp genesis-reachability-challenger --private-output`
  --coverage-summary (optional) file holding the COVERAGE_SUMMARY JSON of that run, for per-checkpoint recall

Output (stdout) is AGGREGATES ONLY: set sizes, ratios, and first-miss attribution counts by generic control
family. No PC, address, byte or disassembly is ever printed. Runtime coverage is a validation oracle only: it
falsifies the challenger and never expands or authorizes it (this tool writes nothing back).

First-miss attribution: for every observed PC not in D, follow its first-entry witness chain backwards through
missing PCs until the first transition whose previous PC is in D (the frontier edge). The frontier edge is
classified by the control family the challenger recorded for that previous PC (or by the witness cause), and
every missing PC behind it is attributed to that family. A PC first executed when a handler resumed an
interrupted instruction carries the instruction retired before the interrupt as its witness predecessor, so an
ordinary interrupt return is never mistaken for the mechanism that reached the interrupted code.
"""
import argparse
import json
import pathlib
import sys

BITMAP_BYTES = 1 << 20
CAUSE_INITIAL, CAUSE_RETIRE, CAUSE_INTERRUPT, CAUSE_DISPATCH, CAUSE_RESUMPTION = 0, 1, 2, 3, 4
WORK_RAM_BEGIN = 0xE00000

# Fine challenger family -> the experiment's coarse first-miss categories.
COARSE = {
    "rts": "ordinary_rts",
    "rts_push_window": "computed_jump_rts",
    "rte": "rte_rtr",
    "rtr": "rte_rtr",
    "jmp_(An)": "jmp_jsr_(An)",
    "jsr_(An)": "jmp_jsr_(An)",
    "jmp_d16(An)": "an_displacement_or_indexed",
    "jsr_d16(An)": "an_displacement_or_indexed",
    "jmp_(d8,An,Xn)": "an_displacement_or_indexed",
    "jsr_(d8,An,Xn)": "an_displacement_or_indexed",
    "jmp_(d8,PC,Xn)": "pc_indexed",
    "jsr_(d8,PC,Xn)": "pc_indexed",
    "unclassified": "other",
}
CATEGORIES = ["ordinary_rts", "rte_rtr", "jmp_jsr_(An)", "pc_indexed", "an_displacement_or_indexed",
              "computed_jump_rts", "ram_alias_materialized", "interrupt_entry", "fixed_flow_not_discovered",
              "initial", "other"]


def load_bitmap(path: pathlib.Path) -> set[int]:
    data = path.read_bytes()
    if len(data) != BITMAP_BYTES:
        raise SystemExit("coverage bitmap has the wrong size")
    observed = set()
    for index, byte in enumerate(data):
        if byte:
            for bit in range(8):
                if byte >> bit & 1:
                    observed.add(((index << 3) | bit) << 1)
    return observed


def load_witnesses(path: pathlib.Path) -> dict[int, tuple[int, int, int]]:
    witnesses = {}
    for line in path.read_text(encoding="ascii").splitlines():
        ordinal, previous, pc, cause = line.split()
        witnesses[int(pc, 16) & 0xFFFFFF] = (int(ordinal), int(previous, 16) & 0xFFFFFF, int(cause))
    return witnesses


def compare(observed: set[int], witnesses: dict, challenger: dict, summary: dict | None) -> dict:
    discovered = {int(x, 16) for x in challenger["discovered"]}
    site_family = {}
    for family, pcs in challenger["sites"].items():
        for pc in pcs:
            site_family[int(pc, 16)] = family
    missing = observed - discovered
    inter = observed & discovered

    frontier_cache: dict[int, tuple[str, str, int | None]] = {}

    def frontier(pc: int) -> tuple[str, str, int | None]:
        chain = []
        result = None
        x = pc
        while True:
            if x in frontier_cache:
                result = frontier_cache[x]
                break
            if x not in witnesses:
                result = ("other", "no_witness", None)
                break
            _, previous, cause = witnesses[x]
            chain.append(x)
            if cause == CAUSE_INITIAL:
                result = ("initial", "initial", None)
                break
            if previous in discovered:
                family = site_family.get(previous)
                if x >= WORK_RAM_BEGIN:
                    coarse = "ram_alias_materialized"
                elif cause == CAUSE_INTERRUPT:
                    coarse = "interrupt_entry"
                elif family is not None:
                    coarse = COARSE.get(family, "other")
                else:
                    coarse = "fixed_flow_not_discovered"
                result = (coarse, family or ("interrupt" if cause == CAUSE_INTERRUPT else "fixed"), previous)
                break
            x = previous
        for member in chain:
            frontier_cache[member] = result
        return result

    first_miss_edges = {c: 0 for c in CATEGORIES}
    first_miss_sites: dict[str, set] = {c: set() for c in CATEGORIES}
    attributed = {c: 0 for c in CATEGORIES}
    fine_edges: dict[str, int] = {}
    fine_attributed: dict[str, int] = {}
    fine_sites: dict[str, set] = {}
    for pc in missing:
        coarse, fine, site = frontier(pc)
        attributed[coarse] += 1
        fine_attributed[fine] = fine_attributed.get(fine, 0) + 1
        if pc in witnesses and witnesses[pc][1] in discovered and witnesses[pc][2] != CAUSE_INITIAL:
            first_miss_edges[coarse] += 1
            fine_edges[fine] = fine_edges.get(fine, 0) + 1
            if site is not None:
                first_miss_sites[coarse].add(site)
                fine_sites.setdefault(fine, set()).add(site)

    sites_by_family = {family: len(pcs) for family, pcs in challenger["sites"].items()}
    executed_sites_by_family = {family: sum(1 for pc in pcs if int(pc, 16) in observed)
                                for family, pcs in challenger["sites"].items()}
    report = {
        "O": len(observed),
        "D": len(discovered),
        "U": challenger["aggregate"].get("universe_immutable_rom_aot"),
        "O_and_D": len(inter),
        "O_minus_D": len(missing),
        "D_minus_O": len(discovered - observed),
        "observed_recall": round(len(inter) / len(observed), 6) if observed else None,
        "observed_in_work_ram": sum(1 for pc in observed if pc >= WORK_RAM_BEGIN),
        "challenger_model": challenger["aggregate"]["exception_model"],
        "unresolved_sites_by_family": sites_by_family,
        "executed_unresolved_sites_by_family": executed_sites_by_family,
        "first_miss_edges_by_category": {k: v for k, v in first_miss_edges.items() if v},
        "first_miss_distinct_sites_by_category": {k: len(v) for k, v in first_miss_sites.items() if v},
        "missing_pcs_attributed_by_category": {k: v for k, v in attributed.items() if v},
        "first_miss_edges_by_family": dict(sorted(fine_edges.items())),
        "first_miss_distinct_sites_by_family": {k: len(v) for k, v in sorted(fine_sites.items())},
        "missing_pcs_attributed_by_family": dict(sorted(fine_attributed.items())),
        "rte_rtr_counterexamples_to_normal_resumption": first_miss_edges["rte_rtr"],
    }
    if report["U"]:
        u = report["U"]
        report["ratios"] = {"D_over_U": round(len(discovered) / u, 6), "O_over_U": round(len(observed) / u, 6),
                            "O_over_D": round(len(observed) / len(discovered), 6) if discovered else None}
    if summary is not None:
        # Recall at each coverage checkpoint, reconstructed from first-entry retirement ordinals.
        ordinals = sorted((w[0], pc) for pc, w in witnesses.items())
        checkpoints = []
        for epoch in summary.get("epochs", []):
            limit = epoch["retirements"]
            seen = [pc for ordinal, pc in ordinals if ordinal < limit]
            hits = sum(1 for pc in seen if pc in discovered)
            checkpoints.append({"frame": epoch["frame"], "distinct": len(seen), "in_D": hits,
                                "recall": round(hits / len(seen), 6) if seen else None})
        report["checkpoints"] = checkpoints
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--coverage-dir", required=True)
    parser.add_argument("--challenger", required=True)
    parser.add_argument("--coverage-summary")
    parser.add_argument("--checkpoint-frames", help="comma-separated frames to keep from the per-epoch recall")
    args = parser.parse_args()
    coverage_dir = pathlib.Path(args.coverage_dir)
    observed = load_bitmap(coverage_dir / "coverage.bitmap")
    witnesses = load_witnesses(coverage_dir / "witnesses.txt")
    if len(witnesses) != len(observed):
        sys.stderr.write("witness set does not match the coverage bitmap (overflow?)\n")
        return 3
    challenger = json.loads(pathlib.Path(args.challenger).read_text(encoding="utf-8"))
    summary = None
    if args.coverage_summary:
        text = pathlib.Path(args.coverage_summary).read_text(encoding="utf-8").strip()
        summary = json.loads(text[len("COVERAGE_SUMMARY "):] if text.startswith("COVERAGE_SUMMARY ") else text)
    report = compare(observed, witnesses, challenger, summary)
    if args.checkpoint_frames and "checkpoints" in report:
        keep = {int(x) for x in args.checkpoint_frames.split(",")}
        report["checkpoints"] = [c for c in report["checkpoints"] if c["frame"] in keep]
    print(json.dumps(report, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
