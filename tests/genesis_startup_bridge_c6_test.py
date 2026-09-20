#!/usr/bin/env python3
"""C6 regression for non-CPU general-startup frontier report metadata.

The project-authored programs below each complete one block before reaching a
different frontier.  Before C6, C5's bridge wrapper required CPU dimensions
for every frontier, so device, unmapped-memory, and unresolved-indirect
programs were rejected before the real driver could create an artifact.  C6
uses GENESIS_CPU_DIMENSIONS_NONE for those frontiers; the CPU control retains
its C5 dimensions.
"""
import base64
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile


ENTRY = "00000b00"
PREFIX = bytes((0x42, 0xB9, 0x00, 0xFF, 0x00, 0x00, 0x60, 0x02, 0x00, 0x00))


def run(command: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, cwd=cwd, check=False)


def load_driver(root: pathlib.Path):
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", root / "tools" / "genesis_startup_bridge.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load genesis startup bridge driver")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expected_provenance(frontier: bytes, access_address: str | None) -> dict:
    has_access = access_address is not None
    has_mapping = access_address is not None or frontier == bytes((0x4E, 0x70))
    image_length = len(PREFIX) + len(frontier)
    return {
        "has_instruction_provenance": True,
        "instruction": {"cpu_variant": "mc68000", "source_address": "0x00000b0a",
                        "image_offset": 10, "primary_bytes": frontier[:2].hex(), "length": len(frontier)},
        "has_access": has_access,
        "access_address": access_address if has_access else "0x00000000",
        "access_width": "long" if has_access else None,
        "access_direction": "read" if has_access else None,
        "mapping_claim_count": 1 if has_mapping else 0,
        "mapping_claims": ([{"name": "raw_cartridge_rom", "target_begin": "0x00000b00",
                             "target_end": f"0x{0xB00 + image_length:08x}", "image_begin": 0,
                             "image_end": image_length}] if has_mapping else []),
        "bus_access_count": 1 if has_mapping else 0,
        "bus_accesses": ([{"ordinal": 0, "kind": "instruction_read", "address": "0x00000b0a",
                          "raw_bytes": frontier.hex(), "raw_byte_count": len(frontier),
                          "region": "raw_cartridge_rom"}] if has_mapping else []),
    }


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(argument).resolve() for argument in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    bridge_driver = load_driver(root)
    cases = {
        # TST.L $00A10000: recognized controller-I/O region, but not T020's
        # narrow selector, so it is a device frontier.
        "device": (bytes((0x4A, 0xB9, 0x00, 0xA1, 0x00, 0x00)),
                    "unsupported_device_access", "unsupported_device_region_controller_io", "0x00a10000", None),
        # TST.L $00A00000: a statically known non-device unmapped access.
        "unmapped-memory": (bytes((0x4A, 0xB9, 0x00, 0xA0, 0x00, 0x00)),
                             "unsupported_memory_region", "unmapped_data_access", "0x00a00000", None),
        # JMP (A0): decoded control transfer with no statically resolved target.
        "unresolved-indirect": (bytes((0x4E, 0xD0)), "unresolved_indirect_target",
                                 "reached_unresolved_direct_edge", None, None),
        # Existing C5 control: RESET is a CPU frontier and has pinned dimensions.
        "cpu-control": (bytes((0x4E, 0x70)), "unsupported_cpu_form",
                         "valid_but_unsupported_instruction", None,
                         {"family": "reset", "size": "none", "addressing_mode_class": "implied"}),
    }
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        for name, (frontier, stop_class, category, access_address, dimensions) in cases.items():
            rom = temporary / f"{name}.bin"
            rom.write_bytes(PREFIX + frontier)
            digest = hashlib.sha256(rom.read_bytes()).hexdigest()
            with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c6-") as output:
                output_dir = pathlib.Path(output)
                full = temporary / f"{name}-full.json"
                # A successful driver result includes its strict C11 compile.
                driver_result = run([sys.executable, str(driver), "--segarecomp", str(binary),
                                     "--cc", str(compiler), "--rom", str(rom), "--entry", ENTRY,
                                      "--mode", "synthetic", "--out-dir", str(output_dir),
                                       "--full-report-path", str(full)], root)
                source = output_dir / "bridge.generated.c"
                source_text = ""
                try:
                    source_text = source.read_text()
                    report = json.loads(driver_result.stdout)
                    full_bytes = full.read_bytes()
                    full_report = bridge_driver.parse_canonical_full(full_bytes)
                except (OSError, json.JSONDecodeError):
                    report = None
                    full_report = None
                expected_runtime = {
                    "d": ["0x00000000"] * 8,
                    "a": ["0x00000000"] * 7 + ["0x00ff0004"],
                    "usp": "0x00000000",
                    "sr": "0x0004", "pc": "0x00000b0a",
                }
                source_dimensions = ("GENESIS_CPU_DIMENSIONS_NONE" if dimensions is None
                                     else "GENESIS_CPU_DIMENSIONS_RESET")
                source_prefix = '#define _POSIX_C_SOURCE 200809L\n#include "runtime.h"\n'
                if (driver_result.returncode != 0 or driver_result.stderr or not source.exists() or
                        not source_text.startswith(source_prefix) or
                        source_text.count("#define _POSIX_C_SOURCE 200809L") != 1 or
                        source_text.find('#include "runtime.h"') >= source_text.find("#include <errno.h>") or
                        "#include <limits.h>" not in source_text or
                        "value > (unsigned long long)INT_MAX" not in source_text or
                        "value >= (unsigned long long)INT_MAX" in source_text or
                        source_text.find("value > (unsigned long long)INT_MAX") >= source_text.find("return fdopen(") or
                        "genesis_open_inherited_report_stream(" not in source_text or
                        "_open_osfhandle(" not in source_text or source_dimensions not in source_text or not isinstance(report, dict) or
                        json.dumps(report, separators=(",", ":")) + "\n" != driver_result.stdout or
                         report.get("result") != "stop" or report.get("stop_class") != stop_class or
                         report.get("diagnostic_category") != category or report.get("cpu_dimensions") != dimensions or
                         report.get("c4_lowering_dimensions") is not None or
                         not isinstance(full_report, dict) or
                         not bridge_driver.valid_full(full_report, report, digest) or
                         full_report.get("c4_lowering_dimensions") is not None or
                        full_report.get("runtime", {}).get("d") != expected_runtime["d"] or
                        full_report.get("runtime", {}).get("a") != expected_runtime["a"] or
                        full_report.get("runtime", {}).get("usp") != expected_runtime["usp"] or
                        full_report.get("runtime", {}).get("sr") != expected_runtime["sr"] or
                        full_report.get("runtime", {}).get("pc") != expected_runtime["pc"] or
                        base64.b64decode(full_report.get("runtime", {}).get("work_ram_base64", "")) != bytes(65536) or
                        full_report.get("provenance") != expected_provenance(frontier, access_address)):
                    failures.append(name)
                    continue
                # INT_MAX passes the representability guard and exercises the
                # normal fdopen failure path. Larger, negative, and malformed
                # descriptors must fail before either report writer runs.
                for fd_name, descriptor in (
                    ("fd-intmax", str(2 ** 31 - 1)),
                    ("fd-intmax-plus-one", str(2 ** 31)),
                    ("fd-much-larger", "999999999999999999999999999999999999999999"),
                    ("fd-negative", "-1"),
                    ("fd-trailing", "7trailing"),
                ):
                    invalid_fd = run([str(output_dir / "bridge"), "--full-report-fd", descriptor], root)
                    if invalid_fd.returncode != 1 or invalid_fd.stdout or invalid_fd.stderr:
                        failures.append(name + "-" + fd_name)
    if failures:
        sys.stderr.write("C6 bridge regression failed: " + ", ".join(failures) + "\n")
        return 1
    print("genesis startup bridge C6: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
