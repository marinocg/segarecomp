#!/usr/bin/env python3
"""SEG-075-T006 (ADR 0163): final research handoff - environment report, one-shot status record, and the complete verification a successor
runs without rebuilding sources or retraining.

  environment  write <out>/environment.json: interpreter and package versions of the research venv, toolchain image ids, product and
               harness commit SHAs, plan / addendum digests (private; the receipt carries the digests)
  status       write <out>/ONE_SHOT_STATUS.json: whether the sealed reserved cohort was ever opened (T006 one-shot), bound to the sealed
               binding digests of the frozen snapshot
  verify       the successor's check (run it in a fresh process from any export of the product at the PR head; no task worktree):
                 1. physical verification of the final handoff manifest (every store object rehashed; manifest bound to the receipt)
                 2. restore the needed objects into an empty temporary directory and, from those copies only (relocated paths), re-score a
                    cross-platform development representative with the preserved C2-all M3-R weights and the unchanged SEG-072 scorer;
                    the result must equal the stored score array bit for bit
                 3. re-apply the bound decision rules to the RECORDED tau / completeness of the committed T004 results (tau > 0.05 ->
                    EVIDENCE_INSUFFICIENT; outcome rule 1 -> DATA_INSUFFICIENT); recomputing tau itself from the stored fold scores is
                    `segarecomp_xplat_transfer.py table` (minutes; needs the operator's Genesis images)
                 4. confirm the one-shot status record (opened = false) and its binding to the frozen sealed digests of the stored
                    snapshot (SNAPSHOT.json in the store, plan snapshot digest)
               prints ARTIFACT_HANDOFF_PASS or ARTIFACT_HANDOFF_INCOMPLETE with counts, bytes and the manifest identity
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_handoff as handoff  # noqa: E402

DIAG_SAMPLE = "seg075/t003/snapshot/dev/xp/amiga-realtime3d/programs/Program_09@hunk0"
DIAG_WEIGHTS = "seg075/t004/scores/M3R.C2.all.xplat.pt"
DIAG_SCORES = "seg075/t004/scores/M3R.C2.all.xplat.npz"
DIAG_ID = "xp/amiga-realtime3d/programs/Program_09#hunk0"


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def cmd_environment(a) -> int:
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    venv = pathlib.Path(a.venv_python)
    freeze = subprocess.run([str(venv), "-m", "pip", "freeze"], capture_output=True, text=True).stdout.split()
    if not freeze:
        freeze = subprocess.run(["uv", "pip", "freeze", "--python", str(venv)], capture_output=True, text=True).stdout.split()
    pyver = subprocess.run([str(venv), "-c", "import platform;print(platform.python_version())"], capture_output=True, text=True).stdout.strip()
    images = {}
    for tid in ("tc-gnu", "tc-elf", "tc-vbcc", "tc-a68k", "tc-mint"):
        r = subprocess.run(["docker", "image", "inspect", f"segarecomp-xplat/{tid}:1", "--format", "{{.Id}}"], capture_output=True, text=True)
        images[tid] = r.stdout.strip() or None
    doc = {"research_venv": {"python": pyver, "packages": sorted(freeze), "path": "<HARNESS_ROOT>/.cache/seg049/venv"},
           "host": {"machine": platform.machine(), "system": platform.system()},
           "toolchain_images": images, "product_sha": a.product_sha, "harness_sha": a.harness_sha,
           "plan_sha256": sha(HERE / "segarecomp_xplat_design.plan.json"), "t005_addendum_sha256": sha(HERE / "segarecomp_xplat_design.t005.json"),
           "decoder_cli": "seg075/cli-build/apps/segarecomp/segarecomp (built from product main 9796fee; decoder unchanged by PR 133)"}
    (out / "environment.json").write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"packages": len(freeze), "python": pyver, "images": images}))
    return 0


def cmd_status(a) -> int:
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    snap = json.loads((pathlib.Path(a.snapshot) / "SNAPSHOT.json").read_text())
    doc = {"schema": "segarecomp.xplat_one_shot_status.v1", "opened": False,
           "reason": "T004 EVIDENCE_INSUFFICIENT: no candidate could be frozen (ADR 0161 t006.freeze); outcome rule 1 fixes DATA_INSUFFICIENT; "
                     "the sealed reserved cohort stays unexposed for a future, better-powered study",
           "sealed_binding_sha256": hashlib.sha256(json.dumps(snap["sealed"], sort_keys=True).encode()).hexdigest(),
           "sealed_samples": len(snap["sealed"]), "snapshot_sha256": sha(pathlib.Path(a.snapshot) / "SNAPSHOT.json")}
    (out / "ONE_SHOT_STATUS.json").write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps(doc))
    return 0


def _restore_subset(manifest: dict, store: pathlib.Path, prefixes: tuple[str, ...], dest: pathlib.Path) -> int:
    n = 0
    for e in manifest["entries"]:
        if "sha256" in e and e["logical"].startswith(prefixes):
            t = dest / e["logical"]
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(handoff.cas_path(store, e["sha256"]), t)
            n += 1
    return n


def diagnostic(manifest_path: pathlib.Path, venv_python: str) -> dict:
    """Re-score the diagnostic sample from restored copies only and compare with the stored scores (subprocess in the research venv)."""
    man = json.loads(manifest_path.read_bytes())
    store = manifest_path.resolve().parent.parent
    with tempfile.TemporaryDirectory() as td:
        dest = pathlib.Path(td)
        n = _restore_subset(man, store, (DIAG_SAMPLE + "/", DIAG_WEIGHTS, DIAG_SCORES), dest)
        sample = dest / DIAG_SAMPLE
        meta = json.loads((sample / "meta.json").read_text())
        meta["image"] = str(sample / "image.bin")  # relocated: nothing outside the restored copy is read
        (sample / "meta.json").write_text(json.dumps(meta))
        code = f"""
import sys, json, numpy as np, torch
sys.path.insert(0, {str(HERE)!r})
import segarecomp_ml_v3_train as t, segarecomp_ml_region_gen2_train as g2
im = t.Image(__import__('pathlib').Path({str(sample)!r}))
m = g2.build_model(True)
m.load_state_dict(torch.load({str(dest / DIAG_WEIGHTS)!r}, map_location='cpu'))
m.eval()
s = t.score_m3(m, im)
ref = np.load({str(dest / DIAG_SCORES)!r})[{DIAG_ID.replace('/', '@@')!r}]
print(json.dumps({{"cells": int(len(s)), "bitwise_identical": bool(np.array_equal(s, ref))}}))
"""
        env = {"PATH": "/usr/bin:/bin", "HOME": os.environ.get("HOME", "/root"), "SEG072_ROOT": str(dest), "SEG049_ROOT": str(dest)}
        r = subprocess.run([venv_python, "-c", code], cwd="/", env=env, capture_output=True, text=True)
        try:
            res = json.loads(r.stdout.strip().splitlines()[-1])
        except (IndexError, json.JSONDecodeError):
            res = {"bitwise_identical": False, "error": r.stderr[-400:]}
        res["restored_files"] = n
        return res


def status_valid(status: dict, snapshot_bytes: bytes) -> bool:
    """The one-shot status says the seal was never opened AND is bound to the frozen snapshot / sealed binding (pure; unit-tested)."""
    plan = json.loads((HERE / "segarecomp_xplat_design.plan.json").read_text())
    sealed = json.loads(snapshot_bytes)["sealed"]
    return (status.get("opened") is False and status.get("snapshot_sha256") == hashlib.sha256(snapshot_bytes).hexdigest()
            == plan["corpus"]["snapshot_sha256"]
            and status.get("sealed_binding_sha256") == hashlib.sha256(json.dumps(sealed, sort_keys=True).encode()).hexdigest())


def decision_check() -> dict:
    import segarecomp_xplat_decide as decide
    r = json.loads((HERE / "segarecomp_xplat_transfer.results.json").read_text())
    tau = r["noise"]["tau"]
    insufficient = (not r["complete"]) or tau is None or tau > 0.05
    t004 = "EVIDENCE_INSUFFICIENT" if insufficient else r["transfer_class"]
    out = decide.outcome(t004, {"config": "M3_R", "condition": "C0", "genesis_b50_misses": 0, "genesis_mean_er": 0, "xplat_mean_er": 0},
                         {"config": "M3_R", "condition": "C0", "genesis_b50_misses": 0, "genesis_mean_er": 0, "xplat_mean_er": 0},
                         tau or 1.0, 0, False, False, None)
    return {"tau": tau, "t004": t004, "recorded_t004": r["transfer_class"], "outcome": out["class"],
            "consistent": t004 == r["transfer_class"] and out["class"] == "DATA_INSUFFICIENT"}


def cmd_verify(a) -> int:
    m = pathlib.Path(a.manifest)
    phys = handoff.verify_fresh(m, False, json.loads(pathlib.Path(a.receipt).read_text())["manifest_sha256"] if a.receipt else None)
    diag = diagnostic(m, a.venv_python)
    dec = decision_check()
    man = json.loads(m.read_bytes())
    store = m.resolve().parent.parent
    status = next((e for e in man["entries"] if e["logical"].endswith("t006/ONE_SHOT_STATUS.json")), None)
    snap = next((e for e in man["entries"] if e["logical"] == "seg075/t003/snapshot/SNAPSHOT.json"), None)
    status_ok = False
    if status and snap:
        status_ok = status_valid(json.loads(handoff.cas_path(store, status["sha256"]).read_bytes()),
                                 handoff.cas_path(store, snap["sha256"]).read_bytes())
    ok = phys["verdict"] == "PASS" and diag.get("bitwise_identical") and dec["consistent"] and status_ok
    res = {"verdict": "ARTIFACT_HANDOFF_PASS" if ok else "ARTIFACT_HANDOFF_INCOMPLETE", "manifest_sha256": phys["manifest_sha256"],
           "verified_files": phys["verified_files"], "verified_bytes": phys["verified_bytes"], "physical_problems": len(phys["problems"]),
           "fresh_process": phys.get("fresh_process"), "diagnostic": diag, "decision": dec, "one_shot_status_present": status_ok}
    print(json.dumps(res, indent=1))
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("environment")
    e.add_argument("--out", required=True)
    e.add_argument("--venv-python", required=True)
    e.add_argument("--product-sha", required=True)
    e.add_argument("--harness-sha", required=True)
    e.set_defaults(func=cmd_environment)
    s = sub.add_parser("status")
    s.add_argument("--out", required=True)
    s.add_argument("--snapshot", required=True)
    s.set_defaults(func=cmd_status)
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--receipt")
    v.add_argument("--venv-python", required=True)
    v.set_defaults(func=cmd_verify)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
