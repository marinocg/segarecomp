#!/usr/bin/env python3
"""SEG-083-T001: offline tests of the five-game TAS batch layer (standard library, synthetic data only; no ROM, no emulator, no model)."""
import io
import json
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import segarecomp_genesis_tasbatch5 as t5  # noqa: E402
import segarecomp_genesis_tasbatch5_ml as ml5  # noqa: E402
from segarecomp_genesis_tasbatch_test import make_bk2  # noqa: E402

tb = t5.tb
FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_games_table():
    check(t5.ORDER == ["shadowrun", "nhl94", "shoveit", "doraemon", "terminator"], "frozen order")
    check(tb.ORDER == t5.ORDER and tb.GAMES is t5.GAMES, "the SEG-081 module is re-pointed at the five-game table")
    for g, v in t5.GAMES.items():
        check(v["rom"]["md5"] == v["movie"]["header_md5"], f"{g}: movie-header hash equals the ROM MD5")
        check(len(v["rom"]["sha256"]) == 64 and len(v["movie"]["sha256"]) == 64, f"{g}: digests")
        check(v["lineage"].startswith("T83_"), f"{g}: lineage namespace")
        check(v["movie"]["Core"] == "Genplus-gx" and v["movie"]["frames"] > 10000, f"{g}: core and frames")
    check(len({v["rom"]["sha256"] for v in t5.GAMES.values()}) == 5, "five distinct images")
    s256, s1 = tb.sealed_identities()
    check(all(v["rom"]["sha256"] not in s256 and v["rom"]["sha1"] not in s1 for v in t5.GAMES.values()), "no sealed/blind entry")
    for old in ("vm2", "bnr", "ewj2"):
        check(old not in t5.GAMES, "SEG-081 games are not in the table")


def rows_of(bk2):
    z = zipfile.ZipFile(io.BytesIO(bk2))
    return [x for x in z.read("Input Log.txt").decode().splitlines() if x.startswith("|")]


def test_control_all_controllers():
    for players, six in ((1, False), (2, False), (2, True)):
        src, row = make_bk2(players, six, 10)
        out = t5.control_movie_all(src, 3000, 1)
        zi, zo = zipfile.ZipFile(io.BytesIO(src)), zipfile.ZipFile(io.BytesIO(out))
        check(zi.namelist() == zo.namelist(), "same archive members")
        for n in ("Header.txt", "SyncSettings.json"):
            check(zi.read(n) == zo.read(n), f"{n} untouched")
        check(zo.read("Subtitles.txt") == b"", "subtitles blanked")
        rows = rows_of(out)
        check(len(rows) == 3000 and all(len(r) == len(row) for r in rows), "one row per frame, movie row shape")
        check(rows[0] == row, "row 0 empty")
        p1 = rows_of(tb.control_movie.__wrapped__(src, 3000, 1)) if hasattr(tb.control_movie, "__wrapped__") else rows_of(t5._CONTROL_P1(src, 3000, 1))
        check([r.split("|")[2] for r in rows] == [r.split("|")[2] for r in p1], "player 1 stream equals the SEG-081 player-1 stream")
        if players == 2:
            p2 = [r.split("|")[3] for r in rows]
            check(any(c != "." * (12 if six else 8) for c in p2[2400:]), "player 2 receives inputs")
            check([r.split("|")[3] for r in rows] != [r.split("|")[2] for r in rows], "player 2 stream is independent of player 1")
            check(all(c == "." * (12 if six else 8) for c in [r.split("|")[3] for r in p1]), "SEG-081 baseline keeps player 2 idle")
    src, _ = make_bk2(2, False, 10)
    check(t5.control_movie_all(src, 3000, 1) == t5.control_movie_all(src, 3000, 1), "deterministic")
    check(t5.control_movie_all(src, 3000, 1) != t5.control_movie_all(src, 3000, 2), "seed matters")


def test_dispatch():
    src, _ = make_bk2(2, False, 10)
    check(tb.control_movie is t5.control_dispatch, "tb.cmd_run uses the dispatch")
    check(t5.control_dispatch(src, 600, 1) == t5.control_movie_all(src, 600, 1), "ctl_a = all-controller primary")
    check(t5.control_dispatch(src, 600, 2) == t5._CONTROL_P1(src, 600, 1), "ctl2_a = player-1-only comparability baseline")


def test_acquisition_class():
    q = "QUALIFIED_USEFUL"
    low = "QUALIFIED_LOW_YIELD"
    check(t5.acquisition_class({"a": q, "b": q, "c": q, "d": low, "e": "REPLAY_INVALID"})["decision"] == "EXPANSION_SUPPORTED", "3 useful")
    check(t5.acquisition_class({"a": q, "b": q, "c": q, "d": "LABEL_POLICY_CONFLICT", "e": low})["decision"] == "EXPANSION_MIXED", "major inconsistency")
    check(t5.acquisition_class({"a": q, "b": q, "c": q}, 1)["decision"] == "EXPANSION_MIXED", "open review blocker")
    check(t5.acquisition_class({"a": q, "b": low, "c": low})["decision"] == "EXPANSION_MIXED", "one useful")
    check(t5.acquisition_class({"a": low, "b": low, "c": "INPUT_REQUIRED"})["decision"] == "EXPANSION_NOT_SUPPORTED", "none useful")
    check(t5.acquisition_class({"a": low, "b": "REPLAY_INVALID"})["decision"] == "EXPANSION_MIXED", "single low-yield game")
    check(t5.acquisition_class({"a": "REPLAY_INVALID", "b": "INPUT_REQUIRED"})["decision"] == "EVIDENCE_INSUFFICIENT", "none qualified")


def test_independent_lineages():
    games = ["a", "b", "c", "d", "e"]
    r = t5.independent_lineages(games, {("a", "b"): 0.5, ("b", "c"): 0.1}, {"d": 0.4})
    check(r["redundant_with_registry"] == ["d"], "registry-redundant game is excluded")
    check(r["effective_independent"] == 3 and r["titles"] == 5, "a~b cluster, c, e")
    check(t5.independent_lineages(games, {}, {})["effective_independent"] == 5, "no overlap: all independent")
    check(t5.independent_lineages(games, {("a", "b"): 0.3}, {})["effective_independent"] == 5, "threshold is strict (> 0.30)")
    chain = t5.independent_lineages(games, {("a", "b"): 0.9, ("b", "c"): 0.9}, {})
    check(chain["effective_independent"] == 3, "transitive clustering")


def test_readiness():
    ok = {"new_useful_independent_lineages": 4, "total_useful_independent_lineages": 6, "new_novel_signatures": 400, "new_hard_negative_cells": 400, "residual_folds": 2, "provenance_ok": True}
    check(t5.readiness_decision(ok)["decision"] == "NEURAL_PILOT_JUSTIFIED", "all criteria")
    for k, v, want in (("new_useful_independent_lineages", 2, "MORE_DIVERSITY_NEEDED"), ("total_useful_independent_lineages", 4, "MORE_DIVERSITY_NEEDED"),
                       ("new_novel_signatures", 299, "MORE_DIVERSITY_NEEDED"), ("new_hard_negative_cells", 299, "MORE_DIVERSITY_NEEDED"), ("residual_folds", 1, "MORE_DIVERSITY_NEEDED"),
                       ("provenance_ok", False, "MORE_DIVERSITY_NEEDED"), ("new_useful_independent_lineages", 1, "NEURAL_RETRAINING_NOT_JUSTIFIED"),
                       ("new_novel_signatures", 99, "NEURAL_RETRAINING_NOT_JUSTIFIED"), ("residual_folds", 0, "NEURAL_RETRAINING_NOT_JUSTIFIED")):
        d = t5.readiness_decision(dict(ok, **{k: v}))
        check(d["decision"] == want, f"{k}={v} -> {want} (got {d['decision']})")
        check(bool(d["missing"] or d["reasons"]), f"{k}: names the missing kind of evidence")
    # SEG-081 evidence alone cannot satisfy the new-batch criteria
    check(t5.readiness_decision(dict(ok, new_useful_independent_lineages=0, total_useful_independent_lineages=3))["decision"] == "NEURAL_RETRAINING_NOT_JUSTIFIED", "old games do not carry readiness")
    check("AP" not in json.dumps(t5.READY) and "headroom" not in json.dumps(t5.READY), "no AP or historical-headroom criterion in the readiness rule")


def fake_image(er_pos, n=8, cell=128):
    """classes with 'I'/'D' bytes: first `er_pos` cells code, rest data."""
    cls = bytearray()
    for c in range(n):
        cls += (b"I" if c < er_pos else b"D") * cell
    return bytes(cls)


def test_region_metrics():
    cls = fake_image(3)
    good = [5, 4, 3, 0, 0, 0, 0, 0]
    m = ml5.image_region_metrics(cls, [], good)
    check(m["informative"] and m["er"] == 0.0, "perfect ranking has zero excess retention")
    bad = [0, 0, 0, 5, 4, 3, 2, 1]
    mb = ml5.image_region_metrics(cls, [], bad)
    check(mb["er"] == 1.0, "inverted ranking needs every cell")
    check(mb["misses_B35"] > 0 and m["misses_B35"] == 0, "B35 misses count instruction starts outside the region")
    fs = ml5.fold_summary([m, mb])
    check(fs["er_max"] == 1.0 and fs["images"] == 2, "lineage max ER")
    check(0 <= fs["recall_cells_B50"] <= 1, "recall bounded")
    h = ml5.headroom({"D01": 0.12, "D02": 0.06, "D03": 0.01, "D04": None})
    check("B0_sub" in ml5.ARMS, "noise-null arm present")
    check(h["folds_ge_005"] == 2 and h["folds_ge_010"] == 1, "headroom counts")


def test_plan():
    p = t5.PROTOCOL
    check(p["limits"]["cpu_hours_per_game"] == 8 and p["limits"]["cpu_hours_total"] == 40, "ceilings 8 h / 40 h")
    check(tb.THRESH["cpu_hours_total"] == 40, "tool ceiling")
    check(p["usefulness"]["rule"] == tb.PROTOCOL["usefulness"]["rule"] and tb.THRESH["extra_strong_code_cells_abs"] == 100, "SEG-081 usefulness criterion retained")
    check("limitation" in p["control"] and "ALL-CONTROLLER" in p["control"]["primary"], "primary control drives every controller; limitation disclosed")
    check(set(p["readiness"]["classes"]) == set(t5.READINESS), "readiness outcomes")
    check("not_required" in p["readiness"] and "not run in SEG-083" in p["readiness"]["pilot_specification_if_justified"], "pilot specified, not run")
    check(set(p["model_a"]["arms"]) == set(ml5.ARMS), "model-A arms match the implementation")
    if t5.PLAN.exists():
        check(bool(tb.verify_plan()), "frozen five-game plan verifies")
        check(t5.PLAN_DIGEST.read_text().strip() == tb.ge.sha256(t5.PLAN.read_bytes()), "digest matches plan bytes")
    # SEG-081 frozen artifacts untouched by importing this layer
    import hashlib
    d81 = (ROOT / "tools" / "segarecomp_genesis_tasbatch.plan.digest").read_text().strip()
    check(hashlib.sha256((ROOT / "tools" / "segarecomp_genesis_tasbatch.plan.json").read_bytes()).hexdigest() == d81, "SEG-081 plan bytes unchanged")


def test_committed_results_are_aggregate_only():
    banned = ('"/root', "/home/", ".bin\"")
    for f in sorted((ROOT / "tools").glob("segarecomp_genesis_tasbatch5*.json")):
        txt = f.read_text()
        for b in banned:
            check(b not in txt, f"{f.name}: no path or file name ({b})")


def main():
    test_games_table()
    test_control_all_controllers()
    test_dispatch()
    test_acquisition_class()
    test_independent_lineages()
    test_readiness()
    test_region_metrics()
    test_plan()
    test_committed_results_are_aggregate_only()
    print("FAILURES:", len(FAILURES), *FAILURES)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
