#!/usr/bin/env python3
"""Static contract of the Windows SDL3 recipe (no network, no build): the pieces that must stay wired together for the native Switch 2 USB path.

This guards the RECIPE text only. That the real SDL3.dll contains the driver, that libusb resolves from the extracted package and that the import libraries
match is proven by the release workflow's gates on the real binaries (packaging/sdl_check.py), not here.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1])
read = lambda rel: (ROOT / rel).read_text(encoding="utf-8")
script, workflow, install, notices = read("packaging/build-sdl3.sh"), read(".github/workflows/release.yml"), read("packaging/install.cmake"), read("packaging/THIRD-PARTY-NOTICES.txt")
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def pin(name):
    match = re.search(rf'{name}="\$\{{{name}:-([0-9a-f]{{64}})\}}"', script)
    check(match is not None, f"{name} must be a 64-hex SHA-256 pin in build-sdl3.sh")
    return match.group(1) if match else ""


pin("SDL_SOURCE_SHA256")
pin("LIBUSB_BINARY_SHA256")
source_sha = pin("LIBUSB_SOURCE_SHA256")
check(re.search(r'SDL_VERSION="\$\{SDL_VERSION:-3\.4\.16\}"', script) is not None, "SDL stays pinned at 3.4.16")
check("libusb.7z" in script and "sha_check" in script, "libusb is downloaded from a pinned release asset and verified")
check('-DLibUSB_LIBRARY=$work/libusb-bin/VS2022/MS64/dll/libusb-1.0.lib' in script, "CMake gets the FULL path of the libusb .lib (upstream issue 14742: a directory fails to identify the DLL)")
check("MinGW64/dll/libusb-1.0.dll" in script, "the runtime libusb is the MinGW build (no Visual C++ redistributable)")
check("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" in script and "CC=cl" in script, "SDL3.dll uses the static CRT like the launcher")
check("SDL_REQUIRE_LIBUSB=1" in script and "SDL_CONFIGURE_ONLY" in script and "HAVE_LIBUSB" in script, "the configure-time gate exists and can be exercised on its own")
check('sdl_check.py" driver' in script and 'sdl_check.py" windows' in script and "dlltool" in script, "post-build gates and the Zig import library come from the built DLL")
check("segarecomp-switch2-usb" in script, "a build that carries the driver declares it in SDL_GetRevision() for the diagnostics")
check('patch -p1 -d "SDL3-${SDL_VERSION}" < "$here/sdl-switch2-diagnostics.patch"' in script and (ROOT / "packaging/sdl-switch2-diagnostics.patch").is_file() and
      "SEGARECOMP_HIDAPI_SETUP_" in read("packaging/sdl-switch2-diagnostics.patch") and "SEGARECOMP_HIDAPI_SETUP_%04x_%04x" in read("packaging/sdl_check.py"),
      "the diagnostics-only SDL patch is applied at packaging time and its presence is gated on the built binary")
check("SDL3-devel" not in workflow and "SDL_VC_SHA256" not in workflow, "the Windows release no longer ships the official SDL binaries")
check("SDL_CONFIGURE_ONLY=1" in workflow and "SDL_HIDAPI_LIBUSB=OFF" in workflow and "ACCEPTED an SDL build without libusb" in workflow, "the workflow proves the gate rejects a build without libusb")
check("sdl_check.py" in workflow and "cmp \"$SDL3_DLL\"" in workflow and "SEGARECOMP_PACKAGE_LIBUSB_DIR" in workflow, "the extracted package is checked against the built binaries")
check("libusb-1.0.dll DESTINATION ." in install.replace("${SEGARECOMP_PACKAGE_LIBUSB_DIR}/", "") and "DESTINATION sdl3/lib" in install and "libusb-LGPL-2.1.txt" in install,
      "libusb-1.0.dll ships beside the launcher AND in sdl3/lib (the PATH directory used for generated games) with its license")
check("libusb-*-source.tar.bz2" in install, "the corresponding libusb source ships with the DLL (LGPL-2.1)")
check("1.0.29" in notices and source_sha in notices and "libusb-LGPL-2.1.txt" in notices, "the notices name the pinned libusb and its source archive")
check("NOT redistributed" in notices and "Windows:" in notices, "the notices distinguish Windows (bundled) from Linux/macOS (OS libusb)")

if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("packaging windows sdl recipe: ok")
