#!/usr/bin/env python3
"""SEG-078-T003 (ADR 0205): the product `segarecomp sega-cd build` - the same fixed point as the Python driver, in the shipped CLI.

On project-authored fixtures (no commercial input) it checks that the native build
  - discovers the same extents and the same program identity as tools/sega_cd_bios_build.py --discover-extents (loader fixture: overlay, direct, duplicates);
  - prints `@stage` progress lines and a final `@result`, and writes status.json (a typed refusal ends the build at the frontier, the program still exists);
  - reuses its object cache (a second build compiles nothing and yields the identical program identity and link inputs);
  - produces the viewer variant (`--viewer 1` with the project-authored test host): `--describe` reports viewer true, the identity equals the headless program's;
  - fails with a typed stage/message, not an exception, for unreadable input, a missing runtime tree and a failing compiler.
usage: sega_cd_native_build_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import os
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_loader_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
HOST = ROOT / "tests" / "tools" / "sega_cd_live_test_host.c"
failures = []
EXE = ".exe" if os.name == "nt" else ""


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def native(out_dir, cue, bios, *extra, runtime=ROOT, cc=CC):
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(cue), "--bios", str(bios), "--output", str(out_dir), "--runtime-dir", str(runtime), "--cc", cc,
                           "--budget", "3000000", "--jobs", "4", *extra], capture_output=True, text=True, timeout=1800)
    status = out_dir / "status.json"
    return done, json.loads(status.read_text()) if status.exists() else None


def main():
    with tempfile.TemporaryDirectory(prefix="scd-native-") as tmp:
        tmp = pathlib.Path(tmp)
        for case in ("overlay", "duplicates"):
            fx = tmp / case
            subprocess.run([sys.executable, str(FIXTURE), "build", "--case", case, "--out", str(fx)], check=True, capture_output=True, text=True)
            cue, bios = fx / "disc" / "disc.cue", fx / "bios.bin"
            done, status = native(tmp / ("n-" + case), cue, bios)
            ref = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(cue), "--bios", str(bios), "--work", str(tmp / ("p-" + case)),
                                  "--discover-extents", "--budget", "3000000", "--timeout", "300"], capture_output=True, text=True, timeout=1800)
            ref = json.loads(ref.stdout)
            check(done.returncode == 0 and status and status["status"] == "ok", "%s: the native build succeeds" % case)
            lines = done.stdout.splitlines()
            check(any(l.startswith("@stage compile running") for l in lines) and any(l.startswith("@stage pass done") for l in lines) and lines[-1] == "@result ok",
                  "%s: progress stages and a final @result are printed" % case)
            check(status["extents"] == ref["extents"] and (status["frontier"] or None) == (ref["frontier"] or None),
                  "%s: the same extents and frontier as the Python driver" % case)
            check(status["program_identity"] == ref["program_identity"], "%s: the same program identity as the Python driver" % case)
            if case == "duplicates":
                check(status["frontier"]["refusal"] == "ambiguous_source" and not status["extents"] and (tmp / "n-duplicates" / ("program" + EXE)).exists(),
                      "duplicates: a typed refusal ends the build at the frontier; the program still exists")
            if case == "overlay":
                first = status
                program = tmp / "n-overlay" / ("program" + EXE)
                report = subprocess.run([str(program), "--disc-dir", str(fx / "disc"), "--bios", str(bios), "--instruction-budget", "3000000"], capture_output=True, text=True, timeout=300)
                check(json.loads(report.stdout)["result"] == "idle", "overlay: the built program runs to its end")
                objects = sorted(p.name for p in (tmp / "n-overlay" / "objects").glob("*.o"))
                # same output folder, same cache: nothing is compiled again
                done2, status2 = native(tmp / "n-overlay", cue, bios)
                check(status2 and status2["program_identity"] == first["program_identity"] and status2["extents"] == first["extents"] and
                      sorted(p.name for p in (tmp / "n-overlay" / "objects").glob("*.o")) == objects, "overlay: a rebuild reuses the object cache (no new object, identical program)")
                done3, status3 = native(tmp / "n-overlay-b", cue, bios, "--cache-dir", str(tmp / "n-overlay" / "objects"))
                check(status3 and status3["program_identity"] == first["program_identity"] and
                      sorted(p.name for p in (tmp / "n-overlay" / "objects").glob("*.o")) == objects, "overlay: a shared cache folder serves a second output folder")
                hashes = {hashlib.sha256((tmp / d / ("program" + EXE)).read_bytes()).hexdigest() for d in ("n-overlay", "n-overlay-b")}
                check(len(hashes) == 1 or os.name == "nt", "overlay: the two builds are byte-identical programs (a Windows PE link stamps a time: identity equality above stands there)")
                done4, status4 = native(tmp / "n-overlay-v", cue, bios, "--viewer", "1", "--viewer-host-source", str(HOST), "--cache-dir", str(tmp / "n-overlay" / "objects"))
                vexe = tmp / "n-overlay-v" / ("program-viewer" + EXE)
                check(status4 and status4["viewer"] is True and status4["viewer_program"] == "program-viewer" + EXE and vexe.exists(), "overlay: --viewer builds the viewer variant")
                head = json.loads(subprocess.run([str(program), "--describe"], capture_output=True, text=True).stdout)
                view = json.loads(subprocess.run([str(vexe), "--describe"], capture_output=True, text=True).stdout)
                check(head["viewer"] is False and view["viewer"] is True and head["program_identity"] == view["program_identity"] == status4["program_identity"],
                      "overlay: --describe: the viewer variant is the same program (identity), viewer true")
                nosdl, st = native(tmp / "n-nosdl", cue, bios, "--viewer", "1")
                check(nosdl.returncode != 0 and st and st["status"] == "failed" and st["stage"] == "viewer" and st["message"] == "sdl3_unavailable", "--viewer without SDL3 fails with the typed sdl3_unavailable")

        # ---- typed failures ----
        fx = tmp / "overlay"
        cue, bios = fx / "disc" / "disc.cue", fx / "bios.bin"
        done, st = native(tmp / "f-bios", cue, tmp / "missing.bin")
        check(done.returncode != 0 and st and st["status"] == "failed" and st["stage"] == "setup", "an unreadable BIOS fails at setup, typed")
        done, st = native(tmp / "f-runtime", cue, bios, runtime=tmp / "nowhere")
        check(done.returncode != 0 and st and st["stage"] == "setup" and "runtime" in st["message"], "a missing runtime tree fails at setup, typed")
        done, st = native(tmp / "f-cc", cue, bios, cc=str(tmp / "no-such-compiler"))
        check(done.returncode != 0 and st and st["status"] == "failed" and st["stage"] == "setup", "an unusable compiler fails at setup, typed")
        done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(cue)], capture_output=True, text=True)
        check(done.returncode == 2, "missing required options are a usage error (exit 2)")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
