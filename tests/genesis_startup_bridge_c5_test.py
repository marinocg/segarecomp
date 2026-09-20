#!/usr/bin/env python3
"""C5 end-to-end test: the public driver owns generation, C11 and execution."""
import json
import base64
import copy
import importlib.util
import pathlib
import re
import subprocess
import sys
import tempfile


def capture_generated_source(output_dir: pathlib.Path) -> dict:
    source = output_dir / "bridge.generated.c"
    snapshot: dict = {"path": str(source), "exists": False, "bytes": None, "error": None}
    try:
        snapshot["exists"] = source.exists()
        if snapshot["exists"]:
            snapshot["bytes"] = source.read_bytes()
    except OSError as error:
        snapshot["error"] = str(error)
    return snapshot


def run_case(name: str, command: list[str], cwd: pathlib.Path, root: pathlib.Path,
             records: list[dict]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c5-") as temporary:
        output_dir = pathlib.Path(temporary)
        result = subprocess.run(command + ["--out-dir", str(output_dir)], text=True, capture_output=True, cwd=cwd)
        source = capture_generated_source(output_dir)
        record = {"fixture": name, "name": name, "command": command, "cwd": str(cwd),
                  "output_dir": str(output_dir), "source": source, "returncode": result.returncode,
                  "stdout": result.stdout, "stderr": result.stderr, "json": None}
        records.append(record)
    try:
        record["json"] = json.loads(result.stdout)
    except json.JSONDecodeError:
        pass
    return result


def capture_full_report(path: pathlib.Path) -> dict:
    snapshot: dict = {"path": str(path), "exists": path.exists(), "bytes": None, "json": None,
                      "error": None}
    if not snapshot["exists"]:
        return snapshot
    try:
        snapshot["bytes"] = path.read_bytes()
        snapshot["json"] = json.loads(snapshot["bytes"])
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        snapshot["error"] = str(error)
    return snapshot


def fail(records: list[dict], snapshots: list[dict]) -> int:
    for record in records:
        diagnostic = dict(record)
        source = diagnostic.get("source")
        if isinstance(source, dict):
            source = dict(source)
            if isinstance(source.get("bytes"), bytes):
                source["bytes"] = source["bytes"].decode("utf-8", errors="replace")
            diagnostic["source"] = source
        sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
    for snapshot in snapshots:
        diagnostic = dict(snapshot)
        if isinstance(diagnostic.get("bytes"), bytes):
            diagnostic["bytes"] = diagnostic["bytes"].decode("utf-8", errors="replace")
        sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
    return 1


def source_contains(snapshot: dict, expected: str) -> bool:
    source = snapshot.get("bytes")
    return (snapshot.get("exists") is True and snapshot.get("error") is None and
            isinstance(source, bytes) and expected.encode("utf-8") in source)


def exact_completed_full(full: object, sanitized: dict, expected_d0: str, expected_pc: str,
                         expected_a7: str, expected_ram_prefix: bytes) -> bool:
    expected_full_keys = ["schema_version", "report_kind", "rom_sha256", "result", "runtime", "stop_class",
                           "diagnostic_category", "c4_lowering_dimensions", "provenance"]
    expected_runtime_keys = ["d", "a", "usp", "sr", "pc", "work_ram_base64"]
    if (not isinstance(full, dict) or list(full) != expected_full_keys or
            full.get("schema_version") != sanitized.get("schema_version") or
            full.get("report_kind") != "full" or full.get("rom_sha256") != sanitized.get("rom_sha256") or
            full.get("result") != sanitized.get("result") or full.get("result") != "completed" or
            full.get("stop_class") != sanitized.get("stop_class") or
            full.get("diagnostic_category") != sanitized.get("diagnostic_category") or
            full.get("c4_lowering_dimensions") is not None or full.get("provenance") is not None):
        return False
    runtime = full.get("runtime")
    if not isinstance(runtime, dict) or list(runtime) != expected_runtime_keys:
        return False
    if (runtime.get("d") != [expected_d0] + ["0x00000000"] * 7 or
            runtime.get("a") != ["0x00000000"] * 7 + [expected_a7] or
            runtime.get("usp") != "0x00000000" or runtime.get("sr") != "0x0000" or
            runtime.get("pc") != expected_pc):
        return False
    try:
        ram = base64.b64decode(runtime["work_ram_base64"], validate=True)
    except (KeyError, TypeError, ValueError):
        return False
    return len(ram) == 65536 and ram[:len(expected_ram_prefix)] == expected_ram_prefix and not any(ram[len(expected_ram_prefix):])


def exact_wrong_return_full(full: object, sanitized: dict) -> bool:
    if (not isinstance(full, dict) or full.get("result") != "stop" or
            full.get("stop_class") != sanitized.get("stop_class") or
            full.get("diagnostic_category") != sanitized.get("diagnostic_category") or
            full.get("c4_lowering_dimensions") is not None):
        return False
    runtime = full.get("runtime")
    if not isinstance(runtime, dict):
        return False
    try:
        ram = base64.b64decode(runtime["work_ram_base64"], validate=True)
    except (KeyError, TypeError, ValueError):
        return False
    return (runtime.get("d") == ["0x00000000"] * 8 and
            runtime.get("a") == ["0x00000000"] * 7 + ["0x00ff0004"] and
            runtime.get("usp") == "0x00000000" and
            runtime.get("sr") == "0x0004" and runtime.get("pc") == "0x00000b06" and
            len(ram) == 65536 and not any(ram))


def exact_frontier_provenance(full: object) -> bool:
    """Check every retained field for the project-authored RESET frontier."""
    if not isinstance(full, dict):
        return False
    provenance = full.get("provenance")
    return provenance == {
        "has_instruction_provenance": True,
        "instruction": {
            "cpu_variant": "mc68000", "source_address": "0x00000b0a", "image_offset": 10,
            "primary_bytes": "4e70", "length": 2,
        },
        "has_access": False,
        "access_address": "0x00000000",
        "access_width": None,
        "access_direction": None,
        "mapping_claim_count": 1,
        "mapping_claims": [{
            "name": "raw_cartridge_rom", "target_begin": "0x00000b00", "target_end": "0x00000b0c",
            "image_begin": 0, "image_end": 12,
        }],
        "bus_access_count": 1,
        "bus_accesses": [{
            "ordinal": 0, "kind": "instruction_read", "address": "0x00000b0a",
            "raw_bytes": "4e70", "raw_byte_count": 2, "region": "raw_cartridge_rom",
        }],
    }


def mutated(value: dict, mutate) -> dict:
    """Apply exactly one mutation to a fresh report baseline."""
    candidate = copy.deepcopy(value)
    mutate(candidate)
    return candidate


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    global bridge_driver
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", driver)
    if spec is None or spec.loader is None:
        return 1
    bridge_driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge_driver)
    runtime_header = (root / "platforms" / "genesis" / "runtime" / "runtime.h").read_text()
    runtime_source = (root / "platforms" / "genesis" / "runtime" / "runtime.c").read_text()
    stop_match = re.search(r"typedef struct GenesisRuntimeStop \{(.*?)\} GenesisRuntimeStop;",
                           runtime_header, re.DOTALL)
    if (stop_match is None or
            re.sub(r"\s+", " ", stop_match.group(1)).strip() !=
            "GenesisStopClass stop_class; GenesisDiagnosticCategory diagnostic_category; /* ADR 0015: this is selected by the generated stop site, not program metadata. */ GenesisC4LoweringDimensions c4_lowering_dimensions; GenesisProvenance provenance;" or
            "cpu_family" in runtime_header or "cpu_size" in runtime_header or
            "cpu_addressing_mode_class" in runtime_header or
            "genesis_valid_report_metadata" not in runtime_source):
        return 1
    records: list[dict] = []
    # Project-authored: CLR.B $00FF0000; BRA.S +2; padding; RESET frontier.
    image = bytes((0x42, 0xB9, 0x00, 0xFF, 0x00, 0x00, 0x60, 0x02, 0, 0, 0x4E, 0x70))
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "bridge-synthetic.bin"
        frontier_full_path = pathlib.Path(temporary) / "frontier-full.json"
        rom.write_bytes(image)
        command = [sys.executable, str(driver),
                   "--segarecomp", str(binary), "--cc", str(compiler), "--rom", str(rom),
                   "--entry", "00000b00", "--mode", "synthetic", "--compare-runs"]
        first = run_case("frontier-from-root", command, root, root, records)
        second = run_case("frontier-from-build", command, binary.parent, root, records)
        explicit = run_case("frontier-from-build-unique-output", command, binary.parent, root, records)
        frontier_full = run_case("frontier-full-provenance", [
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--full-report-path", str(frontier_full_path)], root, root, records)
        frontier_full_snapshot = capture_full_report(frontier_full_path)
        expected_hash_mismatch = run_case("expected-hash-mismatch", [
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--expect-sha256", "0" * 64], root, root, records)
    first_report, second_report, explicit_report = records[0]["json"], records[1]["json"], records[2]["json"]
    if (first.returncode != 0 or second.returncode != 0 or explicit.returncode != 0 or
            first.stdout != second.stdout or first.stdout != explicit.stdout or first_report is None or
            second_report is None or explicit_report is None or
            not records[0]["source"]["exists"] or records[0]["source"]["error"] is not None or
            not records[1]["source"]["exists"] or records[1]["source"]["error"] is not None or
            records[0]["source"]["bytes"] != records[1]["source"]["bytes"] or
            records[0]["output_dir"] == records[1]["output_dir"]):
        return fail(records, [])
    report = first_report
    expected = {"schema_version": 1, "report_kind": "sanitized", "result": "stop",
                 "stop_class": "unsupported_cpu_form",
                 "diagnostic_category": "valid_but_unsupported_instruction", "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
                 "c4_lowering_dimensions": None,
                 "reports_match": True}
    if any(report.get(key) != value for key, value in expected.items()) or len(report.get("rom_sha256", "")) != 64:
        return fail(records, [])
    generated_frontier = records[0]["source"]
    if (not source_contains(generated_frontier,
                            "GenesisReportMetadata GENESIS_BRIDGE_REPORT_METADATA = { GENESIS_CPU_DIMENSIONS_RESET }") or
            source_contains(generated_frontier, "cpu_family") or
            source_contains(generated_frontier, "cpu_size") or
            source_contains(generated_frontier, "cpu_addressing_mode_class")):
        return fail(records, [])
    frontier_full_report = frontier_full_snapshot["json"]
    frontier_report = records[3]["json"]
    digest = frontier_report.get("rom_sha256") if isinstance(frontier_report, dict) else ""
    frontier_checks = {
        "frontier full report: unexpected process/result": frontier_full.returncode == 0 and frontier_full_report is not None,
        "frontier full report: provenance mismatch": exact_frontier_provenance(frontier_full_report),
        "frontier full report: rejected by driver validator": isinstance(frontier_report, dict) and isinstance(digest, str) and bridge_driver.valid_full(frontier_full_report, frontier_report, digest),
        "expected hash mismatch: generation was attempted": expected_hash_mismatch.returncode == 4 and not expected_hash_mismatch.stdout and not records[-1]["source"]["exists"],
    }
    if not all(frontier_checks.values()):
        for name, passed in frontier_checks.items():
            if not passed:
                sys.stderr.write(f"{name}\n")
        return fail(records, [frontier_full_snapshot])
    baseline_raw = frontier_full_snapshot["bytes"]
    baseline = bridge_driver.parse_canonical_full(baseline_raw if baseline_raw is not None else b"")
    baseline_checks = {
        "baseline full report: canonical parsing failed": baseline == frontier_full_report,
        "baseline full report: validator rejected": isinstance(frontier_report, dict) and
            isinstance(digest, str) and bridge_driver.valid_full(baseline, frontier_report, digest),
    }
    if not all(baseline_checks.values()):
        for name, passed in baseline_checks.items():
            if not passed:
                sys.stderr.write(f"{name}\n")
        return fail(records, [frontier_full_snapshot])

    # Every mutation starts from the independently parsed, canonical baseline.
    # Rebuilding runtime keys is intentional: a nested JSON object has a required
    # canonical order just like the enclosing full report.
    validation_mutations = (
        ("reordered-runtime-keys", mutated(baseline, lambda value: value.__setitem__(
            "runtime", {key: value["runtime"][key] for key in
                        ("work_ram_base64", "pc", "sr", "usp", "a", "d")}))),
        ("non-boolean-has-access", mutated(baseline, lambda value: value["provenance"].__setitem__("has_access", 0))),
        ("boolean-schema-version", mutated(baseline, lambda value: value.__setitem__("schema_version", True))),
        ("uppercase-runtime-pc", mutated(baseline, lambda value: value["runtime"].__setitem__("pc", "0X00000b0a"))),
        ("mapping-claim-count-mismatch", mutated(baseline, lambda value: value["provenance"].__setitem__("mapping_claim_count", 2))),
        ("boolean-mapping-claim-count", mutated(baseline, lambda value: value["provenance"].__setitem__("mapping_claim_count", True))),
        ("out-of-range-mapping-claim-count", mutated(baseline, lambda value: value["provenance"].__setitem__("mapping_claim_count", 5))),
        ("invalid-work-ram-base64", mutated(baseline, lambda value: value["runtime"].__setitem__("work_ram_base64", "!" * 87384))),
        ("unsupported-instruction-cpu-variant", mutated(baseline, lambda value: value["provenance"]["instruction"].__setitem__("cpu_variant", "mc68010"))),
        ("boolean-image-offset", mutated(baseline, lambda value: value["provenance"]["instruction"].__setitem__("image_offset", False))),
        ("out-of-range-instruction-length", mutated(baseline, lambda value: value["provenance"]["instruction"].__setitem__("length", 13))),
        ("out-of-range-instruction-address", mutated(baseline, lambda value: value["provenance"]["instruction"].__setitem__("source_address", "0x01000000"))),
        ("invalid-primary-bytes", mutated(baseline, lambda value: value["provenance"]["instruction"].__setitem__("primary_bytes", "4E71"))),
        ("out-of-range-absent-access-address", mutated(baseline, lambda value: value["provenance"].__setitem__("access_address", "0x01000000"))),
        ("inrange-absent-access-address", mutated(baseline, lambda value: value["provenance"].__setitem__("access_address", "0x00000001"))),
        ("present-width-with-absent-access", mutated(baseline, lambda value: value["provenance"].__setitem__("access_width", "word"))),
        ("overlong-mapping-name", mutated(baseline, lambda value: value["provenance"]["mapping_claims"][0].__setitem__("name", "x" * 65))),
        ("invalid-mapping-target-range", mutated(baseline, lambda value: value["provenance"]["mapping_claims"][0].__setitem__("target_end", "0x00000b00"))),
        ("invalid-mapping-image-range", mutated(baseline, lambda value: value["provenance"]["mapping_claims"][0].__setitem__("image_end", 11))),
        ("boolean-bus-count", mutated(baseline, lambda value: value["provenance"].__setitem__("bus_access_count", True))),
        ("out-of-range-bus-count", mutated(baseline, lambda value: value["provenance"].__setitem__("bus_access_count", 5))),
        ("boolean-bus-ordinal", mutated(baseline, lambda value: value["provenance"]["bus_accesses"][0].__setitem__("ordinal", False))),
        ("out-of-range-bus-address", mutated(baseline, lambda value: value["provenance"]["bus_accesses"][0].__setitem__("address", "0x01000000"))),
        ("out-of-range-raw-byte-count", mutated(baseline, lambda value: value["provenance"]["bus_accesses"][0].__setitem__("raw_byte_count", 13))),
        ("invalid-raw-bytes", mutated(baseline, lambda value: value["provenance"]["bus_accesses"][0].__setitem__("raw_bytes", "4E71"))),
        ("unknown-stop-class", mutated(baseline, lambda value: value.__setitem__("stop_class", "unknown_stop"))),
        ("unknown-diagnostic-category", mutated(baseline, lambda value: value.__setitem__("diagnostic_category", "unknown_diagnostic"))),
        ("invalid-stop-diagnostic-pair", mutated(baseline, lambda value: value.__setitem__("diagnostic_category", "rom_write_prohibited"))),
        ("missing-rom-sha256", mutated(baseline, lambda value: value.pop("rom_sha256"))),
        ("malformed-rom-sha256", mutated(baseline, lambda value: value.__setitem__("rom_sha256", "not-a-sha256"))),
    )
    invalid_cpu_dimensions = mutated(
        frontier_report if isinstance(frontier_report, dict) else {},
        lambda value: value.__setitem__(
            "cpu_dimensions", {"family": "nop", "size": "word", "addressing_mode_class": "implied"}))
    unexpected_acceptances = [
        {"mutation": name, "candidate": value}
        for name, value in validation_mutations
        if bridge_driver.valid_full(value, frontier_report, digest)
    ]
    if bridge_driver.valid_sanitized(invalid_cpu_dimensions, digest):
        unexpected_acceptances.append({"mutation": "invalid-sanitized-cpu-dimensions",
                                        "candidate": invalid_cpu_dimensions})
    reordered_cpu_dimensions = mutated(
        frontier_report if isinstance(frontier_report, dict) else {},
        lambda value: value.__setitem__(
            "cpu_dimensions", {"size": "none", "family": "nop", "addressing_mode_class": "implied"}))
    if bridge_driver.valid_sanitized(reordered_cpu_dimensions, digest):
        unexpected_acceptances.append({"mutation": "reordered-sanitized-cpu-dimensions",
                                       "candidate": reordered_cpu_dimensions})
    stop_wire_mutations = (
        ("unknown-stop-class", mutated(frontier_report, lambda value: value.__setitem__("stop_class", "unknown_stop"))),
        ("unknown-diagnostic-category", mutated(frontier_report, lambda value: value.__setitem__("diagnostic_category", "unknown_diagnostic"))),
        ("invalid-stop-diagnostic-pair", mutated(frontier_report, lambda value: value.__setitem__("diagnostic_category", "rom_write_prohibited"))),
    )
    unexpected_acceptances.extend(
        {"mutation": name, "candidate": value}
        for name, value in stop_wire_mutations
        if bridge_driver.valid_sanitized(value, digest))
    if unexpected_acceptances:
        for diagnostic in unexpected_acceptances:
            sys.stderr.write("unexpected validator acceptance:\n")
            sys.stderr.write(json.dumps(diagnostic, indent=2) + "\n")
        return fail(records, [frontier_full_snapshot])
    sha_mutations = {
        "missing-rom-sha256": mutated(baseline, lambda value: value.pop("rom_sha256")),
        "malformed-rom-sha256": mutated(baseline, lambda value: value.__setitem__("rom_sha256", "not-a-sha256")),
        "present-mismatched-rom-sha256": mutated(baseline, lambda value: value.__setitem__("rom_sha256", "0" * 64)),
    }
    if (any(bridge_driver.report_sha_status(value, digest) != 5 for name, value in sha_mutations.items()
            if name in ("missing-rom-sha256", "malformed-rom-sha256")) or
            any(bridge_driver.report_sha_status(value, digest) != 4 for name, value in sha_mutations.items()
                if name == "present-mismatched-rom-sha256") or
            bridge_driver.parse_canonical_full((baseline_raw or b"")[:-1] + b"\r\n") is not None or
            bridge_driver.parse_canonical_full(b"\xff\n") is not None):
        return fail(records, [frontier_full_snapshot])
    with tempfile.TemporaryDirectory() as temporary:
        rom = pathlib.Path(temporary) / "completion.bin"
        full = pathlib.Path(temporary) / "completion-full.json"
        rom.write_bytes(bytes((0x70, 0x05, 0x4E, 0x75)))
        completion = run_case("completion", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b02", "--completion-sentinel", "00400000",
            "--full-report-path", str(full)], root, root, records)
        ordinary = run_case("ordinary-rts", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic"], root, root, records)
        incomplete = run_case("incomplete-completion-options", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b02"], root, root, records)
        commercial = run_case("commercial-completion-options", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(rom), "--entry", "00000b00", "--mode", "commercial",
            "--completion-rts", "00000b02", "--completion-sentinel", "00400000"], root, root, records)
        full_and_compare = run_case("full-path-and-compare", [
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--full-report-path", str(pathlib.Path(temporary) / "incompatible-full.json"), "--compare-runs"],
            root, root, records)
        routable_sentinel = run_case("routable-completion-sentinel", [
            sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
            "--rom", str(rom), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b02", "--completion-sentinel", "00010000"], root, root, records)
        shared_return = pathlib.Path(temporary) / "shared-return.bin"
        shared_full = pathlib.Path(temporary) / "shared-return-full.json"
        # One physical RTS is first reached as the JSR callee and returns to
        # 0xB06; BRA.S then reaches that exact RTS with no frame and completes.
        shared_return.write_bytes(bytes((0x4E, 0xB9, 0x00, 0x00, 0x0B, 0x0A, 0x60, 0x02,
                                         0x00, 0x00, 0x4E, 0x75)))
        shared = run_case("shared-return", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(shared_return), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b0a", "--completion-sentinel", "00400000",
            "--full-report-path", str(shared_full)], binary.parent, root, records)
        intermediate = pathlib.Path(temporary) / "intermediate-completion.bin"
        intermediate_full = pathlib.Path(temporary) / "intermediate-completion-full.json"
        # BRA.S transfers into a distinct MOVEQ/RTS block; completion occurs
        # only after that intermediate dispatch.
        intermediate.write_bytes(bytes((0x60, 0x02, 0, 0, 0x70, 0x05, 0x4E, 0x75)))
        after_intermediate = run_case("completion-after-intermediate-block", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(intermediate), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b06", "--completion-sentinel", "00400000",
            "--full-report-path", str(intermediate_full)], root, root, records)
        wrong_stack = pathlib.Path(temporary) / "wrong-stack.bin"
        wrong_stack_full = pathlib.Path(temporary) / "wrong-stack-full.json"
        # CLR.W is a normal supported RAM write.  It clears the initialized
        # sentinel's nonzero byte before the declared RTS reads it.
        wrong_stack.write_bytes(bytes((0x42, 0x79, 0x00, 0xFF, 0x00, 0x04, 0x4E, 0x75)))
        wrong_return = run_case("wrong-stack-return", [
            sys.executable, str(driver), "--segarecomp", str(binary),
            "--cc", str(compiler), "--rom", str(wrong_stack), "--entry", "00000b00", "--mode", "synthetic",
            "--completion-rts", "00000b06", "--completion-sentinel", "00400000",
            "--full-report-path", str(wrong_stack_full)], root, root, records)
        completion_snapshot = capture_full_report(full)
        shared_snapshot = capture_full_report(shared_full)
        intermediate_snapshot = capture_full_report(intermediate_full)
        wrong_stack_snapshot = capture_full_report(wrong_stack_full)
        completion_full = completion_snapshot["json"]
        shared_full_report = shared_snapshot["json"]
        intermediate_full_report = intermediate_snapshot["json"]
        wrong_stack_full_report = wrong_stack_snapshot["json"]
    by_name = {record["name"]: record for record in records}
    completion_report = by_name["completion"]["json"]
    shared_report = by_name["shared-return"]["json"]
    intermediate_report = by_name["completion-after-intermediate-block"]["json"]
    wrong_return_report = by_name["wrong-stack-return"]["json"]
    completion_source = by_name["completion"]["source"]
    shared_source = by_name["shared-return"]["source"]
    expected_completion = {"schema_version": 1, "report_kind": "sanitized", "result": "completed",
                           "stop_class": None, "diagnostic_category": None, "cpu_dimensions": None,
                           "c4_lowering_dimensions": None}
    checks = {
        "completion: unexpected process/report": completion.returncode == 0 and completion_report is not None and completion_full is not None and all(completion_report.get(key) == value for key, value in expected_completion.items()),
        "completion: unexpected runtime state": completion_report is not None and exact_completed_full(completion_full, completion_report, "0x00000005", "0x00400000", "0x00ff0008", bytes((0, 0, 0, 0, 0, 0x40, 0, 0))),
        "completion: missing ingress/sentinel source": source_contains(completion_source, "runtime.a[7] = UINT32_C(0x00FF0004)") and source_contains(completion_source, "runtime.work_ram[4] = UINT8_C(0x00); runtime.work_ram[5] = UINT8_C(0x40); runtime.work_ram[6] = UINT8_C(0x00); runtime.work_ram[7] = UINT8_C(0x00);"),
        # ADR-0011 Decision §1: an RTS with no preceding open call in this
        # discovery pass is a legitimate dead end at the static-discovery
        # stage (no longer `return_context_missing`), but a program with no
        # calls at all still cannot supply the RTS's own runtime membership
        # check any legal return target, so C11 emission itself rejects it
        # ("translation rejected: invalid C4 static return edge") -- the
        # driver surfaces that as a plain build failure with no generated
        # source, exit code 1.
        "ordinary-rts: unexpected missing-context rejection": ordinary.returncode == 1 and
            not by_name["ordinary-rts"]["source"]["exists"],
        "arguments: unexpected invalid completion status": incomplete.returncode == 8 and commercial.returncode == 8 and full_and_compare.returncode == 8,
        "routable sentinel: expected build rejection without output": routable_sentinel.returncode == 1 and not routable_sentinel.stdout and not by_name["routable-completion-sentinel"]["source"]["exists"],
        "shared-rts: unexpected process/report": shared.returncode == 0 and shared_report is not None and shared_full_report is not None,
        "shared-rts: unexpected runtime state": shared_report is not None and exact_completed_full(shared_full_report, shared_report, "0x00000000", "0x00400000", "0x00ff0008", bytes((0, 0, 0x0B, 0x06, 0, 0x40, 0, 0))),
        "shared-rts: unexpected authorized targets": source_contains(shared_source, "m68k_observed_return != UINT32_C(0x00000B06) && m68k_observed_return != UINT32_C(0x00400000)"),
        "intermediate: unexpected process/report": after_intermediate.returncode == 0 and intermediate_report is not None and intermediate_full_report is not None,
        "intermediate: unexpected runtime state": intermediate_report is not None and exact_completed_full(intermediate_full_report, intermediate_report, "0x00000005", "0x00400000", "0x00ff0008", bytes((0, 0, 0, 0, 0, 0x40, 0, 0))),
        "wrong-return: unexpected diagnostic": wrong_return.returncode == 0 and wrong_return_report is not None and wrong_return_report.get("result") == "stop" and wrong_return_report.get("stop_class") == "unsupported_memory_region" and wrong_return_report.get("diagnostic_category") == "return_target_mismatch",
        "wrong-return: unexpected runtime state": wrong_return_report is not None and exact_wrong_return_full(wrong_stack_full_report, wrong_return_report),
        "generated sources: outputs were not isolated": len({record["output_dir"] for record in records}) == len(records) and all(pathlib.Path(record["output_dir"]).parent == root / "build" for record in records),
    }
    failures = [name for name, passed in checks.items() if not passed]
    if failures:
        for failure in failures:
            sys.stderr.write(f"{failure}\n")
        return fail(records, [completion_snapshot, shared_snapshot, intermediate_snapshot, wrong_stack_snapshot])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
