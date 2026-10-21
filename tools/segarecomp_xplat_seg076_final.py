#!/usr/bin/env python3
"""SEG-076-T004 C7: final verification of the SEG-076 research handoff (extends SEG-075's content-addressed store; OFFLINE tooling).

  verify   the successor's check. Run it in a fresh process from any export of the product at the PR head, with no task worktree:
             1. physical verification of the SEG-076 manifest (every store object rehashed; manifest bound to the committed receipt). The
                manifest extends SEG-075's c7-final-handoff with every SEG-076 private tree
             2. regenerate representative predictions from RESTORED copies only (relocated paths) and compare bit for bit with the
                stored scores:
                - A (CPU, research venv): the preserved all-Genesis HGB pickle re-scores a cross-platform development representative
                - C-v2 (ROCm venv, optional --rocm-python): the preserved all-Genesis C-v2 weights re-score the same sample on the GPU
             3. re-derive every SEG-076 class from the committed result documents with the frozen pure rule functions: T001 expansion
                (pre-registered and both amendment views), T003 transfer / stability / A / M1, T005 C-v2 vs A
             4. SEG-075's one-shot status still says the reserved cohort was never opened (bound to the frozen sealed digests)
           prints ARTIFACT_HANDOFF_PASS or ARTIFACT_HANDOFF_INCOMPLETE
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_final as f075  # noqa: E402
import segarecomp_xplat_handoff as handoff  # noqa: E402

SAMPLE = f075.DIAG_SAMPLE
SAMPLE_ID = f075.DIAG_ID
A_WEIGHTS, A_SCORES = "seg076/t001/scores/A.C0.all.xplat.pkl", "seg076/t001/scores/A.C0.all.xplat.npz"
CV2_WEIGHTS, CV2_SCORES = "seg076/t005/scores/CV2.C0.all.s1.xplat.pt", "seg076/t005/scores/CV2.C0.all.s1.xplat.npz"


def _restore(man: dict, store: pathlib.Path, prefixes: tuple, dest: pathlib.Path) -> pathlib.Path:
    n = f075._restore_subset(man, store, prefixes, dest)
    sample = dest / SAMPLE
    meta = json.loads((sample / "meta.json").read_text())
    meta["image"] = str(sample / "image.bin")
    (sample / "meta.json").write_text(json.dumps(meta))
    return n


def _run(python: str, code: str, dest: pathlib.Path, extra_env: dict | None = None) -> dict:
    env = {"PATH": "/usr/bin:/bin", "HOME": os.environ.get("HOME", "/root"), "SEG072_ROOT": str(dest), "SEG049_ROOT": str(dest),
           **(extra_env or {})}
    r = subprocess.run([python, "-c", code], cwd="/", env=env, capture_output=True, text=True)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError):
        return {"bitwise_identical": False, "error": r.stderr[-400:]}


def regenerate(manifest_path: pathlib.Path, venv_python: str, rocm_python: str | None) -> dict:
    man = json.loads(manifest_path.read_bytes())
    store = manifest_path.resolve().parent.parent
    out = {}
    with tempfile.TemporaryDirectory() as td:
        dest = pathlib.Path(td)
        n = _restore(man, store, (SAMPLE + "/", A_WEIGHTS, A_SCORES, CV2_WEIGHTS, CV2_SCORES), dest)
        sample = dest / SAMPLE
        common = f"""
import sys, json, pathlib, numpy as np
sys.path.insert(0, {str(HERE)!r})
import segarecomp_ml_v3_train as t
im = t.Image(pathlib.Path({str(sample)!r}))
m = json.loads((pathlib.Path({str(sample)!r}) / 'meta.json').read_text())
im._dir = pathlib.Path({str(sample)!r}); im.seed_addresses = m.get('seed_addresses', [])
key = {SAMPLE_ID.replace('/', '@@')!r}
"""
        a_code = common + f"""
import pickle, segarecomp_xplat_models as xm
clf = pickle.loads(pathlib.Path({str(dest / A_WEIGHTS)!r}).read_bytes())
s = xm.score_a(clf, im)
ref = np.load({str(dest / A_SCORES)!r})[key]
print(json.dumps({{"cells": int(len(s)), "bitwise_identical": bool(np.array_equal(np.asarray(s, dtype=ref.dtype), ref))}}))
"""
        out["A_cpu"] = _run(venv_python, a_code, dest)
        if rocm_python:
            c_code = common + f"""
import torch, segarecomp_xplat_models_v2 as v2
torch.use_deterministic_algorithms(True)
dev = torch.device('cuda:0')
net = v2.build_cv2(torch).to(dev)
net.load_state_dict(torch.load({str(dest / CV2_WEIGHTS)!r}, map_location='cpu'))
net.eval()
s = v2.infer_cv2(net, im, dev)
ref = np.load({str(dest / CV2_SCORES)!r})[key]
print(json.dumps({{"words": int(len(s)), "bitwise_identical": bool(np.array_equal(s, ref))}}))
"""
            out["CV2_rocm"] = _run(rocm_python, c_code, dest, {"CUBLAS_WORKSPACE_CONFIG": ":4096:8"})
        out["restored_files"] = n
    return out


def decisions() -> dict:
    """Re-derive every committed SEG-076 class with the frozen pure rule functions (pure over committed documents)."""
    import segarecomp_xplat_cv2 as cv2
    import segarecomp_xplat_explore_eval as ev6
    import segarecomp_xplat_stability as st
    res = {}
    plan1 = json.loads((HERE / "segarecomp_xplat_explore.t001.plan.json").read_text())
    docs = [("t001", json.loads((HERE / "segarecomp_xplat_explore.t001.results.json").read_text()))]
    am = json.loads((HERE / "segarecomp_xplat_explore.t001.rocm.results.json").read_text())
    docs += [(f"t001_amendment_{v}", d) for v, d in am["views"].items()]
    for name, d in docs:
        rule_in = {c: (None if c in d["failed_fits"] or not d["tables"][c]["complete_genesis"] else
                       {"rows": d["tables"][c]["genesis"], "summary": d["tables"][c]["genesis_summary"]})
                   for c in ("A", "B", "C", "M4_DENSE_s1@128", "M3_R_s49")}
        res[name] = {"recorded": d["expansion"]["class"], "consistent": ev6.expansion_class(rule_in, plan1["expansion_rule"]) == d["expansion"]}
    s3 = json.loads((HERE / "segarecomp_xplat_stability.results.json").read_text())
    D = s3["decisions"]
    t = D["m3r_transfer"]
    res["t003_m3r_transfer"] = {"recorded": t["class"],
                                "consistent": st.transfer_class(t["per_seed_d"], list(t["per_lineage_seed_mean_d"].values()))["class"] == t["class"]}
    res["t003_seed_stability"] = {"recorded": D["m3r_seed_stability"]["class"],
                                  "consistent": st.stability_class(D["m3r_seed_stability"]["gains"])["class"] == D["m3r_seed_stability"]["class"]}
    a = D["a_transfer"]
    a_cls = "TRANSFER_EFFECT_MEASURABLE" if abs(a["d_mean_er"]) >= 0.01 and st.sign_flip_p(list(a["per_lineage_d"].values())) <= 0.05 \
        else "NO_MEANINGFUL_ADVANTAGE"
    res["t003_a_transfer"] = {"recorded": a["class"], "consistent": a_cls == a["class"]}
    m1 = D["m1_descriptive_transfer"]
    m1_ci = st.bootstrap_ci(list(m1["per_lineage_d"].values()))
    m1_p = st.sign_flip_p(list(m1["per_lineage_d"].values()))
    m1_cls = "DESCRIPTIVE_EFFECT_ROBUST" if (m1_ci["lo"] > 0 or m1_ci["hi"] < 0) and m1_p <= 0.05 else "DESCRIPTIVE_EFFECT_NOT_ROBUST"
    res["t003_m1"] = {"recorded": m1["class"], "consistent": m1_cls == m1["class"]}
    c5 = json.loads((HERE / "segarecomp_xplat_cv2.results.json").read_text())
    C = c5["configurations"]
    d_seed = [C[f"CV2.s{s}"]["summary"]["mean_er"] - C["A"]["summary"]["mean_er"] for s in cv2.SEEDS]
    got = cv2.cv2_class(d_seed, [C[f"CV2.s{s}"]["summary"]["misses_B50"] for s in cv2.SEEDS], C["A"]["summary"]["misses_B50"],
                        not c5["missing"], c5["determinism_bitwise_d05_s1"])
    res["t005_cv2_vs_a"] = {"recorded": c5["decision"]["class"], "consistent": got["class"] == c5["decision"]["class"]}
    return res


def cmd_verify(a) -> int:
    m = pathlib.Path(a.manifest)
    expect = json.loads(pathlib.Path(a.receipt).read_text())["manifest_sha256"] if a.receipt else None
    phys = handoff.verify_fresh(m, False, expect)
    regen = regenerate(m, a.venv_python, a.rocm_python)
    dec = decisions()
    man = json.loads(m.read_bytes())
    store = m.resolve().parent.parent
    status = next((e for e in man["entries"] if e["logical"].endswith("t006/ONE_SHOT_STATUS.json")), None)
    snap = next((e for e in man["entries"] if e["logical"] == "seg075/t003/snapshot/SNAPSHOT.json"), None)
    sealed_ok = bool(status and snap and f075.status_valid(json.loads(handoff.cas_path(store, status["sha256"]).read_bytes()),
                                                            handoff.cas_path(store, snap["sha256"]).read_bytes()))
    regen_ok = all(v.get("bitwise_identical") for k, v in regen.items() if isinstance(v, dict))
    ok = phys["verdict"] == "PASS" and regen_ok and all(v["consistent"] for v in dec.values()) and sealed_ok
    res = {"verdict": "ARTIFACT_HANDOFF_PASS" if ok else "ARTIFACT_HANDOFF_INCOMPLETE", "manifest_sha256": phys["manifest_sha256"],
           "verified_files": phys["verified_files"], "verified_bytes": phys["verified_bytes"], "physical_problems": len(phys["problems"]),
           "fresh_process": phys.get("fresh_process"), "regeneration": regen, "decisions": dec, "seg075_reserved_cohort_unopened": sealed_ok}
    print(json.dumps(res, indent=1))
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--receipt")
    v.add_argument("--venv-python", required=True)
    v.add_argument("--rocm-python", help="optional: also regenerate the C-v2 prediction on the ROCm GPU")
    v.set_defaults(func=cmd_verify)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
