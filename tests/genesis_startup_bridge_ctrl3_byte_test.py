#!/usr/bin/env python3
"""SEG-007-T111: CTRL3 BYTE-read selector device-reachability proof.

Mirrors tests/genesis_startup_bridge_c4_test.py's own end-to-end shape (drive
the project's public compile-and-run wrapper tools/genesis_startup_bridge.py,
read the full runtime report, confirm the runtime's own
genesis_route_access/genesis_controller_io_access return value actually flowed
into the executing binary's semantics), but for the newly admitted SEG-007-T111
selector: a BYTE read of the EXP-port CTRL 3 register's odd/low meaningful-data
byte lane ($A1000D) -- the same register whose WORD-read selector is
SEG-007-T038.

Fixture: BTST #0,($00A1000D).L (immediate/static bit number, BYTE operand,
direct absolute-long addressing -- already stage-supported, no decode/lift/
emission change), immediately followed by an unsupported RESET frontier with no
intervening branch. BTST reads the selector's policy byte 0x00, so bit 0 is
clear and BTST sets exactly the Z condition-code bit (sr == 0x0004), leaving
every data register untouched (BTST never writes back).
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile


IMAGE = bytes((0x08, 0x39, 0x00, 0x00, 0x00, 0xA1, 0x00, 0x0D,  # 0x000B00: BTST #0,($00A1000D).L
               0x4E, 0x70))                                       # 0x000B08: RESET -- unsupported CPU form frontier
ENTRY = "00000b00"


def run_case(name: str, driver: pathlib.Path, binary: pathlib.Path, compiler: pathlib.Path,
             rom: pathlib.Path, root: pathlib.Path, full_report_path: pathlib.Path,
             records: list[dict]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-t111-") as temporary:
        output_dir = pathlib.Path(temporary)
        result = subprocess.run([
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
            "--out-dir", str(output_dir), "--full-report-path", str(full_report_path)],
            text=True, capture_output=True, cwd=root)
        source_path = output_dir / "bridge.generated.c"
        source_bytes = source_path.read_bytes() if source_path.exists() else None
        full_bytes = full_report_path.read_bytes() if full_report_path.exists() else None
        records.append({"name": name, "returncode": result.returncode, "stdout": result.stdout,
                        "stderr": result.stderr, "source_bytes": source_bytes, "full_bytes": full_bytes,
                        "output_dir": str(output_dir)})
        return result


def fail(records: list[dict]) -> int:
    for record in records:
        diagnostic = dict(record)
        for key in ("source_bytes", "full_bytes"):
            if isinstance(diagnostic.get(key), bytes):
                diagnostic[key] = diagnostic[key].decode("utf-8", errors="replace")
        sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    records: list[dict] = []
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "t111-ctrl3-byte-minimal.bin"
        rom.write_bytes(IMAGE)
        first_full = pathlib.Path(temporary) / "first-full.json"
        second_full = pathlib.Path(temporary) / "second-full.json"
        first = run_case("first", driver, binary, compiler, rom, root, first_full, records)
        second = run_case("second", driver, binary, compiler, rom, root, second_full, records)

    if (first.returncode != 0 or second.returncode != 0 or first.stdout != second.stdout or
            records[0]["source_bytes"] is None or records[0]["source_bytes"] != records[1]["source_bytes"] or
            records[0]["full_bytes"] is None or records[0]["full_bytes"] != records[1]["full_bytes"] or
            records[0]["output_dir"] == records[1]["output_dir"]):
        return fail(records)

    try:
        report = json.loads(first.stdout)
    except json.JSONDecodeError:
        return fail(records)

    expected_sanitized = {
        "schema_version": 1, "report_kind": "sanitized", "result": "stop",
        "stop_class": "unsupported_cpu_form",
        "diagnostic_category": "valid_but_unsupported_instruction",
        "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
        "c4_lowering_dimensions": None,
    }
    if any(report.get(key) != value for key, value in expected_sanitized.items()) or len(report.get("rom_sha256", "")) != 64:
        return fail(records)

    try:
        full_report = json.loads(records[0]["full_bytes"])
    except (TypeError, json.JSONDecodeError):
        return fail(records)
    runtime = full_report.get("runtime")
    if not isinstance(runtime, dict) or full_report.get("c4_lowering_dimensions") is not None:
        return fail(records)

    # Reachability proof: the BTST read through genesis_route_access ->
    # genesis_controller_io_access returns the SEG-007-T111 CTRL3 BYTE policy
    # value 0 for this exact selector. BTST of a clear bit sets exactly Z and
    # leaves N/V/C/X and every register unaffected, from a fully
    # zero-initialized runtime: sr == "0x0004", D0 unchanged. Dead code or a
    # device-access stop would instead leave sr == "0x0000" or never reach the
    # RESET frontier.
    if runtime.get("sr") != "0x2704" or runtime.get("d", [None])[0] != "0x00000000":
        return fail(records)

    # The generated source calls the real runtime routing function for this
    # BYTE read and never folds it as a compile-time literal, and no runtime
    # target-opcode decoder is emitted.
    source_text = records[0]["source_bytes"].decode("utf-8")
    routed = re.search(
        r"const uint32_t (m68k_routed_addr_\d+) = \(UINT32_C\(0x00A1000D\)\) & UINT32_C\(0x00FFFFFF\);",
        source_text)
    routed_ok = routed is not None and (
        f"genesis_route_access(runtime, {routed.group(1)}, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ"
        in source_text)
    if (not routed_ok or "resolved static read" in source_text or
            "m68k_decode_instruction" in source_text):
        return fail(records)

    print("genesis startup bridge CTRL3 BYTE (SEG-007-T111): ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
