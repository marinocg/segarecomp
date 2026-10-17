#!/usr/bin/env python3
"""SEG-069-T005: playing a build of an earlier release after a launcher update, through the real window and the product double.

An "update" is modelled by changing the packaged product CLI (its size is part of the cache identity), so the library's existing build no longer matches the
current identity. Checks: the game stays playable with a Recompile recommendation, Play starts the OLD executable, an unverifiable old build stays blocked with
a reason, a cancelled or failed recompile leaves the old build playable, a ROM that moved does not stop play, and a voluntary recompile adopts a current build.

usage: launcher_older_build_test.py <Segarecomp> <fake_segarecomp> <fake_game>
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
                "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
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
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.game_log = self.dir / "game.log"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="720x520", FAKE_GAME_BIN=fake_game,
                        FAKE_GAME_LOG=str(self.game_log))
        self.rom = self.dir / "g.md"
        genesis_rom(self.rom)
        done = subprocess.run([exe, "--import", str(self.rom)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def update_product(self):
        """A new release: the packaged CLI changes, so every existing cache key stops matching."""
        cli = self.package / "bin" / ("segarecomp" + EXE_SUFFIX)
        with open(cli, "ab") as f:
            f.write(b"\0release-update")

    def builds(self):
        games = self.cache / "games"
        return sorted(p for p in games.iterdir() if p.is_dir() and not p.name.endswith((".partial", ".failed"))) if games.exists() else []

    def run(self, script, extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=180, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []


OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]
LABEL_OLDER = "Playable (older build)"


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    def check(name, trace, expected):
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. build under the current release; record the host-interface marker; then "update" the product.
        p = Profile(tmp, "update", exe, fake_cli, fake_game)
        check("first build", p.run(OPEN + ["build", "wait_idle", "frames 60", "expect build_outcome=ok", "expect status=Ready", "quit"]), 3)
        old_dirs = p.builds()
        expect(len(old_dirs) == 1, f"one build expected: {old_dirs}")
        meta = json.loads((old_dirs[0] / "metadata.json").read_text())
        expect(meta.get("runtime_contract") == 1 and "hint_alias_discovery" in meta, f"new builds record the host-interface marker: {meta}")
        p.update_product()

        # 2. after the update: playable older build, Recompile independent; Play starts the OLD executable.
        trace = p.run(OPEN + ["frames 30", f"expect status={LABEL_OLDER}", "expect can_play=1", "play", "frames 20", "wait_play_end", "frames 6", "quit"])
        check("older playable", trace, 3)
        expect(any(l.startswith("run=older_build") for l in trace), f"Play did not use the older build: {trace!r}")
        logged = p.game_log.read_text().splitlines() if p.game_log.exists() else []
        expect(len(logged) == 1 and str(old_dirs[0]) in logged[0], f"the old executable must be the one started: {logged}")
        expect(not any("build=started" in l for l in trace), "playing an older build must not start a build")

        # 3. a moved ROM does not stop playing the old build.
        p.rom.rename(p.dir / "moved.md")
        check("rom moved", p.run(OPEN + ["frames 30", f"expect status={LABEL_OLDER}", "expect can_play=1", "quit"]), 3)
        (p.dir / "moved.md").rename(p.rom)

        # 4. a cancelled recompile leaves the old build playable and present.
        heartbeat = p.dir / "heartbeat"
        started = str(heartbeat) + ".started"
        trace = p.run(OPEN + ["frames 20", "build", f"wait_file {started}", "cancel_build", "wait_idle", "frames 20", "expect build_outcome=cancelled", f"expect status={LABEL_OLDER}", "expect can_play=1", "quit"],
                      {"FAKE_BUILD_HOLD": str(heartbeat)})
        check("cancelled recompile", trace, 4)
        expect(old_dirs[0].exists(), "the older build is still on disk after a cancelled recompile")

        # 5. a failed recompile reports the failure but keeps the old build playable.
        trace = p.run(OPEN + ["frames 20", "build", "wait_idle", "frames 20", "expect build_outcome=failed", "expect can_play=1", "quit"], {"FAKE_BUILD_FAIL": "optimized", "FAKE_BUILD_FAIL2": "x"})
        # (default mode is Optimized for Genesis, so FAKE_BUILD_FAIL=optimized fails that build)
        check("failed recompile", trace, 3)

        # 6. a voluntary recompile adopts a current build; the status is Ready (not stale).
        trace = p.run(OPEN + ["frames 20", "build", "wait_idle", "frames 60", "expect build_outcome=ok", "expect status=Ready", "quit"])
        check("recompile", trace, 3)
        expect(len(p.builds()) >= 1, "a current build exists after the recompile")

        # 7. unverifiable old builds stay blocked, with a reason, and nothing is started.
        for name, tamper in (("unlisted release", lambda m, dirp: (m.pop("runtime_contract", None), m.update(segarecomp_version="0.4.0"))),
                             ("other host", lambda m, dirp: m.update(host="mips Plan9")),
                             ("other ROM", lambda m, dirp: m.update(rom_sha256="0" * 64)),
                             ("future contract", lambda m, dirp: m.update(runtime_contract=2)),
                             ("missing metadata", None),
                             ("missing executable", None)):
            q = Profile(tmp, "neg-" + name.replace(" ", "-"), exe, fake_cli, fake_game)
            check(name + " build", q.run(OPEN + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"]), 2)
            built = q.builds()[0]
            if tamper is not None:
                m = json.loads((built / "metadata.json").read_text())
                tamper(m, built)
                (built / "metadata.json").write_text(json.dumps(m))
            elif name == "missing metadata":
                (built / "metadata.json").unlink()
            else:
                (built / ("game" + EXE_SUFFIX)).unlink()
            q.update_product()
            trace = q.run(OPEN + ["frames 30", "expect status=Needs rebuild", "expect can_play=0", "play", "frames 10", "expect playing=0", "quit"])
            check("blocked " + name, trace, 4)
            expect(not q.game_log.exists(), f"{name}: an unverifiable build must never be started")

        # 8. a legacy build (no marker) of the explicitly listed release is playable; a legacy one of another release is not (covered above).
        q = Profile(tmp, "legacy", exe, fake_cli, fake_game)
        check("legacy build", q.run(OPEN + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"]), 2)
        built = q.builds()[0]
        m = json.loads((built / "metadata.json").read_text())
        m.pop("runtime_contract", None)
        m.pop("hint_alias_discovery", None)
        m["segarecomp_version"] = "0.5.0"
        (built / "metadata.json").write_text(json.dumps(m))
        q.update_product()
        check("legacy playable", q.run(OPEN + ["frames 30", f"expect status={LABEL_OLDER}", "expect can_play=1", "quit"]), 3)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_older_build_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
