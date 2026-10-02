#!/usr/bin/env python3
"""CI sharding guard (hermetic, no build products used): the CTest `shard-N` partition is complete, disjoint and balanced.

usage: ci_shard_guard_test.py <ctest> <build-dir> <source-root> <shard-count> <budget-seconds> <max-test-seconds>

CI runs one matrix job per shard (`ctest -L ^shard-N$`); the union of the shards must be exactly the `full` tier (which contains the
`fast` tier), so that no test is silently skipped or run twice, and a shard must stay inside its wall-clock budget estimate.
tests/ci_shards.cmake assigns every `full` test a shard (unlisted tests get a default cost and land in the least-loaded shard), so
this guard fails only when that mechanism is bypassed or the estimated cost no longer fits:

  * every test labelled `full` carries exactly one `shard-N` label, N in 1..count, and nothing else carries a shard label;
  * `fast` is a subset of `full`; every test has a tier label (`full` or `extended`) except the documented exceptions;
  * the estimated cost of each shard (the CTest COST property) is within the budget and no single test is above the per-test limit;
  * the CI workflow's shard matrix lists exactly the shards 1..count.
"""
import json
import pathlib
import re
import subprocess
import sys

CTEST, BUILD, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
COUNT, BUDGET, MAX_TEST = int(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6])
# Oracle-only tests that no hermetic tier runs (they skip without a local checkout); they must stay out of every shard.
UNTIERED = {"m68k_bcd_exhaustive_musashi_test"}
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def main():
    listing = subprocess.run([CTEST, "--test-dir", BUILD, "-N", "--show-only=json-v1"], capture_output=True, text=True, check=True)
    tests = json.loads(listing.stdout)["tests"]
    shards = {n: [] for n in range(1, COUNT + 1)}
    cost = {n: 0.0 for n in shards}
    full = set()
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        labels = set(props.get("LABELS", []))
        name = test["name"]
        mine = sorted(label for label in labels if label.startswith("shard-"))
        if "full" in labels:
            full.add(name)
            valid = [m for m in mine if re.fullmatch(r"shard-[0-9]+", m) and 1 <= int(m[6:]) <= COUNT]
            check(len(valid) == 1 and len(mine) == 1, "%s must carry exactly one shard-1..%d label (has %s)" % (name, COUNT, mine))
            if len(valid) == 1:
                shards[int(valid[0][6:])].append(name)
                cost[int(valid[0][6:])] += float(props.get("COST", 0))
                check(float(props.get("COST", 0)) <= MAX_TEST, "%s: estimated cost %s s exceeds the %s s per-test limit; split it" % (name, props.get("COST"), MAX_TEST))
        else:
            check(not mine, "%s carries a shard label but is not in the full tier" % name)
            check(bool(labels & {"extended"}) or name in UNTIERED, "%s has no tier label (full/extended): CI would never run it" % name)
        if "fast" in labels:
            check("full" in labels, "%s is `fast` but not `full`" % name)
    seen = [n for names in shards.values() for n in names]
    check(len(seen) == len(set(seen)) and set(seen) == full, "the shards' union is not exactly the full tier")
    for n, names in shards.items():
        check(bool(names), "shard-%d is empty" % n)
        check(cost[n] <= BUDGET, "shard-%d estimated cost %.0f s exceeds the %.0f s budget" % (n, cost[n], BUDGET))
    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    match = re.search(r"^\s*shard:\s*\[([0-9,\s]+)\]", workflow, re.M)
    check(match is not None and [int(x) for x in match.group(1).split(",")] == list(range(1, COUNT + 1)),
          "the .github/workflows/ci.yml shard matrix must list exactly 1..%d" % COUNT)
    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("ci shards: %d tests in %d shards, estimated cost per shard %s" % (len(seen), COUNT, ", ".join("%.0f" % cost[n] for n in shards)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
