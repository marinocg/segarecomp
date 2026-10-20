#!/usr/bin/env python3
"""SEG-076-T001 (exploratory, development-only): A/B/C pilot runner under a frozen, digest-bound SEG-076 plan (OFFLINE research code).

  plan    write the T001 plan (tools/segarecomp_xplat_explore.t001.plan.json + .digest): bound code digests (the UNCHANGED SEG-075 model,
          transfer, design and decision code, the SEG-072 recipes, this runner and its evaluation module), the SEG-075 plan / snapshot /
          handoff identities, every comparator score file digest, folds, seeds, configurations, metrics, the expansion rule and the
          resource policy. Committed and independently reviewed BEFORE the first fit
  check   verify the plan digest, the bound code, the SEG-075 plan and the comparator score digests
  jobs    the pre-registered fit list
  fit     one fit: --model A|B|C --fold <Genesis lineage>|all [--seed 1] (condition C0 only); refuses unless `check` passes; every
          record carries the plan digest, wall / CPU seconds, peak RSS, per-image inference seconds and the environment
  farm    run the pre-registered jobs with the frozen parallelism, per-model wall-clock caps and RSS watchdog; one attempt per job
          (an infrastructure re-run is explicit, identical and recorded); a fit that fails is recorded failed, never retuned
  table   aggregate-only results document (tools/segarecomp_xplat_explore.t001.results.json) with the pre-registered expansion class

SEG-075's frozen code is imported unchanged (its own verify_plan stays in force). The SEG-075 reserved cohort is refused by both the
SEG-075 barrier and SEG-076's fail-closed resolver (segarecomp_xplat_explore_eval.sealed_target).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import resource
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_explore_eval as ev6  # noqa: E402

HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
SEG075 = HARNESS / ".cache" / "seg075"
PLAN = HERE / "segarecomp_xplat_explore.t001.plan.json"
DIGEST = HERE / "segarecomp_xplat_explore.t001.plan.digest"
RESULTS = HERE / "segarecomp_xplat_explore.t001.results.json"
RECEIPT_C7 = HERE / "segarecomp_xplat_handoff.c7_receipt.json"
SCHEMA = "segarecomp.xplat_explore.t001.plan.v1"
BOUND_CODE = ("segarecomp_xplat_explore.py", "segarecomp_xplat_explore_eval.py", "segarecomp_xplat_models.py",
              "segarecomp_xplat_transfer.py", "segarecomp_xplat_design.py", "segarecomp_xplat_decide.py", "segarecomp_ml_v3.py",
              "segarecomp_ml_v3_train.py", "segarecomp_ml_v3_eval.py", "segarecomp_ml_region_gen2_train.py")
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
PILOT_SEED = 1
DETERMINISM_FOLD = "D05"
RESOURCES = {
    "backend": "CPU (research venv torch 2.14.1 CPU execution) for every T001 fit, the reproducibility baseline",
    "rocm_benchmark": {
        "environment": "torch 2.6.0+rocm6.4.2 (HIP 6.4.43484), AMD Radeon RX 9070 XT gfx1201 via WSL2 /dev/dxg; benchmark only, no scores",
        "method": "unchanged B/C network and M4 recipe, all-Genesis training set, seed 1, median step time after 5 warm-up steps; "
                  "B 60 steps x 3 repeats, C 40 steps x 2 repeats; parameter hash after the run compared across repeats",
        "result": {"B": {"cpu_s_per_step": 0.394, "rocm_s_per_step": 0.038, "reproducible": True},
                   "C": {"cpu_s_per_step": 0.807, "rocm_s_per_step": 0.54, "reproducible": True,
                         "note": "C's typed message passing builds per-edge index lists in Python, so the step stays CPU-bound"}},
        "rule": "SEG-072 hardware rule (ADR 0153): ROCm only if >= 25% end-to-end improvement over the parallel CPU farm AND reproducible, "
                "with every compared candidate on the same backend",
        "decision": "CPU. All B and C fits on one ROCm process: ~0.5 h + ~7.2 h, versus ~2.7 h on the 6-process CPU farm (worse). Splitting "
                    "B onto ROCm and C onto the CPU saves ~50 min but would put B on a different device and torch build than C, M4-Dense "
                    "and A, confounding the B-vs-C and B-vs-M4 contrasts the expansion rule depends on (CPU and ROCm outputs are not "
                    "bit-identical, ADR 0117). A later stage may pre-register its own ROCm decision.",
    },
    "threads": {"A": "sklearn default (OpenMP), the farm sets OMP_NUM_THREADS=2", "B": 2, "C": 2},
    "parallel_fits": 6,
    "wall_clock_cap_minutes": {"A": 30, "B": 90, "C": 160},
    "rss_limit_mb": 4096,
    "min_available_mb_to_start": 3000,
    "attempts": 1,
    "infrastructure_rerun": "at most one, only for an infrastructure cause (host / process killed externally, not a cap or a model error),"
                            " identical configuration and seed, recorded in the farm state with its reason",
    "failure": "a fit that exceeds its cap, its RSS limit or raises is recorded failed; a model with any failed planned Genesis fit is "
               "'missing or failed fits' in the expansion rule (no advantage); no retry with other hyperparameters, steps or seeds",
    "probe": "pre-freeze cost probe (40 training steps on the all-Genesis set, no scores): B 0.39 s/step (~26 min/fit at 4000 steps), "
             "C 0.81 s/step (~54 min/fit), A features 3.4 s for all Genesis images; peak RSS A 0.8 / B 1.0 / C 1.4 GB",
}
RULE = {"candidates": ["B", "C"], "comparators": ["M4_DENSE_s1@128", "M3_R_s49", "A"], "mean_er_margin": 0.02, "lineage_tolerance": 0.05}


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def canonical(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()


def xt():
    import segarecomp_xplat_transfer as m
    return m


def design():
    import segarecomp_xplat_design as m
    return m


def reserved() -> set[str]:
    return design().reserved_ids()


def guard(paths) -> None:
    ev6.refuse(paths, reserved(), design().SEALED_DIRNAME)


def snapshot_dir() -> pathlib.Path:
    return SEG075 / "t003" / "snapshot"


# ------------------------------------------------------------------------------------------------------------------- comparators
def comparator_files() -> dict[str, pathlib.Path]:
    """Every pre-existing score file the comparison reads (bound by digest in the plan; never refitted)."""
    s72, t4 = xt().SEG072_SCORES, SEG075 / "t004" / "scores"
    out = {}
    for f in GENESIS:
        out[f"seg072/M1.{f}.npz"] = s72 / f"M1.{f}.npz"
        out[f"seg072/M3R.{f}.npz"] = s72 / f"M3R.{f}.npz"
        for s in (1, 2, 3):
            out[f"seg072/M4.{f}.s{s}.npz"] = s72 / f"M4.{f}.s{s}.npz"
        out[f"seg075/M3R.C0.{f}.s50.npz"] = t4 / f"M3R.C0.{f}.s50.npz"
    out["seg072/M3_FROZEN.production.npz"] = s72 / "M3_FROZEN.production.npz"
    out["seg075/M3R.C0.all.xplat.npz"] = t4 / "M3R.C0.all.xplat.npz"
    out["seg075/M1.C0.all.xplat.npz"] = t4 / "M1.C0.all.xplat.npz"
    for s in (1, 2, 3):
        out[f"seg075/M4.C0.all.s{s}.xplat.npz"] = t4 / f"M4.C0.all.s{s}.xplat.npz"
    return out


# --------------------------------------------------------------------------------------------------------------------------- plan
def build_plan() -> dict:
    import segarecomp_xplat_models as xm
    d = design()
    p075 = d.verify_plan()
    receipt = json.loads(RECEIPT_C7.read_text())
    return {
        "schema": SCHEMA, "task": "SEG-076-T001", "milestone": "SEG-076", "status": "exploratory, development-only",
        "relation_to_seg075": "new experiment by explicit operator direction (2026-10-20); SEG-075-T005's gate does not apply; SEG-075's "
                              "decision (DATA_INSUFFICIENT, ADR 0163), plan, results and sealed cohort are unchanged and not reinterpreted",
        "bound_code": {n: sha(HERE / n) for n in BOUND_CODE},
        "seg075": {"plan_sha256": sha(d.PLAN), "snapshot_sha256": p075["corpus"]["snapshot_sha256"],
                   "handoff_manifest_sha256": receipt["manifest_sha256"], "handoff_label": receipt["label"],
                   "c7_receipt_sha256": sha(RECEIPT_C7)},
        "comparator_scores": {k: sha(v) for k, v in comparator_files().items()},
        "condition": {"id": "C0", "name": "Genesis-only training", "why": "matched to the preserved, bit-reproduced SEG-072 C0 fold scores "
                      "of M3-R (seed 49), M4-Dense (seeds 1-3) and M1-HGB; C2 is not run in T001"},
        "folds": {"genesis_lolo": list(GENESIS), "all": "one fit on all 11 Genesis lineages per model: preserves the weights and scores "
                  "the four cross-platform development representatives (toslibc, emutos, BattleIsleEditor, amiga-realtime3d) as "
                  "evaluation only (under C0 no cross-platform data trains, so xplat_lolo fits would equal this fit)"},
        "models": {"A": {"config": xm.A_CONFIG, "features": "segarecomp_xplat_models.a_features unchanged (210 columns)",
                         "seed": "fixed (random_state 49; deterministic)"},
                   "B": {"architecture": xm.B_ARCH, "training": "segarecomp_ml_v3_train.M4 recipe unchanged", "seed": PILOT_SEED},
                   "C": {"architecture": dict(xm.B_ARCH, message_passing="2 rounds per layer, 3 typed candidate edge sets, PROVEN flag"),
                         "training": "segarecomp_ml_v3_train.M4 recipe unchanged", "seed": PILOT_SEED},
                   "order": "A first (all folds), then B and C on identical folds; B is not required to beat A before C is trained; "
                            "no result of A cancels B or C"},
        "comparators": {"genesis": {"M1_HGB": "SEG-072 C0 fold scores", "M3_R_s49": "SEG-072 C0 fold scores (recipe seed 49)",
                                    "M3_R_s50": "SEG-075 T004 C0 seed-50 replicate", "M4_DENSE_s1@128": "SEG-072 M4 seed 1 alone, cell max "
                                    "at 128 bytes (the single-seed comparator matched to the pilots)", "M4_DENSE_ens@128": "SEG-072 M4 "
                                    "seeds 1-3 word-logit mean, cell max at 128 (ensemble; reported separately, never used to judge B/C)",
                                    "M3_FROZEN": "production M3 stored scores (descriptive only)"},
                        "xplat": "SEG-075 T004 C0-all scores of M3-R s49, M1-HGB, M4-Dense s1 and the 3-seed ensemble; M3_FROZEN by "
                                 "inference with the production weights"},
        "metrics": {"cell_bytes": 128, "er": "SEG-075 per-image excess retention on informative images; per lineage max ER (primary) and "
                    "mean image ER", "genesis_mean_er": "mean over the 11 lineages of the lineage max ER (SEG-075 definition)",
                    "worst_er": "max lineage max ER", "frf": "retention needed for source-backed full recall (per image; lineage max/mean)",
                    "budgets": "B35/B50/B60 missed starts and missed positive cells; negative cells admitted at B50",
                    "boundary_strata": "positive cells holding any D byte; positive cells adjacent to a D-only cell; positive cells with <= 8 "
                                       "starts; their union ('boundary'); missed cells at B35/B50 and normalized rank quantiles q50/q90/q100",
                    "secondary": "AUROC (mean per lineage), cost (wall / CPU seconds per fit, per-image inference seconds, peak RSS)",
                    "determinism": f"A, B and C are refitted once on fold {DETERMINISM_FOLD} into a separate directory; scores must be "
                                   "bitwise identical (reported; a mismatch is disclosed, it does not change the class)",
                    "code": "segarecomp_xplat_explore_eval.image_metrics_ext / lineage_summary / summary (calls SEG-075's unchanged "
                            "image_metrics and lineage_rows)"},
        "expansion_rule": dict(RULE, text="A model in {B, C} qualifies iff, against the best one-seed comparator (lowest Genesis mean ER "
                               "among M4-Dense seed 1 @128, M3-R seed 49 and A; ties: fewer B50 misses, then that order): Genesis mean ER "
                               "lower by >= 0.02; no lineage max ER worse by more than 0.05; no more boundary-stratum missed cells at B50; "
                               "no more total B50 misses. Qualifying set {} -> NEGATIVE_STOP, {B} -> EXPAND_B, {C} -> EXPAND_C, {B,C} -> "
                               "EXPAND_B_AND_C. Applied once, never tuned. A model with any failed planned Genesis fit does not qualify.",
                               meaning="the class only decides whether SEG-076-T002's multi-seed compute is justified; a single seed "
                                       "cannot establish architectural superiority or inferiority in either direction"),
        "resources": RESOURCES,
        "barrier": {"seg075_reserved_cohort": "sealed, never read, fitted, scored or used for selection", "seg072_sealed_cohort":
                    "already exposed; not used and never presented as independent evidence", "resolver": "every loaded directory, --out "
                    "and --snapshot pass both SEG-075's refuse_sealed and SEG-076's fail-closed sealed_target (both slash conventions, "
                    "nonexistent paths, case-folded)"},
        "outputs": {"private": "<HARNESS_ROOT>/.cache/seg076/t001 (scores, weights, records, logs; ignored, never committed)",
                    "public": "tools/segarecomp_xplat_explore.t001.results.json (aggregates only)"},
    }


def write_plan() -> dict:
    doc = build_plan()
    PLAN.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    DIGEST.write_text(hashlib.sha256(canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan() -> dict:
    if not (PLAN.is_file() and DIGEST.is_file()):
        raise SystemExit("SEG-076 T001 plan missing: no fit before the reviewed, committed plan")
    doc = json.loads(PLAN.read_text())
    if hashlib.sha256(canonical(doc)).hexdigest() != DIGEST.read_text().strip():
        raise SystemExit("T001 plan digest mismatch")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the code bound by the T001 plan")
    d = design()
    d.verify_plan()
    if sha(d.PLAN) != doc["seg075"]["plan_sha256"]:
        raise SystemExit("SEG-075 plan differs from the one bound by the T001 plan")
    files = comparator_files()
    for k, want in doc["comparator_scores"].items():
        if sha(files[k]) != want:
            raise SystemExit(f"comparator score {k} differs from its bound digest")
    return doc


def plan_sha() -> str:
    return DIGEST.read_text().strip()


# ---------------------------------------------------------------------------------------------------------------------------- fit
def score_path(out: pathlib.Path, model: str, fold: str, seed) -> pathlib.Path:
    stem = f"{model}.C0.{fold}" + (f".s{seed}" if seed is not None else "") + (".xplat" if fold == "all" else "")
    return out / "scores" / (stem + ".npz")


def environment() -> dict:
    e = xt().environment()
    e.update({"cpu_count": os.cpu_count(), "os": platform.platform(), "omp_num_threads": os.environ.get("OMP_NUM_THREADS")})
    return e


def load_dirs(dirs):
    import segarecomp_xplat_t005 as x5
    guard(dirs)
    return [x5.attach(im, d) for im, d in zip(xt().load(dirs), dirs)]


def run_fit(model: str, fold: str, seed, out: pathlib.Path) -> dict:
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    verify_plan()
    snap = snapshot_dir()
    guard([out, snap])
    if model == "A" and seed is not None:
        raise SystemExit("A is deterministic (random_state 49): no --seed")
    if model in ("B", "C") and seed != PILOT_SEED:
        raise SystemExit(f"T001 pilots use seed {PILOT_SEED} only")
    if fold != "all" and fold not in GENESIS:
        raise SystemExit("T001 folds: a Genesis lineage or 'all'")
    train_dirs, held_dirs, held = xt().fold_sets(design().verify_plan(), "C0", fold, snap)
    train, held_imgs = load_dirs(train_dirs), load_dirs(held_dirs)
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    if any(im.id.startswith("xp/") for im in train):
        raise SystemExit("C0 training must be Genesis-only")
    w0, c0 = time.time(), time.process_time()
    log = (lambda m: print(f"[{model}/C0/{fold}] {m}", flush=True))
    if model == "A":
        net = xm.fit_a(train)
    else:
        net = xm.fit_bc(train, seed, model == "C", log)
    fit_wall, fit_cpu = time.time() - w0, time.process_time() - c0
    scores, infer = {}, {}
    for im in held_imgs:
        t1 = time.time()
        scores[im.id] = xm.score_a(net, im) if model == "A" else xm.infer_bc(net, im, model == "C")
        infer[im.id] = round(time.time() - t1, 3)
    path = score_path(out, model, fold, seed)
    path.parent.mkdir(parents=True, exist_ok=True)
    t.save_scores(path, scores)
    if fold == "all":
        if model == "A":
            import pickle
            path.with_suffix(".pkl").write_bytes(pickle.dumps(net))
        else:
            import torch
            torch.save(net.state_dict(), path.with_suffix(".pt"))
    rec = {"task": "SEG-076-T001", "model": model, "condition": "C0", "fold": fold, "seed": seed, "plan_sha256": plan_sha(),
           "fit_wall_seconds": round(fit_wall, 1), "fit_cpu_seconds": round(fit_cpu, 1),
           "inference_seconds": {"images": len(infer), "total": round(sum(infer.values()), 2), "max": max(infer.values(), default=0.0)},
           "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024,
           "train_images": sorted(im.id for im in train), "held_images": sorted(im.id for im in held_imgs),
           "train_lineages": sorted({im.lineage for im in train}), "environment": environment()}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "fold", "seed", "fit_wall_seconds", "peak_rss_mb")}), flush=True)
    return rec


def jobs() -> list[dict]:
    out = []
    for model in ("A", "B", "C"):
        seed = None if model == "A" else PILOT_SEED
        for fold in GENESIS + ("all",):
            out.append({"id": f"{model}.{fold}", "model": model, "fold": fold, "seed": seed, "dir": "."})
    for model in ("A", "B", "C"):
        out.append({"id": f"{model}.{DETERMINISM_FOLD}.repro", "model": model, "fold": DETERMINISM_FOLD,
                    "seed": None if model == "A" else PILOT_SEED, "dir": "repro"})
    return out


# --------------------------------------------------------------------------------------------------------------------------- farm
def _rss_mb(pid: int) -> int:
    try:
        for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 0


def _available_mb() -> int:
    try:
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 1 << 30


def cmd_farm(a) -> int:
    verify_plan()
    out = pathlib.Path(a.out).resolve()
    guard([out])
    (out / "logs").mkdir(parents=True, exist_ok=True)
    state_p = out / "farm.json"
    state = json.loads(state_p.read_text()) if state_p.is_file() else {"plan_sha256": plan_sha(), "jobs": {}}
    if state["plan_sha256"] != plan_sha():
        raise SystemExit("farm state belongs to a different plan")
    todo = [j for j in jobs() if (not a.only or j["id"] in a.only) and j["id"] not in state["jobs"]]
    for jid in a.infra_rerun or []:
        prev = state["jobs"].get(jid)
        if not prev or prev.get("attempts", 1) >= 2 or prev["status"] in ("ok",) or prev["status"] in ("timeout", "rss_limit"):
            raise SystemExit(f"{jid}: infrastructure re-run not permitted (one re-run, only after a non-cap, non-ok failure)")
        if not a.reason:
            raise SystemExit("--reason is required for an infrastructure re-run")
        prev["infra_rerun_reason"] = a.reason
        todo.append(next(j for j in jobs() if j["id"] == jid))
    caps = RESOURCES["wall_clock_cap_minutes"]
    running = {}

    def save():
        state_p.write_text(json.dumps(state, indent=1, sort_keys=True))

    def start(j):
        cmd = [sys.executable, "-I", str(HERE / "segarecomp_xplat_explore.py"), "fit", "--model", j["model"], "--fold", j["fold"],
               "--out", str(out / j["dir"])] + (["--seed", str(j["seed"])] if j["seed"] is not None else [])
        env = dict(os.environ, OMP_NUM_THREADS="2", MKL_NUM_THREADS="2")
        log = open(out / "logs" / f"{j['id']}.log", "w")
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        running[j["id"]] = (p, time.time(), j, log, 0)
        print(f"start {j['id']}", flush=True)

    while todo or running:
        while todo and len(running) < RESOURCES["parallel_fits"] and _available_mb() >= RESOURCES["min_available_mb_to_start"]:
            start(todo.pop(0))
        time.sleep(5)
        for jid, (p, t0, j, log, peak) in list(running.items()):
            rss = _rss_mb(p.pid)
            peak = max(peak, rss)
            running[jid] = (p, t0, j, log, peak)
            status = None
            if p.poll() is not None:
                status = "ok" if p.returncode == 0 else f"error_rc{p.returncode}"
            elif time.time() - t0 > caps[j["model"]] * 60:
                p.kill(), p.wait()
                status = "timeout"
            elif rss > RESOURCES["rss_limit_mb"]:
                p.kill(), p.wait()
                status = "rss_limit"
            if status:
                log.close()
                prev = state["jobs"].get(jid, {})
                state["jobs"][jid] = {"status": status, "wall_seconds": round(time.time() - t0, 1), "peak_rss_mb_watchdog": peak,
                                      "attempts": prev.get("attempts", 0) + 1, **({"infra_rerun_reason": prev["infra_rerun_reason"]}
                                                                                 if "infra_rerun_reason" in prev else {})}
                save()
                del running[jid]
                print(f"done {jid} {status} {round(time.time() - t0)}s", flush=True)
    save()
    bad = {k: v["status"] for k, v in state["jobs"].items() if v["status"] != "ok"}
    print(json.dumps({"jobs": len(state["jobs"]), "failed": bad}))
    return 0


# -------------------------------------------------------------------------------------------------------------------------- table
def _stamped(p: pathlib.Path) -> bool:
    rec = p.with_suffix(".json")
    if not (p.is_file() and rec.is_file()):
        return False
    if json.loads(rec.read_text()).get("plan_sha256") != plan_sha():
        raise SystemExit(f"{p.name}: score record not stamped with the current T001 plan")
    return True


def _f64(d: dict) -> dict:
    import numpy as np
    return {k: np.asarray(v, dtype=np.float64) for k, v in d.items()}


def cmd_table(a) -> int:
    import numpy as np
    import segarecomp_ml_v3_eval as ev
    import segarecomp_ml_v3_train as t
    plan = verify_plan()
    out = pathlib.Path(a.out).resolve()
    guard([out])
    X = xt()
    snap = snapshot_dir()
    dev_x = design().verify_plan()["conditions"]["C2"]["xplat_training"]
    meta = {}
    for d in X.genesis_dirs(GENESIS):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": X.genesis_seed_addresses(d)}
    xdirs = X.xplat_dirs(snap, dev_x)
    guard(xdirs)
    for d in xdirs:
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": m.get("seed_addresses", [])}
    lineage_of = {k: v["lineage"] for k, v in meta.items()}
    classes = {}

    def metrics(iid, scores):
        if iid not in classes:
            classes[iid] = (meta[iid]["dir"] / "classes.bin").read_bytes()
        return ev6.image_metrics_ext(classes[iid], meta[iid]["seeds"], scores, 128)

    def words_to_cells(arrs):
        return {iid: t.cell_max(np.mean([x[iid] for x in arrs], axis=0).astype(np.float32), meta[iid]["size"], 128) for iid in arrs[0]}

    files = comparator_files()
    load = X.load_npz
    missing, failed = {}, {}

    def own(model, fold, base=out):
        p = score_path(base, model, fold, None if model == "A" else PILOT_SEED)
        if not _stamped(p):
            return None
        sc = load(p)
        return _f64(sc) if model == "A" else {iid: t.cell_max(v.astype(np.float32), meta[iid]["size"], 128) for iid, v in sc.items()}

    def genesis_scores(cfg, fold):
        if cfg in ("A", "B", "C"):
            return own(cfg, fold)
        if cfg == "M1_HGB":
            return _f64(load(files[f"seg072/M1.{fold}.npz"]))
        if cfg == "M3_R_s49":
            return _f64(load(files[f"seg072/M3R.{fold}.npz"]))
        if cfg == "M3_R_s50":
            return _f64(load(files[f"seg075/M3R.C0.{fold}.s50.npz"]))
        if cfg == "M4_DENSE_s1@128":
            return words_to_cells([load(files[f"seg072/M4.{fold}.s1.npz"])])
        if cfg == "M4_DENSE_ens@128":
            return words_to_cells([load(files[f"seg072/M4.{fold}.s{s}.npz"]) for s in (1, 2, 3)])
        raise SystemExit(cfg)

    def xplat_scores(cfg):
        if cfg in ("A", "B", "C"):
            return own(cfg, "all")
        if cfg == "M1_HGB":
            return _f64(load(files["seg075/M1.C0.all.xplat.npz"]))
        if cfg == "M3_R_s49":
            return _f64(load(files["seg075/M3R.C0.all.xplat.npz"]))
        if cfg == "M4_DENSE_s1@128":
            return words_to_cells([load(files["seg075/M4.C0.all.s1.xplat.npz"])])
        if cfg == "M4_DENSE_ens@128":
            return words_to_cells([load(files[f"seg075/M4.C0.all.s{s}.xplat.npz"]) for s in (1, 2, 3)])
        raise SystemExit(cfg)

    configs = ("A", "B", "C", "M1_HGB", "M3_R_s49", "M3_R_s50", "M4_DENSE_s1@128", "M4_DENSE_ens@128")
    tables = {}
    for cfg in configs:
        g, miss = {}, []
        for fold in GENESIS:
            sc = genesis_scores(cfg, fold)
            if sc is None:
                miss.append(fold)
                continue
            for iid, s in sc.items():
                g[iid] = metrics(iid, s)
        x = {}
        if cfg != "M3_R_s50":
            sc = xplat_scores(cfg)
            if sc is None:
                miss.append("all")
            else:
                for iid, s in sc.items():
                    if lineage_of.get(iid) in dev_x:
                        x[iid] = metrics(iid, s)
        missing[cfg] = miss
        gr, xr = ev6.lineage_summary(g, lineage_of), ev6.lineage_summary(x, lineage_of)
        complete = not [m for m in miss if m != "all"] and set(gr) == set(GENESIS)
        tables[cfg] = {"genesis": gr, "xplat": xr, "genesis_summary": ev6.summary(gr) if complete else None,
                       "xplat_summary": ev6.summary(xr) if xr else None, "complete_genesis": complete}
    # M3_FROZEN (descriptive): stored production scores on Genesis, inference on the cross-platform representatives
    reg = {iid: {"size": v["size"]} for iid, v in meta.items() if not iid.startswith("xp/")}
    fz = ev.config_scores("M3_FROZEN", reg, [])
    g = {iid: metrics(iid, sc[0]) for iid, sc in fz.items()}
    fm = t.frozen_m3_model()
    x = {iid: metrics(iid, t.score_m3(fm, t.Image(meta[iid]["dir"]))) for iid in meta if iid.startswith("xp/")}
    gr, xr = ev6.lineage_summary(g, lineage_of), ev6.lineage_summary(x, lineage_of)
    tables["M3_FROZEN"] = {"genesis": gr, "xplat": xr, "genesis_summary": ev6.summary(gr), "xplat_summary": ev6.summary(xr),
                           "complete_genesis": True}
    # farm state, costs, determinism
    farm = json.loads((out / "farm.json").read_text()) if (out / "farm.json").is_file() else {"jobs": {}}
    for model in ("A", "B", "C"):
        bad = [k for k, v in farm["jobs"].items() if k.startswith(model + ".") and not k.endswith(".repro") and v["status"] != "ok"]
        if bad:
            failed[model] = bad
    costs = {}
    for model in ("A", "B", "C"):
        recs = [json.loads(p.read_text()) for p in sorted((out / "scores").glob(f"{model}.C0.*.json"))]
        if recs:
            n_img = sum(r["inference_seconds"]["images"] for r in recs)
            costs[model] = {"fits": len(recs), "fit_wall_seconds_mean": round(sum(r["fit_wall_seconds"] for r in recs) / len(recs), 1),
                            "fit_wall_seconds_max": max(r["fit_wall_seconds"] for r in recs),
                            "fit_cpu_hours_total": round(sum(r["fit_cpu_seconds"] for r in recs) / 3600, 2),
                            "inference_seconds_per_image_mean": round(sum(r["inference_seconds"]["total"] for r in recs) / max(1, n_img), 3),
                            "inference_seconds_per_image_max": max(r["inference_seconds"]["max"] for r in recs),
                            "peak_rss_mb_max": max(r["peak_rss_mb"] for r in recs)}
    for name, model in (("M3_R", "M3R"), ("M4_DENSE_s1", "M4"), ("M1_HGB", "M1")):
        recs = [json.loads(p.read_text()) for p in sorted((SEG075 / "t004" / "scores").glob(f"{model}.C*.json"))]
        recs = [r for r in recs if r.get("seed") in (None, 1, 49, 50)]
        if recs:
            costs[name + " (SEG-075 T004 records, context)"] = {"fits": len(recs), "fit_wall_seconds_mean":
                                                                round(sum(r["seconds"] for r in recs) / len(recs), 1)}
    determinism = {}
    for model in ("A", "B", "C"):
        seed = None if model == "A" else PILOT_SEED
        p1, p2 = score_path(out, model, DETERMINISM_FOLD, seed), score_path(out / "repro", model, DETERMINISM_FOLD, seed)
        if p1.is_file() and p2.is_file() and _stamped(p2):
            x1, x2 = load(p1), load(p2)
            determinism[model] = bool(sorted(x1) == sorted(x2) and all(np.array_equal(x1[k], x2[k]) for k in x1))
        else:
            determinism[model] = None
    rule_in = {c: (None if c in failed or not tables[c]["complete_genesis"] else {"rows": tables[c]["genesis"],
                                                                                "summary": tables[c]["genesis_summary"]})
               for c in ("A", "B", "C", "M4_DENSE_s1@128", "M3_R_s49")}
    decision = ev6.expansion_class(rule_in, plan["expansion_rule"])
    paired = {}
    for a_, b_ in (("B", "A"), ("C", "A"), ("C", "B"), ("B", "M4_DENSE_s1@128"), ("C", "M4_DENSE_s1@128"), ("B", "M3_R_s49"),
                   ("C", "M3_R_s49"), ("A", "M3_R_s49"), ("A", "M1_HGB"), ("M3_R_s50", "M3_R_s49"), ("M4_DENSE_ens@128", "M4_DENSE_s1@128")):
        ga, gb = tables[a_]["genesis"], tables[b_]["genesis"]
        d = {L: round(ga[L]["max_er"] - gb[L]["max_er"], 6) for L in GENESIS
             if L in ga and L in gb and ga[L]["max_er"] is not None and gb[L]["max_er"] is not None}
        paired[f"{a_}-{b_}"] = {"per_lineage_d_max_er": d, "mean": round(sum(d.values()) / len(d), 6) if d else None,
                                "lineages_better": sum(1 for v in d.values() if v < 0), "lineages_worse": sum(1 for v in d.values() if v > 0)}
    doc = {"schema": "segarecomp.xplat_explore.t001.results.v1", "task": "SEG-076-T001", "plan_sha256": plan_sha(),
           "status": "exploratory, development-only; single training seed for B and C", "missing": missing, "failed_fits": failed,
           "farm": {k: v["status"] for k, v in sorted(farm["jobs"].items())}, "tables": tables, "costs": costs,
           "determinism_bitwise": determinism, "paired_lineage_differences": paired, "expansion": decision,
           "data_boundary": "aggregates only: per-lineage ER/FRF, counts, AUROC and rank quantiles; no per-cell score, address or "
                            "image content"}
    (pathlib.Path(a.results) if a.results else RESULTS).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"expansion": decision["class"], "best_comparator": decision.get("best_comparator"),
                      "summaries": {k: (v["genesis_summary"] or {}).get("mean_er") for k, v in tables.items()},
                      "missing": {k: v for k, v in missing.items() if v}, "failed": failed, "determinism": determinism}, indent=1))
    return 0


# --------------------------------------------------------------------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan").set_defaults(func=lambda a: (print(json.dumps({"plan_sha256": hashlib.sha256(canonical(write_plan())).hexdigest()})), 0)[1])
    sub.add_parser("check").set_defaults(func=lambda a: (verify_plan(), print(json.dumps({"plan_sha256": plan_sha(), "check": "PASS"})), 0)[2])
    sub.add_parser("jobs").set_defaults(func=lambda a: ([print(j["id"]) for j in jobs()], 0)[1])
    f = sub.add_parser("fit")
    f.add_argument("--model", required=True, choices=("A", "B", "C"))
    f.add_argument("--fold", required=True)
    f.add_argument("--seed", type=int)
    f.add_argument("--out", required=True)
    f.set_defaults(func=lambda a: (run_fit(a.model, a.fold, a.seed, pathlib.Path(a.out).resolve()), 0)[1])
    fm = sub.add_parser("farm")
    fm.add_argument("--out", required=True)
    fm.add_argument("--only", nargs="*")
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
