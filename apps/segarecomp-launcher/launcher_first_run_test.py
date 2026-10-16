#!/usr/bin/env python3
"""SEG-069-T003: the one-time "keep or remove build intermediates" question, through the real window (SDL dummy video, virtual gamepad).

Scripted windows skip the question unless `SEGARECOMP_GUI_ONBOARDING=1` forces it (so existing automation fixtures are never blocked); the test forces
it on profiles that are, or are not, genuinely new. Checks: each choice persists the existing cleanup mode and the completion marker and is not asked again; a gamepad
(D-pad focus + South) completes it; Esc/East defers without recording anything; an existing settings file (without the marker) is never asked and keeps its mode; a
failed save keeps the question open; later builds obey the chosen mode (executable kept, intermediates removed or kept).

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
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game,
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

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. gamepad: the first button (Keep) has focus; South confirms. Persisted as the existing policy "never"; not asked again.
        p = Profile(tmp, "keep", exe, fake_cli, fake_game)
        trace = p.run(["pad_attach", "frames 20", "expect modal=1", "pad south down", "pad south up", "frames 8", "expect modal=0", "expect cleanup_mode=never",
                       "expect intermediates_choice=1", "quit"])
        check_trace("keep", trace, 4)
        s = p.settings()
        expect(s is not None and s["cleanup_mode"] == "never" and s["intermediates_choice_made"] is True, f"keep persisted: {s}")
        trace = p.run(["frames 20", "expect modal=0", "quit"])
        check_trace("keep again", trace, 1)

        # 2. gamepad: D-pad moves to the second button; South removes. Policy "intermediates".
        p = Profile(tmp, "remove", exe, fake_cli, fake_game)
        trace = p.run(["pad_attach", "frames 20", "expect modal=1", "pad dpad_right down", "pad dpad_right up", "frames 4", "pad south down", "pad south up", "frames 8",
                       "expect modal=0", "expect cleanup_mode=intermediates", "quit"])
        check_trace("remove", trace, 3)
        s = p.settings()
        expect(s is not None and s["cleanup_mode"] == "intermediates" and s["intermediates_choice_made"] is True, f"remove persisted: {s}")

        # 3. East / Escape defer: nothing is recorded and the question returns.
        p = Profile(tmp, "defer", exe, fake_cli, fake_game)
        trace = p.run(["pad_attach", "frames 20", "expect modal=1", "pad east down", "pad east up", "frames 8", "expect modal=0", "expect intermediates_choice=0", "quit"])
        check_trace("defer east", trace, 3)
        expect(p.settings() is None or p.settings().get("intermediates_choice_made") is not True, "a deferred question is not recorded")
        trace = p.run(["frames 20", "expect modal=1", "key Escape", "frames 8", "expect modal=0", "quit"])
        check_trace("defer escape", trace, 2)

        # 4. an existing profile is never asked and keeps its policy (a settings file written before the marker existed).
        p = Profile(tmp, "existing", exe, fake_cli, fake_game)
        p.lib.mkdir(parents=True)
        p.settings_file.write_text(json.dumps({"schema": 1, "view": "grid", "cleanup_mode": "keep_last_n", "keep_last_n": 7}))
        trace = p.run(["frames 20", "expect modal=0", "expect cleanup_mode=keep_last_n", "quit"], force="1")
        check_trace("existing", trace, 2)
        # ... and so is a profile that has a library but no settings file yet.
        p = Profile(tmp, "library-only", exe, fake_cli, fake_game)
        p.add_game()
        trace = p.run(["frames 20", "expect modal=0", "quit"], force="1")
        check_trace("library only", trace, 1)
        # ... while a brand-new profile in an automated window is asked only when the test says so.
        p = Profile(tmp, "automated-fresh", exe, fake_cli, fake_game)
        check_trace("automation skips", p.run(["frames 20", "expect modal=0", "quit"], force=None), 1)
        check_trace("automation opt-in", p.run(["frames 20", "expect modal=1", "key Escape", "frames 6", "quit"], force="1"), 1)

        # 5. a failed save keeps the question open and records nothing; fixing the cause lets a retry succeed.
        p = Profile(tmp, "failed-save", exe, fake_cli, fake_game)
        blocker = p.lib / "launcher-settings.json.tmp"
        (blocker / "x").mkdir(parents=True)
        trace = p.run(["pad_attach", "frames 20", "expect modal=1", "pad south down", "pad south up", "frames 8", "expect modal=1", "expect intermediates_choice=0", "quit"])
        check_trace("failed save", trace, 3)
        expect(not p.settings_file.exists(), "a failed save must not leave a settings file claiming success")
        shutil.rmtree(blocker)
        trace = p.run(["pad_attach", "frames 20", "expect modal=1", "pad south down", "pad south up", "frames 8", "expect modal=0", "quit"])
        check_trace("retry", trace, 2)
        expect(p.settings() is not None and p.settings()["intermediates_choice_made"] is True, "the retry is recorded")

        # 6. later builds obey the choice; the executable is always kept.
        for name, choose, expect_removed in (("build-remove", ["pad dpad_right down", "pad dpad_right up", "frames 4"], True), ("build-keep", [], False)):
            p = Profile(tmp, name, exe, fake_cli, fake_game)
            p.add_game()
            script = ["pad_attach", "frames 20", "expect modal=1"] + choose + ["pad south down", "pad south up", "frames 8", "expect modal=0",
                      "focus_card 0", "frames 4", "key Return", "frames 8", "build", "wait_idle", "frames 240", "expect build_outcome=ok", "quit"]
            trace = p.run(script)
            check_trace(name, trace, 3)
            if not (p.cache / "games").exists():
                failures.append(f"{name}: no build happened: {trace!r}")
                continue
            games = [g for g in (p.cache / "games").iterdir() if g.is_dir() and not g.name.endswith((".partial", ".failed"))]
            expect(len(games) == 1 and (games[0] / ("game" + EXE_SUFFIX)).exists(), f"{name}: the executable must remain: {games}")
            if games:
                present = (games[0] / "generated.c").exists() or (games[0] / "obj").exists()
                expect(present != expect_removed, f"{name}: intermediates present={present}, expected removed={expect_removed}")

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_first_run_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
