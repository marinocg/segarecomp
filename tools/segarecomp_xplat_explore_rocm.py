#!/usr/bin/env python3
"""SEG-076-T001 resource amendment (exploratory, development-only): B and C on ROCm after C could not complete under the frozen CPU caps.

Why: under the frozen T001 farm (6 CPU fits x 2 threads on an 8-core / 16-thread host) C ran at ~5.5 s/step against 0.8 s/step in the
pre-freeze probe, so every C fit hit its 160-minute cap at ~1500 of 4000 steps. Under the frozen plan C is therefore 'failed' and does
not qualify; that pre-registered result is NOT changed by this amendment. This amendment only changes the DEVICE: identical code paths,
network, M4 recipe, steps, seed and folds, trained on the ROCm GPU, with B also refitted on ROCm as a device control (device-matched B vs C
contrast, and B CPU vs B ROCm = the device effect).

  plan      write the amendment plan (tools/segarecomp_xplat_explore.t001.rocm.plan.json + .digest) binding the T001 plan digest, this file,
            the ROCm environment identity, jobs, caps and the reporting rule; committed and reviewed BEFORE any amendment fit
  selftest  CPU equivalence: fit_bc_device(cpu) is bitwise identical to the frozen segarecomp_xplat_models.fit_bc for a few steps (B, C)
  fit       one ROCm fit (--model B|C --fold <lineage>|all), write-once, stamped with the T001 AND amendment digests
  farm      the amendment jobs with the frozen parallelism / caps, one attempt each (same failure semantics as T001)
  views     build two read-only score views (symlinks + a farm.json derived from the real farm states) so the UNCHANGED T001 `table`
            evaluates: (1) 'rocm_bc' = A (CPU) + B (ROCm) + C (ROCm); (2) 'mixed' = A (CPU) + B (CPU) + C (ROCm)
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
import segarecomp_xplat_explore as rx  # noqa: E402

PLAN = HERE / "segarecomp_xplat_explore.t001.rocm.plan.json"
DIGEST = HERE / "segarecomp_xplat_explore.t001.rocm.plan.digest"
RESOURCES = {"device": "cuda:0 (ROCm/HIP)", "parallel_fits": 4, "cpu_threads_per_fit": 2,
             "host_protection": "two host (WSL2) crashes occurred when 3 ROCm probes plus a CPU self-test were started on top of 6 running "
                                "CPU fits (~11.5 GB RSS on a 16 GB host). Protection is a host-RAM guard, not serialization: a fit starts "
                                "only when MemAvailable >= 4000 MB (starts staggered by 20 s), and when MemAvailable < 2000 MB the most "
                                "recently started fit is killed and recorded 'host_memory_guard' (failed, not re-runnable)",
             "min_available_mb_to_start": 4000, "host_memory_floor_mb": 2000,
             "wall_clock_cap_minutes": {"B": 30, "C": 180},
             "caps_basis": "uncontended single-process ROCm probe: B 0.038 s/step (~2.5 min per 4000 steps), C 0.54 s/step (~36 min); C's "
                           "edge building is CPU-bound, so 4 concurrent fits may each slow down; caps are set generously (C 5x) so a "
                           "resource cap does not decide the result again (new caps chosen after the CPU failure; disclosed)",
             "rss_limit_mb": 4096, "attempts": 1,
             "env": {"CUBLAS_WORKSPACE_CONFIG": ":4096:8", "OMP_NUM_THREADS": "2", "MKL_NUM_THREADS": "2"}}
JOB_MODELS = ("B", "C")


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def rocm_identity() -> dict:
    import torch
    return {"torch": torch.__version__, "hip": torch.version.hip, "device": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            "python": platform.python_version(), "numpy": __import__("numpy").__version__}


def jobs() -> list[dict]:
    out = [{"id": f"{m}.{f}", "model": m, "fold": f, "dir": "."} for m in JOB_MODELS for f in rx.GENESIS + ("all",)]
    out += [{"id": f"{m}.{rx.DETERMINISM_FOLD}.repro", "model": m, "fold": rx.DETERMINISM_FOLD, "dir": "repro"} for m in JOB_MODELS]
    return out


def build_plan(identity: dict) -> dict:
    return {"schema": "segarecomp.xplat_explore.t001.rocm_amendment.v1", "task": "SEG-076-T001", "kind": "resource-only amendment",
            "t001_plan_sha256": rx.plan_sha(), "bound_code": {n: sha(HERE / n) for n in ("segarecomp_xplat_explore_rocm.py",)},
            "rocm_environment": identity, "resources": RESOURCES, "seed": rx.PILOT_SEED, "jobs": [j["id"] for j in jobs()],
            "trigger": "every planned C fit exceeded the frozen 160-min CPU cap (C ~5.5 s/step under the 6-fit CPU farm vs 0.8 s/step in "
                       "the probe); C is 'failed' under the frozen plan and that pre-registered T001 class is unchanged",
            "unchanged": "network (segarecomp_xplat_models.build_bc), M4 recipe, 4000 steps, seed 1, folds, data, metrics, expansion rule; "
                         "fit_bc_device is a line-for-line copy of the frozen fit_bc with tensors moved to the device, proven bitwise "
                         "identical to fit_bc on CPU by `selftest`",
            "device_control": "B is refitted on ROCm on every fold: B(ROCm) vs B(CPU) measures the device effect; B(ROCm) vs C(ROCm) is the "
                              "device-matched architecture contrast",
            "reporting": "the unchanged T001 table and rule are evaluated on two views: 'rocm_bc' (A CPU, B ROCm, C ROCm) and 'mixed' "
                         "(A CPU, B CPU, C ROCm). Both are reported as AMENDMENT results next to the pre-registered class (C failed under "
                         "the CPU caps), never replacing it. A C that qualifies only under the amendment is reported as such; SEG-076-T002 "
                         "admission follows the pre-registered class",
            "disclosure": "frozen after the CPU A and B results were visible (A Genesis mean ER 0.105, B 0.155); no C score existed. The "
                          "amendment changes no threshold and no model setting",
            "selftest": "fit_bc_device / infer_bc_device on the CPU are bitwise identical to the frozen fit_bc / infer_bc (6 steps on the "
                        "D05-fold training set, B and C: parameters and inference)",
            "cpu_determinism_command": "segarecomp_xplat_explore.py farm --out <t001> --only A.D05.repro B.D05.repro (the absent C CPU "
                                       "jobs are deliberately not re-run)",
            "cpu_farm_interruption": "the T001 CPU farm was interrupted by a host crash while C.D07, C.D09-C.D12 and C.all were ~40 min "
                                     "into their runs (C.D01-C.D06 had already hit the 160-min cap at ~1500/4000 steps). They are not "
                                     "re-run on the CPU: they are absent from the CPU farm state, which the frozen table counts as "
                                     "failed. Only the planned A and B D05 determinism refits are completed on the CPU",
            "outputs": "<HARNESS_ROOT>/.cache/seg076/t001-rocm (private)"}


def write_plan() -> dict:
    doc = build_plan(rocm_identity())
    PLAN.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    DIGEST.write_text(hashlib.sha256(rx.canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan(require_device: bool = False) -> dict:
    rx.verify_plan()
    if not (PLAN.is_file() and DIGEST.is_file()):
        raise SystemExit("ROCm amendment plan missing: no amendment fit before the reviewed, committed amendment")
    doc = json.loads(PLAN.read_text())
    if hashlib.sha256(rx.canonical(doc)).hexdigest() != DIGEST.read_text().strip():
        raise SystemExit("amendment plan digest mismatch")
    if doc["t001_plan_sha256"] != rx.plan_sha():
        raise SystemExit("amendment bound to a different T001 plan")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the amendment binding")
    if require_device:
        import torch
        if not torch.cuda.is_available():
            raise SystemExit("no ROCm device: the amendment never falls back to the CPU")
        if rocm_identity() != doc["rocm_environment"]:
            raise SystemExit(f"ROCm environment differs from the amendment plan: {rocm_identity()}")
    return doc


def amend_sha() -> str:
    return DIGEST.read_text().strip()


# ------------------------------------------------------------------------------------------------------------- device-aware fit
def fit_bc_device(images: list, seed: int, graph: bool, device, log=None):
    """Line-for-line copy of segarecomp_xplat_models.fit_bc (frozen) with the network and every batch tensor on `device`."""
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    np = xm.np_()
    M4 = t.M4
    torch.set_num_threads(M4["threads"])
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = xm.build_bc(torch, graph).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=M4["lr"], weight_decay=M4["weight_decay"])
    pos_weight = t.m4_pos_weight(np, images)
    lineages = sorted({im.lineage for im in images})
    by_lin = {L: [im for im in images if im.lineage == L] for L in lineages}
    sizes = {L: np.array([im.n_words for im in by_lin[L]], dtype=np.float64) for L in lineages}
    q, central, total = M4["start_quantum"], M4["central"], M4["steps"]
    model.train()
    t0 = time.time()
    for step in range(total):
        for gr in opt.param_groups:
            gr["lr"] = M4["lr"] * (M4["lr_drop"] if step >= int(M4["lr_drop_at"] * total) else 1.0)
        hs, ls, ts, ys, rs, es = [], [], [], [], [], []
        for _ in range(M4["batch"]):
            L = lineages[int(rng.integers(0, len(lineages)))]
            im = by_lin[L][int(rng.choice(len(by_lin[L]), p=sizes[L] / sizes[L].sum()))]
            lo_q, hi_q = -(central // 2) // q, (im.n_words - central // 2) // q
            start = int(rng.integers(lo_q, max(lo_q, hi_q) + 1)) * q
            h, l, tk, y = t.m4_window(np, im, start, central)
            hs.append(h), ls.append(l), ts.append(tk), ys.append(y)
            if graph:
                r, e = xm.window_graph(np, torch, im, start, central)
                rs.append(r), es.append([(a.to(device), b.to(device)) for a, b in e])
        hb, lb, tb, yb = (torch.from_numpy(np.stack(a)).to(device) for a in (hs, ls, ts, ys))
        gi = (torch.from_numpy(np.stack(rs)).to(device), es) if graph else None
        logits = model(hb, lb, tb, gi)
        mask = yb >= 0
        if not bool(mask.any()):
            continue
        lg, tgt = logits[mask], yb[mask].float()
        one = torch.ones((), device=device)
        wgt = torch.where(tgt > 0.5, one * pos_weight, one)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(lg, tgt, reduction="none") * wgt).sum() / wgt.sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
        if log and (step + 1) % 500 == 0:
            log(f"step {step + 1}/{total} loss {float(loss):.5f} t={time.time() - t0:.0f}s")
    model.eval()
    return model


def infer_bc_device(model, im, graph: bool, device):
    """Copy of segarecomp_xplat_models.infer_bc with inputs on `device` (outputs returned on the CPU as float32)."""
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    np = xm.np_()
    out = np.zeros(im.n_words, dtype=np.float32)
    c = t.M4["infer_central"]
    with torch.no_grad():
        for start in range(0, im.n_words, c):
            h, l, tk, _ = t.m4_window(np, im, start, c)
            gi = None
            if graph:
                r, e = xm.window_graph(np, torch, im, start, c)
                gi = (torch.from_numpy(r[None]).to(device), [[(a.to(device), b.to(device)) for a, b in e]])
            lg = model(torch.from_numpy(h[None]).to(device), torch.from_numpy(l[None]).to(device), torch.from_numpy(tk[None]).to(device),
                       gi)[0].float().cpu().numpy()
            k = min(c, im.n_words - start)
            out[start:start + k] = lg[:k]
    return out


def cmd_selftest(a) -> int:
    """fit_bc_device on the CPU must equal the frozen fit_bc bitwise (parameters and inference) for a short run on real training data."""
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    rx.verify_plan()
    train_dirs, held_dirs, _ = rx.xt().fold_sets(rx.design().verify_plan(), "C0", "D05", rx.snapshot_dir())
    train, held = rx.load_dirs(train_dirs), rx.load_dirs(held_dirs)
    saved = t.M4
    t.M4 = dict(saved, steps=a.steps)
    res = {}
    try:
        for model in ("B", "C"):
            g = model == "C"
            n1 = xm.fit_bc(train, 1, g)
            n2 = fit_bc_device(train, 1, g, torch.device("cpu"))
            same = all(torch.equal(p, q) for p, q in zip(n1.state_dict().values(), n2.state_dict().values()))
            s1, s2 = xm.infer_bc(n1, held[0], g), infer_bc_device(n2, held[0], g, torch.device("cpu"))
            res[model] = {"parameters_bitwise": bool(same), "inference_bitwise": bool((s1 == s2).all())}
    finally:
        t.M4 = saved
    print(json.dumps({"steps": a.steps, **res}))
    return 0 if all(v["parameters_bitwise"] and v["inference_bitwise"] for v in res.values()) else 1


def score_path(out: pathlib.Path, model: str, fold: str) -> pathlib.Path:
    return rx.score_path(out, model, fold, rx.PILOT_SEED)


AMEND_RESULTS = HERE / "segarecomp_xplat_explore.t001.rocm.results.json"
TERMINAL_RERUNNABLE = ("interrupted", "killed_signal")


def _farm_root(out: pathlib.Path) -> pathlib.Path:
    return out.parent if out.name == "repro" else out


def _require_amendment_out(out: pathlib.Path) -> None:
    """Amendment fits write only into an out dir whose farm state belongs to THIS amendment (never into the CPU T001 tree)."""
    st = _farm_root(out) / "farm.json"
    if not st.is_file() or json.loads(st.read_text()).get("amendment_sha256") != amend_sha():
        raise SystemExit(f"{out}: not an amendment out dir (farm.json missing or bound to another amendment)")


def cmd_fit(a) -> int:
    import torch
    import segarecomp_ml_v3_train as t
    verify_plan(require_device=True)
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    _require_amendment_out(out)
    model, fold = a.model, a.fold
    if model not in JOB_MODELS or (fold != "all" and fold not in rx.GENESIS):
        raise SystemExit("amendment fits: B|C on a Genesis lineage or 'all'")
    path = score_path(out, model, fold)
    if path.exists() or path.with_suffix(".json").exists():
        raise SystemExit(f"{path.name}: already exists; fits are write-once")
    dev = torch.device("cuda:0")
    train_dirs, held_dirs, held = rx.xt().fold_sets(rx.design().verify_plan(), "C0", fold, rx.snapshot_dir())
    train, held_imgs = rx.load_dirs(train_dirs), rx.load_dirs(held_dirs)
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    if any(im.id.startswith("xp/") for im in train):
        raise SystemExit("C0 training must be Genesis-only")
    w0, c0 = time.time(), time.process_time()
    net = fit_bc_device(train, rx.PILOT_SEED, model == "C", dev, lambda m: print(f"[{model}/C0/{fold}/rocm] {m}", flush=True))
    torch.cuda.synchronize()
    fit_wall, fit_cpu = time.time() - w0, time.process_time() - c0
    scores, infer = {}, {}
    for im in held_imgs:
        t1 = time.time()
        scores[im.id] = infer_bc_device(net, im, model == "C", dev)
        infer[im.id] = round(time.time() - t1, 3)
    path.parent.mkdir(parents=True, exist_ok=True)
    t.save_scores(path, scores)
    if fold == "all":
        torch.save({k: v.cpu() for k, v in net.state_dict().items()}, path.with_suffix(".pt"))
    commit = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    rec = {"task": "SEG-076-T001", "amendment": "rocm", "model": model, "condition": "C0", "fold": fold, "seed": rx.PILOT_SEED,
           "plan_sha256": rx.plan_sha(), "amendment_sha256": amend_sha(), "product_commit": commit, "device": "rocm",
           "fit_wall_seconds": round(fit_wall, 1), "fit_cpu_seconds": round(fit_cpu, 1),
           "inference_seconds": {"images": len(infer), "total": round(sum(infer.values()), 2), "max": max(infer.values(), default=0.0)},
           "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024, "train_images": sorted(im.id for im in train),
           "held_images": sorted(im.id for im in held_imgs), "train_lineages": sorted({im.lineage for im in train}),
           "environment": dict(rocm_identity(), os=platform.platform(), cpu_count=os.cpu_count())}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "fold", "fit_wall_seconds", "peak_rss_mb")}), flush=True)
    return 0


def cmd_farm(a) -> int:
    """T001 farm semantics for the amendment jobs, plus crash-safe attempt accounting and a host-RAM guard:
    - a job is persisted as 'started' BEFORE launch; a leftover 'started' at farm start (host crash) becomes 'interrupted' (failed);
    - 'interrupted' / 'killed_signal*' may be re-run once, explicitly (--infra-rerun JOB --reason), identical configuration; never a
      timeout, rss_limit, host_memory_guard or error; the earlier attempt's partial outputs make the write-once fit refuse (disclosed);
    - logs are per attempt; new fits start only when MemAvailable >= the start threshold; when MemAvailable falls below the floor the
      most recently started fit is killed and recorded 'host_memory_guard' (failed) to protect the host."""
    verify_plan(require_device=True)
    out = pathlib.Path(a.out).resolve()
    rx.guard([out])
    (out / "logs").mkdir(parents=True, exist_ok=True)
    state_p = out / "farm.json"
    state = json.loads(state_p.read_text()) if state_p.is_file() else {"plan_sha256": rx.plan_sha(), "amendment_sha256": amend_sha(), "jobs": {}}
    if state.get("amendment_sha256") != amend_sha() or state.get("plan_sha256") != rx.plan_sha():
        raise SystemExit("farm state belongs to a different amendment / plan")

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
    for jid in a.infra_rerun or []:
        prev = state["jobs"].get(jid)
        if not prev or prev.get("attempts", 1) >= 2 or not prev["status"].startswith(TERMINAL_RERUNNABLE) or not a.reason:
            raise SystemExit(f"{jid}: infra re-run refused (only once, only after interrupted / killed_signal, --reason required)")
        prev["infra_rerun_reason"] = a.reason
        todo.append(by_id[jid])
    running = {}
    env = dict(os.environ, **RESOURCES["env"])
    stall = None
    while todo or running:
        avail = rx._available_mb()
        if running and avail < RESOURCES["host_memory_floor_mb"]:
            jid = max(running, key=lambda k: running[k][1])
            p, t0, j, log, peak = running.pop(jid)
            p.kill(), p.wait(), log.close()
            state["jobs"][jid].update(status="host_memory_guard", wall_seconds=round(time.time() - t0, 1), peak_rss_mb_watchdog=peak,
                                      mem_available_mb=avail)
            save()
            print(f"done {jid} host_memory_guard ({avail} MB available)", flush=True)
            continue
        if todo and not running and avail < RESOURCES["min_available_mb_to_start"]:
            stall = stall or time.time()
            if time.time() - stall > 1800:
                save()
                raise SystemExit("farm stalled: available memory below the start threshold for 30 min with nothing running")
        else:
            stall = None
        while todo and len(running) < RESOURCES["parallel_fits"] and rx._available_mb() >= RESOURCES["min_available_mb_to_start"]:
            j = todo.pop(0)
            attempt = state["jobs"].get(j["id"], {}).get("attempts", 0) + 1
            state["jobs"][j["id"]] = dict(state["jobs"].get(j["id"], {}), status="started", attempts=attempt)
            save()
            log = open(out / "logs" / f"{j['id']}.a{attempt}.log", "w")
            cmd = [sys.executable, "-I", str(HERE / "segarecomp_xplat_explore_rocm.py"), "fit", "--model", j["model"], "--fold", j["fold"],
                   "--out", str(out / j["dir"])]
            running[j["id"]] = (subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env), time.time(), j, log, 0)
            print(f"start {j['id']} attempt {attempt}", flush=True)
            time.sleep(20)  # stagger corpus loading (RAM peaks) between concurrent fits
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


def _check_record(rec_p: pathlib.Path, rocm_side: bool) -> None:
    r = json.loads(rec_p.read_text())
    if rocm_side and (r.get("device") != "rocm" or r.get("amendment_sha256") != amend_sha()):
        raise SystemExit(f"{rec_p.name}: not a ROCm record of this amendment")
    if not rocm_side and ("device" in r or "amendment_sha256" in r):
        raise SystemExit(f"{rec_p.name}: not a CPU T001 record")


def cmd_views(a) -> int:
    """Two read-only views for the UNCHANGED T001 table (symlinked scores; each view's farm.json copies the real statuses of the farm
    that produced each linked score). Provenance is asserted: the CPU farm is the T001 farm (no amendment stamp), the ROCm farm is this
    amendment's, and every linked record carries the matching device / amendment stamps."""
    verify_plan()
    cpu, rocm, base = (pathlib.Path(x).resolve() for x in (a.cpu, a.rocm, a.views))
    rx.guard([cpu, rocm, base])
    cpu_farm, rocm_farm = json.loads((cpu / "farm.json").read_text()), json.loads((rocm / "farm.json").read_text())
    if cpu_farm.get("plan_sha256") != rx.plan_sha() or "amendment_sha256" in cpu_farm:
        raise SystemExit("--cpu is not the T001 CPU farm")
    if rocm_farm.get("amendment_sha256") != amend_sha() or rocm_farm.get("plan_sha256") != rx.plan_sha():
        raise SystemExit("--rocm is not this amendment's farm")
    for view, src in VIEWS.items():
        v = base / view
        if v.exists():
            raise SystemExit(f"{v}: view exists (views are write-once)")
        (v / "scores").mkdir(parents=True)
        (v / "repro" / "scores").mkdir(parents=True)
        st = {"plan_sha256": rx.plan_sha(), "view": view, "view_of": src, "amendment_sha256_ref": amend_sha(), "jobs": {}}
        for j in rx.jobs():
            side = src[j["model"]]
            root, state = (cpu, cpu_farm["jobs"]) if side == "cpu" else (rocm, rocm_farm["jobs"])
            if j["id"] not in state:
                continue
            st["jobs"][j["id"]] = state[j["id"]]
            sp = rx.score_path(root / j["dir"], j["model"], j["fold"], j["seed"])
            if state[j["id"]]["status"] == "ok":
                _check_record(sp.with_suffix(".json"), side == "rocm")
            for f in (sp, sp.with_suffix(".json")):
                if f.is_file():
                    (v / j["dir"] / "scores" / f.name).symlink_to(f)
        (v / "farm.json").write_text(json.dumps(st, indent=1, sort_keys=True))
        print(f"view {view}: {len(st['jobs'])} jobs")
    return 0


VIEWS = {"rocm_bc": {"A": "cpu", "B": "rocm", "C": "rocm"}, "mixed": {"A": "cpu", "B": "cpu", "C": "rocm"}}


def cmd_table(a) -> int:
    """Evaluate BOTH views with the unchanged T001 table into private files, then write ONE labelled, write-once amendment document
    (never the pre-registered results path). Both views are always reported."""
    verify_plan()
    base, priv = pathlib.Path(a.views).resolve(), pathlib.Path(a.private).resolve()
    rx.guard([base, priv])
    target = pathlib.Path(a.results).resolve() if a.results else AMEND_RESULTS
    if target == rx.RESULTS.resolve():
        raise SystemExit("refused: the pre-registered T001 results path is not an amendment output")
    if target.exists():
        raise SystemExit(f"{target.name}: exists (the amendment document is write-once)")
    priv.mkdir(parents=True, exist_ok=True)
    docs = {}
    for view in VIEWS:
        r = priv / f"t001-rocm-view-{view}.json"
        if r.exists():
            raise SystemExit(f"{r}: exists (write-once)")
        rx.main(["table", "--out", str(base / view), "--results", str(r)])
        docs[view] = json.loads(r.read_text())
    doc = {"schema": "segarecomp.xplat_explore.t001.rocm_amendment.results.v1", "task": "SEG-076-T001",
           "label": "AMENDMENT - not the pre-registered class (resource-only ROCm amendment; the pre-registered T001 class is in "
                    "segarecomp_xplat_explore.t001.results.json and governs SEG-076-T002 admission)",
           "t001_plan_sha256": rx.plan_sha(), "amendment_sha256": amend_sha(),
           "views": {v: {"view_of": VIEWS[v], **{k: d[k] for k in ("expansion", "tables", "failed_fits", "missing", "farm",
                                                                 "determinism_bitwise", "paired_lineage_differences", "costs")}}
                     for v, d in docs.items()},
           "notes": ["both views are always reported, whichever looks better",
                     "B is also redrawn on ROCm (device control): a class change driven by B(ROCm) is not acted on either",
                     "costs in a view mix CPU (A, and B in 'mixed') and ROCm (B in 'rocm_bc', C) fit times under one model key",
                     "B(CPU) vs B(ROCm) mixes device, torch build (2.14.1 CPU vs 2.6.0+rocm6.4.2), BLAS and optimizer paths: a "
                     "single-seed perturbation estimate, not a measured bias; it does not exercise C's message passing"],
           "data_boundary": "aggregates only"}
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({v: d["expansion"]["class"] for v, d in docs.items()}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan").set_defaults(func=lambda a: (print(json.dumps({"amendment_sha256": hashlib.sha256(rx.canonical(write_plan())).hexdigest()})), 0)[1])
    sub.add_parser("check").set_defaults(func=lambda a: (verify_plan(), print(json.dumps({"amendment_sha256": amend_sha(), "check": "PASS"})), 0)[2])
    s = sub.add_parser("selftest")
    s.add_argument("--steps", type=int, default=6)
    s.set_defaults(func=cmd_selftest)
    f = sub.add_parser("fit")
    f.add_argument("--model", required=True)
    f.add_argument("--fold", required=True)
    f.add_argument("--out", required=True)
    f.set_defaults(func=cmd_fit)
    fm = sub.add_parser("farm")
    fm.add_argument("--out", required=True)
    fm.add_argument("--infra-rerun", nargs="*")
    fm.add_argument("--reason")
    fm.set_defaults(func=cmd_farm)
    v = sub.add_parser("views")
    v.add_argument("--cpu", required=True)
    v.add_argument("--rocm", required=True)
    v.add_argument("--views", required=True)
    v.set_defaults(func=cmd_views)
    tb = sub.add_parser("table")
    tb.add_argument("--views", required=True)
    tb.add_argument("--private", required=True)
    tb.add_argument("--results")
    tb.set_defaults(func=cmd_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
