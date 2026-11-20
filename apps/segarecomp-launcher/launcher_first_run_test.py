#!/usr/bin/env python3
"""The first-run experience, through the real window (SDL dummy video, virtual gamepad).

History: SEG-069-T003 asked a one-time "keep or remove build intermediates" question at the first preparation. SEG-150 corrective round B (F30) removed it: a first-time user
goes Add game -> Prepare -> Play with no compiler question. A fresh profile simply has the existing, NON-DELETING cleanup default (`cleanup_mode` "never"); Settings > General >
Preparation changes it. `intermediates_choice_made` is a legacy marker: loaded and saved verbatim, never read.

Scripted windows skip the welcome unless `SEGARECOMP_GUI_ONBOARDING=1` forces it (so existing automation fixtures are never blocked). Checks: a fresh profile never gets a sheet and keeps the
intermediates (policy never); Add -> Prepare -> Play works on a brand-new profile; an upgraded profile (answered true or never asked, any policy) keeps its stored values, is never asked, and
its settings file is byte-identical after a run that changed nothing; the setting is reachable with a gamepad and persists; keyboard focus after Add & prepare is not trapped (T010 F2).

usage: launcher_first_run_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import json
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
                "sdl3/include/SDL3/SDL.h"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(fake_cli, root / "bin" / ("segarecomp" + EXE_SUFFIX))


def genesis_rom(path):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    path.write_bytes(bytes(image))


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        package = self.dir / "package"
        make_package(package, fake_cli)
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="720x520", FAKE_GAME_BIN=fake_game,
                        FAKE_BUILD_INTERMEDIATES="1")

    @property
    def settings_file(self):
        return self.lib / "launcher-settings.json"

    def settings(self):
        return json.loads(self.settings_file.read_text()) if self.settings_file.exists() else None

    def add_game(self):
        genesis_rom(self.dir / "g.md")
        done = subprocess.run([self.exe, "--import", str(self.dir / "g.md")], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def run(self, script, force="always", extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        if force:
            e["SEGARECOMP_GUI_ONBOARDING"] = force
        subprocess.run([self.exe], env=e, timeout=180, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    def check_trace(name, trace, expected):
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def has_intermediates(profile):
        games = [g for g in (profile.cache / "games").iterdir() if g.is_dir() and not g.name.endswith((".partial", ".failed"))] if (profile.cache / "games").exists() else []
        return [(g / "generated.c").exists() or (g / "obj").exists() for g in games]

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        # No sheet, whatever the profile: opening a game and pressing Prepare must go straight to preparing.
        OPEN = ["pad_attach", "frames 20", "expect modal=0", "focus_card 0", "frames 4", "key Return", "frames 8", "build", "frames 8"]

        # 1. a fresh profile (no settings file) is never asked; the policy is the non-deleting default; the build's intermediates are kept; nothing is written to the settings file.
        p = Profile(tmp, "fresh", exe, fake_cli, fake_game)
        p.add_game()
        expect(p.settings() is None, "a fresh profile has no settings file before anything is changed")
        trace = p.run(OPEN + ["expect modal=0", "expect cleanup_policy=never:10:4096", "wait_idle", "frames 60", "expect build_outcome=ok", "quit"])
        check_trace("fresh", trace, 4)
        expect(has_intermediates(p) == [True], f"fresh: the default policy keeps the build's intermediates: {has_intermediates(p)}")
        expect(p.settings() is None, f"fresh: preparing a game writes no settings: {p.settings()}")

        # 2. Add game -> Prepare -> Play on a brand-new profile, with the welcome forced: no question, one launch.
        p = Profile(tmp, "add-prepare-play", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "k.md")
        trace = p.run(["frames 20", "expect welcome=1", f"import_files {p.dir / 'k.md'}", "wait_import", "frames 8", "review_prepare", "frames 8", "expect modal=0", "expect page=library",
                       "wait_idle", "frames 40", "expect build_outcome=ok", "expect main_action=Play", "main_action n:0", "frames 10", "wait_play_end", "frames 10", "quit"], extra_env={"FAKE_GAME_LOG": str(p.dir / "game.log")})
        check_trace("add prepare play", trace, 5)
        expect(sum(1 for l in trace if l.startswith("run=started")) == 1, f"add prepare play: the game started once: {trace!r}")
        expect(not any(l.startswith("onboarding=intermediates") for l in trace), "no intermediates question was ever traced")

        # 3. an upgraded profile that ANSWERED the old question keeps its values; its settings file is byte-identical after a run that changed nothing (a build included).
        p = Profile(tmp, "upgraded-answered", exe, fake_cli, fake_game)
        p.add_game()
        p.lib.mkdir(parents=True, exist_ok=True)
        answered = json.dumps({"schema": 1, "view": "list", "cleanup_mode": "intermediates", "keep_last_n": 10, "soft_limit_mb": 4096, "intermediates_choice_made": True})
        p.settings_file.write_text(answered)
        trace = p.run(OPEN + ["expect modal=0", "expect cleanup_mode=intermediates", "expect intermediates_choice=1", "wait_idle", "frames 60", "expect build_outcome=ok", "quit"], force="1")
        check_trace("upgraded answered", trace, 5)
        expect(p.settings_file.read_text() == answered, "upgraded (answered): the settings file is byte-identical")
        expect(has_intermediates(p) == [False], f"upgraded (answered, remove): the stored policy still removes intermediates: {has_intermediates(p)}")

        # 4. an upgraded profile that was NEVER asked (marker false) keeps its policy and the marker untouched; no question; byte-identical file.
        for mode in ("never", "keep_last_n"):
            p = Profile(tmp, "upgraded-unasked-" + mode, exe, fake_cli, fake_game)
            p.add_game()
            p.lib.mkdir(parents=True, exist_ok=True)
            unasked = json.dumps({"schema": 1, "cleanup_mode": mode, "keep_last_n": 7, "intermediates_choice_made": False})
            p.settings_file.write_text(unasked)
            trace = p.run(OPEN + ["expect modal=0", f"expect cleanup_policy={mode}:7:4096", "expect intermediates_choice=0", "wait_idle", "frames 60", "expect build_outcome=ok", "quit"], force="1")
            check_trace("upgraded unasked " + mode, trace, 5)
            expect(p.settings_file.read_text() == unasked, f"upgraded (never asked, {mode}): the settings file is byte-identical")

        # 5. an existing profile (a settings file written before the marker existed) and a library without a settings file get no welcome and no question.
        p = Profile(tmp, "existing", exe, fake_cli, fake_game)
        p.lib.mkdir(parents=True)
        legacy = json.dumps({"schema": 1, "view": "grid", "cleanup_mode": "keep_last_n", "keep_last_n": 7})
        p.settings_file.write_text(legacy)
        trace = p.run(["frames 20", "expect modal=0", "expect welcome=0", "expect cleanup_mode=keep_last_n", "quit"], force="1")
        check_trace("existing", trace, 3)
        expect(p.settings_file.read_text() == legacy, "existing: the legacy settings file is byte-identical")
        p = Profile(tmp, "library-only", exe, fake_cli, fake_game)
        p.add_game()
        trace = p.run(["frames 20", "expect modal=0", "expect welcome=0", "quit"], force="1")
        check_trace("library only", trace, 2)
        # ... while a brand-new profile sees the welcome only when the test says so, and the welcome writes nothing.
        p = Profile(tmp, "automated-fresh", exe, fake_cli, fake_game)
        check_trace("automation skips", p.run(["frames 20", "expect modal=0", "expect welcome=0", "quit"], force=None), 2)
        check_trace("automation opt-in", p.run(["frames 20", "expect welcome=1", "expect modal=0", "expect library_size=0", "quit"], force="1"), 3)
        expect(p.settings() is None, "the welcome records nothing")

        # 6. the policy is reachable in Settings > General > Preparation with the gamepad alone, persists, and is the same one a later build obeys.
        p = Profile(tmp, "setting", exe, fake_cli, fake_game)
        p.add_game()
        trace = p.run(["pad_attach", "frames 20", "page settings", "settings_tab 0", "frames 10", "expect cleanup_mode=never", "pad dpad_right down", "pad dpad_right up", "frames 6", "pad_seek cleanup#0 dpad_down 80", "pad dpad_right down", "pad dpad_right up", "frames 6", "expect focus_id=cleanup#1", "pad south down", "pad south up", "frames 8",
                       "expect cleanup_mode=intermediates", "quit"], force=None, extra_env={"SEGARECOMP_SHOT_SIZE": "1080x740"})
        check_trace("setting", trace, 4)
        s = p.settings()
        expect(s is not None and s["cleanup_mode"] == "intermediates", f"setting persisted: {s}")
        trace = p.run(OPEN + ["expect modal=0", "expect cleanup_mode=intermediates", "wait_idle", "frames 60", "expect build_outcome=ok", "quit"], force=None)
        check_trace("setting obeyed", trace, 4)
        expect(has_intermediates(p) == [False], f"setting: a later build removes intermediates as chosen: {has_intermediates(p)}")

        # 7. SEG-150-T010 (F2): keyboard only. Add & prepare leaves the Add game review for the Library: focus must land on the Library's sidebar entry so Tab keeps working
        #    (it once stayed on the vanished review button and Tab did nothing). Since round B no sheet is raised; the route is Tab, Tab, Return, then Tab through the sidebar.
        p = Profile(tmp, "keyboard-after-prepare", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "k.md")
        trace = p.run(["frames 10", f"import_files {p.dir / 'k.md'}", "wait_import", "frames 8", "key Tab", "frames 3", "key Tab", "frames 3", "expect focus_id=review_prepare", "key Return", "frames 8",
                       "expect modal=0", "expect page=library", "expect focus=Library", "key Tab", "frames 3", "expect focus=Settings",
                       "key Tab", "frames 3", "expect focus=Add game", "wait_idle", "frames 60", "expect build_outcome=ok", "quit"])
        check_trace("keyboard after prepare", trace, 7)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_first_run_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
