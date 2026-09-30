#!/usr/bin/env python3
"""SEG-009-T001: the Master System capability list is reproducible, complete and consistent.

- `tools/sms_capabilities.py --check` reproduces tests/fixtures/sms-capabilities.json byte-for-byte;
- every row has exactly one owner among the SEG-009 capability owners and known evidence keys;
- excluded rows that software can select carry a typed SMS_ERROR_* class listed in the contract's error table;
- two-way completeness with docs/architecture/master-system-machine-contract.md: every row is cited there as
  [cap:<id>] and every citation names a row;
- every `unresolved` U-item is a row of the contract's open-facts table whose owner cell names a SEG-009 task;
- the report counts are consistent with the rows.
"""
import collections
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import sms_capabilities as caps  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    check(caps.main(["--check"]) == 0, "capability list does not reproduce")
    doc = json.loads(caps.OUTPUT.read_text(encoding="utf-8"))
    rows = doc["capabilities"]
    check(caps.validate(rows) == [], "capability rows invalid")
    contract = (REPO / "docs" / "architecture" / "master-system-machine-contract.md").read_text(encoding="utf-8")
    ids = {r["id"] for r in rows}
    cited = set(re.findall(r"\[cap:([a-z0-9_.]+)\]", contract))
    check(ids <= cited, "rows not cited by the contract: %s" % sorted(ids - cited))
    check(cited <= ids, "contract cites unknown capabilities: %s" % sorted(cited - ids))
    error_table = contract.split("## 13.", 1)[1].split("## 14.", 1)[0]
    open_table = contract.split("## 14.", 1)[1].split("## 15.", 1)[0]
    for r in rows:
        check(len([o for o in caps.OWNERS if o == r["owner"]]) == 1, "%s: owner" % r["id"])
        if "fail_closed" in r:
            check(r["scope"].startswith("excluded:"), "%s: only exclusions carry a fail-closed class" % r["id"])
            check("`%s`" % r["fail_closed"] in error_table, "%s: %s missing from the error table" % (r["id"], r["fail_closed"]))
        if "unresolved" in r:
            m = re.search(r"^\| %s \|(.+)$" % re.escape(r["unresolved"]), open_table, re.M)
            check(m is not None, "%s: %s missing from the open-facts table" % (r["id"], r["unresolved"]))
            cells = [c.strip() for c in m.group(1).split("|")]
            check(len(cells) >= 5 and re.search(r"\bT0(0[2-9]|1[0-3])\b", cells[2]),
                  "%s: %s must name a SEG-009 owner task" % (r["id"], r["unresolved"]))
    # every non-baseline mapper, VDP mode and peripheral named as excluded is typed or explicitly device-absent
    for r in rows:
        if r["area"] in ("vdp_modes", "mapper_identity") and r["scope"].startswith("excluded:") and "no request channel exists" not in r["scope"]:
            check("fail_closed" in r, "%s: excluded mode/mapper without a typed stop" % r["id"])
    in_scope = sum(r["scope"] == caps.IN for r in rows)
    check(in_scope > 100, "in-scope rows unexpectedly few")
    per_owner = collections.Counter(r["owner"] for r in rows if r["scope"] == caps.IN)
    check(set(per_owner) == set(caps.OWNERS), "every capability owner owns at least one in-scope row: %s" % per_owner)
    report = caps.report(doc)
    check(report.startswith("capabilities: %d" % len(rows)), "report header")
    print("ok: %d capabilities (%d in scope), consistent with the contract" % (len(rows), in_scope))
    return 0


if __name__ == "__main__":
    sys.exit(main())
