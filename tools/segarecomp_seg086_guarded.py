#!/usr/bin/env python3
"""SEG-086 post-freeze amendment (ADR 0198): the ONLY approved entry point for running the SEG-086 development pipeline.

The sample-role guard (`segarecomp_sample_guard`) is installed BEFORE any command that can load a sample runs, then the existing frozen implementation is called unchanged:

  verify-frozen              frozen plan verifies; guarded records match the post-freeze reference digest; the nested evaluation recomputed under the guard reproduces the
                             FROZEN determinism digest of `segarecomp_seg086_cal.results.json`. No sample is ever loaded without the guard.
  diag      --out PATH       `segarecomp_seg086_diag.py run`        (writes PATH instead of the committed results file)
  select    --out PATH       `segarecomp_seg086_cal.py select`      (writes PATH instead of the frozen, digest-bound selection file)
  nested    [--out PATH] [--variants V ...]   `segarecomp_seg086_cal.py nested`
  report | h01               the post-freeze report / exposed-image diagnostic (outputs byte-identical to the committed JSON)

Not exposed: `freeze` and `final` (they would overwrite frozen artifacts), and any command that fits a model other than the documented diagnostic refit inside `diag`.
`--out` may not name a frozen artifact. The historical scripts (`segarecomp_seg086_cal.py`, `segarecomp_seg086_diag.py`) remain callable directly for archival reproducibility only: they use the
unguarded legacy loaders and are NOT approved for future research with evaluation-only data. An authorization failure raises; nothing falls back to the original loader.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# Reference digest of the 50 development records (ids, lineages, scores, labels, seeds, class maps). Documented in ADR 0198 when the guard was added; it is a POST-FREEZE reference,
# not part of the frozen plan (the frozen evidence is the determinism digest of the nested results). It is compared, never recomputed to make a check pass.
REFERENCE_RECORDS_SHA256 = "e4d471dec7d7a88c673b67a6210ad98b3f50bdf6697ba1308393fa6f0f4ebfe1"
FROZEN_ARTIFACTS = ("segarecomp_seg086_cal.results.json", "segarecomp_seg086_cal.plan.json", "segarecomp_seg086_cal.plan.digest", "segarecomp_seg086_cal.selection.json",
                    "segarecomp_seg086_diag.results.json", "segarecomp_seg086_cal.h01.json", "segarecomp_seg086_report.json")


def records_digest(recs) -> str:
    h = hashlib.sha256()
    for r in sorted(recs, key=lambda r: r["id"]):
        h.update(json.dumps([r["id"], r["lineage"], r["scores"], r["labels"], sorted(r["seeds"]), hashlib.sha256(r["classes"]).hexdigest()]).encode())
    return h.hexdigest()


def check_out(path: str) -> pathlib.Path:
    p = pathlib.Path(path).resolve()
    if p.name in FROZEN_ARTIFACTS or p.parent == HERE.resolve() and p.name.startswith("segarecomp_seg086"):
        raise SystemExit(f"--out may not name a committed or frozen SEG-086 artifact: {p.name}")
    return p


class Env:
    """The modules the commands delegate to; injectable so the dispatcher can be tested without a registry."""

    def __init__(self, guard_install=None, cal=None, diag=None, report=None, h01=None):
        import segarecomp_sample_guard as sg
        self.guard_install = guard_install or sg.install
        self._cal, self._diag, self._report, self._h01 = cal, diag, report, h01

    @property
    def cal(self):
        if self._cal is None:
            import segarecomp_seg086_cal as c
            self._cal = c
        return self._cal

    @property
    def diag(self):
        if self._diag is None:
            import segarecomp_seg086_diag as d
            self._diag = d
        return self._diag

    @property
    def report(self):
        if self._report is None:
            import segarecomp_seg086_report as r
            self._report = r
        return self._report

    @property
    def h01(self):
        if self._h01 is None:
            import segarecomp_seg086_h01 as h
            self._h01 = h
        return self._h01


def verify_frozen(env: Env) -> int:
    cal = env.cal
    digest = cal.verify_plan()                                              # manifest + plan + bound files; no sample is touched
    res = json.loads(pathlib.Path(cal.RESULTS).read_text())
    env.guard_install()                                                     # BEFORE the first and only load
    _, recs = cal.load_records()
    rd = records_digest(recs)
    nested = cal.run_nested(("RAW", "N1", "N2"), recs)
    out = {"plan_sha256": digest, "records_sha256": rd, "records_match_reference": rd == REFERENCE_RECORDS_SHA256,
           "nested_digest_reproduces_frozen_results": cal.canonical(nested) == res["determinism_sha256"], "frozen_determinism_sha256": res["determinism_sha256"],
           "development_decision": res["development_decision"]["decision"], "frozen_policy_fallback_images": f"{res['tables_all_images']['primary_' + res['chosen_normaliser']]['all']['fallback_images']}/"
           f"{res['tables_all_images']['primary_' + res['chosen_normaliser']]['all']['domain_images']}"}
    print(json.dumps(out))
    return 0 if out["records_match_reference"] and out["nested_digest_reproduces_frozen_results"] else 1


def run(argv, env: Env = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("verify-frozen")
    d = sub.add_parser("diag")
    d.add_argument("--out", required=True)
    s = sub.add_parser("select")
    s.add_argument("--out", required=True)
    n = sub.add_parser("nested")
    n.add_argument("--out")
    n.add_argument("--variants", nargs="+", choices=("RAW", "N1", "N2"))
    sub.add_parser("report")
    sub.add_parser("h01")
    a = ap.parse_args(argv)                                                 # unknown commands / arguments are rejected here, before anything is installed or loaded
    out = check_out(a.out) if getattr(a, "out", None) else None
    env = env or Env()
    if a.cmd == "verify-frozen":
        return verify_frozen(env)
    env.guard_install()                                                     # installed before the delegate can load anything
    if a.cmd == "diag":
        env.diag.RESULTS = out
        return env.diag.main(["run"]) or 0
    if a.cmd == "select":
        env.cal.SELECTION = out
        return env.cal.main(["select"]) or 0
    if a.cmd == "nested":
        args = ["nested"] + (["--out", str(out)] if out else []) + (["--variants", *a.variants] if a.variants else [])
        return env.cal.main(args) or 0
    if a.cmd == "report":
        return env.report.main() or 0
    return env.h01.main() or 0


def main(argv=None) -> int:
    return run(sys.argv[1:] if argv is None else argv)


if __name__ == "__main__":
    raise SystemExit(main())
