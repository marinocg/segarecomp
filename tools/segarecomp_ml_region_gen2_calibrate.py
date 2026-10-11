#!/usr/bin/env python3
"""SEG-049-T002 (ADR 0101): OFFLINE evaluation driver for the pre-registered rank budgets B30..B50 over the frozen gen2 M3 outer folds.

Private research tooling (numpy/torch are NOT product dependencies). All definitions live in the stdlib modules
`segarecomp_ml_region_gen2` (T001) and `segarecomp_ml_region_gen2_rank` (T002); this file only wires private inputs to them.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import segarecomp_ml_region_gen2 as g  # noqa: E402
import segarecomp_ml_region_gen2_rank as rk  # noqa: E402
import segarecomp_ml_region_gen2_train as tr  # noqa: E402
import segarecomp_ml_region_v2 as v2  # noqa: E402

RANK_PLAN = TOOLS / "segarecomp_ml_region_gen2_rank.plan.json"
T001_PLAN = TOOLS / "segarecomp_ml_region_gen2.plan.json"


def load_outer_scores():
    """Reuse the private T001 outer M3 scores only if every prediction digest equals the one frozen in the T002 plan."""
    import numpy as np
    plan = rk.verify_plan(RANK_PLAN, T001_PLAN)
    scores = {}
    for t in g.TITLE_ORDER:
        s = np.load(tr.scores_path("M3", t))
        if g.prediction_digest({t: s.tolist()}) != plan["frozen_from"]["outer_score_digests"][t]:
            raise SystemExit(f"outer score identity mismatch for {t}: regenerate with the frozen M3 recipe")
        scores[t] = s
    return plan, scores


def cmd_evaluate(args) -> int:
    plan, scores = load_outer_scores()
    titles = {t: tr.Title(t) for t in g.TITLE_ORDER}
    runner = v2.PruneRunner(str(tr.CLI), str(tr.ROOT / "prune"))
    folds = {}
    for held in g.TITLE_ORDER:
        t = titles[held]
        folds[held] = {}
        for name, pct in rk.BUDGET_PERCENT.items():
            rec = rk.r_stage(scores[held].tolist(), t.labels.tolist(), t.truth, t.seeds, t.size, pct)
            region = rec.pop("_region")
            rec["r_gate"] = rk.r_gate(rec)
            if rec["r_gate"]:  # K stage only for R-passing policies (plan): unchanged structural prune + validator
                stats = runner.run(t, g.regions_text(t.sha, t.size, region))
                accepted = stats.get("returncode") == 0 and stats.get("validator") == "accepted"
                k_set = v2.admitted_set(t.universe, stats["ranges"]) if accepted else None
                att = g.attribute(t.truth, region, k_set, accepted)
                rec.update({"validator": "accepted" if accepted else "rejected", "k": len(k_set) if accepted else None,
                            "universe_u": len(t.universe), "k_over_u": len(k_set) / len(t.universe) if accepted else None,
                            "attribution": att, "pruned": int(stats["pruned"]) if accepted else None})
                rec["k_gate"] = rk.k_gate(rec)
            folds[held][name] = rec
    r_ok, k_ok = rk.policy_r_passes(folds), rk.policy_k_passes(folds)
    chosen = rk.choose_budget(folds)
    out = {"schema": "segarecomp.ml_region_gen2_rank_results.v1", "plan_sha256": rk.plan_digest(plan), "folds": folds,
           "policy_r_passes": r_ok, "policy_k_passes": k_ok, "selected_policy": chosen,
           "data_boundary": "aggregates only; no address, per-cell label, score array or ROM byte"}
    (tr.ROOT / "results").mkdir(exist_ok=True)
    (tr.ROOT / "results" / "rank_evaluate.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({"selected_policy": chosen, "r": r_ok, "k": k_ok}))
    return 0


def cmd_final(args) -> int:
    """Fit the exact frozen T001 M3 recipe on ALL five titles (no held-out title exists for the research candidate). Run twice independently."""
    import numpy as np
    import torch
    plan = rk.verify_plan(RANK_PLAN, T001_PLAN)
    tr.verify_plan()
    titles = {t: tr.Title(t) for t in g.TITLE_ORDER}
    train_ids = list(g.TITLE_ORDER)
    out = tr.fit_and_score("M3", titles, train_ids, train_ids, log=lambda m: print(f"[final/{args.run}] {m}", flush=True))
    model = out["_model"]
    d = tr.ROOT / "final"
    d.mkdir(exist_ok=True)
    state = {k: v.detach().double().numpy() for k, v in model.state_dict().items() if not k.endswith("num_batches_tracked")}
    np.savez(d / f"run{args.run}.weights.npz", **state)
    np.savez(d / f"run{args.run}.scores.npz", **{t: out[t] for t in train_ids})
    digest = g.prediction_digest({t: out[t].tolist() for t in train_ids})
    (d / f"run{args.run}.json").write_text(json.dumps({"run": args.run, "score_digest": digest, "params": out["_meta"]["params"],
                                                       "rank_plan_sha256": rk.plan_digest(plan)}, indent=1, sort_keys=True))
    print(json.dumps({"run": args.run, "score_digest": digest}))
    return 0


def cmd_freeze(args) -> int:
    """Compare the two independent all-five-title fits, then write the model / frozen / parity research artifacts (selected policy from results)."""
    import numpy as np
    import torch
    import segarecomp_ml_region_gen2_reference as ref
    plan = rk.verify_plan(RANK_PLAN, T001_PLAN)
    result = json.loads((tr.ROOT / "results" / "rank_evaluate.json").read_text())
    policy_id = result["selected_policy"]
    if policy_id is None or result["plan_sha256"] != rk.plan_digest(plan):
        raise SystemExit("no selected policy (or the evaluation belongs to a different plan): nothing to freeze")
    d = tr.ROOT / "final"
    w1, w2 = np.load(d / "run1.weights.npz"), np.load(d / "run2.weights.npz")
    s1, s2 = np.load(d / "run1.scores.npz"), np.load(d / "run2.scores.npz")
    weights_equal = sorted(w1.files) == sorted(w2.files) and all(np.array_equal(w1[k], w2[k]) for k in w1.files)
    digests = [g.prediction_digest({t: z[t].tolist() for t in g.TITLE_ORDER}) for z in (s1, s2)]
    scores_equal = all(np.array_equal(s1[t], s2[t]) for t in g.TITLE_ORDER)
    if not (weights_equal and scores_equal and digests[0] == digests[1]):
        raise SystemExit("final fit is not deterministic across independent runs")
    weights = {k: tr._nested(w1[k].tolist()) for k in sorted(w1.files)}
    model_doc = {"schema": "segarecomp.ml_region_gen2_model.v1", "model": "M3", "architecture_ref": "tools/segarecomp_ml_region_gen2.plan.json models.M3",
                 "trainable_parameters": 77441, "weights_format": "PyTorch eval-mode state dict, nested lists, 9 significant digits (float32 exact)",
                 "weights": weights}
    out = TOOLS
    model_text = json.dumps(model_doc, sort_keys=True, separators=(",", ":")) + "\n"
    (out / rk.FROZEN_NAMES["model"]).write_text(model_text)
    model_sha = g.sha256_bytes(model_text.encode())
    # parity: torch model rebuilt from the EXPORTED (rounded) weights versus the stdlib reference evaluator on deterministic synthetic probes
    model = tr.build_model(True)
    model.load_state_dict({k: torch.tensor(np.array(v), dtype=torch.float32) for k, v in weights.items()} | {
        k: v for k, v in model.state_dict().items() if k.endswith("num_batches_tracked")})
    model.eval()
    probes = rk.parity_probes()
    ref_logits, torch_logits = [], []
    for pr in probes:
        ref_logits.append(ref.cnn_logit(weights, pr["hi"], pr["lo"], pr["tok"]))
        with torch.no_grad():
            torch_logits.append(float(model(torch.tensor(pr["hi"])[None].long(), torch.tensor(pr["lo"])[None].long(), torch.tensor(pr["tok"])[None].long())[0, 0]))
    max_diff = max(abs(a - b) for a, b in zip(ref_logits, torch_logits))
    if max_diff > 1e-5:
        raise SystemExit(f"reference/torch parity failure: {max_diff}")
    pct = rk.BUDGET_PERCENT[policy_id]
    selected_digest, selected_counts = {}, {}
    for t in g.TITLE_ORDER:
        sel = sorted(rk.rank_select(s1[t].tolist(), pct))
        selected_counts[t] = len(sel)
        selected_digest[t] = g.sha256_bytes(("".join(f"{c}\n" for c in sel)).encode())
    policy = {"id": policy_id, "percent": pct, "kind": "rank budget"}
    parity = {"schema": "segarecomp.ml_region_gen2_parity.v1", "model_sha256": model_sha, "policy": policy,
              "synthetic_probes": {"count": len(probes), "generator": "64-bit LCG seeded from the gen2 seed (no ROM data)",
                                   "reference_logits_digest": rk.logits_digest(ref_logits), "reference_logits_6e": [f"{x:.6e}" for x in ref_logits],
                                   "max_abs_diff_reference_vs_torch": max_diff},
              "in_sample_prediction_digest": digests[0], "in_sample_selected_cells": selected_counts,
              "in_sample_selected_cell_set_digest": selected_digest,
              "note": "in-sample (all five fitting titles) and aggregate/hash only: no address, per-cell score or ROM byte is stored"}
    parity_text = json.dumps(parity, indent=1, sort_keys=True) + "\n"
    (out / rk.FROZEN_NAMES["parity"]).write_text(parity_text)
    plan_bytes = (out / rk.FROZEN_NAMES["plan"]).read_bytes()
    frozen = {"schema": "segarecomp.ml_region_gen2_frozen.v1", "experiment": "SEG-049-T002", "status": "RESEARCH CANDIDATE (not production)",
              "representation_version": g.REPRESENTATION_VERSION, "representation_sha256": g.representation_sha256(),
              "token_schema_sha256": g.token_schema_sha256(), "architecture": "M3 (T001 plan models.M3)", "trainable_parameters": 77441,
              "recipe": tr.RECIPE, "seed": g.SEED, "corpus": g.CORPUS, "training_titles": list(g.TITLE_ORDER),
              "t001_plan_sha256": rk.t001_plan_digest(T001_PLAN), "rank_plan_sha256": rk.plan_digest(plan),
              "model_artifact": {"file": rk.FROZEN_NAMES["model"], "sha256": model_sha},
              "plan_artifact": {"file": rk.FROZEN_NAMES["plan"], "sha256": g.sha256_bytes(plan_bytes)},
              "parity_artifact_sha256": g.sha256_bytes(parity_text.encode()),
              "policy": policy, "prediction_digest_in_sample": digests[0],
              "deterministic_refit": {"independent_fits": 2, "weights_bitwise_equal": True, "predictions_bitwise_equal": True,
                                      "torch_threads": tr.RECIPE["torch_threads"]},
              "environment": plan["frozen_from"] and json.loads(T001_PLAN.read_text())["environment"],
              "scope": "OFFLINE research candidate; production Optimized AOT continues to use SEG-047 v1; no native inference yet"}
    (out / rk.FROZEN_NAMES["frozen"]).write_text(json.dumps(frozen, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"policy": policy_id, "model_sha256": model_sha, "digest": digests[0], "max_diff": max_diff}))
    return 0


# ------------------------------------------------------------------------------------------------------------- runtime / economics
HIST_ROMS = {"s2": "Sonic The Hedgehog 2 (World).md", "cs": "Cool Spot.md", "sor": "Streets of Rage.md", "or": "OutRun.md"}


def _hist_title(tid: str):
    """Title-like object for a ROM outside the corpus (bytes + CPU tokens only; no truth, no universe)."""
    import hashlib
    import subprocess
    import numpy as np
    t = object.__new__(tr.Title)
    t.id = tid
    path = tr.GAMES / HIST_ROMS[tid]
    t.rom = path.read_bytes()
    t.sha = hashlib.sha256(t.rom).hexdigest()
    t.size, t.rom_path = len(t.rom), str(path)
    t.n_words, t.n_cells = t.size // 2, g.cell_count(t.size)
    tok = tr.ROOT / "tok" / f"{tid}.tok"
    if not tok.exists():
        subprocess.run([str(tr.CLI), "m68k-decode-token-report", "--rom", t.rom_path, "--output", str(tok)], check=True)
    body = g.parse_token_report(tok.read_bytes(), t.size)
    t.tokens = np.frombuffer(body, dtype=np.uint8).reshape(t.n_words, len(g.TOKEN_FIELDS))
    arr = np.frombuffer(t.rom[:t.n_words * 2], dtype=np.uint8).reshape(t.n_words, 2)
    t.hi, t.lo = arr[:, 0], arr[:, 1]
    t.labels = t.truth = None
    t._padded = None
    return t


def _seeds_for(t, work: pathlib.Path) -> set:
    import subprocess
    work.mkdir(parents=True, exist_ok=True)
    d = work / "d.txt"
    subprocess.run([str(tr.CLI), "emit-general-startup-bridge-c", "--rom", t.rom_path, "--reset-entry", "--rom-sha256", t.sha, "--immutable-rom-aot",
                    "--direct-control-address-report", str(d), "--immutable-aot-address-report", str(work / "u.txt"),
                    "--generated-c-output", str(work / "probe.c")], check=True, capture_output=True)
    seeds = g.seed_cells(d.read_text(), t.size)
    (work / "probe.c").unlink(missing_ok=True)
    return seeds


def _final_model():
    import numpy as np
    import torch
    w = np.load(tr.ROOT / "final" / "run1.weights.npz")
    model = tr.build_model(True)
    sd = model.state_dict()
    model.load_state_dict({k: (torch.tensor(w[k], dtype=torch.float32) if k in w.files else v) for k, v in sd.items()})
    return model


def _timed(cmd, log: pathlib.Path, timeout: int):
    """Run `cmd` under /usr/bin/time; returns (completed, wall, cpu, rss_kib). cpu = user+sys of the whole process tree."""
    import subprocess
    stat = log.with_suffix(".time")
    done = subprocess.run(["/usr/bin/time", "-f", "%e %U %S %M", "-o", str(stat)] + cmd, capture_output=True, text=True, timeout=timeout)
    wall, user, sys_, rss = stat.read_text().split()[-4:]
    return done, float(wall), float(user) + float(sys_), int(rss)


def _bridge_variant(rom: pathlib.Path, out: pathlib.Path, frames: int, extra: list, timeout: int) -> dict:
    import shutil
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    cmd = [sys.executable, str(TOOLS / "genesis_startup_bridge.py"), "--segarecomp", str(tr.CLI), "--rom", str(rom), "--mode", "commercial",
           "--immutable-rom-aot", "--execution-coverage", str(frames), "--coverage-epoch-frames", str(frames), "--coverage-no-render",
           "--instruction-budget", str(max(10_000_000, frames * 100_000)), "--out-dir", str(out), "--cc", "clang"] + extra
    done, wall, cpu, rss = _timed(cmd, out / "bridge.log", timeout)
    line = next((ln for ln in done.stderr.splitlines() if ln.startswith("COVERAGE_SUMMARY ")), None)
    files = [p for p in (out / "generated").rglob("*") if p.is_file()] if (out / "generated").exists() else []
    files += [p for p in out.glob("*.generated.c") if p.is_file()]
    rec = {"ok": line is not None, "wall_seconds": wall, "cpu_seconds": cpu, "peak_rss_kib": rss,
           "generated_bytes": sum(p.stat().st_size for p in files), "generated_lines": sum(p.read_bytes().count(b"\n") for p in files)}
    if line:
        d = json.loads(line.split(" ", 1)[1])
        rec.update({"outcome": d["outcome"], "frames": d["frames_published"], "final_state": d["final_state_digest"],
                    "frame_stream": d["frame_stream_digest"], "coverage": d["coverage_digest"]})
    return rec


def _executed_rom_pcs(out: pathlib.Path, rom_size: int) -> set:
    bm = (out / "coverage-private" / "coverage.bitmap").read_bytes()
    pcs = set()
    for byte_index in range(min(len(bm), (rom_size + 15) // 16)):
        b = bm[byte_index]
        if b:
            for bit in range(8):
                if b >> bit & 1:
                    pc = (byte_index * 8 + bit) * 2
                    if pc < rom_size:
                        pcs.add(pc)
    return pcs


def cmd_runtime(args) -> int:
    """Broad vs production v1 (--ml-admission) vs gen2 B-plan (--admission-plan) for one title: behaviour equivalence, K containment, economics."""
    import subprocess
    import time
    plan = rk.verify_plan(RANK_PLAN, T001_PLAN)
    result = json.loads((tr.ROOT / "results" / "rank_evaluate.json").read_text())
    policy = result["selected_policy"]
    pct = rk.BUDGET_PERCENT[policy]
    tid, work = args.title, tr.ROOT / "runtime" / args.title
    work.mkdir(parents=True, exist_ok=True)
    inference_seconds = None
    if tid in g.TITLE_ORDER:
        _, scores = load_outer_scores()
        t = tr.Title(tid, with_truth=False)
        seeds, ml_scores, model_kind = t.seeds, scores[tid].tolist(), "outer held-out M3"
    else:
        t = _hist_title(tid)
        seeds = _seeds_for(t, work)
        model = _final_model()
        started = time.monotonic()
        ml_scores = tr.sigmoid(tr.score_logits(model, t, True)).tolist()
        inference_seconds = time.monotonic() - started
        model_kind = "final all-five M3 (title outside the corpus)"
    region = rk.rank_select(ml_scores, pct) | set(seeds)
    regions_path, plan_path = work / "gen2.regions", work / "gen2.plan"
    regions_path.write_text(g.regions_text(t.sha, t.size, region), newline="\n")
    prune = subprocess.run([str(tr.CLI), "emit-general-startup-bridge-c", "--rom", t.rom_path, "--reset-entry", "--rom-sha256", t.sha, "--immutable-rom-aot",
                            "--immutable-aot-region-proposal", str(regions_path), "--region-admission-plan-output", str(plan_path)],
                           capture_output=True, text=True)
    rec = {"title": tid, "policy": policy, "model": model_kind, "r_over_rom": g.region_bytes(region, t.size) / t.size,
           "validator": "accepted" if prune.returncode == 0 else "rejected", "research_inference_seconds": inference_seconds}
    ranges = [tuple(int(x, 16) for x in ln.split()[1:3]) for ln in plan_path.read_text().splitlines() if ln.startswith("range ")] if prune.returncode == 0 else []
    rec["k_bytes"] = sum(e - b for b, e in ranges)
    rec["k_over_rom"] = rec["k_bytes"] / t.size
    if prune.returncode != 0:
        (work / "result.json").write_text(json.dumps(rec, indent=1, sort_keys=True))
        print(json.dumps(rec))
        return 0
    rom = pathlib.Path(t.rom_path)
    vwork = TOOLS.parent / "build" / "seg049-t002" / tid  # the bridge requires an ignored out-dir inside the product worktree
    variants = {"broad": [], "v1": ["--ml-admission"], "gen2": ["--admission-plan", str(plan_path)]}
    pcs = {}
    for name in args.variants.split(","):
        rec[name] = _bridge_variant(rom, vwork / name, args.frames, variants[name], args.timeout)
        pcs[name] = _executed_rom_pcs(vwork / name, t.size) if rec[name]["ok"] else None
    if pcs.get("broad") is not None:
        starts = [b for b, _ in ranges]
        import bisect

        def inside(pc):
            i = bisect.bisect_right(starts, pc) - 1
            return i >= 0 and pc < ranges[i][1]
        rec["broad_executed_pcs"] = len(pcs["broad"])
        rec["broad_executed_pcs_outside_k"] = sum(1 for pc in pcs["broad"] if not inside(pc))
        if pcs.get("gen2") is not None:
            rec["gen2_executed_pcs"] = len(pcs["gen2"])
            rec["gen2_executed_pcs_outside_broad"] = len(pcs["gen2"] - pcs["broad"])
            rec["broad_pcs_not_executed_by_gen2"] = len(pcs["broad"] - pcs["gen2"])
    keys = ("outcome", "frames", "final_state", "frame_stream", "coverage")
    for name in ("v1", "gen2"):
        if rec.get(name, {}).get("ok") and rec.get("broad", {}).get("ok"):
            rec[f"{name}_equal_to_broad"] = {k: rec[name][k] == rec["broad"][k] for k in keys}
    (work / "result.json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in rec if k not in ("broad", "v1", "gen2")}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("evaluate").set_defaults(func=cmd_evaluate)
    final = sub.add_parser("final")
    final.add_argument("--run", type=int, required=True, choices=(1, 2))
    final.set_defaults(func=cmd_final)
    sub.add_parser("freeze").set_defaults(func=cmd_freeze)
    rt = sub.add_parser("runtime")
    rt.add_argument("--title", required=True, choices=list(g.TITLE_ORDER) + list(HIST_ROMS))
    rt.add_argument("--frames", type=int, default=600)
    rt.add_argument("--variants", default="broad,v1,gen2")
    rt.add_argument("--timeout", type=int, default=2400)
    rt.set_defaults(func=cmd_runtime)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
