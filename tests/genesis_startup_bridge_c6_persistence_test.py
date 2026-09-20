#!/usr/bin/env python3
"""C6 persistent work-RAM MOVE.L bridge regression."""
import base64
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


ENTRY = "00000b00"
# Project-authored synthetic programs: MOVEQ; Dn -> RAM; RAM -> Dm; device frontier.
CASES = (
    (bytes.fromhex("700123c000ff001060020000223900ff0010600200004ab900a10000"),
     0, 1, 16, "00000001"),
    (bytes.fromhex("707f23c000ff0020600200002a3900ff0020600200004ab900a10000"),
     0, 5, 32, "0000007f"),
)


def run(command: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, cwd=cwd, check=False)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    try:
        policy_source = (root / "platforms" / "genesis" / "machine" / "src" / "frontend.cpp").read_text()
        require("0x00FF0010" not in policy_source and "source_ea.reg ==" not in policy_source and
                "destination_ea.reg ==" not in policy_source, "fixture-specific bridge policy")
        for image, source_register, destination_register, ram_offset, value in CASES:
            digest = hashlib.sha256(image).hexdigest()
            with tempfile.TemporaryDirectory() as temporary:
                work = pathlib.Path(temporary)
                rom = work / "persistence.bin"
                rom.write_bytes(image)
                with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c6-persistence-") as output:
                    output_dir = pathlib.Path(output)
                    full_path = work / "full.json"
                    command = [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                               "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
                               "--out-dir", str(output_dir), "--expect-sha256", digest]
                    compared = run(command + ["--compare-runs"], root)
                    require(compared.stdout, f"missing compared report: {compared.stderr}")
                    report = json.loads(compared.stdout)
                    require(compared.returncode == 0 and not compared.stderr and report == {
                        "schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
                        "result": "stop", "stop_class": "unsupported_device_access",
                        "diagnostic_category": "unsupported_device_region_controller_io",
                        "cpu_dimensions": None, "c4_lowering_dimensions": None, "reports_match": True}, "unexpected compared report")
                    source = (output_dir / "bridge.generated.c").read_text()
                    require("genesis_block_00000B00" in source and "genesis_block_00000B0C" in source and
                            "static GenesisControlTransfer genesis_dispatch" in source and
                            source.count("static GenesisControlTransfer genesis_block_") == 2 and
                            source.count("genesis_route_access(") == 2 and "runtime.work_ram[" not in source and
                            "translation rejected" not in source and "decode" not in source.lower(),
                            "missing static blocks or routed MOVE.L lowering")

                    executed = run(command + ["--full-report-path", str(full_path)], root)
                    full = json.loads(full_path.read_text())
                    ram = base64.b64decode(full["runtime"]["work_ram_base64"], validate=True)
                    expected_provenance = {
                        "has_instruction_provenance": True,
                        "instruction": {"cpu_variant": "mc68000", "source_address": "0x00000b16",
                                        "image_offset": 22, "primary_bytes": "4ab9", "length": 6},
                        "has_access": True, "access_address": "0x00a10000", "access_width": "long",
                        "access_direction": "read", "mapping_claim_count": 1,
                        "mapping_claims": [{"name": "raw_cartridge_rom", "target_begin": "0x00000b00",
                                            "target_end": "0x00000b1c", "image_begin": 0, "image_end": 28}],
                        "bus_access_count": 1,
                        "bus_accesses": [{"ordinal": 0, "kind": "instruction_read", "address": "0x00000b16",
                                          "raw_bytes": "4ab900a10000", "raw_byte_count": 6,
                                          "region": "raw_cartridge_rom"}],
                    }
                    expected_registers = ["0x00000000"] * 8
                    expected_registers[source_register] = f"0x{value}"
                    expected_registers[destination_register] = f"0x{value}"
                    require(executed.returncode == 0 and not executed.stderr and
                        full["result"] == "stop" and full["stop_class"] == "unsupported_device_access" and
                        full["diagnostic_category"] == "unsupported_device_region_controller_io" and
                        full["c4_lowering_dimensions"] is None and
                            full["runtime"]["d"] == expected_registers and
                            full["runtime"]["a"] == ["0x00000000"] * 7 + ["0x00ff0004"] and
                            full["runtime"]["sr"] == "0x0000" and full["runtime"]["pc"] == "0x00000b16" and
                            ram[ram_offset:ram_offset + 4] == bytes.fromhex(value) and
                            not any(ram[:ram_offset] + ram[ram_offset + 4:]) and
                             full["provenance"] == expected_provenance, "unexpected persistent runtime result")
        # A fully accepted two-block cycle uses the same structural lowering as
        # the partial-program cases above.  Its bounded stop proves the D5
        # load and persistent RAM write survive repeated dispatcher entries.
        image = bytes.fromhex("707f23c000ff0020600200002a3900ff002060ec")
        digest = hashlib.sha256(image).hexdigest()
        with tempfile.TemporaryDirectory() as temporary:
            work = pathlib.Path(temporary)
            rom = work / "accepted-cycle.bin"
            full_path = work / "full.json"
            rom.write_bytes(image)
            with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c6-cycle-") as output:
                output_dir = pathlib.Path(output)
                command = [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                           "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
                           "--out-dir", str(output_dir), "--expect-sha256", digest,
                           "--full-report-path", str(full_path)]
                executed = run(command, root)
                full = json.loads(full_path.read_text())
                ram = base64.b64decode(full["runtime"]["work_ram_base64"], validate=True)
                # SEG-007-T252 / ADR-0040: this accepted two-block BRA-only
                # cycle emits no CPU/codegen-proven loop-progress note (there
                # is no DBcc here, and no such note exists any more), so the
                # runner's own default 128-step dispatch allowance is what
                # eventually reports the disjoint runner_resource_limit
                # outcome -- never the guest instruction_budget_exhausted
                # stop. The per-step dispatch() call count and effects (and
                # therefore the resulting runtime/d/pc/ram state) are exactly
                # as before this task: only the top-level result labeling and
                # the new deterministic runner_dispatch_count field differ.
                require(executed.returncode == 0 and not executed.stderr and
                        full["result"] == "runner_resource_limit" and
                        full["stop_class"] is None and
                        full["diagnostic_category"] is None and
                        full["runner_dispatch_count"] == 128 and
                        full["runtime"]["d"] == ["0x0000007f", "0x00000000", "0x00000000", "0x00000000",
                                                 "0x00000000", "0x0000007f", "0x00000000", "0x00000000"] and
                        full["runtime"]["pc"] == "0x00000b00" and
                        ram[32:36] == bytes.fromhex("0000007f") and
                        full["provenance"] is None,
                        "unexpected accepted persistent cycle")
                compared_command = command[:-2] + ["--compare-runs"]
                compared = run(compared_command, root)
                require(compared.stdout, f"missing accepted-cycle compared report: {compared.stderr}")
                compared_report = json.loads(compared.stdout)
                require(compared.returncode == 0 and not compared.stderr and compared_report == {
                    "schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
                        "result": "runner_resource_limit", "stop_class": None,
                        "diagnostic_category": None, "cpu_dimensions": None,
                        "c4_lowering_dimensions": None, "runner_dispatch_count": 128,
                        "reports_match": True}, "unexpected accepted-cycle compared report")
    except (OSError, RuntimeError, json.JSONDecodeError, ValueError) as error:
        sys.stderr.write(f"C6 persistence regression failed: {error}\n")
        return 1
    print("genesis startup bridge C6 persistence: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
