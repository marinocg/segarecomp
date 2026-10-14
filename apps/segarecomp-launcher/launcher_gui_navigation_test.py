#!/usr/bin/env python3
"""SEG-058: keyboard and gamepad navigation of the launcher window (SDL dummy video driver, software renderer, a virtual gamepad).

Drives the real window loop through the SEGARECOMP_GUI_SCRIPT automation hook (synthetic key events and virtual-joystick buttons) and checks the
window's own trace: focus moves through the sidebar with Tab / arrow keys / the D-pad, Return / the South button activate, Escape / the East button
go back, and the page changes accordingly. Game Details is contextual (not in the sidebar); Back returns to the same library state. Needs no ROM, package or display.

usage: launcher_gui_navigation_test.py <path to the Segarecomp executable>
"""
import os
import pathlib
import subprocess
import struct
import sys
import tempfile
import time


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
        # Fictional synthetic cartridges (distinct digests): enough cards that the library grid scrolls.
        titles = ["Fictional Nav Test"] + [f"Synthetic Card {i:02d}" for i in range(1, 13)]
        for index, title in enumerate(titles):
            rom = tmp / (title + ".md")
            image = bytearray(0x200)
            image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
            image[0x100:0x110] = b"SEGA GENESIS    "
            image[0x1E0] = index
            rom.write_bytes(bytes(image))
            subprocess.run([exe, "--import", str(rom)], env=env, timeout=60, check=True, capture_output=True)

        # A zipped ROM imports like a plain one (extracted once by digest); the same ROM zipped again is the same game.
        import zipfile
        image = bytearray(0x200)
        image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
        image[0x100:0x110] = b"SEGA GENESIS    "
        image[0x1E0] = 200
        for name in ("Zipped Nav Test.zip", "Zipped Again.zip"):
            with zipfile.ZipFile(tmp / name, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("readme.txt", "fictional")
                archive.writestr("Zipped Nav Test.md", bytes(image))
        first = subprocess.run([exe, "--import", str(tmp / "Zipped Nav Test.zip")], env=env, timeout=60, capture_output=True, text=True)
        again = subprocess.run([exe, "--import", str(tmp / "Zipped Again.zip")], env=env, timeout=60, capture_output=True, text=True)
        if "import=added" not in first.stdout or "title=Zipped Nav Test" not in first.stdout:
            failures.append(f"zip import: {first.stdout!r} {first.stderr!r}")
        if "import=duplicate" not in again.stdout:
            failures.append(f"zip re-import is not the same game: {again.stdout!r}")
        with zipfile.ZipFile(tmp / "Two Roms.zip", "w") as archive:
            archive.writestr("a.md", bytes(image))
            archive.writestr("b.sms", bytes(image))
        two = subprocess.run([exe, "--import", str(tmp / "Two Roms.zip")], env=env, timeout=60, capture_output=True, text=True)
        if two.returncode == 0 or "contains 2 ROMs" not in two.stdout:
            failures.append(f"a zip with two ROMs must be refused: {two.stdout!r}")

        def check(name, script, expected):
            trace = run(exe, env, script, tmp)
            failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
            ok = sum(1 for l in trace if l.startswith("expect ok"))
            if ok != expected:
                failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")
            return ok

        assertions = 0
        # The primary navigation is exactly the four application areas; Game Details is not one of them.
        assertions += check("primary navigation", ["expect nav_items=Library,Import ROMs,Storage,Settings", "expect page=library", "quit"], 2)
        # keyboard: Tab focuses the active entry, arrows walk the sidebar, Return activates, Escape returns to the library
        assertions += check("keyboard", ["expect page=library", "key Tab", "frames 2", "key Down", "frames 2", "expect focus=Import ROMs", "key Down", "frames 2",
                                         "expect focus=Storage", "key Down", "frames 2", "expect focus=Settings", "key Down", "frames 2", "expect focus=Settings",
                                         "key Up", "frames 2", "key Return", "frames 6", "expect page=storage", "key Escape", "frames 6", "expect page=library", "quit"], 7)
        # gamepad: a virtual SDL gamepad (hot-plugged after start) does the same with the D-pad, South and East
        pad = ["expect page=library", "pad_attach", "frames 20"]
        for _ in range(2):
            pad += ["pad dpad_down down", "pad dpad_down up", "frames 3"]
        pad += ["expect focus=Storage", "pad south down", "pad south up", "frames 10", "expect page=storage", "pad east down", "pad east up", "frames 10", "expect page=library", "quit"]
        assertions += check("gamepad", pad, 4)
        # Details is contextual: a card opens it with Return, Back (Escape) returns to the SAME library state - search, filter, view, focus.
        state = ["key F", "frames 4", "text Synthetic Card 0", "frames 6", "expect search=Synthetic Card 0", "expect visible=9", "key Tab", "frames 2",
                 "platform_filter 1", "view list", "frames 4", "focus_card 2", "frames 4", "expect focus=card:Synthetic Card 03", "key Return", "frames 8",
                 "expect page=details", "expect detail=Synthetic Card 03", "key Escape", "frames 8", "expect page=library", "expect search=Synthetic Card 0",
                 "expect platform_filter=1", "expect view=list", "expect visible=9", "expect focus=card:Synthetic Card 03", "quit"]
        assertions += check("details round trip (keyboard)", state, 11)
        # the same with a gamepad: South opens the focused card, East goes back; the grid's scroll position is restored too
        state = ["pad_attach", "frames 20", "scroll_library 180", "frames 6", "expect library_scroll=180", "focus_card 6", "frames 4", "expect focus=card:Synthetic Card 06",
                 "pad south down", "pad south up", "frames 10", "expect page=details", "expect detail=Synthetic Card 06", "pad east down", "pad east up", "frames 10",
                 "expect page=library", "expect library_scroll=180", "expect focus=card:Synthetic Card 06", "quit"]
        assertions += check("details round trip (gamepad)", state, 7)
        # Mouse: the buttons drawn over a card (favorite star, Play) stay put while the pointer moves onto them, and a click on them is theirs:
        # it must not fall through to the card (which would open Details).
        state = ["frames 6", "mouse_card 1", "frames 6", "mouse_fav 1", "frames 6", "mouse_fav 1", "frames 4", "click", "frames 10",
                 "expect page=library", "expect favorites=1", "mouse_fav 1", "frames 4", "click", "frames 10", "expect page=library", "expect favorites=0",
                 "mouse_card 1", "frames 4", "click", "frames 10", "expect page=details", "quit"]
        assertions += check("card overlay buttons (mouse)", state, 5)
        # the context menu of a card (gamepad North / the Menu key) offers Details; choosing it opens the page, Back returns to the library
        state = ["pad_attach", "frames 20", "focus_card 0", "frames 4", "pad north down", "pad north up", "frames 6", "expect menu=1", "pad south down", "pad south up", "frames 10",
                 "expect page=details", "pad east down", "pad east up", "frames 10", "expect page=library", "quit"]
        assertions += check("context menu", state, 3)
        # Responsive screen smoke: render the actual widgets, including the nested settings
        # scroller and confirmation sheet, at both supported sizes. No builds or guest runs.
        pages = ["library", "list", "details:0", "details:1", "import", "storage", "storage-modal"] + [f"settings{i}" for i in range(6)]
        for size in ("1080x740", "720x520"):
            for page in pages:
                shot = tmp / "responsive.bmp"
                shot.unlink(missing_ok=True)
                trace_path = tmp / "capture-trace.txt"
                trace_path.unlink(missing_ok=True)
                e = dict(env, SEGARECOMP_SHOT=str(shot), SEGARECOMP_SHOT_PAGE=page,
                         SEGARECOMP_SHOT_SIZE=size, SEGARECOMP_SHOT_FRAMES="4", SEGARECOMP_GUI_TRACE=str(trace_path))
                started = time.monotonic()
                print(f"capture start: {page} at {size}", flush=True)
                try:
                    subprocess.run([exe], env=e, timeout=60, check=True, capture_output=True)
                except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
                    progress = trace_path.read_text() if trace_path.exists() else "no startup trace"
                    raise RuntimeError(f"capture {page} at {size}; bmp_exists={shot.exists()}; progress:\n{progress}; stderr={error.stderr!r}") from error
                print(f"capture done: {page} at {size} in {time.monotonic() - started:.2f}s", flush=True)
                bmp = shot.read_bytes()
                width, height = struct.unpack_from("<ii", bmp, 18)
                if bmp[:2] != b"BM" or (width, abs(height)) != tuple(map(int, size.split("x"))):
                    failures.append(f"responsive capture failed: {page} at {size}")
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print(f"launcher_gui_navigation_test: ok ({assertions} navigation/state assertions, {len(pages) * 2} responsive screen captures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
