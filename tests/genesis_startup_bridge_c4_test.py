#!/usr/bin/env python3
"""SEG-007-T040 C4: device-reachability proof.

Unlike C2/C3's own controller-I/O checks (tests/genesis_startup_runtime_c4_test.py,
compile-only), this test proves the reset-to-controller-I/O path is genuinely
reachable from a compiled-AND-LINKED-AND-EXECUTED generated program: it drives
the project's own public compile-and-run wrapper
(tools/genesis_startup_bridge.py), reads the resulting full runtime report, and
confirms the runtime's own genesis_route_access/genesis_controller_io_access
function's real return value (0) actually flowed into the executing binary's
TST.L semantics -- not merely present as unreachable source text.

The fixture is exactly the byte pattern already proven, at the discovery/C4-
generated-source-shape level, by
general_startup_promotes_a_minimal_single_instruction_block_at_a_controller_io_routed_read
(tests/m68k_pipeline_test.cpp) and its CLI counterpart
(--emit-general-startup-runtime-c4-controller-io-minimal): a CTRL1/CTRL2 (SEG-
007-T020/T021) LONG-read TST.L at $00A10008, immediately followed by an
unsupported RESET frontier with no intervening branch.
"""
import json
import pathlib
import subprocess
import sys
import tempfile


IMAGE = bytes((0x4A, 0xB9, 0x00, 0xA1, 0x00, 0x08,  # 0x000B00: TST.L $00A10008 (CTRL1/CTRL2 selector)
               0x4E, 0x70))                          # 0x000B06: RESET -- unsupported CPU form frontier
ENTRY = "00000b00"


def run_case(name: str, driver: pathlib.Path, binary: pathlib.Path, compiler: pathlib.Path,
             rom: pathlib.Path, root: pathlib.Path, full_report_path: pathlib.Path,
             records: list[dict]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c4-") as temporary:
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
        rom = pathlib.Path(temporary) / "c4-controller-io-minimal.bin"
        rom.write_bytes(IMAGE)
        first_full = pathlib.Path(temporary) / "first-full.json"
        second_full = pathlib.Path(temporary) / "second-full.json"
        # Run the driver twice, end to end (generate, compile, link, execute),
        # from two independent invocations with isolated output directories --
        # proving determinism the same way C5's own from-root/from-build pair
        # does, not merely relying on --compare-runs' single-compile reuse.
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

    # The frontier is the immediately-following RESET: an ordinary
    # unsupported_cpu_form stop, matching this task's own established
    # frontier shape (SEG-007-T029/T030 and this task's own C2/C3 fixtures).
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
    if not isinstance(runtime, dict):
        return fail(records)

    # This is the actual C4 reachability proof. TST.L reads through
    # genesis_route_access -> genesis_controller_io_access, which returns the
    # SEG-007-T020/T021 CTRL1/CTRL2 policy value 0 for this exact selector.
    # TST.L of 0 sets exactly the Z condition-code bit and clears N/V/C, and
    # leaves X (and D0, since TST never writes back) unaffected. The runtime
    # starts fully zero-initialized (SR=0x0000), so the only bit TST can set
    # is Z: sr == "0x0004". This is this codebase's own established
    # Z-set/otherwise-clear encoding (M68kMoveResultCcrSpecification's
    # zero_mask == UINT16_C(0x0004) in src/m68k_pipeline.cpp; independently
    # cross-checked against tests/genesis_startup_bridge_c5_test.py's
    # exact_wrong_return_full expecting "sr": "0x0004" for its own Z-set
    # case, and tests/genesis_startup_bridge_c6_test.py's expected_runtime
    # asserting "sr": "0x0004" after its own Z-setting CLR.B prefix). If this
    # were still unreachable dead code -- the C2/C3 defect this checkpoint
    # closes -- sr would instead be "0x0000" (TST never executed) or the
    # runtime would report a device-access stop instead of completing the
    # TST and reaching the RESET frontier.
    if runtime.get("sr") != "0x0004" or runtime.get("d", [None])[0] != "0x00000000":
        return fail(records)

    # Belt-and-suspenders: the exact code path C2/C3 built is what actually
    # executed -- the generated source calls the real runtime routing
    # function for this instruction, and never folds it as a compile-time
    # literal (the alternate branch that would bypass the runtime entirely).
    source_text = records[0]["source_bytes"].decode("utf-8")
    # SEG-007-T105: the routed EA now passes through the single MC68000 24-bit
    # external-address-bus truncation seam; the route call consumes that
    # bus-address local (for this in-range absolute the mask is a no-op) and it
    # is still genuine runtime routing, never a compile-time fold.
    if ("const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00A10008)) & UINT32_C(0x00FFFFFF);"
            not in source_text or
            "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ"
            not in source_text or "resolved static read" in source_text):
        return fail(records)

    print("genesis startup bridge C4: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
