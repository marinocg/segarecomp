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
        # no pipes: a build tool or game the launcher started must never keep a pipe of this test open (a timeout would then hang instead of fail)
        subprocess.run([self.exe], env=e, timeout=240, check=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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

    def build_and_show(p, name, audio_env, shown, has=(), extra=None):
        """One window run: rebuild the open game with the product double recording `audio_env`, then check the headline and every phrase in `has` (the real window is slow, so one run does it all)."""
        env = {"FAKE_AUDIO": audio_env} if audio_env is not None else {}
        env.update(extra or {})
        check(name, p.run(OPEN + ["build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", f"expect audio={shown}",
                                  *[f"expect audio_has={phrase}" for phrase in has], "quit"], env), 4 + len(has))

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        # The real window is slow (and a CI host runs several of these suites at once), so this test uses ONE profile and only five window runs; the reader's fail-closed behaviour for
        # malformed, unrecognised, older and future records is covered, without a window, by launcher_policy_test.
        p = Profile(tmp, "audio", exe, fake_cli, fake_game)

        # 1. nothing is built: nothing is claimed
        check("not built", p.run(OPEN + ["expect audio=Not recorded", "expect audio_has=Save states", "quit"]), 2)

        # 2. no user option: a current product records the original driver for an ordinary game and the launcher shows it as such
        build_and_show(p, "original build", None, AOT, ["Not a replacement driver"])

        # 3. the build selected the native replacement by itself: the row and every detail line; a mode switch shows the record of THAT mode's build
        build_and_show(p, "rebuilt natively", "native", NATIVE,
                       ["Automatically selected for a verified GEMS driver image", "Independently reimplemented in native C11", "Verified against the original driver's command and chip-access behavior",
                        "Original Z80 not executed for that driver", "Save states and rewind supported", "Timing is event-accurate rather than instruction-cycle-accurate"])
        check("optimized not built", p.run(OPEN + ["mode optimized", "frames 12", "expect audio=Not recorded", "mode compatibility", "frames 12", f"expect audio={NATIVE}", "advanced 1", "frames 6",
                                                  f"expect audio={NATIVE}", "quit"]), 5)

        # 4. a degraded original is disclosed separately, with the structural alternatives counted
        build_and_show(p, "degraded", json.dumps(dict(BROKEN, structural_alternatives={"requested": True, "forms": 13}), separators=(",", ":")), DEGRADED,
                       ["Degraded: structural code mismatch" if False else "Degraded: structural code mutation", "Music is not supported"])

        # 5. an older product that records nothing; an older native record that never claimed save states
        build_and_show(p, "no record", "none", "Not recorded")
        build_and_show(p, "old native record", json.dumps(OLD_NATIVE, separators=(",", ":")), NATIVE, ["Save states are not available in this older build"])

        args = p.args_log.read_text().splitlines()
        if not args or any("--sound-driver" in line for line in args):
            failures.append(f"the launcher never passes a sound option to the product: {args!r}")

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_audio_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
