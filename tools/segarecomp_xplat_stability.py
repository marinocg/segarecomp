#!/usr/bin/env python3
"""SEG-076-T003 (exploratory, development-only): M3-R seed stability, paired multi-seed C0/C2 transfer, A and M1-HGB re-comparison.

  diagnose  Step 1 (evidence tier 1, reanalysis, no fits): M3-R seed 49 vs 50 per image / lineage: ER, FRF, B50 misses, between-seed
            cell-rank correlation, boundary-stratum membership of the weakest positive (the cell that sets FRF/ER), loss curves from the
            preserved logs, training class balance; aggregate-only output (D01 images are pseudonymous size-ordered indices)
  plan      write the frozen T003 plan (tools/segarecomp_xplat_stability.plan.json + .digest): bound code (this file, the SEG-076 T001
            plan / evaluation module, the unchanged SEG-075 / SEG-072 closure), corpus files, reused score digests, seeds, jobs, rules,
            resources; committed and reviewed BEFORE the first T003 fit
  check     verify the plan
  fit       one fit: M3R --condition C0|C2 --fold <lineage> --seed 50..52 | A --condition C2 --fold <lineage>; write-once, stamped
  farm      the pre-registered jobs: crash-safe attempt accounting, wall-clock / RSS caps, host-RAM guard (as the T001 ROCm amendment)
  table     aggregate-only results (tools/segarecomp_xplat_stability.results.json) with the pre-registered classes

Evidence tiers (every reported finding carries one): 1 = reanalysis of existing development results; 2 = new training-seed experiment
on the existing development folds (confirmatory only for the pre-registered development-fold question); 3 = independent evaluation on
unseen lineages (none here).
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import os
import pathlib
import platform
import re
import resource
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_explore as rx  # noqa: E402
import segarecomp_xplat_explore_eval as ev6  # noqa: E402

PLAN = HERE / "segarecomp_xplat_stability.plan.json"
DIGEST = HERE / "segarecomp_xplat_stability.plan.digest"
RESULTS = HERE / "segarecomp_xplat_stability.results.json"
DIAGNOSIS = HERE / "segarecomp_xplat_stability.diagnosis.json"
SEEDS = (49, 50, 51, 52)
NEW_SEEDS = {"C0": (51, 52), "C2": (50, 51, 52)}
DETERMINISM = (("C0", "D01", 50), ("C2", "D01", 49))
T001_OUT = rx.HARNESS / ".cache" / "seg076" / "t001"
SEG072_LOG = rx.HARNESS / ".cache" / "seg072" / "m3r.log"
T004_LOGS = rx.SEG075 / "t004" / "logs"
BOUND_EXTRA = ("segarecomp_xplat_stability.py", "segarecomp_xplat_explore.t001.plan.json", "segarecomp_xplat_explore.t001.plan.digest")
RESOURCES = {"backend": "CPU (research venv torch 2.14.1, the device of every reused M3-R fit)", "m3r_threads": 4, "a_threads": 2,
             "parallel_fits": 3, "start_stagger_seconds": 60, "wall_clock_cap_minutes": {"M3R": 90, "A": 20}, "rss_limit_mb": 4096,
             "min_available_mb_to_start": 4000, "host_memory_floor_mb": 2000, "attempts": 1,
             "infra_rerun": "once, only after 'interrupted' (host crash: a persisted 'started') or an external signal kill, identical "
                            "configuration, recorded with a reason; never after timeout, rss_limit, host_memory_guard or a model error. "
                            "Disclosed edge cases: a host crash after the score and record are written but before the farm records ok "
                            "leaves an 'interrupted' job whose re-run refuses (write-once) - fail-closed, the job counts as failed; a "
                            "kernel OOM kill is recorded killed_signal9 and is re-runnable once",
             "basis": "SEG-075 T004 M3-R fits: ~21 min mean at 4 threads in a 4-fit farm (max 38 min); 3 parallel x 4 threads on the 8-core "
                      "host; cap ~4x the mean"}
RULES = {
    "noise": "per condition: per-lineage between-seed spread of the lineage max ER over seeds 49-52; tau4 = 2 x mean over the 11 lineages "
             "of the mean absolute pairwise difference (the SEG-075 tau formula generalized from one pair to all 6 pairs)",
    "transfer": {"statistic": "d_s = Genesis mean ER (C2, seed s) - Genesis mean ER (C0, seed s), s in 49..52 (paired by seed number)",
                 "pairing": "the C0 and C2 runs of seed s share only the initial weights (torch.manual_seed precedes build_model); the "
                            "segment offsets / order walk different training lists, so the pairing is nominal and the df=3 paired t is "
                            "conservative against an unpaired design",
                 "ci": "two-sided 95% t interval over the 4 paired seeds (t_0.975,3 = 3.182)",
                 "lineage_test": "exact two-sided sign-flip permutation over the 11 lineages of the seed-averaged per-lineage paired "
                                 "difference (2048 sign patterns)",
                 "classes": {"TRANSFER_EFFECT_MEASURABLE": "the t interval excludes 0 AND |mean d| >= 0.01 AND sign-flip p <= 0.05 "
                                                           "(direction: helps if negative, harms if positive)",
                             "EFFECT_LINEAGE_CONCENTRATED": "the t interval excludes 0 AND |mean d| >= 0.01 but sign-flip p > 0.05 "
                                                            "(the seed-level effect is carried by few lineages)",
                             "NO_EFFECT_DETECTED_BOUNDED": "otherwise, when the whole t interval lies within [-0.02, +0.02] (an "
                                                           "equivalence margin of twice the 0.01 relevance floor: any effect beyond "
                                                           "0.02 is excluded); maps to NO_MEANINGFUL_ADVANTAGE in the synthesis",
                             "EVIDENCE_INSUFFICIENT": "otherwise (the interval reaches beyond +/-0.02)"},
                 "power": "expected: the existing C0 seeds 49/50 differ by 0.036 in Genesis mean ER; with independent seeds SD(d) ~0.036 "
                          "gives a half-width ~0.057, so EVIDENCE_INSUFFICIENT is the likely outcome; stated before any fit",
                 "persistence": "number of lineages whose seed-averaged difference has the sign of the mean effect; D01 reported"},
    "stability": {"statistic": "score-averaging gain g_score = metric(logit-averaged ensemble) - m_lin, where m_lin applies the SAME "
                               "aggregation (lineage max over images, Genesis mean over lineages) to the image-wise mean of the member "
                               "seeds' image ERs. Pure averaging of member image ERs gives g_score = 0 exactly; g_score < 0 means the "
                               "averaged SCORES rank the tail better than averaging the members' outcomes. The Jensen term g_jensen = "
                               "m_lin - mean(member metrics) <= 0 (max is convex) is reported descriptively only",
                  "units": "the 6 seed pairs per condition and the 4-seed ensemble (for the Genesis mean ER and for D01 max ER)",
                  "class": "SEED_STABILITY_IMPROVED iff in BOTH conditions: at least 5 of the 6 pair ensembles have D01 g_score < 0, the "
                           "mean pair g_score of the Genesis mean ER is <= -0.005, and the 4-seed ensemble's Genesis g_score is <= 0; "
                           "otherwise NO_MEANINGFUL_ADVANTAGE (EVIDENCE_INSUFFICIENT when fits failed). The 5-of-6 count is a heuristic: "
                           "the pairs share member seeds, so no binomial p is claimed; power is low (4 seeds) and disclosed",
                  "descriptive": "SD of the 6 overlapping pair ensembles / SD of the 4 single seeds is reported against its arithmetic "
                                 "null of exactly 1/sqrt(3) = 0.577 for a linear statistic (overlapping pairs); it is NOT a criterion"},
    "a_transfer": "A is deterministic: d = Genesis mean ER (A, C2) - (A, C0) with per-lineage differences and the exact sign-flip test; "
                  "TRANSFER_EFFECT_MEASURABLE iff |d| >= 0.01 AND sign-flip p <= 0.05; else NO_MEANINGFUL_ADVANTAGE (tier 2: new fits "
                  "on the existing folds; training-noise free, so only lineage resampling bounds it)",
    "m1": "tier 1 reanalysis of the SEG-075 M1-HGB C0 / C2 fold scores (M1 is deterministic: its random_state has no effect below "
          "sklearn's 200k-sample binning threshold): per-lineage paired differences, lineage bootstrap 95% percentile CI (10000 "
          "resamples, numpy default_rng(0)), exact sign-flip p; DESCRIPTIVE_EFFECT_ROBUST iff the bootstrap CI excludes 0 and p <= "
          "0.05, else DESCRIPTIVE_EFFECT_NOT_ROBUST; never a confirmatory claim",
    "determinism": "two refits are compared bitwise with the reused files: M3R C0/D01/s50 (SEG-075 T004) and M3R C2/D01/s49 (SEG-075 "
                   "T004, also checking that the SEG-076 loader path equals T004's for C2). If either is not bitwise identical, or fails, "
                   "every tier-2 M3-R class (transfer, stability, and the descriptive secondary / ensemble contrasts) is reported "
                   "EVIDENCE_INSUFFICIENT (mixed provenance of reused and new seeds)",
    "barriers": "D01 is never excluded from any aggregate; the SEG-075 reserved cohort stays sealed; no SEG-075 tau / decision changes",
}


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def bound_files() -> list[str]:
    import segarecomp_xplat_explore as m
    return sorted(set(m.BOUND_CODE) | set(BOUND_EXTRA))


def reused_scores() -> dict[str, pathlib.Path]:
    s72, t4 = rx.xt().SEG072_SCORES, rx.SEG075 / "t004" / "scores"
    out = {}
    for f in rx.GENESIS:
        out[f"seg072/M3R.{f}.npz"] = s72 / f"M3R.{f}.npz"                       # C0 seed 49
        out[f"seg075/M3R.C0.{f}.s50.npz"] = t4 / f"M3R.C0.{f}.s50.npz"          # C0 seed 50
        out[f"seg075/M3R.C2.{f}.npz"] = t4 / f"M3R.C2.{f}.npz"                  # C2 seed 49
        out[f"seg072/M1.{f}.npz"] = s72 / f"M1.{f}.npz"                         # M1 C0
        out[f"seg075/M1.C2.{f}.npz"] = t4 / f"M1.C2.{f}.npz"                    # M1 C2
        out[f"seg076/A.C0.{f}.npz"] = T001_OUT / "scores" / f"A.C0.{f}.npz"      # A C0 (T001)
    return out


def jobs() -> list[dict]:
    out = []
    for cond in ("C0", "C2"):
        for seed in NEW_SEEDS[cond]:
            for fold in rx.GENESIS:
                out.append({"id": f"M3R.{cond}.{fold}.s{seed}", "model": "M3R", "condition": cond, "fold": fold, "seed": seed, "dir": "."})
    for fold in rx.GENESIS:
        out.append({"id": f"A.C2.{fold}", "model": "A", "condition": "C2", "fold": fold, "seed": None, "dir": "."})
    for c, f, sd in DETERMINISM:
        out.append({"id": f"M3R.{c}.{f}.s{sd}.repro", "model": "M3R", "condition": c, "fold": f, "seed": sd, "dir": "repro"})
    return out


def build_plan() -> dict:
    rx.verify_plan()
    d = rx.design()
    return {"schema": "segarecomp.xplat_stability.plan.v1", "task": "SEG-076-T003", "status": "exploratory, development-only",
            "bound_code": {n: sha(HERE / n) for n in bound_files()}, "seg075_plan_sha256": sha(d.PLAN),
            "t001_plan_sha256": rx.plan_sha(), "corpus_files": rx.corpus_manifest(),
            "reused_scores": {k: sha(v) for k, v in reused_scores().items()},
            "conditions": {"C0": "Genesis-only training", "C2": "Genesis + toslibc, emutos, BattleIsleEditor, amiga-realtime3d (the "
                           "SEG-075 C2 list; cross-platform representatives only, as in T004)"},
            "seeds": {"M3R": list(SEEDS), "reused": {"C0": [49, 50], "C2": [49]}, "new": {k: list(v) for k, v in NEW_SEEDS.items()},
                      "why_four": "Step-1 diagnosis: the seed variation is concentrated in D01 (other lineages move <= 0.04 between seeds "
                                  "49 and 50). Four seeds per condition give 6 pairwise differences for the noise estimate and 4 paired "
                                  "transfer differences, within a ~7-8 h CPU budget; a larger count was not affordable in this delivery "
                                  "and the rule's EVIDENCE_INSUFFICIENT branch covers an under-powered outcome"},
            "a_c2": "A (T001's unchanged structural HGB) refitted under C2 on the 11 Genesis folds; its C0 fits are T001's",
            "determinism": "M3R C0/D01/s50 and C2/D01/s49 refitted once into repro/ and compared bitwise with the SEG-075 T004 files",
            "jobs": [j["id"] for j in jobs()], "rules": RULES, "resources": RESOURCES,
            "metrics": "segarecomp_xplat_explore_eval (SEG-075 image_metrics + boundary strata), 128-byte cells; Genesis mean ER = mean over "
                       "lineages of the lineage max ER (SEG-075); mean image ER reported as a secondary, less tail-sensitive statistic",
            "outputs": {"private": "<HARNESS_ROOT>/.cache/seg076/t003", "public": "tools/segarecomp_xplat_stability.results.json"}}


def write_plan() -> dict:
    doc = build_plan()
    PLAN.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    DIGEST.write_text(hashlib.sha256(rx.canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan() -> dict:
    rx.verify_plan()
    if not (PLAN.is_file() and DIGEST.is_file()):
        raise SystemExit("T003 plan missing: no fit before the reviewed, committed plan")
    doc = json.loads(PLAN.read_text())
    if hashlib.sha256(rx.canonical(doc)).hexdigest() != DIGEST.read_text().strip():
        raise SystemExit("T003 plan digest mismatch")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the code bound by the T003 plan")
    if doc["t001_plan_sha256"] != rx.plan_sha():
        raise SystemExit("T003 plan bound to a different T001 plan")
    files = reused_scores()
    for k, want in doc["reused_scores"].items():
        if sha(files[k]) != want:
            raise SystemExit(f"reused score {k} differs from its bound digest")
    if rx.corpus_manifest() != doc["corpus_files"]:
        raise SystemExit("corpus differs from the T003 plan")
    return doc


def plan_sha() -> str:
    return DIGEST.read_text().strip()


# ---------------------------------------------------------------------------------------------------------------------------- fit
def score_path(out: pathlib.Path, model: str, cond: str, fold: str, seed) -> pathlib.Path:
    return out / "scores" / (f"{model}.{cond}.{fold}" + (f".s{seed}" if seed is not None else "") + ".npz")


def run_fit(model: str, cond: str, fold: str, seed, out: pathlib.Path) -> dict:
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    verify_plan()
    rx.guard([out])
    st = (out.parent if out.name == "repro" else out) / "farm.json"
    if not st.is_file() or json.loads(st.read_text()).get("plan_sha256") != plan_sha():
        raise SystemExit(f"{out}: not a T003 out dir")
    job = next((j for j in jobs() if (j["model"], j["condition"], j["fold"], j["seed"]) == (model, cond, fold, seed)
                and (out.name == "repro") == (j["dir"] == "repro")), None)
    if job is None:
        raise SystemExit("not a pre-registered T003 fit")
    path = score_path(out, model, cond, fold, seed)
    if path.exists() or path.with_suffix(".json").exists():
        raise SystemExit(f"{path.name}: already exists; fits are write-once")
    train_dirs, held_dirs, held = rx.xt().fold_sets(rx.design().verify_plan(), cond, fold, rx.snapshot_dir())
    train, held_imgs = rx.load_dirs(train_dirs), rx.load_dirs(held_dirs)
    if any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    if cond == "C0" and any(im.id.startswith("xp/") for im in train):
        raise SystemExit("C0 training must be Genesis-only")
    w0, c0 = time.time(), time.process_time()
    log = (lambda m: print(f"[{model}/{cond}/{fold}/s{seed}] {m}", flush=True))
    if model == "M3R":
        import segarecomp_ml_region_gen2_train as g2
        saved = g2.RECIPE
        try:
            g2.RECIPE = dict(saved, seed=seed)
            net = t.train_m3r(train, log)
        finally:
            g2.RECIPE = saved
        scores = {im.id: t.score_m3(net, im) for im in held_imgs}
    else:
        net = xm.fit_a(train)
        scores = {im.id: xm.score_a(net, im) for im in held_imgs}
    fit_wall, fit_cpu = time.time() - w0, time.process_time() - c0
    path.parent.mkdir(parents=True, exist_ok=True)
    t.save_scores(path, scores)
    commit = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    rec = {"task": "SEG-076-T003", "model": model, "condition": cond, "fold": fold, "seed": seed, "plan_sha256": plan_sha(),
           "product_commit": commit, "fit_wall_seconds": round(fit_wall, 1), "fit_cpu_seconds": round(fit_cpu, 1),
           "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024, "train_images": sorted(im.id for im in train),
           "held_images": sorted(im.id for im in held_imgs), "train_lineages": sorted({im.lineage for im in train}),
           "environment": rx.environment()}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "condition", "fold", "seed", "fit_wall_seconds")}), flush=True)
    return rec


def cmd_farm(a) -> int:
    verify_plan()
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    (out / "logs").mkdir(parents=True, exist_ok=True)
    state_p = out / "farm.json"
    state = json.loads(state_p.read_text()) if state_p.is_file() else {"plan_sha256": plan_sha(), "jobs": {}}
    if state.get("plan_sha256") != plan_sha():
        raise SystemExit("farm state belongs to a different T003 plan")

    def save():
        tmp = state_p.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=1, sort_keys=True))
        os.replace(tmp, state_p)

    for v in state["jobs"].values():
        if v["status"] == "started":
            v["status"] = "interrupted"
    save()
    by_id = {j["id"]: j for j in jobs()}
    todo = [j for j in jobs() if j["id"] not in state["jobs"]]
    for jid in dict.fromkeys(a.infra_rerun or []):
        prev = state["jobs"].get(jid)
        if not prev or prev.get("attempts", 1) >= 2 or not prev["status"].startswith(("interrupted", "killed_signal")) or not a.reason:
            raise SystemExit(f"{jid}: infra re-run refused")
        prev["infra_rerun_reason"] = a.reason
        todo.append(by_id[jid])
    running, stall, next_start = {}, None, 0.0
    while todo or running:
        avail = rx._available_mb()
        if running and avail < RESOURCES["host_memory_floor_mb"]:
            jid = max(running, key=lambda k: running[k][1])
            p, t0, j, log, peak = running.pop(jid)
            p.kill(), p.wait(), log.close()
            state["jobs"][jid].update(status="host_memory_guard", wall_seconds=round(time.time() - t0, 1), mem_available_mb=avail)
            save()
            print(f"done {jid} host_memory_guard", flush=True)
            continue
        if todo and not running and avail < RESOURCES["min_available_mb_to_start"]:
            stall = stall or time.time()
            if time.time() - stall > 1800:
                save()
                raise SystemExit("farm stalled: available memory below the start threshold for 30 min with nothing running")
        else:
            stall = None
        if todo and len(running) < RESOURCES["parallel_fits"] and avail >= RESOURCES["min_available_mb_to_start"] \
                and time.time() >= next_start:
            next_start = time.time() + RESOURCES["start_stagger_seconds"]
            j = todo.pop(0)
            attempt = state["jobs"].get(j["id"], {}).get("attempts", 0) + 1
            state["jobs"][j["id"]] = dict(state["jobs"].get(j["id"], {}), status="started", attempts=attempt)
            save()
            threads = RESOURCES["m3r_threads"] if j["model"] == "M3R" else RESOURCES["a_threads"]
            env = dict(os.environ, OMP_NUM_THREADS=str(threads), MKL_NUM_THREADS=str(threads))
            cmd = [sys.executable, "-I", str(HERE / "segarecomp_xplat_stability.py"), "fit", "--model", j["model"], "--condition",
                   j["condition"], "--fold", j["fold"], "--out", str(out / j["dir"])] + (["--seed", str(j["seed"])] if j["seed"] else [])
            log = open(out / "logs" / f"{j['id']}.a{attempt}.log", "w")
            running[j["id"]] = (subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env), time.time(), j, log, 0)
            print(f"start {j['id']} attempt {attempt}", flush=True)
        time.sleep(5)
        for jid, (p, t0, j, log, peak) in list(running.items()):
            rss = rx._rss_mb(p.pid)
            peak = max(peak, rss)
            running[jid] = (p, t0, j, log, peak)
            status = None
            if p.poll() is not None:
                status = "ok" if p.returncode == 0 else (f"killed_signal{-p.returncode}" if p.returncode < 0 else f"error_rc{p.returncode}")
            elif time.time() - t0 > RESOURCES["wall_clock_cap_minutes"][j["model"]] * 60:
                p.kill(), p.wait()
                status = "timeout"
            elif rss > RESOURCES["rss_limit_mb"]:
                p.kill(), p.wait()
                status = "rss_limit"
            if status:
                log.close()
                state["jobs"][jid].update(status=status, wall_seconds=round(time.time() - t0, 1), peak_rss_mb_watchdog=peak)
                save()
                del running[jid]
                print(f"done {jid} {status} {round(time.time() - t0)}s", flush=True)
    save()
    print(json.dumps({"jobs": len(state["jobs"]), "failed": {k: v["status"] for k, v in state["jobs"].items() if v["status"] != "ok"}}))
    return 0


# ----------------------------------------------------------------------------------------------------------------------- statistics
def t_interval(xs: list[float], t_crit: float = 3.182) -> dict:
    n = len(xs)
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1)) if n > 1 else float("inf")
    h = t_crit * sd / math.sqrt(n)
    return {"mean": round(m, 6), "sd": round(sd, 6), "lo": round(m - h, 6), "hi": round(m + h, 6), "half_width": round(h, 6), "n": n}


def sign_flip_p(ds: list[float]) -> float:
    """Exact two-sided sign-flip permutation p of |mean| over all 2^n patterns (pure; unit-tested)."""
    obs = abs(sum(ds))
    n, hit = len(ds), 0
    for signs in itertools.product((1, -1), repeat=n):
        if abs(sum(s * d for s, d in zip(signs, ds))) >= obs - 1e-12:
            hit += 1
    return hit / 2 ** n


def bootstrap_ci(ds: list[float], reps: int = 10000, seed: int = 0) -> dict:
    import numpy as np
    rng = np.random.default_rng(seed)
    arr = np.asarray(ds, dtype=float)
    means = arr[rng.integers(0, len(arr), size=(reps, len(arr)))].mean(axis=1)
    return {"lo": round(float(np.quantile(means, 0.025)), 6), "hi": round(float(np.quantile(means, 0.975)), 6), "reps": reps}


def pop_sd(xs: list[float]) -> float:
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / len(xs))


def transfer_class(d_seed: list[float], d_lineage: list[float]) -> dict:
    ci = t_interval(d_seed)
    p = sign_flip_p(d_lineage)
    excl = (ci["lo"] > 0 or ci["hi"] < 0) and abs(ci["mean"]) >= 0.01
    if excl and p <= 0.05:
        cls = "TRANSFER_EFFECT_MEASURABLE"
    elif excl:
        cls = "EFFECT_LINEAGE_CONCENTRATED"
    elif ci["lo"] >= -0.02 and ci["hi"] <= 0.02:
        cls = "NO_EFFECT_DETECTED_BOUNDED"
    else:
        cls = "EVIDENCE_INSUFFICIENT"
    direction = "helps" if ci["mean"] < 0 else "harms"
    return {"class": cls, "direction": direction, "t_interval": ci, "sign_flip_p": round(p, 6),
            "lineages_with_effect_sign": sum(1 for x in d_lineage if (x < 0) == (ci["mean"] < 0) and x != 0)}


def aggregate_er(image_er: dict, lineage_of: dict) -> dict:
    """{'mean_er': Genesis mean of lineage max image ER, 'd01': D01 max} over informative images (None ERs skipped) (pure)."""
    lin = {}
    for iid, e in image_er.items():
        if e is not None:
            L = lineage_of[iid]
            lin[L] = max(lin.get(L, e), e)
    return {"mean_er": sum(lin.values()) / len(lin), "d01": lin.get("D01")}


def gain_terms(member_image_ers: list, ensemble_image_er: dict, lineage_of: dict) -> dict:
    """g_score = aggregate(ensemble) - aggregate(image-wise member mean) and g_jensen = aggregate(image-wise member mean) - mean of the
    members' aggregates, for the Genesis mean ER and D01 max ER (pure; unit-tested). Pure averaging of member image ERs gives g_score 0."""
    ids = sorted(member_image_ers[0])
    if any(sorted(m) != ids for m in member_image_ers) or sorted(ensemble_image_er) != ids:
        raise SystemExit("gain_terms: image sets differ")
    mean_img = {}
    for iid in ids:
        v = [m[iid] for m in member_image_ers]
        mean_img[iid] = None if any(x is None for x in v) else sum(v) / len(v)
    ref, ens = aggregate_er(mean_img, lineage_of), aggregate_er(ensemble_image_er, lineage_of)
    mem = [aggregate_er(m, lineage_of) for m in member_image_ers]
    out = {}
    for k in ("mean_er", "d01"):
        out[f"g_score_{k}"] = ens[k] - ref[k]
        out[f"g_jensen_{k}"] = ref[k] - sum(x[k] for x in mem) / len(mem)
    return out


def stability_class(gains: dict) -> dict:
    """gains: {cond: {"pair_d01": [6 D01 max-ER gains], "pair_mean_er": [6 Genesis mean-ER gains], "ens4_mean_er": gain}} (pure; tested)."""
    rows, ok = {}, bool(gains)
    for cond, g in gains.items():
        neg = sum(1 for x in g["pair_d01"] if x < 0)
        mean_pair = sum(g["pair_mean_er"]) / len(g["pair_mean_er"])
        c_ok = neg >= 5 and mean_pair <= -0.005 and g["ens4_mean_er"] <= 0
        rows[cond] = {"pair_d01_negative_gains": neg, "pairs": len(g["pair_d01"]), "mean_pair_gain_mean_er": round(mean_pair, 6),
                      "ens4_gain_mean_er": round(g["ens4_mean_er"], 6), "condition_ok": c_ok}
        ok &= c_ok
    return {"class": "SEED_STABILITY_IMPROVED" if ok else "NO_MEANINGFUL_ADVANTAGE", "conditions": rows}


# ---------------------------------------------------------------------------------------------------------------------- shared io
def genesis_meta() -> dict:
    X = rx.xt()
    meta = {}
    for d in X.genesis_dirs(rx.GENESIS):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": X.genesis_seed_addresses(d)}
    rx.guard([v["dir"] for v in meta.values()])
    return meta


def _f64(x: dict) -> dict:
    import numpy as np
    return {k: np.asarray(v, dtype=np.float64) for k, v in x.items()}


def m3r_scores(cond: str, seed: int, fold: str, out: pathlib.Path, status: dict):
    X, files = rx.xt(), reused_scores()
    if cond == "C0" and seed == 49:
        return _f64(X.load_npz(files[f"seg072/M3R.{fold}.npz"]))
    if cond == "C0" and seed == 50:
        return _f64(X.load_npz(files[f"seg075/M3R.C0.{fold}.s50.npz"]))
    if cond == "C2" and seed == 49:
        return _f64(X.load_npz(files[f"seg075/M3R.C2.{fold}.npz"]))
    jid = f"M3R.{cond}.{fold}.s{seed}"
    if status.get(jid) != "ok":
        return None
    p = score_path(out, "M3R", cond, fold, seed)
    r = json.loads(p.with_suffix(".json").read_text())
    if r.get("plan_sha256") != plan_sha() or (r["model"], r["condition"], r["fold"], r["seed"]) != ("M3R", cond, fold, seed):
        raise SystemExit(f"{p.name}: record does not match")
    return _f64(X.load_npz(p))


def lineage_table(per_fold_scores: dict, meta: dict, classes: dict) -> tuple[dict, dict]:
    """per_fold_scores: {fold: {iid: cell scores}} (all 11 folds required) -> (lineage rows, summary)."""
    per = {}
    for fold, sc in per_fold_scores.items():
        want = sorted(k for k, v in meta.items() if v["lineage"] == fold)
        if sorted(sc) != want:
            raise SystemExit(f"{fold}: scored image set differs from the bound corpus")
        for iid, s in sc.items():
            if iid not in classes:
                classes[iid] = (meta[iid]["dir"] / "classes.bin").read_bytes()
            per[iid] = ev6.image_metrics_ext(classes[iid], meta[iid]["seeds"], s, 128)
    rows = ev6.lineage_summary(per, {k: v["lineage"] for k, v in meta.items()})
    return rows, ev6.summary(rows), per


# ----------------------------------------------------------------------------------------------------------------------- diagnose
def parse_losses(text: str, tag: str) -> list[float]:
    return [float(m.group(1)) for m in re.finditer(rf"\[{re.escape(tag)}\] epoch \d+/30 loss ([0-9.]+)", text)]


def loss_summary(xs: list[float]) -> dict:
    if not xs:
        return {"epochs": 0}
    spikes = sum(1 for a, b in zip(xs, xs[1:]) if b > a * 1.2)
    return {"epochs": len(xs), "final": round(xs[-1], 5), "min": round(min(xs), 5), "epoch5": round(xs[min(4, len(xs) - 1)], 5),
            "spikes_gt20pct": spikes}


def cmd_diagnose(a) -> int:
    """Tier-1 reanalysis of the preserved seed-49 / seed-50 M3-R C0 fits (no new fit). Aggregates only."""
    import numpy as np
    import segarecomp_ml_v3 as v3
    import segarecomp_ml_v3_train as t
    rx.verify_plan()
    meta, classes = genesis_meta(), {}
    s49 = {f: m3r_scores("C0", 49, f, pathlib.Path("/nonexistent"), {}) for f in rx.GENESIS}
    s50 = {f: m3r_scores("C0", 50, f, pathlib.Path("/nonexistent"), {}) for f in rx.GENESIS}
    r49, sum49, p49 = lineage_table(s49, meta, classes)
    r50, sum50, p50 = lineage_table(s50, meta, classes)
    seg072_log = SEG072_LOG.read_text(errors="replace")
    lineages = {}
    for L in rx.GENESIS:
        ids = sorted((k for k, v in meta.items() if v["lineage"] == L), key=lambda k: (meta[k]["size"], k))
        imgs = []
        for i, iid in enumerate(ids):
            a_, b_ = s49[L][iid], s50[L][iid]
            rho = float(np.corrcoef(np.argsort(np.argsort(-a_)), np.argsort(np.argsort(-b_)))[0, 1]) if len(a_) > 2 else None
            labels = v3.cell_labels(classes[iid], 128)
            st = ev6.boundary_strata(classes[iid], labels, 128)
            pos = [c for c, y in enumerate(labels) if y == 1]

            def weakest(s):
                if not pos:
                    return None
                c = min(pos, key=lambda c: (s[c], c))
                return sorted(k for k in ("d_byte", "adjacent_d_only", "short_run") if c in st[k])
            m49, m50 = p49[iid], p50[iid]
            imgs.append({"index": i, "cells": m49["cells"], "positive_cells": m49["positive_cells"], "informative": m49["informative"],
                         "er_s49": None if m49["er"] is None else round(m49["er"], 6), "er_s50": None if m50["er"] is None else round(m50["er"], 6),
                         "b50_misses_s49": m49["misses_B50"], "b50_misses_s50": m50["misses_B50"],
                         "cell_rank_spearman": None if rho is None else round(rho, 4),
                         "weakest_positive_strata_s49": weakest(a_), "weakest_positive_strata_s50": weakest(b_)})
        tr_ids = [k for k, v in meta.items() if v["lineage"] != L]
        n_pos = sum(sum(1 for y in v3.cell_labels(classes[k], 128) if y == 1) for k in tr_ids)
        n_neg = sum(sum(1 for y in v3.cell_labels(classes[k], 128) if y == 0) for k in tr_ids)
        log50 = (T004_LOGS / f"M3R_C0_{L}_50.log")
        lineages[L] = {"images": len(ids), "max_er": {"s49": r49[L]["max_er"], "s50": r50[L]["max_er"]},
                       "mean_image_er": {"s49": r49[L]["mean_image_er"], "s50": r50[L]["mean_image_er"]},
                       "b50_misses": {"s49": r49[L]["misses_B50"], "s50": r50[L]["misses_B50"]},
                       "max_er_image_index": {s: max((x for x in imgs if x[f"er_{s}"] is not None), key=lambda x: x[f"er_{s}"])["index"]
                                              if any(x[f"er_{s}"] is not None for x in imgs) else None for s in ("s49", "s50")},
                       "training_pos_weight": round(n_neg / n_pos, 4) if n_pos else None,
                       "loss_s49": loss_summary(parse_losses(seg072_log, f"M3R/{L}")),
                       "loss_s50": loss_summary(parse_losses(log50.read_text(errors="replace"), f"M3R/C0/{L}")) if log50.is_file() else None,
                       "images_detail": imgs if L == "D01" else None}
    d01 = [x for x in lineages["D01"]["images_detail"] if x["informative"]]
    deltas = [x["er_s50"] - x["er_s49"] for x in d01]
    doc = {"schema": "segarecomp.xplat_stability.diagnosis.v1", "task": "SEG-076-T003", "tier": 1,
           "summary": {"genesis_mean_er": {"s49": sum49["mean_er"], "s50": sum50["mean_er"]},
                       "genesis_mean_of_mean_image_er": {"s49": sum49["mean_of_mean_image_er"], "s50": sum50["mean_of_mean_image_er"]},
                       "per_lineage_abs_d_max_er": {L: round(abs(lineages[L]["max_er"]["s50"] - lineages[L]["max_er"]["s49"]), 6) for L in rx.GENESIS},
                       "d01_informative_images": len(d01), "d01_images_worse_by_gt_0.05": sum(1 for x in deltas if x > 0.05),
                       "d01_images_better_by_gt_0.05": sum(1 for x in deltas if x < -0.05),
                       "d01_mean_image_d_er": round(sum(deltas) / len(deltas), 6),
                       "d01_cell_rank_spearman_median": (lambda v: v[len(v) // 2])(sorted(x["cell_rank_spearman"] for x in
                                                          lineages["D01"]["images_detail"] if x["cell_rank_spearman"] is not None))},
           "lineages": lineages,
           "data_boundary": "aggregates only; D01 images are pseudonymous size-ordered indices with cell counts, no names or addresses"}
    target = pathlib.Path(a.results) if a.results else DIAGNOSIS
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps(doc["summary"], indent=1))
    return 0


# -------------------------------------------------------------------------------------------------------------------------- table
def cmd_table(a) -> int:
    import numpy as np
    plan = verify_plan()
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    farm = json.loads((out / "farm.json").read_text())
    if farm.get("plan_sha256") != plan_sha():
        raise SystemExit("farm state belongs to a different plan")
    status = {k: v["status"] for k, v in farm["jobs"].items()}
    meta, classes = genesis_meta(), {}
    X, files = rx.xt(), reused_scores()
    runs, missing = {}, {}
    for cond in ("C0", "C2"):
        for seed in SEEDS:
            sc = {f: m3r_scores(cond, seed, f, out, status) for f in rx.GENESIS}
            miss = [f for f, v in sc.items() if v is None]
            if miss:
                missing[f"M3R.{cond}.s{seed}"] = miss
                continue
            runs[(cond, seed)] = sc
    tables = {}

    def add(name, sc):
        rows, s, per = lineage_table(sc, meta, classes)
        tables[name] = {"genesis": rows, "summary": s, "image_er": {iid: m["er"] for iid, m in per.items()}}
        return tables[name]



    for (cond, seed), sc in runs.items():
        add(f"M3R.{cond}.s{seed}", sc)

    def ens(cond, seeds):
        return {f: {iid: np.mean([runs[(cond, s)][f][iid] for s in seeds], axis=0) for iid in runs[(cond, seeds[0])][f]} for f in rx.GENESIS}

    complete = {cond: all((cond, s) in runs for s in SEEDS) for cond in ("C0", "C2")}
    pair_names = {}
    for cond in ("C0", "C2"):
        if not complete[cond]:
            continue
        for pr in itertools.combinations(SEEDS, 2):
            pair_names.setdefault(cond, []).append(f"M3R.{cond}.ens{pr[0]}+{pr[1]}")
            add(pair_names[cond][-1], ens(cond, pr))
        add(f"M3R.{cond}.ens4", ens(cond, SEEDS))
    # A and M1
    a0 = {f: _f64(X.load_npz(files[f"seg076/A.C0.{f}.npz"])) for f in rx.GENESIS}
    add("A.C0", a0)
    a2 = {}
    for f in rx.GENESIS:
        if status.get(f"A.C2.{f}") == "ok":
            p = score_path(out, "A", "C2", f, None)
            r = json.loads(p.with_suffix(".json").read_text())
            if r.get("plan_sha256") != plan_sha() or (r["model"], r["condition"], r["fold"], r["seed"]) != ("A", "C2", f, None):
                raise SystemExit(f"{p.name}: record does not match")
            a2[f] = _f64(X.load_npz(p))
    if len(a2) == len(rx.GENESIS):
        add("A.C2", a2)
    else:
        missing["A.C2"] = [f for f in rx.GENESIS if f not in a2]
    add("M1.C0", {f: _f64(X.load_npz(files[f"seg072/M1.{f}.npz"])) for f in rx.GENESIS})
    add("M1.C2", {f: _f64(X.load_npz(files[f"seg075/M1.C2.{f}.npz"])) for f in rx.GENESIS})

    def per_lineage_d(t2, t0):
        g2, g0 = tables[t2]["genesis"], tables[t0]["genesis"]
        return {L: g2[L]["max_er"] - g0[L]["max_er"] for L in rx.GENESIS if g2[L]["max_er"] is not None and g0[L]["max_er"] is not None}

    decisions = {}
    noise = {}
    for cond in ("C0", "C2"):
        if not complete[cond]:
            continue
        diffs = {L: [abs(tables[f"M3R.{cond}.s{x}"]["genesis"][L]["max_er"] - tables[f"M3R.{cond}.s{y}"]["genesis"][L]["max_er"])
                     for x, y in itertools.combinations(SEEDS, 2)] for L in rx.GENESIS}
        noise[cond] = {"tau4": round(2 * sum(sum(v) / len(v) for v in diffs.values()) / len(diffs), 6),
                       "per_lineage_mean_abs_pair_d": {L: round(sum(v) / len(v), 6) for L, v in diffs.items()},
                       "per_lineage_sd_max_er": {L: round(pop_sd([tables[f"M3R.{cond}.s{s}"]["genesis"][L]["max_er"] for s in SEEDS]), 6)
                                                 for L in rx.GENESIS}}
    if all(complete.values()):
        d_seed = [tables[f"M3R.C2.s{s}"]["summary"]["mean_er"] - tables[f"M3R.C0.s{s}"]["summary"]["mean_er"] for s in SEEDS]
        per_seed_L = [per_lineage_d(f"M3R.C2.s{s}", f"M3R.C0.s{s}") for s in SEEDS]
        d_L = {L: sum(d[L] for d in per_seed_L) / len(SEEDS) for L in rx.GENESIS}
        decisions["m3r_transfer"] = dict(transfer_class(d_seed, list(d_L.values())), per_seed_d=[round(x, 6) for x in d_seed],
                                         per_lineage_seed_mean_d={L: round(v, 6) for L, v in d_L.items()}, tier=2)
        d_seed_mi = [tables[f"M3R.C2.s{s}"]["summary"]["mean_of_mean_image_er"] - tables[f"M3R.C0.s{s}"]["summary"]["mean_of_mean_image_er"]
                     for s in SEEDS]
        decisions["m3r_transfer_secondary_mean_image_er"] = dict(t_interval(d_seed_mi), tier=2, note="secondary statistic, not the rule")
        decisions["m3r_transfer_ens4"] = {"d_mean_er": round(tables["M3R.C2.ens4"]["summary"]["mean_er"] - tables["M3R.C0.ens4"]["summary"]["mean_er"], 6),
                                          "per_lineage": {L: round(v, 6) for L, v in per_lineage_d("M3R.C2.ens4", "M3R.C0.ens4").items()},
                                          "tier": 2, "note": "descriptive: one 4-seed ensemble per condition"}
        gains, desc, jensen = {}, {}, {}
        lineage_of = {k: v["lineage"] for k, v in meta.items()}
        for c in ("C0", "C2"):
            def m(name, k):
                return tables[name]["summary"]["mean_er"] if k == "mean_er" else tables[name]["genesis"]["D01"]["max_er"]
            prs = list(itertools.combinations(SEEDS, 2))
            g = {"pair_d01": [], "pair_mean_er": []}
            jj = {"pair_d01": [], "pair_mean_er": []}
            for (x, y), name in zip(prs, pair_names[c]):
                gt = gain_terms([tables[f"M3R.{c}.s{x}"]["image_er"], tables[f"M3R.{c}.s{y}"]["image_er"]], tables[name]["image_er"], lineage_of)
                g["pair_d01"].append(gt["g_score_d01"])
                g["pair_mean_er"].append(gt["g_score_mean_er"])
                jj["pair_d01"].append(gt["g_jensen_d01"])
                jj["pair_mean_er"].append(gt["g_jensen_mean_er"])
            gt4 = gain_terms([tables[f"M3R.{c}.s{x}"]["image_er"] for x in SEEDS], tables[f"M3R.{c}.ens4"]["image_er"], lineage_of)
            g["ens4_mean_er"] = gt4["g_score_mean_er"]
            jj["ens4_mean_er"] = gt4["g_jensen_mean_er"]
            gains[c], jensen[c] = g, jj
            sd1 = {k: pop_sd([m(f"M3R.{c}.s{x}", k) for x in SEEDS]) for k in ("mean_er", "d01")}
            sd2 = {k: pop_sd([m(n, k) for n in pair_names[c]]) for k in ("mean_er", "d01")}
            desc[c] = {k: {"single_sd": round(sd1[k], 6), "pair_ensemble_sd": round(sd2[k], 6),
                           "ratio": round(sd2[k] / sd1[k], 6) if sd1[k] else None, "linear_null_ratio": round(1 / math.sqrt(3), 6)}
                       for k in sd1}
        decisions["m3r_seed_stability"] = dict(stability_class(gains), tier=2, descriptive_spread=desc,
                                               jensen_term={c: {k: ([round(x, 6) for x in v] if isinstance(v, list) else round(v, 6))
                                                                for k, v in jj.items()} for c, jj in jensen.items()},
                                               gains={c: {k: ([round(x, 6) for x in v] if isinstance(v, list) else round(v, 6))
                                                          for k, v in g.items()} for c, g in gains.items()})
    else:
        decisions["m3r_transfer"] = {"class": "EVIDENCE_INSUFFICIENT", "reason": "planned M3-R fits failed or missing", "missing": missing}
        decisions["m3r_seed_stability"] = {"class": "EVIDENCE_INSUFFICIENT", "reason": "planned M3-R fits failed or missing"}
    if "A.C2" in tables:
        dA = per_lineage_d("A.C2", "A.C0")
        d = tables["A.C2"]["summary"]["mean_er"] - tables["A.C0"]["summary"]["mean_er"]
        p = sign_flip_p(list(dA.values()))
        decisions["a_transfer"] = {"class": "TRANSFER_EFFECT_MEASURABLE" if abs(d) >= 0.01 and p <= 0.05 else "NO_MEANINGFUL_ADVANTAGE",
                                   "direction": "helps" if d < 0 else "harms", "d_mean_er": round(d, 6), "sign_flip_p": round(p, 6),
                                   "per_lineage_d": {L: round(v, 6) for L, v in dA.items()}, "tier": 2}
    else:
        decisions["a_transfer"] = {"class": "EVIDENCE_INSUFFICIENT", "reason": "A C2 fits failed or missing"}
    dM = per_lineage_d("M1.C2", "M1.C0")
    ci, p = bootstrap_ci(list(dM.values())), sign_flip_p(list(dM.values()))
    decisions["m1_descriptive_transfer"] = {"class": "DESCRIPTIVE_EFFECT_ROBUST" if (ci["lo"] > 0 or ci["hi"] < 0) and p <= 0.05
                                            else "DESCRIPTIVE_EFFECT_NOT_ROBUST",
                                            "d_mean_er": round(tables["M1.C2"]["summary"]["mean_er"] - tables["M1.C0"]["summary"]["mean_er"], 6),
                                            "bootstrap_ci": ci, "sign_flip_p": round(p, 6), "per_lineage_d": {L: round(v, 6) for L, v in dM.items()},
                                            "lineages_better": sum(1 for v in dM.values() if v < 0), "lineages_worse": sum(1 for v in dM.values() if v > 0),
                                            "tier": 1}
    determinism = {}
    for c, f, sd in DETERMINISM:
        jid = f"M3R.{c}.{f}.s{sd}.repro"
        ref = files[f"seg075/M3R.C0.{f}.s50.npz"] if c == "C0" else files[f"seg075/M3R.C2.{f}.npz"]
        if status.get(jid) == "ok":
            x1, x2 = X.load_npz(ref), X.load_npz(score_path(out / "repro", "M3R", c, f, sd))
            determinism[jid] = bool(sorted(x1) == sorted(x2) and all(np.array_equal(x1[k], x2[k]) for k in x1))
        else:
            determinism[jid] = None
    if not all(v is True for v in determinism.values()):
        for k in ("m3r_transfer", "m3r_seed_stability", "m3r_transfer_secondary_mean_image_er", "m3r_transfer_ens4"):
            decisions[k] = dict(decisions.get(k, {}), class_before_determinism_rule=decisions.get(k, {}).get("class"),
                                **{"class": "EVIDENCE_INSUFFICIENT", "determinism_rule": "a determinism refit was not bitwise identical "
                                   "or failed: mixed provenance of reused and new seeds"})
    costs = {}
    for model in ("M3R", "A"):
        recs = [json.loads(score_path(out, j["model"], j["condition"], j["fold"], j["seed"]).with_suffix(".json").read_text()) for j in jobs()
                if j["model"] == model and j["dir"] == "." and status.get(j["id"]) == "ok"]
        if recs:
            costs[model] = {"fits": len(recs), "fit_wall_seconds_mean": round(sum(r["fit_wall_seconds"] for r in recs) / len(recs), 1),
                            "fit_cpu_hours_total": round(sum(r["fit_cpu_seconds"] for r in recs) / 3600, 2)}
    compact = {k: {"summary": v["summary"], "lineage_max_er": {L: r["max_er"] for L, r in v["genesis"].items()},
                   "lineage_b50_misses": {L: r["misses_B50"] for L, r in v["genesis"].items()},
                   "d01": {kk: v["genesis"]["D01"][kk] for kk in ("max_er", "mean_image_er", "misses_B50", "informative_images")}}
               for k, v in tables.items()}
    doc = {"schema": "segarecomp.xplat_stability.results.v1", "task": "SEG-076-T003", "plan_sha256": plan_sha(),
           "status": "exploratory, development-only", "farm": dict(sorted(status.items())), "missing": missing, "noise": noise,
           "decisions": decisions, "determinism_bitwise": determinism, "costs": costs, "configurations": compact,
           "tiers": {"1": "reanalysis of existing development results", "2": "new training-seed experiment on existing development folds",
                     "3": "independent unseen-lineage evaluation: none"},
           "data_boundary": "aggregates only"}
    target = pathlib.Path(a.results) if a.results else RESULTS
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: v.get("class") for k, v in decisions.items()}, indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    dg = sub.add_parser("diagnose")
    dg.add_argument("--results")
    dg.set_defaults(func=cmd_diagnose)
    sub.add_parser("plan").set_defaults(func=lambda a: (print(json.dumps({"plan_sha256": hashlib.sha256(rx.canonical(write_plan())).hexdigest()})), 0)[1])
    sub.add_parser("check").set_defaults(func=lambda a: (verify_plan(), print(json.dumps({"plan_sha256": plan_sha(), "check": "PASS"})), 0)[2])
    sub.add_parser("jobs").set_defaults(func=lambda a: ([print(j["id"]) for j in jobs()], 0)[1])
    f = sub.add_parser("fit")
    f.add_argument("--model", required=True, choices=("M3R", "A"))
    f.add_argument("--condition", required=True, choices=("C0", "C2"))
    f.add_argument("--fold", required=True)
    f.add_argument("--seed", type=int)
    f.add_argument("--out", required=True)
    f.set_defaults(func=lambda a: (run_fit(a.model, a.condition, a.fold, a.seed, pathlib.Path(a.out).resolve()), 0)[1])
    fm = sub.add_parser("farm")
    fm.add_argument("--out", required=True)
    fm.add_argument("--infra-rerun", nargs="*")
    fm.add_argument("--reason")
    fm.set_defaults(func=cmd_farm)
    tb = sub.add_parser("table")
    tb.add_argument("--out", required=True)
    tb.add_argument("--results")
    tb.set_defaults(func=cmd_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
