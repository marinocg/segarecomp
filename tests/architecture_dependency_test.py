#!/usr/bin/env python3
"""Fail closed on T006's final module-direction boundaries."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def production_files(directory: Path):
    return tuple(directory.rglob("*.cpp")) + tuple(directory.rglob("*.hpp"))


def assert_absent(path: str) -> None:
    assert not (ROOT / path).exists(), f"obsolete compatibility surface remains: {path}"


def assert_no_token(paths, token: str, rule: str) -> None:
    offenders = [str(path.relative_to(ROOT)) for path in paths if token in path.read_text(encoding="utf-8")]
    assert not offenders, f"{rule}: {offenders}"


def assert_no_include(paths, token: str, rule: str) -> None:
    pattern = re.compile(r'^\s*#\s*include\s*[<"][^>"]*' + re.escape(token), re.MULTILINE)
    offenders = [str(path.relative_to(ROOT)) for path in paths if pattern.search(path.read_text(encoding="utf-8"))]
    assert not offenders, f"{rule}: {offenders}"


def main() -> None:
    assert_absent("include/segarecomp/m68k_pipeline.hpp")
    assert_absent("src/m68k_pipeline.cpp")
    assert_absent("src/m68k_pipeline_direct_flow.cpp")
    assert_absent("platforms/genesis/machine/include/segarecomp/machine/genesis/frontend_compat.hpp")

    cpu = production_files(ROOT / "libs" / "cpu" / "m68k" / "src") + production_files(ROOT / "libs" / "cpu" / "m68k" / "include" / "segarecomp" / "cpu" / "m68k")
    assert_no_include(cpu, "machine/genesis", "MC68000 must not depend on Genesis machine policy")
    assert_no_include(cpu, "device/sega/genesis", "MC68000 must not depend on Genesis device policy")

    core = production_files(ROOT / "libs" / "core" / "include" / "segarecomp" / "core")
    assert_no_include(core, "cpu/", "core must not import a CPU module")
    assert_no_include(core, "machine/", "core must not import a machine module")

    codegen = production_files(ROOT / "libs" / "codegen" / "c11" / "src")
    assert_no_include(codegen, "static_discovery.hpp", "codegen must not import discovery")

    recompiler = (production_files(ROOT / "libs" / "recompiler" / "src") +
                  production_files(ROOT / "libs" / "recompiler" / "include" / "segarecomp" / "recompiler"))
    assert_no_include(recompiler, "machine/genesis", "recompiler must not import Genesis machine policy")
    assert_no_include(recompiler, "device/sega/genesis", "recompiler must not import Genesis device policy")

    sh2_directories = (ROOT / "libs" / "cpu" / "sh2" / "src", ROOT / "libs" / "cpu" / "sh2" / "include" / "segarecomp" / "cpu" / "sh2")
    if any(directory.exists() for directory in sh2_directories):
        sh2 = tuple(path for directory in sh2_directories for path in production_files(directory))
        assert_no_include(sh2, "machine/x32", "SH-2 must not depend on 32X machine policy")
        assert_no_include(sh2, "machine/saturn", "SH-2 must not depend on Saturn machine policy")
        assert_no_include(sh2, "device/sega/x32", "SH-2 must not depend on 32X device policy")
        assert_no_include(sh2, "device/sega/saturn", "SH-2 must not depend on Saturn device policy")

    cli = [ROOT / "apps" / "segarecomp" / "main.cpp"]
    assert_no_token(cli, "M68kInstructionKind", "CLI must not classify MC68000 instruction kinds")
    assert_no_token(cli, "M68kEaMode", "CLI must not inspect MC68000 effective-address modes")
    assert_no_token(cli, "M68kMemoryEmissionContext", "CLI must not construct codegen memory contexts")
    assert_no_token(cli, "m68k_startup_ram_range_in_range", "CLI must not classify Genesis work-RAM mappings")
    # Command spelling and syntactic argument parsing remain CLI concerns, but
    # literal Genesis startup scenario composition has one machine owner.
    assert_no_token(cli, '"genesis-reset-image"', "CLI must not construct reset startup images")
    assert_no_token(cli, '"bridge-generation-input"', "CLI must not construct bridge startup images")
    assert_no_token(cli, '"raw_cartridge_rom"', "CLI must not construct Genesis startup mappings")
    assert_no_token(cli, "M68kStartupIngress", "CLI must not construct Genesis startup ingress")
    assert_no_token(cli, "FrontendProgram::CompletionContract",
                    "CLI must not construct synthetic completion contracts")
    assert_no_token(cli, "M68kFrontendProfile::genesis_rom_startup",
                    "CLI must not select Genesis startup profiles")
    assert_no_token(cli, "M68kFrontendProfile::general_startup",
                    "CLI must not select Genesis general-startup profiles")

    genesis_startup = ROOT / "platforms" / "genesis" / "machine" / "src" / "startup.cpp"
    genesis_frontend = ROOT / "platforms" / "genesis" / "machine" / "include" / "segarecomp" / "machine" / "genesis" / "frontend.hpp"
    for helper in ("make_genesis_reset_startup_state", "make_genesis_reset_startup_program",
                   "make_genesis_bridge_startup_program"):
        assert helper in genesis_startup.read_text(encoding="utf-8"), f"machine helper missing: {helper}"
        assert helper in genesis_frontend.read_text(encoding="utf-8"), f"machine declaration missing: {helper}"

    runtime = production_files(ROOT / "platforms" / "genesis" / "runtime")
    assert_no_include(runtime, "segarecomp/", "native runtime must not import host recompiler headers")
    assert_no_token(runtime, "std::", "native runtime must remain strict C11")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "add_library(segarecomp_core" not in cmake, "umbrella semantic target remains"
    assert "src/m68k_pipeline.cpp" not in cmake, "obsolete pipeline source remains in CMake"
    print("architecture dependency boundaries: ok")


if __name__ == "__main__":
    main()
