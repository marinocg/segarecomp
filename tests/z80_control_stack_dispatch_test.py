#!/usr/bin/env python3
"""SEG-008-T005: control-flow, stack and exact generated-entry dispatch scenarios (hermetic; oracle optional).

usage: z80_control_stack_dispatch_test.py <z80_image_emitter> <cc> <product-root>

Runs tests/fixtures/z80-conformance-vectors/control_stack_scenarios.json natively through the public emitter and the
strict-C11 generated runtime: a table-dispatch program (jump table through (HL), JP (IX), computed RET, a jump into the
middle of another instruction), 0xFFFF wrap of operands/return addresses/relative targets, SP wrap, a stack inside the
image region, window-relative CALL/RST/RET across two windows of one banked image, mapping-sensitive CALL into two
different banked images, a remap between CALL and RET, and fail-closed typed stops for an unmapped and a not-emitted
indirect target. Scenarios not flagged `oracle: false` are also compared with the pinned oracle when
SEGARECOMP_Z80_ORACLE_CHECKOUT is set (skipped otherwise, never a failure).
"""
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FIXTURE = ROOT / "tests" / "fixtures" / "z80-conformance-vectors" / "control_stack_scenarios.json"
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def main():
    doc = z.load_scenarios(FIXTURE)
    root = z.oracle_checkout()
    tc = z.Toolchain(CC, EMITTER, root)
    with tempfile.TemporaryDirectory() as tmp:
        batches = z.scenario_batches(doc)
        report = z.run_batches(tc, batches, pathlib.Path(tmp), with_oracle=root is not None)
        oracle_compared = 0
        for batch in batches:
            r = report[batch.name]
            if r.get("error"):
                check(False, "%s: %s" % (batch.name, str(r["error"])[:600]))
                continue
            vecs = [v for v, _ in batch.vectors]
            for vec in vecs:
                steps = r["generated"].get(vec.name, [])
                check(len(steps) == len(vec.steps), "%s: missing step results" % vec.name)
                for problem in z.check_expectations(vec, steps):
                    check(False, problem)
                check(all(s["out"] not in ("no_owner", "mutable_code", "unknown_image_identity") for s in steps)
                      or not vec.oracle, "%s: unexpected fail-closed stop" % vec.name)
            for problem in z.check_same_final(vecs, r["generated"]):
                check(False, problem)
            if root is not None:
                order = [v.name for v in vecs if v.oracle]
                oracle_compared += len(order)
                d = z.first_divergence(r["generated"], r["oracle"], order)
                check(d is None, "%s diverges from the oracle: %s" % (batch.name, json.dumps(d)[:600]))
        if root is None:
            print("oracle comparison skipped: " + z.skip_reason())
    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("z80 control/stack dispatch scenarios: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
