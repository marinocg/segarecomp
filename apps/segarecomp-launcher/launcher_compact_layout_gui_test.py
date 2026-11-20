#!/usr/bin/env python3
"""SEG-150 corrective round B (F30), compact layout (F11), through the real window (SDL dummy video, product and game doubles).

At 720x520 the library uses horizontal cards (grid) and rows (list). The state of a game is the important text: it is drawn at the full width of the card and is never cut by an ellipsis
(`expect states_clipped=0`, which counts every state label cut by an ellipsis), for the states a library shows at once (not prepared, game file missing, Master System needing a mapper
choice, failed, preparing, queued), with a very long title, in both views and at the roomier size too.

usage: launcher_compact_layout_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
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


def sms_rom(path, seed):
    image = bytearray(0x8000)
    for i in range(len(image)):
        image[i] = (i * 31 + seed) & 0xFF
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    path.write_bytes(bytes(image))


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def check(trace, name, expected_ok):
        failures.extend(f"{name}: {l}" for l in trace if "FAIL" in l)
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected_ok:
            failures.append(f"{name}: {ok} of {expected_ok} expectations ran: {trace!r}")

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        for size in ("720x520", "1080x740"):
            p = Profile(tmp, "states-" + size, exe, fake_cli, fake_game)
            long_title = "AN EXTRAORDINARILY LONG DISPLAY NAME FOR A SYNTHETIC GAME"
            for i, title in enumerate((long_title, "READY GAME", "MISSING GAME", "BROKEN GAME", "BUSY GAME", "QUEUED GAME")):
                image = bytearray(0x200)
                image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
                image[0x100:0x110] = b"SEGA GENESIS    "
                name = title.encode("ascii")[:47].ljust(48)
                image[0x120:0x150] = name
                image[0x150:0x180] = name
                image[0x1E0] = i + 1
                (p.roms / f"t{i}.md").write_bytes(bytes(image))
                done = subprocess.run([exe, "--import", str(p.roms / f"t{i}.md")], env=p.env, timeout=60, capture_output=True, text=True)
                assert "import=added" in done.stdout, done.stdout + done.stderr
            sms_rom(p.roms / "cart.sms", 5)
            subprocess.run([exe, "--import", str(p.roms / "cart.sms")], env=p.env, timeout=60, capture_output=True, text=True, check=True)
            (p.roms / "t2.md").unlink()   # MISSING GAME: its file is gone
            # run A: one game fails to prepare (a recorded, persistent state)
            check(p.run(["frames 20", "main_action BROKEN GAME", "wait_idle", "frames 20", "expect build_outcome=failed", "quit"], size, {"FAKE_BUILD_FAIL": "optimized,compatibility"}), "failing build " + size, 1)
            # run B: a preparation is running and another queued, beside the not prepared, missing, mapper and failed ones: no state label is cut, in the grid and the list
            hold = p.dir / "hb"
            script = ["frames 20", "expect states_clipped=0", "view list", "frames 10", "expect states_clipped=0",
                      "view grid", "frames 6", "main_action BUSY GAME", f"wait_file {hold}.started", "main_action QUEUED GAME", "frames 10", "expect states_clipped=0", "view list", "frames 10", "expect states_clipped=0",
                      "view grid", "frames 6", "main_action QUEUED GAME", "frames 6", "main_action BUSY GAME", "wait_idle", "frames 10", "quit"]
            check(p.run(script, size, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}), "states " + size, 4)
            for f in p.dir.glob("hb*"):
                f.unlink()

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_compact_layout_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
