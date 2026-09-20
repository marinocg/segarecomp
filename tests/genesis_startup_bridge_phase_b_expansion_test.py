#!/usr/bin/env python3
"""ADR 0013 Decision §7 Phase B: the driver-owned build/execute/expand loop.

Project-authored synthetic driver-level fixture, no commercial input. A
scripted `segarecomp`-shaped proxy (following the existing C6/C7 driver test
pattern) stands in for the real emitter/compiler pipeline so this test
exercises `tools/genesis_startup_bridge.py`'s own round-management, private
boundary-address extraction, seed promotion, and terminal-result selection
logic in isolation from CPU discovery correctness (which the C++ unit/
integration suite already covers separately).

The proxy's own emitted "generated C" is a tiny, real, strict-C11-compilable
program that just prints one pre-baked canonical sanitized/full report pair,
selected deterministically by the exact ordered seed set the driver passed on
that round's `--analysis-seed` arguments (env `GSB_TEST_SCENARIO`, a JSON map
keyed by the sorted, comma-joined seed hex list). Every invocation's own seed
set is also logged (env `GSB_TEST_LOG`) so this test can assert on the
driver's actual round-by-round seed promotion, never merely on its final
output.
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def c_string_literal(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


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
    if kind == "boundary":
        assert boundary_hex is not None
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
    if kind == "runner_resource_limit":
        # SEG-007-T252 / ADR-0040: a runner-resource-limit exhaustion is a
        # sibling top-level "result" value, never nested under stop_class /
        # diagnostic_category (both null, exactly like "completed").
        sanitized = {"schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
                     "result": "runner_resource_limit", "stop_class": None, "diagnostic_category": None,
                     "cpu_dimensions": None, "c4_lowering_dimensions": None, "runner_dispatch_count": 128}
        full = {"schema_version": 1, "report_kind": "full", "rom_sha256": digest,
                "result": "runner_resource_limit", "runtime": runtime, "stop_class": None,
                "diagnostic_category": None, "c4_lowering_dimensions": None, "provenance": None,
                "runner_dispatch_count": 128}
        return sanitized, full
    if kind == "other_stop":
        sanitized = {"schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest, "result": "stop",
                     "stop_class": "instruction_budget_exhausted",
                     "diagnostic_category": "instruction_budget_exhausted",
                     "cpu_dimensions": None, "c4_lowering_dimensions": None}
        provenance = {
            "has_instruction_provenance": False, "instruction": None, "has_access": False,
            "access_address": "0x00000000", "access_width": None, "access_direction": None,
            "mapping_claim_count": 0, "mapping_claims": [], "bus_access_count": 0, "bus_accesses": [],
        }
        full = {"schema_version": 1, "report_kind": "full", "rom_sha256": digest, "result": "stop",
                "runtime": runtime, "stop_class": "instruction_budget_exhausted",
                "diagnostic_category": "instruction_budget_exhausted", "c4_lowering_dimensions": None,
                "provenance": provenance}
        return sanitized, full
    raise ValueError(kind)


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

log_path = os.environ.get("GSB_TEST_LOG")
if log_path:
    with open(log_path, "a", encoding="utf-8") as handle:
        handle.write(key + "\n")

def c_literal(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

# A 65536-byte work_ram_base64 filler is 87384 characters -- past the ISO C99
# minimum maximum string-literal length (4095) strict -pedantic compilation
# requires. Split the full report's own already-canonical JSON text around
# its one fixed filler run and print that run with a small runtime loop
# instead of embedding it as a literal, exactly like the sibling C6/C7
# driver tests' own proxy programs do.
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


def run_driver(driver: pathlib.Path, proxy: pathlib.Path, compiler: pathlib.Path, rom: pathlib.Path,
               root: pathlib.Path, out_dir: pathlib.Path, scenario_path: pathlib.Path,
               log_path: pathlib.Path) -> subprocess.CompletedProcess[str]:
    import os
    env = dict(os.environ)
    env["GSB_TEST_SCENARIO"] = str(scenario_path)
    env["GSB_TEST_LOG"] = str(log_path)
    return subprocess.run([
        sys.executable, str(driver), "--segarecomp", str(proxy), "--cc", str(compiler),
        "--rom", str(rom), "--mode", "commercial", "--diagnose-frontier", "--out-dir", str(out_dir)],
        text=True, capture_output=True, cwd=root, env=env)


def expansion_summary(stderr: str) -> dict | None:
    for line in stderr.splitlines():
        if line.startswith("EXPANSION_SUMMARY "):
            return json.loads(line[len("EXPANSION_SUMMARY "):])
    return None


def fail(name: str, result: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(f"{name} failed: rc={result.returncode}\nSTDOUT={result.stdout!r}\nSTDERR={result.stderr!r}\n")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    _, compiler_arg, root_arg = sys.argv[1:]
    compiler, root = pathlib.Path(compiler_arg).resolve(), pathlib.Path(root_arg).resolve()
    driver = root / "tools" / "genesis_startup_bridge.py"

    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-phase-b-") as directory:
        temporary = pathlib.Path(directory)
        proxy = temporary / "proxy.py"
        write_proxy(proxy)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-phase-b-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()

        # Case 1: positive two-round expansion. Round 1 (no seeds) reaches a
        # boundary at A; round 2's seed set is exactly {reset, A} (encoded
        # here as the single --analysis-seed A) and reaches a genuinely
        # different real frontier (here: completion).
        boundary_a = "00002000"
        scenario_1 = {
            "": build_reports("boundary", digest, boundary_a),
            boundary_a: build_reports("completed", digest),
        }
        scenario_path = temporary / "scenario-1.json"
        scenario_path.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                  json.dumps(v[1], separators=(",", ":"))]
                                             for k, v in scenario_1.items()}))
        log_path = temporary / "log-1.txt"
        out_dir_1 = temporary / "out-1"
        result = run_driver(driver, proxy, compiler, rom, root, out_dir_1, scenario_path, log_path)
        if result.returncode != 0:
            return fail("case1", result)
        summary = expansion_summary(result.stderr)
        if summary != {"rounds": 2, "driver_result": "completed", "seed_count": 2}:
            sys.stderr.write(f"case1 unexpected summary: {summary}\n")
            return 1
        report = json.loads(result.stdout)
        if report.get("result") != "completed":
            sys.stderr.write(f"case1 unexpected final report: {report}\n")
            return 1
        rounds_seen = log_path.read_text().splitlines()
        if rounds_seen != ["", boundary_a]:
            sys.stderr.write(f"case1 unexpected round seed log: {rounds_seen}\n")
            return 1
        if boundary_a in result.stdout or boundary_a in result.stderr:
            sys.stderr.write("case1 leaked the boundary address into durable/diagnostic output\n")
            return 1

        # Determinism: two byte-identical full-loop runs (same final seed
        # set/terminal result; per-round generated C is byte-identical since
        # this proxy's own output is a pure function of the seed set).
        log_path_2 = temporary / "log-1b.txt"
        out_dir_1b = temporary / "out-1b"
        result_2 = run_driver(driver, proxy, compiler, rom, root, out_dir_1b, scenario_path, log_path_2)
        if result_2.returncode != 0 or result_2.stdout != result.stdout:
            return fail("case1-determinism", result_2)
        if expansion_summary(result_2.stderr) != summary:
            sys.stderr.write("case1 two full-loop runs produced different terminal summaries\n")
            return 1
        for round_number in (1, 2):
            source_1 = (out_dir_1 / f"round-{round_number}" / "bridge.generated.c").read_bytes()
            source_2 = (out_dir_1b / f"round-{round_number}" / "bridge.generated.c").read_bytes()
            if source_1 != source_2:
                sys.stderr.write(f"case1 round {round_number} generated C differed across full-loop runs\n")
                return 1

        # Case 2: duplicate-seed halts with expansion_no_progress. Round 1
        # reaches boundary A; round 2 (seed {A}) reaches the SAME boundary A
        # again, which is already a member of the seed set.
        scenario_2 = {
            "": build_reports("boundary", digest, boundary_a),
            boundary_a: build_reports("boundary", digest, boundary_a),
        }
        scenario_path_2 = temporary / "scenario-2.json"
        scenario_path_2.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                    json.dumps(v[1], separators=(",", ":"))]
                                               for k, v in scenario_2.items()}))
        log_path_2b = temporary / "log-2.txt"
        result = run_driver(driver, proxy, compiler, rom, root, temporary / "out-2", scenario_path_2, log_path_2b)
        if result.returncode != 0:
            return fail("case2", result)
        summary = expansion_summary(result.stderr)
        if summary != {"rounds": 2, "driver_result": "expansion_no_progress", "seed_count": 2}:
            sys.stderr.write(f"case2 unexpected summary: {summary}\n")
            return 1

        # Case 3: the per-invocation round budget (m68k_expansion_max_rounds,
        # 8) is exhausted while the final round still confirms a new,
        # distinct boundary worth continuing from -- ADR-0013 Decision §7c's
        # resumable phase_b_batch_complete outcome, not a lifetime cap. Eight
        # rounds, each reaching a new, distinct boundary; round 8 confirms an
        # 8th newly-promoted boundary (b8) but the loop's own round budget
        # ends before a 9th round could run.
        b1, b2, b3, b4, b5, b6, b7, b8 = (
            "00002100", "00002200", "00002300", "00002400",
            "00002500", "00002600", "00002700", "00002800",
        )
        scenario_3 = {
            "": build_reports("boundary", digest, b1),
            b1: build_reports("boundary", digest, b2),
            ",".join(sorted([b1, b2])): build_reports("boundary", digest, b3),
            ",".join(sorted([b1, b2, b3])): build_reports("boundary", digest, b4),
            ",".join(sorted([b1, b2, b3, b4])): build_reports("boundary", digest, b5),
            ",".join(sorted([b1, b2, b3, b4, b5])): build_reports("boundary", digest, b6),
            ",".join(sorted([b1, b2, b3, b4, b5, b6])): build_reports("boundary", digest, b7),
            ",".join(sorted([b1, b2, b3, b4, b5, b6, b7])): build_reports("boundary", digest, b8),
        }
        scenario_path_3 = temporary / "scenario-3.json"
        scenario_path_3.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                    json.dumps(v[1], separators=(",", ":"))]
                                               for k, v in scenario_3.items()}))
        log_path_3 = temporary / "log-3.txt"
        result = run_driver(driver, proxy, compiler, rom, root, temporary / "out-3", scenario_path_3, log_path_3)
        if result.returncode != 0:
            return fail("case3", result)
        summary = expansion_summary(result.stderr)
        if summary != {"rounds": 8, "driver_result": "phase_b_batch_complete", "seed_count": 9}:
            sys.stderr.write(f"case3 unexpected summary: {summary}\n")
            return 1

        # Case 4: a non-boundary stop at round 1 ends the loop immediately,
        # with no seed ever promoted (exactly one build, with zero seeds --
        # this is also the structural guarantee that a non-runtime_confirmed
        # (e.g. static_only) address is never promoted: the driver only ever
        # extracts an address from a discovery_prefix_boundary stop).
        scenario_4 = {"": build_reports("other_stop", digest)}
        scenario_path_4 = temporary / "scenario-4.json"
        scenario_path_4.write_text(json.dumps({k: [json.dumps(v[0], separators=(",", ":")),
                                                    json.dumps(v[1], separators=(",", ":"))]
                                               for k, v in scenario_4.items()}))
        log_path_4 = temporary / "log-4.txt"
        result = run_driver(driver, proxy, compiler, rom, root, temporary / "out-4", scenario_path_4, log_path_4)
        if result.returncode != 0:
            return fail("case4", result)
        summary = expansion_summary(result.stderr)
        if summary != {"rounds": 1, "driver_result": "instruction_budget_exhausted", "seed_count": 1}:
            sys.stderr.write(f"case4 unexpected summary: {summary}\n")
            return 1
        if log_path_4.read_text().splitlines() != [""]:
            sys.stderr.write("case4 promoted a seed after a non-boundary stop\n")
            return 1

    print("genesis startup bridge ADR-0013 Decision §7 Phase B expansion loop: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
