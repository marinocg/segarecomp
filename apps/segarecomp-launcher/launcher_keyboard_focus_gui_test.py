#!/usr/bin/env python3
"""SEG-150 corrective round B (F30), keyboard focus (F10), through the real window (SDL dummy video, product and game doubles).

  * a Game page opened with the keyboard (Return on a card) starts on its primary action: Play, Prepare / Prepare & play, Retry, Find game file or Cancel; Escape returns to the card;
  * the welcome's Add game is the first Tab stop (and Return then opens the picker);
  * focus after a failed preparation is on Retry, after a cancelled one on the Prepare button, and a sheet (Restore defaults) returns focus to the control that raised it;
  * the first-run focus trap fixed in T010 stays fixed (launcher_first_run_test case 7).

usage: launcher_keyboard_focus_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
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


def genesis_rom(path, seed):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x120:0x130] = f"GAME NUMBER {seed:03d}".encode()[:16].ljust(16)
    image[0x1F0] = seed
    path.write_bytes(bytes(image))


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game, games=0):
        self.exe, self.fake_cli = exe, fake_cli
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.lib, self.cache = self.dir / "lib", self.dir / "cache"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache), SEGARECOMP_LIBRARY_DIR=str(self.lib),
                        SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game)
        self.roms = self.dir / "roms"
        self.roms.mkdir()
        for i in range(games):
            genesis_rom(self.roms / f"g{i}.md", i + 1)
            done = subprocess.run([exe, "--import", str(self.roms / f"g{i}.md")], env=self.env, timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr

    def start(self, script, size, extra=None):
        script_path, self.trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        self.trace_path.unlink(missing_ok=True)
        env = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(self.trace_path), SEGARECOMP_SHOT_SIZE=size, **(extra or {}))
        return subprocess.Popen([self.exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def run(self, script, size, extra=None):
        proc = self.start(script, size, extra)
        proc.wait(timeout=240)
        return self.trace()

    def trace(self):
        return self.trace_path.read_text().splitlines() if self.trace_path.exists() else []


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def check(trace, name, expected_ok):
        failures.extend(f"{name}: {l}" for l in trace if "FAIL" in l)
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected_ok:
            failures.append(f"{name}: {ok} of {expected_ok} expectations ran: {trace!r}")

    OPEN0 = ["frames 20", "focus_card 0", "frames 4", "key Return", "frames 12", "expect page=details"]
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        for size in ("720x520", "1080x740"):
            # 1. welcome: Add game is the first Tab stop; Return opens the chooser (the built-in picker here); Escape comes back with the welcome intact.
            p = Profile(tmp, "welcome-" + size, exe, fake_cli, fake_game)
            check(p.run(["frames 20", "expect welcome=1", "key Tab", "frames 3", "expect focus_id=welcome_add", "key Return", "frames 8", "expect picker=1", "key Escape", "frames 8", "expect picker=0",
                         "expect welcome=1", "expect focus_id=welcome_add", "quit"], size, {"SEGARECOMP_GUI_ONBOARDING": "always"}), "welcome " + size, 6)

            # 2. a game that must be prepared: the page opens on Prepare & play; Escape returns to the card; Tab leaves it for the next stop (no trap).
            p = Profile(tmp, "prepare-" + size, exe, fake_cli, fake_game, games=2)
            check(p.run(["frames 20", "focus_card 1", "frames 4", "key Return", "frames 12", "expect page=details", "expect focus_id=build", "key Tab", "frames 3", "expect focus_id=more", "key Escape", "frames 8",
                         "expect page=library", "expect focus=card:GAME NUMBER 002", "quit"], size), "prepare " + size, 5)

            # 3. a prepared game opens on Play; a prepared game's failed re-preparation puts focus back on the same button; a failed first preparation shows Retry with focus.
            p = Profile(tmp, "ready-" + size, exe, fake_cli, fake_game, games=1)
            p.run(OPEN0 + ["build", "wait_idle", "frames 40", "quit"], size)
            check(p.run(OPEN0 + ["expect focus_id=play", "key Escape", "frames 8", "expect page=library", "expect focus=card:GAME NUMBER 001", "quit"], size), "ready " + size, 4)

            p = Profile(tmp, "failed-" + size, exe, fake_cli, fake_game, games=1)
            check(p.run(OPEN0 + ["expect focus_id=build", "key Return", "wait_idle", "frames 40", "expect build_outcome=failed", "expect focus_id=build", "expect main_action=Retry",
                                 "key Return", "wait_idle", "frames 40", "expect build_outcome=failed", "expect focus_id=build", "quit"], size, {"FAKE_BUILD_FAIL": "optimized,compatibility"}),
                  "failed " + size, 7)

            # 4. a running preparation: the page opens on Cancel; Return cancels and focus lands on the Prepare button.
            p = Profile(tmp, "cancel-" + size, exe, fake_cli, fake_game, games=1)
            hold = p.dir / "hb"
            check(p.run(["frames 20", "main_action n:0", f"wait_file {hold}.started", "frames 6", "focus_card 0", "frames 4", "key Return", "frames 12", "expect page=details", "expect focus_id=cancel_build",
                         "key Return", "wait_idle", "frames 30", "expect focus_id=build", "quit"], size, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}), "cancel " + size, 3)

            # 5. a missing game file: the page opens on Find game file.
            p = Profile(tmp, "missing-" + size, exe, fake_cli, fake_game, games=1)
            (p.roms / "g0.md").unlink()
            check(p.run(OPEN0 + ["expect focus_id=locate", "quit"], size), "missing " + size, 2)

            # 6. a sheet returns focus to its origin: Restore defaults (Settings > Advanced... reached with the pad), Esc/East closes it.
            p = Profile(tmp, "sheet-" + size, exe, fake_cli, fake_game, games=1)
            pad = lambda b: [f"pad {b} down", f"pad {b} up", "frames 4"]
            check(p.run(["pad_attach", "frames 20", "page settings", "frames 10", *pad("dpad_right"), "pad_seek restore dpad_down 80", *pad("south"), "expect modal=1", "expect focus_id=mod_cancel", *pad("east"),
                         "expect modal=0", "expect focus_id=restore", "quit"], size), "sheet " + size, 5)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_keyboard_focus_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
