#!/usr/bin/env python3
"""SEG-042-T002: automatic real-title external-fact harvest.

Consumes segarecomp's own existing, unmodified-analysis full private report (produced by
`segarecomp-genesis-analysis-report --domains all --universe --private-output <path>`) and attempts, for
every ELIGIBLE unresolved dynamic-control site it names, either:

  - an exact-target proof via angr (`tools/segarecomp_angr_m68k_facts.py`'s `explore_exact_target_pc`,
    which reads the resulting PC one step after the dynamic-control instruction itself, generalizing to
    every eligible family without ever naming a register or addressing mode); or
  - for `rts_computed` sites specifically, a structural, zero-angr containment fact derived entirely from
    the report's own already-exact call graph (`call_target_continuations`).

per ADR 0090 (`docs/decisions/0090-...md`), SEG-042-T001's qualification contract. Writes one combined,
deterministic, ROM-bound `segarecomp.m68k_external_facts.v1` file consumable unchanged by
`segarecomp-genesis-analysis-report --hybrid-plan --external-m68k-facts <path>`.

No hand-entered site list for any title: every site this tool attempts comes from the report's own
`computed_sites`. Resource exhaustion means `Unknown`, never a credited result. Bounds are fixed up
front and never raised solely because a desired result has not yet appeared.

Usage:
    python3 tools/segarecomp_recomp_map_harvest.py \
        --report <private-report.json> --rom <path> --rom-sha256 <sha256> \
        --output <facts-output-path> [--summary-output <path>] \
        [--max-steps 100000] [--max-entries 64] [--producer segarecomp-recomp-map-harvest-v1]

Exit status 0 means at least one fact was written (possibly zero if every site was Unknown -- check the
summary); the fact file always matches the format `--external-m68k-facts` expects. Exit status 2 is a
usage/precondition error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import time

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import segarecomp_angr_m68k_facts as angr_producer  # noqa: E402

INELIGIBLE_FAMILIES = {"rte", "unclassified"}
RTS_COMPUTED_FAMILY = "rts_computed"
DEFAULT_CLASSIFIER = TOOLS_DIR.parent / "build" / "dev" / "apps" / "m68k-primary-word-classify" / \
    "segarecomp-m68k-primary-word-classify"


def hex6(value: int) -> str:
    return format(value & 0xFFFFFF, "06x")


def load_report(path: str) -> dict:
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def eligible_sites(report: dict) -> list[dict]:
    """Every `computed_sites` entry the report itself classifies unresolved, excluding structurally
    ineligible families (ADR 0090 section 1). No title-specific filtering: this is purely a function of
    the report's own family/outcome fields."""
    out = []
    for pc_hex, site in report.get("computed_sites", {}).items():
        if site.get("outcome") != "unknown":
            continue
        family = site.get("family", "unclassified")
        if family in INELIGIBLE_FAMILIES:
            continue
        out.append({"pc": int(pc_hex, 16), "family": family, "reason": site.get("reason"), "detail": site.get("detail")})
    out.sort(key=lambda s: s["pc"])
    return out


def build_predecessor_index(discovered_lengths: dict) -> dict[int, int]:
    """end_pc -> start_pc for every discovered instruction (prev_pc + length(prev_pc) == end_pc)."""
    index = {}
    for pc_hex, length in discovered_lengths.items():
        pc = int(pc_hex, 16)
        index[pc + length] = pc
    return index


def static_jump_targets(static_successors: dict) -> set[int]:
    """Every address named as a target by at least one discovered instruction's own static successor
    (branch, taken-conditional-branch or call target) -- i.e. every address segarecomp's own discovery
    can already prove has at least one accounted-for incoming edge."""
    targets = set()
    for targets_hex in static_successors.values():
        for target_hex in targets_hex:
            targets.add(int(target_hex, 16))
    return targets


def backward_entry_candidates(site_pc: int, predecessor_index: dict, call_target_entries: set, roots: set,
                               jump_targets: set, max_candidates: int = 64) -> list[int]:
    """Nearest-first candidates: the site's own PC first (the degenerate case where the site IS its own
    function's entry, e.g. a one-instruction `rts`-only function), then walk backward through strictly
    contiguous predecessors, collecting every address that is itself a proven call-target entry or one of
    the program's own reachability roots, until the walk cannot continue (a gap) or the bound is hit. The
    point where a genuine gap stops the walk is itself always a sound final-resort candidate: a discovered
    address that is not reached by straight-line fallthrough from a lower discovered address was
    necessarily found via some other proven edge (a branch, a call, or a root) -- confirmed directly
    against `jump_targets`/`call_target_entries`/`roots` rather than merely assumed, and still subject to
    the same external-entry check every other candidate is."""
    candidates = []
    if site_pc in call_target_entries or site_pc in roots:
        candidates.append(site_pc)
    cur = site_pc
    seen = set()
    while cur in predecessor_index and len(candidates) < max_candidates:
        prev = predecessor_index[cur]
        if prev in seen:
            break  # a cycle would mean a contiguity data error; never loop forever
        seen.add(prev)
        if prev in call_target_entries or prev in roots:
            candidates.append(prev)
        cur = prev
    if (cur not in predecessor_index and cur != site_pc and len(candidates) < max_candidates and
            cur not in candidates and (cur in jump_targets or cur in call_target_entries or cur in roots)):
        candidates.append(cur)  # the gap-termination point itself, confirmed to have a proven incoming edge
    return candidates


def external_entry_violates(start_pc: int, site_pc: int, static_successors: dict) -> bool:
    """True if any discovered instruction OUTSIDE [start_pc, site_pc] has a static successor strictly
    inside (start_pc, site_pc] -- the one residual risk the local backward-walk heuristic alone cannot
    rule out (ADR 0090 section 2). O(|static_successors|) per call by design (a one-time bounded scan,
    not a reusable index, since this is only evaluated a handful of times per site)."""
    for source_hex, targets_hex in static_successors.items():
        source = int(source_hex, 16)
        if start_pc <= source <= site_pc:
            continue
        for target_hex in targets_hex:
            target = int(target_hex, 16)
            if start_pc < target <= site_pc:
                return True
    return False


def find_sound_start_pc(site_pc: int, predecessor_index: dict, call_target_entries: set, roots: set,
                         static_successors: dict, jump_targets: set) -> int | None:
    for candidate in backward_entry_candidates(site_pc, predecessor_index, call_target_entries, roots, jump_targets):
        if not external_entry_violates(candidate, site_pc, static_successors):
            return candidate
    return None


def classify_words(classifier_path: str, words: list[int]) -> dict[int, str]:
    """Batch-classifies every distinct opcode word via the tiny native CLI (ADR 0090 section 4); never
    duplicates decoder logic in Python. Empty input is a no-op (nothing to classify)."""
    if not words:
        return {}
    joined = ",".join(f"{w:04x}" for w in sorted(set(words)))
    result = subprocess.run([classifier_path, "--words", joined], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"segarecomp-m68k-primary-word-classify failed: {result.stderr.strip()}")
    classes = {}
    for line in result.stdout.splitlines():
        word_hex, word_class = line.split()
        classes[int(word_hex, 16)] = word_class
    return classes


def attempt_exact(site_pc: int, start_pc: int, rom_path: str, classifier_path: str, max_entries: int,
                   max_steps: int) -> dict:
    started = time.monotonic()
    targets, steps, reason, fetched_words = angr_producer.explore_exact_target_pc(
        rom_path, start_pc, site_pc, [], max_entries, max_steps)
    wall = time.monotonic() - started
    cost = {"steps": steps, "wall_seconds": round(wall, 3), "fetched_word_count": len(fetched_words)}
    if targets is None:
        return {"outcome": "resource_exhausted" if reason == "resource_exhausted" else "unresolved",
                "reason": reason, "cost": cost}
    classes = classify_words(classifier_path, fetched_words)
    illegal = sorted(w for w in fetched_words if classes.get(w) != "legal")
    if illegal:
        return {"outcome": "unsupported", "reason": "unsupported_proof_path_illegal_word", "cost": cost,
                "illegal_words": [f"{w:04x}" for w in illegal]}
    return {"outcome": "exact", "targets": targets, "cost": cost}


def attempt_containment(site_pc: int, start_pc: int, call_target_continuations: dict) -> dict:
    entries = call_target_continuations.get(hex6(start_pc))
    if not entries:
        return {"outcome": "unresolved", "reason": "no_known_callers"}
    targets = sorted({int(e, 16) for e in entries})
    return {"outcome": "contained", "targets": targets}


def harvest(report: dict, rom_path: str, classifier_path: str, max_entries: int, max_steps: int) -> list[dict]:
    predecessor_index = build_predecessor_index(report.get("discovered_lengths", {}))
    call_target_continuations = report.get("call_target_continuations", {})
    call_target_entries = {int(k, 16) for k in call_target_continuations}
    roots = {int(r, 16) for r in report.get("roots", [])}
    static_successors = report.get("static_successors", {})
    jump_targets = static_jump_targets(static_successors)

    results = []
    for site in eligible_sites(report):
        site_pc = site["pc"]
        start_pc = find_sound_start_pc(site_pc, predecessor_index, call_target_entries, roots, static_successors,
                                        jump_targets)
        if start_pc is None:
            results.append({**site, "outcome": "unresolved", "reason": "no_sound_starting_scope"})
            continue
        if site["family"] == RTS_COMPUTED_FAMILY:
            outcome = attempt_containment(site_pc, start_pc, call_target_continuations)
        else:
            outcome = attempt_exact(site_pc, start_pc, rom_path, classifier_path, max_entries, max_steps)
        results.append({**site, "start_pc": hex6(start_pc), **outcome})
    return results


def write_facts(results: list[dict], rom_sha256: str, producer: str, output: str) -> int:
    lines = ["segarecomp.m68k_external_facts.v1", f"rom_sha256 {rom_sha256}", f"producer {producer}"]
    written = 0
    for result in sorted((r for r in results if r["outcome"] in ("exact", "contained")), key=lambda r: r["pc"]):
        keyword = result["outcome"]
        entries = ",".join(f"{t:08x}" for t in result["targets"])
        lines.append(f"fact {result['pc']:08x} {keyword} {entries}")
        written += 1
    lines.append("end")
    lines.append("")
    pathlib.Path(output).write_text("\n".join(lines), encoding="ascii")
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--report", required=True, help="private JSON from segarecomp-genesis-analysis-report "
                                                          "--domains all --universe --private-output")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--classifier", default=str(DEFAULT_CLASSIFIER))
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-output")
    parser.add_argument("--max-entries", type=int, default=64)
    parser.add_argument("--max-steps", type=int, default=100_000)
    parser.add_argument("--producer", default="segarecomp-recomp-map-harvest-v1")
    args = parser.parse_args()

    rom = open(args.rom, "rb").read()
    digest = hashlib.sha256(rom).hexdigest()
    if digest != args.rom_sha256.lower():
        print(f"segarecomp_recomp_map_harvest: --rom-sha256 does not match the ROM's actual digest "
              f"({digest} != {args.rom_sha256.lower()}); refusing to analyse a mismatched image", file=sys.stderr)
        return 2

    report = load_report(args.report)
    results = harvest(report, args.rom, args.classifier, args.max_entries, args.max_steps)
    written = write_facts(results, digest, args.producer, args.output)

    summary = {
        "schema": "segarecomp.recomp_map_harvest.summary.v1",
        "sites_considered": len(results),
        "exact": sum(1 for r in results if r["outcome"] == "exact"),
        "contained": sum(1 for r in results if r["outcome"] == "contained"),
        "unresolved": sum(1 for r in results if r["outcome"] == "unresolved"),
        "unsupported": sum(1 for r in results if r["outcome"] == "unsupported"),
        "resource_exhausted": sum(1 for r in results if r["outcome"] == "resource_exhausted"),
        "facts_written": written,
        "sites": results,
    }
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.summary_output:
        pathlib.Path(args.summary_output).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
