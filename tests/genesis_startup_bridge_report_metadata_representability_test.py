#!/usr/bin/env python3
"""SEG-007-T073: `cpu_dimensions` is representable per actually-reached
``stop_class``, never per the whole compiled program.

Before this task, `genesis_valid_report_metadata` (`tools/
genesis_startup_bridge_runtime.c`) rejected an otherwise valid, already-
`genesis_valid_stop_pair`-conforming stop or completion whenever the
program's single whole-program-static `GenesisReportMetadata.cpu_dimensions`
(compiled from *any* `unsupported_cpu_form` frontier the emitter found
anywhere in the source program, independent of whether that frontier is ever
actually reached) happened to be non-`GENESIS_CPU_DIMENSIONS_NONE` while the
actually-reached stop was something else entirely -- including the
already-designed defensive `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY`
fallback `genesis_dispatch` produces for a PC outside every statically
discovered block. `genesis_write_sanitized_report`/`genesis_write_full_report`
then failed (returned nonzero) before ever emitting a report line, and a
generated bridge program's `main` returned 1 before writing anything --
exactly the `child_nonzero_before_report` shape SEG-007-T072 first observed
against the real Sonic ROM.

Two project-authored, non-ROM-derived proofs, both compiled and executed
under strict C11 against the real, unmodified `tools/
genesis_startup_bridge_runtime.c`:

- Part A drives the real generator/compile/execute/driver contract
  end to end (`tools/genesis_startup_bridge.py`, synthetic mode) with a
  hand-encoded MC68000 program that has two static exits from one
  conditional branch: an unreached RESET (`unsupported_cpu_form`) frontier,
  which is what the compiled `GENESIS_BRIDGE_REPORT_METADATA` is derived
  from, and a `MOVE.L (A0),D1` whose runtime-routed, non-foldable ROM read
  (A0 defaults to 0) actually reaches `GENESIS_STOP_INTERNAL_DISPATCH_
  INCONSISTENCY` -- the driver's default initial state (`SR == 0`) makes
  the branch outcome, and therefore which frontier is actually reached,
  deterministic. Before this task's fix, this combination made the compiled
  program exit nonzero before writing any report line; after it, both
  production routes succeed with a canonical, schema-valid sanitized report
  and `cpu_dimensions: null`, regardless of the unrelated compiled static
  value.
- Part B directly proves the two adjacent library-level shapes precisely
  through the real, unmodified `genesis_write_sanitized_report`/
  `genesis_write_full_report` API (`tools/genesis_startup_bridge_runtime.h`),
  compiled and executed under strict C11: (1) the exact previously-
  rejected/now-accepted shape from the backlog's own hypothesis (an
  `internal_dispatch_inconsistency` stop with a compiled `move_to_usp`
  static value) and the completion shape, both now representable; and (2)
  the nearby genuinely invalid/unrepresentable shape -- an actually-reached
  `unsupported_cpu_form` stop with no compiled `cpu_dimensions` at all --
  which must still fail closed exactly as before, with no new
  false-positive acceptance.
"""
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# BNE.S +2 ; RESET ; MOVE.L (A0),D1 ; RESET
#
# Static discovery retains both of the BNE's continuations as separate
# frontiers: the fallthrough RESET (an `unsupported_cpu_form` frontier the
# emitter finds, even though it is never the frontier actually reached by
# this program's own default runtime state) and the taken branch's
# MOVE.L (A0),D1, a register-indirect source that C4 cannot statically fold,
# so it is routed through `genesis_route_access` at runtime. The generated
# bridge program's `main` always starts with every register, including A0,
# zero-initialized, so that routed read always targets ROM address 0 --
# below `genesis_route_access`'s own `0x00400000` boundary -- which
# unconditionally resolves to `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY`
# for a READ direction (see `tools/genesis_startup_bridge_runtime.c`), the
# exact defensive stop class this task's fix concerns. The trailing RESET is
# never reached by this program's own deterministic default execution; it
# only ensures the taken-branch block itself is well-formed for discovery.
PROGRAM = bytes((0x66, 0x02, 0x4E, 0x70, 0x22, 0x10, 0x4E, 0x70))
ENTRY = "00000b00"

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

static char SHA_ZERO[65];

int main(int argc, char **argv) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer stop_transfer = {0};
  GenesisControlTransfer complete_transfer = {0};
  GenesisControlTransfer rejected_transfer = {0};
  GenesisControlTransfer c4_transfer = {0};
  /* Simulates a compiled `GenesisReportMetadata` derived from an unrelated,
     never-reached `unsupported_cpu_form` frontier elsewhere in the same
     program -- exactly the backlog's own hypothesis shape. */
  GenesisReportMetadata unrelated_metadata = {0};
  GenesisReportMetadata none_metadata = {0};
  FILE *fixed_full;
  FILE *rejected_full;
  long rejected_size;

  if (argc != 3) return 2;
  memset(SHA_ZERO, '0', 64U); SHA_ZERO[64] = '\0';
  unrelated_metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_MOVE_AN_TO_USP;
  none_metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_NONE;

  /* Previously-rejected, now-representable: an already-well-formed
     GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY stop, independent of the
     unrelated compiled static cpu_dimensions value. */
  stop_transfer.kind = GENESIS_STOP;
  stop_transfer.stop.stop_class = GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY;
  stop_transfer.stop.diagnostic_category = GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY;
  assert(genesis_write_sanitized_report(&stop_transfer, SHA_ZERO, &unrelated_metadata) == 0);

  /* Nearby genuinely invalid/unrepresentable twin: an ACTUALLY-reached
     unsupported_cpu_form stop with no compiled cpu_dimensions at all must
     still fail closed, with no report line printed, exactly as before this
     task's fix. */
  rejected_transfer.kind = GENESIS_STOP;
  rejected_transfer.stop.stop_class = GENESIS_STOP_UNSUPPORTED_CPU_FORM;
  rejected_transfer.stop.diagnostic_category = GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION;
  assert(genesis_write_sanitized_report(&rejected_transfer, SHA_ZERO, &none_metadata) == 1);

  /* ADR-0015 Q7: C4 dimensions are stop-owned.  The valid pair is accepted;
     every adjacent invalid combination fails before a report is written. */
  c4_transfer.kind = GENESIS_STOP;
  c4_transfer.stop.stop_class = GENESIS_STOP_C4_LOWERING_GAP;
  c4_transfer.stop.diagnostic_category = GENESIS_DIAG_C4_LOWERING_GAP;
  c4_transfer.stop.c4_lowering_dimensions = GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_MISSING_DISPATCHER;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 0);
  c4_transfer.stop.c4_lowering_dimensions = GENESIS_C4_LOWERING_DIMENSIONS_NONE;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 1);
  c4_transfer.stop.c4_lowering_dimensions = (GenesisC4LoweringDimensions)99;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 1);
  c4_transfer.stop.c4_lowering_dimensions = GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_MISSING_DISPATCHER;
  c4_transfer.stop.diagnostic_category = GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 1);
  c4_transfer.stop.diagnostic_category = GENESIS_DIAG_C4_LOWERING_GAP;
  none_metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_NOP;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 1);
  none_metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_NONE;
  c4_transfer.stop.stop_class = GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY;
  assert(genesis_write_sanitized_report(&c4_transfer, SHA_ZERO, &none_metadata) == 1);

  /* A completion is also representable regardless of any unrelated
     compiled static cpu_dimensions value. */
  complete_transfer.kind = GENESIS_COMPLETE;
  assert(genesis_write_sanitized_report(&complete_transfer, SHA_ZERO, &unrelated_metadata) == 0);

  /* The same fixed/rejected pair through the full-report writer, since
     SEG-007-T072's own reproduction reached this same gate first via the
     `--full-report-fd` path before the sanitized writer ever ran. */
  fixed_full = fopen(argv[1], "w");
  assert(fixed_full != NULL);
  assert(genesis_write_full_report(fixed_full, &runtime, &stop_transfer, SHA_ZERO, &unrelated_metadata) == 0);
  assert(fclose(fixed_full) == 0);

  rejected_full = fopen(argv[2], "w");
  assert(rejected_full != NULL);
  assert(genesis_write_full_report(rejected_full, &runtime, &rejected_transfer, SHA_ZERO, &none_metadata) == 1);
  assert(fclose(rejected_full) == 0);
  rejected_full = fopen(argv[2], "r");
  assert(rejected_full != NULL);
  assert(fseek(rejected_full, 0, SEEK_END) == 0);
  rejected_size = ftell(rejected_full);
  assert(rejected_size == 0);
  assert(fclose(rejected_full) == 0);

  assert(printf("HARNESS_DONE\n") > 0);
  return 0;
}
'''


def run(command, cwd=None):
    return subprocess.run(command, text=True, capture_output=True, cwd=cwd, check=False)


def check_canonical(line: str, expected: dict) -> bool:
    import json
    if not line.endswith("\n"):
        return False
    try:
        value = json.loads(line)
    except json.JSONDecodeError:
        return False
    return (list(value) == list(expected) and value == expected and
            __import__("json").dumps(value, separators=(",", ":"), ensure_ascii=False) == line[:-1])


def part_a(binary: pathlib.Path, compiler: pathlib.Path, root: pathlib.Path) -> list:
    failures = []
    digest = hashlib.sha256(PROGRAM).hexdigest()
    driver = root / "tools" / "genesis_startup_bridge.py"
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        rom = temporary / "program.bin"
        rom.write_bytes(PROGRAM)
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-t073-") as output:
            output_dir = pathlib.Path(output)
            full = temporary / "full.json"
            result = run([sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                          "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic",
                          "--out-dir", str(output_dir), "--full-report-path", str(full)], root)
            source_text = (output_dir / "bridge.generated.c").read_text() if (output_dir / "bridge.generated.c").exists() else ""
            # The whole-program-static compiled metadata really is derived from
            # the unreached RESET frontier, not the actually-reached stop -- this
            # is the exact shape this task's fix must tolerate.
            if "GENESIS_CPU_DIMENSIONS_RESET" not in source_text:
                failures.append("compiled-metadata-not-reset")
            if result.returncode != 0 or result.stderr:
                failures.append(f"driver-nonzero:{result.returncode}:{result.stderr!r}")
                return failures
            expected_sanitized = {
                "schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
                "result": "stop", "stop_class": "internal_dispatch_inconsistency",
                "diagnostic_category": "internal_dispatch_inconsistency", "cpu_dimensions": None,
                "c4_lowering_dimensions": None,
            }
            if result.stdout != __import__("json").dumps(expected_sanitized, separators=(",", ":")) + "\n":
                failures.append(f"sanitized-mismatch:{result.stdout!r}")
            if not full.exists():
                failures.append("full-report-missing")
            else:
                import json
                full_report = json.loads(full.read_text().rstrip("\n"))
                if (full_report.get("result") != "stop" or
                        full_report.get("stop_class") != "internal_dispatch_inconsistency" or
                        full_report.get("diagnostic_category") != "internal_dispatch_inconsistency" or
                        full_report.get("rom_sha256") != digest):
                    failures.append(f"full-report-mismatch:{full_report!r}")
    return failures


def part_b(compiler: pathlib.Path, root: pathlib.Path) -> list:
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / "harness.c").write_text(HARNESS)
        fixed_full = path / "fixed-full.json"
        rejected_full = path / "rejected-full.json"
        build = run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                      "-I", str(root / "platforms" / "genesis" / "runtime"), str(path / "harness.c"),
                      str(root / "platforms" / "genesis" / "runtime" / "runtime.c"),
                     "-o", str(path / "report-metadata")])
        if build.returncode != 0:
            failures.append(f"build-failed:{build.stderr!r}")
            return failures
        ran = run([str(path / "report-metadata"), str(fixed_full), str(rejected_full)])
        if ran.returncode != 0:
            failures.append(f"harness-nonzero:{ran.returncode}:{ran.stderr!r}")
            return failures
        lines = ran.stdout.splitlines(keepends=True)
        if len(lines) != 4:
            failures.append(f"unexpected-stdout-line-count:{ran.stdout!r}")
            return failures
        if not check_canonical(lines[0], {
            "schema_version": 1, "report_kind": "sanitized",
            "rom_sha256": "0" * 64, "result": "stop",
            "stop_class": "internal_dispatch_inconsistency",
            "diagnostic_category": "internal_dispatch_inconsistency", "cpu_dimensions": None,
            "c4_lowering_dimensions": None,
        }):
            failures.append(f"line1-mismatch:{lines[0]!r}")
        if not check_canonical(lines[1], {
            "schema_version": 1, "report_kind": "sanitized", "rom_sha256": "0" * 64,
            "result": "stop", "stop_class": "c4_lowering_gap", "diagnostic_category": "c4_lowering_gap",
            "cpu_dimensions": None, "c4_lowering_dimensions": {"family": "compare_missing_dispatcher"},
        }):
            failures.append(f"line2-mismatch:{lines[1]!r}")
        if not check_canonical(lines[2], {
            "schema_version": 1, "report_kind": "sanitized", "rom_sha256": "0" * 64,
            "result": "completed", "stop_class": None, "diagnostic_category": None, "cpu_dimensions": None,
            "c4_lowering_dimensions": None,
        }):
            failures.append(f"line3-mismatch:{lines[2]!r}")
        if lines[3] != "HARNESS_DONE\n":
            failures.append(f"line4-mismatch:{lines[3]!r}")
        if not fixed_full.exists() or fixed_full.stat().st_size == 0:
            failures.append("fixed-full-report-missing-or-empty")
        if not rejected_full.exists() or rejected_full.stat().st_size != 0:
            failures.append("rejected-full-report-not-empty")
    return failures


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(argument).resolve() for argument in sys.argv[1:])
    failures = part_a(binary, compiler, root) + part_b(compiler, root)
    if failures:
        sys.stderr.write("genesis startup bridge report-metadata representability regression failed: " +
                          ", ".join(failures) + "\n")
        return 1
    print("genesis startup bridge report-metadata representability: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
