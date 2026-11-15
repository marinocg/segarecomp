#!/usr/bin/env python3
"""SEG-143-T008: driver of launcher_schema3_flow_test (the launcher's local schema 3 install / approve / select / build flow for a NATIVE ABI 2 mod). Prepares fabricated inputs only: the scrolling fixture ROM of the
ABI 2 scenery test (tests/fixtures/mod_scenery), its native package, a package made for another image, an installation root that points at this source tree and the real `segarecomp` driver with the host compiler
(through a thin stand-in for the bundled Zig, an OS dependency). Then runs the C++ test.
usage: launcher_schema3_flow_test.py <launcher_schema3_flow_test> <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    test_exe, cli, compiler, root, sdl = sys.argv[1], pathlib.Path(sys.argv[2]).resolve(), sys.argv[3], pathlib.Path(sys.argv[4]).resolve(), pathlib.Path(sys.argv[5]).resolve()
    if sys.platform != "linux":
        print("launcher_schema3_flow_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-launcher-s3-") as directory:
        tmp = pathlib.Path(directory)
        install = tmp / "install"
        (install / "bin").mkdir(parents=True)
        (install / "toolchain").mkdir()
        (install / "sdl3").mkdir()
        os.symlink(cli, install / "bin" / "segarecomp")
        os.symlink(cli.parent / "segarecomp-luau-compile", install / "bin" / "segarecomp-luau-compile")
        os.symlink(root, install / "runtime")
        os.symlink(sdl / "include", install / "sdl3" / "include")
        os.symlink(sdl / "lib", install / "sdl3" / "lib")
        zig = install / "toolchain" / "zig"
        zig.write_text(f'#!/bin/sh\ndriver="$1"\nshift 3\ncase "$driver" in c++) exec "{shutil.which("c++") or "c++"}" "$@" ;; *) exec "{compiler}" "$@" ;; esac\n')
        zig.chmod(0o755)
        rom = tmp / "fixture.rom"
        rom.write_bytes(fixture.make_rom())
        other = tmp / "other.rom"
        other.write_bytes(rom.read_bytes() + b"\x60\xfe")

        def package(name, image):
            project = tmp / name
            (project / "src").mkdir(parents=True)
            shutil.copy(fixtures / "provider.c", project / "src/provider.c")
            (project / "mod.toml").write_text((fixtures / "mod.toml").read_text().replace("@@SHA@@", hashlib.sha256(image.read_bytes()).hexdigest()))
            out = tmp / (name + ".segapatch")
            r = subprocess.run([str(cli), "mod", "pack", str(project), "--out", str(out)], text=True, capture_output=True, timeout=300)
            assert r.returncode == 0 and json.loads(r.stdout)["ok"], r.stdout
            return out

        data_project = tmp / "data"
        (data_project / "edits").mkdir(parents=True)
        # a data-only mod: ONE typed ROM edit (the fixture's palette colour 1, green -> blue); no provider, no scripts
        (data_project / "edits/main.edits").write_text("fixture u16 0x00000244 0x00e0 0x0e00\n")
        (data_project / "mod.toml").write_text(f'''[mod]
id = "fixture-data-edit"
author = "segarecomp tests"
version = "0.1.0"
name = "Fixture data edit"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "fixture"
sha256 = "{hashlib.sha256(rom.read_bytes()).hexdigest()}"

[[source]]
kind = "rom-edit-set"
path = "edits/main.edits"
''')
        data_pkg = tmp / "data.segapatch"
        r = subprocess.run([str(cli), "mod", "pack", str(data_project), "--out", str(data_pkg)], text=True, capture_output=True, timeout=300)
        assert r.returncode == 0 and json.loads(r.stdout)["ok"], r.stdout
        luau_project = tmp / "luau"
        r = subprocess.run([str(cli), "mod", "init", str(luau_project), "--platform", "genesis", "--id", "fixture-luau", "--template", "luau", "--rom-sha256", hashlib.sha256(rom.read_bytes()).hexdigest()], text=True, capture_output=True, timeout=300)
        assert r.returncode == 0, r.stdout
        luau_pkg = tmp / "luau.segapatch"
        r = subprocess.run([str(cli), "mod", "pack", str(luau_project), "--out", str(luau_pkg)], text=True, capture_output=True, timeout=300)
        assert r.returncode == 0 and json.loads(r.stdout)["ok"], r.stdout
        packages = [package("native", rom), package("wrong", other), data_pkg, luau_pkg]
        env = dict(os.environ, LD_LIBRARY_PATH=str(sdl / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = subprocess.run([test_exe, str(install), str(rom), str(other), *map(str, packages)], text=True, capture_output=True, env=env, timeout=1500)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


sys.exit(main())
