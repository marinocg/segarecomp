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

SEG-026-T002: when the challenger ran with --pc-index-recovery, its private output lists every encountered
PC-indexed site with its proven exact target set. Runtime coverage then FALSIFIES those proofs: any observed first
entry whose witness predecessor is a resolved site but which lies outside that site's proven set is counted as a
recovery escape (expected 0). The tool also reports how many proven targets were ever observed (a precision
indicator). Nothing observed is ever fed back into the challenger.
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
    recovery = pc_index_recovery_check(observed, witnesses, challenger)
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
    if recovery is not None:
        report["pc_index_recovery_check"] = recovery
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


def resolved_pc_index_targets(challenger: dict) -> dict[int, set[int]]:
    return {int(pc, 16): {int(t, 16) for t in entry["targets"]}
            for pc, entry in challenger.get("pc_index_sites", {}).items() if entry["outcome"] == "resolved"}


def pc_index_recovery_check(observed: set[int], witnesses: dict, challenger: dict) -> dict | None:
    """Falsification of proven PC-indexed target sets (aggregates only)."""
    if "pc_index_sites" not in challenger:
        return None
    resolved = resolved_pc_index_targets(challenger)
    escapes = 0
    escape_sites = set()
    entries_from_resolved = 0
    for x, (_, previous, cause) in witnesses.items():
        if cause == CAUSE_RETIRE and previous in resolved:
            entries_from_resolved += 1
            if x not in resolved[previous]:
                escapes += 1
                escape_sites.add(previous)
    all_targets = set().union(*resolved.values()) if resolved else set()
    sites = challenger["pc_index_sites"]
    return {
        "resolved_sites": len(resolved),
        "resolved_sites_executed": sum(1 for pc in resolved if pc in observed),
        "unresolved_sites_executed": sum(1 for pc, e in sites.items() if e["outcome"] != "resolved" and int(pc, 16) in observed),
        "proven_targets": len(all_targets),
        "proven_targets_observed": len(all_targets & observed),
        "first_entries_from_resolved_sites": entries_from_resolved,
        "escapes_outside_proven_targets": escapes,
        "sites_with_escapes": len(escape_sites),
    }


DYNAMIC_FAMILIES = {"rts", "rts_push_window", "rte", "rtr", "jmp_(An)", "jsr_(An)", "jmp_d16(An)", "jsr_d16(An)",
                    "jmp_(d8,An,Xn)", "jsr_(d8,An,Xn)", "jmp_(d8,PC,Xn)", "jsr_(d8,PC,Xn)", "unclassified"}


def structural_attribution(observed: set[int], witnesses: dict, discovered: set[int], classification: dict,
                           resolved: dict[int, set[int]] | None = None) -> dict:
    """Attribution using a private classification of observed PCs (same decoder; never discovery input).

    Each first entry prev -> x is labelled structurally:
      fixed        x is a fixed successor of prev (fallthrough / branch / call target)
      call_return  prev is an RTS and x is the stacked continuation of an observed call C; the structural
                   predecessor of x is C (the return itself is ordinary once C is known)
      exception_return  prev is an RTE/RTR and x is the stacked continuation of an observed TRAP C
      dynamic      prev owns a runtime-derived PC (its family) and x is not explained above
      interrupt / exception_entry / initial
    First gate: the step leaving D on the structural chain. Nearest mechanism: the closest dynamic step.
    """
    pcs = classification["pcs"]
    info = {int(pc, 16): entry for pc, entry in pcs.items()}
    stacked_call = {}
    stacked_exception = {}
    for pc, entry in info.items():
        if pc in observed and entry.get("decoded"):
            target = int(entry["stacked_address"], 16)
            if entry["stacked"] == "call":
                stacked_call.setdefault(target, pc)
            elif entry["stacked"] == "exception":
                stacked_exception.setdefault(target, pc)

    def step(x: int):
        ordinal, previous, cause = witnesses[x]
        if cause == CAUSE_INITIAL:
            return None, "initial", None
        if cause == CAUSE_INTERRUPT:
            return previous, "interrupt", None
        entry = info.get(previous)
        if entry is None or not entry.get("decoded"):
            return previous, "unclassified_predecessor", None
        family = entry["family"]
        if x in {int(t, 16) for t in entry["successors"]}:
            return previous, "fixed", None
        if resolved and previous in resolved:
            # SEG-026-T002: a proven exact PC-indexed edge is structural; leaving the proven set is an escape.
            if x in resolved[previous]:
                return previous, "recovered", None
            return previous, "dynamic", "pc_index_recovery_escape"
        if family == "rts" and x in stacked_call and witnesses.get(stacked_call[x], (ordinal + 1,))[0] < ordinal:
            return stacked_call[x], "call_return", None
        if family in ("rte", "rtr") and x in stacked_exception:
            return stacked_exception[x], "exception_return", None
        if family in DYNAMIC_FAMILIES and family != "none":
            return previous, "dynamic", family
        if entry.get("exception_entry") or entry["stacked"] == "exception":
            return previous, "exception_entry", None
        return previous, "other", None

    steps = {x: step(x) for x in observed if x in witnesses}
    first_gate: dict[str, int] = {}
    first_gate_sites: dict[str, set] = {}
    nearest: dict[str, int] = {}
    nearest_sites: dict[str, set] = {}
    dynamic_entries: dict[str, int] = {}
    dynamic_entry_sites: dict[str, set] = {}
    dynamic_entries_missing: dict[str, int] = {}
    kinds_missing: dict[str, int] = {}
    for x, (pred, kind, family) in steps.items():
        if kind == "dynamic":
            dynamic_entries[family] = dynamic_entries.get(family, 0) + 1
            dynamic_entry_sites.setdefault(family, set()).add(pred)
            if x not in discovered:
                dynamic_entries_missing[family] = dynamic_entries_missing.get(family, 0) + 1
        if x not in discovered:
            kinds_missing[kind] = kinds_missing.get(kind, 0) + 1
    missing = observed - discovered
    nearest_site_of: dict[int, int] = {}
    for x in missing:
        cursor = x
        gate = None
        near = None
        seen = set()
        while cursor is not None and cursor not in seen:
            seen.add(cursor)
            if cursor not in steps:
                gate = gate or ("no_witness", None)
                break
            pred, kind, family = steps[cursor]
            label = family if kind == "dynamic" else kind
            if near is None and kind == "dynamic":
                near = (family, pred)
            if pred is None or pred in discovered:
                gate = (label, pred)
                break
            cursor = pred
        gate = gate or ("cycle", None)
        first_gate[gate[0]] = first_gate.get(gate[0], 0) + 1
        if gate[1] is not None:
            first_gate_sites.setdefault(gate[0], set()).add(gate[1])
        near_label = near[0] if near else "none_(" + gate[0] + ")"
        nearest[near_label] = nearest.get(near_label, 0) + 1
        if near:
            nearest_sites.setdefault(near_label, set()).add(near[1])
            nearest_site_of[x] = near[1]
    # SEG-026-T002: strict local index-domain labels (classification only) of observed PC-indexed sites, and the
    # missing PCs whose nearest mechanism is such a site, by that label.
    domain_sites: dict[str, int] = {}
    domain_sites_outside_d: dict[str, int] = {}
    for pc, entry in info.items():
        label = entry.get("pc_index_domain")
        if label is None or pc not in observed:
            continue
        if label == "index_unknown":
            label += ":" + entry.get("pc_index_unknown_origin", "none")
        domain_sites[label] = domain_sites.get(label, 0) + 1
        if pc not in discovered:
            domain_sites_outside_d[label] = domain_sites_outside_d.get(label, 0) + 1
    nearest_by_domain: dict[str, int] = {}
    for x in missing:
        near = nearest_site_of.get(x)
        if near is None or near not in info or info[near].get("pc_index_domain") is None:
            continue
        label = info[near]["pc_index_domain"]
        if label == "index_unknown":
            label += ":" + info[near].get("pc_index_unknown_origin", "none")
        nearest_by_domain[label] = nearest_by_domain.get(label, 0) + 1
    return {
        "observed_pc_index_sites_by_local_domain": dict(sorted(domain_sites.items())),
        "observed_pc_index_sites_outside_d_by_local_domain": dict(sorted(domain_sites_outside_d.items())),
        "missing_pcs_by_nearest_pc_index_site_local_domain": dict(sorted(nearest_by_domain.items())),
        "structural_first_gate_missing_pcs": dict(sorted(first_gate.items())),
        "structural_first_gate_distinct_sites": {k: len(v) for k, v in sorted(first_gate_sites.items())},
        "nearest_mechanism_missing_pcs": dict(sorted(nearest.items())),
        "nearest_mechanism_distinct_sites": {k: len(v) for k, v in sorted(nearest_sites.items())},
        "observed_dynamic_entries_by_family": dict(sorted(dynamic_entries.items())),
        "observed_dynamic_entry_sites_by_family": {k: len(v) for k, v in sorted(dynamic_entry_sites.items())},
        "missing_first_entry_kinds": dict(sorted(kinds_missing.items())),
        "missing_dynamic_entries_by_family": dict(sorted(dynamic_entries_missing.items())),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--coverage-dir", required=True)
    parser.add_argument("--challenger", required=True)
    parser.add_argument("--coverage-summary")
    parser.add_argument("--checkpoint-frames", help="comma-separated frames to keep from the per-epoch recall")
    parser.add_argument("--classification",
                        help="private classification of the observed PCs (segarecomp genesis-reachability-challenger "
                             "--classify-pcs/--classify-output); enables structural attribution")
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
    if args.classification:
        classification = json.loads(pathlib.Path(args.classification).read_text(encoding="utf-8"))
        report.update(structural_attribution(observed, witnesses, {int(x, 16) for x in challenger["discovered"]},
                                             classification, resolved_pc_index_targets(challenger)))
    if args.checkpoint_frames and "checkpoints" in report:
        keep = {int(x) for x in args.checkpoint_frames.split(",")}
        report["checkpoints"] = [c for c in report["checkpoints"] if c["frame"] in keep]
    print(json.dumps(report, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
