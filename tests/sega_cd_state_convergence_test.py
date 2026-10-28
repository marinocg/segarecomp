#!/usr/bin/env python3
"""SEG-099-T003 (ADR 0217): state convergence in the static loader scan.

The scan keeps exact abstract states up to a per-location threshold; beyond it the states at that location are JOINED (identical knowledge is kept, any difference becomes
unknown - never a chosen value). Loops whose state keeps changing therefore reach a fixed point instead of exhausting the 4,096-state bound, without raising any limit and
without admitting a module the proofs do not support. Project-authored discs (tools/sega_cd_bios_services_fixture.py cases); scan-level only (no build, no execution).
usage: sega_cd_state_convergence_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_services_fixture.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def fixture(tmp, case):
    out = tmp / ("fx-" + case)
    oracle = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(out), "--case", case], check=True, capture_output=True, text=True).stdout)
    return out, oracle


def scan(fx, *flags):
    done = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(fx / "disc" / "disc.cue"), "--module-table-roots", "--constant-dispatch", "--work-state", *flags],
                          capture_output=True, text=True, timeout=300)
    return json.loads(done.stdout)


def blocks(report):
    return {m["block"]: m for m in report["modules"]}


def classes(report):
    out = {}
    for r in report["refusals"]:
        out[r["reason"]] = out.get(r["reason"], 0) + 1
    return out


def no_false_admission(fx, oracle, report):
    """Every module is a real file of the disc, with the right digest, at a destination the program really uses."""
    data = (fx / "disc" / "track01.bin").read_bytes()
    valid = {m["lba"]: m for m in oracle["modules"].values()}
    allowed_bases = {40: {0x090000, 0x0B8000, 0x0A0000, 0x0B0000}, 44: {0x0A0000}, 46: {0x0B0000}}   # A's extra bases are the first iterations of the loop_memory_base arithmetic (BASE_A + k * $10000)
    for m in report["modules"]:
        v = valid.get(m["block"])
        if v is None or m["size"] != 2048 * v["sectors"] or hashlib.sha256(data[m["block"] * 2048:m["block"] * 2048 + m["size"]]).hexdigest() != m["sha256"]:
            return False
        if not set(m["bases"]) <= allowed_bases[m["block"]]:
            return False
    return True


def main():
    with tempfile.TemporaryDirectory(prefix="scd-converge-") as tmp:
        tmp = pathlib.Path(tmp)
        fb, ob = fixture(tmp, "base")
        A, B = ob["modules"]["A"]["lba"], ob["modules"]["B"]["lba"]

        # ---- loops whose state keeps changing ----
        for case, why in (("loop_pointer", "a counted read loop over a known advancing pointer (6,000 exact iterations)"),
                          ("loop_unknown_exit", "a register that grows without bound while the exit depends on an unknown value"),
                          ("loop_memory", "a work variable that changes on every iteration")):
            fx, oc = fixture(tmp, case)
            off = scan(fx)
            on = scan(fx, "--state-convergence")
            check(not off["complete"] and classes(off).get("scan_limit_exceeded", 0) >= 1 and off["modules"] == [], "%s: %s exhausts the state bound without convergence (typed, nothing admitted)" % (case, why))
            check(on["complete"] and on["states"] < 4096 and on["survey"]["joins"] > 0, "%s: with convergence the scan completes in %d states (%d joins, %d widenings), the bound unchanged" % (case, on["states"], on["survey"]["joins"], on["survey"]["widenings"]))
            check(set(blocks(on)) == {A, B} and no_false_admission(fx, oc, on), "%s: the independent knowledge survives the loop: A and B are still proven, no false admission" % case)
            check(json.dumps(scan(fx, "--state-convergence"), sort_keys=True) == json.dumps(on, sort_keys=True), "%s: deterministic" % case)

        # ---- a loop that changes the destination itself: unknown, never invented ----
        fx, oc = fixture(tmp, "loop_memory_base")
        on = scan(fx, "--state-convergence")
        a_bases = set(blocks(on).get(A, {"bases": []})["bases"])
        check(on["complete"] and on["states"] < 4096 and any(c in classes(on) for c in ("destination_unknown", "lba_unknown", "destination_unsupported")) and no_false_admission(fx, oc, on) and all((b - 0x090000) % 0x10000 == 0 for b in a_bases),
              "loop_memory_base: a destination the loop keeps changing: the exact iterations give the bases the program's own arithmetic produces (BASE + k * $10000), the widened remainder is unknown (a typed refusal); nothing else is chosen")
        fx, oc = fixture(tmp, "loop_pointer_late")
        on = scan(fx, "--state-convergence")
        check(on["complete"] and on["states"] < 4096 and A not in blocks(on) and B not in blocks(on) and "destination_unknown" in classes(on) and no_false_admission(fx, oc, on),
              "loop_pointer_late: a pointer widened to unknown makes a store loop an unknown store: the work area stored before it is forgotten (destination_unknown) - converged, sound, less precise")
        fx, oc = fixture(tmp, "loop_pointer_early_store")
        on = scan(fx, "--state-convergence")
        check(on["complete"] and on["states"] < 4096 and A not in blocks(on) and B not in blocks(on) and "lookup_key_unknown" in classes(on) and no_false_admission(fx, oc, on),
              "loop_pointer_early_store: an unknown-pointer store loop BEFORE the program's lookup makes the file name (image data) unknown: the directory search has an unknown key, the typed refusal lookup_key_unknown, no candidate file is proposed")

        # ---- two candidate destinations must not merge into an invented third ----
        fx, oc = fixture(tmp, "branch_two")
        exact = scan(fx, "--state-convergence")
        check(A in blocks(exact) and sorted(blocks(exact)[A]["bases"]) == [0x090000, 0x0B8000] and no_false_admission(fx, oc, exact), "branch_two: within the threshold both destinations are explored exactly: A at both bases")
        forced = scan(fx, "--converge-always", "--widen-after", "1")
        check(forced["complete"] and no_false_admission(fx, oc, forced), "branch_two: with joins forced at once the destinations meet at the shared loader: whatever is admitted is at one of the two real bases")
        check(forced["survey"]["widenings"] > 0 and (A not in blocks(forced) or set(blocks(forced)[A]["bases"]) <= {0x090000, 0x0B8000}), "branch_two: the join lost the destination (unknown) rather than choosing one or inventing a value")

        # ---- an unknown alias in a loop; a computed transfer stays unresolved ----
        fx, oc = fixture(tmp, "loop_alias")
        on = scan(fx, "--state-convergence")
        check(on["complete"] and A not in blocks(on) and B in blocks(on) and "destination_unknown" in classes(on) and no_false_admission(fx, oc, on),
              "loop_alias: stores through an unknown pointer in a loop: the scan converges, the work area is forgotten, A is refused (destination_unknown), B is proven")
        fx, oc = fixture(tmp, "computed")
        on = scan(fx, "--state-convergence")
        check(on["complete"] and A not in blocks(on) and "computed_control_unresolved" in classes(on) and "entry_unknown" in classes(on) and no_false_admission(fx, oc, on),
              "computed: a computed transfer to an unknown target remains unresolved after convergence (computed_control_unresolved, entry_unknown)")

        # ---- repeatability across exploration orders ----
        for case in ("base", "loop_pointer", "branch_two", "loop_alias", "two_paths", "loop_unknown_exit"):
            fx, oc = fixture(tmp, case)
            results, full = [], []
            for order in (0, 1, 2, 3):
                r = scan(fx, "--state-convergence", "--scan-order", str(order))
                results.append(json.dumps([r["modules"], r["complete"]], sort_keys=True))
                full.append(json.dumps(sorted(classes(r).items())))
            check(len(set(results)) == 1, "%s: the proven modules and the completeness do not depend on the exploration order (4 orders)" % case)
            if case == "base":
                check(len(set(full)) == 1, "base: with no join at all the refusal classes are identical too (joins are the only order-dependent step)")

        # ---- the previous scan is unchanged when convergence is off ----
        r1 = scan(fb)
        r2 = scan(fb, "--state-convergence")
        check(set(blocks(r1)) == set(blocks(r2)) == {A, B} and r1["survey"]["joins"] == 0, "base: convergence changes nothing on a loader that never needed it (same modules; no joins without it)")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
