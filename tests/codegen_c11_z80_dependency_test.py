#!/usr/bin/env python3
"""SEG-008-T003 / ADR 0059: codegen_c11_z80 dependency direction and the family-file extension seam (hermetic).

  - the target links only segarecomp::codegen_c11 and segarecomp::cpu_z80 (no cpu_m68k, codegen_c11_m68k, Genesis
    machine/device or platform target);
  - its sources include only their own headers, the generic sharding/entry-table headers and cpu_z80 headers, and use
    no M68k/Genesis vocabulary (also inside emitted C text);
  - the generic C11 sources (common support, sharding, entry table) never mention Z80;
  - the machine-neutral runtime ABI header includes only C11 stddef/stdint;
  - every lowering family lives in its own src/z80_lower_<family>.cpp that depends only on z80_lowering.hpp, and the
    merged registry lists each family accessor exactly once (the extension protocol of
    docs/testing/z80-conformance-harness.md).
usage: codegen_c11_z80_dependency_test.py <product-root>
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
C11 = ROOT / "libs" / "codegen" / "c11"
INC = C11 / "include" / "segarecomp" / "codegen" / "c11"
FAMILIES = ("data_alu", "control_stack", "cb_bit_prefix", "ed_io_interrupt")
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def includes(text):
    return re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M)


Z80_SOURCES = [C11 / "src" / n for n in ("z80.cpp", "z80_lowering.cpp")] + [C11 / "src" / ("z80_lower_%s.cpp" % f) for f in FAMILIES]
Z80_HEADERS = [INC / "z80.hpp", INC / "z80_lowering.hpp"]
RUNTIME = INC / "runtime" / "z80_runtime.h"
ALLOWED_PREFIXES = ("segarecomp/codegen/c11/z80", "segarecomp/codegen/c11/compiled_entry_table.hpp",
                    "segarecomp/codegen/c11/translation_units.hpp", "segarecomp/cpu/z80/")
FORBIDDEN_VOCABULARY = re.compile(r"m68k|genesis|vdp|megadrive|cpu_m68k|machine_|device/|platforms", re.I)

cmake = (C11 / "CMakeLists.txt").read_text()
block = re.search(r"target_link_libraries\(segarecomp_codegen_c11_z80\s+PUBLIC\s+([^)]*)\)", cmake)
check(block is not None, "codegen_c11_z80 has no link block")
if block:
    check(sorted(block.group(1).split()) == ["segarecomp::codegen_c11", "segarecomp::cpu_z80"],
          "codegen_c11_z80 must link exactly codegen_c11 and cpu_z80: %r" % block.group(1).split())
check("segarecomp::codegen_c11_z80" in cmake, "codegen_c11_z80 alias missing")

for path in Z80_SOURCES + Z80_HEADERS:
    check(path.is_file(), "%s is missing" % path.name)
    if not path.is_file():
        continue
    text = path.read_text()
    for inc in includes(text):
        std = "/" not in inc and "." not in inc
        check(std or inc.startswith(ALLOWED_PREFIXES), "%s includes %s" % (path.name, inc))
    check(not FORBIDDEN_VOCABULARY.search(text), "%s uses M68k/Genesis/platform vocabulary: %s" % (
        path.name, FORBIDDEN_VOCABULARY.search(text).group(0) if FORBIDDEN_VOCABULARY.search(text) else ""))

runtime = RUNTIME.read_text()
check(sorted(includes(runtime)) == ["stddef.h", "stdint.h"], "runtime ABI header must include only stddef.h and stdint.h")
check(not FORBIDDEN_VOCABULARY.search(runtime), "runtime ABI header carries machine policy vocabulary")

GENERIC = [C11 / "src" / "manifest.cpp", C11 / "src" / "translation_units.cpp", INC / "translation_units.hpp",
           INC / "compiled_entry_table.hpp", C11 / "include" / "segarecomp" / "c_emitter.hpp"]
for path in GENERIC:
    check("z80" not in path.read_text().lower(), "generic %s mentions Z80" % path.name)

registry = (C11 / "src" / "z80_lowering.cpp").read_text()
for family in FAMILIES:
    accessor = "%s_lowering_rows" % family
    file_text = (C11 / "src" / ("z80_lower_%s.cpp" % family)).read_text()
    check(("std::span<const LoweringRow> %s()" % accessor) in file_text, "%s.cpp does not define %s" % (family, accessor))
    check(registry.count(accessor + "()") == 1, "registry must list %s exactly once" % accessor)
    check(includes(file_text) == ["segarecomp/codegen/c11/z80_lowering.hpp"], "%s.cpp must include only z80_lowering.hpp" % family)
    other = [f for f in FAMILIES if f != family]
    check(not any(("%s_lowering_rows" % o) in file_text for o in other), "%s.cpp references another family" % family)
check(len(re.findall(r"_lowering_rows\(\)", registry)) == len(FAMILIES), "registry lists a family accessor twice or an unknown one")

if FAILED:
    print("\n".join("FAIL: " + m for m in FAILED))
    sys.exit(1)
print("codegen_c11_z80 dependency boundaries and family seam: ok")
