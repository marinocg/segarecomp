#!/usr/bin/env python3
"""SEG-018-T004: C11 codegen dependency direction, mechanically enforced.

  codegen_c11          (common)  -> no CPU, Genesis machine/device, or wrapper dependency
  codegen_c11_m68k     (lowering)-> no Genesis machine/device or Genesis-wrapper dependency
  codegen_c11_genesis  (wrapper) -> may depend on both

Checks #include directives, CMake link edges, and (for the common and M68k sources) any
occurrence of Genesis platform vocabulary in the M68k lowering -- including inside emitted C text, since a
runtime ABI spelled in the lowering is a real platform dependency. The Genesis machine must
also not depend on C11 rendering. Fully synthetic.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
C11 = ROOT / "libs" / "codegen" / "c11"
INC = C11 / "include" / "segarecomp"

COMMON_FILES = [C11 / "src" / "manifest.cpp", INC / "c_emitter.hpp"]
M68K_FILES = [C11 / "src" / "m68k.cpp", C11 / "src" / "direct_flow.cpp",
              INC / "codegen" / "c11" / "m68k.hpp"]
WRAPPER_FILES = [C11 / "src" / "frontend.cpp", C11 / "src" / "genesis.cpp",
                 C11 / "src" / "genesis_m68k_runtime.cpp",
                 INC / "codegen" / "c11" / "genesis.hpp",
                 INC / "codegen" / "c11" / "genesis_frontend.hpp"]

GENESIS_INCLUDES = ("segarecomp/machine/genesis", "segarecomp/device/sega/genesis",
                    "codegen/c11/genesis")
CPU_INCLUDES = ("segarecomp/cpu/", "codegen/c11/m68k")


def includes(path):
    return re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(), re.M)


PLATFORM_VOCABULARY = re.compile(
    r"genesis|vdp|controller_io|synthetic_work_ram|raw_cartridge_rom|routed_device|"
    r"M68kAbsoluteOperandRegion|m68k_startup_ram|00FF0000", re.I)


def cmake_block(text, target):
    m = re.search(r"target_link_libraries\(%s\s+PUBLIC\s+([^)]*)\)" % target, text)
    return m.group(1).split() if m else []


def main():
    failures = []
    for path in COMMON_FILES:
        for inc in includes(path):
            if any(n in inc for n in GENESIS_INCLUDES + CPU_INCLUDES):
                failures.append(f"common {path.name} includes {inc}")
    for path in M68K_FILES:
        for inc in includes(path):
            if any(n in inc for n in GENESIS_INCLUDES):
                failures.append(f"m68k lowering {path.name} includes {inc}")
    for path in M68K_FILES:
        text = path.read_text()
        m = PLATFORM_VOCABULARY.search(text)
        if m:
            line = text.count("\n", 0, m.start()) + 1
            failures.append(f"{path.name}:{line} contains platform vocabulary {m.group(0)!r}")
    machine = ROOT / "platforms" / "genesis" / "machine"
    for path in list(machine.rglob("*.hpp")) + list(machine.rglob("*.cpp")):
        for inc in includes(path):
            if "codegen/c11" in inc:
                failures.append(f"genesis machine {path.name} includes {inc}")
    if "codegen" in (machine / "CMakeLists.txt").read_text():
        failures.append("genesis machine CMake references codegen")
    for path in WRAPPER_FILES:
        if not path.exists():
            failures.append(f"missing wrapper file {path}")

    cm = (C11 / "CMakeLists.txt").read_text()
    common = cmake_block(cm, "segarecomp_codegen_c11")
    lowering = cmake_block(cm, "segarecomp_codegen_c11_m68k")
    wrapper = cmake_block(cm, "segarecomp_codegen_c11_genesis")
    if not common or not lowering or not wrapper:
        failures.append("codegen CMake targets not declared")
    for dep in common:
        if any(k in dep for k in ("cpu_", "genesis", "m68k", "recompiler")):
            failures.append(f"common links {dep}")
    for dep in lowering:
        if "genesis" in dep:
            failures.append(f"m68k lowering links {dep}")
    for name in ("m68k.cpp", "direct_flow.cpp", "manifest.cpp", "frontend.cpp", "genesis.cpp"):
        want = {"manifest.cpp": "segarecomp_codegen_c11 ",
                "m68k.cpp": "segarecomp_codegen_c11_m68k", "direct_flow.cpp": "segarecomp_codegen_c11_m68k",
                "frontend.cpp": "segarecomp_codegen_c11_genesis", "genesis.cpp": "segarecomp_codegen_c11_genesis"}[name]
        if not re.search(r"add_library\(%s[^)]*src/%s" % (want.strip(), re.escape(name)), cm):
            failures.append(f"{name} not owned by {want.strip()}")
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print("codegen c11 layering ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
