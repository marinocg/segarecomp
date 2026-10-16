#!/usr/bin/env python3
"""SEG-069-T001: cancelling and dequeuing a build through the real launcher window (SDL dummy video driver, software renderer).

Runs the window against a synthetic package whose `segarecomp` is the product double; `FAKE_BUILD_HOLD` makes a build run (with a grandchild process)
until it is cancelled. Checks the window's own trace: Cancel moves a running build through "stopping" to a `cancelled` outcome that is not a failure,
leaves no `.partial`/`.failed` folder and no running process, and a queued game can be removed without ever starting.

usage: launcher_build_cancel_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def make_package(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                "sdl3/include/SDL3/SDL.h"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(fake_cli, root / "bin" / ("segarecomp" + EXE_SUFFIX))


def genesis_rom(path, index):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x1E0] = index
    path.write_bytes(bytes(image))


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        package = tmp / "package"
        make_package(package, fake_cli)
        cache = tmp / "cache"
        heartbeat = tmp / "heartbeat"
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(cache),
                   SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game)
        for index, title in enumerate(("Alpha Cancel Test", "Beta Cancel Test")):
            rom = tmp / (title + ".md")
            genesis_rom(rom, index)
            done = subprocess.run([exe, "--import", str(rom)], env=env, timeout=60, capture_output=True, text=True)
            if "import=added" not in done.stdout:
                failures.append(f"import {title}: {done.stdout!r} {done.stderr!r}")

        def run(script, extra_env):
            script_path, trace_path = tmp / "script.txt", tmp / "trace.txt"
            script_path.write_text("\n".join(script) + "\n")
            trace_path.unlink(missing_ok=True)
            e = dict(env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **extra_env)
            subprocess.run([exe], env=e, timeout=120, check=True, capture_output=True)
            return trace_path.read_text().splitlines() if trace_path.exists() else []

        def check(name, script, expected, extra_env):
            trace = run(script, extra_env)
            failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
            ok = sum(1 for l in trace if l.startswith("expect ok"))
            if ok != expected:
                failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")
            return trace

        started = str(heartbeat) + ".started"
        hold = {"FAKE_BUILD_HOLD": str(heartbeat)}

        # 1. running build: cancel -> stopping -> cancelled; the game is not marked failed; nothing of the attempt remains.
        trace = check("cancel running", [
            "frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details",
            "build", "frames 4", "expect building=1", "expect build_count=1",
            f"wait_file {started}",
            "cancel_build", "cancel_build",  # a second request while stopping is ignored (the stopping phase itself is covered by launcher_build_cancel_test)
            "wait_idle", "frames 6",
            "expect building=0", "expect stopping=0", "expect build_count=0", "expect build_outcome=cancelled", "quit"], 7, hold)
        if not any("build=cancelled" in l for l in trace):
            failures.append(f"cancel running: no cancelled trace: {trace!r}")
        if any("build=failed" in l for l in trace):
            failures.append(f"cancel running: a cancel was reported as a failure: {trace!r}")
        leftovers = [p for p in cache.rglob("*") if p.name.endswith((".partial", ".failed"))]
        if leftovers:
            failures.append(f"cancel running: leftover artifacts {leftovers}")
        before = heartbeat.stat().st_size if heartbeat.exists() else 0
        time.sleep(0.4)
        after = heartbeat.stat().st_size if heartbeat.exists() else 0
        if before != after:
            failures.append("cancel running: a process of the build tree is still running")

        # 2. queued build: removing it never starts it.
        heartbeat.unlink(missing_ok=True)
        pathlib.Path(started).unlink(missing_ok=True)
        check("dequeue queued", [
            "frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details",
            "build", "frames 4", "expect building=1", f"wait_file {started}",
            "key Escape", "frames 8", "focus_card 1", "frames 4", "key Return", "frames 8", "expect page=details",
            "build", "frames 4", "expect queued=1", "expect build_count=2",
            "cancel_build", "frames 4", "expect queued=0", "expect build_count=1",
            "key Escape", "frames 8", "focus_card 0", "frames 4", "key Return", "frames 8",
            "cancel_build", "wait_idle", "frames 6", "expect build_count=0", "expect build_outcome=cancelled", "quit"], 9, hold)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_build_cancel_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
