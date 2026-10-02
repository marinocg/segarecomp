#!/usr/bin/env python3
"""SEG-008-T002 / ADR 0059: cpu_z80 is independent; no generic library depends on it.

- libs/cpu/z80 links only segarecomp::base and includes none of cpu/m68k, codegen, machine, device or platform code;
- libs/core, libs/recompiler and libs/codegen/c11 (the generic libraries) never reference cpu_z80 or its headers
  (codegen_c11_z80, added by later tasks, is a sibling target and is not scanned here for the reverse rule).
Comments are stripped before scanning. Fully synthetic; needs no ROM.
usage: cpu_z80_dependency_test.py <product-root>
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".txt", ".cmake"}
FORBIDDEN_FROM_Z80 = ("cpu/m68k", "cpu_m68k", "codegen", "machine/", "machine_", "device/", "device_", "platforms",
                      "m68k", "genesis", "segarecomp/recompiler", "segarecomp/media")
GENERIC_LIBS = ("libs/core", "libs/recompiler", "libs/media", "libs/device", "libs/legacy_compat")
FAILED = []


def strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def files(directory):
    return [p for p in directory.rglob("*") if p.is_file() and p.suffix in SUFFIXES]


def check(cond, msg):
    if not cond:
        FAILED.append(msg)


z80 = ROOT / "libs" / "cpu" / "z80"
check(z80.is_dir(), "libs/cpu/z80 missing")
for path in files(z80):
    text = strip(path.read_text(encoding="utf-8")).lower()
    if path.name == "CMakeLists.txt":
        text = "\n".join(l for l in path.read_text(encoding="utf-8").splitlines() if not l.lstrip().startswith("#")).lower()
        links = re.findall(r"target_link_libraries\([^)]*\)", text, flags=re.S)
        if path.parent.relative_to(z80).parts[:1] == ("analysis",):
            # SEG-029 (ADR 0078): the report-only adapter target may link exactly cpu_z80, the generic analysis core and base.
            allowed = {"segarecomp::base", "segarecomp::cpu_z80", "segarecomp::analysis"}
            check(links and all(set(re.findall(r"segarecomp::[a-z0-9_]+", l)) <= allowed for l in links),
                  "cpu_z80_analysis may link only cpu_z80, analysis and base: %r" % links)
        else:
            check(links and all("segarecomp::base" in l and l.count("segarecomp::") == 1 for l in links),
                  "cpu_z80 must link only segarecomp::base: %r" % links)
    for token in FORBIDDEN_FROM_Z80:
        check(token not in text, "%s references forbidden dependency %r" % (path.relative_to(ROOT), token))

def cmake_links_z80_from_generic_target(text):
    """A target_link_libraries block of a non-z80 target that names cpu_z80."""
    for block in re.findall(r"target_link_libraries\(([^)]*)\)", text, flags=re.S):
        words = block.split()
        if words and "z80" not in words[0].lower() and any("cpu_z80" in w for w in words[1:]):
            return True
    return False


for lib in GENERIC_LIBS + ("libs/codegen/c11", "libs/cpu/m68k", "platforms", "apps"):
    directory = ROOT / lib
    if not directory.is_dir():
        continue
    for path in files(directory):
        if "z80" in path.name.lower() and path.name != "CMakeLists.txt":
            continue  # the sibling codegen_c11_z80 sources (ADR 0059) are allowed to use cpu_z80
        raw = path.read_text(encoding="utf-8", errors="ignore")
        if path.name == "CMakeLists.txt":
            check(not cmake_links_z80_from_generic_target(raw), "%s: a non-z80 target links cpu_z80" % path.relative_to(ROOT))
            continue
        check("cpu/z80" not in strip(raw) and "cpu_z80" not in strip(raw), "%s depends on cpu_z80" % path.relative_to(ROOT))

# negative controls: the scanners bite
check("cpu/m68k" in strip('#include "segarecomp/cpu/m68k/decode.hpp"\n').lower(), "control: include scanner")
check("cpu/z80" not in strip("// cpu/z80 mentioned in a comment\n"), "control: comment stripping")
check(cmake_links_z80_from_generic_target("target_link_libraries(segarecomp_recompiler PUBLIC segarecomp::cpu_z80)"), "control: cmake scanner")
check(not cmake_links_z80_from_generic_target("target_link_libraries(segarecomp_codegen_c11_z80 PUBLIC segarecomp::cpu_z80)"), "control: sibling target allowed")

if FAILED:
    print("\n".join("FAIL: " + m for m in FAILED))
    sys.exit(1)
print("cpu_z80 dependency boundaries: ok")
