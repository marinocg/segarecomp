#!/usr/bin/env python3
"""SEG-101 (ADR 0228, 0229): the multi-route runner on a project-authored disc. One frozen file-first program; the build route (A) defines the build-observed
set; routes that load B and C are counted as previously unvisited; the program, its directory and the PATH=/nonexistent rule are checked by the tool.
usage: sega_cd_m5_routes_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_unvisited_fixture.py"
TOOL = ROOT / "tools" / "sega_cd_m5_routes.py"
EXE = ".exe" if os.name == "nt" else ""
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-m5-routes-") as tmp:
        tmp = pathlib.Path(tmp)
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx), "--case", "base", "--iso"], check=True, capture_output=True)
        out = tmp / "build"
        done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                               "--budget", "3000000", "--jobs", "4", "--cache-dir", str(tmp / "objects"), "--static-discovery", "0", "--file-first", "structural"], capture_output=True, text=True, timeout=1800)
        program = out / ("program" + EXE)
        check(done.returncode == 0 and program.exists(), "the frozen program is built once, with no input and no module list")
        base = [str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", "3000000"]
        subprocess.run(base + ["--report", str(tmp / "r0.json")], check=True, capture_output=True, timeout=300)
        private = tmp / "private.json"
        done = subprocess.run([sys.executable, str(TOOL), "--program", str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--work", str(tmp / "work"), "--budget", "3000000",
                               "--observed-from", str(tmp / "r0.json"), "--private", str(private), "--route", "R0:::", "--route", "R2|||||--set main:d7=2", "--route", "R3|||||--set main:d7=3", "--expect", "R0=none", "--expect", "R2=none", "--expect", "R3=none"],
                              capture_output=True, text=True, timeout=600)
        check(done.returncode == 0, "the runner succeeds and the frozen program is unchanged after every route (exit %d)" % done.returncode)
        if done.returncode != 0:
            print(done.stderr[-1500:])
        res = json.loads(done.stdout)
        r = res["routes"]
        check(res["build_observed_images"] == 1 and r["R0"]["images_previously_unvisited_executed"] == 0 and r["R0"]["images_executed"] == 1, "the build route executes only the build-observed module")
        check(r["R2"]["images_previously_unvisited_executed"] == 1 and r["R2"]["unvisited_retired"] > 0 and r["R2"]["images_build_observed_executed"] == 0, "route 2: module B was compiled, never executed by the build route, and counts once")
        check(r["R3"]["images_previously_unvisited_executed"] == 1 and r["R3"]["binds"] == 2, "route 3: module C at two bases is one previously unvisited image")
        check(all(x["program_unchanged"] and x["empty_path"] and x["refusals"] == {} and x["retired_dynamic"] > 0 for x in r.values()) and res["program_unchanged_after_all_routes"],
              "every route ran with PATH=/nonexistent, with no refusal, with dynamic bodies retiring, and the program directory unchanged")
        check("unvisited_ids" not in done.stdout and json.loads(private.read_text())["private"]["R2"]["unvisited_ids"], "aggregate output names no image id; the private file does")
        # negative regressions: the runner must FAIL on a wrong expected stop, a missing expectation and a missing/different reference frame
        def runner(*extra):
            return subprocess.run([sys.executable, str(TOOL), "--program", str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--work", str(tmp / "w2"), "--budget", "3000000",
                                   "--observed-from", str(tmp / "r0.json"), *extra], capture_output=True, text=True, timeout=600)
        bad = runner("--route", "R0:::", "--expect", "R0=pcm_unsupported")
        check(bad.returncode == 5 and "expected pcm_unsupported" in bad.stdout, "a route that ends differently from its pre-registered stop fails the evaluation")
        check(runner("--route", "R0:::").returncode == 5, "a route with no pre-registered expectation fails the evaluation")
        (tmp / "emptyref").mkdir()
        miss = runner("--route", "R0|||%s|5" % (tmp / "emptyref"), "--expect", "R0=none")
        check(miss.returncode == 5 and "missing or different" in miss.stdout, "an expected reference frame that is missing or different fails the evaluation")
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
