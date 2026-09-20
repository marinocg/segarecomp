#!/usr/bin/env python3
"""SEG-014-T002: cpu/m68k/ must never depend on Genesis/machine content.

docs/architecture/post-seg007-architecture-refactor-contract.md section 8.2
("cpu/m68k/") states the forbidden dependency explicitly:

    cpu/m68k -> machine/genesis
    cpu/m68k -> device/sega/genesis

This is a project-authored, fail-closed regression modeled on
tests/stage1_classifier_tooling_only_test.py's dependency-direction-guard
pattern: it asserts that no file under libs/cpu/m68k/include/segarecomp/cpu/m68k/ or
libs/cpu/m68k/src/ (a) #includes the remaining Genesis-shaped god-header
(m68k_pipeline.hpp), or (b) references any symbol this task's migration map
(docs/architecture/seg-014-t001-symbol-migration-map.md) classifies as
Genesis/machine/recompiler/codegen content -- the exact content
m68k_pipeline.hpp/its three .cpp files still own.

Comments are stripped before scanning so historical/explanatory prose (e.g. a
relocated header's own doc comment citing its prior location, or a
static_program.hpp comment explaining *why* a Genesis-shaped function is
deliberately NOT moved here) cannot produce a false positive: only actual
C++ token references count. Fully synthetic; requires no commercial ROM and
no local Musashi checkout.
"""
import pathlib
import re
import sys

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
CPU_M68K_DIRS = ("libs/cpu/m68k/include/segarecomp/cpu/m68k", "libs/cpu/m68k/src")

FORBIDDEN_HEADER_NEEDLES = ("m68k_pipeline.hpp",)

# Genesis/machine/recompiler/codegen-shaped symbols that remain owned by
# m68k_pipeline.hpp/src/m68k_pipeline*.cpp per the migration map's
# `machine/genesis/`, `device/sega/genesis/`, `recompiler/`, and
# `codegen/c11/` sections. cpu/m68k/ content must never reference any of
# these identifiers.
FORBIDDEN_SYMBOLS = (
    # machine/genesis/ (Genesis-specific scenario/composition)
    "FrontendProgram", "FrontendAnalysis", "FrontendResult", "FrontendRejected",
    "FrontendPartialProgram", "FrontendImage", "FrontendOracleVector",
    "FrontendVectorAccepted", "FrontendVectorRejected", "FrontendVectorResult",
    "M68kFrontendProfile", "GenesisFrontierClass", "UnresolvedFrontier",
    "M68kMemoryAccessRequest", "M68kStartupIngress", "StartupInstructionKind",
    "StartupInstruction", "StartupState", "StartupBoundary", "StartupReturn",
    "StartupExecution", "StartupFailure", "StartupExecutionTestContext",
    "StartupResult", "StartupBusKind", "StartupBusRecord",
    "execute_m68k_frontend_startup", "analyze_m68k_frontend",
    "discover_m68k_general_startup", "discover_m68k_static_call_return",
    "StaticCallReturnResult", "format_m68k_general_startup_result",
    "format_m68k_frontend_result", "format_genesis_rom_startup_result",
    "validate_and_execute_m68k_frontend_vector",
    "m68k_startup_ram_begin", "m68k_startup_ram_end",
    "m68k_startup_ram_range_in_range", "m68k_startup_ram_operand_in_range",
    "m68k_startup_ram_offset",
    "m68k_resolve_absolute_test_operand", "M68kAbsoluteOperandRegion",
    "M68kAbsoluteTestOperand", "M68kAbsoluteTestOperandResolution",
    "m68k_route_genesis_device_access", "M68kGenesisDeviceRoutingResult",
    # device/sega/genesis/ (controller I/O device protocol)
    "ControllerIoAccessShapeMismatch", "ControllerIoTargetRegisterClass",
    "ControllerIoAccessShape", "M68kControllerIoPolicyProvenance",
    "M68kControllerIoWordObservation", "M68kControllerIoResult",
    "M68kControllerIoFailure", "M68kControllerIoAccessResult",
    "m68k_controller_io_access", "m68k_classify_controller_io_access_shape",
    "m68k_classify_controller_io_target_register_class",
    # recompiler/ (partial-program/frontier/C4 compilation-plan composition)
    "M68kStaticMemoryFactRole", "M68kStaticMemoryFact",
    "M68kMovemAdjacentLeaFact", "M68kOwnedCartridgeRegionFact",
    # (M68kC4* now legitimately live in cpu/m68k/c4.hpp, SEG-018-T003)
    "preflight_m68k_general_startup_c4", "StaticEmissionUnit",
    # codegen/c11/ (C11 rendering; cpu/m68k decides meaning, never renders)
    "M68kMemoryEmissionContext", "emit_m68k_operation_c",
    "emit_m68k_direct_flow_c", "emit_m68k_structured_direct_flow_c",
    "emit_m68k_general_startup_runtime_block_c",
    "emit_m68k_general_startup_runtime_c",
    "emit_m68k_general_startup_bridge_c", "emit_m68k_frontend_c",
    "emit_c_manifest",
)

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(" ", text)
    text = _LINE_COMMENT_RE.sub("", text)
    return text


def main() -> None:
    offenders = []
    for directory in CPU_M68K_DIRS:
        root = PROJECT_ROOT / directory
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            try:
                raw_text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            rel = str(path.relative_to(PROJECT_ROOT))

            for line in raw_text.splitlines():
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle in FORBIDDEN_HEADER_NEEDLES:
                    if needle in stripped:
                        offenders.append((rel, f"#include of {needle}"))

            code_only = _strip_comments(raw_text)
            for symbol in FORBIDDEN_SYMBOLS:
                if re.search(r"\b" + re.escape(symbol) + r"\b", code_only):
                    offenders.append((rel, f"references {symbol}"))

    assert not offenders, (
        "cpu/m68k/ must not depend on Genesis/machine/recompiler/codegen "
        f"content, found: {offenders}"
    )
    print("cpu/m68k no-Genesis-dependency test: ok")


if __name__ == "__main__":
    main()
    sys.exit(0)
