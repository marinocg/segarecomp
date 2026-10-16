#!/usr/bin/env python3
"""SEG-069-T002: Optimized is the launcher default for new Genesis and Master System games, through the real window and a product double.

The product double records the argument list of every `build`; the window runs under the SDL dummy video driver. Checks:
  * a new Genesis game builds `--aot-policy optimized --aot-model gen2` (the former Optimized + Experimental request) and the Experimental checkbox is gone;
  * a new Master System game builds `--aot-policy optimized` and never `--aot-model`;
  * where the product does not offer SMS Optimized, the build is Compatibility (no `--aot-policy`) while the stored intent stays Optimized;
  * a slow capability probe does not turn the first build into Compatibility;
  * an explicit Compatibility selection and a legacy Optimized/v1 Genesis record are honoured / migrated.

usage: launcher_default_mode_test.py <Segarecomp> <fake_segarecomp> <fake_game>
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


def sms_rom(path):
    image = bytearray(0x8000)
    for i in range(len(image)):
        image[i] = (i * 31 + 7) & 0xFF
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    path.write_bytes(bytes(image))


class Case:
    def __init__(self, tmp, name, exe, fake_cli, fake_game):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        package = self.dir / "package"
        make_package(package, fake_cli)
        self.lib = self.dir / "lib"
        self.args_log = self.dir / "args.txt"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.dir / "cache"),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game,
                        FAKE_BUILD_ARGS_LOG=str(self.args_log))

    def import_rom(self, path):
        done = subprocess.run([self.exe, "--import", str(path)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def catalog(self):
        return json.loads((self.lib / "library.json").read_text())

    def edit_catalog(self, edit):
        data = self.catalog()
        for game in data["games"]:
            edit(game)
        (self.lib / "library.json").write_text(json.dumps(data))

    def run(self, script, extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=180, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def builds(self):
        return self.args_log.read_text().splitlines() if self.args_log.exists() else []


OPEN_AND_BUILD = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]


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

        # 1. a new Genesis game: Optimized, gen2.
        c = Case(tmp, "genesis", exe, fake_cli, fake_game)
        genesis_rom(c.dir / "g.md")
        c.import_rom(c.dir / "g.md")
        game = c.catalog()["games"][0]
        expect(game["aot_policy"] == "optimized" and game["aot_model"] == "gen2", f"genesis default selection: {game}")
        trace = c.run(OPEN_AND_BUILD + ["expect mode=Optimized", "build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"])
        check_trace("genesis", trace, 3)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy optimized" in builds[0] and "--aot-model gen2" in builds[0], f"genesis build argv: {builds}")

        # 1b. the Experimental checkbox is gone from the Build Configuration.
        source = (pathlib.Path(__file__).parent / "app_details.cpp").read_text()
        expect("\"model_exp\"" not in source and "checkbox(ctx_, \"model\"" not in source, "the Experimental model checkbox is still in the UI source")

        # 2. a new Master System game with the product offering Optimized.
        c = Case(tmp, "sms", exe, fake_cli, fake_game)
        sms_rom(c.dir / "s.sms")
        c.import_rom(c.dir / "s.sms")
        c.edit_catalog(lambda g: g.update(mapper="sega"))
        game = c.catalog()["games"][0]
        expect(game["aot_policy"] == "optimized", f"sms default selection: {game}")
        trace = c.run(OPEN_AND_BUILD + ["expect mode=Optimized", "build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"])
        check_trace("sms", trace, 3)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy optimized" in builds[0] and "--aot-model" not in builds[0], f"sms build argv: {builds}")

        # 3. the product does not offer SMS Optimized: Compatibility build, the stored intent is not rewritten.
        c = Case(tmp, "sms-unsupported", exe, fake_cli, fake_game)
        sms_rom(c.dir / "s.sms")
        c.import_rom(c.dir / "s.sms")
        c.edit_catalog(lambda g: g.update(mapper="sega"))
        trace = c.run(OPEN_AND_BUILD + ["frames 60", "expect mode=Compatibility", "build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"],
                      {"FAKE_SMS_OPTIMIZED": "0"})
        check_trace("sms-unsupported", trace, 3)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy" not in builds[0] and "--aot-model" not in builds[0], f"unsupported sms build argv: {builds}")
        expect(c.catalog()["games"][0]["aot_policy"] == "optimized", "an unsupported probe must not persist Compatibility")

        # 4. a slow capability probe: the first build waits for the product's statement instead of building Compatibility.
        c = Case(tmp, "sms-slow-probe", exe, fake_cli, fake_game)
        sms_rom(c.dir / "s.sms")
        c.import_rom(c.dir / "s.sms")
        c.edit_catalog(lambda g: g.update(mapper="sega"))
        trace = c.run(OPEN_AND_BUILD + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"], {"FAKE_IDENTITY_DELAY_MS": "3500"})
        check_trace("sms-slow-probe", trace, 2)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy optimized" in builds[0], f"slow-probe build argv: {builds}")

        # 5. explicit Compatibility is honoured for both platforms; a legacy Optimized/v1 Genesis record builds gen2.
        c = Case(tmp, "explicit", exe, fake_cli, fake_game)
        genesis_rom(c.dir / "g.md")
        c.import_rom(c.dir / "g.md")
        c.edit_catalog(lambda g: g.update(aot_policy="compatibility", aot_model="v1"))
        trace = c.run(OPEN_AND_BUILD + ["expect mode=Compatibility", "build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"])
        check_trace("explicit", trace, 3)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy" not in builds[0], f"explicit compatibility argv: {builds}")
        c = Case(tmp, "legacy-v1", exe, fake_cli, fake_game)
        genesis_rom(c.dir / "g.md")
        c.import_rom(c.dir / "g.md")
        c.edit_catalog(lambda g: g.update(aot_policy="optimized", aot_model="v1"))
        trace = c.run(OPEN_AND_BUILD + ["expect mode=Optimized", "build", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"])
        check_trace("legacy-v1", trace, 3)
        builds = c.builds()
        expect(len(builds) == 1 and "--aot-policy optimized" in builds[0] and "--aot-model gen2" in builds[0], f"legacy v1 argv: {builds}")

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_default_mode_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
