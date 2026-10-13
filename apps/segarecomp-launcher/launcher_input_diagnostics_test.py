#!/usr/bin/env python3
"""SEG-058 corrective delivery: the launcher's Input diagnostics against REAL SDL events from virtual joysticks (SDL dummy video driver, software
renderer; no ROM, package or display needed).

Drives the real window loop through the SEGARECOMP_GUI_SCRIPT hook and checks the launcher's own trace for each diagnostic class:
  A no joystick -> B a joystick SDL has no gamepad mapping for -> C a recognised gamepad whose SDL_OpenGamepad fails (SDL's error is kept) ->
  D an opened gamepad that has produced no event (never reported as working) -> OK only after real button events arrive, with Dear ImGui's own
  gamepad navigation seeing the same pad -> back to A after a hot-unplug.
A virtual device proves the code path, NOT that a physical controller works (docs/testing/input-diagnostics.md).

usage: launcher_input_diagnostics_test.py <path to the Segarecomp executable>
"""
import os
import pathlib
import subprocess
import sys
import tempfile


def run(exe, env, script, tmp):
    script_path = tmp / "script.txt"
    trace_path = tmp / "trace.txt"
    script_path.write_text("\n".join(script) + "\n")
    trace_path.unlink(missing_ok=True)
    e = dict(env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path))
    subprocess.run([exe], env=e, timeout=120, check=True, capture_output=True)
    return trace_path.read_text().splitlines() if trace_path.exists() else []


def check(name, trace, expected, failures):
    bad = [l for l in trace if l.startswith("expect FAIL")]
    good = [l for l in trace if l.startswith("expect ok")]
    if bad or len(good) != expected:
        failures.append("%s: %d/%d expectations ok; trace=%r" % (name, len(good), expected, trace))


def main():
    exe = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_JOYSTICK_HIDAPI="0",
                   SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1", SEGARECOMP_CACHE_DIR=str(tmp / "cache"), SEGARECOMP_HOME=str(tmp / "no-package"))
        env.pop("SEGARECOMP_GAMEPAD_MAPPINGS", None)
        head = ["settings_tab 4", "frames 6"]
        # A: nothing is attached (the page works without any ROM)
        check("A", run(exe, env, head + ["expect page=settings", "expect pad_count=0", "expect input_state=A", "quit"], tmp), 3, failures)
        # B: a joystick without a gamepad mapping; raw button presses never turn it into a working gamepad
        check("B", run(exe, env, head + ["pad_raw_attach", "frames 10", "expect pad_count=1", "expect input_state=B", "pad south down", "pad south up", "frames 6",
                                         "expect pad_events=0", "expect input_state=B", "quit"], tmp), 4, failures)
        # C: recognised as a gamepad, SDL_OpenGamepad fails: SDL's error text is surfaced
        check("C", run(exe, env, head + ["pad_lock on", "pad_attach", "frames 10", "expect pad_count=1", "expect input_state=C",
                                         "expect pad_error=simulated: the device is locked by another program", "quit"], tmp), 3, failures)
        # D then OK: opened but silent is NOT working; real events (with the launcher's own navigation seeing them) are
        check("D/OK", run(exe, env, head + ["pad_attach", "frames 10", "expect pad_count=1", "expect input_state=D", "expect pad_events=0",
                                            "pad south down", "frames 4", "pad south up", "frames 6", "expect input_state=OK", "quit"], tmp), 4, failures)
        # hot-unplug returns to A, replug works again
        check("unplug", run(exe, env, head + ["pad_attach", "frames 10", "expect pad_count=1", "pad_detach", "frames 6", "expect pad_count=0", "expect input_state=A",
                                              "pad_attach", "frames 10", "expect pad_count=1", "expect input_state=D", "quit"], tmp), 5, failures)
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print("launcher_input_diagnostics_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
