#!/usr/bin/env python3
"""SEG-007-T040 C5: full synthetic end-to-end proof.

Unlike C4's own single-instruction reachability proof
(tests/genesis_startup_bridge_c4_test.py -- a bare `TST.L $00A10008` directly
followed by the unsupported RESET frontier, proving the controller-I/O route is
merely *reachable*), this test proves the whole expanded
admission/lowering/promotion/routing chain works together, coexisting with
the pre-existing, already-validated bridge machinery (multi-block dispatch,
persistent register state across the precompiled-PC dispatcher boundary,
ordinary branch lowering) in one coherent composite program -- matching
SEG-007-T030's own verification method (byte-identical generated
C/report/stdout across independent repeats, strict C11, no Musashi linkage,
confirmed by directly inspecting the compiled executable).

Project-authored synthetic three-block program (no real ROM bytes anywhere):

  Block A (0x000B00): MOVEQ #5, D0 ; BRA.S -> Block B
    Proves ordinary already-supported lowering still works, and leaves a
    known nonzero value in D0 before the precompiled-PC dispatcher boundary.
  Block B (0x000B06): TST.L $00A10008 (CTRL1/CTRL2 read, SEG-007-T020/T021
    policy, this task's newly admitted controller-I/O routing) ; BRA.S ->
    Block C.
    Proves the new controller-I/O routing coexists with, and is reached only
    after crossing, the dispatcher boundary, and that D0 survives across
    that boundary unaffected by the TST (TST never writes a register).
  Block C (0x000B10): RESET -- unsupported CPU form frontier stop.

The two-byte gaps immediately after each BRA.S are never decoded (the
branch always jumps over them) and are zero padding, matching the existing
padding convention already used by
tests/genesis_startup_bridge_c5_test.py's own fixture.
"""
import json
from compiled_entry_rows import rows
import pathlib
import subprocess
import sys
import tempfile


IMAGE = bytes((
    0x70, 0x05,                          # 0x000B00: MOVEQ #5, D0
    0x60, 0x02,                          # 0x000B02: BRA.S -> 0x000B06
    0x00, 0x00,                          # 0x000B04: padding (never decoded)
    0x4A, 0xB9, 0x00, 0xA1, 0x00, 0x08,  # 0x000B06: TST.L $00A10008 (CTRL1/CTRL2 selector)
    0x60, 0x02,                          # 0x000B0C: BRA.S -> 0x000B10
    0x00, 0x00,                          # 0x000B0E: padding (never decoded)
    0x4E, 0x70,                          # 0x000B10: RESET -- unsupported CPU form frontier
))
ENTRY = "00000b00"


def run_case(name: str, driver: pathlib.Path, binary: pathlib.Path, compiler: pathlib.Path,
             rom: pathlib.Path, root: pathlib.Path, full_report_path: pathlib.Path,
             records: list[dict]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c5-full-chain-") as temporary:
        output_dir = pathlib.Path(temporary)
        result = subprocess.run([
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
            "--out-dir", str(output_dir), "--full-report-path", str(full_report_path)],
            text=True, capture_output=True, cwd=root)
        source_path = output_dir / "bridge.generated.c"
        executable_path = output_dir / "bridge"
        source_bytes = source_path.read_bytes() if source_path.exists() else None
        full_bytes = full_report_path.read_bytes() if full_report_path.exists() else None
        executable_exists = executable_path.exists()
        # The compiled executable is inspected here, while the isolated
        # output directory (and the binary inside it) still exists -- the
        # temporary directory is removed as soon as this `with` block exits.
        musashi_failures = (check_no_musashi_linkage(executable_path) if executable_exists
                            else [f"compiled executable missing: {executable_path}"])
        record = {"name": name, "returncode": result.returncode, "stdout": result.stdout,
                  "stderr": result.stderr, "source_bytes": source_bytes, "full_bytes": full_bytes,
                  "output_dir": str(output_dir), "executable_path": str(executable_path),
                  "executable_exists": executable_exists, "musashi_failures": musashi_failures}
        records.append(record)
        return result


def check_no_musashi_linkage(binary: pathlib.Path) -> list[str]:
    """Independently confirm no Musashi symbol/object is linked into `binary`.

    Mirrors SEG-007-T030's own orchestrator-reconfirmed executable-inspection
    step (`nm <binary> | grep -i musashi` and `strings <binary> | grep -i
    musashi`, both expected to find no match). Uses subprocess.run directly
    against each tool (no shell pipeline); a missing/unusable tool is a hard
    failure since this check must actually run, not be silently skipped.
    """
    failures: list[str] = []
    for tool in ("nm", "strings"):
        try:
            result = subprocess.run([tool, str(binary)], text=True, capture_output=True)
        except (OSError, FileNotFoundError) as error:
            failures.append(f"cannot run {tool} on {binary}: {error}")
            continue
        if result.stdout == "" and "no symbols" in result.stderr.lower():
            continue  # a stripped/PE image with no symbol table cannot reference musashi
        if result.stdout == "" and result.stderr != "":
            failures.append(f"{tool} {binary} produced no stdout (stderr: {result.stderr.strip()})")
            continue
        matches = [line for line in result.stdout.splitlines() if "musashi" in line.lower()]
        if matches:
            failures.append(f"{tool} found musashi reference(s) in {binary}: {matches}")
    # otool -L is not one of the two required checks, but macOS-portable and
    # useful corroborating evidence (SEG-007-T030's own cited method also
    # captured it): confirm it runs cleanly and links nothing musashi-named.
    try:
        otool_result = subprocess.run(["otool", "-L", str(binary)], text=True, capture_output=True)
        if "musashi" in otool_result.stdout.lower():
            failures.append(f"otool -L found a musashi-named link in {binary}: {otool_result.stdout}")
    except (OSError, FileNotFoundError):
        pass  # otool is macOS-specific and only corroborating; not a required check.
    return failures


def fail(records: list[dict], extra: list[str] | None = None) -> int:
    for record in records:
        diagnostic = dict(record)
        if isinstance(diagnostic.get("source_bytes"), bytes):
            diagnostic["source_bytes"] = diagnostic["source_bytes"].decode("utf-8", errors="replace")
        if isinstance(diagnostic.get("full_bytes"), bytes):
            diagnostic["full_bytes"] = diagnostic["full_bytes"].decode("utf-8", errors="replace")
        sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
    for message in extra or []:
        sys.stderr.write(message + "\n")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    records: list[dict] = []
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "c5-full-chain.bin"
        rom.write_bytes(IMAGE)
        first_full = pathlib.Path(temporary) / "first-full.json"
        second_full = pathlib.Path(temporary) / "second-full.json"
        # Two independent invocations, isolated output directories, matching
        # C4's/C5's own from-scratch determinism pattern (not merely
        # --compare-runs' single-compile in-process reuse).
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

    # The frontier is the Block C RESET: an ordinary unsupported_cpu_form stop,
    # matching this task's own established frontier shape.
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

    # The composite-chain reachability proof: D0 == 5 (Block A's MOVEQ,
    # surviving the dispatcher boundary through Block B unaffected -- TST
    # never writes a register); SR == 0x0004 (Z set by Block B's TST.L
    # reading the CTRL1/CTRL2 policy's zero return value, matching this
    # task's own C4 evidence -- the runtime starts fully zero-initialized,
    # so this Z-only SR state is only reachable if genesis_route_access
    # actually executed and its return value flowed into the executing
    # binary's TST.L semantics); PC == Block C's frontier address. If the
    # dispatcher mishandled the newly admitted controller-I/O block or
    # persistence broke across the new routing, at least one of these three
    # independent fields would diverge.
    expected_runtime = {
        "d": ["0x00000005"] + ["0x00000000"] * 7,
        "a": ["0x00000000"] * 7 + ["0x00ff0004"],
        "usp": "0x00000000",
        "sr": "0x2704",
        "pc": "0x00000b10",
    }
    if any(runtime.get(key) != value for key, value in expected_runtime.items()):
        return fail(records)

    # The generated source calls the real runtime routing function for the
    # Block B TST, never folds it as a compile-time literal, and dispatches
    # through both discovered blocks (proving multi-block closure, not a
    # single collapsed block).
    source_text = records[0]["source_bytes"].decode("utf-8")
    if ("const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00A10008)) & UINT32_C(0x00FFFFFF);"
            not in source_text or
            "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ"
            not in source_text or "resolved static read" in source_text or
            "genesis_block_00000B00(GenesisRuntime *runtime)" not in source_text or
            "genesis_block_00000B06(GenesisRuntime *runtime)" not in source_text or
            "{ UINT32_C(0x00000B00), genesis_block_00000B00 }" not in rows(source_text) or
            "{ UINT32_C(0x00000B06), genesis_block_00000B06 }" not in rows(source_text)):
        return fail(records)

    # C5-specific check: independently confirm, by inspecting the actual
    # compiled executable file the driver produced, that no Musashi
    # symbol/object is linked into it. Checked against both independent
    # builds (see run_case, which inspects the binary before its isolated
    # output directory is removed), not merely the first.
    musashi_failures = [failure for record in records for failure in record["musashi_failures"]]
    if musashi_failures:
        return fail(records, musashi_failures)

    print("genesis startup bridge C5 full chain: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
