#!/usr/bin/env python3
"""SEG-150-T004: the Game page's tabs (Overview / Mods / Game settings / Advanced), the state-aware primary action and the recovery states.

Drives the real window (SDL dummy video driver, software renderer, a virtual gamepad) through SEGARECOMP_GUI_SCRIPT against the product and game doubles.
Checks, each by the window's own trace and never by pixels:
  tabs      the tab strip (a Genesis game has Mods, the tabs exist only where they carry content), stable focus ids, a gamepad activates a tab and keeps
            focus on it, each tab returns to the scroll it was left at, Back (B) returns to the library card that opened the game and a re-opened
            game starts on Overview again
  failed    a failed preparation shows the "failure" card on Overview with the primary action Retry (the library says Retry as well); Retry from the pad
            prepares again and the game becomes Ready with Play as the primary action
  missing   a game whose file moved and that was never prepared offers Find game file as its one primary action; no Prepare button is drawn beside it

usage: launcher_details_tabs_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def make_package(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
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
    path.write_bytes(image)


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def expect(**kw):
    return [f"expect {k}={v}" for k, v in kw.items()]


START = ["frames 10", "pad_attach", "frames 20"]
# the only card is the first: the pad lands on the sidebar, Down reaches the card, South opens it
OPEN = START + press("dpad_down") + seek("card:game0", "dpad_right", 4) + press("south") + press("dpad_right")


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.dir / "cache"),
                        SEGARECOMP_LIBRARY_DIR=str(self.dir / "lib"), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game,
                        FAKE_GAME_LOG=str(self.dir / "game.log"), SEGARECOMP_SHOT_SIZE="720x520")
        self.rom = self.dir / "game0.md"
        genesis_rom(self.rom, 1)
        done = subprocess.run([exe, "--import", str(self.rom)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def run(self, script, extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=300, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []


def layout_names(trace):
    lines = [l for l in trace if l.startswith("layout ")]
    return {part.split(":")[0] for part in lines[-1][len("layout "):].split(";") if ":" in part} if lines else set()


class Runner:
    def __init__(self):
        self.failures = []

    def check(self, name, trace, expected):
        self.failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            self.failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def expect(self, condition, message):
        if not condition:
            self.failures.append(message)


def tabs_journey(r, tmp, exe, fake_cli, fake_game):
    p = Profile(tmp, "tabs", exe, fake_cli, fake_game)
    script = OPEN + expect(page="details", details_tab="overview", details_tabs="overview,mods,settings,advanced", primary_action="Prepare & play")
    # a gamepad reaches a tab by its stable focus id, South selects it, and the focus stays on the tab
    script += seek("tab_overview", "dpad_down", 12) + seek("tab_settings", "dpad_right", 3) + expect(focus_id="tab_settings") + press("south") + expect(details_tab="settings", focus_id="tab_settings")
    script += ["scroll_page 150", "frames 6"] + expect(details_scroll=150)
    script += seek("tab_advanced", "dpad_right", 4) + press("south") + expect(details_tab="advanced", focus_id="tab_advanced")
    script += ["scroll_page 60", "frames 6"] + expect(details_scroll=60)
    # each tab returns to where it was left
    script += seek("tab_settings", "dpad_left", 4) + press("south") + ["frames 6"] + expect(details_tab="settings", details_scroll=150)
    script += seek("tab_advanced", "dpad_right", 4) + press("south") + ["frames 6"] + expect(details_tab="advanced", details_scroll=60)
    # the Overview's rows open the other tabs (Advanced details is reachable from the Overview)
    script += seek("tab_overview", "dpad_left", 6) + press("south") + ["frames 6"] + expect(details_tab="overview")
    script += seek("ov_advanced", "dpad_down", 12) + press("south") + ["frames 6"] + expect(details_tab="advanced")
    # Back (B) leaves the page for the card that opened it; the game opens on Overview again
    script += press("east") + expect(page="library", focus="card:game0")
    script += seek("card:game0", "dpad_right", 4) + press("south") + press("dpad_right") + expect(page="details", details_tab="overview")
    script += press("east") + expect(page="library") + ["quit"]
    r.check("tabs", p.run(script), 31)


def failed_journey(r, tmp, exe, fake_cli, fake_game):
    p = Profile(tmp, "failed", exe, fake_cli, fake_game)
    script = OPEN + ["build", "wait_idle", "frames 6"] + expect(build_outcome="failed", status="Build failed", failure=1, details_tab="overview", primary_action="Retry", can_play=0)
    script += ["layout", "frames 2", "quit"]
    trace = p.run(script, {"FAKE_BUILD_FAIL": "optimized,compatibility"})
    r.check("failed", trace, 7)
    r.expect("failure" in layout_names(trace), f"the Overview of a failed preparation draws the failure card: {layout_names(trace)}")
    # the library says the same: Retry; from the pad the Retry button prepares again (no failure injected) and the game becomes Ready with Play first
    script = START + expect(main_action="Retry") + press("dpad_down") + seek("card:game0", "dpad_right", 4) + press("south") + press("dpad_right") + expect(page="details", primary_action="Retry")
    script += seek("build", "dpad_down", 14) + press("south") + ["wait_idle", "frames 20"] + expect(build_outcome="ok", status="Ready", primary_action="Play", can_play=1) + ["quit"]
    r.check("retry", p.run(script), 9)


def missing_journey(r, tmp, exe, fake_cli, fake_game):
    p = Profile(tmp, "missing", exe, fake_cli, fake_game)
    p.rom.rename(p.dir / "moved.md")
    script = START + expect(main_action="Find game file") + press("dpad_down") + seek("card:game0", "dpad_right", 4) + press("south") + press("dpad_right")
    script += expect(page="details", primary_action="Find game file", can_play=0) + seek("locate", "dpad_down", 8) + expect(focus_id="locate")
    script += ["layout", "frames 2", "quit"]
    trace = p.run(script)
    r.check("missing", trace, 7)
    names = layout_names(trace)
    r.expect("play" in names and "build" not in names, f"Find game file is the one primary action, no Prepare button beside it: {names}")


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    r = Runner()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        tabs_journey(r, tmp, exe, fake_cli, fake_game)
        failed_journey(r, tmp, exe, fake_cli, fake_game)
        missing_journey(r, tmp, exe, fake_cli, fake_game)
    for failure in r.failures:
        print("FAIL:", failure)
    print("launcher_details_tabs_gui_test:", "FAILED" if r.failures else "ok")
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
