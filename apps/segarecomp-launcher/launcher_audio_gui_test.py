#!/usr/bin/env python3
"""ADR 0291: the audio implementation row of the Game Details page (Build provenance), through the real window and the product double.

Everything shown comes from the `audio_implementation` member of the status.json of the build Play would launch. Checks: a native GEMS build, an original
ahead-of-time Z80 build, one with compiled structural alternatives, a degraded one, a game that is not built, an older build without the member, malformed
and unrecognised records (Unknown, never a claim), a rebuild with a different sound implementation, build-mode switches (each entry shows its own record),
and that the launcher offers no sound option and passes none to the product (ADR 0291: the build selects the implementation itself).

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

NATIVE = "Native GEMS 2.5"
ORIGINAL_HEADLINE = "Original Z80 (generated-native)"
AOT = DEGRADED = ORIGINAL_HEADLINE   # a degraded original is disclosed in the detail text, not by another headline

ORIGINAL = {"schema": 2, "requested": "auto", "kind": "original_z80_aot", "selection": "automatic", "reason": "original_supported", "audio_status": "supported",
            "save_states": "supported", "structural_alternatives": {"requested": False, "forms": 0}}
ALTERNATIVES = dict(ORIGINAL, structural_alternatives={"requested": True, "images": 1, "bytes": 11, "entries": 7, "forms": 13, "images_dropped": 0})
BROKEN = dict(ORIGINAL, audio_status="degraded", reason="no_verified_replacement", degraded_reason="structural_code_mismatch")
OLD_ORIGINAL = {"schema": 1, "requested": "original", "kind": "original_z80_aot", "selection": "default", "audio_status": "supported", "structural_alternatives": {"requested": False, "forms": 0}}
OLD_NATIVE = {"schema": 1, "requested": "gems", "kind": "native_driver", "selection": "explicit", "driver": "gems", "version": "2.5", "coverage": "complete", "verification": "reference_checked",
              "reason": "explicit_selection", "original_driver_rewrites_own_code": True, "audio_status": "supported", "observed": {"fault": "none", "commands": 17, "sequences": 7, "dac_samples": 100}}


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

        # 2. there is no user option: a current product records the original driver for an ordinary game and the native replacement for a verified one, chosen by the build
        #    itself, and the launcher never passes a sound option to the product. A rebuild that selects another implementation replaces the record.
        p = Profile(tmp, "rebuild", exe, fake_cli, fake_game)
        build_and_show(p, "original build", None, AOT)
        check("original detail", p.run(OPEN + [f"expect audio={AOT}", "expect audio_has=Not a replacement driver", "expect audio_has=Save states", "quit"]), 4)
        check("rebuilt natively", p.run(OPEN + ["build", "wait_idle", "frames 40", "expect build_outcome=ok", f"expect audio={NATIVE}", "quit"], {"FAKE_AUDIO": "native"}), 3)
        for phrase in ("Automatically selected for a verified GEMS driver image", "Independently reimplemented in native C11", "Verified against the original driver's command and chip-access behavior",
                       "Original Z80 not executed for that driver", "Save states and rewind supported", "Timing is event-accurate rather than instruction-cycle-accurate"):
            check("native detail: " + phrase, p.run(OPEN + [f"expect audio_has={phrase}", "quit"]), 2)
        args = p.args_log.read_text().splitlines()
        if any("--sound-driver" in line for line in args):
            failures.append(f"the launcher never passes a sound option to the product: {args!r}")
        check("no checkbox command exists", p.run(OPEN + ["advanced 1", "frames 6", "expect audio=" + NATIVE, "quit"]), 2)

        # 3. the other cases, one record each (each is a build made by the product double that records exactly that)
        for name, record, shown in (("structural alternatives", ALTERNATIVES, AOT), ("degraded", BROKEN, DEGRADED),
                                    ("old original record", OLD_ORIGINAL, AOT), ("old native record", OLD_NATIVE, NATIVE),
                                    ("malformed", '{"schema":2,"kind":"native_dri', "Unknown"),
                                    ("unrecognised driver", {"schema": 2, "selection": "automatic", "reason": "original_aot_failure_verified_replacement", "kind": "native_driver", "driver": "other", "verification": "reference_checked", "coverage": "complete", "audio_status": "supported"}, "Unknown"),
                                    ("unrecognised kind", {"schema": 2, "selection": "automatic", "reason": "original_supported", "kind": "mystery", "audio_status": "supported"}, "Unknown"),
                                    ("unrecognised selection", dict(ORIGINAL, selection="guessed"), "Unknown"),
                                    ("future schema", dict(ORIGINAL, schema=3), "Unknown")):
            q = Profile(tmp, "case-" + name.replace(" ", "-"), exe, fake_cli, fake_game)
            build_and_show(q, name, record if isinstance(record, str) else json.dumps(record), shown)
        q = Profile(tmp, "case-degraded-detail", exe, fake_cli, fake_game)
        build_and_show(q, "degraded detail", json.dumps(BROKEN), DEGRADED)
        check("degraded is disclosed separately", q.run(OPEN + ["expect audio_has=Degraded: structural code mutation", "expect audio_has=Music is not supported", "quit"]), 3)
        q = Profile(tmp, "case-old-native-detail", exe, fake_cli, fake_game)
        build_and_show(q, "old native detail", json.dumps(OLD_NATIVE), NATIVE)
        check("an older native record does not claim save states", q.run(OPEN + ["expect audio_has=Save states are not available in this older build", "quit"]), 2)

        # 4. an older build without any record
        q = Profile(tmp, "older", exe, fake_cli, fake_game)
        build_and_show(q, "no record", "none", "Not recorded")

        # 5. a build mode switch shows the record of the build of THAT mode: Compatibility has the native build, Optimized is not built
        q = Profile(tmp, "modes", exe, fake_cli, fake_game)
        build_and_show(q, "native in compatibility", "native", NATIVE)
        check("optimized not built", q.run(OPEN + ["mode optimized", "frames 12", "expect audio=Not recorded", "mode compatibility", "frames 12", f"expect audio={NATIVE}", "quit"]), 3)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_audio_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
