#!/usr/bin/env python3
"""SEG-009-T002 / ADR 0063: Master System machine placement and dependency direction (hermetic).

  - the machine library links only media, codegen_c11_z80, the CPU-neutral executable-image projection (SEG-028) and the SMS runtime (no M68k, Genesis machine/device or other
    platform target); the runtime library links only the platform-neutral PSG device library and sees the Z80 ABI header by include path;
  - no source in the SMS machine/runtime trees includes or mentions M68k/Genesis vocabulary;
  - the mapper contract table is ONE definition: the C runtime and the C++ ImageSet builder both include it and neither
    re-declares slot bases, reset values or the bank mask;
  - mapper behaviour stays out of libs/cpu/z80, the Z80 emitter and the Z80 runtime header (non-goal), and Genesis files are
    not referenced from SMS code;
  - run-time sources are plain C11 (no C++ include, no host file I/O).
usage: sms_machine_dependency_test.py <product-root>
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
SMS = ROOT / "platforms" / "master-system"
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def link_block(cmake_text, target):
    m = re.search(r"target_link_libraries\(%s\s+PUBLIC\s+([^)]*)\)" % target, cmake_text)
    return sorted(m.group(1).split()) if m else None


machine_cmake = (SMS / "machine" / "CMakeLists.txt").read_text()
check(link_block(machine_cmake, "segarecomp_machine_master_system") ==
      ["segarecomp::codegen_c11_z80", "segarecomp::codegen_c11_z80_image", "segarecomp::media", "segarecomp::runtime_master_system"],
      "machine_master_system links exactly media, codegen_c11_z80, codegen_c11_z80_image (SEG-028) and runtime_master_system")
runtime_cmake = (SMS / "runtime" / "CMakeLists.txt").read_text()
check(link_block(runtime_cmake, "segarecomp_runtime_master_system") == ["segarecomp::device_psg"],
      "the SMS runtime library links only the platform-neutral PSG device library")
check("C_STANDARD 11" in runtime_cmake and "C_EXTENSIONS NO" in runtime_cmake, "the SMS runtime is strict C11")

FORBIDDEN = re.compile(r"m68k|genesis|megadrive|cpu_m68k|device_genesis", re.I)
sources = [p for p in SMS.rglob("*") if p.suffix in (".c", ".h", ".cpp", ".hpp", ".txt")]
check(len(sources) >= 10, "SMS sources not found")
for path in sources:
    if path.name == "README.md":
        continue
    text = path.read_text()
    check(not FORBIDDEN.search(text), "%s mentions M68k/Genesis" % path.relative_to(ROOT))

for path in (SMS / "runtime").glob("*.[ch]"):
    text = path.read_text()
    for inc in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M):
        ok = inc in ("stdint.h", "string.h", "stddef.h") or inc.startswith("sms_") or inc in ("segarecomp/codegen/c11/runtime/z80_runtime.h", "segarecomp/device/sega/psg/sn76489.h")
        check(ok, "%s includes %s" % (path.name, inc))
    check("fopen" not in text and "stdio" not in text, "%s performs host I/O" % path.name)

# one mapper contract definition
contract = (SMS / "runtime" / "sms_mapper_contract.h").read_text()
check("sms_sega_slots" in contract and "sms_sega_reset_regs" in contract, "contract table missing")
for path in (SMS / "runtime" / "sms_memory.c", SMS / "machine" / "src" / "image_set.cpp"):
    text = path.read_text()
    check("sms_sega_slots" in text or "sms_mapper_code_image" in text, "%s does not consume the contract" % path.name)
    check(not re.search(r"0x(4000|8000)u?\s*,\s*0x0000", text), "%s re-declares slot windows" % path.name)
image_set = (SMS / "machine" / "src" / "image_set.cpp").read_text()
check("sms_sega_slots" in image_set, "the ImageSet builder does not derive windows from the contract table")
memory = (SMS / "runtime" / "sms_memory.c").read_text()
check("sms_mapper_code_image" in memory and "sms_sega_rom_offset" in memory and "sms_sega_reset_regs" in memory,
      "the run-time memory map does not derive its decisions from the contract table")

# non-goal: no mapper behaviour in the Z80 CPU side
for rel in ("libs/cpu/z80", "libs/codegen/c11/src/z80.cpp", "libs/codegen/c11/include/segarecomp/codegen/c11/z80.hpp",
            "libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h"):
    base = ROOT / rel
    for path in ([base] if base.is_file() else sorted(base.rglob("*"))):
        if path.is_file():
            check(not re.search(r"master_system|sms_|mapper", path.read_text(), re.I), "%s mentions the SMS mapper" % path.relative_to(ROOT))

if FAILED:
    print("\n".join(FAILED))
    sys.exit(1)
print("sms machine dependency: ok")
