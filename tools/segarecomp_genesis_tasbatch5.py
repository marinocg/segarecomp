#!/usr/bin/env python3
"""SEG-083-T001: five-game TAS corpus expansion and neural-retraining readiness (OFFLINE research tooling).

A thin configuration layer over the merged SEG-081 batch tool (`segarecomp_genesis_tasbatch`, imported UNCHANGED and re-pointed at the five-game
table, plan and ceilings at import time) and therefore over the unchanged SEG-080 modules. It adds exactly three things:

  * the primary control drives EVERY controller present in the movie with independent seeded inputs (SEG-081 moved player 1 only); the
    SEG-081 player-1-only control stays as the secondary comparability baseline (scenario `ctl2_a`; its player-1 stream is the primary
    control's player-1 stream, so the two differ only by the other controllers' inputs);
  * pure decision rules: per-batch acquisition class, effective independent lineages and the neural-pilot readiness decision;
  * the frozen five-game protocol (plan + digest) and a CPU ceiling of 8 h per game / 40 h in total.

Everything else (identity, run, label, analyze, batch, fixture, guard) is the SEG-081 command set with the new game table:

  python3 tools/segarecomp_genesis_tasbatch5.py {identity,run,label,registry-sigs,sonic-sigs,analyze,batch,fixture,guard,freeze,verify-plan}
  python3 tools/segarecomp_genesis_tasbatch5.py {decide,aggregate}      # five-game batch class / readiness decision (pure, aggregate input)
"""
from __future__ import annotations

import argparse
import copy
import io
import json
import pathlib
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_tasbatch as tb  # noqa: E402
import segarecomp_genesis_evidence as ge  # noqa: E402

SCHEMA = "segarecomp.genesis_tasbatch5.v1"
GAMES_FILE = HERE / "segarecomp_genesis_tasbatch5.games.json"
PLAN = HERE / "segarecomp_genesis_tasbatch5.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_tasbatch5.plan.digest"
GAMES = json.loads(GAMES_FILE.read_text())
ORDER = sorted(GAMES, key=lambda g: GAMES[g]["order"])
THRESH = dict(tb.THRESH, cpu_hours_per_game=8, cpu_hours_total=40)
INDEPENDENCE_CONTAINMENT_MAX = 0.30

# frozen readiness rule (see PROTOCOL["readiness"]); every number here is pre-registered, none was tuned on a result. All counts come from the
# NEW BATCH ALONE (the five games, judged against the all-controller control); SEG-081 contributes only to the eight-lineage comparison.
READY = {"floor_new_useful_independent_lineages": 2, "floor_new_novel_signatures": 100, "floor_residual_er": 0.05, "floor_residual_folds": 1,
         "pilot_new_useful_independent_lineages": 3, "pilot_total_useful_independent_lineages": 5, "pilot_new_novel_signatures": 300,
         "pilot_new_hard_negative_cells": 300, "pilot_residual_er": 0.05, "pilot_residual_folds": 2, "noise_note": "see model_a.noise"}
ACQ_CLASSES = ("EXPANSION_SUPPORTED", "EXPANSION_MIXED", "EXPANSION_NOT_SUPPORTED", "EVIDENCE_INSUFFICIENT")
READINESS = ("NEURAL_PILOT_JUSTIFIED", "MORE_DIVERSITY_NEEDED", "NEURAL_RETRAINING_NOT_JUSTIFIED")


# ------------------------------------------------------------------------------------------------------------------- control
def control_movie_all(tas_bk2: bytes, frames: int, seed: int) -> bytes:
    """A .bk2 identical to the TAS movie except for its input log: the SEG-080/081 seeded policy drives EVERY controller present in the
    movie. Player 1 uses `seed` exactly as SEG-081 did; player k >= 2 uses an independent stream seeded `seed * 7919 + 104729 * (k - 1)`."""
    zin = zipfile.ZipFile(io.BytesIO(tas_bk2))
    log = zin.read("Input Log.txt").decode().splitlines()
    key = next(ln for ln in log if ln.startswith("LogKey:"))
    groups = [g for g in key[len("LogKey:"):].split("#") if g]
    names = [[x for x in g.split("|") if x] for g in groups]
    streams = {gi: tb.policy_rows(frames, seed if gi == 1 else seed * 7919 + 104729 * (gi - 1)) for gi in range(1, len(groups))}
    out = ["[Input]", key]
    for f in range(frames):
        parts = ["|" + "." * len(names[0]) + "|"]
        for gi in range(1, len(groups)):
            pressed = streams[gi][f]
            cells = []
            for nm in names[gi]:
                b = nm.split(" ", 1)[1]
                cells.append(tb.MNEMONIC[b] if b in pressed else ".")
            parts.append("".join(cells) + "|")
        out.append("".join(parts))
    out.append("[/Input]")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.filename == "Input Log.txt":
                data = ("\n".join(out) + "\n").encode()
            elif info.filename == "Subtitles.txt":
                data = b""
            zout.writestr(info.filename, data)
    return buf.getvalue()


_CONTROL_P1 = tb.control_movie


def control_dispatch(tas_bk2: bytes, frames: int, seed: int) -> bytes:
    """`tb.cmd_run` asks for seed 1 (ctl_a) and seed 2 (ctl2_a). Here seed 1 = the all-controller PRIMARY control (seed 1); seed 2 = the SEG-081
    player-1-only control with seed 1 (secondary comparability baseline)."""
    if seed == 1:
        return control_movie_all(tas_bk2, frames, 1)
    if seed == 2:
        return _CONTROL_P1(tas_bk2, frames, 1)
    raise ge.EvidenceError("unknown control id")


# --------------------------------------------------------------------------------------------------------- pure decision rules
def acquisition_class(classes: dict, open_review_blockers: int = 0) -> dict:
    """Five-game acquisition class (pure). classes: {game: per-game class}. Independent of the model-A and readiness verdicts."""
    qual = [g for g, c in classes.items() if c.startswith("QUALIFIED")]
    use = [g for g, c in classes.items() if c == "QUALIFIED_USEFUL"]
    conflict = [g for g, c in classes.items() if c == "LABEL_POLICY_CONFLICT"]
    major = len(conflict) + open_review_blockers
    if not qual:
        d = "EVIDENCE_INSUFFICIENT"
    elif len(use) >= 3 and major == 0:
        d = "EXPANSION_SUPPORTED"
    elif use:
        d = "EXPANSION_MIXED"
    elif len(qual) >= 2:
        d = "EXPANSION_NOT_SUPPORTED"
    else:
        d = "EXPANSION_MIXED"          # exactly one qualified low-yield game: not enough evidence to call the expansion unsupported
    return {"decision": d, "qualified": qual, "useful": use, "unresolved_major": major,
            "limiting_factor": ("unresolved major inconsistency" if d == "EXPANSION_MIXED" and len(use) >= 3 else
                                "fewer than three useful games" if d == "EXPANSION_MIXED" else None)}


def independent_lineages(games: list, containment: dict, registry_containment: dict, threshold: float = INDEPENDENCE_CONTAINMENT_MAX) -> dict:
    """Effective independent lineage count (pure). `containment[(a, b)]` is the pair's max directional 12-byte code-shingle containment;
    `registry_containment[g]` is g's max containment against any SEG-072 development lineage. A game above the threshold against the registry adds no
    independent diversity; the remaining games are clustered (connected components over containment > threshold); the count is the cluster number."""
    redundant = sorted(g for g in games if registry_containment.get(g, 0.0) > threshold)
    rest = [g for g in games if g not in redundant]
    parent = {g: g for g in rest}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for (a, b), v in containment.items():
        if a in parent and b in parent and v > threshold:
            parent[find(a)] = find(b)
    clusters: dict = {}
    for g in rest:
        clusters.setdefault(find(g), []).append(g)
    return {"titles": len(games), "redundant_with_registry": redundant, "clusters": sorted(sorted(c) for c in clusters.values()),
            "effective_independent": len(clusters)}


def readiness_decision(m: dict) -> dict:
    """Frozen readiness decision (pure). m carries (new batch = the five games): new_useful_independent_lineages, total_useful_independent_lineages (eight
    candidates), new_novel_signatures (union of the independent useful new games' forms/cfg signatures minus the registry's AND the SEG-081 games'),
    new_hard_negative_cells (strong hard-negative cells of their TAS labels), residual_folds (development folds on which the DATA-ONLY arm T_all still has lineage-max
    ER >= 0.05 AND the data-only gain over B0 does not exceed the diagnostic's noise floor, i.e. more data did not close the gap an architecture could address),
    provenance_ok. The B0 headroom is a historical constant and is NOT a criterion."""
    R = READY
    floor = []
    if m["new_useful_independent_lineages"] < R["floor_new_useful_independent_lineages"]:
        floor.append(f"new useful independent lineages {m['new_useful_independent_lineages']} < {R['floor_new_useful_independent_lineages']}")
    if m["new_novel_signatures"] < R["floor_new_novel_signatures"]:
        floor.append(f"new novel structural signatures {m['new_novel_signatures']} < {R['floor_new_novel_signatures']}")
    if m["residual_folds"] < R["floor_residual_folds"]:
        floor.append(f"folds where data alone leaves ER >= {R['floor_residual_er']} (beyond noise): {m['residual_folds']} < {R['floor_residual_folds']}")
    if floor:
        return {"decision": "NEURAL_RETRAINING_NOT_JUSTIFIED", "reasons": floor, "missing": []}
    missing = []
    if m["new_useful_independent_lineages"] < R["pilot_new_useful_independent_lineages"]:
        missing.append(f"new useful independent lineages {m['new_useful_independent_lineages']} < {R['pilot_new_useful_independent_lineages']}")
    if m["total_useful_independent_lineages"] < R["pilot_total_useful_independent_lineages"]:
        missing.append(f"total useful independent lineages (8 candidates) {m['total_useful_independent_lineages']} < {R['pilot_total_useful_independent_lineages']}")
    if m["new_novel_signatures"] < R["pilot_new_novel_signatures"]:
        missing.append(f"new novel structural signatures {m['new_novel_signatures']} < {R['pilot_new_novel_signatures']}")
    if m["new_hard_negative_cells"] < R["pilot_new_hard_negative_cells"]:
        missing.append(f"new strong hard-negative cells {m['new_hard_negative_cells']} < {R['pilot_new_hard_negative_cells']}")
    if m["residual_folds"] < R["pilot_residual_folds"]:
        missing.append(f"folds where data alone leaves ER >= {R['pilot_residual_er']} (beyond noise): {m['residual_folds']} < {R['pilot_residual_folds']}")
    if not m["provenance_ok"]:
        missing.append("label provenance / snapshot verification incomplete")
    return {"decision": "NEURAL_PILOT_JUSTIFIED" if not missing else "MORE_DIVERSITY_NEEDED", "reasons": [], "missing": missing}


# ----------------------------------------------------------------------------------------------------------------------- plan
def build_protocol() -> dict:
    p = copy.deepcopy(tb.PROTOCOL)
    p["schema"] = SCHEMA + ".plan"
    p["task"] = "SEG-083-T001"
    p["games"] = GAMES
    p["order"] = ORDER
    p["emulator"]["note"] = ("BizHawk 2.9.1 linux-x64 under Mono 6.8, Genplus-gx waterbox core, Xvfb, unprivileged user (SEG-080/081 setup). All five movies "
                             "name 2.9.1 as the recording emulator or an older release that 2.9.1 replays; replay on 2.9.1 is attempted first and one bounded "
                             "compatibility investigation per game is allowed. No movie input or emulator setting is altered.")
    p["identity"]["rom_source"] = "operator-supplied ROMs and movies under games/5-game-exp only; nothing is downloaded or substituted"
    p["identity"]["procedure"] = (p["identity"]["procedure"] + " The local ROM and movie metadata are authoritative; the TASVideos reference ids are candidate "
                                  "identities and are recorded, not trusted.")
    p["isolation"] = {"checked": "SEG-072 corpus manifest (development and sealed), SEG-075/077 candidate lists, SEG-080 Sonic 2 and SEG-081 games. None of the five "
                      "images is a sealed or reserved-blind member (machine-checked by image SHA-256/SHA-1) or in the development registry. NHL '94 is a SEG-077 "
                      "development CANDIDATE (title level) whose acquisition tasks were cancelled: no label or registry entry for it exists; its image is the same "
                      "bytes as the ignored local copy under games/. It is therefore a valid new lineage, disclosed. Developer / engine adjacency is measured by "
                      "12-byte code-shingle containment against every registry lineage, every SEG-081 game and every earlier game of this batch.",
                      "rule": "an image or lineage id in the sealed/blind set is excluded and reported; containment > 0.30 in either direction against a registry "
                              "lineage makes the game redundant for the independent-lineage count; games above 0.30 against each other form one lineage cluster"}
    p["control"] = {
        "policy": "the SEG-080/081 seeded-input policy ported verbatim (`tb.policy_rows`), delivered as a synthetic `.bk2` that reuses the TAS movie's header, controller "
                  "and sync settings and replaces only the input log, so ROM, core configuration, frame count, tracing and labelling are identical by construction",
        "primary": "ctl_a = ALL-CONTROLLER control, seed 1: every controller present in the movie is driven; player 1 uses policy seed 1 (exactly the SEG-081 "
                   "stream), player k >= 2 an independent stream seeded seed*7919 + 104729*(k-1)",
        "secondary": "ctl2_a = the SEG-081 player-1-only control (seed 1) as a comparability baseline; only player 1 moves. It replaces SEG-081's seed-2 "
                     "sensitivity control: the primary class never depends on it (reported as `useful_vs_seed2` in the tool output, i.e. versus the player-1-only control)",
        "frames": "N (the TAS movie frame count)", "label_inputs": "tas = tas_a only; ctl = ctl_a (primary, all-controller); ctl2 = ctl2_a (player-1-only); tas_b serves determinism",
        "limitation": "the control is a random-input policy, not a game-playing agent, and often stays near the title or menu. Control depth is reported as the control/TAS "
                      "executed-PC ratio per game. Yield over the control establishes executed-code acquisition feasibility, not optimality against another intelligent policy "
                      "and not ML benefit",
        "no_game_specific": "no per-game script, RAM address, progression shortcut or code-discovery heuristic"}
    p["usefulness"]["sensitivity"] = "the player-1-only control is reported (extra cells, usefulness; tool keys `*_seed2` are renamed `*_p1only` in the output) but never changes the primary class"
    p["limits"] = {"cpu_hours_per_game": THRESH["cpu_hours_per_game"], "cpu_hours_total": THRESH["cpu_hours_total"], "compat_attempts_per_game": 1,
                   "traced_tas_replays": 2, "controls": "one all-controller primary + the player-1-only comparability baseline",
                   "ledger": "append-only <private>/<game>/ledger.jsonl", "model_a_diagnostic": "one HistGradientBoosting fit per arm per fold; at most 16 CPU-hours"}
    p["batch_decision"] = {
        "EXPANSION_SUPPORTED": ">= 3 of the 5 games QUALIFIED_USEFUL (each with structural novelty) and no unresolved major inconsistency (no LABEL_POLICY_CONFLICT game, no open review blocker)",
        "EXPANSION_MIXED": "1 or 2 useful games, or >= 3 useful with an unresolved major inconsistency, or exactly one qualified low-yield game; the limiting factor is named",
        "EXPANSION_NOT_SUPPORTED": ">= 2 qualified games and none QUALIFIED_USEFUL",
        "EVIDENCE_INSUFFICIENT": "no game is qualified (inputs missing, replay invalid, untraceable, over budget)",
        "separate": "neither the model-A diagnostic nor the readiness decision changes the acquisition class; ML_INCONCLUSIVE is not failed acquisition",
        "per_game_failure": "a failed game keeps its exact class (INPUT_REQUIRED, REPLAY_INVALID, TRACING_UNAVAILABLE, ...); valid findings of the other games are preserved; no substitution"}
    p["model_a"] = {
        "runs_when": ">= 3 new games qualified (again at the final checkpoint); a diagnostic on the first three qualified games is allowed before the batch ends and is "
                     "superseded by the final one",
        "model": "SEG-072 model A / M1 HistGradientBoosting (frozen HGB_CONFIG), masked cells excluded; leave-one-lineage-out over every qualified SEG-072 lineage",
        "arms": {"B0": "baseline development corpus", "C": "+ primary all-controller control strong labels, five new games",
                 "T": "+ TAS strong labels, five new games", "TW": "+ TAS strong+weak labels (cell weights), five new games",
                 "T_matched": "+ TAS strong labels randomly down-sampled (seed 83) to the primary control's positive and negative cell counts, five new games",
                 "C81": "+ SEG-081 player-1-only control strong labels, three SEG-081 games", "T81": "+ SEG-081 TAS strong labels, three SEG-081 games",
                 "T_all": "+ TAS strong labels of the SEG-081 and the new games (eight)", "C_all": "+ control strong labels of the eight games (SEG-081 p1-only, new all-controller)",
                 "B0_sub": "noise null: B0 trained on a random 90 % of the base cells (seed 83); no new-game data"},
        "noise": "noise floor = mean over folds of |ER(B0_sub) - ER(B0)| (data-perturbation spread of the near-saturated baseline). A paired ER delta counts as better / worse only if "
                 "its magnitude exceeds the floor; the same rule gates `residual_folds`. Deltas inside the floor are reported as undetermined",
        "isolation": "as SEG-081: new-game lineage ids and image SHA-256 absent from the registry; 12-byte-shingle containment of labelled code runs between a new game and the held-out "
                     "lineage <= 0.30 in both directions, else the game is withheld from that fold",
        "operating_point": "threshold = the 10th percentile of B0 scores over the fold's positive cells (B0 recall 0.90), every arm scored at it",
        "metrics": {"primary_not_ap": ["Genesis mean ER (mean over folds of the lineage-max image ER, the SEG-075 definition) and worst-lineage ER",
                                       "retained-region recall at B35 and B50 (positive cells inside the region) and missed executable instruction starts",
                                       "boundary-stratum missed cells at B35/B50 (D byte, adjacent D-only, short run)",
                                       "hard-negative false-positive rate at the fixed operating point",
                                       "D01, D03 and D07 reported separately",
                                       "paired per-fold deltas (ER, missed starts, B35/B50 recall) against C and T_matched (volume-matched controls), each judged against the noise floor"],
                    "reported": ["pooled AP and AUC (the historical baseline is near-saturated; never a readiness criterion)"]},
        "verdict": "the SEG-081 frozen `ml_verdict` rule on pooled AP for T (continuity), reported beside the ER-based paired deltas; ML_INCONCLUSIVE is a valid outcome",
        "uncertainty": "the new games have no source truth: they enter training only; every evaluation fold is a source-backed SEG-072 lineage; incomplete labels are never used as test targets",
        "forbidden": "reserved blind cohorts, calibration on held-out targets, hyperparameter search, models B/C"}
    p["readiness"] = {
        "purpose": "decide whether a separate, prospective, bounded neural B / C-v2 pilot is worth its compute; the frozen figures of SEG-076 (A ER ~0.105, B ~0.155, C-v2 ~0.127, "
                   "~1.6 CPU-hours per C-v2 fit) are historical context, not success thresholds",
        "scope": "all counts come from the NEW BATCH ALONE (five games vs the all-controller control); SEG-081's three games enter only the eight-candidate comparison and the "
                 "`total_useful_independent_lineages` criterion (their class was judged against the player-1-only control, which is disclosed)",
        "inputs": {"new_useful_independent_lineages": "QUALIFIED_USEFUL new games after the independence rule below (cluster count)",
                   "total_useful_independent_lineages": "the same over the eight candidates (SEG-081's three included)",
                   "independence": "a game whose max code-shingle containment (either direction, on the union of control and TAS labelled code runs) against any SEG-072 development lineage "
                                   "exceeds 0.30 adds no independent diversity; the remaining games are clustered by pairwise containment > 0.30; titles are reported separately",
                   "new_novel_signatures": "|(forms U cfg) signatures of the TAS strong labels of the independent useful NEW games  minus the registry's  minus the SEG-081 games'|",
                   "new_hard_negative_cells": "strong hard-negative cells in the TAS strong labels of the independent useful NEW games",
                   "residual_folds": "SEG-072 development folds on which the data-only arm T_all (TAS strong labels of all eight) still has lineage-max ER >= 0.05 and the T_all-over-B0 ER "
                                     "reduction does not exceed the noise floor (below); it measures what extra data did NOT fix, i.e. headroom an architecture could address",
                   "headroom_note": "B0's absolute ER is a historical constant of the SEG-072 registry (A ~0.105): it is reported but never a criterion, because it cannot depend on the new data",
                   "provenance_ok": "no LABEL_POLICY_CONFLICT game, the seg083-v1 snapshot verifies and round-trips"},
        "floor_NEURAL_RETRAINING_NOT_JUSTIFIED": "ANY of: new useful independent lineages < %d; new novel signatures < %d; residual folds < %d" % (
            READY["floor_new_useful_independent_lineages"], READY["floor_new_novel_signatures"], READY["floor_residual_folds"]),
        "NEURAL_PILOT_JUSTIFIED": "no floor failure AND new useful independent lineages >= %d AND total (eight) >= %d AND new novel signatures >= %d AND new strong hard-negative cells >= %d "
                                  "AND residual folds >= %d AND provenance_ok" % (READY["pilot_new_useful_independent_lineages"], READY["pilot_total_useful_independent_lineages"],
                                                                                 READY["pilot_new_novel_signatures"], READY["pilot_new_hard_negative_cells"], READY["pilot_residual_folds"]),
        "MORE_DIVERSITY_NEEDED": "no floor failure but at least one pilot criterion unmet; the unmet criteria name the kind of game or evidence missing",
        "not_required": "a significant model-A AP improvement (the baseline is near-saturated)",
        "pilot_specification_if_justified": "separate prospective pilot: one or two difficult development folds, one seed per architecture (B and C-v2), frozen compute limits, "
                                            "source-backed evaluation; full three-seed all-fold retraining needs another operator decision informed by that pilot; not run in SEG-083",
        "classes": list(READINESS), "acquisition_classes": list(ACQ_CLASSES), "thresholds": READY}
    p["disclosures"] = [
        "the primary control drives all controllers with independent seeded streams; the SEG-081 player-1-only control is retained as the comparability baseline",
        "the controls are random-input policies, may stay shallow (reported as the control/TAS executed-PC ratio), and do not bound what an intelligent policy would reach",
        "the policy never presses X/Y/Z/Mode, so six-button games are only partly driven, and player-2 Start can enter two-player modes: both inherited from the SEG-080/081 policy and not corrected (no game-specific logic)",
        "TAS-versus-control yield measures executed-code acquisition feasibility, not ML benefit and not a fair gameplay comparison",
        "the native CDL data flag is blanket: E bytes also read as data are counted and disclosed, never gating; RAM execution disables weak-read supervision under the existing "
        "conservative policy (weak-read loss is reported per game)",
        "no source truth exists for these titles: correctness is internal consistency, ROM mapping, decode boundaries and provenance only",
        "NHL '94 is a SEG-077 development candidate with no previous label or registry data",
        "engine independence is approximated by shingle containment and the header publisher / copyright code; true engine ancestry is not measurable here",
        "Windows source-file digests: the bound files are pinned to LF by the existing .gitattributes patterns (tools/segarecomp_genesis_*, tools/*.plan.*, tests/segarecomp_genesis_*_test.py), so a Windows autocrlf checkout cannot change a verified digest"]
    return p


PROTOCOL = build_protocol()
BOUND = tb.BOUND + ["tools/segarecomp_xplat_explore_eval.py", "tools/segarecomp_xplat_transfer.py", "tools/segarecomp_ml_v3.py", "tools/segarecomp_ml_v3_eval.py",
                    "tools/segarecomp_genesis_tasbatch5.py", "tools/segarecomp_genesis_tasbatch5.games.json", "tools/segarecomp_genesis_tasbatch5_ml.py",
                    "tests/segarecomp_genesis_tasbatch5_test.py"]


def configure() -> None:
    """Re-point the SEG-081 module at the five-game table, plan, protocol, ceilings and control (module globals; no file of SEG-081 is modified)."""
    tb.GAMES, tb.ORDER = GAMES, ORDER
    tb.PLAN, tb.PLAN_DIGEST = PLAN, PLAN_DIGEST
    tb.PROTOCOL, tb.BOUND = PROTOCOL, BOUND
    tb.THRESH.update(THRESH)
    tb.control_movie = control_dispatch
    tb.SCHEMA = SCHEMA


configure()


# ------------------------------------------------------------------------------------------------------------------- commands
RENAMES = {"control_seed2": "control_p1only", "extra_vs_control_seed2": "extra_vs_control_p1only", "useful_vs_seed2": "useful_vs_p1only", "ctl2": "ctl_p1only"}


def _rename(o):
    if isinstance(o, dict):
        return {RENAMES.get(k, k): _rename(v) for k, v in o.items()}
    if isinstance(o, list):
        return [_rename(x) for x in o]
    return o


_ORIG_ANALYZE = tb.cmd_analyze


def cmd_analyze(a) -> int:
    rc = _ORIG_ANALYZE(a)
    out = pathlib.Path(a.out)
    res = _rename(json.loads(out.read_text()))
    cov = res.get("coverage")
    if cov:
        e = cov["executed_rom_pcs"]
        res["control_quality"] = {"primary_all_controller_pc_ratio": round(e["control"] / e["tas"], 4) if e["tas"] else None,
                                 "p1only_baseline_pc_ratio": round(e["control_p1only"] / e["tas"], 4) if e["tas"] else None,
                                 "note": "executed ROM PCs of the control divided by the TAS's; a low value means a shallow control (disclosed limitation)"}
    out.write_text(json.dumps(res, indent=1, sort_keys=True))
    return rc


def cmd_decide(a) -> int:
    """Pure: classes -> acquisition class; or readiness measurements -> readiness decision."""
    if a.classes:
        print(json.dumps(acquisition_class(dict(x.split("=", 1) for x in a.classes), a.open_review_blockers), sort_keys=True))
    if a.measurements:
        print(json.dumps(readiness_decision(json.loads(pathlib.Path(a.measurements).read_text())), sort_keys=True))
    return 0


def main(argv=None) -> int:
    tb.cmd_analyze = cmd_analyze
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "decide":
        ap = argparse.ArgumentParser(prog="decide")
        ap.add_argument("cmd")
        ap.add_argument("--classes", nargs="*"), ap.add_argument("--measurements"), ap.add_argument("--open-review-blockers", type=int, default=0)
        return cmd_decide(ap.parse_args(argv))
    return tb.main(argv)


if __name__ == "__main__":
    sys.exit(main())
