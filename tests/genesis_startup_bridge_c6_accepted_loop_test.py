#!/usr/bin/env python3
"""Accepted no-completion BRA.S loops use the existing bounded bridge ABI."""
import base64
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


ENTRY = "00000b00"
# T084 scopes every emitted C4 operation to make per-operation routed-EA
# temporaries valid when the same EA form repeats in one block. This is an
# intentional deterministic artifact update; the generated C6 partial still
# follows the same dispatcher/runtime ABI.


def run(command: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, cwd=cwd, check=False)


def emit(binary: pathlib.Path, rom: pathlib.Path, image: bytes, extra: list[str] | None = None) -> bytes:
    digest = hashlib.sha256(image).hexdigest()
    result = subprocess.run(
        [str(binary), "emit-general-startup-bridge-c", "--rom", str(rom), "--entry", ENTRY,
         "--mapping-base", ENTRY,
         "--rom-sha256", digest, *(extra or [])], capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    # These fixtures are the established C5 completion and partial inputs.
    completion = bytes((0x70, 0x05, 0x4E, 0x75))
    partial = bytes((0x42, 0xB9, 0x00, 0xFF, 0x00, 0x00, 0x60, 0x02,
                     0x00, 0x00, 0x4E, 0x70))
    loops = {
        "self": (bytes((0x60, 0xFE)), ("00000b00",), 1),
        "two-block": (bytes((0x60, 0x02, 0x00, 0x00, 0x60, 0xFA)),
                      ("00000b00", "00000b04"), 2),
        # BNE.S +2 has a taken block at 0xB04 and, with initial SR == 0,
        # follows its fallthrough block at 0xB02. Both return to 0xB00.
        "conditional": (bytes((0x66, 0x02, 0x60, 0xFC, 0x60, 0xFA)),
                        ("00000b00", "00000b02", "00000b04"), 3),
    }
    try:
        with tempfile.TemporaryDirectory() as temporary:
            work = pathlib.Path(temporary)
            completion_rom = work / "completion.bin"
            partial_rom = work / "partial.bin"
            completion_rom.write_bytes(completion)
            partial_rom.write_bytes(partial)
            completion_source = emit(binary, completion_rom, completion,
                                     ["--synthetic-completion-rts", "00000b02",
                                      "--synthetic-completion-sentinel", "00400000"])
            require(completion_source == emit(binary, completion_rom, completion,
                                              ["--synthetic-completion-rts", "00000b02",
                                               "--synthetic-completion-sentinel", "00400000"]),
                    "completion bridge source is non-deterministic")
            partial_source = emit(binary, partial_rom, partial)
            require(partial_source == emit(binary, partial_rom, partial),
                    "partial bridge source is non-deterministic")

            for name, (image, targets, block_count) in loops.items():
                rom = work / f"{name}.bin"
                full = work / f"{name}.full.json"
                rom.write_bytes(image)
                digest = hashlib.sha256(image).hexdigest()
                with tempfile.TemporaryDirectory(dir=root / "build", prefix="c6-accepted-loop-") as output:
                    output_dir = pathlib.Path(output)
                    command = [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                                "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
                                "--out-dir", str(output_dir), "--expect-sha256", digest]
                    compared = run(command + ["--compare-runs"], root)
                    require(compared.returncode == 0 and not compared.stderr,
                            f"{name}: driver failed ({compared.returncode}): {compared.stderr.strip()} {compared.stdout.strip()} "
                            f"{(output_dir / 'bridge.generated.c').read_text() if (output_dir / 'bridge.generated.c').exists() else 'no source'}")
                    report = json.loads(compared.stdout)
                    # SEG-007-T252 / ADR-0040: none of these accepted no-completion
                    # loops emit any CPU/codegen-proven progress note (there is no
                    # such note any more), so the runner's own default 128-step
                    # dispatch allowance is what eventually reports the disjoint
                    # runner_resource_limit outcome -- never the guest
                    # instruction_budget_exhausted stop.
                    require(json.dumps(report, separators=(",", ":")) + "\n" == compared.stdout and
                            report == {"schema_version": 1, "report_kind": "sanitized",
                                        "rom_sha256": hashlib.sha256(image).hexdigest(),
                                        "result": "runner_resource_limit",
                                        "stop_class": None,
                                        "diagnostic_category": None,
                                        "cpu_dimensions": None, "c4_lowering_dimensions": None,
                                        "runner_dispatch_count": 128, "reports_match": True},
                            f"{name}: non-canonical runner-resource-limit report")
                    source = (output_dir / "bridge.generated.c").read_text()
                    require("translation rejected" not in source and "decode" not in source.lower() and
                            "GENESIS_COMPLETE" not in source and "runtime.work_ram[" not in source and
                            # SEG-007-T106 retained the bounded project-engineering limit at 128
                            # after no permitted higher candidate cleared its production budget stop.
                            # It is not a hardware or timing value and must appear exactly once.
                            source.count("UINT32_C(128)") == 1 and
                            source.count("static GenesisControlTransfer genesis_block_") == block_count,
                            f"{name}: not accepted no-completion source")
                    for target in targets:
                        static_target = f"{int(target, 16):08X}"
                        require(f"genesis_block_{static_target}" in source and
                                f"runtime->pc == UINT32_C(0x{static_target})" in source,
                                f"{name}: missing statically emitted target {target}")
                    for target in targets:
                        require(f"runtime->pc = UINT32_C(0x{int(target, 16):08X})" in source,
                                f"{name}: missing shared direct-branch target {target}")

                    full_run = run(command + ["--full-report-path", str(full)], root)
                    full_report = json.loads(full_run.stdout)
                    full_json = json.loads(full.read_text())
                    expected_runtime = {
                        "d": ["0x00000000"] * 8,
                        "a": ["0x00000000"] * 7 + ["0x00ff0004"],
                        "usp": "0x00000000",
                        "sr": "0x0000",
                        "pc": "0x00000b00",
                        "work_ram_base64": base64.b64encode(bytes(65536)).decode("ascii"),
                    }
                    # SEG-007-T252 / ADR-0040 correction: `recent_pc_history` must
                    # NEVER appear in a `--full-report-path` file (this is the
                    # exact filesystem-persistence defect this correction closes):
                    # it is only ever available via the separate, FD-only
                    # `--ephemeral-report-fd` diagnostic transport, never here.
                    require("recent_pc_history" not in full_json,
                            f"{name}: recent_pc_history must never appear in a --full-report-path file")
                    require(full_run.returncode == 0 and not full_run.stderr and
                            full_report.get("cpu_dimensions") is None and
                            full_json == {
                                "schema_version": 1, "report_kind": "full", "rom_sha256": digest,
                                "result": "runner_resource_limit", "runtime": expected_runtime,
                                "stop_class": None,
                                "diagnostic_category": None, "c4_lowering_dimensions": None,
                                "provenance": None, "runner_dispatch_count": 128},
                            f"{name}: unexpected full runner-resource-limit state or provenance")
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        sys.stderr.write(f"C6 accepted loop regression failed: {error}\n")
        return 1
    print("genesis startup bridge C6 accepted loops: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
