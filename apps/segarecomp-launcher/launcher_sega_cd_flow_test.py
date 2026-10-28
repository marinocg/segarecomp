#!/usr/bin/env python3
"""SEG-078-T002 (ADR 0204): launcher -> verified program -> SDL viewer -> input -> stop / exit, on a project-authored fixture.

The launcher is run without a window (`Segarecomp --sega-cd ...`: the same core and the same process orchestration as the Play button) against
  * the REAL viewer variant of the live fixture (tools/sega_cd_bios_live_fixture.py built with `sega_cd_bios_build.py --viewer`, SDL3, dummy
    video/audio drivers; keys are pushed by the viewer's automation seam), and
  * the program double (apps/segarecomp-launcher/fake_scd_program.cpp) for typed stops and refusal shapes a real build cannot produce on demand.
Checked: the program is verified against the BIOS and the disc before it starts; the viewer opens, the arrow keys reach the pad, closing the window
is "Closed", the recording replays to the identical report; a mismatched BIOS, a missing disc file, a headless-only build, a program that cannot state
itself (an older build), a changed program and an absent program are typed refusals; the automatic build is the typed unsupported state; a typed
execution stop is reported with its class.

Skipped (exit 0) without SDL3 or off Linux.

usage: launcher_sega_cd_flow_test.py <Segarecomp> <fake_scd_program> <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

LAUNCHER, FAKE, SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], pathlib.Path(sys.argv[5]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_bios_build as build_driver  # noqa: E402

FRAME = 262 * 3420 * 500_000
FIRST_ONSET = 111_858 * 500_000
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def lines(out):
    result = {}
    for line in out.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            result.setdefault(key.split()[0], value)
    return result


def launch(tmp, args, env=None, name="lib"):
    library = tmp / name
    library.mkdir(exist_ok=True)
    full = {**os.environ, "SEGARECOMP_LIBRARY_DIR": str(library), "SEGARECOMP_HOME": str(tmp / "no-home"), "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy", "SEGARECOMP_VIEWER_UNTHROTTLED": "1", "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1",
            "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "settings.cfg"), **(env or {})}
    out = subprocess.run([LAUNCHER, "--sega-cd", *map(str, args)], capture_output=True, text=True, timeout=600, env=full)
    return out.returncode, out.stdout, out.stderr, library


def main():
    if sys.platform != "linux" or build_driver.find_sdl3() is None:
        print("SKIP: SDL3 or a Linux host is not available")
        return 0
    with tempfile.TemporaryDirectory(prefix="scd-flow-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_live_fixture.py"), "build", "--out", str(fx)], check=True, capture_output=True)
        bios = fx / "bios.bin"
        built = subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_build.py"), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                                "--bios", str(bios), "--work", str(tmp / "work"), "--pass-set", "main:d6=3", "--viewer"], capture_output=True, text=True, timeout=1800)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok"), "the fixture program and its viewer variant build")
        if not summary.get("ok"):
            return 1
        headless, viewer = pathlib.Path(summary["program"]), pathlib.Path(summary["viewer"]["program"])
        cue = disc / "disc.cue"
        keys = tmp / "keys.txt"
        keys.write_text("3 down Right\n11 up Right\n14 down Up\n18 up Up\n25 quit x\n")
        rec = tmp / "session.txt"

        # ---- the good path: add, verify, run in the viewer, press keys, close ----
        code, out, err, library = launch(tmp, ["--bios", bios, "--cue", cue, "--program", viewer, "--record", rec], {"SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)})
        got = lines(out)
        check(code == 0 and got.get("program") == "verified" and got.get("state") == "Ready to play" and got.get("platform") == "sega-cd",
              "launcher: the viewer program is verified against the BIOS and the disc and Ready to play (%s)" % out.replace("\n", " | ")[:300])
        check(got.get("run", "").startswith("exited code=0") and got.get("outcome") == "Closed." and got.get("live", "").startswith("quit frames=25 presented=24"),
              "launcher: the viewer opened, ran 25 frames, presented 24, and closing the window is 'Closed.'")
        lib = json.loads((library / "library.json").read_text())
        entry = lib["games"][0]
        check(lib["schema"] == 2 and entry["platform"] == "sega-cd" and "sega_cd" in entry and entry["sha256"] == got["entry"].split(":")[1], "library: a Sega CD entry under schema 2")
        text = (library / "library.json").read_text()
        check(str(bios) in text and str(viewer) in text and "SEGA" not in text, "library: only references and digests are stored")
        lines_rec = rec.read_text().splitlines()
        check(lines_rec[0] == "segarecomp-input-script 1" and lines_rec[1:] == ["%d 1 press RIGHT" % (FIRST_ONSET + 3 * FRAME), "%d 1 release RIGHT" % (FIRST_ONSET + 11 * FRAME),
                                                                               "%d 1 press UP" % (FIRST_ONSET + 14 * FRAME), "%d 1 release UP" % (FIRST_ONSET + 18 * FRAME)],
              "recording: the four key transitions at their frame boundaries")
        report1 = json.loads(next((library / "sega-cd").glob("*/last-report.json")).read_text())

        # ---- replay without a window: the recorded script reproduces the run ----
        code, out, err, library2 = launch(tmp, ["--bios", bios, "--cue", cue, "--program", viewer, "--replay", rec, "--windowless"], {"SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)}, "lib2")
        got2 = lines(out)
        report2 = json.loads(next((library2 / "sega-cd").glob("*/last-report.json")).read_text())
        strip = lambda r: {k: v for k, v in r.items() if k != "live"}   # noqa: E731
        check(code == 0 and got2.get("live", "").startswith("quit frames=25 presented=24") and strip(report1)["input"]["events"] == 4 and strip(report1) == strip(report2),
              "replay: the recording replays to the identical report (registers, digests, input counters)")

        # ---- a replay without a window ends by itself shortly after its last event (nobody closes a window that is not there) ----
        code, out, err, library3 = launch(tmp, ["--bios", bios, "--cue", cue, "--program", viewer, "--replay", rec, "--windowless"], name="lib2b")
        got3 = lines(out)
        report3 = json.loads(next((library3 / "sega-cd").glob("*/last-report.json")).read_text())
        last_event = FIRST_ONSET + 18 * FRAME
        check(code == 0 and report3["result"] == "deadline" and report3["ticks"] < last_event + 121 * FRAME and 130 <= report3["live"]["frames"] <= 141,
              "replay without a window: ends at its last event + 120 frames by itself (%s frames), reported as the time limit" % report3["live"]["frames"])
        check("time limit" in got3.get("outcome", ""), "replay without a window: the outcome says the time limit was reached")

        # ---- typed refusals ----
        other_bios = tmp / "other-bios.bin"
        other_bios.write_bytes(bios.read_bytes()[:-1] + b"\xff")
        code, out, _, _ = launch(tmp, ["--bios", other_bios, "--cue", cue, "--program", viewer], name="lib3")
        got = lines(out)
        check(code == 5 and got.get("program", "").startswith("refused program_bios_mismatch") and got.get("state") == "No program yet (can be built)" and got.get("build") == "unsupported_build",
              "refusal: a program built for another BIOS is program_bios_mismatch, and the automatic build is the typed unsupported state")
        moved = tmp / "disc2"
        shutil.copytree(disc, moved)
        victim = [p for p in moved.iterdir() if p.suffix != ".cue"][0]
        victim.rename(victim.with_name("renamed" + victim.suffix))
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", moved / "disc.cue", "--program", viewer], name="lib4")
        check(code == 5 and lines(out).get("program", "").startswith("refused disc_file_missing"), "refusal: a disc file the program names is missing beside the CUE")
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--program", headless], name="lib5")
        got = lines(out)
        check(code == 5 and got.get("program", "").startswith("refused program_headless_only"), "refusal: the headless build of the same program is not playable in the viewer")
        old = tmp / "old-program"
        old.write_text("#!/bin/sh\nexit 2\n")
        old.chmod(0o755)
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--program", old], name="lib6")
        check(code == 5 and lines(out).get("program", "").startswith("refused program_unverifiable"), "refusal: a program that cannot state itself (an older build) is unverifiable, never trusted")
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue], name="lib7")
        got = lines(out)
        check(code == 0 and got.get("program") == "none" and got.get("state") == "No program yet (can be built)" and got.get("build") == "unsupported_build" and "run" not in got,
              "no program: the entry is added, nothing is run, and the automatic build is the typed unsupported state")
        code, out, _, _ = launch(tmp, ["--bios", tmp / "absent.bin", "--cue", cue, "--program", viewer], name="lib8")
        check(code == 3 and "refusal=bios_missing" in out, "refusal: a missing BIOS file refuses the whole add")

        # ---- a program that changed after it was verified is stale and is not started ----
        mutable = tmp / "mutable-program"
        shutil.copy(viewer, mutable)
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--program", mutable, "--windowless"], {"SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)}, "lib11")
        check(code == 0 and lines(out).get("program") == "verified", "a copy of the program is verified and run")
        with open(mutable, "ab") as f:
            f.write(b"\0")
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--windowless"], {"SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)}, "lib11")
        got = lines(out)
        check(code == 5 and got.get("state") == "Stale program" and got.get("refusal") == "program_changed" and "run" not in got,
              "stale: the program changed after it was verified - it is not started (program_changed)")

        # ---- a typed execution stop, with the program double ----
        double = tmp / "double"
        shutil.copy(FAKE, double)
        statement = {"describe": 1, "runtime_abi": 1, "profile": "bios_image", "viewer": True, "program_identity": "7" * 64, "disc_identity": "8" * 64,
                     "bios": {"size": bios.stat().st_size, "sha256": hashlib.sha256(bios.read_bytes()).hexdigest()}, "disc_files": []}
        for f in sorted(p for p in disc.iterdir() if p.suffix != ".cue"):
            statement["disc_files"].append({"name": f.name, "size": f.stat().st_size, "sha256": hashlib.sha256(f.read_bytes()).hexdigest()})
        pathlib.Path(str(double) + ".describe").write_text(json.dumps(statement) + "\n")
        stop_report = tmp / "stop.json"
        stop_report.write_text(json.dumps({"result": "stopped", "stop": {"class": "entry_not_compiled", "cpu": 0, "pc": 1}, "input": {"events": 0}, "live": {"outcome": "machine_stop", "frames": 957, "presented": 956}}))
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--program", double], {"FAKE_SCD_EXIT": "10", "FAKE_SCD_REPORT": str(stop_report)}, "lib9")
        got = lines(out)
        check(code == 0 and got.get("stop") == "entry_not_compiled" and "Typed execution stop: entry_not_compiled" in out and got.get("run", "").startswith("exited code=10"),
              "typed stop: the class of the program's stop is reported (entry_not_compiled), exit 10 is a stop, not a crash")
        code, out, _, _ = launch(tmp, ["--bios", bios, "--cue", cue, "--program", double], {"FAKE_SCD_EXIT": "21"}, "lib10")
        check(code == 4 and "BIOS image does not match" in out, "typed refusal exit 21 of the program: reported as the BIOS refusal, a failure status for the launcher")

        # ---- SEG-078-T003 (ADR 0205): BIOS + disc -> automatic build (extent discovery) -> verified viewer program -> run in the SDL3 viewer ----
        # The loader fixture's modules reach the sub CPU only through the CD data path, so the build must DISCOVER where they came from. The launcher is
        # given nothing but the two files: no program, no extent, no recipe.
        package = common.stage_launcher_package(tmp / "package", SEGARECOMP, CC, os.environ.get("SDL3_PREFIX", "/usr"))
        loader = tmp / "loader"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "overlay", "--out", str(loader)], check=True, capture_output=True)
        env = {"SEGARECOMP_HOME": str(package), "SEGARECOMP_CACHE_DIR": str(tmp / "cache"), "SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys), "SEGARECOMP_SCD_BUILD_ARGS": "--budget 3000000"}
        code, out, err, library = launch(tmp, ["--bios", loader / "bios.bin", "--cue", loader / "disc" / "disc.cue", "--build"], env, "lib-build")
        got = lines(out)
        check(code == 0 and got.get("build") == "ok" and "\nprogram=verified\n" in out and got.get("program") == "none" and got.get("state") == "Ready to play",
              "automatic build: BIOS + disc -> built, verified and Ready to play with no program, extent or recipe given (%s)" % out.replace("\n", " | ")[:400])
        check("No unexplained code region" in got.get("build_summary", "") and got.get("run", "").startswith("exited code="), "automatic build: the build reports full coverage and the viewer ran the built program (%s)" % got.get("run"))
        entry_dir = next((library / "sega-cd").glob("*"))
        status = json.loads((entry_dir / "build" / "status.json").read_text())
        check(status["status"] == "ok" and len(status["extents"]) == 2 and status["frontier"] is None and status["viewer"] is True and not (entry_dir / "build.partial").exists(),
              "automatic build: the product recorded two discovered disc extents, no unexplained frontier, a viewer variant, and left no partial folder")
        text = (library / "library.json").read_text()
        check(str(entry_dir / "build") in text and "SEGA" not in text, "automatic build: the library stores only the reference to the built program")
        # an unsupported installation is a typed refusal, not a crash
        code, out, _, _ = launch(tmp, ["--bios", loader / "bios.bin", "--cue", loader / "disc" / "disc.cue", "--build"], {"SEGARECOMP_HOME": str(tmp / "no-home")}, "lib-nobuild")
        check(code == 5 and lines(out).get("build") == "unsupported_build", "automatic build: an installation without the Sega CD runtime refuses with the typed unsupported_build")
        # a disc whose code is not on the disc builds, stops honestly, and the program says so
        gen = tmp / "generated"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "generated", "--out", str(gen)], check=True, capture_output=True)
        code, out, _, library2 = launch(tmp, ["--bios", gen / "bios.bin", "--cue", gen / "disc" / "disc.cue", "--build"], env, "lib-build2")
        got = lines(out)
        check(code == 0 and got.get("build") == "ok" and "stops at the first code it cannot" in got.get("build_summary", "") and got.get("stop") == "entry_not_compiled",
              "automatic build: generated code is never guessed - the build says where it stops and the program ends with the typed stop (%s)" % got.get("build_summary", "")[:200])
        # ---- SEG-097-T003 (ADR 0208): the EXPERIMENTAL Optimized choice. Off by default (the build above is Compatibility); chosen explicitly it is the product's own
        # option, the summary says it is experimental, status.json records the effective policy, and the program stops only at a typed uncompiled entry. ----
        code, out, err, library3 = launch(tmp, ["--bios", loader / "bios.bin", "--cue", loader / "disc" / "disc.cue", "--build", "--aot-policy", "optimized"], env, "lib-build-exp")
        got = lines(out)
        status = json.loads(next((library3 / "sega-cd").glob("*")).joinpath("build", "status.json").read_text())
        check(code == 0 and got.get("build") == "ok" and "experimental_optimized" in got.get("build_policy", "") and "Experimental Optimized" in got.get("build_summary", ""),
              "experimental build: the summary says it is the experimental policy (%s)" % got.get("build_policy"))
        check(status["aot_policy"]["requested"] == "optimized" and status["aot_policy"]["effective"] == "selective" and not status["aot_policy"]["fallback"] and
              status["aot_policy"]["admitted_entries"] < status["aot_policy"]["broad_entries"],
              "experimental build: status.json records the product's effective policy (admitted %s of %s)" % (status["aot_policy"]["admitted_entries"], status["aot_policy"]["broad_entries"]))
        check(got.get("stop") in (None, "entry_not_compiled", "image_entry_not_compiled"), "experimental build: the program ends normally or at a typed uncompiled entry (%s)" % got.get("stop"))
        library_text = (library3 / "library.json").read_text()
        check('"experimental_m3": true' in library_text and '"program_experimental": true' in library_text, "experimental build: the library records the choice and the program's policy")
        check('experimental' not in (library / "library.json").read_text(), "the default (Compatibility) build leaves no experimental member in the library record")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
