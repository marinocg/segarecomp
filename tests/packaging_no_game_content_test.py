#!/usr/bin/env python3
"""SEG-078-T002 (ADR 0204): the release package contains no BIOS, disc or generated Sega CD program (only the project-authored runtime SOURCES ship, ADR 0205).

Two independent checks, both hermetic:
  1. the extracted-package verifier (packaging/dependency_check.py) refuses every such file name and accepts every file the package really holds;
  2. the install rules (packaging/install.cmake, apps/segarecomp-launcher/CMakeLists.txt) name platforms/sega-cd only to stage the runtime and viewer sources, never a program or a disc.

usage: packaging_no_game_content_test.py <source-root>
"""
import importlib.util
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("dependency_check", ROOT / "packaging" / "dependency_check.py")
dep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dep)
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


forbidden = ["games/Sonic CD (USA).cue", "disc.cue", "bios/Sega CD BIOS.bin", "Sega-CD Model 1 BIOS V1.10 (U).bin", "sega-cd/program-viewer", "programs/game-viewer.describe",
             "runtime/platforms/sega-cd/runtime/scd_main.bin", "runtime/platforms/sega-cd/runtime/bios.c.gz", "runtime/platforms/sega-cd/machine/x.cpp",
             "runtime/platforms/sega-cd/runtime/sub/x.c", "sega-cd/last-report.json", "image.iso", "game.chd", "x/Mega-CD BIOS.md"]
allowed = ["runtime/platforms/sega-cd/runtime/scd_main.c", "runtime/platforms/sega-cd/runtime/scd_runtime.h", "runtime/platforms/sega-cd/viewer/scd_viewer.c",
           "Segarecomp", "segarecomp.exe", "bin/segarecomp", "licenses/MPL-2.0.txt", "assets/ui/icon-play.png", "runtime/platforms/genesis/runtime/runtime.c",
           "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "sdl3/lib/libSDL3.so.0", "toolchain/zig", "README.txt",
           "runtime/libs/device/sega/ym2612/ym2612.cpp", "toolchain/lib/libc/include/any-linux-any/linux/apm_bios.h", "toolchain/lib/libc/include/x86/bios_stub.c"]
for name in forbidden:
    check(dep.FORBIDDEN_COMMERCIAL.search(name) is not None, "refused: " + name)
for name in allowed:
    check(dep.FORBIDDEN_COMMERCIAL.search(name) is None, "accepted: " + name)

install = (ROOT / "packaging" / "install.cmake").read_text() + (ROOT / "apps" / "segarecomp-launcher" / "CMakeLists.txt").read_text()
installs = [line for line in install.splitlines() if re.match(r"\s*install\(", line)]
check(not any(re.search(r"sega_cd|segacd|\.cue|program-viewer", line, re.I) for line in installs), "no install rule names a disc or a program")
sega_rules = [line for line in installs if re.search(r"sega-cd", line, re.I)]
check(len(sega_rules) == 2 and all(re.search(r"platforms/sega-cd/(runtime|viewer)/\s+DESTINATION \$\{_rt\}/platforms/sega-cd/(runtime|viewer)\s+FILES_MATCHING PATTERN \"\*\.c\" PATTERN \"\*\.h\"", line)
      for line in sega_rules), "the only install rules naming sega-cd stage the runtime and viewer sources (*.c, *.h)")
print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
