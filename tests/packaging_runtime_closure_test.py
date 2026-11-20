#!/usr/bin/env python3
"""SEG-146 (Windows mod build): the runtime sources a packaged program is compiled from must be COMPLETE.

The package ships `runtime/` as a mirror of the repository (packaging/install.cmake). A game built from the package (`segarecomp build`, the bundled Zig) compiles the entry sources below with the include
directories below; a header that is `#include`d but not installed fails there ("'segarecomp_mod.h' file not found", "'luau_hud_pack.hpp' file not found") while every in-repository test passes.
This test resolves, from the repository, the full header closure of every entry source (the preprocessor's own -MM, so conditional includes are followed under the same defines the build passes) and
checks that each project header (and each entry source) is covered by an install rule of packaging/install.cmake that mirrors it into runtime/.

usage: packaging_runtime_closure_test.py <repo root> <C compiler> <C++ compiler>
"""
import fnmatch
import pathlib
import re
import subprocess
import sys
import tempfile


def install_rules(root):
    text = (root / "packaging" / "install.cmake").read_text()
    text = re.sub(r"#[^\n]*", "", text)   # comments (a trailing `# ...` after a rule included)
    dirs, files = [], []
    for m in re.finditer(r"install\((DIRECTORY|FILES)\s+([^)]*)\)", text, re.S):
        kind, body = m.group(1), m.group(2)
        if "${_rt}" not in body:
            continue
        before, _, after = body.partition("DESTINATION")
        dest = after.split()[0]
        if kind == "DIRECTORY":
            src = before.split()[0].replace("${_src}/", "").rstrip("/")
            mirrored = dest.replace("${_rt}/", "").rstrip("/") == src
            patterns = re.findall(r'PATTERN\s+"([^"]+)"', after)
            dirs.append((src, mirrored, patterns))
        else:
            for token in before.split():
                src = token.replace("${_src}/", "")
                files.append((src, dest.replace("${_rt}/", "").rstrip("/") == str(pathlib.PurePosixPath(src).parent)))
    return dirs, files


def covered(rel, dirs, files):
    for src, mirrored in files:
        if src == rel and mirrored:
            return True
    name = pathlib.PurePosixPath(rel).name
    for src, mirrored, patterns in dirs:
        if mirrored and (rel == src or rel.startswith(src + "/")) and any(fnmatch.fnmatch(name, p) for p in patterns):
            return True
    return False


CXX_INCLUDES = ["libs/device/sega/ym2612/include", "libs/luau_host/include", "libs/luau_host/src", "libs/media/include", "libs/core/include", "libs/viewer/include", "platforms/genesis/sdk", "platforms/sdk",
                "third_party/luau/Common/include", "third_party/luau/VM/include", "third_party/luau/Compiler/include", "third_party/luau/Ast/include", "third_party/luau/Bytecode/include",
                "platforms/genesis/runtime", "platforms/genesis/viewer"]
C_INCLUDES = ["platforms/genesis/runtime", "platforms/genesis/viewer", "libs/codegen/c11/include", "libs/device/sega/psg/include", "libs/device/sega/ym2612/include", "libs/viewer/include", "libs/viewer/src",
              "platforms/genesis/sdk", "platforms/sdk"]
MOD_DEFS = ["-DSEGARECOMP_MOD_NATIVE=1", "-DSEGARECOMP_MOD_VIEWER=1", "-DSEGARECOMP_MOD_HOOKS=1", "-DSEGARECOMP_LUAU_CODEGEN=0", "-DSEGARECOMP_LUAU_PIN=\"0.746\""]
CXX_UNITS = [("libs/luau_host/src/luau_vm.cpp", []), ("libs/luau_host/src/luau_api.cpp", []), ("libs/luau_host/src/luau_stored.cpp", []), ("libs/luau_host/src/luau_phase_provider.cpp", []),
             ("libs/media/src/sha256.cpp", []), ("libs/media/src/mod_adapter.cpp", []),
             ("platforms/genesis/viewer/mod_capture.cpp", MOD_DEFS), ("platforms/genesis/viewer/mod_capture.cpp", ["-DSEGARECOMP_MOD_VIEWER=1"]), ("platforms/genesis/viewer/mod_capture.cpp", []),
             ("platforms/genesis/viewer/luau_provider.cpp", [])]
C_UNITS = [("platforms/genesis/runtime/mod_scheduler.c", []), ("platforms/genesis/runtime/genesis_snapshot.c", ["-DSEGARECOMP_GENESIS_SOUND"]), ("platforms/genesis/runtime/gems_native.c", []),
           ("platforms/genesis/runtime/genesis_sound_hook.c", ["-DSEGARECOMP_SESSION_MACHINE_DIGEST", "-DSEGARECOMP_SESSION_MACHINE_MOD"]), ("platforms/genesis/runtime/virtual_sprite.c", []),
           ("platforms/genesis/viewer/viewer_main_hook.c", ["-DSEGARECOMP_GENESIS_SOUND", "-DSEGARECOMP_NATIVE_PROVIDER_CAPS=13U", "-DSEGARECOMP_MOD_VIEWER=1"]),
           ("platforms/genesis/viewer/frame_capture.c", []), ("platforms/genesis/viewer/genesis_viewer_state.c", []), ("platforms/genesis/viewer/viewer_sdl3.c", []), ("platforms/genesis/viewer/viewer.c", [])]


def main():
    root = pathlib.Path(sys.argv[1]).resolve()
    cc, cxx = sys.argv[2], sys.argv[3]
    dirs, files = install_rules(root)
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        stub = pathlib.Path(tmp) / "sdl"
        (stub / "SDL3").mkdir(parents=True)
        (stub / "SDL3" / "SDL.h").write_text("")   # the SDL3 headers are not project files (the package ships its own sdl3/include)
        for compiler, std, units, includes in ((cxx, "-std=c++20", CXX_UNITS, CXX_INCLUDES), (cc, "-std=c11", C_UNITS, C_INCLUDES)):
            for rel, defs in units:
                if not covered(rel, dirs, files):
                    failures.append(f"entry source {rel} is not installed into runtime/")
                    continue
                cmd = [compiler, std, "-MM", *defs, *sum((["-I", str(root / d)] for d in includes), []), "-I", str(stub), str(root / rel)]
                done = subprocess.run(cmd, capture_output=True, text=True)
                if done.returncode != 0:
                    failures.append(f"{rel} {defs}: the closure cannot be resolved from the repository: {done.stderr.strip().splitlines()[:2]}")
                    continue
                deps = done.stdout.replace("\\\n", " ").split(":", 1)[1].split()
                for dep in deps:
                    path = pathlib.Path(dep).resolve()
                    try:
                        dep_rel = path.relative_to(root).as_posix()
                    except ValueError:
                        continue   # outside the repository (system or stub headers)
                    if not covered(dep_rel, dirs, files):
                        failures.append(f"{rel}: #includes {dep_rel}, which packaging/install.cmake does not install into runtime/")
    if failures:
        for f in sorted(set(failures)):
            print("FAIL:", f)
        return 1
    print(f"packaging_runtime_closure_test: ok ({len(CXX_UNITS) + len(C_UNITS)} entry units, headers covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
