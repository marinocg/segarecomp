#!/usr/bin/env python3
"""SEG-009-T012: Master System capability-coverage ratchet (hermetic, no ROM, no oracle).

usage: sms_capability_ratchet_test.py <product-root>

Re-measures tests/fixtures/sms-capabilities.json against the registered tests (tools/sms_capability_coverage.py) and compares
with the committed snapshot tests/fixtures/sms-capability-coverage.json and the generated report
docs/testing/sms-capability-coverage.md:
  * 100% of in-scope rows are `covered`; every exclusion has a reason, an evidence test and a pending-T013 approval;
  * no stage may drop for any row; an improvement needs a deliberate `--update-snapshot`;
  * every claim's test is registered in CTest and its source contains the claim's needle (enforced by the tool);
  * the open facts named by the capability list appear in the report's fact table, U2/U8 stay unresolved (tolerance/mask),
    U3 resolved, U6 classified;
  * the tool and the report contain no ROM-derived data: no hex byte strings or addresses beyond the public mapper
    register window, and no local paths;
  * negative controls: the ratchet detects a dropped stage, an unrecorded improvement and a needle that is gone.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import sms_capability_coverage as cov  # noqa: E402


def check(cond, message):
    if not cond:
        print("FAIL:", message)
        sys.exit(1)


def main():
    check(cov.main(["--check"]) == 0, "coverage check failed (snapshot/report stale, stage dropped, or row uncovered)")
    rows = cov.load_rows()
    doc = cov.snapshot_doc(rows)
    snapshot = json.loads(cov.SNAPSHOT.read_text(encoding="utf-8"))
    check(snapshot == doc, "snapshot differs from the fresh measurement")
    in_scope = [r for r in rows if r["scope"] == "in_scope"]
    check(set(doc["rows"]) == {r["id"] for r in in_scope}, "snapshot row set differs from the in-scope rows")
    check(all(cov.covered(m) for m in doc["rows"].values()), "an in-scope row is not covered")
    t = cov.tally(doc)
    check(t["covered"][0] == t["covered"][1] == len(in_scope), "coverage is not 100%% of in-scope rows: %r" % (t["covered"],))
    check(t["implemented"][0] == len(in_scope), "an in-scope row has no implementation test")
    excluded = [r for r in rows if r["scope"] != "in_scope"]
    check(set(doc["excluded"]) == {r["id"] for r in excluded}, "exclusion set differs")
    for row_id, e in doc["excluded"].items():
        check(e["reason"] and e["class"] and e["evidence_test"] and e["approval"] == "pending T013", "exclusion %s lacks reason/evidence/approval" % row_id)
    for r in excluded:
        if r["area"] in ("vdp_modes", "mapper_identity"):
            check(doc["excluded"][r["id"]]["fail_closed"], "%s: excluded mode/mapper without typed stop" % r["id"])
    # unresolved facts: the statuses recorded in the report follow the contract's open-fact table
    contract = (ROOT / "docs/architecture/master-system-machine-contract.md").read_text(encoding="utf-8")
    status = {uid: s for uid, s, _ in cov.U_STATUS}
    check(status["U2"].startswith("unresolved") and status["U8"].startswith("unresolved") and status["U3"].startswith("resolved") and
          status["U6"].startswith("classified"), "unresolved-fact statuses")
    open_table = contract.split("## 14.", 1)[1].split("## 15.", 1)[0]
    check("| U3 | **RESOLVED (T006)**" in open_table, "contract no longer records U3 as resolved")
    check("U6 | VDP access-slot loss" in open_table and "T012 evidence" in open_table, "contract U6 row lacks the T012 classification")
    for row in rows:
        if "unresolved" in row:
            check(row["unresolved"] in status, "%s: %s has no status in the report" % (row["id"], row["unresolved"]))
    # no commercial-derived data
    report = cov.REPORT.read_text(encoding="utf-8") + cov.SNAPSHOT.read_text(encoding="utf-8")
    check(not re.search(r"[0-9A-Fa-f]{32,}", report), "long hex string in the report")
    check("/Users/" not in report and "games/" not in report.replace("`games/sms`", ""), "local path in the report")
    # negative controls
    old = {"x": "11111"}
    check(cov.diff_masks(old, {"x": "11011"})[0], "control: dropped stage not detected")
    check(cov.diff_masks(old, {"x": "11111", "y": "11111"})[1], "control: added row not detected")
    check(cov.diff_masks({"x": "11101"}, {"x": "11111"})[1], "control: unrecorded improvement not detected")
    saved = list(cov.GROUPS)
    try:
        cov.GROUPS.append(cov.g("mem.ram_8k", "sms_machine_tests", "I", "needle that is not in the test source"))
        check(cov.validate_groups(), "control: missing needle not detected")
    finally:
        cov.GROUPS[:] = saved
    check(not cov.covered("01101") and cov.covered("1-1 10".replace(" ", "")), "control: covered() rule")
    print("sms capability ratchet OK: %d/%d rows covered, %d exclusions" % (t["covered"][0], t["covered"][1], len(excluded)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
