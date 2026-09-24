#!/usr/bin/env python3
"""Keep the generated-program runtime a standalone strict-C11 owner."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> None:
    runtime_dir = ROOT / "platforms" / "genesis" / "runtime"
    header = runtime_dir / "runtime.h"
    source = runtime_dir / "runtime.c"
    legacy_header = ROOT / "tools" / "genesis_startup_bridge_runtime.h"
    legacy_source = ROOT / "tools" / "genesis_startup_bridge_runtime.c"
    cmake = "".join((ROOT / p).read_text(encoding="utf-8") for p in ("platforms/genesis/runtime/CMakeLists.txt", "libs/codegen/c11/CMakeLists.txt"))
    bridge = (ROOT / "tools" / "genesis_startup_bridge.py").read_text(encoding="utf-8")
    # SEG-014-T005 correction: the bridge ABI-splice/render call sites this
    # test exercises moved from platforms/genesis/machine/src/frontend.cpp (machine
    # scenario ownership) into libs/codegen/c11/src/frontend.cpp (C11 rendering
    # ownership); see docs/architecture/seg-014-t001-symbol-migration-map.md.
    codegen = (ROOT / "libs" / "codegen" / "c11" / "src" / "frontend.cpp").read_text(encoding="utf-8")
    machine_frontend = (ROOT / "platforms" / "genesis" / "machine" / "src" / "frontend.cpp").read_text(encoding="utf-8")
    codegen_owner = ROOT / "libs" / "codegen" / "c11" / "src" / "genesis.cpp"

    assert header.is_file() and source.is_file()
    assert not legacy_source.exists()
    assert legacy_header.read_text(encoding="utf-8").count("#include") == 1
    assert "../platforms/genesis/runtime/runtime.h" in legacy_header.read_text(encoding="utf-8")

    runtime_text = source.read_text(encoding="utf-8")
    assert "controller_io_contract.h" in runtime_text
    assert "address_space_contract.h" in runtime_text
    assert "#include \"runtime.h\"" in runtime_text
    for forbidden in ("#include <segarecomp/", ".hpp", "std::", "decode_m68k", "lift_m68k",
                      "emit_m68k", "opcode decoder", "interpreter"):
        assert forbidden not in runtime_text

    assert "add_library(segarecomp_runtime_genesis STATIC" in cmake
    assert "runtime.c" in cmake
    assert "C_STANDARD 11" in cmake
    assert "target_link_libraries(segarecomp_runtime_genesis" not in cmake
    assert codegen_owner.is_file()
    assert "add_library(segarecomp_codegen_c11" in cmake
    assert "target_link_libraries(segarecomp_codegen_c11_genesis PUBLIC segarecomp::codegen_c11_m68k segarecomp::machine_genesis segarecomp::recompiler)" in cmake
    assert '"platforms" / "genesis" / "runtime" / "runtime.c"' in bridge
    assert '"platforms" / "genesis" / "runtime"' in bridge
    # SEG-022-T002: the frontend streams the runtime header through the single owner helper.
    assert "emit_genesis_runtime_c11_include" in codegen
    assert '#include \\"runtime.h\\"' in codegen_owner.read_text(encoding="utf-8")
    assert "genesis_startup_bridge_runtime.h" not in codegen
    assert "emit_genesis_bridge_c11_prelude" in codegen
    assert "emit_genesis_bridge_c11_main" in codegen
    assert "emit_genesis_bridge_c11_main_open" in codegen
    assert "emit_genesis_bridge_c11_main_finish" in codegen
    for duplicate_scaffold in (
            '"#define _POSIX_C_SOURCE 200809L',
            '"static int genesis_write_requested_full_report',
            '"int main(int argc, char **argv) { GenesisRuntime runtime',
    ):
        assert duplicate_scaffold not in codegen
    # The Genesis machine frontend retains only analysis/scenario/C4-fact
    # composition; it must never grow its own copy of the bridge ABI
    # splice/render call sites this test already proves codegen/c11 owns.
    for moved_to_codegen in (
            '#include \\"runtime.h\\"',
            "emit_genesis_bridge_c11_prelude",
            "emit_genesis_bridge_c11_main",
            "genesis_startup_bridge_runtime.h",
    ):
        assert moved_to_codegen not in machine_frontend
    owner_text = codegen_owner.read_text(encoding="utf-8")
    assert "emit_genesis_runtime_c11_include" in owner_text
    assert "emit_genesis_bridge_c11_prelude" in owner_text
    assert "emit_genesis_bridge_c11_main" in owner_text
    assert "emit_genesis_bridge_c11_main_open" in owner_text
    assert "emit_genesis_bridge_c11_main_finish" in owner_text
    print("Genesis runtime ownership: ok")


if __name__ == "__main__":
    main()
