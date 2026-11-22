#!/usr/bin/env python3
"""SEG-137-T007 (ADR 0296): driver of launcher_mod_variant_test (the launcher's executable cache and BuildJob with a Luau-only mod). Prepares fabricated inputs only: the runnable fixture ROM of the
Luau HUD example, its package and variants of it, an installation root that points at this source tree, the real `segarecomp` driver and the host compiler (through a thin stand-in for the
bundled Zig, an OS dependency: the launcher invokes `<zig> cc -target <triple> ...`), and the SDL3 prefix. Then runs the C++ test against them.
usage: launcher_mod_variant_test.py <launcher_mod_variant_test> <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
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
    if sys.platform != "linux":   # the full generated-native build is exercised on the Linux runners (like genesis_patch_provider_build_test)
        print("launcher_mod_variant_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("fixture_rom", root / "platforms/genesis/sdk/examples/luau-hud/fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    example = root / "platforms/genesis/sdk/examples/luau-hud"
    with tempfile.TemporaryDirectory(prefix="segarecomp-launcher-mod-") as directory:
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
        zig = install / "toolchain" / "zig"   # `zig cc -target <t> ...` / `zig c++ -target <t> ...` -> the host compiler
        zig.write_text(f'#!/bin/sh\ndriver="$1"\nshift 3\ncase "$driver" in c++) exec "{shutil.which("c++") or "c++"}" "$@" ;; *) exec "{compiler}" "$@" ;; esac\n')
        zig.chmod(0o755)
        rom = tmp / "fixture.rom"
        rom.write_bytes(fixture.make_rom())
        rom_sha = hashlib.sha256(rom.read_bytes()).hexdigest()
        other = tmp / "other.rom"
        other.write_bytes(rom.read_bytes() + b"\x60\xfe")

        def run(*args):
            return subprocess.run([str(cli), *map(str, args)], text=True, capture_output=True, timeout=300)

        def package(name, script=None, caps=None, template=None, platform_id="genesis", extra_toml=""):
            project = tmp / name
            if template is None:
                shutil.copytree(example, project, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
                if script is not None:
                    (project / "scripts/main.luau").write_text(script)
                if caps is not None:
                    toml = (project / "mod.toml").read_text()
                    (project / "mod.toml").write_text(toml.split("[capabilities]")[0] + "[capabilities]\nrequired = [" + ", ".join(f'"{c}"' for c in caps) + "]\n")
            else:
                r = run("mod", "init", project, "--platform", platform_id, "--id", name, "--template", template, "--rom-sha256", rom_sha)
                assert r.returncode == 0, r.stdout
            if extra_toml:
                (project / "mod.toml").write_text((project / "mod.toml").read_text() + extra_toml)
            out = tmp / (name + ".segapatch")
            r = run("mod", "pack", project, "--out", out)
            assert r.returncode == 0 and json.loads(r.stdout)["ok"], r.stdout
            return out

        script = (example / "scripts/main.luau").read_text()
        packages = [package("hud"), package("hud-v2", script=script.replace('"LUAU HUD"', '"HUD V2"')), package("native", template="c11-provider"),
                    package("wram", caps=("frame.work_ram", "host.log", "overlay.rgba")), package("syntax", script="function render(frame)\n  local = 1\nend\n")]
        # presentation defaults (ROM-free): verified widescreen geometry without an explicit [presentation] (the shape of the published Castle 0.1.0 package), the same with an explicit one, a HUD-only geometry, and a mod with no geometry
        geo = '\n[geometry]\naspect_max = "32:9"\n\n[[scene]]\nid = "main"\ncomponents = ["actors", "scenery"]\n'
        packages += [package("ws-implicit", extra_toml=geo), package("ws-explicit", extra_toml=geo + '\n[presentation]\nwidescreen = "extended_backgrounds"\nside_style = "borders"\n'),
                     package("hud-only", extra_toml='\n[geometry]\naspect_max = "21:9"\n\n[[scene]]\nid = "main"\ncomponents = ["hud"]\n')]
        env = dict(os.environ, LD_LIBRARY_PATH=str(sdl / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = subprocess.run([test_exe, str(install), str(rom), str(other), *map(str, packages)], text=True, capture_output=True, env=env, timeout=1500)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


sys.exit(main())
