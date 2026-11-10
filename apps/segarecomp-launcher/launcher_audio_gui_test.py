#!/usr/bin/env python3
"""ADR 0291: the audio implementation row of the Game Details page (Build provenance), through the real window and the product double.

Everything shown comes from the `audio_implementation` member of the status.json of the build Play would launch. Checks: a native GEMS build, an original
ahead-of-time Z80 build, one with compiled structural alternatives, a degraded one, a game that is not built, an older build without the member, malformed
and unrecognised records (Unknown, never a claim), a rebuild with a different sound implementation, build-mode switches (each entry shows its own record),
and that requesting the native replacement passes `--sound-driver gems` to the product and selects a cache entry of its own.

usage: launcher_audio_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""

NATIVE = "Native GEMS replacement"
AOT = "Original Z80 - ahead-of-time compiled"
DEGRADED = "Original Z80 - degraded"

ORIGINAL = {"schema": 1, "requested": "original", "kind": "original_z80_aot", "selection": "default", "audio_status": "supported",
            "structural_alternatives": {"requested": False, "forms": 0}}
ALTERNATIVES = dict(ORIGINAL, structural_alternatives={"requested": True, "images": 1, "bytes": 11, "entries": 7, "forms": 13, "images_dropped": 0})
BROKEN = dict(ORIGINAL, audio_status="degraded", reason="structural_code_mismatch")


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
        self.args_log = self.dir / "args.log"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="720x520",
                        FAKE_GAME_BIN=fake_game, FAKE_BUILD_ARGS_LOG=str(self.args_log))
        self.rom = self.dir / "g.md"
        genesis_rom(self.rom)
        done = subprocess.run([exe, "--import", str(self.rom)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

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


OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "mode compatibility", "frames 6"]


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def check(name, trace, expected):
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def build_and_show(p, name, audio_env, shown, extra=None):
        env = {"FAKE_AUDIO": audio_env} if audio_env is not None else {}
        env.update(extra or {})
        check(name, p.run(OPEN + ["build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", f"expect audio={shown}", "quit"], env), 4)

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. nothing is built: nothing is claimed
        p = Profile(tmp, "unbuilt", exe, fake_cli, fake_game)
        check("not built", p.run(OPEN + ["expect audio=Not recorded", "quit"]), 2)

        # 2. a current product records the original driver by default; a rebuild with the native replacement replaces the record; a rebuild without it restores it
        p = Profile(tmp, "rebuild", exe, fake_cli, fake_game)
        build_and_show(p, "original build", None, AOT)
        check("native requested and built", p.run(OPEN + ["native_gems 1", "frames 4", "build", "wait_idle", "frames 40", "expect build_outcome=ok", f"expect audio={NATIVE}", "quit"]), 3)
        args = p.args_log.read_text().splitlines()
        if sum(1 for l in args if "--sound-driver gems" in l) != 1 or "--sound-driver" in args[0]:
            failures.append(f"only the request for the native replacement passes --sound-driver gems to the product: {args!r}")
        entries = p.builds()
        if len(entries) != 2:
            failures.append(f"the native request must select a cache entry of its own: {[e.name for e in entries]}")
        check("back to original", p.run(OPEN + ["native_gems 0", "frames 10", f"expect audio={AOT}", "quit"]), 2)   # the first entry is still there and still Ready

        # 3. the other cases, one record each (each is a build made by the product double that records exactly that)
        for name, record, shown in (("structural alternatives", ALTERNATIVES, AOT), ("degraded", BROKEN, DEGRADED),
                                    ("malformed", '{"schema":1,"kind":"native_dri', "Unknown"),
                                    ("unrecognised driver", {"schema": 1, "kind": "native_driver", "driver": "other", "verification": "reference_checked", "coverage": "partial", "audio_status": "partial"}, "Unknown"),
                                    ("unrecognised kind", {"schema": 1, "kind": "mystery", "audio_status": "supported"}, "Unknown"),
                                    ("future schema", dict(ORIGINAL, schema=2), "Unknown")):
            q = Profile(tmp, "case-" + name.replace(" ", "-"), exe, fake_cli, fake_game)
            build_and_show(q, name, record if isinstance(record, str) else json.dumps(record), shown)

        # 4. an older build without any record
        q = Profile(tmp, "older", exe, fake_cli, fake_game)
        build_and_show(q, "no record", "none", "Not recorded")

        # 5. a build mode switch shows the record of the build of THAT mode: Compatibility has the native build, Optimized is not built
        q = Profile(tmp, "modes", exe, fake_cli, fake_game)
        check("native in compatibility", q.run(OPEN + ["native_gems 1", "frames 4", "build", "wait_idle", "frames 40", f"expect audio={NATIVE}", "quit"]), 2)
        check("optimized not built", q.run(OPEN + ["mode optimized", "frames 12", "expect audio=Not recorded", "mode compatibility", "frames 12", f"expect audio={NATIVE}", "quit"]), 3)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_audio_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
