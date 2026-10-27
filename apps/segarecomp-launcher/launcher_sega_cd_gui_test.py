#!/usr/bin/env python3
"""SEG-078-T002 (ADR 0204): the Sega CD game in the REAL launcher window (dummy video driver, software renderer, scripted).

The window's own code paths run: the "Add a Sega CD game" form on the Import page, the Details panel and its states, Play (the verification worker, the
process orchestration, the SDL3 viewer of the project-authored fixture as a child process with scripted keys), the typed end of the run, the recording
list, the windowless replay, the platform filter, and the typed refusals of the unsupported build and of programs that do not belong to the BIOS and disc.

Skipped (exit 0) without SDL3 or off Linux.

usage: launcher_sega_cd_gui_test.py <Segarecomp> <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

LAUNCHER, SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_bios_build as build_driver  # noqa: E402

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def run_window(tmp, name, script, extra_env=None):
    lib = tmp / name
    lib.mkdir(exist_ok=True)
    script_path, trace_path = tmp / (name + ".script"), tmp / (name + ".trace")
    script_path.write_text("\n".join(script) + "\n")
    trace_path.unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_LIBRARY_DIR=str(lib), SEGARECOMP_CACHE_DIR=str(tmp / (name + "-cache")),
               SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="1080x740", SEGARECOMP_VIEWER_UNTHROTTLED="1",
               SEGARECOMP_VIEWER_NO_STOP_SCREEN="1", SEGARECOMP_VIEWER_SETTINGS=str(tmp / "viewer.cfg"), SEGARECOMP_GUI_SCRIPT=str(script_path),
               SEGARECOMP_GUI_TRACE=str(trace_path))
    env.setdefault("SEGARECOMP_HOME", str(tmp / "no-home"))
    env.update(extra_env or {})
    subprocess.run([LAUNCHER], env=env, timeout=300, check=True, capture_output=True)
    return trace_path.read_text().splitlines() if trace_path.exists() else [], lib


def main():
    if sys.platform != "linux" or build_driver.find_sdl3() is None:
        print("SKIP: SDL3 or a Linux host is not available")
        return 0
    with tempfile.TemporaryDirectory(prefix="scd-gui-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_live_fixture.py"), "build", "--out", str(fx)], check=True, capture_output=True)
        bios, cue = fx / "bios.bin", disc / "disc.cue"
        built = subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_build.py"), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(cue), "--bios", str(bios),
                                "--work", str(tmp / "work"), "--pass-set", "main:d6=3", "--viewer"], capture_output=True, text=True, timeout=1800)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok"), "the fixture program and its viewer variant build")
        if not summary.get("ok"):
            return 1
        headless, viewer = summary["program"], summary["viewer"]["program"]
        keys = tmp / "keys.txt"
        keys.write_text("3 down Right\n11 up Right\n14 down Up\n18 up Up\n25 quit x\n")

        # ---- window A: add with a program, play with a recording, replay, filter, unsupported build ----
        trace, lib = run_window(tmp, "a", [
            "frames 8", "page import", "frames 6", "scd_form %s|%s|%s" % (bios, cue, viewer), "frames 4", "scd_add", "wait_scd", "frames 10",
            "expect page=details", "expect scd_state=Ready to play", "expect status=Ready to play", "expect can_play=1", "expect detail=disc",
            "scd_record on", "play", "frames 4", "wait_scd", "wait_play_end", "frames 10",
            "expect scd_outcome=Closed.", "expect scd_recordings=1", "expect playing=0",
            "scd_replay windowless", "frames 4", "wait_scd", "wait_play_end", "frames 10",
            "expect scd_outcome=Closed. Replay: 25 frames, 4 input events.",
            "platform_filter 3", "frames 3", "expect visible=1", "platform_filter 1", "frames 3", "expect visible=0", "platform_filter 0",
            "build", "frames 6", "expect modal=1", "quit"], {"SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)})
        bad = [l for l in trace if l.startswith("expect FAIL")]
        check(not bad and sum(l.startswith("expect ok") for l in trace) >= 12, "window A: every expectation held (%s)" % (bad or "%d ok" % sum(l.startswith("expect ok") for l in trace)))
        text = "\n".join(trace)
        check("scd=added program=verified" in text and "scd=run_started windowless=0 recording=1" in text and "scd=run_exited code=0" in text and "live=quit" in text,
              "window A: added with a verified program, ran in the viewer with a recording, closed with exit 0")
        check("scd=run_started windowless=1" in text, "window A: the replay ran without a window")
        check("build=refused reason=unsupported_build platform=sega-cd" in text, "window A: Build is the typed unsupported state when the installation has no Sega CD runtime")
        stored = json.loads((lib / "library.json").read_text())
        check(stored["schema"] == 2 and stored["games"][0]["platform"] == "sega-cd" and stored["games"][0]["last_run"] == "clean", "window A: the library holds the Sega CD game and the clean run")

        # ---- window B: no program, then refused programs, then the right one ----
        other = tmp / "other-bios.bin"
        other.write_bytes(bios.read_bytes()[:-1] + b"\xff")
        trace, lib = run_window(tmp, "b", [
            "frames 8", "page import", "frames 6", "scd_form %s|%s|" % (bios, cue), "frames 4", "scd_add", "wait_scd", "frames 10",
            "expect page=details", "expect scd_state=No program yet (can be built)", "expect can_play=0", "expect scd_refusal=program_missing",
            "scd_attach %s" % headless, "wait_scd", "frames 6", "expect modal=1", "key Escape", "frames 6", "expect modal=0", "expect scd_state=No program yet (can be built)",
            "scd_attach %s" % viewer, "wait_scd", "frames 8", "expect scd_state=Ready to play", "expect can_play=1",
            "scd_verify", "wait_scd", "frames 8", "expect scd_state=Ready to play", "quit"])
        bad = [l for l in trace if l.startswith("expect FAIL")]
        check(not bad and sum(l.startswith("expect ok") for l in trace) >= 8, "window B: every expectation held (%s)" % (bad or "%d ok" % sum(l.startswith("expect ok") for l in trace)))
        text = "\n".join(trace)
        check("scd=program_refused refusal=program_headless_only" in text and "scd=program_attached" in text, "window B: a headless-only program is refused with its typed reason, the viewer program is then attached")

        # ---- window C: a moved BIOS file is 'inputs not configured'; a changed program is stale ----
        trace, lib = run_window(tmp, "c", ["frames 8", "page import", "frames 6", "scd_form %s|%s|%s" % (bios, cue, viewer), "frames 4", "scd_add", "wait_scd", "frames 8", "quit"])
        moved = bios.with_name("moved-bios.bin")
        bios.rename(moved)
        trace, _ = run_window(tmp, "c", ["frames 12", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details",
                                          "expect scd_state=Inputs not configured", "expect scd_refusal=bios_missing", "expect can_play=0", "quit"])
        bad = [l for l in trace if l.startswith("expect FAIL")]
        check(not bad and sum(l.startswith("expect ok") for l in trace) >= 4, "window C: a moved BIOS file shows 'Inputs not configured' with the reason (%s)" % (bad or "ok"))
        moved.rename(bios)
        with open(viewer, "ab") as f:
            f.write(b"\0")
        trace, _ = run_window(tmp, "c", ["frames 12", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details",
                                          "expect scd_state=Stale program", "expect scd_refusal=program_changed", "expect can_play=0", "quit"])
        bad = [l for l in trace if l.startswith("expect FAIL")]
        check(not bad and sum(l.startswith("expect ok") for l in trace) >= 4, "window C: a program changed after verification shows 'Stale program' (%s)" % (bad or "ok"))

        # ---- SEG-078-T003 (ADR 0205): the Build button of a Sega CD game, in the real window, on a complete (staged) installation ----
        package = common.stage_launcher_package(tmp / "package", SEGARECOMP, CC, os.environ.get("SDL3_PREFIX", "/usr"))
        loader = tmp / "loader"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "overlay", "--out", str(loader)], check=True, capture_output=True)
        lbios, lcue = loader / "bios.bin", loader / "disc" / "disc.cue"
        env = {"SEGARECOMP_HOME": str(package), "SEGARECOMP_SCD_BUILD_ARGS": "--budget 3000000", "SEGARECOMP_VIEWER_KEY_SCRIPT": str(keys)}
        # window D: add without a program, cancel a running build (nothing is left behind), then build to the end and play the built program
        trace, lib = run_window(tmp, "d", [
            "frames 8", "page import", "frames 6", "scd_form %s|%s|" % (lbios, lcue), "frames 4", "scd_add", "wait_scd", "frames 10",
            "expect page=details", "expect scd_state=No program yet (can be built)", "expect can_play=0",
            "build", "frames 20", "expect scd_building=1", "cancel_build", "wait_scd", "frames 10", "expect scd_building=0", "expect scd_state=No program yet (can be built)",
            "build", "frames 6", "expect scd_building=1", "wait_scd", "frames 20",
            "expect scd_building=0", "expect scd_state=Ready to play", "expect can_play=1", "expect scd_build_note=Built in 4 round(s).", "play", "frames 4", "wait_scd", "wait_play_end", "frames 10",
            "expect scd_outcome=The program finished.", "quit"], env)
        bad = [l for l in trace if l.startswith("expect FAIL")]
        check(not bad and sum(l.startswith("expect ok") for l in trace) >= 12, "window D: Build cancels cleanly, then builds, verifies and plays (%s)" % (bad or "%d ok" % sum(l.startswith("expect ok") for l in trace)))
        text = "\n".join(trace)
        check("build=started platform=sega-cd" in text and "build=cancel_requested platform=sega-cd" in text and "build=cancelled platform=sega-cd" in text and "build=ok platform=sega-cd" in text
              and "scd=program_attached" in text, "window D: the trace shows start, cancel, cancelled, start again, ok, and the program attached")
        scd_dir = next((lib / "sega-cd").glob("*"))
        check((scd_dir / "build").is_dir() and not (scd_dir / "build.partial").exists() and not (scd_dir / "build.failed").exists(), "window D: only the finished build remains (no partial or failed folder)")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
