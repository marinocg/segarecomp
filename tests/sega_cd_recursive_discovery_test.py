#!/usr/bin/env python3
"""SEG-099-T004 (ADR 0218): bounded recursive loader discovery - the loads a PROVEN module itself performs.

Project-authored BIOS image + disc (tools/sega_cd_bios_services_fixture.py cases). With recursive_modules the walk follows a transfer that the entry proof accepted INTO a loaded module
(its instructions are decoded from the abstract memory holding the proven sector bytes) and finds the loads that module performs, with the same state, bounds and refusals. Data is never
walked because it decodes; a transfer into bytes that are not known to be the disc's (decoded in place, overwritten) is refused. usage: sega_cd_recursive_discovery_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_services_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
BUDGET = "3000000"
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
    done = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(fx / "disc" / "disc.cue"), "--module-table-roots", "--constant-dispatch", "--work-state", *flags], capture_output=True, text=True, timeout=300)
    return json.loads(done.stdout)


def build(fx, out, cache, *extra):
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(ROOT),
                           "--cc", CC, "--budget", BUDGET, "--jobs", "4", "--cache-dir", str(cache), *extra], capture_output=True, text=True, timeout=1800)
    status = out / "status.json"
    return done, json.loads(status.read_text()) if status.exists() else None


def run(program, fx, choice=0):
    extra = ["--set", "main:d7=%d" % choice] if choice else []
    out = subprocess.run([str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, *extra], capture_output=True, text=True, timeout=300)
    report = json.loads(out.stdout)
    block = bytes.fromhex(report.get("result_block", "")).ljust(16, b"\0")
    return report, [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]


def blocks(r):
    return {m["block"]: m for m in r["modules"]}


def classes(r):
    out = {}
    for x in r["refusals"]:
        out[x["reason"]] = out.get(x["reason"], 0) + 1
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="scd-recursive-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"
        fx, oracle = fixture(tmp, "recursive")
        A, B, R = (oracle["modules"][k]["lba"] for k in "ABR")
        data = (fx / "disc" / "track01.bin").read_bytes()

        # ---- boot SP loads A; A loads R (on a route the build pass does not take) ----
        flat = scan(fx)
        rec = scan(fx, "--recursive-modules")
        check(set(blocks(flat)) == {A, B} and set(blocks(rec)) == {A, B, R}, "recursive: without recursion R is invisible; with it the loads performed BY module A are found: R is proven (boot SP -> A -> R)")
        check(rec["complete"] and rec["survey"]["module_entries_walked"] >= 2 and blocks(rec)[R]["bases"] == oracle["modules"]["R"]["bases"] and
              hashlib.sha256(data[R * 2048:R * 2048 + 2048]).hexdigest() == blocks(rec)[R]["sha256"], "recursive: R is a verified one-sector source at its proven destination; the walk followed the proven entries (counted)")
        check(json.dumps(scan(fx, "--recursive-modules"), sort_keys=True) == json.dumps(rec, sort_keys=True), "recursive: deterministic")
        for order in (1, 2, 3):
            r = scan(fx, "--recursive-modules", "--scan-order", str(order))
            check(json.dumps(r["modules"], sort_keys=True) == json.dumps(rec["modules"], sort_keys=True) and r["complete"], "recursive: exploration order %d gives the same modules" % order)

        # ---- the supported build and launcher ----
        done, st = build(fx, tmp / "n-rec", cache)
        check(done.returncode == 0 and st and st["status"] == "ok" and st["stop"] == "none" and st["result"] == "idle", "build: the supported build succeeds; the build pass (route 0) never loads R")
        check(st["static_discovery"]["status"] == "admitted" and st["static_extents"] == 3 and len(st["extents"]) == 2, "build: A, B and R are admitted statically; only the IP and SP are resolved from evidence")
        program = tmp / "n-rec" / ("program" + EXE)
        before = hashlib.sha256(program.read_bytes()).hexdigest()
        r0, w0 = run(program, fx, 0)
        check(r0["result"] == "idle" and w0[:3] == [1, 0, 0] and w0[3] == oracle["markers"]["A"], "route 0: A runs (R is not loaded)")
        r3, w3 = run(program, fx, 3)
        check(r3["result"] == "idle" and w3[:3] == [1, 0, 1] and w3[5] == oracle["markers"]["C"], "route 3: A loads and runs R - never loaded by the build pass - from the same binary, no recompilation")
        check(hashlib.sha256(program.read_bytes()).hexdigest() == before, "the binary is unchanged by the runs")
        done, ctl = build(fx, tmp / "n-ctl", cache, "--static-discovery", "0")
        rc3, wc3 = run(tmp / "n-ctl" / ("program" + EXE), fx, 3)
        check(rc3["stop"]["class"] != "none" and wc3[2] == 0, "control: without the static producer R stops typed and never executes")

        # ---- loading cycle ----
        fc, oc = fixture(tmp, "cycle")
        c = scan(fc, "--recursive-modules")
        check(c["complete"] and c["states"] < 4096 and {A, B} <= set(blocks(c)), "cycle: A loads B and B loads A (jump chain): the walk reaches a fixed point, both modules once, no unbounded discovery")
        check(set(blocks(c)) == {A, B} and all(len(m["bases"]) == 1 for m in c["modules"]), "cycle: no duplicated or invented module")

        # ---- a module the loader decodes in place ----
        ft, ot = fixture(tmp, "transformed")
        t = scan(ft, "--recursive-modules")
        check(B not in blocks(t) and "loaded_bytes_unknown" in classes(t), "transformed: B is stored encoded and decoded in place before it runs: the executed bytes are not a disc span - refused (loaded_bytes_unknown), never guessed")
        check(A in blocks(t), "transformed: the independent module A is still proven")

        # ---- two modules at one guest base (overlay) ----
        fo, oo = fixture(tmp, "overlay")
        o = scan(fo, "--recursive-modules")
        check(set(blocks(o)) == {A, B} and blocks(o)[A]["bases"] == blocks(o)[B]["bases"], "overlay: two different verified sources at one guest base are both kept (selected by live bytes at run time), neither shadows the other")

        # ---- two guest bases of one module ----
        f2, o2 = fixture(tmp, "branch_two")
        b2 = scan(f2, "--recursive-modules")
        check(sorted(blocks(b2)[A]["bases"]) == [0x090000, 0x0B8000], "two bases: one source mapped at two guest bases is one module with two mappings (each analysed at its own address)")

        # ---- data loaded and never executed ----
        fn, on = fixture(tmp, "no_exec")
        n = scan(fn, "--recursive-modules")
        check(A not in blocks(n) and B in blocks(n) and "loaded_not_executed" in classes(n), "no_exec: a module that loads data and never transfers execution to it is not a module (loaded_not_executed)")

        # ---- recursion off keeps the previous scan ----
        fb, ob = fixture(tmp, "base")
        s0, s1 = scan(fb), scan(fb, "--recursive-modules")
        check({m["block"]: (m["size"], m["bases"], m["sha256"]) for m in s0["modules"]} == {m["block"]: (m["size"], m["bases"], m["sha256"]) for m in s1["modules"]},
              "base: recursion adds no module on a disc with no nested loader (sources, sizes and mappings are identical; it only proves more internal entries)")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
