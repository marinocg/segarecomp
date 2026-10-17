#!/usr/bin/env python3
"""SEG-069-T004: the launcher's ordinary journeys with a virtual SDL gamepad ONLY (no mouse event is ever injected), on the real window (SDL dummy video driver).

Every step is a virtual-joystick button event (D-pad, South/A, East/B, North/Y) delivered through the window's own automation hook; `pad_seek <control> <direction>`
presses one D-pad direction until the control holds focus (and records `expect ok|FAIL seek=<control>`), which is how reachability is asserted. Journeys:

  first run     the intermediates question: focus is trapped inside the sheet (every direction), South confirms, East defers
  import        an empty library -> built-in controller picker (the native OS dialog cannot be assumed gamepad-operable): tick ROMs, import; cancel/back/root confinement
  library       sidebar <-> grid hops, toolbar, secondary actions through the card menu (North), list view, Details and Back with focus restoration
  build         Details -> Build -> Cancel running build; a second game queued then removed from the queue; the Build/Cancel/Unqueue controls follow the build state
  older build   after an update the "playable older build" offers Play and Recompile, both reachable
  settings      tabs, options, scrolling to the bottom; restore-defaults confirmation confirmed/cancelled
  storage       the cleanup confirmation sheet is reachable, cancel leaves files, East backs out

Run at 1080x740 and 720x520 (SEGARECOMP_SHOT_SIZE).

usage: launcher_gamepad_journey_test.py <Segarecomp> <fake_segarecomp> <fake_game> [size ...]
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
DIRS = ("dpad_up", "dpad_down", "dpad_left", "dpad_right")


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
    path.write_bytes(bytes(image))


# ---- script vocabulary ----
def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def narrow(size):
    return int((size or "1080x740").split("x")[0]) < 900


def presses(button, count):
    return [step for _ in range(count) for step in press(button)]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def expect(**kw):
    return [f"expect {k}={v}" for k, v in kw.items()]


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game, games=1):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.game_log = self.dir / "game.log"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game,
                        FAKE_GAME_LOG=str(self.game_log))
        for i in range(games):
            rom = self.dir / f"game{i}.md"
            genesis_rom(rom, i + 1)
            done = subprocess.run([exe, "--import", str(rom)], env=self.env, timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr

    def run(self, script, extra_env=None, size=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        if size:
            e["SEGARECOMP_SHOT_SIZE"] = size
        res = subprocess.run([self.exe], env=e, timeout=300, check=True, capture_output=True)
        self.stderr = res.stderr.decode(errors='replace')
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def builds(self):
        games = self.cache / "games"
        return sorted(p for p in games.iterdir() if p.is_dir() and not p.name.endswith((".partial", ".failed"))) if games.exists() else []


class Runner:
    def __init__(self):
        self.failures = []
        self.checked = 0

    def check(self, name, trace, expected):
        self.failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        self.checked += ok
        if ok != expected:
            self.failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def expect(self, condition, message):
        if not condition:
            self.failures.append(message)


START = ["frames 10", "pad_attach", "frames 20"]


def first_run(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "first-run", exe, fake_cli, fake_game)
    script = START + expect(modal=1)
    for d in DIRS:  # focus is trapped in the sheet: whatever the direction, it stays on one of its two choices
        script += press(d)
    script += expect(modal=1, focus_id="mod_remove")   # the last press was Right
    script += press("dpad_left") + expect(focus_id="mod_keep") + press("dpad_right") + expect(focus_id="mod_remove") + press("dpad_left") + expect(focus_id="mod_keep")
    script += press("east") + expect(modal=0, intermediates_choice=0)          # East defers: nothing recorded
    script += ["quit"]
    r.check("first run defer", p.run(script, {"SEGARECOMP_GUI_ONBOARDING": "always"}, size), 8)
    script = START + expect(modal=1) + press("dpad_right") + press("south") + expect(modal=0, cleanup_mode="intermediates", intermediates_choice=1) + ["quit"]
    r.check("first run choose", p.run(script, {"SEGARECOMP_GUI_ONBOARDING": "always"}, size), 4)
    settings = json.loads((p.lib / "launcher-settings.json").read_text())
    r.expect(settings.get("cleanup_mode") == "intermediates" and settings.get("intermediates_choice_made") is True, f"first run persisted: {settings}")


def import_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "import", exe, fake_cli, fake_game, games=0)
    root = tmp / "import" / "roms"
    (root / "sub").mkdir(parents=True)
    for i, name in enumerate(("a.md", "b.md")):
        genesis_rom(root / name, 60 + i)
    genesis_rom(root / "sub" / "deep.md", 70)
    (root / "notes.txt").write_text("not a rom")
    env = {"SEGARECOMP_PICKER_ROOT": str(root)}
    # empty library: Browse (controller) -> picker -> into the folder -> tick both ROMs -> Import
    script = START + seek("dz_browse", "dpad_right", 6) + press("south") + expect(picker=1, focus_id="pkrow#0")
    script += press("south") + expect(picker_level="root")
    script += seek("pkrow#2", "dpad_down", 4) + press("south") + expect(picker_chosen=1)
    script += seek("pkrow#3", "dpad_down", 2) + press("south") + expect(picker_chosen=2)
    script += seek("pk_cancel", "dpad_down", 3) + seek("pk_ok", "dpad_right", 3) + press("south") + ["frames 90"] + expect(picker=0, library_size=2) + ["quit"]
    r.check("import via picker", p.run(script, env, size), 12)
    # back / cancel semantics and root confinement: East goes up, then to the locations, then closes; nothing is imported
    script = START + press("dpad_down") + seek("card:a", "dpad_right", 3) + seek("lib_import", "dpad_up", 8) + press("south") + expect(picker=1)
    script += press("south") + expect(picker_level="root")
    script += seek("pkrow#1", "dpad_down", 2) + press("south") + expect(picker_level="sub")        # into the sub folder
    script += press("east") + expect(picker_level="root") + press("east") + expect(picker_level="locations")
    script += press("south") + expect(picker_level="root")
    script += seek("pkrow#0", "dpad_up", 4) + press("south") + expect(picker_level="locations")      # the "Back to locations" row
    script += press("east") + expect(picker=0, library_size=2) + ["quit"]
    r.check("picker back/cancel", p.run(script, env, size), 13)


def library_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "library", exe, fake_cli, fake_game, games=3)
    script = START + press("dpad_down") + seek("card:game0", "dpad_right", 3) + expect(visible=3)
    # toolbar: the platform drop-down is a popup list a pad can operate (a filter that hides every game, then back)
    script += seek("pf", "dpad_up", 3) + press("south") + press("dpad_down") + press("dpad_down") + press("south") + expect(platform_filter=2, visible=0)
    script += press("south") + press("south") + expect(platform_filter=0, visible=3)
    if narrow(size):   # the view switches sit on the search row of a narrow window
        script += seek("lib_import", "dpad_up", 3) + seek("view_list", "dpad_left", 4) + press("south") + expect(view="list") + seek("view_grid", "dpad_left", 2) + press("south") + expect(view="grid")
        script += seek("search", "dpad_left", 3)
    else:
        script += seek("view_list", "dpad_right", 10) + press("south") + expect(view="list") + seek("view_grid", "dpad_left", 2) + press("south") + expect(view="grid")
        script += seek("lib_import", "dpad_up", 3) + seek("search", "dpad_left", 3)
    # search: a pad can enter the field and leave it again (East) without being trapped
    script += press("south") + expect(want_text=1) + press("east") + expect(want_text=0)
    # secondary actions through the card menu (North): favorite, then Details; Back restores the card
    script += seek("pf", "dpad_down", 3) + seek("card:game0", "dpad_down", 3) + seek("card:game1", "dpad_right", 2) + press("north") + expect(menu=1)
    script += seek("m_fav", "dpad_down", 6) + press("south") + expect(favorites=1)
    script += seek("card:game1", "dpad_down", 3) + press("north") + seek("m_details", "dpad_up", 8) + press("south") + expect(page="details", detail="game1")
    script += press("east") + expect(page="library", focus="card:game1") + ["quit"]
    r.check("library", p.run(script, None, size), 27)


def build_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "build", exe, fake_cli, fake_game, games=2)
    hold = tmp / "build" / "heartbeat"
    started = f"{hold}.started"
    open_game = lambda title: seek(f"card:{title}", "dpad_right", 4) + press("south") + press("dpad_right") + expect(page="details")
    script = START + press("dpad_down") + open_game("game0")
    script += seek("build", "dpad_down", 14) + press("south") + [f"wait_file {started}"] + expect(building=1)
    script += seek("cancel_build", "dpad_down", 10) + press("south") + ["wait_idle", "frames 20"] + expect(building=0, build_outcome="cancelled")
    # a running build blocks nothing else: start it again, go back, queue the second game, take it out of the queue again, then cancel the running one
    script += seek("build", "dpad_down", 14) + press("south") + [f"wait_file {started}"] + press("east") + expect(page="library")
    script += press("dpad_down") + seek("card:game1", "dpad_right", 4) + press("south") + press("dpad_right") + expect(page="details")
    script += seek("build", "dpad_down", 14) + press("south") + expect(queued=1, build_count=2)
    script += seek("unqueue", "dpad_down", 10) + press("south") + expect(queued=0, build_count=1)
    script += press("east") + seek("card:game0", "dpad_left", 3) + press("south") + press("dpad_right")
    script += seek("cancel_build", "dpad_down", 12) + press("south") + ["wait_idle", "frames 20"] + expect(build_count=0, build_outcome="cancelled") + ["quit"]
    r.check("build/cancel/queue", p.run(script, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "20000"}, size), 21)
    left = [x for x in p.cache.rglob("*") if x.name.endswith((".partial", ".failed"))] if p.cache.exists() else []
    r.expect(not left, f"cancelled builds leave nothing behind: {left}")


def older_build_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "older", exe, fake_cli, fake_game, games=1)
    setup = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "build", "wait_idle", "frames 60", "quit"]
    p.run(setup, None, size)                                       # a build of "this release" ...
    with open(p.package / "bin" / ("segarecomp" + EXE_SUFFIX), "ab") as f:  # ... then the product is updated
        f.write(b"\0release-update")
    script = START + press("dpad_down") + seek("card:game0", "dpad_right", 3) + press("south") + press("dpad_right")
    script += expect(page="details", status="Playable (older build)", can_play=1)
    script += seek("play", "dpad_down", 6) + press("south") + ["frames 20", "wait_play_end", "frames 6"]             # Play: the OLD executable
    script += seek("build", "dpad_right", 3) + press("south") + ["wait_idle", "frames 60"] + expect(build_outcome="ok", status="Ready") + ["quit"]  # Recompile
    trace = p.run(script, None, size)
    r.check("older build", trace, 8)
    r.expect(any(l.startswith("run=older_build") for l in trace), f"Play did not start the earlier build: {trace!r}")


def settings_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "settings", exe, fake_cli, fake_game, games=1)
    script = START + seek("Settings", "dpad_down", 4) + press("south") + press("dpad_right") + expect(page="settings")
    if narrow(size):   # the six sections are a drop-down in a narrow window
        pick = lambda index: press("south") + presses("dpad_down", index) + press("south")
        script += expect(focus_id="settings_section") + pick(5) + expect(settings_tab=5)              # About
        script += seek("settings_section", "dpad_up", 3) + pick(2) + expect(settings_tab=2)           # Build & Runtime: a long section
        script += press("dpad_down") + seek("keep_old", "dpad_down", 4) + seek("go_storage", "dpad_down", 12)
        script += seek("settings_section", "dpad_up", 40) + press("south") + press("south") + expect(settings_tab=0)   # first item = Library & Display
        script += press("dpad_down")
    else:
        script += expect(focus_id="tab#0")
        script += seek("tab#5", "dpad_down", 6) + press("south") + expect(settings_tab=5)              # About
        script += seek("tab#2", "dpad_up", 6) + press("south") + expect(settings_tab=2)                # Build & Runtime: a long tab
        script += seek("keep_old", "dpad_right", 3) + seek("go_storage", "dpad_down", 12)              # its options, down to the last control
        script += seek("tab#2", "dpad_left", 3) + seek("tab#0", "dpad_up", 3) + press("south") + expect(settings_tab=0)
        script += press("dpad_right")
    script += seek("restore", "dpad_down", 40) + press("south") + expect(modal=1, focus_id="mod_cancel") + press("east") + expect(modal=0)   # restore defaults: East cancels
    script += seek("restore", "dpad_down", 3) + press("south") + expect(modal=1) + seek("mod_ok", "dpad_right", 3) + press("south") + expect(modal=0)
    script += press("east") + expect(page="library") + ["quit"]
    r.check("settings", p.run(script, None, size), 21 if not narrow(size) else 19)


def storage_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "storage", exe, fake_cli, fake_game, games=1)
    p.run(["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "build", "wait_idle", "frames 60", "quit"], {"FAKE_BUILD_INTERMEDIATES": "1"}, size)
    built = p.builds()[0]
    has_intermediates = lambda: (built / "generated.c").exists() or (built / "obj").exists()
    r.expect(has_intermediates(), "the setup build left intermediates")
    script = START + seek("Storage", "dpad_down", 4) + press("south") + press("dpad_right") + expect(page="storage")
    script += seek("sel", "dpad_down", 6) + press("south") + seek("act:a_zig", "dpad_down", 4) + seek("act:a_inter", "dpad_right", 2) + press("south") + expect(modal=1)
    script += press("east") + expect(modal=0)                                                       # cancel: nothing deleted
    script += press("south") + expect(modal=1) + seek("mod_ok", "dpad_right", 4) + press("south") + ["frames 60"] + expect(modal=0)
    script += press("east") + expect(page="library") + ["quit"]
    r.check("storage", p.run(script, None, size), 11)
    r.expect(not has_intermediates(), "the confirmed cleanup removed the intermediates")
    r.expect((built / ("game" + EXE_SUFFIX)).exists(), "the playable executable stays after the cleanup")


def hotplug_journey(r, tmp, exe, fake_cli, fake_game, size):
    p = Profile(tmp, "hotplug", exe, fake_cli, fake_game, games=2)
    # mouse/keyboard behave as before; a pad attached later works; the controller hint bar follows the last input device; a pad removed mid-use leaves the UI operable
    script = ["frames 10"] + expect(hints=0) + ["key Tab", "frames 3", "key Down", "frames 3"] + expect(page="library", hints=0)
    script += ["pad_attach", "frames 20"] + press("dpad_down") + expect(hints=1)
    script += seek("card:game0", "dpad_right", 3) + press("south") + expect(page="details")
    script += ["pad_detach", "frames 20", "key Escape", "frames 8"] + expect(page="library", hints=0)         # keyboard still works with the pad gone
    script += ["pad_attach", "frames 20"] + press("dpad_down") + expect(hints=1) + seek("card:game0", "dpad_right", 4) + press("south") + expect(page="details", detail="game0")
    script += press("east") + expect(page="library") + ["quit"]
    r.check("hotplug", p.run(script, None, size), 13)


JOURNEYS = [first_run, import_journey, library_journey, build_journey, older_build_journey, settings_journey, storage_journey, hotplug_journey]


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    sizes = sys.argv[4:] or ["1080x740", "720x520"]
    r = Runner()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        for size in sizes:
            sub = tmp / size
            sub.mkdir()
            for journey in JOURNEYS:
                journey(r, sub, exe, fake_cli, fake_game, size)
    if r.failures:
        print("\n".join(r.failures))
        return 1
    print(f"launcher_gamepad_journey_test: ok ({r.checked} controller-only expectations at {', '.join(sizes)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
