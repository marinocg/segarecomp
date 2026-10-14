#!/usr/bin/env python3
"""SEG-058: keyboard and gamepad navigation of the launcher window (SDL dummy video driver, software renderer, a virtual gamepad).

Drives the real window loop through the SEGARECOMP_GUI_SCRIPT automation hook (synthetic key events and virtual-joystick buttons) and checks the
window's own trace: focus moves through the sidebar with Tab / arrow keys / the D-pad, Return / the South button activate, Escape / the East button
go back, and the page changes accordingly. Needs no ROM, package or display.

usage: launcher_gui_navigation_test.py <path to the Segarecomp executable>
"""
import os
import pathlib
import subprocess
import struct
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


def main():
    exe = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(tmp / "cache"),
                   SEGARECOMP_HOME=str(tmp / "no-package"))
        # one fictional synthetic cartridge in the library, so every sidebar entry (including Game Details) is enabled
        rom = tmp / "Fictional Nav Test.md"
        image = bytearray(0x200)
        image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
        image[0x100:0x110] = b"SEGA GENESIS    "
        rom.write_bytes(bytes(image))
        subprocess.run([exe, "--import", str(rom)], env=env, timeout=60, check=True, capture_output=True)
        # keyboard: Tab focuses the active entry, arrows walk the sidebar, Return activates, Escape returns to the library
        trace = run(exe, env, ["expect page=library", "key Tab", "frames 2", "key Down", "frames 2", "expect focus=Import ROMs", "key Down", "frames 2", "key Down", "frames 2",
                               "expect focus=Storage", "key Return", "frames 6", "expect page=storage", "key Escape", "frames 6", "expect page=library", "quit"], tmp)
        failures += [l for l in trace if l.startswith("expect FAIL")]
        if sum(1 for l in trace if l.startswith("expect ok")) != 5:
            failures.append("keyboard: not every expectation ran: %r" % trace)
        # gamepad: a virtual SDL gamepad (hot-plugged after start) does the same with the D-pad, South and East
        pad = ["expect page=library", "pad_attach", "frames 20"]
        for _ in range(3):
            pad += ["pad dpad_down down", "pad dpad_down up", "frames 3"]
        pad += ["expect focus=Storage", "pad south down", "pad south up", "frames 10", "expect page=storage", "pad east down", "pad east up", "frames 10", "expect page=library", "quit"]
        trace = run(exe, env, pad, tmp)
        failures += [l for l in trace if l.startswith("expect FAIL")]
        if sum(1 for l in trace if l.startswith("expect ok")) != 4:
            failures.append("gamepad: not every expectation ran: %r" % trace)
        # Responsive screen smoke: render the actual widgets, including the nested settings
        # scroller and confirmation sheet, at both supported sizes. No builds or guest runs.
        pages = ["library", "list", "details:0", "import", "storage", "storage-modal"] + [f"settings{i}" for i in range(6)]
        for size in ("1080x740", "720x520"):
            for page in pages:
                shot = tmp / "responsive.bmp"
                shot.unlink(missing_ok=True)
                e = dict(env, SEGARECOMP_SHOT=str(shot), SEGARECOMP_SHOT_PAGE=page,
                         SEGARECOMP_SHOT_SIZE=size, SEGARECOMP_SHOT_FRAMES="4")
                subprocess.run([exe], env=e, timeout=60, check=True, capture_output=True)
                bmp = shot.read_bytes()
                width, height = struct.unpack_from("<ii", bmp, 18)
                if bmp[:2] != b"BM" or (width, abs(height)) != tuple(map(int, size.split("x"))):
                    failures.append(f"responsive capture failed: {page} at {size}")
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print("launcher_gui_navigation_test: ok (9 navigation assertions, 24 responsive screen captures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
