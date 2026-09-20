#!/usr/bin/env python3
"""ADR 0013 Decision §7 Phase A: discovery-prefix-boundary end-to-end proof.

Project-authored synthetic fixture only (no commercial input): a chain of
`m68k_discovery_max_instructions + 1` decodable MOVEQ instructions. Discovery
admits the first `m68k_discovery_max_instructions` and trips the ceiling
exactly at the one-past-budget MOVEQ, whose boundary provenance probe
(ADR 0013 Decision §2) succeeds (it is a plain, decodable, mapped MOVEQ under
`M68kDecodeProfile::general_startup`), so the ceiling is classified as
`discovery_prefix_boundary` (Decision §3's empty-`unresolved_reason` /
`has_target == false` discriminator) and promoted to a runnable partial
program (Decision §6's C11/runtime wiring).

This drives the exact production route (`tools/genesis_startup_bridge.py`,
`--mode synthetic`) end to end -- generate, strict-C11 compile
(`-std=c11 -Wall -Wextra -Werror -pedantic`), link, and execute the generated
native binary -- and checks:

  - two independent driver invocations (separate output directories) produce
    byte-identical generated C (Decision §8's determinism invariant);
  - the strict-C11 compile succeeds;
  - the executed generated-native program actually stops with
    `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY` /
    `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` (Decision §6's runtime wiring,
    proven reachable, not merely present as unreachable source text).
"""
import json
import pathlib
import subprocess
import sys
import tempfile


# m68k_discovery_max_instructions (platforms/genesis/machine/include/segarecomp/machine/genesis/frontend.hpp) == 256U.
# One more decodable, mapped MOVEQ than the ceiling: MOVEQ #i,D0 (0x70, i), two
# bytes each, so the (256*2)-byte-offset instruction is the one-past-budget
# boundary probe target.
M68K_DISCOVERY_MAX_INSTRUCTIONS = 256
IMAGE = bytes(byte for i in range(M68K_DISCOVERY_MAX_INSTRUCTIONS + 1) for byte in (0x70, i & 0xFF))
ENTRY = "00000600"
BOUNDARY_SOURCE_ADDRESS = 0x600 + M68K_DISCOVERY_MAX_INSTRUCTIONS * 2


def run_case(name: str, driver: pathlib.Path, binary: pathlib.Path, compiler: pathlib.Path,
             rom: pathlib.Path, root: pathlib.Path, full_report_path: pathlib.Path,
             records: list[dict]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-boundary-") as temporary:
        output_dir = pathlib.Path(temporary)
        result = subprocess.run([
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
            "--out-dir", str(output_dir), "--full-report-path", str(full_report_path)],
            text=True, capture_output=True, cwd=root)
        source_path = output_dir / "bridge.generated.c"
        source_bytes = source_path.read_bytes() if source_path.exists() else None
        full_bytes = full_report_path.read_bytes() if full_report_path.exists() else None
        record = {"name": name, "returncode": result.returncode, "stdout": result.stdout,
                  "stderr": result.stderr, "source_bytes": source_bytes, "full_bytes": full_bytes,
                  "output_dir": str(output_dir)}
        records.append(record)
        return result


def fail(records: list[dict]) -> int:
    for record in records:
        diagnostic = dict(record)
        if isinstance(diagnostic.get("source_bytes"), bytes):
            diagnostic["source_bytes"] = diagnostic["source_bytes"].decode("utf-8", errors="replace")
        if isinstance(diagnostic.get("full_bytes"), bytes):
            diagnostic["full_bytes"] = diagnostic["full_bytes"].decode("utf-8", errors="replace")
        sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    records: list[dict] = []
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "discovery-prefix-boundary.bin"
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

    # ADR 0013 Decision §6: the boundary's own real diagnostic category lowers
    # through the already-resolvable GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED.
    expected_sanitized = {
        "schema_version": 1, "report_kind": "sanitized", "result": "stop",
        "stop_class": "discovery_prefix_boundary",
        "diagnostic_category": "discovery_budget_exhausted",
        "cpu_dimensions": None,
        "c4_lowering_dimensions": None,
    }
    if any(report.get(key) != value for key, value in expected_sanitized.items()) or len(report.get("rom_sha256", "")) != 64:
        return fail(records)

    try:
        full_report = json.loads(records[0]["full_bytes"])
    except (TypeError, json.JSONDecodeError):
        return fail(records)
    if not isinstance(full_report, dict) or full_report.get("c4_lowering_dimensions") is not None:
        return fail(records)
    provenance = full_report.get("provenance")
    instruction = provenance.get("instruction") if isinstance(provenance, dict) else None
    if (not isinstance(provenance, dict) or not provenance.get("has_instruction_provenance") or
            not isinstance(instruction, dict) or
            instruction.get("source_address") != f"0x{BOUNDARY_SOURCE_ADDRESS:08x}"):
        return fail(records)
    # Decision §2/§3: the boundary carries real provenance but no access
    # (needs_access == false) and no target.
    if provenance.get("has_access", False):
        return fail(records)

    # Decision §6's genesis_dispatch-emits-a-frontier-stop-function wiring:
    # the emitted source actually names the boundary's own stop function.
    source_text = records[0]["source_bytes"].decode("utf-8")
    if f"genesis_frontier_stop_{BOUNDARY_SOURCE_ADDRESS:08X}" not in source_text:
        return fail(records)
    if "GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY" not in source_text:
        return fail(records)

    return 0


if __name__ == "__main__":
    sys.exit(main())
