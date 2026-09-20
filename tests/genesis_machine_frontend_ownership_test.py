#!/usr/bin/env python3
"""Keep Genesis scenario/C4 ownership above the MC68000 module."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> None:
    legacy = ROOT / "src/m68k_pipeline_frontend.cpp"
    machine = ROOT / "platforms/genesis/machine/src/frontend.cpp"
    header = ROOT / "platforms/genesis/machine/include/segarecomp/machine/genesis/frontend.hpp"
    assert not legacy.exists()
    assert machine.is_file()
    assert header.is_file()
    contract = ROOT / "libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"
    address_contract = ROOT / "platforms/genesis/machine/include/segarecomp/machine/genesis/address_space_contract.h"
    controller = ROOT / "libs/device/sega/genesis/src/controller_io.cpp"
    address_space = ROOT / "platforms/genesis/machine/src/address_space.cpp"
    controller_header = ROOT / "libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io.hpp"
    address_header = ROOT / "platforms/genesis/machine/include/segarecomp/machine/genesis/address_space.hpp"
    assert contract.is_file() and address_contract.is_file()
    assert controller.is_file() and address_space.is_file()
    assert controller_header.is_file() and address_header.is_file()
    source = machine.read_text(encoding="utf-8")
    # These are Genesis scenario/machine facts, not CPU discovery semantics.
    for owned in (
        "M68kGeneralStartupEnvironment",
        "m68k_classify_general_memory_access",
        "runtime_frontier_eligible",
        "M68kStaticMemoryFact",
        "M68kMovemAdjacentLeaFact",
        "M68kOwnedCartridgeRegionFact",
    ):
        assert owned in source
    # An access record is carried broadly by CPU discovery.  Genesis promotion
    # remains deliberately narrower: exact diagnostic category and access
    # presence must both agree before an executable frontier is formed.
    assert "unsupported_device_region_controller_io" in source
    assert "unmapped_data_access" in source
    assert "has_frontier_access ? std::optional" in source
    assert "if (needs_access != access.has_value()) return false;" in source
    cpu = (ROOT / "libs/cpu/m68k/src/static_discovery.cpp").read_text(encoding="utf-8")
    assert "machine/genesis" not in cpu
    assert "Genesis" not in cpu
    runtime = (ROOT / "platforms/genesis/runtime/runtime.c").read_text(encoding="utf-8")
    assert "controller_io_contract.h" in runtime
    assert "address_space_contract.h" in runtime
    assert "0x00FF0000" not in runtime
    assert "0x01000000" not in runtime
    controller_source = controller.read_text(encoding="utf-8")
    address_source = address_space.read_text(encoding="utf-8")
    assert "M68kControllerIoAccessResult m68k_controller_io_access" in controller_source
    assert "M68kGenesisDeviceRoutingResult m68k_route_genesis_device_access" in address_source
    assert "M68kAbsoluteTestOperandResolution m68k_resolve_absolute_test_operand" in address_source
    controller_public = controller_header.read_text(encoding="utf-8")
    address_public = address_header.read_text(encoding="utf-8")
    assert "m68k_pipeline.hpp" not in controller_public
    assert "m68k_pipeline.hpp" not in address_public
    assert "M68kControllerIoAccessResult" in controller_public
    assert "M68kAbsoluteTestOperandResolution" in address_public
    print("Genesis machine frontend ownership: ok")


if __name__ == "__main__":
    main()
