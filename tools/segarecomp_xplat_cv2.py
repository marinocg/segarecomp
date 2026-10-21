#!/usr/bin/env python3
"""SEG-076-T005 (exploratory, operator-directed, development-only): deterministic C-v2 and a matched multi-seed comparison with A.

C-v2 (segarecomp_xplat_models_v2) is the SEG-075 graph-guided Transformer C with a deterministic, vectorized message-passing
implementation: same function (equal to the frozen C within float rounding on identical weights and batches), same M4 recipe.

  equiv    CPU equivalence evidence: identical weights / real training batches -> logits and every parameter gradient vs the frozen C
  plan     freeze the T005 plan (tools/segarecomp_xplat_cv2.plan.json + .digest) before the first C-v2 fit
  fit      one ROCm fit (C0, a Genesis fold or 'all', seed 1|2|3); write-once, stamped
  farm     the pre-registered jobs: crash-safe single attempts, wall-clock / RSS caps, host-RAM guard
  table    aggregate-only results (tools/segarecomp_xplat_cv2.results.json) with the pre-registered class

This does not reverse SEG-076-T001's pre-registered NEGATIVE_STOP or SEG-076-T002's cancellation (ADR 0165); it is a separately labelled
experiment authorized by the operator after the post-hoc C determinism check failed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import resource
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_explore as rx  # noqa: E402
import segarecomp_xplat_explore_eval as ev6  # noqa: E402

PLAN = HERE / "segarecomp_xplat_cv2.plan.json"
DIGEST = HERE / "segarecomp_xplat_cv2.plan.digest"
RESULTS = HERE / "segarecomp_xplat_cv2.results.json"
EQUIV = HERE / "segarecomp_xplat_cv2.equivalence.json"
SEEDS = (1, 2, 3)
T001 = rx.HARNESS / ".cache" / "seg076" / "t001"
T001_ROCM = rx.HARNESS / ".cache" / "seg076" / "t001-rocm"
BOUND_EXTRA = ("segarecomp_xplat_cv2.py", "segarecomp_xplat_models_v2.py", "segarecomp_xplat_explore.t001.plan.digest",
               "segarecomp_xplat_explore.t001.rocm.plan.digest")
RESOURCES = {"device": "cuda:0 (ROCm/HIP), every C-v2 fit (one backend)", "parallel_fits": 3, "cpu_threads_per_fit": 2,
             "start_stagger_seconds": 30, "wall_clock_cap_minutes": 120, "rss_limit_mb": 4096, "min_available_mb_to_start": 4000,
             "host_memory_floor_mb": 2000, "attempts": 1,
             "infra_rerun": "once, only after 'interrupted' (persisted 'started' found after a host crash) or an external signal kill; never "
                            "after timeout / rss_limit / host_memory_guard / model error",
             "env": {"CUBLAS_WORKSPACE_CONFIG": ":4096:8", "OMP_NUM_THREADS": "2", "MKL_NUM_THREADS": "2"},
             "basis": "pre-freeze probe (150 steps, all-but-D05 Genesis, CPU shared with the T003 farm): 0.33-0.39 s/step, bitwise identical "
                      "same-seed repeat (parameters and inference); 4000 steps ~25 min alone; cap ~4x"}
T_CRIT_2 = 4.303
RULE = {
    "statistic": "d_s = Genesis mean ER (C-v2, seed s) - Genesis mean ER (A); s in 1..3; A is T001's deterministic CPU fits",
    "ci": "two-sided 95% t interval over the 3 seeds (t_0.975,2 = 4.303)",
    "classes": {"C_ADVANTAGE_OVER_A": "interval entirely below 0 AND |mean d| >= 0.01 AND every seed has no more Genesis B50 misses than A",
                "A_ADVANTAGE_OVER_C": "interval entirely above 0 AND |mean d| >= 0.01",
                "NO_MEANINGFUL_DIFFERENCE_BOUNDED": "otherwise, when the whole interval lies within [-0.02, +0.02]",
                "EVIDENCE_INSUFFICIENT": "otherwise, or when any planned C-v2 Genesis fit failed, or the C-v2 determinism refit is not "
                                         "bitwise identical"},
    "descriptive": "per-lineage seed-mean differences with the exact 11-lineage sign-flip p; the 3-seed C-v2 logit-ensemble vs A; per-seed "
                   "worst lineage, B50 misses and boundary-stratum misses; C-v2 seed 1 vs the original C seed 1 (ROCm amendment; same "
                   "function, different summation order and a non-deterministic original); costs (fit / inference time)",
    "synthesis_mapping": "C_ADVANTAGE_OVER_A -> MULTISEED_ADVANTAGE_SUPPORTED_ON_DEVELOPMENT (for C); A_ADVANTAGE_OVER_C or "
                         "NO_MEANINGFUL_DIFFERENCE_BOUNDED -> LIGHTWEIGHT_MODEL_PREFERRED (A is ~150x cheaper per fit); "
                         "EVIDENCE_INSUFFICIENT -> EVIDENCE_INSUFFICIENT",
    "power": "the original C seed-1 draw was 0.011 above A; the D05 non-determinism moved one image's ER by 0.003; with 3 seeds and "
             "t_0.975,2 = 4.303 a between-seed SD of 0.01 gives a half-width of ~0.025, so EVIDENCE_INSUFFICIENT is plausible; stated "
             "before any fit",
}


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def bound_files() -> list[str]:
    return sorted(set(rx.BOUND_CODE) | set(BOUND_EXTRA))


def comparator_files() -> dict[str, pathlib.Path]:
    out = {}
    for f in rx.GENESIS:
        out[f"seg076/A.C0.{f}.npz"] = T001 / "scores" / f"A.C0.{f}.npz"
        out[f"seg076-rocm/C.C0.{f}.s1.npz"] = T001_ROCM / "scores" / f"C.C0.{f}.s1.npz"
    out["seg076/A.C0.all.xplat.npz"] = T001 / "scores" / "A.C0.all.xplat.npz"
    return out


def jobs() -> list[dict]:
    out = [{"id": f"CV2.{f}.s{s}", "fold": f, "seed": s, "dir": "."} for s in SEEDS for f in rx.GENESIS]
    out.append({"id": "CV2.all.s1", "fold": "all", "seed": 1, "dir": "."})
    out.append({"id": "CV2.D05.s1.repro", "fold": "D05", "seed": 1, "dir": "repro"})
    return out


def rocm_identity() -> dict:
    import torch
    return {"torch": torch.__version__, "hip": torch.version.hip, "device": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            "python": platform.python_version()}


def build_plan(identity: dict) -> dict:
    rx.verify_plan()
    if not EQUIV.is_file():
        raise SystemExit("equivalence evidence missing: run `equiv` (CPU) and commit it first")
    return {"schema": "segarecomp.xplat_cv2.plan.v1", "task": "SEG-076-T005", "status": "exploratory, operator-directed, development-only",
            "bound_code": {n: sha(HERE / n) for n in bound_files()}, "t001_plan_sha256": rx.plan_sha(),
            "corpus_files": rx.corpus_manifest(), "comparator_scores": {k: sha(v) for k, v in comparator_files().items()},
            "equivalence_sha256": sha(EQUIV), "rocm_environment": identity, "condition": "C0", "seeds": list(SEEDS),
            "jobs": [j["id"] for j in jobs()], "rule": RULE, "resources": RESOURCES,
            "relation": "does not reverse T001's NEGATIVE_STOP or T002's cancellation; operator-directed after the post-hoc C ROCm determinism "
                        "failure (D05 ER 0.2053 vs 0.2086)",
            "outputs": {"private": "<HARNESS_ROOT>/.cache/seg076/t005", "public": "tools/segarecomp_xplat_cv2.results.json"}}


def write_plan() -> dict:
    doc = build_plan(rocm_identity())
    PLAN.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    DIGEST.write_text(hashlib.sha256(rx.canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan(require_device: bool = False) -> dict:
    rx.verify_plan()
    if not (PLAN.is_file() and DIGEST.is_file()):
        raise SystemExit("T005 plan missing: no C-v2 fit before the reviewed, committed plan")
    doc = json.loads(PLAN.read_text())
    if hashlib.sha256(rx.canonical(doc)).hexdigest() != DIGEST.read_text().strip():
        raise SystemExit("T005 plan digest mismatch")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the code bound by the T005 plan")
    if sha(EQUIV) != doc["equivalence_sha256"]:
        raise SystemExit("equivalence evidence differs from the plan")
    files = comparator_files()
    for k, want in doc["comparator_scores"].items():
        if sha(files[k]) != want:
            raise SystemExit(f"comparator {k} differs from its bound digest")
    if rx.corpus_manifest() != doc["corpus_files"]:
        raise SystemExit("corpus differs from the T005 plan")
    if require_device:
        import torch
        if not torch.cuda.is_available():
            raise SystemExit("no ROCm device: C-v2 fits never fall back to the CPU")
        if rocm_identity() != doc["rocm_environment"]:
            raise SystemExit(f"ROCm environment differs from the plan: {rocm_identity()}")
    return doc


def plan_sha() -> str:
    return DIGEST.read_text().strip()


# ----------------------------------------------------------------------------------------------------------------------- equivalence
def cmd_equiv(a) -> int:
    """CPU: identical weights (the frozen C's state_dict loaded into C-v2) and real training batches (fold D05's training set, seeded
    sampling) -> max |logit difference| and max relative gradient difference per parameter. Pass iff logits <= 1e-5 and gradients
    <= 1e-4 relative in every batch."""
    import numpy as np
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    import segarecomp_xplat_models_v2 as v2
    rx.verify_plan()
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)
    dirs, _, _ = rx.xt().fold_sets(rx.design().verify_plan(), "C0", "D05", rx.snapshot_dir())
    ims = rx.load_dirs(dirs)
    torch.manual_seed(1)
    c = xm.build_bc(torch, True)
    n2 = v2.build_cv2(torch)
    n2.load_state_dict(c.state_dict())
    rng = np.random.default_rng(3)
    M4 = t.M4
    q, central = M4["start_quantum"], M4["central"]
    rows = []
    for _trial in range(a.batches):
        hs, ls, ts, ys, rs, es = [], [], [], [], [], []
        for _ in range(M4["batch"]):
            im = ims[int(rng.integers(0, len(ims)))]
            lo_q, hi_q = -(central // 2) // q, (im.n_words - central // 2) // q
            start = int(rng.integers(lo_q, max(lo_q, hi_q) + 1)) * q
            h, l, tk, y = t.m4_window(np, im, start, central)
            hs.append(h), ls.append(l), ts.append(tk), ys.append(y)
            r, e = xm.window_graph(np, torch, im, start, central)
            rs.append(r), es.append(e)
        hb, lb, tb, yb = (torch.from_numpy(np.stack(x)) for x in (hs, ls, ts, ys))
        gi = (torch.from_numpy(np.stack(rs)), es)
        out = {}
        for name, net in (("c", c), ("v2", n2)):
            net.zero_grad()
            lg = net(hb, lb, tb, gi)
            mask = yb >= 0
            torch.nn.functional.binary_cross_entropy_with_logits(lg[mask], yb[mask].float()).backward()
            out[name] = (lg.detach().clone(), {k: p.grad.detach().clone() for k, p in net.named_parameters()})
        dl = float((out["c"][0] - out["v2"][0]).abs().max())
        gd = max(float((out["c"][1][k] - out["v2"][1][k]).abs().max()) / (float(out["c"][1][k].abs().max()) + 1e-30) for k in out["c"][1])
        rows.append({"logit_max_abs_diff": dl, "logit_scale": round(float(out["c"][0].abs().max()), 4), "grad_max_rel_diff": gd})
    ok = all(r["logit_max_abs_diff"] <= 1e-5 and r["grad_max_rel_diff"] <= 1e-4 for r in rows)
    doc = {"schema": "segarecomp.xplat_cv2.equivalence.v1", "device": "cpu", "torch": torch.__version__, "batches": rows,
           "tolerance": {"logit_abs": 1e-5, "grad_rel": 1e-4}, "pass": ok,
           "rocm_probe": a.rocm_probe and json.loads(a.rocm_probe)}
    EQUIV.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"pass": ok, "max_logit": max(r["logit_max_abs_diff"] for r in rows), "max_grad": max(r["grad_max_rel_diff"] for r in rows)}))
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------------------------------------- fit
def score_path(out: pathlib.Path, fold: str, seed: int) -> pathlib.Path:
    return out / "scores" / (f"CV2.C0.{fold}.s{seed}" + (".xplat" if fold == "all" else "") + ".npz")


def cmd_fit(a) -> int:
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models_v2 as v2
    verify_plan(require_device=True)
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    st = (out.parent if out.name == "repro" else out) / "farm.json"
    if not st.is_file() or json.loads(st.read_text()).get("plan_sha256") != plan_sha():
        raise SystemExit(f"{out}: not a T005 out dir")
    if not any((j["fold"], j["seed"]) == (a.fold, a.seed) and (out.name == "repro") == (j["dir"] == "repro") for j in jobs()):
        raise SystemExit("not a pre-registered T005 fit")
    path = score_path(out, a.fold, a.seed)
    if path.exists() or path.with_suffix(".json").exists():
        raise SystemExit(f"{path.name}: already exists; fits are write-once")
    dev = torch.device("cuda:0")
    train_dirs, held_dirs, held = rx.xt().fold_sets(rx.design().verify_plan(), "C0", a.fold, rx.snapshot_dir())
    train, held_imgs = rx.load_dirs(train_dirs), rx.load_dirs(held_dirs)
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    if any(im.id.startswith("xp/") for im in train):
        raise SystemExit("C0 training must be Genesis-only")
    w0, c0 = time.time(), time.process_time()
    net = v2.fit_cv2(train, a.seed, dev, lambda m: print(f"[CV2/C0/{a.fold}/s{a.seed}] {m}", flush=True))
    torch.cuda.synchronize()
    fit_wall, fit_cpu = time.time() - w0, time.process_time() - c0
    scores, infer = {}, {}
    for im in held_imgs:
        t1 = time.time()
        scores[im.id] = v2.infer_cv2(net, im, dev)
        infer[im.id] = round(time.time() - t1, 3)
    path.parent.mkdir(parents=True, exist_ok=True)
    t.save_scores(path, scores)
    if a.fold == "all":
        torch.save({k: v.cpu() for k, v in net.state_dict().items()}, path.with_suffix(".pt"))
    commit = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    rec = {"task": "SEG-076-T005", "model": "CV2", "condition": "C0", "fold": a.fold, "seed": a.seed, "plan_sha256": plan_sha(),
           "product_commit": commit, "device": "rocm", "fit_wall_seconds": round(fit_wall, 1), "fit_cpu_seconds": round(fit_cpu, 1),
           "inference_seconds": {"images": len(infer), "total": round(sum(infer.values()), 2), "max": max(infer.values(), default=0.0)},
           "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024, "train_images": sorted(im.id for im in train),
           "held_images": sorted(im.id for im in held_imgs), "environment": dict(rocm_identity(), os=platform.platform())}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("fold", "seed", "fit_wall_seconds", "peak_rss_mb")}), flush=True)
    return 0


def cmd_farm(a) -> int:
    verify_plan(require_device=True)
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    (out / "logs").mkdir(parents=True, exist_ok=True)
    state_p = out / "farm.json"
    state = json.loads(state_p.read_text()) if state_p.is_file() else {"plan_sha256": plan_sha(), "jobs": {}}
    if state.get("plan_sha256") != plan_sha():
        raise SystemExit("farm state belongs to a different T005 plan")

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
    env = dict(os.environ, **RESOURCES["env"])
    running, stall, next_start = {}, None, 0.0
    while todo or running:
        avail = rx._available_mb()
        if running and avail < RESOURCES["host_memory_floor_mb"]:
            jid = max(running, key=lambda k: running[k][1])
            p, t0, j, log, peak = running.pop(jid)
            p.kill(), p.wait(), log.close()
            state["jobs"][jid].update(status="host_memory_guard", wall_seconds=round(time.time() - t0, 1), mem_available_mb=avail)
            save()
            continue
        if todo and not running and avail < RESOURCES["min_available_mb_to_start"]:
            stall = stall or time.time()
            if time.time() - stall > 1800:
                save()
                raise SystemExit("farm stalled: available memory below the start threshold for 30 min with nothing running")
        else:
            stall = None
        if todo and len(running) < RESOURCES["parallel_fits"] and avail >= RESOURCES["min_available_mb_to_start"] and time.time() >= next_start:
            next_start = time.time() + RESOURCES["start_stagger_seconds"]
            j = todo.pop(0)
            attempt = state["jobs"].get(j["id"], {}).get("attempts", 0) + 1
            state["jobs"][j["id"]] = dict(state["jobs"].get(j["id"], {}), status="started", attempts=attempt)
            save()
            cmd = [sys.executable, "-I", str(HERE / "segarecomp_xplat_cv2.py"), "fit", "--fold", j["fold"], "--seed", str(j["seed"]),
                   "--out", str(out / j["dir"])]
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
            elif time.time() - t0 > RESOURCES["wall_clock_cap_minutes"] * 60:
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


# -------------------------------------------------------------------------------------------------------------------------- rules
def t_interval(xs: list[float], t_crit: float = T_CRIT_2) -> dict:
    n = len(xs)
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1)) if n > 1 else float("inf")
    h = t_crit * sd / math.sqrt(n)
    return {"mean": round(m, 6), "sd": round(sd, 6), "lo": round(m - h, 6), "hi": round(m + h, 6), "half_width": round(h, 6), "n": n}


def cv2_class(d_seed: list[float], seed_b50: list[int], a_b50: int, complete: bool, deterministic) -> dict:
    """Pre-registered T005 rule (pure; unit-tested)."""
    if not complete or deterministic is not True:
        return {"class": "EVIDENCE_INSUFFICIENT", "reason": "failed / missing fits" if not complete else "determinism refit not bitwise identical"}
    ci = t_interval(d_seed)
    if ci["hi"] < 0 and abs(ci["mean"]) >= 0.01 and all(b <= a_b50 for b in seed_b50):
        cls = "C_ADVANTAGE_OVER_A"
    elif ci["lo"] > 0 and abs(ci["mean"]) >= 0.01:
        cls = "A_ADVANTAGE_OVER_C"
    elif ci["lo"] >= -0.02 and ci["hi"] <= 0.02:
        cls = "NO_MEANINGFUL_DIFFERENCE_BOUNDED"
    else:
        cls = "EVIDENCE_INSUFFICIENT"
    return {"class": cls, "t_interval": ci}


# -------------------------------------------------------------------------------------------------------------------------- table
def cmd_table(a) -> int:
    import numpy as np
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_stability as st
    verify_plan()
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    farm = json.loads((out / "farm.json").read_text())
    if farm.get("plan_sha256") != plan_sha():
        raise SystemExit("farm state belongs to a different plan")
    status = {k: v["status"] for k, v in farm["jobs"].items()}
    meta, classes = st.genesis_meta(), {}
    X, files = rx.xt(), comparator_files()
    tables, missing = {}, []

    def add(name, sc):
        rows, s, per = st.lineage_table(sc, meta, classes)
        tables[name] = {"genesis": rows, "summary": s}

    def cv2_scores(fold, seed):
        jid = f"CV2.{fold}.s{seed}"
        if status.get(jid) != "ok":
            return None
        p = score_path(out, fold, seed)
        r = json.loads(p.with_suffix(".json").read_text())
        if r.get("plan_sha256") != plan_sha() or (r["fold"], r["seed"], r["device"]) != (fold, seed, "rocm"):
            raise SystemExit(f"{p.name}: record does not match")
        return {iid: v for iid, v in X.load_npz(p).items()}

    def cells(words):
        return {iid: t.cell_max(np.asarray(w, dtype=np.float32), meta[iid]["size"], 128) for iid, w in words.items()}

    add("A", {f: {k: np.asarray(v, dtype=np.float64) for k, v in X.load_npz(files[f"seg076/A.C0.{f}.npz"]).items()} for f in rx.GENESIS})
    add("C_orig_rocm_s1", {f: cells(X.load_npz(files[f"seg076-rocm/C.C0.{f}.s1.npz"])) for f in rx.GENESIS})
    words = {}
    for s in SEEDS:
        per = {}
        for f in rx.GENESIS:
            w = cv2_scores(f, s)
            if w is None:
                missing.append(f"CV2.{f}.s{s}")
            else:
                per[f] = w
        if len(per) == len(rx.GENESIS):
            words[s] = per
            add(f"CV2.s{s}", {f: cells(w) for f, w in per.items()})
    complete = len(words) == len(SEEDS)
    if complete:
        add("CV2.ens3", {f: cells({iid: np.mean([words[s][f][iid] for s in SEEDS], axis=0) for iid in words[1][f]}) for f in rx.GENESIS})
    rp = score_path(out / "repro", "D05", 1)
    if status.get("CV2.D05.s1.repro") == "ok" and status.get("CV2.D05.s1") == "ok":
        x1, x2 = X.load_npz(score_path(out, "D05", 1)), X.load_npz(rp)
        deterministic = bool(sorted(x1) == sorted(x2) and all(np.array_equal(x1[k], x2[k]) for k in x1))
    else:
        deterministic = None
    a_sum = tables["A"]["summary"]
    d_seed = [tables[f"CV2.s{s}"]["summary"]["mean_er"] - a_sum["mean_er"] for s in SEEDS] if complete else []
    decision = cv2_class(d_seed, [tables[f"CV2.s{s}"]["summary"]["misses_B50"] for s in SEEDS] if complete else [],
                         a_sum["misses_B50"], complete, deterministic)
    desc = {}
    if complete:
        dL = {L: sum(tables[f"CV2.s{s}"]["genesis"][L]["max_er"] - tables["A"]["genesis"][L]["max_er"] for s in SEEDS) / len(SEEDS)
              for L in rx.GENESIS}
        desc = {"per_seed_d_mean_er": [round(x, 6) for x in d_seed], "per_lineage_seed_mean_d": {L: round(v, 6) for L, v in dL.items()},
                "lineage_sign_flip_p": round(st.sign_flip_p(list(dL.values())), 6),
                "ens3_minus_a_mean_er": round(tables["CV2.ens3"]["summary"]["mean_er"] - a_sum["mean_er"], 6),
                "cv2_s1_minus_orig_c_s1_mean_er": round(tables["CV2.s1"]["summary"]["mean_er"] - tables["C_orig_rocm_s1"]["summary"]["mean_er"], 6)}
    recs = [json.loads(score_path(out, j["fold"], j["seed"]).with_suffix(".json").read_text()) for j in jobs()
            if j["dir"] == "." and status.get(j["id"]) == "ok"]
    costs = {"fits": len(recs), "fit_wall_seconds_mean": round(sum(r["fit_wall_seconds"] for r in recs) / len(recs), 1) if recs else None,
             "fit_cpu_hours_total": round(sum(r["fit_cpu_seconds"] for r in recs) / 3600, 2) if recs else None,
             "inference_seconds_per_image_mean": round(sum(r["inference_seconds"]["total"] for r in recs) /
                                                       max(1, sum(r["inference_seconds"]["images"] for r in recs)), 3) if recs else None}
    compact = {k: {"summary": v["summary"], "lineage_max_er": {L: r["max_er"] for L, r in v["genesis"].items()},
                   "lineage_b50_misses": {L: r["misses_B50"] for L, r in v["genesis"].items()}} for k, v in tables.items()}
    doc = {"schema": "segarecomp.xplat_cv2.results.v1", "task": "SEG-076-T005", "plan_sha256": plan_sha(),
           "status": "exploratory, operator-directed, development-only", "farm": dict(sorted(status.items())), "missing": missing,
           "determinism_bitwise_d05_s1": deterministic, "decision": decision, "descriptive": desc, "costs": costs, "configurations": compact,
           "synthesis_mapping": RULE["synthesis_mapping"], "data_boundary": "aggregates only"}
    target = pathlib.Path(a.results) if a.results else RESULTS
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"decision": decision, "descriptive": desc}, indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("equiv")
    e.add_argument("--batches", type=int, default=4)
    e.add_argument("--rocm-probe", help="JSON of the ROCm same-seed probe (recorded verbatim)")
    e.set_defaults(func=cmd_equiv)
    sub.add_parser("plan").set_defaults(func=lambda a: (print(json.dumps({"plan_sha256": hashlib.sha256(rx.canonical(write_plan())).hexdigest()})), 0)[1])
    sub.add_parser("check").set_defaults(func=lambda a: (verify_plan(), print(json.dumps({"plan_sha256": plan_sha(), "check": "PASS"})), 0)[2])
    sub.add_parser("jobs").set_defaults(func=lambda a: ([print(j["id"]) for j in jobs()], 0)[1])
    f = sub.add_parser("fit")
    f.add_argument("--fold", required=True)
    f.add_argument("--seed", type=int, required=True)
    f.add_argument("--out", required=True)
    f.set_defaults(func=cmd_fit)
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
