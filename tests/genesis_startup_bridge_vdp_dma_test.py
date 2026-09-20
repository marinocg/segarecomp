#!/usr/bin/env python3
"""SEG-007-T084 generated-C reachability for bounded VDP DMA.

The fixture uses only project-authored instruction/data bytes. It drives the
public bridge driver, so the test exercises discovery, C emission, strict-C11
compilation/linking, and execution -- not a handwritten runtime harness.
Nine emitted MOVE.W immediate-to-CONTROL writes configure and trigger the
selected memory-to-VRAM DMA. Two emitted TST.W CONTROL reads are the exact
T084 deterministic progress events; the following RESET frontier proves both
reads returned to generated code and execution continued through them.
"""
import json
import pathlib
import subprocess
import sys
import tempfile


ENTRY = 0x00000B00
def write_word(value: int) -> bytes:
    # MOVE.W #imm,(A0). A0 is initialized by the emitted LEA below, keeping
    # the access runtime-routed rather than claiming static VDP resolution.
    return bytes((0x30, 0xBC, value >> 8, value & 0xFF))


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"

    # The payload follows the RESET frontier. It is immutable generated cartridge
    # data, not discovered code. Its address is selected solely from this
    # project-authored layout and is programmed through VDP source registers.
    writes_prefix = [0x8110, 0x9302, 0x9400, 0x8F02]
    provisional = b"".join(write_word(word) for word in writes_prefix)
    # Add source-register writes after computing the final payload address.
    # Remaining instructions: source regs 21--23, two command words, two
    # status reads, and the RESET frontier.
    payload_address = ENTRY + 6 + len(provisional) + 3 * 4 + 2 * 4 + 2 * 2 + 2
    if payload_address & 1:
        return 1
    source_word_address = payload_address >> 1
    if source_word_address > 0x3FFFFF:
        return 1
    source_writes = [0x9500 | (source_word_address & 0xFF),
                     0x9600 | ((source_word_address >> 8) & 0xFF),
                     0x9700 | ((source_word_address >> 16) & 0x3F)]
    image = (bytes((0x41, 0xF9, 0x00, 0xC0, 0x00, 0x04)) +
             provisional + b"".join(write_word(word) for word in source_writes) +
             write_word(0x6010) + write_word(0x0080) +
             bytes((0x4A, 0x50, 0x4A, 0x50)) +
             bytes((0x4E, 0x70, 0x12, 0x34, 0x56, 0x78)))
    if ENTRY + len(image) != payload_address + 4:
        return 1

    with tempfile.TemporaryDirectory() as temporary:
        temporary_path = pathlib.Path(temporary)
        rom = temporary_path / "vdp-dma.synthetic.bin"
        full = temporary_path / "full.json"
        rom.write_bytes(image)
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-vdp-dma-") as output:
            output_path = pathlib.Path(output)
            result = subprocess.run([
                sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                "--rom", str(rom), "--entry", f"{ENTRY:08x}", "--mode", "synthetic",
                "--out-dir", str(output_path), "--full-report-path", str(full)],
                text=True, capture_output=True, cwd=root)
            source = (output_path / "bridge.generated.c").read_text() if (output_path / "bridge.generated.c").exists() else ""
        # A second, independently generated bridge asks the public driver to
        # execute its compiled child twice and compare canonical full reports
        # in memory. This is the strongest determinism assertion the existing
        # report ABI exposes without adding VDP-private fields to that ABI.
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-vdp-dma-") as output:
            output_path = pathlib.Path(output)
            repeated = subprocess.run([
                sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                "--rom", str(rom), "--entry", f"{ENTRY:08x}", "--mode", "synthetic",
                "--out-dir", str(output_path), "--compare-runs"],
                text=True, capture_output=True, cwd=root)
            repeated_source = ((output_path / "bridge.generated.c").read_text()
                               if (output_path / "bridge.generated.c").exists() else "")

        try:
            report = json.loads(result.stdout)
            full_report = json.loads(full.read_text())
            repeated_report = json.loads(repeated.stdout)
        except (json.JSONDecodeError, OSError):
            sys.stderr.write(result.stderr)
            sys.stderr.write(result.stdout)
            return 1
    expected = {
        "schema_version": 1, "report_kind": "sanitized", "result": "stop",
        "stop_class": "unsupported_cpu_form", "diagnostic_category": "valid_but_unsupported_instruction",
        "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
        "c4_lowering_dimensions": None,
    }
    runtime = full_report.get("runtime") if isinstance(full_report, dict) else None
    route_call = "genesis_route_access(runtime, "
    if (result.returncode != 0 or any(report.get(key) != value for key, value in expected.items()) or
            len(report.get("rom_sha256", "")) != 64 or not isinstance(runtime, dict) or
            full_report.get("c4_lowering_dimensions") is not None or
            runtime.get("pc") != f"0x{ENTRY + len(image) - 6:08x}" or
            source.count(route_call) != 11 or
            source.count(", GENESIS_ACCESS_WRITE, &m68k_routed_value") != 9 or
            source.count(", GENESIS_ACCESS_READ, &m68k_routed_value_") != 2 or
            "genesis_dispatch" not in source or
            repeated.returncode != 0 or repeated_source != source or
            any(repeated_report.get(key) != value for key, value in expected.items()) or
            repeated_report.get("reports_match") is not True):
        sys.stderr.write(result.stderr)
        sys.stderr.write(result.stdout)
        sys.stderr.write(repeated.stderr)
        sys.stderr.write(repeated.stdout)
        sys.stderr.write(f"returncode={result.returncode} pc={runtime.get('pc') if isinstance(runtime, dict) else None} "
                         f"route_calls={source.count(route_call)} image_length={len(image)}\n")
        return 1
    print("genesis startup bridge VDP DMA: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
