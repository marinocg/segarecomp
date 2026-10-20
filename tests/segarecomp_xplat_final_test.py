#!/usr/bin/env python3
"""SEG-075-T006 (ADR 0163): the final decision is re-derivable from the committed results with the bound rules, and the committed C7
receipt (when present) is a sanitized ARTIFACT_HANDOFF_PASS record. Standard library only."""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_xplat_final as fin  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def main():
    d = fin.decision_check()
    check(d["consistent"] and d["t004"] == "EVIDENCE_INSUFFICIENT" and d["outcome"] == "DATA_INSUFFICIENT",
          "committed T004 results re-derive EVIDENCE_INSUFFICIENT -> DATA_INSUFFICIENT under the bound rules")
    rec = ROOT / "tools" / "segarecomp_xplat_handoff.c7_receipt.json"
    if rec.is_file():
        r = json.loads(rec.read_text())
        check(r.get("final_verify", {}).get("verdict") == "ARTIFACT_HANDOFF_PASS", "C7 receipt records ARTIFACT_HANDOFF_PASS")
        text = json.dumps(r)
        check("/root/" not in text and '"offset"' not in text, "C7 receipt is sanitized")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_final_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
