#!/usr/bin/env python3
"""SEG-020-T003: generated failure/frontier emission of the bounded typed
execution history over the existing ephemeral-report-fd channel.

Project-authored synthetic BRA.W chain only. Also proves the generation-time
opt-in contract (diagnostics-off C has no history hooks) and that malformed
ephemeral data is non-fatal for the parser.
"""
import hashlib
import importlib.util
import pathlib
import sys
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("genesis_startup_bridge", ROOT / "tools" / "genesis_startup_bridge.py")
assert SPEC and SPEC.loader
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)
ENTRY = "00012340"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    binary, compiler, root = (pathlib.Path(v).resolve() for v in sys.argv[1:4])
    image = bytes((0x70, 0x01, 0x60, 0x00, 0x00, 0x02, 0x60, 0x00, 0x00, 0x02, 0x60, 0x00, 0xFF, 0xFE))
    digest = hashlib.sha256(image).hexdigest()
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        rom = work / "fixture.bin"
        rom.write_bytes(image)
        base = [str(binary), "emit-general-startup-bridge-c", "--rom", str(rom), "--entry", ENTRY,
                "--mapping-base", ENTRY, "--rom-sha256", digest, "--immutable-rom-aot"]
        # Diagnostics OFF: no history hooks in the emitted C (generation-time opt-in).
        off = subprocess.run(base, text=True, capture_output=True, check=True).stdout
        require("execution_history" not in off and "_retire_m68k_instruction_at" not in off,
                "diagnostics-off C must contain no history hooks")
        require("genesis_route_access_bus" not in off, "diagnostics-off C must not use bus-kind routing")
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="exec-history-") as output:
            out_dir = pathlib.Path(output)
            status, _, executable = bridge.generate_and_compile(base + ["--provenance-diagnostics"], compiler, root,
                                                                out_dir, True)
            require(status == 0 and executable is not None, f"generate_and_compile failed: {status}")
            run_status, report, full_bytes, message, ephemeral = bridge.run_bridge(
                executable, root, full_pipe=True, instruction_budget=3, ephemeral_pipe=True)
            require(run_status == 0, f"run_bridge failed: {message}")
            require(report.get("result") == "runner_resource_limit", f"unexpected result {report}")
            require(bridge.parse_ephemeral_pc_history(ephemeral) is not None, "PC line still parses")
            history = bridge.parse_ephemeral_execution_history(ephemeral)
            require(history is not None, "typed history line missing")
            # This fixture runs through static blocks (no per-instruction retirement); the
            # emitted immutable-AOT bodies carry the hooks (proved below and by the runtime
            # unit test), and the typed line itself is emitted at the frontier.
            require([e["pc"] for e in history if e["k"] == "dispatch"] ==
                    ["0x00012340", "0x00012346", "0x0001234a"], f"dispatch events {history}")
            generated = (out_dir / "bridge.generated.c").read_text()
            require("runtime.execution_history.detail_enabled = 1;" in generated, "history enabled by generated main")
            require("genesis_runtime_retire_m68k_instruction_at(runtime, UINT32_C(0x00012342), UINT32_C(0x00012346), "
                    "GENESIS_HISTORY_TRANSFER_DIRECT, " in generated, "AOT BRA retire carries static PC/transfer kind")
            require("genesis_runtime_retire_m68k_instruction_at(runtime, UINT32_C(0x00012344), UINT32_C(0x00012348), "
                    "GENESIS_HISTORY_TRANSFER_NONE, " in generated, "non-transfer AOT retire carries static PC")
            frontier = bridge.with_execution_history({"report_kind": "ephemeral_frontier"}, ephemeral)
            require(frontier.get("execution_history") == history, "frontier carries the history")
            # Diagnostics-off binary emits only the PC line.
            status, _, plain = bridge.generate_and_compile(base, compiler, root, out_dir / "off" if False else
                                                           pathlib.Path(tempfile.mkdtemp(dir=root / "build", prefix="exec-history-off-")),
                                                           True)
            require(status == 0 and plain is not None, "plain build failed")
            _, _, _, _, plain_eph = bridge.run_bridge(plain, root, full_pipe=True, instruction_budget=3,
                                                       ephemeral_pipe=True)
            require(bridge.parse_ephemeral_execution_history(plain_eph) is None, "off binary must emit no typed history")
        # Malformed ephemeral data is non-fatal (returns None, never raises).
        for bad in (b"", b"\xff\xfe\n\n", b"[]\nnot json\n", b"[]\n[1,2]\n", b"[]\n[{\"b\":0}]", b"[]\n[]\n[]\n"):
            require(bridge.parse_ephemeral_execution_history(bad) is None, f"malformed accepted: {bad!r}")
    print("genesis execution history generated emission: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
