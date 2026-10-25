#!/usr/bin/env python3
"""SEG-086 post-freeze amendment (ADR 0198): the sanctioned, role-guarded entry point for the frozen development pipeline. Nothing here fits or writes the frozen artifacts.

  verify-frozen   (1) the frozen plan still verifies (bound files untouched); (2) the development records loaded WITH the protection layer are identical to those loaded
                  without it (ids, lineages, scores, labels, seeds, classes); (3) the nested evaluation recomputed under the guard reproduces the stored determinism digest
                  of `segarecomp_seg086_cal.results.json`. (Refusal behaviour is covered by tests/segarecomp_seg086_guard_test.py.)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_seg086_cal as cal  # noqa: E402
import segarecomp_sample_guard as sg  # noqa: E402


def records_digest(recs) -> str:
    h = hashlib.sha256()
    for r in sorted(recs, key=lambda r: r["id"]):
        h.update(json.dumps([r["id"], r["lineage"], r["scores"], r["labels"], sorted(r["seeds"]), hashlib.sha256(r["classes"]).hexdigest()]).encode())
    return h.hexdigest()


def cmd_verify_frozen(a) -> int:
    digest = cal.verify_plan()
    res = json.loads(cal.RESULTS.read_text())
    _, plain = cal.load_records()
    before = records_digest(plain)
    sg.install()
    _, guarded = cal.load_records()
    after = records_digest(guarded)
    nested = cal.run_nested(("RAW", "N1", "N2"), guarded)
    out = {"plan_sha256": digest, "records_identical_with_and_without_guard": before == after, "records_sha256": after,
           "nested_digest_reproduces_stored_results": cal.canonical(nested) == res["determinism_sha256"], "stored_determinism_sha256": res["determinism_sha256"]}
    print(json.dumps(out))
    return 0 if out["records_identical_with_and_without_guard"] and out["nested_digest_reproduces_stored_results"] else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_subparsers(dest="cmd", required=True).add_parser("verify-frozen").set_defaults(func=cmd_verify_frozen)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
