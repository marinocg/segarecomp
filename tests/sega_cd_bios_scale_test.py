#!/usr/bin/env python3
"""SEG-098-T004 (ADR 0213): overlay-inventory scale and computed entries in the supported BIOS-image build (project-authored fixtures).

Scale: N distinct ~1.6 KiB modules behind the dispatcher, loaded over 1 or 4 Word RAM bases (overlay depth N or N/4). The measured bounds (64 disc images, 48 static
extents) hold, spot-checked first/middle/last modules run from the one binary, and one module over the static bound is the typed static_extent_bound_exceeded (nothing admitted).
Computed entries: module B's second routine is reached only through JMP (A0) (A0 loaded by LEA). With the default `hull` static range it is not compiled: typed stop, classified
`entry_not_compiled` inside an admitted extent; with `--static-range loaded` (every loaded byte of a proven module) it runs. usage: sega_cd_bios_scale_test.py <segarecomp> <c-compiler> <root>
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_unvisited_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
BUDGET = "6000000"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def fixture(tmp, name, *args):
    out = tmp / name
    return out, json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(out), *args], check=True, capture_output=True, text=True).stdout)


def build(fx, out, cache, *extra):
    subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                    "--budget", BUDGET, "--jobs", "4", "--cache-dir", str(cache), *extra], capture_output=True, text=True, timeout=3000)
    return json.loads((out / "status.json").read_text())


def run(program, fx, choice):
    out = subprocess.run([str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, "--set", "main:d7=%d" % choice],
                         capture_output=True, text=True, timeout=300)
    rep = json.loads(out.stdout)
    block = bytes.fromhex(rep.get("result_block", "")).ljust(16, b"\0")
    return rep, [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-scale-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"

        # ---- computed entries ----
        fx, oracle = fixture(tmp, "fx-computed", "--case", "computed_entry")
        marker = oracle["computed"]["marker"]
        st = build(fx, tmp / "o-hull", cache, "--pass-set", "main:d7=2")
        check(st["static_discovery"]["status"] == "admitted" and st["stop"] == "entry_not_compiled" and
              st["final_stop"] == {"class": "entry_not_compiled", "cpu": "sub", "location": "inside_admitted_extent"},
              "hull (default): the build pass stops at the computed transfer whose destination is beyond the traversed range; classified entry_not_compiled / inside an admitted extent")
        check(st["inventory"]["status"] == "blocked" and st["inventory"]["barriers"] == ["stop:entry_not_compiled"], "inventory: an uncompiled entry inside an admitted image is a typed barrier (`blocked`)")
        rep, words = run(tmp / "o-hull" / ("program" + EXE), fx, 2)
        check(rep["stop"]["class"] == "entry_not_compiled" and words[4] != marker, "hull: the same typed stop at run time; the uncompiled routine never executes")
        lines = (tmp / "o-hull" / "static-extents.txt").read_text().split()
        check(sorted(int(l.split(":")[3]) for l in lines)[0] < 2048, "hull: B's admitted extent is shorter than its loaded sector")
        st = build(fx, tmp / "o-loaded", cache, "--pass-set", "main:d7=2", "--static-range", "loaded")
        check(st["stop"] == "none" and st["final_stop"] is None and st["static_extents"] == 4, "loaded: every loaded byte of a proven module is admitted; the same route completes")
        check(st["inventory"]["status"] == "partial" and st["inventory"]["barriers"] == [], "inventory (loaded): the same route has no barrier: `partial`")
        lines = (tmp / "o-loaded" / "static-extents.txt").read_text().split()
        check(all(int(l.split(":")[3]) == 2048 for l in lines), "loaded: each extent is the whole loaded sector")
        rep, words = run(tmp / "o-loaded" / ("program" + EXE), fx, 2)
        check(rep["result"] == "idle" and words[4] == marker, "loaded: the computed destination runs (marker written) from the same binary")
        bad = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(tmp / "o-bad"), "--runtime-dir", str(ROOT),
                              "--cc", CC, "--static-range", "everything"], capture_output=True, text=True)
        check(bad.returncode == 2, "an unknown --static-range value is a usage error")

        # ---- scale ----
        for n, groups in ((48, 4), (24, 1)):
            fx, oracle = fixture(tmp, "fx-%d-%d" % (n, groups), "--many", str(n), "--many-groups", str(groups), "--many-nops", "400")
            st = build(fx, tmp / ("o-%d-%d" % (n, groups)), cache)
            check(st["status"] == "ok" and st["static_discovery"]["status"] == "admitted" and st["static_extents"] == n and len(st["extents"]) == 2,
                  "%d modules over %d base(s) (overlay depth %d): all admitted statically, only IP and SP resolved from evidence" % (n, groups, n // groups))
            good = True
            for k in sorted({0, n // 2, n - 1}):
                rep, words = run(tmp / ("o-%d-%d" % (n, groups)) / ("program" + EXE), fx, k + 1)
                good = good and rep["result"] == "idle" and words[0] == 1 and words[3] == oracle["many"][k]["marker"]
            check(good, "%d/%d: the first, middle and last module run from the one binary" % (n, groups))
        fx, oracle = fixture(tmp, "fx-49", "--many", "49", "--many-groups", "4", "--many-nops", "400")
        st = build(fx, tmp / "o-49", cache)
        check(st["static_discovery"]["status"] == "static_extent_bound_exceeded" and st["static_extents"] == 0, "49 modules: the typed static_extent_bound_exceeded, nothing is admitted statically")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
