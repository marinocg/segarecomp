#!/usr/bin/env python3
"""SEG-007-T252 / ADR-0040 correction: the dedicated, FD-only ephemeral
recent-PC-history transport.

This test proves the two-part invariant the correction establishes:
1. `run_bridge(..., ephemeral_pipe=True)` against a real compiled synthetic
   fixture that reaches `runner_resource_limit` returns bytes that parse into
   the expected recorded PC history, and `ephemeral_frontier(...)` folds that
   parsed history into its own bounded diagnostic shape.
2. A distinctive, project-authored-only synthetic marker PC value used ONLY
   by this test's own self-looping fixture never appears in either the
   stable full report (any transport) or the sanitized report -- it is only
   ever observable through the dedicated ephemeral channel.

Only project-authored synthetic PC values are used; no commercial value
appears anywhere in this file.
"""
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "genesis_startup_bridge", ROOT / "tools" / "genesis_startup_bridge.py")
assert SPEC and SPEC.loader
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)

# A distinctive, clearly-synthetic entry address used ONLY by this test's own
# fixture, chosen so it can never collide with any other test's or fixture's
# PC value. MARKER_PC is the SECOND block's address: with --instruction-budget
# 2, it is the one address that is BOTH recorded in the PC history AND
# distinct from the stable full report's own legitimate final `pc` register
# value (which lands on the not-yet-dispatched THIRD block instead -- see
# genesis_runtime_run's own `runtime->pc = result.next_pc` accounting). This
# lets the marker unambiguously separate "present only via the ephemeral
# channel" from "a normal, always-present runtime.pc field".
ENTRY = "00012340"
MARKER_PC = "00012344"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    # A chain of four unconditional BRA.W blocks (block0 -> block1 -> block2 ->
    # block3 -> self-loop), the same accepted no-completion-loop shape family
    # tests/genesis_startup_bridge_c6_accepted_loop_test.py already uses,
    # mapped starting at ENTRY. With --instruction-budget 2, exactly block0
    # and block1 (MARKER_PC) are dispatched/recorded before the runner-
    # resource-limit stop.
    image = bytes((
        0x60, 0x00, 0x00, 0x02,  # block0 @ ENTRY:      BRA.W +2 -> block1
        0x60, 0x00, 0x00, 0x02,  # block1 @ MARKER_PC:  BRA.W +2 -> block2
        0x60, 0x00, 0x00, 0x02,  # block2:              BRA.W +2 -> block3
        0x60, 0x00, 0xFF, 0xFE,  # block3:              BRA.W -2 -> self-loop
    ))
    digest = hashlib.sha256(image).hexdigest()
    try:
        with tempfile.TemporaryDirectory() as temporary:
            work = pathlib.Path(temporary)
            rom = work / "marker.bin"
            rom.write_bytes(image)

            expected_history = [
                f"0x{int(ENTRY, 16):08x}",
                f"0x{int(MARKER_PC, 16):08x}",
            ]

            # (1) End-to-end CLI invocation with --diagnose-frontier: the
            # ephemeral channel must surface MARKER_PC, and the sanitized
            # stdout report must never carry it.
            with tempfile.TemporaryDirectory(dir=root / "build", prefix="ephemeral-transport-") as output:
                output_dir = pathlib.Path(output)
                result = subprocess.run(
                    [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                     "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
                     "--out-dir", str(output_dir), "--expect-sha256", digest,
                     "--diagnose-frontier", "--instruction-budget", "2"],
                    text=True, capture_output=True, cwd=root, check=False)
                require(result.returncode == 0, f"driver failed ({result.returncode}): {result.stderr}")
                sanitized = json.loads(result.stdout)
                require(sanitized.get("result") == "runner_resource_limit",
                        f"expected runner_resource_limit, got {sanitized}")
                require(MARKER_PC not in result.stdout,
                        "MARKER_PC must never appear in the sanitized stdout report")
                lines = [line for line in result.stderr.splitlines() if line.startswith("EPHEMERAL_FRONTIER ")]
                require(len(lines) == 1, "exactly one EPHEMERAL_FRONTIER diagnostic line is expected")
                ephemeral = json.loads(lines[0][len("EPHEMERAL_FRONTIER "):])
                require(ephemeral.get("report_kind") == "ephemeral_frontier",
                        f"unexpected ephemeral frontier shape: {ephemeral}")
                require(ephemeral.get("recent_pc_history") == expected_history,
                        f"unexpected recorded PC history: {ephemeral}")

            # (2) Direct run_bridge(ephemeral_pipe=True) call against a freshly
            # generated+compiled executable, proving the parsed bytes/frontier
            # assembly work exactly as documented for programmatic callers.
            with tempfile.TemporaryDirectory(dir=root / "build", prefix="ephemeral-transport-direct-") as output:
                output_dir = pathlib.Path(output)
                emitter_command = [str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                                   "--entry", ENTRY, "--mapping-base", ENTRY,
                                   "--rom-sha256", digest]
                status, generated, executable = bridge.generate_and_compile(
                    emitter_command, compiler, root, output_dir, True)
                require(status == 0 and executable is not None,
                        f"generate_and_compile failed: status={status}")
                run_status, report, full_bytes, message, ephemeral_bytes = bridge.run_bridge(
                    executable, root, full_pipe=True, instruction_budget=2, ephemeral_pipe=True)
                require(run_status == 0, f"run_bridge failed: {message}")
                require(report.get("result") == "runner_resource_limit",
                        f"expected runner_resource_limit, got {report}")
                # (4) No durable leakage: MARKER_PC must be absent from both
                # the full-report bytes and the sanitized report, present
                # only in the ephemeral capture. (ENTRY itself is not used
                # here: it is the fixture's own start address and would
                # trivially collide with ordinary decoded-source bytes; the
                # distinguishing property under test is MARKER_PC, the
                # SECOND recorded PC, which is neither the entry nor the
                # final `pc` register value the stable full report legitimately
                # carries -- see the module docstring.)
                require(full_bytes is not None and MARKER_PC.encode() not in full_bytes,
                        "MARKER_PC must never appear in the stable full-report bytes")
                require(MARKER_PC not in json.dumps(report),
                        "MARKER_PC must never appear in the sanitized report")
                parsed_history = bridge.parse_ephemeral_pc_history(ephemeral_bytes)
                require(parsed_history == expected_history,
                        f"unexpected parsed ephemeral history: {parsed_history}")
                full = bridge.parse_canonical_full(full_bytes)
                require(full is not None and bridge.valid_full(full, report, digest),
                        "the stable full report must validate with zero knowledge of PC history")
                require(full.get("runtime", {}).get("pc") != f"0x{int(MARKER_PC, 16):08x}",
                        "setup: the final pc register must genuinely differ from MARKER_PC")
                frontier = bridge.ephemeral_frontier(full, executable, parsed_history)
                require(
                    frontier == {
                        "report_kind": "ephemeral_frontier",
                        "debug_binary": str(executable),
                        "recent_pc_history": parsed_history,
                    },
                    f"unexpected ephemeral_frontier shape: {frontier}")
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        sys.stderr.write(f"ephemeral PC history transport test failed: {error}\n")
        return 1
    print("genesis ephemeral PC history transport: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
