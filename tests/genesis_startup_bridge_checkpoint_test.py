#!/usr/bin/env python3
"""ADR-0013 Decision §7c: generic Phase-B checkpoint/resume mechanism.

Project-authored synthetic driver-level fixture, no commercial input. Reuses
the exact scripted `segarecomp`-shaped proxy pattern the sibling
`genesis_startup_bridge_phase_b_expansion_test.py` already uses, but exercises
`tools/genesis_startup_bridge.py`'s `run_expansion_loop` / `load_checkpoint` /
`save_checkpoint` functions directly (imported as a module) so the
per-invocation batch budget can be temporarily narrowed to a small value for a
fast, deterministic two-invocation resumption test, without retuning the
production constants themselves.
"""
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile


def load_bridge_module(root: pathlib.Path):
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", root / "tools" / "genesis_startup_bridge.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def work_ram_base64() -> str:
    return "A" * 87382 + "=="


def build_reports(kind: str, digest: str, boundary_hex: str | None = None) -> tuple[dict, dict]:
    runtime = {
        "d": ["0x00000000"] * 8, "a": ["0x00000000"] * 8, "usp": "0x00000000",
        "sr": "0x0000", "pc": "0x00000000", "work_ram_base64": work_ram_base64(),
    }
    if kind == "completed":
        sanitized = {"schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest, "result": "completed",
                     "stop_class": None, "diagnostic_category": None, "cpu_dimensions": None,
                     "c4_lowering_dimensions": None}
        full = {"schema_version": 1, "report_kind": "full", "rom_sha256": digest, "result": "completed",
                "runtime": runtime, "stop_class": None, "diagnostic_category": None,
                "c4_lowering_dimensions": None, "provenance": None}
        return sanitized, full
    assert kind == "boundary" and boundary_hex is not None
    sanitized = {"schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest, "result": "stop",
                 "stop_class": "discovery_prefix_boundary", "diagnostic_category": "discovery_budget_exhausted",
                 "cpu_dimensions": None, "c4_lowering_dimensions": None}
    provenance = {
        "has_instruction_provenance": True,
        "instruction": {"cpu_variant": "mc68000", "source_address": f"0x{boundary_hex}",
                        "image_offset": 0, "primary_bytes": "7000", "length": 2},
        "has_access": False, "access_address": "0x00000000", "access_width": None,
        "access_direction": None, "mapping_claim_count": 0, "mapping_claims": [],
        "bus_access_count": 0, "bus_accesses": [],
    }
    full = {"schema_version": 1, "report_kind": "full", "rom_sha256": digest, "result": "stop",
            "runtime": runtime, "stop_class": "discovery_prefix_boundary",
            "diagnostic_category": "discovery_budget_exhausted", "c4_lowering_dimensions": None,
            "provenance": provenance}
    return sanitized, full


def write_proxy(path: pathlib.Path) -> None:
    proxy = r'''#!/usr/bin/env python3
import json
import os
import re
import sys

if len(sys.argv) < 2 or sys.argv[1] != "emit-general-startup-bridge-c":
    raise SystemExit(2)
try:
    digest = sys.argv[sys.argv.index("--rom-sha256") + 1]
except (ValueError, IndexError):
    raise SystemExit(2)
if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
    raise SystemExit(2)
if "--reset-entry" not in sys.argv:
    raise SystemExit(2)
seeds = []
index = 0
while index < len(sys.argv):
    if sys.argv[index] == "--analysis-seed" and index + 1 < len(sys.argv):
        seeds.append(sys.argv[index + 1].lower())
        index += 2
    else:
        index += 1
key = ",".join(sorted(seeds))

with open(os.environ["GSB_TEST_SCENARIO"], encoding="utf-8") as handle:
    table = json.load(handle)
sanitized_text, full_text = table[key]

def c_literal(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

FILLER = "A" * 87382 + "=="
if FILLER in full_text:
    full_prefix, full_suffix = full_text.split(FILLER, 1)
else:
    full_prefix, full_suffix = full_text, ""

source = "\n".join([
    "#define _POSIX_C_SOURCE 200809L",  # fdopen/unistd under strict -std=c11 on glibc
    "#include <stdio.h>",
    "#include <stdlib.h>",
    "#include <string.h>",
    "#include <stdint.h>",
    "#if defined(_WIN32)",
    "#include <fcntl.h>",
    "#include <io.h>",
    "#endif",
    "",
    "/* Same contract as the generated bridge: the token is a POSIX fd, or on Windows an inherited HANDLE. */",
    "static FILE *open_inherited(const char *token) {",
    "  unsigned long long value = strtoull(token, NULL, 10);",
    "#if defined(_WIN32)",
    "  int fd = _open_osfhandle((intptr_t)(uintptr_t)value, _O_WRONLY | _O_BINARY);",
    "  FILE *stream = fd < 0 ? NULL : _fdopen(fd, \"wb\");",
    "  if (fd >= 0 && stream == NULL) _close(fd);",
    "  return stream;",
    "#else",
    "  return fdopen((int)value, \"w\");",
    "#endif",
    "}",
    "",
    "static const char *SANITIZED = " + c_literal(sanitized_text) + ";",
    "static const char *FULL_PREFIX = " + c_literal(full_prefix) + ";",
    "static const char *FULL_SUFFIX = " + c_literal(full_suffix) + ";",
    "",
    "int main(int argc, char **argv) {",
    "  FILE *full = NULL;",
    "  int index;",
    "  for (index = 1; index < argc; ++index) {",
    "    if (strcmp(argv[index], \"--full-report-fd\") == 0 && index + 1 < argc) {",
    "      full = open_inherited(argv[++index]);",
    "    } else if (strcmp(argv[index], \"--full-report-path\") == 0 && index + 1 < argc) {",
    "      full = fopen(argv[++index], \"w\");",
    "    }",
    "  }",
    "  fputs(SANITIZED, stdout);",
    "  fputc('\\n', stdout);",
    "  if (full != NULL) {",
    "    unsigned filler_index;",
    "    fputs(FULL_PREFIX, full);",
    "    for (filler_index = 0; filler_index < 87382U; ++filler_index) { fputc('A', full); }",
    "    fputs(\"==\", full);",
    "    fputs(FULL_SUFFIX, full);",
    "    fputc('\\n', full);",
    "    fclose(full);",
    "  }",
    "  return 0;",
    "}",
    "",
])
sys.stdout.write(source)
'''
    path.write_text(proxy)
    path.chmod(0o755)


def fail(name: str, message: str) -> int:
    sys.stderr.write(f"{name} failed: {message}\n")
    return 1


def test_automatic_continuation_across_multiple_batches(bridge, compiler: pathlib.Path, root: pathlib.Path) -> int:
    """SEG-007-T172 (ADR-0013 Decision §7c): `main`'s `--checkpoint` path must
    automatically resume across MORE THAN ONE Phase-B batch boundary within a
    SINGLE CLI invocation -- the caller never sees, or needs to notice,
    `phase_b_batch_complete` any more once `--checkpoint` is supplied.

    Narrows the per-invocation batch budget to 1 newly-promoted root / 1
    round (monkeypatched module attributes only, restored by the caller) so a
    scenario requiring FOUR total rounds to reach the genuine `completed`
    terminal needs four internal batches -- proving the automatic outer
    resume loop, not merely a single batch, drove this one `main()` call to
    the true terminal.
    """
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-auto-resume-") as directory:
        temporary = pathlib.Path(directory)
        proxy = temporary / "proxy.py"
        write_proxy(proxy)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-auto-resume-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()

        b1, b2, b3 = "00004100", "00004200", "00004300"
        scenario = {
            "": build_reports("boundary", digest, b1),
            b1: build_reports("boundary", digest, b2),
            ",".join(sorted([b1, b2])): build_reports("boundary", digest, b3),
            ",".join(sorted([b1, b2, b3])): build_reports("completed", digest),
        }
        scenario_path = temporary / "scenario.json"
        scenario_path.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                  json.dumps(v[1], separators=(",", ":"))]
                                             for k, v in scenario.items()}))
        checkpoint_path = temporary / "checkpoint.json"
        out_dir = temporary / "out"

        import os
        env = dict(os.environ)
        env["GSB_TEST_SCENARIO"] = str(scenario_path)

        original_seed_cap, original_round_cap = (bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES,
                                                   bridge.M68K_EXPANSION_MAX_ROUNDS)
        bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES = 1
        bridge.M68K_EXPANSION_MAX_ROUNDS = 1
        original_argv = sys.argv
        try:
            sys.argv = ["genesis_startup_bridge.py", "--segarecomp", str(proxy), "--cc", str(compiler),
                        "--rom", str(rom), "--mode", "commercial", "--diagnose-frontier",
                        "--out-dir", str(out_dir), "--checkpoint", str(checkpoint_path)]
            os.environ["GSB_TEST_SCENARIO"] = str(scenario_path)
            status = bridge.main()
        finally:
            sys.argv = original_argv
            bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES = original_seed_cap
            bridge.M68K_EXPANSION_MAX_ROUNDS = original_round_cap

        if status != 0:
            return fail("auto-resume", f"main() returned status={status}")
        final_seeds = bridge.load_checkpoint(checkpoint_path, digest, None)
        if final_seeds != [int(b1, 16), int(b2, 16), int(b3, 16)]:
            return fail("auto-resume", f"unexpected final persisted seeds: {final_seeds}")
    return 0


def test_checkpoint_seed_is_no_more_trusted_than_an_ordinary_analysis_seed(
        bridge, compiler: pathlib.Path, root: pathlib.Path) -> int:
    """SEG-007-T172 (ADR-0013 Decision §7c trust boundary): a checkpoint file
    is driver-owned generated build/session state, the SAME trust class as
    the existing in-memory `--analysis-seed` transport surface -- never an
    externally-audited input format. Persisting the confirmed-root set across
    invocations creates no NEW authority path: a hand-edited checkpoint
    carrying a "foreign" seed address that was never actually produced by a
    genuine `run_expansion_loop`/`save_checkpoint` extraction receives NO
    special weaker or stronger admission than supplying that exact same
    address as an ordinary in-process `--analysis-seed` in a single,
    non-checkpoint invocation -- both reach the frontend through the
    identical `--analysis-seed` transport and the identical
    `discover_m68k_static_graph`/`merge_root_result` admission path.
    """
    import contextlib
    import io
    import os

    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-trust-") as directory:
        temporary = pathlib.Path(directory)
        proxy = temporary / "proxy.py"
        write_proxy(proxy)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-trust-boundary-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()

        # A "foreign" address: never derived from any genuine boundary
        # extraction in this scenario (no scripted "" -> boundary(foreign)
        # transition exists at all) -- it is only ever hand-supplied below,
        # exactly as a hand-edited checkpoint or a hand-typed --analysis-seed
        # would supply one.
        foreign = "0000dead"
        scenario = {foreign: build_reports("completed", digest)}
        scenario_path = temporary / "scenario.json"
        scenario_path.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                  json.dumps(v[1], separators=(",", ":"))]
                                             for k, v in scenario.items()}))
        os.environ["GSB_TEST_SCENARIO"] = str(scenario_path)

        # Path B (baseline): the foreign address supplied as an ordinary
        # in-process seed to run_expansion_loop directly, no checkpoint file
        # involved at all -- today's existing --analysis-seed trust contract.
        emitter_command = [sys.executable, str(proxy), "emit-general-startup-bridge-c", "--rom", str(rom),
                           "--reset-entry", "--rom-sha256", digest]
        out_b = temporary / "ordinary-seed-route"
        (status_b, report_b, _full_b, _exe_b, _summary_b, message_b,
         seeds_b) = bridge.run_expansion_loop(emitter_command, compiler, root, out_b, digest, [int(foreign, 16)])
        if status_b:
            return fail("trust-boundary-ordinary", f"status={status_b} message={message_b!r}")

        # Path A: the identical foreign address supplied ONLY via a
        # hand-constructed checkpoint file -- save_checkpoint was called here
        # directly with a value this test invented, never with a value
        # genuinely extracted from a run_expansion_loop round's own output.
        checkpoint_path = temporary / "checkpoint.json"
        bridge.save_checkpoint(checkpoint_path, digest, None, [int(foreign, 16)])
        out_a = temporary / "checkpoint-route"
        original_argv = sys.argv
        captured_stdout = io.StringIO()
        try:
            sys.argv = ["genesis_startup_bridge.py", "--segarecomp", str(proxy), "--cc", str(compiler),
                        "--rom", str(rom), "--mode", "commercial", "--diagnose-frontier",
                        "--out-dir", str(out_a), "--checkpoint", str(checkpoint_path)]
            with contextlib.redirect_stdout(captured_stdout):
                status_a = bridge.main()
        finally:
            sys.argv = original_argv

        if status_a != 0:
            return fail("trust-boundary-checkpoint", f"main() returned status={status_a}")
        report_a = json.loads(captured_stdout.getvalue())

        if report_a != report_b or seeds_b != [int(foreign, 16)]:
            return fail("trust-boundary-mismatch",
                        f"checkpoint-sourced seed produced a different outcome than the identical "
                        f"ordinary --analysis-seed value: {report_a} vs {report_b}")
    return 0


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    _, compiler_arg, root_arg = sys.argv[1:]
    compiler, root = pathlib.Path(compiler_arg).resolve(), pathlib.Path(root_arg).resolve()
    bridge = load_bridge_module(root)

    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-checkpoint-") as directory:
        temporary = pathlib.Path(directory)
        proxy = temporary / "proxy.py"
        write_proxy(proxy)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-checkpoint-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()

        b1, b2, b3, b4 = "00003100", "00003200", "00003300", "00003400"
        scenario = {
            "": build_reports("boundary", digest, b1),
            b1: build_reports("boundary", digest, b2),
            ",".join(sorted([b1, b2])): build_reports("boundary", digest, b3),
            ",".join(sorted([b1, b2, b3])): build_reports("boundary", digest, b4),
            ",".join(sorted([b1, b2, b3, b4])): build_reports("completed", digest),
        }
        scenario_path = temporary / "scenario.json"
        scenario_path.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                  json.dumps(v[1], separators=(",", ":"))]
                                             for k, v in scenario.items()}))
        import os
        os.environ["GSB_TEST_SCENARIO"] = str(scenario_path)
        emitter_command = [sys.executable, str(proxy), "emit-general-startup-bridge-c", "--rom", str(rom),
                            "--reset-entry", "--rom-sha256", digest]

        # --- Reference: a single larger run at the real, unmodified batch
        # budget (8/8) reaches the eventual "completed" terminal directly.
        single_out = temporary / "single"
        (status, single_report, single_full, single_executable, single_summary, message,
         single_seeds) = bridge.run_expansion_loop(emitter_command, compiler, root, single_out, digest)
        if status:
            return fail("reference-run", f"status={status} message={message!r}")
        if single_report.get("result") != "completed" or single_seeds != [int(b1, 16), int(b2, 16),
                                                                            int(b3, 16), int(b4, 16)]:
            return fail("reference-run", f"unexpected report/seeds: {single_report} {single_seeds}")

        # --- Two-invocation resumption with a narrowed per-invocation batch
        # budget (2 newly-promoted roots / 3 rounds), never retuning the
        # production constants themselves (monkeypatched module attributes
        # only, restored below).
        original_seed_cap, original_round_cap = (bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES,
                                                   bridge.M68K_EXPANSION_MAX_ROUNDS)
        bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES = 2
        bridge.M68K_EXPANSION_MAX_ROUNDS = 3
        try:
            checkpoint_path = temporary / "checkpoint.json"
            out_1 = temporary / "invocation-1"
            (status, report_1, full_1, executable_1, summary_1, message,
             seeds_1) = bridge.run_expansion_loop(emitter_command, compiler, root, out_1, digest, [])
            if status:
                return fail("invocation-1", f"status={status} message={message!r}")
            if summary_1.get("driver_result") != bridge.PHASE_B_BATCH_COMPLETE or seeds_1 != [int(b1, 16), int(b2, 16)]:
                return fail("invocation-1", f"unexpected batch-exhaustion summary/seeds: {summary_1} {seeds_1}")
            bridge.save_checkpoint(checkpoint_path, digest, None, seeds_1)

            # Two independent resumptions from the identical persisted state
            # must be byte-identical (determinism).
            def resume(tag: str) -> tuple[dict, list[int], pathlib.Path]:
                loaded = bridge.load_checkpoint(checkpoint_path, digest, None)
                if loaded != seeds_1:
                    raise AssertionError(f"{tag} loaded unexpected seeds: {loaded}")
                out_dir = temporary / f"resume-{tag}"
                (status, report, full_bytes, executable, summary, message,
                 seeds) = bridge.run_expansion_loop(emitter_command, compiler, root, out_dir, digest, loaded)
                if status:
                    raise AssertionError(f"{tag} status={status} message={message!r}")
                return report, seeds, out_dir

            report_a, seeds_a, out_a = resume("a")
            report_b, seeds_b, out_b = resume("b")
            if report_a != report_b or seeds_a != seeds_b:
                return fail("resume-determinism", f"{report_a} {seeds_a} vs {report_b} {seeds_b}")
            if report_a.get("result") != "completed" or seeds_a != [int(b1, 16), int(b2, 16), int(b3, 16), int(b4, 16)]:
                return fail("resume-result", f"unexpected resumed report/seeds: {report_a} {seeds_a}")
            # The resumed multi-invocation result matches the single larger
            # reference run's own final seed set and terminal report.
            if seeds_a != single_seeds or report_a != single_report:
                return fail("resume-vs-reference", f"{report_a} {seeds_a} vs {single_report} {single_seeds}")
            source_a = (out_a / "round-3" / "bridge.generated.c").read_bytes()
            source_b = (out_b / "round-3" / "bridge.generated.c").read_bytes()
            if source_a != source_b:
                return fail("resume-determinism", "generated C differed across independent resumptions")

            # --- Fail-closed: corrupted / incompatible persisted state must
            # never silently apply to a different program/configuration.
            corrupt_path = temporary / "corrupt.json"
            corrupt_path.write_text("{not json")
            if bridge.load_checkpoint(corrupt_path, digest, None) != []:
                return fail("fail-closed-corrupt", "corrupted JSON was not rejected")

            wrong_sha_path = temporary / "wrong-sha.json"
            bridge.save_checkpoint(wrong_sha_path, "0" * 64, None, seeds_1)
            if bridge.load_checkpoint(wrong_sha_path, digest, None) != []:
                return fail("fail-closed-rom", "mismatched rom_sha256 was not rejected")

            wrong_hints_path = temporary / "wrong-hints.json"
            bridge.save_checkpoint(wrong_hints_path, digest, "/some/other/hints.json", seeds_1)
            if bridge.load_checkpoint(wrong_hints_path, digest, None) != []:
                return fail("fail-closed-hints", "mismatched hints binding was not rejected")
            if bridge.load_checkpoint(checkpoint_path, digest, "/some/other/hints.json") != []:
                return fail("fail-closed-hints-2", "checkpoint without hints binding was applied to a hints run")

            missing_field_path = temporary / "missing-field.json"
            missing_field_path.write_text(json.dumps({
                "schema_version": 1, "rom_sha256": digest, "seeds": [f"{b1}"],
            }))
            if bridge.load_checkpoint(missing_field_path, digest, None) != []:
                return fail("fail-closed-schema", "missing-field checkpoint was not rejected")
        finally:
            bridge.M68K_DISCOVERY_MAX_SEED_ENTRIES = original_seed_cap
            bridge.M68K_EXPANSION_MAX_ROUNDS = original_round_cap

    status = test_automatic_continuation_across_multiple_batches(bridge, compiler, root)
    if status:
        return status
    status = test_checkpoint_seed_is_no_more_trusted_than_an_ordinary_analysis_seed(bridge, compiler, root)
    if status:
        return status

    print("genesis startup bridge ADR-0013 Decision §7c checkpoint/resume: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
