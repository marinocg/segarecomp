#!/usr/bin/env python3
"""Deterministic project-owned architecture baseline verifier (SEG-014-T001).

This is the regression anchor later SEG-014 structural-move tasks must run
instead of relying only on "tests are still green" after large file moves.
It drives a small, fixed set of already-established, already-fixture-backed
synthetic probes across the production pipeline stages named in the SEG-014-T001
issue scope (analysis-only output, generated C / semantic hashes, bridge
compile/run reports, diagnostics/provenance) and either:

  * `--mode capture` -- runs every probe and writes a deterministic manifest
    (`tests/fixtures/architecture-baseline-manifest.json` by default), or
  * `--mode verify`  -- re-runs every probe and diffs the result against the
    committed manifest, printing a JSON diff of any mismatched probe names to
    stderr and exiting nonzero on any unexpected (hard-gating) mismatch.

Every probe input is either read from an already-committed project fixture
(`tests/fixtures/*.json`) or a small project-authored synthetic byte literal
already established by an existing focused test (see each probe function's
docstring for its exact provenance). No commercial ROM, local path, or
privacy-sensitive content is ever read, printed, or recorded.

Only the report/state *semantics* of each probe hard-gate `verify`. Where a
probe also captures generated C, its SHA-256 is retained purely as an
informational/soft field: the SEG-014-T001 issue's own non-goals say
byte-identical generated text is not required once later pure namespace/
include moves happen, so an informational-only mismatch is reported but never
causes a nonzero exit.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST_PATH = PROJECT_ROOT / "tests" / "fixtures" / "architecture-baseline-manifest.json"

# Exit codes (deterministic, per failure class -- never a bare generic 1
# except for a genuinely unexpected internal exception).
EXIT_OK = 0
EXIT_USAGE = 2
EXIT_PROBE_EXECUTION_FAILED = 3
EXIT_PROBE_OUTPUT_MALFORMED = 4
EXIT_VERIFY_MISMATCH = 5
EXIT_MANIFEST_UNAVAILABLE = 6


class ProbeError(Exception):
    """A deterministic, classified probe failure. `code` is one of the
    EXIT_PROBE_* constants above."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def canonical_json_text(value: Any) -> str:
    """The one shared canonicalization used for both the SHA-256 hard-gate
    input and the committed manifest's own on-disk JSON: sorted keys, no
    incidental whitespace, ASCII-only. Independent of any producer's own
    output key order, so the hash is stable even if an upstream JSON emitter
    reorders fields for an unrelated reason."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_of(value: Any) -> str:
    return hashlib.sha256(canonical_json_text(value).encode("ascii")).hexdigest()


def run_json_cli(command: list[str], *, cwd: pathlib.Path) -> dict:
    try:
        result = subprocess.run(command, text=True, capture_output=True, cwd=cwd, check=False)
    except OSError as error:
        raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"cannot run {command[0]}: {error}") from error
    if result.returncode != 0 or result.stderr != "":
        raise ProbeError(
            EXIT_PROBE_EXECUTION_FAILED,
            f"{' '.join(command)} exited {result.returncode}: {result.stderr}",
        )
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, f"{' '.join(command)} wrote invalid JSON: {error}") from error


def redact_full_bridge_runtime(full_report: dict) -> dict:
    """Replaces the full bridge report's `runtime.work_ram_base64` (a full
    64 KiB, mostly-zero synthetic work-RAM dump, ~87 KiB of base64 text) with
    its own SHA-256 digest and decoded length. The digest still hard-gates
    every byte of that RAM; only the manifest's own committed size stays
    small. Every other observable field (registers, stop/diagnostic
    category, provenance) is retained verbatim."""
    runtime = full_report.get("runtime")
    if not isinstance(runtime, dict) or "work_ram_base64" not in runtime:
        return full_report
    redacted_runtime = dict(runtime)
    encoded = redacted_runtime.pop("work_ram_base64")
    try:
        decoded = base64.b64decode(encoded, validate=True) if isinstance(encoded, str) else b""
    except (ValueError, TypeError):
        decoded = b""
    redacted_runtime["work_ram_sha256"] = hashlib.sha256(decoded).hexdigest()
    redacted_runtime["work_ram_length"] = len(decoded)
    redacted = dict(full_report)
    redacted["runtime"] = redacted_runtime
    return redacted


def probe_analysis_genesis_general_startup(args: argparse.Namespace) -> tuple[dict, dict]:
    """Analysis-only pipeline stage: the `genesis-general-startup` CLI's JSON
    report over the already-committed, already-hash-checked synthetic
    two-call-frame reset image
    (`tests/fixtures/genesis-general-startup-vectors.json`, the exact
    `build_image` construction used by `tests/genesis_general_startup_cli_test.py`).
    """
    fixture_path = PROJECT_ROOT / "tests" / "fixtures" / "genesis-general-startup-vectors.json"
    vector = json.loads(fixture_path.read_text())
    image = bytearray(vector["image_length"])
    for offset, data in vector["writes"].items():
        start = int(offset, 16)
        raw = bytes.fromhex(data)
        image[start:start + len(raw)] = raw
    image[0x120:0x150] = vector["title"].encode("ascii").ljust(48, b" ")
    image = bytes(image)
    if hashlib.sha256(image).hexdigest() != vector["sha256"]:
        raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, "genesis-general-startup fixture image hash mismatch")
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "architecture-baseline-general-startup.bin"
        rom.write_bytes(image)
        report = run_json_cli(
            [str(args.segarecomp), "genesis-general-startup", str(rom)], cwd=PROJECT_ROOT)
    expected = vector["expected"]
    if (report.get("result") != expected["general_result"] or report.get("profile") != "general_startup" or
            any(report.get(field) != expected[field] for field in ("decoded", "blocks", "edges", "frames"))):
        raise ProbeError(
            EXIT_PROBE_OUTPUT_MALFORMED,
            "genesis-general-startup report diverged from its own established fixture expectation",
        )
    fields = {"fixture_id": vector["fixture_id"], "report": report}
    return fields, {}


def probe_legacy_moveq_slice(args: argparse.Namespace) -> tuple[dict, dict]:
    """Retained legacy pipeline surface: `emit-moveq-c` generates C for the
    already-committed, already-hash-checked, project-authored `zero-d0`
    MOVEQ fixture (`tests/fixtures/moveq-fixtures.json`, the same fixture
    `tests/moveq_static_slice_test.py` validates), then compiles and runs
    that generated C to observe the one-instruction final state."""
    manifest = json.loads((PROJECT_ROOT / "tests" / "fixtures" / "moveq-fixtures.json").read_text())
    entry = next(fixture for fixture in manifest["fixtures"] if fixture["id"] == "zero-d0")
    image = bytes.fromhex(entry["image_hex"])
    if hashlib.sha256(image).hexdigest() != entry["sha256"]:
        raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, "moveq zero-d0 fixture image hash mismatch")
    with tempfile.TemporaryDirectory() as temporary:
        temporary_path = pathlib.Path(temporary)
        rom = temporary_path / "architecture-baseline-moveq.bin"
        rom.write_bytes(image)
        command = [str(args.segarecomp), "emit-moveq-c", str(rom), entry["pc"], entry["offset"],
                   entry["sr"], *entry["d"]]
        try:
            generated = subprocess.run(command, text=True, capture_output=True, cwd=PROJECT_ROOT, check=False)
        except OSError as error:
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"cannot run emit-moveq-c: {error}") from error
        if generated.returncode != 0 or generated.stderr != "":
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"emit-moveq-c failed: {generated.stderr}")
        generated_c_sha256 = hashlib.sha256(generated.stdout.encode("utf-8")).hexdigest()
        source = temporary_path / "architecture-baseline-moveq.c"
        binary = temporary_path / "architecture-baseline-moveq"
        source.write_text(generated.stdout)
        try:
            compiled = subprocess.run(
                [str(args.cc), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(binary)],
                text=True, capture_output=True, cwd=PROJECT_ROOT, check=False)
        except OSError as error:
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"cannot run C compiler: {error}") from error
        if compiled.returncode != 0:
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"generated MOVEQ C failed to compile: {compiled.stderr}")
        try:
            executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
        except OSError as error:
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"cannot run generated MOVEQ binary: {error}") from error
        if executed.returncode != 0 or executed.stderr != "":
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"generated MOVEQ binary failed: {executed.stderr}")
        try:
            state = json.loads(executed.stdout)
        except json.JSONDecodeError as error:
            raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, f"generated MOVEQ binary wrote invalid JSON: {error}") from error
    if state.get("d") != entry["expected_d"] or state.get("sr") != entry["expected_sr"]:
        raise ProbeError(
            EXIT_PROBE_OUTPUT_MALFORMED,
            "emit-moveq-c executed state diverged from its own established fixture expectation",
        )
    fields = {"fixture_id": "synthetic/moveq-fixtures/zero-d0", "executed_state": state}
    informational = {"generated_c_sha256": generated_c_sha256}
    return fields, informational


# Project-authored synthetic byte pattern already established by
# tests/genesis_startup_bridge_c4_test.py: a CTRL1/CTRL2 (SEG-007-T020/T021)
# LONG-read TST.L at $00A10008, immediately followed by an unsupported RESET
# frontier with no intervening branch. No commercial content.
_BRIDGE_C4_IMAGE = bytes((0x4A, 0xB9, 0x00, 0xA1, 0x00, 0x08,  # TST.L $00A10008
                           0x4E, 0x70))                          # RESET
_BRIDGE_C4_ENTRY = "00000b00"


def probe_bridge_synthetic_c4_full_chain(args: argparse.Namespace, work_root: pathlib.Path) -> tuple[dict, dict]:
    """Full generate/compile/execute pipeline stage: drives
    `tools/genesis_startup_bridge.py --mode synthetic` end to end (generate,
    compile, link, execute) over the same project-authored synthetic byte
    pattern `tests/genesis_startup_bridge_c4_test.py` already establishes,
    capturing the sanitized wire report and the full runtime report. The
    generated C text is hashed only as an informational field."""
    driver = PROJECT_ROOT / "tools" / "genesis_startup_bridge.py"
    with tempfile.TemporaryDirectory(dir=work_root) as temporary:
        temporary_path = pathlib.Path(temporary)
        rom = temporary_path / "architecture-baseline-bridge-c4.bin"
        rom.write_bytes(_BRIDGE_C4_IMAGE)
        out_dir = temporary_path / "out"
        full_report_path = temporary_path / "full-report.json"
        command = [sys.executable, str(driver), "--segarecomp", str(args.segarecomp), "--cc", str(args.cc),
                   "--rom", str(rom), "--entry", _BRIDGE_C4_ENTRY, "--mode", "synthetic",
                   "--out-dir", str(out_dir), "--full-report-path", str(full_report_path)]
        try:
            result = subprocess.run(command, text=True, capture_output=True, cwd=PROJECT_ROOT, check=False)
        except OSError as error:
            raise ProbeError(EXIT_PROBE_EXECUTION_FAILED, f"cannot run genesis_startup_bridge.py: {error}") from error
        if result.returncode != 0:
            raise ProbeError(
                EXIT_PROBE_EXECUTION_FAILED,
                f"genesis_startup_bridge.py exited {result.returncode}: {result.stderr}",
            )
        try:
            sanitized_report = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, f"bridge sanitized report was invalid JSON: {error}") from error
        try:
            full_report = json.loads(full_report_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, f"bridge full report was unavailable/invalid: {error}") from error
        source_path = out_dir / "bridge.generated.c"
        try:
            generated_c_bytes = source_path.read_bytes()
        except OSError as error:
            raise ProbeError(EXIT_PROBE_OUTPUT_MALFORMED, f"generated bridge C source was unavailable: {error}") from error
    expected_sanitized = {
        "result": "stop", "stop_class": "unsupported_cpu_form",
        "diagnostic_category": "valid_but_unsupported_instruction",
        "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
    }
    if any(sanitized_report.get(key) != value for key, value in expected_sanitized.items()):
        raise ProbeError(
            EXIT_PROBE_OUTPUT_MALFORMED,
            "bridge sanitized report diverged from its own established fixture expectation",
        )
    runtime = full_report.get("runtime") if isinstance(full_report, dict) else None
    if not isinstance(runtime, dict) or runtime.get("sr") != "0x0004" or runtime.get("d", [None])[0] != "0x00000000":
        raise ProbeError(
            EXIT_PROBE_OUTPUT_MALFORMED,
            "bridge full report diverged from its own established fixture expectation",
        )
    fields = {
        "entry": _BRIDGE_C4_ENTRY,
        "sanitized_report": sanitized_report,
        "full_report": redact_full_bridge_runtime(full_report),
    }
    informational = {"generated_c_sha256": hashlib.sha256(generated_c_bytes).hexdigest()}
    return fields, informational


PROBE_DESCRIPTIONS: dict[str, str] = {
    "analysis_genesis_general_startup": (
        "Analysis-only pipeline stage: genesis-general-startup CLI JSON report over the "
        "synthetic two-call-frame reset image in "
        "tests/fixtures/genesis-general-startup-vectors.json."
    ),
    "legacy_moveq_slice": (
        "Retained legacy pipeline surface: emit-moveq-c generated C, compiled and executed, "
        "over the project-authored zero-d0 vector in tests/fixtures/moveq-fixtures.json."
    ),
    "bridge_synthetic_c4_full_chain": (
        "Full generate/compile/execute pipeline stage: tools/genesis_startup_bridge.py "
        "--mode synthetic over the project-authored CTRL1/CTRL2 TST.L-then-RESET synthetic "
        "image, capturing the sanitized and full runtime reports."
    ),
}
# Fixed, deterministic probe order (also the manifest's own "probes" array order).
PROBE_NAMES: list[str] = list(PROBE_DESCRIPTIONS)


def run_all_probes(args: argparse.Namespace, work_root: pathlib.Path) -> dict[str, dict]:
    results: dict[str, dict] = {}
    for name in PROBE_NAMES:
        if name == "analysis_genesis_general_startup":
            fields, informational = probe_analysis_genesis_general_startup(args)
        elif name == "legacy_moveq_slice":
            fields, informational = probe_legacy_moveq_slice(args)
        elif name == "bridge_synthetic_c4_full_chain":
            fields, informational = probe_bridge_synthetic_c4_full_chain(args, work_root)
        else:  # pragma: no cover - PROBE_NAMES is fixed above
            raise AssertionError(f"unhandled probe {name}")
        results[name] = {
            "name": name,
            "description": PROBE_DESCRIPTIONS[name],
            "fields": fields,
            "canonical_sha256": sha256_of(fields),
            "informational": informational,
        }
    return results


def build_manifest(results: dict[str, dict]) -> dict:
    return {
        "schema": 1,
        "ownership": "Project-authored synthetic architecture baseline manifest; no commercial or local-path content.",
        "probes": [results[name] for name in PROBE_NAMES],
    }


def write_manifest(manifest: dict, manifest_path: pathlib.Path) -> None:
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n")


def load_committed_manifest(manifest_path: pathlib.Path) -> dict:
    if not manifest_path.is_file():
        raise ProbeError(EXIT_MANIFEST_UNAVAILABLE, f"missing committed manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as error:
        raise ProbeError(EXIT_MANIFEST_UNAVAILABLE, f"committed manifest is invalid JSON: {error}") from error
    if (not isinstance(manifest, dict) or manifest.get("schema") != 1 or
            not isinstance(manifest.get("probes"), list)):
        raise ProbeError(EXIT_MANIFEST_UNAVAILABLE, "committed manifest has an unrecognized shape")
    return manifest


def run_capture(args: argparse.Namespace, manifest_path: pathlib.Path, work_root: pathlib.Path) -> int:
    results = run_all_probes(args, work_root)
    manifest = build_manifest(results)
    write_manifest(manifest, manifest_path)
    print(canonical_json_text({"status": "captured", "probe_count": len(results),
                                "manifest_path": str(manifest_path)}))
    return EXIT_OK


def run_verify(args: argparse.Namespace, manifest_path: pathlib.Path, work_root: pathlib.Path) -> int:
    committed = load_committed_manifest(manifest_path)
    committed_probes = {probe.get("name"): probe for probe in committed["probes"] if isinstance(probe, dict)}
    results = run_all_probes(args, work_root)

    mismatches: dict[str, dict] = {}
    informational_differences: dict[str, dict] = {}
    for name in PROBE_NAMES:
        committed_probe = committed_probes.get(name)
        current_probe = results.get(name)
        if committed_probe is None:
            mismatches[name] = {"reason": "missing_from_committed_manifest"}
            continue
        if committed_probe.get("canonical_sha256") != current_probe["canonical_sha256"]:
            mismatches[name] = {
                "reason": "fields_mismatch",
                "committed_fields": committed_probe.get("fields"),
                "current_fields": current_probe["fields"],
            }
        if committed_probe.get("informational") != current_probe["informational"]:
            informational_differences[name] = {
                "committed_informational": committed_probe.get("informational"),
                "current_informational": current_probe["informational"],
            }

    if informational_differences:
        sys.stdout.write(json.dumps(
            {"informational_differences": informational_differences}, indent=2, sort_keys=False) + "\n")
    if mismatches:
        sys.stderr.write(json.dumps({"mismatches": mismatches}, indent=2, sort_keys=False) + "\n")
        return EXIT_VERIFY_MISMATCH
    print(canonical_json_text({"status": "verified", "probe_count": len(results),
                                "manifest_path": str(manifest_path)}))
    return EXIT_OK


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--segarecomp", required=True, help="path to the built segarecomp binary")
    parser.add_argument("--cc", required=True, help="path to a C11 compiler")
    parser.add_argument("--mode", required=True, choices=("capture", "verify"))
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST_PATH),
                         help="manifest path (default: tests/fixtures/architecture-baseline-manifest.json)")
    args = parser.parse_args()
    args.segarecomp = pathlib.Path(args.segarecomp).resolve()
    args.cc = pathlib.Path(args.cc).resolve()
    if not args.segarecomp.is_file():
        sys.stderr.write(f"architecture_baseline: no such segarecomp binary: {args.segarecomp}\n")
        return EXIT_USAGE
    manifest_path = pathlib.Path(args.manifest).resolve()
    work_root = PROJECT_ROOT / "build"
    work_root.mkdir(parents=True, exist_ok=True)

    try:
        if args.mode == "capture":
            return run_capture(args, manifest_path, work_root)
        return run_verify(args, manifest_path, work_root)
    except ProbeError as error:
        sys.stderr.write(f"architecture_baseline: {error.message}\n")
        return error.code


if __name__ == "__main__":
    raise SystemExit(main())
