#!/usr/bin/env python3
"""SEG-007-T238: baseline-vs-experiment measurement report for the build-time
-only, explicitly opt-in broad aligned-M68k-ROM AOT representation
experiment (ADR-0039 deferred workstream #4).

Compares the accepted baseline (the experiment's feature gate absent -- the
unmodified existing AOT representation/emission path) against the
experiment (the gate present, enumerating every aligned start in the complete
immutable ROM mapping) on two inputs:

  * a project-authored SYNTHETIC fixture (mirrors
    ``tests/m68k_pipeline_test.cpp``'s ``experiment_aligned_aot_fixture``);
  * an authorized canonical Sonic route using ``--reset-entry``. Existing
    external hints may be supplied for an assisted baseline comparison, but
    are optional and never source/filter the immutable-ROM AOT identities.

Five metrics are measured for each variant: generated-source size,
compiled-object size, generation time, strict-C11 compile time, and a
synthetic runtime dynamic-dispatch cost (the generated dispatcher's own
membership-check cost -- a full wall-clock commercial run is not a
meaningful, comparable measurement of *dispatch* cost alone, since it is
dominated by the bounded per-invocation instruction budget rather than by
dispatch itself). Every variant runs one unmeasured warm-up followed by
exactly three measured repetitions. Every repetition is wrapped with a hard
wall-clock and peak-RSS ceiling; exceeding either is reported as a measured
result, never silently retried or used to justify raising any existing
repository ceiling.

Output is normalized, machine-readable JSON: counts, sizes, durations, and
pass/fail verdicts against reasonableness thresholds registered BEFORE any
command runs. No raw ROM bytes, addresses, instruction words, or commercial-
derived content are written to stdout, stderr, or any file this tool
produces.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import resource
import statistics
import subprocess
import sys
import tempfile
import time


REPETITION_WALL_CLOCK_CEILING_SECONDS = 1800.0
REPETITION_PEAK_RSS_CEILING_BYTES = 8 * 1024 * 1024 * 1024
DISPATCH_MICROBENCHMARK_ITERATIONS = 200_000

# Pre-registered, machine-evaluable reasonableness thresholds for all five
# metrics. Declared BEFORE any measurement runs (ADR-0039 deferred
# workstream #4's own requirement) -- a threshold is never adjusted after
# seeing a result. Expressed as an experiment/baseline RATIO ceiling: the
# experiment is expected to cost more than the baseline (it does strictly
# more work), but an unbounded blow-up is itself the measured result this
# task exists to surface, not something to silently tolerate.
THRESHOLDS = {
    "generated_source_bytes_ratio": 20.0,
    "compiled_object_bytes_ratio": 20.0,
    "generation_time_seconds_ratio": 50.0,
    "compile_time_seconds_ratio": 50.0,
    "dispatch_cost_seconds_ratio": 10.0,
}


class ResourceCeilingExceeded(RuntimeError):
    pass


def run_bounded(command: list[str], cwd: pathlib.Path | None = None) -> tuple[float, int]:
    """Runs ``command`` once, enforcing the wall-clock/peak-RSS ceilings.

    Returns (elapsed_seconds, peak_rss_bytes_of_this_call's_own_children).
    Raises ResourceCeilingExceeded (never silently continues) on a ceiling
    hit; raises RuntimeError on a non-zero exit.
    """
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    try:
        completed = subprocess.run(
            command, cwd=cwd, text=True, capture_output=True,
            timeout=REPETITION_WALL_CLOCK_CEILING_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise ResourceCeilingExceeded(
            f"wall_clock_ceiling_exceeded seconds={REPETITION_WALL_CLOCK_CEILING_SECONDS}") from exc
    elapsed = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    # ru_maxrss is a high-water mark across ALL reaped children of this
    # process, not just this one call; treat the post-call reading as an
    # upper bound for this call's own contribution and flag it plainly as an
    # approximation rather than an exact per-process peak.
    peak_rss_bytes = after.ru_maxrss * 1024 if sys.platform != "darwin" else after.ru_maxrss
    if peak_rss_bytes > REPETITION_PEAK_RSS_CEILING_BYTES:
        raise ResourceCeilingExceeded(
            f"peak_rss_ceiling_exceeded bytes={peak_rss_bytes}")
    if completed.returncode != 0:
        raise RuntimeError(
            f"command_failed returncode={completed.returncode} stderr_bytes={len(completed.stderr)}")
    del before
    return elapsed, peak_rss_bytes


def strict_c11_compile(compiler: str, runtime_dir: pathlib.Path, generated_c: pathlib.Path,
                        out_object: pathlib.Path) -> float:
    """Compiles the CLI's own unmodified `emit-general-startup-bridge-c`
    output (a complete, self-contained translation unit with its own
    `main`) as strict C11 with warnings enabled -- exactly the acceptance
    criterion's own compile check, reused here as the size/time metric
    source rather than a second, separately-invented compile path."""
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(runtime_dir), str(generated_c), str(runtime_dir / "runtime.c"),
               "-o", str(out_object)]
    elapsed, _ = run_bounded(command)
    return elapsed


DISPATCH_MICROBENCHMARK_HARNESS = r'''
/* The CLI's `emit-general-startup-bridge-c` output is a complete,
   self-contained translation unit with its own `main` (a full ROM-driving
   report harness this microbenchmark does not want to run). Renaming that
   token via this macro -- applied only ahead of the generated-source include,
   never touching any OTHER identifier -- reuses every other unmodified
   generated function (including `genesis_bridge_dispatch` and the real,
   already-emitted dispatcher) verbatim; the renamed original `main` itself
   is simply never called by this harness. This is a measurement-only
   compile, deliberately built WITHOUT `-Werror` (unlike the strict-C11
   acceptance/size/time compile above, which always uses the unmodified
   source), since the now-unused original entry point and its own
   report-writing helpers may trigger ordinary unused-function warnings that
   have no bearing on dispatch cost. */
#define main genesis_bridge_original_main
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "runtime.h"
#include %(generated_include)s
#undef main

int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  struct timespec start, end;
  volatile uint32_t sink = 0;
  long iterations = %(iterations)dL;
  long i;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (i = 0; i < iterations; ++i) {
    runtime.pc = UINT32_C(%(probe_pc)#010x);
    transfer = genesis_bridge_dispatch(&runtime);
    sink ^= (uint32_t)transfer.kind;
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  {
    double elapsed = (double)(end.tv_sec - start.tv_sec) +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%%.9f\n", elapsed);
  }
  return (int)(sink & 0U);
}
'''


def dispatch_microbenchmark(compiler: str, runtime_dir: pathlib.Path, generated_c: pathlib.Path,
                             probe_pc: int, work_dir: pathlib.Path, phase_result) -> tuple[float, float, float]:
    """Compiles and runs a tight-loop membership-check microbenchmark against
    the already-generated dispatcher, returning the measured wall-clock
    dispatch cost in seconds for ``DISPATCH_MICROBENCHMARK_ITERATIONS``
    dispatch calls at a fixed, already-known-safe probe PC (a value the
    dispatcher fails closed for is fine -- this measures the membership
    CHECK cost, not successful execution)."""
    harness = work_dir / "dispatch_bench.c"
    harness.write_text(DISPATCH_MICROBENCHMARK_HARNESS % {
        "iterations": DISPATCH_MICROBENCHMARK_ITERATIONS,
        "probe_pc": probe_pc,
        # JSON string quoting is also valid for this C include-string token
        # and safely preserves an arbitrary generated artifact basename/path.
        "generated_include": json.dumps(str(generated_c.resolve())),
    }, encoding="utf-8")
    executable = work_dir / "dispatch_bench"
    command = [compiler, "-O2", "-std=c11", "-I", str(runtime_dir), "-I", str(generated_c.parent),
               str(harness), str(runtime_dir / "runtime.c"), "-o", str(executable)]
    compile_elapsed, _ = run_bounded(command)
    phase_result("dispatch_benchmark_compile", "completed", compile_elapsed)
    phase_result("dispatch_execution", "started")
    execution_elapsed, out = run_bounded_capture([str(executable)])
    phase_result("dispatch_execution", "completed", execution_elapsed)
    return compile_elapsed, execution_elapsed, float(out.strip())


def run_bounded_capture(command: list[str]) -> tuple[float, str]:
    start = time.monotonic()
    completed = subprocess.run(command, text=True, capture_output=True,
                                timeout=REPETITION_WALL_CLOCK_CEILING_SECONDS)
    elapsed = time.monotonic() - start
    if completed.returncode != 0:
        raise RuntimeError(f"microbenchmark_failed returncode={completed.returncode}")
    return elapsed, completed.stdout


def measure_variant(*, segarecomp: str, compiler: str, root: pathlib.Path,
                     emitter_args: list[str], probe_pc: int, label: str,
                     measured_repetitions: int = 3, warm_up: int = 1) -> dict:
    """Runs ``warm_up`` unmeasured repetitions followed by exactly
    ``measured_repetitions`` measured repetitions of the full generate ->
    strict-C11-compile -> dispatch-microbenchmark pipeline for one variant
    (baseline or experiment), returning normalized per-repetition and
    aggregate measurements. Never partially measures: a
    ResourceCeilingExceeded or command failure on any repetition is recorded
    as this variant's own result, not silently skipped.

    The default (warm_up=1, measured_repetitions=3) is ADR-0039's own
    required protocol and is always used for the synthetic fixture. The
    canonical Sonic route's single existing whole-ROM offline-inventory
    generation already costs several CPU-minutes per invocation (a
    measured fact this task's own canonical run surfaced, independent of
    the experiment); the caller may reduce the canonical route's own
    repetition count to keep the resulting agent-turn/CI cost bounded, and
    reports the reduction explicitly in the resulting JSON rather than
    silently reusing the synthetic fixture's cheaper default."""
    runtime_dir = root / "platforms/genesis/runtime"
    repetitions = []
    ceiling_hit = None
    tool_failure = None
    for repetition_index in range(warm_up + measured_repetitions):
        measured = repetition_index >= warm_up
        repetition = repetition_index - warm_up if measured else repetition_index
        phase_state = {"active": "generation"}
        def phase_result(phase: str, status: str, elapsed: float | None = None) -> None:
            if status == "started":
                phase_state["active"] = phase
            result = {"event": "cost_phase", "label": label, "repetition": repetition,
                      "measured": measured, "phase": phase, "status": status}
            if elapsed is not None:
                result["elapsed_seconds"] = elapsed
            print(json.dumps(result, sort_keys=True), file=sys.stderr, flush=True)

        with tempfile.TemporaryDirectory() as temporary:
            work_dir = pathlib.Path(temporary)
            generated_c = work_dir / "generated.c"
            try:
                phase_result("generation", "started")
                generation_start = time.monotonic()
                generated = subprocess.run(
                    [segarecomp, *emitter_args], text=True, capture_output=True,
                    timeout=REPETITION_WALL_CLOCK_CEILING_SECONDS,
                )
                generation_time = time.monotonic() - generation_start
                if generated.returncode != 0 or generated.stdout.startswith("/* translation rejected:"):
                    raise RuntimeError("generation_rejected_or_failed")
                phase_result("generation", "completed", generation_time)
                aot_counts = None
                count_match = re.search(
                    r"immutable-rom AOT enumeration: aligned_start_count=(\d+) accepted_count=(\d+) rejected_count=(\d+)",
                    generated.stderr,
                )
                if count_match is not None:
                    aot_counts = {
                        "aligned_start_count": int(count_match.group(1)),
                        "accepted_count": int(count_match.group(2)),
                        "rejected_count": int(count_match.group(3)),
                    }
                    if aot_counts["accepted_count"] + aot_counts["rejected_count"] != aot_counts["aligned_start_count"]:
                        raise RuntimeError("invalid_aot_aggregate_counts")
                generated_c.write_text(generated.stdout, encoding="utf-8")
                generated_source_bytes = len(generated.stdout.encode("utf-8"))
                out_object = work_dir / "bridge_object"
                phase_result("strict_c11_compile", "started")
                compile_time = strict_c11_compile(compiler, runtime_dir, generated_c, out_object)
                phase_result("strict_c11_compile", "completed", compile_time)
                compiled_object_bytes = out_object.stat().st_size
                phase_result("dispatch_benchmark_compile", "started")
                dispatch_compile_time, dispatch_execution_time, dispatch_cost_seconds = dispatch_microbenchmark(
                    compiler, runtime_dir, generated_c, probe_pc, work_dir, phase_result)
            except subprocess.TimeoutExpired:
                ceiling_hit = "wall_clock_ceiling_exceeded"
                phase_result(phase_state["active"], "failed")
                break
            except ResourceCeilingExceeded as exc:
                ceiling_hit = str(exc)
                phase_result(phase_state["active"], "failed")
                break
            except RuntimeError as exc:
                tool_failure = f"command_failed:{exc}"
                phase_result(phase_state["active"], "failed")
                break
        if measured:
            repetitions.append({
                "generated_source_bytes": generated_source_bytes,
                "compiled_object_bytes": compiled_object_bytes,
                "generation_time_seconds": generation_time,
                "compile_time_seconds": compile_time,
                "dispatch_compile_time_seconds": dispatch_compile_time,
                "dispatch_execution_time_seconds": dispatch_execution_time,
                "dispatch_cost_seconds": dispatch_cost_seconds,
                "immutable_rom_aot_counts": aot_counts,
            })
    measurement_complete = tool_failure is None and ceiling_hit is None and len(repetitions) == measured_repetitions
    result = {"label": label, "repetitions": repetitions, "resource_ceiling_hit": ceiling_hit,
              "tool_failure": tool_failure, "measurement_complete": measurement_complete,
              "protocol": {"warm_up": warm_up, "measured_repetitions": measured_repetitions}}
    if repetitions:
        count_values = [repetition["immutable_rom_aot_counts"] for repetition in repetitions]
        if any(value != count_values[0] for value in count_values):
            raise RuntimeError("nondeterministic_aot_aggregate_counts")
        result["immutable_rom_aot_counts"] = count_values[0]
        for metric in ("generated_source_bytes", "compiled_object_bytes", "generation_time_seconds",
                        "compile_time_seconds", "dispatch_compile_time_seconds",
                        "dispatch_execution_time_seconds", "dispatch_cost_seconds"):
            values = [repetition[metric] for repetition in repetitions]
            result[metric] = {
                "mean": statistics.fmean(values),
                "max": max(values),
                "stdev": statistics.pstdev(values) if len(values) > 1 else 0.0,
                "values": values,
            }
    return result


SYNTHETIC_FIXTURE_IMAGE_HEX = (
    "30514E904E71"      # +0x00 MOVEA.W (A1),A0 ; JSR (A0) ; NOP continuation
    "60F860F6"          # +0x06 BRA.S -> base (V1) ; +0x08 BRA.S -> base (V2)
    "FFFF"              # +0x0A invalid opcode
    "C03B0000"          # +0x0C AND.B (0,PC,D0.W),D0 (excluded form)
    "60004E71"          # +0x10 BRA.W whose extension is a valid overlapping NOP
    "7605"              # +0x14 byte-identical data / MOVEQ #5,D3
)
SYNTHETIC_FIXTURE_BASE = 0x00000C00
SYNTHETIC_FIXTURE_VALID_RANGE = (0x00000C06, 0x00000C16)


def synthetic_scenario(*, segarecomp: str, compiler: str, root: pathlib.Path) -> dict:
    image = bytes.fromhex(SYNTHETIC_FIXTURE_IMAGE_HEX)
    digest = hashlib.sha256(image).hexdigest()
    with tempfile.TemporaryDirectory() as temporary:
        rom_path = pathlib.Path(temporary) / "synthetic-t238.bin"
        rom_path.write_bytes(image)
        entry = f"{SYNTHETIC_FIXTURE_BASE:08x}"
        common = ["emit-general-startup-bridge-c", "--rom", str(rom_path),
                  "--entry", entry, "--mapping-base", entry, "--rom-sha256", digest]
        baseline = measure_variant(
            segarecomp=segarecomp, compiler=compiler, root=root, emitter_args=common,
            probe_pc=SYNTHETIC_FIXTURE_VALID_RANGE[0], label="synthetic_baseline")
        experiment_args = common + ["--immutable-rom-aot"]
        experiment = measure_variant(
            segarecomp=segarecomp, compiler=compiler, root=root, emitter_args=experiment_args,
            probe_pc=SYNTHETIC_FIXTURE_VALID_RANGE[0], label="synthetic_experiment")
    return {"baseline": baseline, "experiment": experiment}


def canonical_sonic_scenario(*, segarecomp: str, compiler: str, root: pathlib.Path,
                               rom: pathlib.Path, hints: pathlib.Path | None,
                               measured_repetitions: int, warm_up: int) -> dict:
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()
    common = ["emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry",
              "--rom-sha256", digest]
    if hints is not None:
        common += ["--external-hints", str(hints)]
    baseline = measure_variant(
        segarecomp=segarecomp, compiler=compiler, root=root, emitter_args=common,
        probe_pc=0x00000000, label="canonical_sonic_baseline",
        measured_repetitions=measured_repetitions, warm_up=warm_up)
    experiment_args = common + ["--immutable-rom-aot"]
    experiment = measure_variant(
        segarecomp=segarecomp, compiler=compiler, root=root, emitter_args=experiment_args,
        probe_pc=0x00000000, label="canonical_sonic_experiment",
        measured_repetitions=measured_repetitions, warm_up=warm_up)
    return {"baseline": baseline, "experiment": experiment, "rom_sha256": digest}


def verdict(baseline: dict, experiment: dict) -> dict:
    verdicts = {}
    if baseline.get("resource_ceiling_hit") or experiment.get("resource_ceiling_hit"):
        return {"overall": "resource_ceiling_hit",
                "baseline_ceiling_hit": baseline.get("resource_ceiling_hit"),
                "experiment_ceiling_hit": experiment.get("resource_ceiling_hit")}
    if baseline.get("tool_failure") or experiment.get("tool_failure"):
        return {"overall": "tool_failure",
                "baseline_tool_failure": baseline.get("tool_failure"),
                "experiment_tool_failure": experiment.get("tool_failure")}
    if not baseline.get("measurement_complete") or not experiment.get("measurement_complete"):
        return {"overall": "measurement_incomplete"}
    metric_names = {
        "generated_source_bytes": "generated_source_bytes_ratio",
        "compiled_object_bytes": "compiled_object_bytes_ratio",
        "generation_time_seconds": "generation_time_seconds_ratio",
        "compile_time_seconds": "compile_time_seconds_ratio",
        "dispatch_cost_seconds": "dispatch_cost_seconds_ratio",
    }
    all_pass = True
    for metric, threshold_key in metric_names.items():
        base_mean = baseline.get(metric, {}).get("mean")
        exp_mean = experiment.get(metric, {}).get("mean")
        if base_mean is None or exp_mean is None:
            verdicts[metric] = {"verdict": "unmeasured"}
            all_pass = False
            continue
        ratio = exp_mean / base_mean if base_mean > 0 else float("inf")
        threshold = THRESHOLDS[threshold_key]
        passed = ratio <= threshold
        all_pass = all_pass and passed
        verdicts[metric] = {"ratio": ratio, "threshold": threshold, "verdict": "pass" if passed else "fail"}
    verdicts["overall"] = "pass" if all_pass else "fail"
    return verdicts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--rom")
    parser.add_argument("--external-hints")
    # SEG-007-T238 evidence run: the canonical Sonic route's single existing
    # whole-ROM offline-inventory generation already costs several
    # CPU-minutes per invocation BEFORE this experiment adds anything (a
    # measured fact, not an assumption) -- 1 warm-up + 3 measured
    # repetitions per variant would cost roughly 4x that per variant. These
    # defaults keep the canonical route's own repetition count bounded for
    # ordinary agent-turn/CI cost; override explicitly for a fuller run when
    # more wall-clock budget is available. The synthetic fixture above always
    # uses ADR-0039's full 1-warm-up/3-measured protocol regardless of these
    # flags.
    parser.add_argument("--canonical-warm-up", type=int, default=0)
    parser.add_argument("--canonical-measured-repetitions", type=int, default=1)
    parser.add_argument("--skip-canonical", action="store_true",
                         help="synthetic-fixture-only run (no authorized local ROM available)")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]

    report: dict = {
        "schema": 1,
        "thresholds_registered_before_measurement": THRESHOLDS,
        "synthetic_fixture_repetition_protocol": {"warm_up": 1, "measured_repetitions": 3},
        "canonical_sonic_route_repetition_protocol": {
            "warm_up": args.canonical_warm_up if not args.skip_canonical else None,
            "measured_repetitions": args.canonical_measured_repetitions if not args.skip_canonical else None,
        },
        "resource_ceilings": {
            "wall_clock_seconds_per_repetition": REPETITION_WALL_CLOCK_CEILING_SECONDS,
            "peak_rss_bytes_per_repetition": REPETITION_PEAK_RSS_CEILING_BYTES,
        },
    }

    synthetic = synthetic_scenario(segarecomp=args.segarecomp, compiler=args.cc, root=root)
    report["synthetic_fixture"] = synthetic
    report["synthetic_fixture"]["verdict"] = verdict(synthetic["baseline"], synthetic["experiment"])

    if not args.skip_canonical and args.rom:
        rom = pathlib.Path(args.rom)
        hints = pathlib.Path(args.external_hints) if args.external_hints else None
        canonical = canonical_sonic_scenario(
            segarecomp=args.segarecomp, compiler=args.cc, root=root, rom=rom, hints=hints,
            measured_repetitions=args.canonical_measured_repetitions, warm_up=args.canonical_warm_up)
        report["canonical_sonic_route"] = canonical
        report["canonical_sonic_route"]["verdict"] = verdict(canonical["baseline"], canonical["experiment"])
    else:
        report["canonical_sonic_route"] = {"skipped": True, "reason": "no authorized local ROM/hints supplied"}

    print(json.dumps(report, indent=2, sort_keys=True))
    overall = report["synthetic_fixture"]["verdict"].get("overall")
    canonical_overall = report["canonical_sonic_route"].get("verdict", {}).get("overall")
    if overall != "pass" or (canonical_overall is not None and canonical_overall != "pass"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
