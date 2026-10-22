#!/usr/bin/env python3
"""SEG-077-T005/T006/T007: A / B / C-v2 retraining with the qualified SEG-077 commercial lineage, development evaluation, blind evaluation
and the frozen decision rules (OFFLINE research code; development-only fits, private inputs, aggregate-only durable output).

Conditions (SEG-075 C0 Genesis folds unchanged):
  K0  the SEG-076 C0 training set: the 11 qualified SEG-072 Genesis development lineages (complete source-backed truth)
  K1  K0 plus every SEG-077 lineage that qualified (G077-01, Chuck Rock, partial evidence-tier labels: masked cells never train)
Development folds: leave-one-lineage-out over the 11 Genesis lineages (complete truth, directly comparable with SEG-076); a new lineage is
always training data, never a Genesis fold. `all` fits train on every condition image and are the models the blind cohort sees.

Models (frozen SEG-075/076 code, unchanged): A = segarecomp_xplat_models.fit_a/score_a (CPU venv); B = segarecomp_xplat_explore_rocm.
fit_bc_device(graph=False)/infer_bc_device (ROCm); C-v2 = segarecomp_xplat_models_v2.fit_cv2/infer_cv2 (ROCm). Seeds 1-3 for B and C-v2.

Commercial images enter through `attach_new`: seeds come ONLY from the cartridge header (reset vector cell when the vector lies in the
image), never from labels, so training and inference see the same label-free inputs (leakage tests in tests/segarecomp_seg077_test.py).

  plan      freeze the T005 pre-registration (tools/segarecomp_seg077.plan.json + .digest) before any SEG-077 fit or blind exposure
  check     verify the plan digest, bound code, corpus and comparator digests
  jobs      the pre-registered fit list
  fit       one write-once fit: --model A|B|CV2 --cond K0|K1 --fold <Genesis lineage>|all [--seed 1|2|3] [--repro]
  farm      run pending jobs with the frozen resource policy (single attempt; failures recorded, never retuned)
  reuse     the K0 reuse check: refit A.K0.D05, B.K0.D05.s1, CV2.K0.D05.s1 and compare bit for bit with the SEG-076 arrays
  table     aggregate-only development results (tools/segarecomp_seg077.t006.results.json) with the frozen development classes
  freeze-models  record the digests of every final model (all fits) and the evaluation policy before T007 exposure
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
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CORPUS = HARNESS / ".cache" / "m68k-corpus"
OUT = CORPUS / "seg077"
SEG076 = HARNESS / ".cache" / "seg076"
PLAN = HERE / "segarecomp_seg077.plan.json"
PLAN_DIGEST = HERE / "segarecomp_seg077.plan.digest"
RESULTS = HERE / "segarecomp_seg077.t006.results.json"
MODELS_FROZEN = HERE / "segarecomp_seg077.models.json"
CPU_PY = pathlib.Path(os.environ.get("SEG077_CPU_PYTHON", str(HARNESS / ".cache" / "seg049" / "venv" / "bin" / "python")))
ROCM_PY = pathlib.Path(os.environ.get("SEG077_ROCM_PYTHON", "/root/.venvs/torch-rocm64/bin/python"))
SEEDS = (1, 2, 3)
NEW_LINEAGES = {"G077-01": CORPUS / "titles" / "G077-01-a1b" / "sample"}
BLIND_ROOT = CORPUS / "blind"
BLIND_COHORT = {   # reserved 8 (operator-provided inputs; identity only, never used for training, tuning or selection)
    "B077-01": {"title": "Mortal Kombat (JUE) (REV 00)", "raw_sha256": "658547d3115c52f165a269099a9f7eaf66e5303a5004abf4a8774bcce6565bd2", "raw_size": 2097152},
    "B077-02": {"title": "Puyo Puyo (Japan)", "raw_sha256": "963abf6672c5875b26fd4cd010aa6d280ba9fe3081be46ec86b399e61262b167", "raw_size": 524288},
    "B077-03": {"title": "Super Fantasy Zone (E)", "raw_sha256": "4810844f80d7e2cbabffec456c0812e77fe53c720834735a603b356e21ffc2d2", "raw_size": 1048576},
    "B077-04": {"title": "ToeJam & Earl (U) (REV 02)", "raw_sha256": "65ea2386f5f4eda45335d8298ab30bc19d38b9bd5483ffa592818b645c14c163", "raw_size": 1048576},
    "B077-05": {"title": "The Lost Vikings (E)", "raw_sha256": "a536f13d5b74726ddad69e164225a6295a5ad27475c7b4d29bd5511924611e83", "raw_size": 1048576},
    "B077-06": {"title": "Zombies Ate My Neighbors (USA)", "raw_sha256": "b575e4e13d04c20e2be5aeeceb189f51a7a913ddb02d7b8f14d89c4ba2850845", "raw_size": 1048576},
    "B077-07": {"title": "Ristar (UE)", "raw_sha256": "1debdd6853a5630fb5602fbc2f5f03a4a929f833d00e072e733cac573414f05a", "raw_size": 2097152},
    "B077-08": {"title": "The Immortal (U)", "raw_sha256": "b60aa52e46a199f755a1eb99daea543c468acf07d83624c85a84cdd14b786375", "raw_size": 1048576}}
BOUND = ("segarecomp_seg077.py", "segarecomp_genesis_evidence.py", "segarecomp_genesis_evidence.t001.plan.digest",
         "segarecomp_xplat_models.py", "segarecomp_xplat_models_v2.py", "segarecomp_xplat_explore.py", "segarecomp_xplat_explore_rocm.py",
         "segarecomp_xplat_explore_eval.py", "segarecomp_xplat_stability.py", "segarecomp_xplat_transfer.py", "segarecomp_xplat_t005.py",
         "segarecomp_xplat_design.py", "segarecomp_ml_v3.py", "segarecomp_ml_v3_train.py", "segarecomp_ml_region_gen2.py",
         "segarecomp_xplat_cv2.py", "segarecomp_xplat_economics.py")
RESOURCES = {
    "phases": "all CPU (A) jobs first (at most 4 at once), then the GPU (B, C-v2) jobs (at most 3 at once, starts staggered); the two "
              "groups never overlap",
    "gpu_parallel_fits": 3, "cpu_parallel_fits": 4, "cpu_threads_per_fit": 2, "start_stagger_seconds": 30,
    "wall_clock_cap_minutes": {"A": 30, "B": 60, "CV2": 150}, "rss_limit_mb": 4096, "min_available_mb_to_start": 4000,
    "host_memory_floor_mb": 2000, "attempts": 1,
    "guard": "when MemAvailable < floor the MOST RECENTLY started fit is killed (SEG-076 practice) and recorded host_memory_guard",
    "infra_rerun": "exactly one identical re-run (recorded as attempt 2) after host_memory_guard, a signal kill, or 'interrupted' (a "
                   "persisted 'started' job found at farm start after a host crash); never after timeout, rss_limit or a model error",
    "missing_fits": "a configuration with any failed fit is INCOMPLETE: its ensemble and development class are not computed (reported "
                    "INCOMPLETE); a missing K1 all-fit model is excluded from the blind evaluation; if A_K1 is missing, the final class is "
                    "decided by precedence steps 1-2 only and otherwise recorded as INCOMPLETE_BASELINE; a failed reuse check means "
                    "K0 = INCOMPLETE (no silent refit)",
    "env": {"CUBLAS_WORKSPACE_CONFIG": ":4096:8", "OMP_NUM_THREADS": "2", "MKL_NUM_THREADS": "2"},
    "basis": "SEG-076 measured costs (B ROCm ~7 min, C-v2 ROCm ~26 min per 4000-step fit, 3 parallel C-v2 fits); the caps are >= 5x B and "
             "~6x C-v2; the T005 runtime preflight (recorded in the plan) confirms them on K1 before the matrix"}


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def canonical(obj) -> bytes:
    return (json.dumps(obj, indent=1, sort_keys=True) + "\n").encode()


def xt():
    import segarecomp_xplat_transfer as m
    return m


def rx():
    import segarecomp_xplat_explore as m
    return m


def genesis() -> list[str]:
    return list(xt().GENESIS_LINEAGES)


# ----------------------------------------------------------------------------------------------------------------------- images
def attach_new(im, d: pathlib.Path):
    """Label-free seeds for a SEG-077 commercial image: the reset-vector cell from the header identity, independent of classes.bin."""
    m = json.loads((d / "meta.json").read_text())
    ident = m["identity"]
    im._dir = d
    im.seed_addresses = [ident["reset"]] if ident.get("reset_in_image") else []
    im.seeds = {a // 128 for a in im.seed_addresses}
    return im


def load_new(lineages) -> list:
    import segarecomp_ml_v3_train as t
    out = []
    for L in lineages:
        d = NEW_LINEAGES[L]
        out.append(attach_new(t.Image(d), d))
    return out


def fold_images(cond: str, fold: str):
    """(train images, held images) for one fit; Genesis dirs come from the frozen SEG-075 C0 fold sets."""
    plan = verify_plan()
    if cond not in plan["conditions"]:
        raise SystemExit(f"unknown condition {cond}")
    train_dirs, held_dirs, held = xt().fold_sets(rx().design().verify_plan(), "C0", fold, rx().snapshot_dir())
    train = rx().load_dirs(train_dirs)
    if any(im.id.startswith("xp/") for im in train):
        raise SystemExit("SEG-077 conditions are Genesis-based (no cross-platform training)")
    new = plan["conditions"][cond]["new_lineages"]
    train += load_new(new)
    held_imgs = rx().load_dirs(held_dirs) if fold != "all" else load_new(plan["new_lineages_qualified"])
    if fold != "all" and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    return train, held_imgs


# ------------------------------------------------------------------------------------------------------------------------- plan
def corpus_identity() -> dict:
    out = {}
    for L, d in sorted(NEW_LINEAGES.items()):
        m = json.loads((d / "meta.json").read_text())
        out[L] = {"sha256": m["sha256"], "classes_sha256": m["classes_sha256"], "tokens_sha256": m["tokens_sha256"],
                  "seeds_sha256": m["seeds_sha256"], "cells": m["cells"], "evidence_plan_sha256": m["tools"]["plan_sha256"]}
    return out


def comparator_files() -> dict[str, pathlib.Path]:
    """SEG-076 K0 score files admitted for reuse (only after `reuse` passes): A (T001), B ROCm s1 (T001 amendment), C-v2 s1-s3 (T005)."""
    out = {}
    for f in genesis():
        out[f"A.K0.{f}"] = SEG076 / "t001" / "scores" / f"A.C0.{f}.npz"
        out[f"B.K0.{f}.s1"] = SEG076 / "t001-rocm" / "scores" / f"B.C0.{f}.s1.npz"
        for s in SEEDS:
            out[f"CV2.K0.{f}.s{s}"] = SEG076 / "t005" / "scores" / f"CV2.C0.{f}.s{s}.npz"
    out["A.K0.all"] = SEG076 / "t001" / "scores" / "A.C0.all.xplat.pkl"
    out["B.K0.all.s1"] = SEG076 / "t001-rocm" / "scores" / "B.C0.all.s1.xplat.pt"
    out["CV2.K0.all.s1"] = SEG076 / "t005" / "scores" / "CV2.C0.all.s1.xplat.pt"
    return out


DECISION = {
    "development_classes_per_model": {
        "definition": "paired lineage differences d_L = ER_K1(L) - ER_K0(L) over the 11 Genesis lineages, ER(L) = the lineage's maximum "
                      "image ER (SEG-075 definition; the Genesis 'mean ER' is the mean of these over lineages); B and C-v2 on the 3-seed "
                      "word-logit-mean ensemble, cell max at 128 bytes; mean d and the exact sign-flip p (two-sided)",
        "IMPROVED": "mean d <= -0.005 and p <= 0.05", "WORSENED": "mean d >= +0.005 and p <= 0.05", "NO_CHANGE": "otherwise"},
    "blind": {
        "metrics": "per title, on labelled cells only (positive: any I; negative: all D; masked cells excluded from both numerator and "
                   "denominator): labelled ER = (labelled cells ranked at or above the weakest labelled positive, ties counted - labelled "
                   "positives) / labelled negatives; plus B35/B50/B60 misses of labelled positive cells under the full-image ranking "
                   "(segarecomp_ml_v3 fixed budgets); a title is informative when it has >= 50 labelled positive and >= 50 labelled "
                   "negative cells; incomplete truth is reported as incomplete (no full-image recall is claimed)",
        "models": "A_K1, B_K1 3-seed ensemble, CV2_K1 3-seed ensemble (all fits); comparators A_K0, B_K0, CV2_K0 (SEG-076's all fits "
                  "plus the matching K0 seeds 2-3); the production M3_FROZEN path is bound to the SEG-072 registry images and is not run on "
                  "the blind cohort",
        "custodian": "a session separate from the training/selection side runs the frozen evidence protocol (tools/segarecomp_genesis_"
                     "evidence.py run-title, plan 03ebc8f8...) on each reserved title into <corpus>/blind/<B077-xx>/ (identity checked "
                     "against the pinned cohort); its labels are read only by blind-table after blind-score, which itself requires the "
                     "committed models.json",
        "exposure": "the custodian prepares blind labels under the frozen evidence protocol (plan 03ebc8f8...) in a sealed directory; the "
                    "evaluator scores only after tools/segarecomp_seg077.models.json is committed; no change after exposure"},
    "neural_advantage": "model X in {B, CV2} (K1 ensemble) beats A_K1 when ALL hold: blind mean labelled ER over informative titles lower by "
                        ">= 0.01; lower on >= 75% of informative titles; blind B50 misses not greater; development mean ER lower by >= "
                        "0.005 with sign-flip p <= 0.05 over the 11 lineages; and each of its 3 seeds alone has development mean ER below "
                        "A_K1's",
    "classes_precedence": [
        "LABEL_TRUTH_INSUFFICIENT: fewer than 4 informative blind titles",
        "DATA_INSUFFICIENT: fewer than 4 qualified new development lineages (the SEG-077 data hypothesis cannot be tested); the model "
        "comparison is still run and reported descriptively",
        "ECONOMICS_NOT_JUSTIFIED: a neural advantage exists but the measured economics (SEG-076 C5 bench, unchanged) do not show >= 10% "
        "lower generated C or compile CPU than A at equal recall",
        "C_ADVANTAGE / B_ADVANTAGE: a neural advantage (if both, the lower blind mean labelled ER; within 0.005 -> "
        "EQUIVALENT_WITH_COST_DIFFERENCE)",
        "EQUIVALENT_WITH_COST_DIFFERENCE: no advantage either way and, for at least one neural model, |blind delta| < 0.01 and "
        "|development delta| < 0.005",
        "A_PREFERRED: otherwise"],
    "economics": "measured (SEG-076 C5 bench, unchanged, 11 SEG-074 gate images) for (a) every model with a neural advantage and (b) "
                 "every K1 model whose development mean ER beats A_K0 (SEG-076 A) by >= 0.01; otherwise classified 'not measured: no "
                 "development retention advantage at verified full recall'; an unmeasured winner never receives economic credit",
    "learning_curve": "4/8/12/16-game checkpoints are infeasible: SEG-077-T002 bound STOP and exactly one new lineage qualified",
    "adaptive_budget": "no separately validated recall-safe adaptive policy exists for these models; fixed B35/B50/B60 only",
}


def environments() -> dict:
    def probe(py, code):
        r = subprocess.run([str(py), "-c", code], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else f"unavailable: {r.stderr.strip()[-120:]}"
    return {"cpu": probe(CPU_PY, "import sys,numpy,sklearn,torch;print(sys.version.split()[0],numpy.__version__,sklearn.__version__,torch.__version__)"),
            "rocm": probe(ROCM_PY, "import sys,numpy,torch;print(sys.version.split()[0],numpy.__version__,torch.__version__,torch.version.hip)")}


def build_plan() -> dict:
    files = comparator_files()
    preflight = OUT / "preflight.json"
    return {
        "schema": "segarecomp.seg077.plan.v1", "task": "SEG-077-T005",
        "status": "frozen before the first SEG-077 fit and before any blind exposure",
        "conditions": {"K0": {"new_lineages": []}, "K1": {"new_lineages": sorted(NEW_LINEAGES)}},
        "new_lineages_qualified": sorted(NEW_LINEAGES),
        "blind_cohort": BLIND_COHORT,
        "corpus": corpus_identity(),
        "genesis_folds": genesis(),
        "models": {"A": "segarecomp_xplat_models.fit_a/score_a (HistGradientBoosting, fixed configuration), CPU venv",
                   "B": "segarecomp_xplat_explore_rocm.fit_bc_device(graph=False)/infer_bc_device, M4 recipe, ROCm",
                   "CV2": "segarecomp_xplat_models_v2.fit_cv2/infer_cv2, M4 recipe, ROCm"},
        "seeds": list(SEEDS), "reuse": "K0 SEG-076 arrays and all-fit weights are admitted only when `reuse` refits A.K0.D05, B.K0.D05.s1 "
                                       "and CV2.K0.D05.s1 under this plan and all three are bitwise identical to the SEG-076 arrays "
                                       "(SEG-076 precedent: D05 is the determinism sentinel; C-v2 seeds 2-3 and the all weights are "
                                       "covered by the same environment identity, not by their own refit); otherwise K0 is INCOMPLETE and "
                                       "any K0 refit requires a recorded amendment",
        "comparators": {k: sha(p) for k, p in sorted(files.items())},
        "seeds_policy": "commercial images: reset-vector cell from the header only (label-free); Genesis images: SEG-072 registry seeds",
        "graph_integrity": "C-v2 candidate edges and the PROVEN flag come from CPU-owned decode tokens, instruction words and the label-free "
                           "seed roots only (segarecomp_xplat_models.window_graph, unchanged); tests/segarecomp_seg077_test.py proves that "
                           "A features, the C graph and seeds are invariant to classes.bin",
        "partial_labels": "masked cells (-1) never enter any loss (A: labels >= 0; B/C: word labels >= 0); the M4 lineage-uniform sampler "
                          "gives a mostly-masked lineage 1/12 of the windows, some of which carry no loss (frozen recipe, disclosed)",
        "resources": RESOURCES, "preflight": json.loads(preflight.read_text()) if preflight.is_file() else None,
        "decision": DECISION,
        "environments": environments(),
        "bound_code": {f: sha(HERE / f) for f in BOUND},
        "jobs": [j["id"] for j in jobs()],
    }


def write_plan() -> dict:
    doc = build_plan()
    PLAN.write_bytes(canonical(doc))
    PLAN_DIGEST.write_text(hashlib.sha256(canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if hashlib.sha256(data).hexdigest() != PLAN_DIGEST.read_text().strip():
        raise SystemExit("SEG-077 plan digest mismatch")
    doc = json.loads(data)
    if doc["bound_code"] != {f: sha(HERE / f) for f in BOUND}:
        raise SystemExit("SEG-077 bound code changed after the freeze")
    if doc["decision"] != json.loads(json.dumps(DECISION)) or doc["resources"] != json.loads(json.dumps(RESOURCES)):
        raise SystemExit("SEG-077 plan differs from the frozen rules in code")
    return doc


def verify_plan_full() -> dict:
    doc = verify_plan()
    if doc["corpus"] != corpus_identity():
        raise SystemExit("SEG-077 corpus differs from the plan")
    for k, p in comparator_files().items():
        if sha(p) != doc["comparators"][k]:
            raise SystemExit(f"comparator {k} changed")
    return doc


def plan_sha() -> str:
    return hashlib.sha256(PLAN.read_bytes()).hexdigest()


# ------------------------------------------------------------------------------------------------------------------------- jobs
def job_id(model, cond, fold, seed, repro=False) -> str:
    return f"{model}.{cond}.{fold}" + (f".s{seed}" if seed is not None else "") + (".repro" if repro else "")


def jobs() -> list[dict]:
    out = []
    folds = genesis() + ["all"]

    def add(model, cond, fold, seed, repro=False):
        out.append({"id": job_id(model, cond, fold, seed, repro), "model": model, "cond": cond, "fold": fold, "seed": seed, "repro": repro})
    add("A", "K0", "D05", None, True)
    add("B", "K0", "D05", 1, True)
    add("CV2", "K0", "D05", 1, True)
    for f in folds:
        add("A", "K1", f, None)
    for m in ("B", "CV2"):
        for s in SEEDS:
            for f in folds:
                add(m, "K1", f, s)
    for s in (2, 3):
        for f in folds:
            add("B", "K0", f, s)
        add("CV2", "K0", "all", s)
    return out


def score_path(model, cond, fold, seed, repro=False) -> pathlib.Path:
    return OUT / ("repro" if repro else "scores") / (job_id(model, cond, fold, seed) + ".npz")


def k0_reused(model, fold, seed) -> pathlib.Path | None:
    """SEG-076 array admitted as K0 (only after a passing reuse.json)."""
    key = job_id(model, "K0", fold, seed)
    return comparator_files().get(key)


# -------------------------------------------------------------------------------------------------------------------------- fit
def cmd_fit(a) -> int:
    import segarecomp_ml_v3_train as t
    verify_plan_full()
    seed = a.seed if a.model != "A" else None
    if not any(j["id"] == job_id(a.model, a.cond, a.fold, seed, a.repro) for j in jobs()):
        raise SystemExit("not a pre-registered SEG-077 fit")
    path = score_path(a.model, a.cond, a.fold, seed, a.repro)
    if path.exists() or path.with_suffix(".json").exists():
        raise SystemExit(f"{path.name}: already exists; fits are write-once")
    train, held = fold_images(a.cond, a.fold)
    log = (lambda m: print(f"[{a.model}/{a.cond}/{a.fold}/s{seed}] {m}", flush=True))
    w0, c0 = time.time(), time.process_time()
    env = {}
    if a.model == "A":
        import segarecomp_xplat_models as xm
        net = xm.fit_a(train)
        scorer = (lambda im: xm.score_a(net, im))
    else:
        import torch
        dev = torch.device("cuda:0")
        if a.model == "B":
            import segarecomp_xplat_explore_rocm as er
            net = er.fit_bc_device(train, seed, False, dev, log)
            scorer = (lambda im: er.infer_bc_device(net, im, False, dev))
        else:
            import segarecomp_xplat_models_v2 as v2
            net = v2.fit_cv2(train, seed, dev, log)
            scorer = (lambda im: v2.infer_cv2(net, im, dev))
        torch.cuda.synchronize()
        env = {"torch": torch.__version__, "hip": torch.version.hip, "device": torch.cuda.get_device_name(0)}
    fit_wall, fit_cpu = time.time() - w0, time.process_time() - c0
    scores, infer = {}, {}
    for im in held:
        t1 = time.time()
        scores[im.id] = scorer(im)
        infer[im.id] = round(time.time() - t1, 3)
    path.parent.mkdir(parents=True, exist_ok=True)
    t.save_scores(path, scores)
    if a.fold == "all":
        if a.model == "A":
            import pickle
            path.with_suffix(".pkl").write_bytes(pickle.dumps(net))
        else:
            import torch
            torch.save({k: v.cpu() for k, v in net.state_dict().items()}, path.with_suffix(".pt"))
    commit = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    rec = {"task": "SEG-077-T006", "model": a.model, "condition": a.cond, "fold": a.fold, "seed": seed, "repro": a.repro,
           "plan_sha256": plan_sha(), "product_commit": commit, "fit_wall_seconds": round(fit_wall, 1), "fit_cpu_seconds": round(fit_cpu, 1),
           "inference_seconds": {"images": len(infer), "total": round(sum(infer.values()), 2)},
           "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024,
           "train_images": sorted(im.id for im in train), "held_images": sorted(im.id for im in held),
           "train_lineages": sorted({im.lineage for im in train}),
           "environment": dict(env, python=platform.python_version(), os=platform.platform())}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "condition", "fold", "seed", "fit_wall_seconds", "peak_rss_mb")}), flush=True)
    return 0


def _available_mb() -> int:
    for ln in pathlib.Path("/proc/meminfo").read_text().splitlines():
        if ln.startswith("MemAvailable:"):
            return int(ln.split()[1]) // 1024
    return 0


def _rss_mb(pid: int) -> int:
    try:
        for ln in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
            if ln.startswith("VmRSS:"):
                return int(ln.split()[1]) // 1024
    except OSError:
        pass
    return 0


def cmd_farm(a) -> int:
    verify_plan_full()
    status_p = OUT / "farm.json"
    OUT.mkdir(parents=True, exist_ok=True)
    st = json.loads(status_p.read_text()) if status_p.is_file() else {"plan_sha256": plan_sha(), "jobs": {}}
    if st["plan_sha256"] != plan_sha():
        raise SystemExit("farm.json belongs to another plan")
    for jid, v in st["jobs"].items():             # a 'started' job found here was interrupted by a host crash
        if v.get("status") == "started":
            st["jobs"][jid] = {"status": "interrupted", "attempts": v.get("attempts", 1)}
    only = set(a.only or [])

    def runnable(j):
        v = st["jobs"].get(j["id"])
        if v is None:
            return True
        return v["status"] in ("interrupted", "host_memory_guard", "killed_signal") and v.get("attempts", 1) < 2
    env = dict(os.environ, **RESOURCES["env"])
    (OUT / "logs").mkdir(exist_ok=True)

    def save():
        status_p.write_text(json.dumps(st, indent=1, sort_keys=True))

    for phase in ("cpu", "gpu"):
        todo = [j for j in jobs() if (not only or j["id"] in only) and runnable(j) and (j["model"] == "A") == (phase == "cpu")]
        limit = RESOURCES["cpu_parallel_fits"] if phase == "cpu" else RESOURCES["gpu_parallel_fits"]
        running = {}
        while todo or running:
            if todo and len(running) < limit and _available_mb() >= RESOURCES["min_available_mb_to_start"]:
                j = todo.pop(0)
                attempt = st["jobs"].get(j["id"], {}).get("attempts", 0) + 1
                py = CPU_PY if j["model"] == "A" else ROCM_PY
                cmd = [str(py), str(HERE / "segarecomp_seg077.py"), "fit", "--model", j["model"], "--cond", j["cond"], "--fold", j["fold"]]
                if j["seed"] is not None:
                    cmd += ["--seed", str(j["seed"])]
                if j["repro"]:
                    cmd += ["--repro"]
                if attempt == 2:                  # an infrastructure re-run starts from a clean write-once slot
                    sp = score_path(j["model"], j["cond"], j["fold"], j["seed"], j["repro"])
                    for q in (sp, sp.with_suffix(".json"), sp.with_suffix(".pt"), sp.with_suffix(".pkl")):
                        if q.exists():
                            q.unlink()
                log = open(OUT / "logs" / f"{j['id']}.a{attempt}.log", "w")
                pr = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
                st["jobs"][j["id"]] = {"status": "started", "attempts": attempt}
                save()
                running[j["id"]] = (pr, time.time(), j, log, 0, attempt)
                time.sleep(RESOURCES["start_stagger_seconds"])
                continue
            newest = max(running, key=lambda k: running[k][1]) if running else None
            for jid, (pr, t0, j, log, peak, attempt) in list(running.items()):
                peak = max(peak, _rss_mb(pr.pid))
                running[jid] = (pr, t0, j, log, peak, attempt)
                reason = None
                if pr.poll() is None:
                    if time.time() - t0 > RESOURCES["wall_clock_cap_minutes"][j["model"]] * 60:
                        reason = "timeout"
                    elif peak > RESOURCES["rss_limit_mb"]:
                        reason = "rss_limit"
                    elif jid == newest and _available_mb() < RESOURCES["host_memory_floor_mb"]:
                        reason = "host_memory_guard"
                    if reason is None:
                        continue
                    pr.kill()
                    pr.wait()
                    rec = {"status": reason if reason == "host_memory_guard" else "failed", "reason": reason}
                else:
                    ok = pr.returncode == 0
                    rec = {"status": "ok"} if ok else ({"status": "killed_signal"} if pr.returncode < 0 else
                                                       {"status": "failed", "reason": f"exit {pr.returncode}"})
                rec.update({"attempts": attempt, "wall_seconds": round(time.time() - t0, 1), "peak_rss_mb_watchdog": peak})
                if rec["status"] in ("host_memory_guard", "killed_signal") and attempt < 2:
                    todo.append(j)                # the single pre-registered infrastructure re-run
                st["jobs"][jid] = rec
                log.close()
                del running[jid]
                save()
            time.sleep(5)
    save()
    print(json.dumps({k: sum(1 for v in st["jobs"].values() if v["status"] == k) for k in ("ok", "failed", "host_memory_guard", "killed_signal")}))
    return 0


# ---------------------------------------------------------------------------------------------------------------------- metrics
def labelled_metrics(classes: bytes, scores, seeds=(), cell: int = 128) -> dict:
    """Blind/partial-truth metrics on labelled cells only (pure). Positive: any I; negative: all D; masked cells are excluded from the
    labelled ER but still occupy rank positions in the full-image budgets (SEG-072 policy_region: top-k of ALL cells plus seed cells);
    misses count labelled instruction starts outside the region."""
    import segarecomp_ml_v3 as v3
    labels = v3.cell_labels(classes, cell)
    s = [float(x) for x in scores]
    if len(s) != len(labels):
        raise SystemExit("score/cell count mismatch")
    seeds = {a // cell for a in seeds if 0 <= a < len(classes)}     # seed ADDRESSES -> cells, as the frozen SEG-075/076 callers do
    pos = [i for i, y in enumerate(labels) if y == 1]
    neg = [i for i, y in enumerate(labels) if y == 0]
    out = {"cells": len(labels), "labelled_positive": len(pos), "labelled_negative": len(neg),
           "informative": len(pos) >= 50 and len(neg) >= 50}
    if pos and neg:
        weakest = min(s[i] for i in pos)
        needed_neg = sum(1 for i in neg if s[i] >= weakest)
        out["labelled_er"] = needed_neg / len(neg)
    else:
        out["labelled_er"] = None
    starts = v3.starts_of(classes)
    for pol in ("B35", "B50", "B60"):
        reg = v3.policy_region(pol, s, None, seeds)["region"]
        out[f"misses_{pol}"] = v3.misses(starts, reg, cell)
        out[f"missed_cells_{pol}"] = sum(1 for i in pos if i not in reg)
    return out


def paired(d: dict) -> dict:
    import segarecomp_xplat_stability as st
    vals = [d[k] for k in sorted(d)]
    return {"mean": sum(vals) / len(vals), "p": st.sign_flip_p(vals), "n": len(vals)}


def dev_class(d: dict) -> str:
    p = paired(d)
    if p["mean"] <= -0.005 and p["p"] <= 0.05:
        return "IMPROVED"
    if p["mean"] >= 0.005 and p["p"] <= 0.05:
        return "WORSENED"
    return "NO_CHANGE"


def neural_advantage(blind_x: dict, blind_a: dict, dev_x_mean: float, dev_a_mean: float, dev_d: dict, seed_dev_means: list,
                     misses_x: int, misses_a: int) -> bool:
    """The frozen neural-advantage rule (pure): blind_* = {title: labelled ER} over informative titles."""
    titles = sorted(blind_a)
    if not titles or sorted(blind_x) != titles:
        return False
    mx = sum(blind_x[t] for t in titles) / len(titles)
    ma = sum(blind_a[t] for t in titles) / len(titles)
    lower = sum(1 for t in titles if blind_x[t] < blind_a[t])
    p = paired(dev_d)
    return (ma - mx >= 0.01 and lower >= 0.75 * len(titles) and misses_x <= misses_a and dev_a_mean - dev_x_mean >= 0.005
            and p["p"] <= 0.05 and len(seed_dev_means) == 3 and all(m < dev_a_mean for m in seed_dev_means))


def final_class(n_informative: int, n_new_lineages: int, adv: dict, econ_ok: dict, equivalent: bool, blind_means: dict) -> str:
    """The frozen precedence (pure). adv = {"B": bool, "CV2": bool}; econ_ok = {"B": bool|None, "CV2": bool|None}."""
    if n_informative < 4:
        return "LABEL_TRUTH_INSUFFICIENT"
    if n_new_lineages < 4:
        return "DATA_INSUFFICIENT"
    winners = [m for m in ("B", "CV2") if adv.get(m)]
    if winners and not any(econ_ok.get(m) for m in winners):
        return "ECONOMICS_NOT_JUSTIFIED"
    winners = [m for m in winners if econ_ok.get(m)]
    if len(winners) == 2:
        if abs(blind_means["B"] - blind_means["CV2"]) < 0.005:
            return "EQUIVALENT_WITH_COST_DIFFERENCE"
        return "C_ADVANTAGE" if blind_means["CV2"] < blind_means["B"] else "B_ADVANTAGE"
    if winners:
        return "C_ADVANTAGE" if winners[0] == "CV2" else "B_ADVANTAGE"
    if equivalent:
        return "EQUIVALENT_WITH_COST_DIFFERENCE"
    return "A_PREFERRED"


# ------------------------------------------------------------------------------------------------------------------------ reuse
def cmd_reuse(a) -> int:
    import segarecomp_xplat_transfer as X
    import numpy as np
    verify_plan_full()
    res = {}
    for model, seed in (("A", None), ("B", 1), ("CV2", 1)):
        mine = score_path(model, "K0", "D05", seed, True)
        theirs = k0_reused(model, "D05", seed)
        if not mine.is_file():
            res[model] = {"status": "missing"}
            continue
        x1, x2 = X.load_npz(mine), X.load_npz(theirs)
        same = sorted(x1) == sorted(x2) and all(np.array_equal(np.asarray(x1[k]), np.asarray(x2[k])) for k in x1)
        res[model] = {"status": "bitwise_identical" if same else "different"}
    res["admitted"] = all(v.get("status") == "bitwise_identical" for v in res.values())
    res["plan_sha256"] = plan_sha()
    (OUT / "reuse.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps(res))
    return 0


# ------------------------------------------------------------------------------------------------------------------------ table
def _scores(model, cond, fold, seed, status) -> dict | None:
    """Raw score arrays of one development fit (K0 from SEG-076 when reuse was admitted and the fit exists there)."""
    import segarecomp_xplat_transfer as X
    reuse = json.loads((OUT / "reuse.json").read_text()) if (OUT / "reuse.json").is_file() else {}
    if cond == "K0" and k0_reused(model, fold, seed) is not None and fold != "all":
        if not reuse.get("admitted"):
            return None                           # K0 INCOMPLETE (a refit needs a recorded amendment)
        return X.load_npz(k0_reused(model, fold, seed))
    jid = job_id(model, cond, fold, seed)
    if status.get(jid, {}).get("status") != "ok":
        return None
    p = score_path(model, cond, fold, seed)
    r = json.loads(p.with_suffix(".json").read_text())
    if r.get("plan_sha256") != plan_sha() or (r["model"], r["condition"], r["fold"], r["seed"]) != (model, cond, fold, seed):
        raise SystemExit(f"{p.name}: record does not match the plan")
    return X.load_npz(p)


def cmd_table(a) -> int:
    import numpy as np
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_stability as st
    verify_plan_full()
    status = json.loads((OUT / "farm.json").read_text())["jobs"]
    meta = st.genesis_meta()
    classes: dict = {}
    tables, missing = {}, []

    def cells(words):
        return {iid: t.cell_max(np.asarray(w, dtype=np.float32), meta[iid]["size"], 128) for iid, w in words.items()}

    def add(name, per_fold):
        rows, summ, _ = st.lineage_table(per_fold, meta, classes)
        tables[name] = {"rows": rows, "summary": summ}

    for cond in ("K0", "K1"):
        per = {}
        for f in genesis():
            sc = _scores("A", cond, f, None, status)
            if sc is None:
                missing.append(job_id("A", cond, f, None))
            else:
                per[f] = {k: np.asarray(v, dtype=np.float64) for k, v in sc.items()}
        if len(per) == len(genesis()):
            add(f"A.{cond}", per)
        for m in ("B", "CV2"):
            words = {}
            for s in SEEDS:
                per = {}
                for f in genesis():
                    sc = _scores(m, cond, f, s, status)
                    if sc is None:
                        missing.append(job_id(m, cond, f, s))
                    else:
                        per[f] = sc
                if len(per) == len(genesis()):
                    words[s] = per
                    add(f"{m}.{cond}.s{s}", {f: cells(w) for f, w in per.items()})
            if len(words) == len(SEEDS):
                add(f"{m}.{cond}.ens3", {f: cells({iid: np.mean([words[s][f][iid] for s in SEEDS], axis=0) for iid in words[1][f]})
                                         for f in genesis()})
    dev = {}
    for m, k0, k1 in (("A", "A.K0", "A.K1"), ("B", "B.K0.ens3", "B.K1.ens3"), ("CV2", "CV2.K0.ens3", "CV2.K1.ens3")):
        if k0 not in tables or k1 not in tables:
            dev[m] = {"class": "INCOMPLETE", "missing": [k for k in (k0, k1) if k not in tables]}
            continue
        if True:
            d = {L: tables[k1]["rows"][L]["max_er"] - tables[k0]["rows"][L]["max_er"] for L in genesis()}
            pr = paired(d)
            dev[m] = {"class": dev_class(d), "mean_d": round(pr["mean"], 6), "p": round(pr["p"], 6),
                      "per_lineage_d": {L: round(v, 6) for L, v in d.items()}}
    seed_means = {m: [tables[f"{m}.K1.s{s}"]["summary"]["mean_er"] for s in SEEDS if f"{m}.K1.s{s}" in tables] for m in ("B", "CV2")}
    recs = [json.loads(p.read_text()) for p in sorted((OUT / "scores").glob("*.json"))]
    costs = {m: {"fits": sum(1 for r in recs if r["model"] == m),
                 "fit_hours": round(sum(r["fit_wall_seconds"] for r in recs if r["model"] == m) / 3600, 2),
                 "inference_seconds_per_image": round(sum(r["inference_seconds"]["total"] for r in recs if r["model"] == m) /
                                                      max(1, sum(r["inference_seconds"]["images"] for r in recs if r["model"] == m)), 3)}
             for m in ("A", "B", "CV2")}
    secondary = {}
    for m, seeds in (("A", [None]), ("B", list(SEEDS)), ("CV2", list(SEEDS))):
        for cond in ("K0", "K1"):
            for s in seeds:
                sc = _scores(m, cond, "all", s, status) if job_id(m, cond, "all", s) in status else None
                if not sc:
                    continue
                for L, d in NEW_LINEAGES.items():
                    iid = json.loads((d / "meta.json").read_text())["id"]
                    if iid in sc:
                        size = json.loads((d / "meta.json").read_text())["size"]
                        v = np.asarray(sc[iid], dtype=np.float64) if m == "A" else t.cell_max(np.asarray(sc[iid], dtype=np.float32), size, 128)
                        mm = labelled_metrics((d / "classes.bin").read_bytes(), v, attach_new(type("I", (), {})(), d).seed_addresses)
                        secondary[job_id(m, cond, "all", s) + "@" + L] = dict(mm, in_sample=(cond == "K1"))
    reuse = json.loads((OUT / "reuse.json").read_text()) if (OUT / "reuse.json").is_file() else None
    doc = {"schema": "segarecomp.seg077.t006.results.v1", "task": "SEG-077-T006", "plan_sha256": plan_sha(), "reuse": reuse,
           "farm": {k: v["status"] for k, v in sorted(status.items())}, "missing": sorted(set(missing)),
           "configurations": {k: {"summary": v["summary"], "lineage_max_er": {L: r["max_er"] for L, r in v["rows"].items()},
                                  "lineage_b50_misses": {L: r["misses_B50"] for L, r in v["rows"].items()}} for k, v in tables.items()},
           "development_classes": dev, "k1_seed_mean_er": seed_means, "costs": costs, "secondary_new_lineage": secondary,
           "data_boundary": "aggregates only"}
    pathlib.Path(a.results or RESULTS).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"development_classes": {m: v["class"] for m, v in dev.items()},
                      "mean_er": {k: v["summary"]["mean_er"] for k, v in tables.items()}, "missing": len(set(missing))}, indent=1))
    return 0


# ----------------------------------------------------------------------------------------------------------------- final models
def final_models() -> dict[str, pathlib.Path]:
    """The blind cohort's models: K1 all fits (A; B and C-v2 seeds 1-3) and the K0 (SEG-076-equivalent) comparators. SEG-076 weights
    are admitted only with a passing reuse check."""
    if not json.loads((OUT / "reuse.json").read_text()).get("admitted"):
        raise SystemExit("K0 reuse not admitted: the SEG-076 all-fit weights cannot be used")
    out = {"A.K1": score_path("A", "K1", "all", None).with_suffix(".pkl"), "A.K0": comparator_files()["A.K0.all"]}
    for m in ("B", "CV2"):
        for s in SEEDS:
            out[f"{m}.K1.s{s}"] = score_path(m, "K1", "all", s).with_suffix(".pt")
            out[f"{m}.K0.s{s}"] = comparator_files()[f"{m}.K0.all.s1"] if s == 1 else score_path(m, "K0", "all", s).with_suffix(".pt")
    return out


def cmd_freeze_models(a) -> int:
    verify_plan_full()
    fm = final_models()
    doc = {"schema": "segarecomp.seg077.models.v1", "plan_sha256": plan_sha(),
           "models": {k: sha(p) for k, p in sorted(fm.items()) if p.is_file()},
           "missing": sorted(k for k, p in fm.items() if not p.is_file()),
           "policy": {"budgets": ["B35", "B50", "B60"], "ensemble": "B and C-v2: mean of the 3 seeds' word logits, cell max at 128 bytes",
                      "metrics": DECISION["blind"]["metrics"]}}
    MODELS_FROZEN.write_bytes(canonical(doc))
    print(json.dumps({"models_sha256": hashlib.sha256(canonical(doc)).hexdigest(), "models": len(doc["models"])}))
    return 0


def verify_models() -> dict:
    doc = json.loads(MODELS_FROZEN.read_text())
    fm = final_models()
    if doc["plan_sha256"] != plan_sha() or doc["models"] != {k: sha(fm[k]) for k in sorted(doc["models"])}:
        raise SystemExit("final models differ from the committed tools/segarecomp_seg077.models.json")
    return doc


# ------------------------------------------------------------------------------------------------------------------------ blind
def blind_dirs(root: pathlib.Path) -> list[pathlib.Path]:
    """The custodian's blind samples; each must match the pinned cohort identity."""
    out = []
    for p in sorted(root.glob("*/sample/meta.json")):
        bid = p.parent.parent.name
        m = json.loads(p.read_text())
        want = BLIND_COHORT.get(bid)
        if want is None or m["identity"]["raw_sha256"] != want["raw_sha256"] or m["identity"]["raw_size"] != want["raw_size"]:
            raise SystemExit(f"{bid}: blind sample is not a pinned cohort member")
        img = pathlib.Path(m["image"]).read_bytes()
        if hashlib.sha256(img).hexdigest() != m["sha256"] or (m["identity"]["format"] == "plain" and m["sha256"] != want["raw_sha256"]):
            raise SystemExit(f"{bid}: recomputed image identity differs")
        if sha(p.parent / "tokens.bin") != m["tokens_sha256"]:
            raise SystemExit(f"{bid}: tokens differ from the custodian record")
        out.append(p.parent)
    missing = sorted(set(BLIND_COHORT) - {d.parent.name for d in out})
    if missing:
        raise SystemExit(f"blind cohort incomplete: {missing} (record them as incomplete-truth titles via the custodian, then rerun)")
    return out


def cmd_blind_score(a) -> int:
    """Evaluator side: score the custodian's blind images with one frozen model (no labels are read here). With --equiv, score the
    qualified development lineage(s) through the same label-free path and require bitwise equality with the scores the K1 all fit wrote
    in its own process (proves the hand-built image and the weight reload are equivalent) BEFORE any blind exposure."""
    import pickle
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_transfer as X
    import numpy as np
    verify_plan_full()
    verify_models()
    key = a.key
    if key not in verify_models()["models"]:
        raise SystemExit(f"{key} is not a recorded final model")
    path = final_models()[key]
    ims = []
    src = [NEW_LINEAGES[L] for L in sorted(NEW_LINEAGES)] if a.equiv else blind_dirs(pathlib.Path(a.blind))
    if not a.equiv:
        for k in verify_models()["models"]:
            if k.endswith(("K1", ".K1.s1", ".K1.s2", ".K1.s3")) and not (OUT / "blind" / f"equiv.{k}.json").is_file():
                raise SystemExit(f"equivalence gate missing for {k}")
    for d in src:
        im = t.Image.__new__(t.Image)
        m = json.loads((d / "meta.json").read_text())
        im.id, im.lineage, im.kind = m["id"], m["lineage"], m["kind"]
        rom = pathlib.Path(m["image"]).read_bytes()
        if hashlib.sha256(rom).hexdigest() != m["sha256"]:
            raise SystemExit("blind image identity mismatch")
        im.rom, im.size = rom, len(rom)
        im.n_words = im.size // 2
        np_ = __import__("numpy")
        tok = (d / "tokens.bin").read_bytes()
        im.tokens = np_.frombuffer(tok, dtype=np_.uint8).reshape(im.n_words, len(t.TOKEN_FIELDS))
        arr = np_.frombuffer(rom[:im.n_words * 2], dtype=np_.uint8).reshape(im.n_words, 2)
        im.hi, im.lo = arr[:, 0], arr[:, 1]
        im.n_cells = (im.size + 127) // 128
        im.classes = None                      # labels are never loaded on the evaluator's scoring path
        im.labels = np_.full(im.n_cells, -1, dtype=np_.int8)
        im.wlabels = np_.full(im.n_words, -1, dtype=np_.int8)
        im.starts = []
        im._padded = None
        ims.append(attach_new(im, d))
    scores = {}
    if key.startswith("A."):
        import segarecomp_xplat_models as xm
        clf = pickle.loads(path.read_bytes())
        for im in ims:
            scores[im.id] = xm.score_a(clf, im)
    else:
        import torch
        dev = torch.device("cuda:0")
        if key.startswith("B."):
            import segarecomp_xplat_models as xm
            import segarecomp_xplat_explore_rocm as er
            net = xm.build_bc(torch, False)
            net.load_state_dict(torch.load(path, map_location="cpu"))
            net = net.to(dev).eval()
            for im in ims:
                scores[im.id] = er.infer_bc_device(net, im, False, dev)
        else:
            import segarecomp_xplat_models_v2 as v2
            net = v2.build_cv2(torch)()
            net.load_state_dict(torch.load(path, map_location="cpu"))
            net = net.to(dev).eval()
            for im in ims:
                scores[im.id] = v2.infer_cv2(net, im, dev)
    if a.equiv:
        m, cond, rest = key.split(".")[0], "K1", key.split(".")[2:]
        seed = int(rest[0][1:]) if rest else None
        ref = X.load_npz(score_path(m, cond, "all", seed))
        same = sorted(ref) == sorted(scores) and all(np.array_equal(np.asarray(ref[k]), np.asarray(scores[k])) for k in ref)
        (OUT / "blind").mkdir(parents=True, exist_ok=True)
        (OUT / "blind" / f"equiv.{key}.json").write_text(json.dumps({"key": key, "bitwise_identical": bool(same)}))
        print(json.dumps({"key": key, "bitwise_identical": bool(same)}))
        if not same:
            (OUT / "blind" / f"equiv.{key}.json").unlink()
            raise SystemExit("blind scoring path is not equivalent to the fit-time scoring")
        return 0
    outp = OUT / "blind" / f"{key}.npz"
    t.save_scores(outp, scores)
    outp.with_suffix(".json").write_text(json.dumps({"key": key, "model_sha256": sha(path), "plan_sha256": plan_sha(),
                                                     "models_json_sha256": sha(MODELS_FROZEN), "images": sorted(scores)}, indent=1))
    print(json.dumps({"key": key, "images": len(scores)}))
    return 0


BLIND_RESULTS = HERE / "segarecomp_seg077.t007.results.json"


def cmd_blind_table(a) -> int:
    """Evaluator side, after every blind-score run: labelled metrics per title (custodian labels read only here), ensembles, the frozen
    neural-advantage rule and the final class precedence."""
    import numpy as np
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_transfer as X
    plan = verify_plan_full()
    verify_models()
    dev = json.loads(RESULTS.read_text())
    if dev.get("plan_sha256") != plan_sha():
        raise SystemExit("development results belong to another plan")
    dirs = blind_dirs(pathlib.Path(a.blind))
    metas = {json.loads((d / "meta.json").read_text())["id"]: d for d in dirs}
    raw = {}
    recorded = verify_models()["models"]
    for key in recorded:
        f = OUT / "blind" / f"{key}.npz"
        if not f.is_file():
            raise SystemExit(f"missing blind scores for {key}")
        r = json.loads(f.with_suffix(".json").read_text())
        if r["model_sha256"] != sha(final_models()[key]) or r["plan_sha256"] != plan_sha():
            raise SystemExit(f"{key}: blind scores do not match the frozen model")
        raw[key] = X.load_npz(f)

    def cellsof(iid, w):
        size = json.loads((metas[iid] / "meta.json").read_text())["size"]
        return t.cell_max(np.asarray(w, dtype=np.float32), size, 128)
    configs = {k: {i: np.asarray(v, dtype=np.float64) for i, v in raw[k].items()} for k in ("A.K1", "A.K0") if k in raw}
    for m in ("B", "CV2"):
        for c in ("K0", "K1"):
            for sd in SEEDS:
                if f"{m}.{c}.s{sd}" in raw:
                    configs[f"{m}.{c}.s{sd}"] = {i: cellsof(i, w) for i, w in raw[f"{m}.{c}.s{sd}"].items()}
            if all(f"{m}.{c}.s{sd}" in raw for sd in SEEDS):
                configs[f"{m}.{c}.ens3"] = {i: cellsof(i, np.mean([raw[f"{m}.{c}.s{sd}"][i] for sd in SEEDS], axis=0)) for i in metas}
    per = {}
    for name, sc in configs.items():
        per[name] = {}
        for iid, d in metas.items():
            seeds = attach_new(type("I", (), {})(), d).seed_addresses
            per[name][iid] = labelled_metrics((d / "classes.bin").read_bytes(), sc[iid], seeds)
    ref = per.get("A.K1") or next(iter(per.values()))
    informative = sorted(i for i, mm in ref.items() if mm["informative"])

    def blind_er(name):
        return {i: per[name][i]["labelled_er"] for i in informative}

    def b50(name):
        return sum(per[name][i]["misses_B50"] for i in metas)
    conf = dev["configurations"]
    adv, equiv, means = {}, False, {}
    baseline_ok = "A.K1" in configs and "A.K1" in conf
    a_mean = conf["A.K1"]["summary"]["mean_er"] if baseline_ok else None
    for m in ("B", "CV2"):
        x = f"{m}.K1.ens3"
        if not baseline_ok or x not in configs or x not in conf or any(f"{m}.K1.s{sd}" not in conf for sd in SEEDS):
            adv[m], means[m] = False, None
            continue
        dd = {L: conf[x]["lineage_max_er"][L] - conf["A.K1"]["lineage_max_er"][L] for L in genesis()}
        seed_means = [conf[f"{m}.K1.s{sd}"]["summary"]["mean_er"] for sd in SEEDS]
        adv[m] = neural_advantage(blind_er(x), blind_er("A.K1"), conf[x]["summary"]["mean_er"], a_mean, dd, seed_means, b50(x), b50("A.K1"))
        bx = blind_er(x)
        ba = blind_er("A.K1")
        means[m] = sum(bx.values()) / len(bx) if bx else None
        if informative and abs(sum(bx.values()) / len(bx) - sum(ba.values()) / len(ba)) < 0.01 and abs(conf[x]["summary"]["mean_er"] - a_mean) < 0.005:
            equiv = True
    econ = json.loads(pathlib.Path(a.economics).read_text()) if a.economics else {}
    econ_ok = {m: econ.get(m) for m in ("B", "CV2")}
    cls = final_class(len(informative), len(plan["new_lineages_qualified"]), adv, econ_ok, equiv, means)
    if not baseline_ok and cls not in ("LABEL_TRUTH_INSUFFICIENT", "DATA_INSUFFICIENT"):
        cls = "INCOMPLETE_BASELINE"
    summary = {}
    for name in configs:
        ers = [per[name][i]["labelled_er"] for i in informative]
        summary[name] = {"mean_labelled_er": round(sum(ers) / len(ers), 6) if ers else None, "worst_labelled_er": round(max(ers), 6) if ers else None,
                         "misses_B35": sum(per[name][i]["misses_B35"] for i in metas), "misses_B50": b50(name),
                         "misses_B60": sum(per[name][i]["misses_B60"] for i in metas)}
    titles = {f"T{k + 1}": {"informative": ref[i]["informative"], "labelled_positive": ref[i]["labelled_positive"],
                           "labelled_negative": ref[i]["labelled_negative"],
                           "labelled_er": {n: (round(per[n][i]["labelled_er"], 6) if per[n][i]["labelled_er"] is not None else None)
                                           for n in configs}} for k, i in enumerate(sorted(metas))}
    doc = {"schema": "segarecomp.seg077.t007.results.v1", "task": "SEG-077-T007", "plan_sha256": plan_sha(),
           "models_json_sha256": sha(MODELS_FROZEN), "titles": len(metas), "informative_titles": len(informative),
           "per_title": titles, "summary": summary, "neural_advantage": adv, "equivalent": equiv, "economics": econ_ok,
           "final_class": cls, "data_boundary": "aggregates only; T1..Tn follow the sorted image ids (B077-01..08 order)"}
    pathlib.Path(a.results or BLIND_RESULTS).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"final_class": cls, "informative": len(informative), "summary": {k: v["mean_labelled_er"] for k, v in summary.items()}},
                     indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan").set_defaults(func=lambda a: (print(json.dumps({"plan_sha256": hashlib.sha256(canonical(write_plan())).hexdigest()})), 0)[1])
    sub.add_parser("check").set_defaults(func=lambda a: (verify_plan_full(), print(json.dumps({"plan_sha256": plan_sha(), "check": "PASS"})), 0)[2])
    sub.add_parser("jobs").set_defaults(func=lambda a: ([print(j["id"]) for j in jobs()], 0)[1])
    f = sub.add_parser("fit")
    f.add_argument("--model", required=True, choices=("A", "B", "CV2"))
    f.add_argument("--cond", required=True, choices=("K0", "K1"))
    f.add_argument("--fold", required=True)
    f.add_argument("--seed", type=int)
    f.add_argument("--repro", action="store_true")
    f.set_defaults(func=cmd_fit)
    fm = sub.add_parser("farm")
    fm.add_argument("--only", nargs="*")
    fm.set_defaults(func=cmd_farm)
    sub.add_parser("reuse").set_defaults(func=cmd_reuse)
    tb = sub.add_parser("table")
    tb.add_argument("--results")
    tb.set_defaults(func=cmd_table)
    sub.add_parser("freeze-models").set_defaults(func=cmd_freeze_models)
    bs = sub.add_parser("blind-score")
    bs.add_argument("--key", required=True)
    bs.add_argument("--blind")
    bs.add_argument("--equiv", action="store_true")
    bs.set_defaults(func=cmd_blind_score)
    bt = sub.add_parser("blind-table")
    bt.add_argument("--blind", required=True)
    bt.add_argument("--economics")
    bt.add_argument("--results")
    bt.set_defaults(func=cmd_blind_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
