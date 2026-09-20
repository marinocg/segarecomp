#!/usr/bin/env python3
"""Prove the production C11 rendering owner is codegen/c11, not the legacy
pipeline/machine-frontend files.

docs/architecture/seg-014-t001-symbol-migration-map.md assigns nine C11
rendering responsibilities to codegen/c11: M68kMemoryEmissionContext,
emit_m68k_operation_c, emit_m68k_general_startup_runtime_block_c,
emit_m68k_general_startup_runtime_c (both overloads),
emit_m68k_general_startup_bridge_c (both overloads), emit_m68k_frontend_c,
emit_m68k_direct_flow_c, emit_m68k_structured_direct_flow_c, and
emit_c_manifest. SEG-014-T005's correction pass physically moved every real
rendering body out of src/m68k_pipeline.cpp, src/m68k_pipeline_direct_flow.cpp,
platforms/genesis/machine/src/frontend.cpp, and src/c_emitter.cpp into libs/codegen/c11/src/.

This regression fails closed if a real definition body reappears in any of
those four legacy files, and fails closed if codegen/c11 stops owning the
definition, without being whitespace-brittle: it matches a definition-shaped
line (`std::string <name>(` optionally preceded only by whitespace/
`[[nodiscard]]`), independent of parameter formatting/line-wrapping, and
never a call site (which is never at column 0 with a `std::string` return
prefix).
"""

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]

# Every emitter this task's migration map assigns to codegen/c11 that has a
# real (non-forwarding) function body somewhere in the tree today.
EMITTERS = (
    "emit_m68k_operation_c",
    "emit_m68k_general_startup_runtime_block_c",
    "emit_m68k_general_startup_runtime_c",
    "emit_m68k_general_startup_bridge_c",
    "emit_m68k_frontend_c",
    "emit_m68k_direct_flow_c",
    "emit_m68k_structured_direct_flow_c",
    "emit_c_manifest",
)

CODEGEN_OWNERS = (
    ROOT / "libs" / "codegen" / "c11" / "src" / "m68k.cpp",
    ROOT / "libs" / "codegen" / "c11" / "src" / "frontend.cpp",
    ROOT / "libs" / "codegen" / "c11" / "src" / "direct_flow.cpp",
    ROOT / "libs" / "codegen" / "c11" / "src" / "genesis.cpp",
    ROOT / "libs" / "codegen" / "c11" / "src" / "manifest.cpp",
)

# The exact legacy files this correction pass must have vacated. These are
# the only historical production owners named in the migration map's
# codegen/c11 section; a real definition body reappearing here (not merely a
# forwarding call) is the exact regression this test exists to catch.
FORBIDDEN_OWNERS = (
    ROOT / "platforms" / "genesis" / "machine" / "src" / "frontend.cpp",
)


def _definition_pattern(name: str) -> re.Pattern:
    # A real definition is a top-level (non-call) declaration: optionally
    # `[[nodiscard]] `, the `std::string` return type, the emitter name, then
    # `(`. A call site is never written with the return type immediately
    # before the name. Anchored to (optional-whitespace) start-of-line so an
    # in-body call like `out << emit_m68k_operation_c(...)` never matches.
    return re.compile(
        r"^[ \t]*(?:\[\[nodiscard\]\]\s*)?std::string\s+" + re.escape(name) + r"\s*\(",
        re.MULTILINE,
    )


def main() -> None:
    for owner in CODEGEN_OWNERS:
        assert owner.is_file(), f"expected codegen/c11 owner file missing: {owner}"
    codegen_text = {owner: owner.read_text(encoding="utf-8") for owner in CODEGEN_OWNERS}
    legacy_text = {legacy: legacy.read_text(encoding="utf-8") for legacy in FORBIDDEN_OWNERS}

    for name in EMITTERS:
        pattern = _definition_pattern(name)
        owned_by = [owner for owner, text in codegen_text.items() if pattern.search(text)]
        assert owned_by, (
            f"{name}: no codegen/c11 owner defines this emitter; "
            "SEG-014-T005's codegen ownership gap is not closed"
        )
        duplicated_in = [legacy for legacy, text in legacy_text.items() if pattern.search(text)]
        assert not duplicated_in, (
            f"{name}: a real definition body still exists outside codegen/c11 in "
            f"{[str(path.relative_to(ROOT)) for path in duplicated_in]}"
        )

    print("codegen/c11 emitter ownership: ok")


if __name__ == "__main__":
    main()
