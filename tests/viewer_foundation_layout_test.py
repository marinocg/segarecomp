#!/usr/bin/env python3
"""SEG-058: layering and source-list contract of the shared viewer foundation (libs/viewer). Hermetic, no build, no SDL.

Pins that: the pure core files stay SDL-free; headless/runtime/machine sources never reference the viewer foundation; the five SDL-host-and-core source
files are part of EVERY independent source list (CLI Genesis and Master System recipes, the dev bridge, the packaged install, the CMake targets) so a
change to only the CMake target cannot silently break the packaged per-game compile; and the launcher refuses a package without them.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SV = ROOT / "libs" / "viewer"
FILES = ("sv_settings.c", "sv_layout.c", "sv_menu.c", "sv_input_state.c", "sv_input_diag.c", "sv_host.c", "sv_pages.c")
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def text(path):
    return path.read_text(encoding="utf-8", errors="replace")


def code(path):
    """The file without comments (comments may mention SDL; code must not)."""
    body = text(path)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return re.sub(r"//[^\n]*", "", body)


# 1. the pure core never mentions SDL
for name in ("src/sv_settings.c", "src/sv_layout.c", "src/sv_menu.c", "src/sv_input_state.c", "include/sv_settings.h", "include/sv_layout.h", "include/sv_menu.h", "include/sv_input_state.h"):
    check(not re.search(r"SDL3?/|SDL_|SDL3::", code(SV / name)), f"{name} must stay SDL-free")
# ... and the host header only forward-declares SDL types
check("#include <SDL3" not in code(SV / "include" / "sv_host.h"), "sv_host.h must not include SDL headers (adapters and CMake-only consumers stay light)")

# 2. every source file exists and is C11 strict-friendly (no C++ in libs/viewer/src)
for f in FILES:
    check((SV / "src" / f).is_file(), f"libs/viewer/src/{f} exists")
check(not list((SV / "src").glob("*.cpp")), "libs/viewer/src is C only (no per-game C++ compile)")

# 3. headless / runtime / machine code never reaches into the viewer foundation
for sub in ("platforms/genesis/runtime", "platforms/genesis/machine", "platforms/master-system/runtime", "platforms/master-system/headless", "libs"):
    for path in (ROOT / sub).rglob("*"):
        if path.suffix not in (".c", ".h", ".cpp", ".hpp") or "libs/viewer" in path.as_posix():
            continue
        body = text(path)
        check(not re.search(r'#include\s+[<"]sv_(host|pages|settings|layout|menu)\.h', body), f"{path.relative_to(ROOT)} must not include the viewer foundation")

# 4. the console cores stay SDL-free (their SDL adapters are the only SDL users)
for core in ("platforms/master-system/viewer/sms_viewer.c", "platforms/master-system/viewer/sms_viewer.h", "platforms/genesis/viewer/viewer.c", "platforms/genesis/viewer/viewer.h"):
    check(not re.search(r"SDL3?/|SDL_", code(ROOT / core)), f"{core} must not use SDL")

# 5. the source lists
lists = {
    "apps/segarecomp/build_command.cpp": text(ROOT / "apps/segarecomp/build_command.cpp"),
    "tools/genesis_startup_bridge.py": text(ROOT / "tools/genesis_startup_bridge.py"),
    "libs/viewer/CMakeLists.txt": text(SV / "CMakeLists.txt"),
}
for name, body in lists.items():
    for f in FILES:
        check(f in body, f"{name} must list {f}")
bc = lists["apps/segarecomp/build_command.cpp"]
check(len(re.findall(r"for \(const char \*name : kViewerFoundationSources\)", bc)) == 2, "build_command.cpp must compile the shared foundation list in BOTH the Genesis and the Master System recipes")
check(all(f'"{f}"' in bc.split("kViewerFoundationSources[]")[1].split("};")[0] for f in FILES), "kViewerFoundationSources must list every foundation file")
check('"libs" / "viewer" / "include"' in bc or '"viewer" / "include"' in bc, "build_command.cpp must add the viewer include directories")
install = text(ROOT / "packaging/install.cmake")
check("libs/viewer/" in install and "runtime/libs/viewer" in install.replace("${_rt}", "runtime"), "packaging/install.cmake must install libs/viewer into the runtime tree")
core = text(ROOT / "apps/segarecomp-launcher/launcher_core.cpp")
check('"sv_host.c"' in core, "launcher_core must refuse a package without the viewer foundation")
# CMake: SDL3 is linked only by the host target, never by the core
cm = text(SV / "CMakeLists.txt")
check("SDL3::SDL3" not in cm.split("# Input diagnostics")[0], "the pure core target must not link SDL3")
check("segarecomp_viewer_host" not in text(ROOT / "libs/CMakeLists.txt"), "the host target is declared in libs/viewer only")

# 6. the adapters keep the console loops: the shared host is entered at the sampling point only
g = text(ROOT / "platforms/genesis/viewer/viewer_sdl3.c")
s = text(ROOT / "platforms/master-system/viewer/sms_viewer_sdl3.c")
check("sv_host_service" in g and "sv_host_present" in g, "Genesis adapter uses the shared host at its sampling/present points")
check("sv_host_service" in s and "sv_host_present" in s, "Master System adapter uses the shared host at its sampling/present points")
check("genesis_runtime_run" not in g and "sms_run_until_frame" not in s, "the adapters never call a guest run function")
check("genesis_runtime_run" not in text(SV / "src" / "sv_host.c") and "sms_" not in text(SV / "src" / "sv_host.c"), "the shared host never touches guest execution")

if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("viewer_foundation_layout_test: ok")
