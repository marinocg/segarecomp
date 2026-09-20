#!/usr/bin/env python3
"""Route B keeps a cartridge mapping independent from its reset entry."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(command: list[str], root: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, cwd=root, check=False)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    # Reset SSP, reset PC=8, then an unsupported RESET frontier.  The full
    # project-authored image is mapped at zero while execution starts at its
    # independently selected reset-vector entry.
    image = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08,
                   0x70, 0x00, 0x4E, 0x70))
    digest = hashlib.sha256(image).hexdigest()
    try:
        with tempfile.TemporaryDirectory() as temporary:
            rom = pathlib.Path(temporary) / "reset-mapped.bin"
            rom.write_bytes(image)
            canonical = run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                             "--reset-entry", "--rom-sha256", digest], root)
            explicit = run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                            "--entry", "00000008", "--mapping-base", "00000000",
                            "--rom-sha256", digest], root)
            require(canonical.returncode == 0 and not canonical.stderr and
                    explicit.returncode == 0 and not explicit.stderr,
                    "canonical or explicit full mapping preparation rejected")
            require(canonical.stdout == explicit.stdout and
                    "UINT32_C(0x00000000), UINT32_C(0x0000000C)" in canonical.stdout,
                    "full mapping and reset entry were not retained independently")

            with tempfile.TemporaryDirectory(dir=root / "build", prefix="bridge-reset-map-") as output:
                driven = run([sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                              "--rom", str(rom), "--mode", "synthetic", "--out-dir", output,
                              "--expect-sha256", digest, "--compare-runs"], root)
            report = json.loads(driven.stdout)
            require(driven.returncode == 0 and not driven.stderr and report == {
                "schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
                "result": "stop", "stop_class": "unsupported_cpu_form",
                "diagnostic_category": "valid_but_unsupported_instruction",
                "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
                "c4_lowering_dimensions": None,
                 "reports_match": True}, "canonical driver preparation did not use the reset entry")

            # Commercial Route B is intentionally not an entry-addressed
            # slice route.  Its only preparation is the full ROM plus reset
            # ingress; manual entry or mapping selection must be rejected
            # before generation.
            commercial_entry = run([sys.executable, str(driver), "--segarecomp", str(binary),
                                    "--cc", str(compiler), "--rom", str(rom), "--mode", "commercial",
                                    "--entry", "00000008"], root)
            commercial_mapping = run([sys.executable, str(driver), "--segarecomp", str(binary),
                                      "--cc", str(compiler), "--rom", str(rom), "--mode", "commercial",
                                      "--entry", "00000008", "--mapping-base", "00000000"], root)
            # This deliberately nonexistent ROM and emitter prove the mapping-only
            # rejection happens during argument validation, before either input
            # consumption or generation can recreate a manual commercial route.
            commercial_mapping_only = run([
                sys.executable, str(driver), "--segarecomp", str(pathlib.Path(temporary) / "must-not-run"),
                "--cc", str(compiler), "--rom", str(pathlib.Path(temporary) / "must-not-read.bin"),
                "--mode", "commercial", "--mapping-base", "00000000"], root)
            require(commercial_entry.returncode == 8 and commercial_mapping.returncode == 8 and
                    commercial_mapping_only.returncode == 8 and not commercial_entry.stdout and
                    not commercial_mapping.stdout and not commercial_mapping_only.stdout,
                    "commercial route accepted manual entry or mapping preparation")

            unmapped = run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                            "--entry", "0000000C", "--mapping-base", "00000000",
                            "--rom-sha256", digest], root)
            overflow = run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                            "--entry", "00FFFFFE", "--mapping-base", "00FFFFFE",
                            "--rom-sha256", digest], root)
            ambiguous = run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                             "--reset-entry", "--entry", "00000008", "--mapping-base", "00000000",
                             "--rom-sha256", digest], root)
            require(unmapped.returncode == 2 and overflow.returncode == 2 and ambiguous.returncode == 2 and
                    not unmapped.stdout and not overflow.stdout and not ambiguous.stdout,
                    "unsafe or ambiguous bridge preparation did not reject deterministically")
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        sys.stderr.write(f"genesis bridge mapping preparation regression failed: {error}\n")
        return 1
    print("genesis startup bridge mapping preparation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
