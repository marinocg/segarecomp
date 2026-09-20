// SEG-007-T252 / ADR-0040: focused unit coverage of the runner/guest
// ownership split. `genesis_runtime_step` is the guest-owned single-step
// contract; `genesis_runtime_run` is the runner-owned finite dispatch
// allowance. This replaces the former SEG-007-T107 (ADR-0007),
// SEG-007-T155/ADR-0017, and SEG-007-T211/ADR-0035 watchdog-progress-note
// tests: there is no watchdog left, and these tests instead prove the new
// disjoint GENESIS_RUNNER_RESOURCE_LIMIT outcome. No target opcode, image, or
// decoder is involved -- every dispatch function here is a project-authored
// synthetic closure.

#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

std::function<GenesisControlTransfer(GenesisRuntime *)> g_step;

GenesisControlTransfer trampoline(GenesisRuntime *runtime) { return g_step(runtime); }

GenesisControlTransfer make_stop(GenesisStopClass stop_class, GenesisDiagnosticCategory diagnostic) {
  GenesisControlTransfer transfer{};
  transfer.kind = GENESIS_STOP;
  transfer.stop.stop_class = stop_class;
  transfer.stop.diagnostic_category = diagnostic;
  return transfer;
}

GenesisControlTransfer make_complete() {
  GenesisControlTransfer transfer{};
  transfer.kind = GENESIS_COMPLETE;
  return transfer;
}

GenesisControlTransfer make_continue(uint32_t next_pc) {
  GenesisControlTransfer transfer{};
  transfer.kind = GENESIS_CONTINUE_AT_PC;
  transfer.next_pc = next_pc;
  return transfer;
}

// (a) genesis_runtime_run completes before its allowance is exhausted.
void run_completes_before_exhaustion() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    if (step >= 3) return make_complete();
    return make_continue(0x00000B00U + static_cast<uint32_t>(step) * 2U);
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(result.kind == GENESIS_COMPLETE, "genesis_runtime_run returns GENESIS_COMPLETE");
  check(step == 3, "exactly the steps needed to reach completion were dispatched");
}

// (b) genesis_runtime_run stops (genuine guest semantic stop) before its
// allowance is exhausted.
void run_stops_before_exhaustion() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    if (step >= 3) return make_stop(GENESIS_STOP_UNSUPPORTED_CPU_FORM, GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION);
    return make_continue(0x00000B00U + static_cast<uint32_t>(step) * 2U);
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(result.kind == GENESIS_STOP, "genesis_runtime_run returns GENESIS_STOP");
  check(result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM, "the genuine guest stop class is preserved");
  check(step == 3, "exactly the steps needed to reach the stop were dispatched");
}

// (c) genesis_runtime_run's allowance is exhausted while the guest dispatcher
// never stops or completes: GENESIS_RUNNER_RESOURCE_LIMIT, with `.stop` left
// structurally invalid/zeroed -- proving a caller must switch on `.kind`
// first, never infer a guest stop from `.stop` alone.
void run_reports_runner_resource_limit_with_zeroed_stop() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    // Never stops or completes: an unconditional self-loop shape.
    return make_continue(0x00000B00U);
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 16U);
  check(result.kind == GENESIS_RUNNER_RESOURCE_LIMIT, "genesis_runtime_run reports the disjoint runner outcome");
  check(step == 16, "exactly the allowance's worth of steps were dispatched");
  check(result.runner_dispatch_count == 16U, "the deterministic dispatch count matches the allowance");
  GenesisRuntimeStop zero_stop{};
  check(std::memcmp(&result.stop, &zero_stop, sizeof(zero_stop)) == 0,
        "GENESIS_RUNNER_RESOURCE_LIMIT leaves .stop fully zeroed/structurally meaningless");
  check(result.stop.stop_class == 0, "0 is not a valid GenesisStopClass, so a kind-blind reader cannot mistake this");
}

// A synthetic dispatcher looping far beyond the former dispatch_budget=128 /
// GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT (0x00200000) magnitude, given a large
// allowance, reaches genuine completion rather than any credit-based cutoff
// -- replacing the intent of the deleted watchdog tests.
void large_allowance_reaches_genuine_completion_beyond_former_watchdog_magnitude() {
  GenesisRuntime runtime{};
  const uint32_t target_steps = 200000U; // far beyond the former W=128 no-progress window
  uint32_t step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    if (step >= target_steps) return make_complete();
    return make_continue(0x00000B00U);
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, target_steps + 1U);
  check(result.kind == GENESIS_COMPLETE, "a large allowance reaches genuine completion, not a credit-based cutoff");
  check(step == target_steps, "every dispatch step ran; no watchdog-style early cutoff fired");
}

// Identical fixture, identical allowance, twice: byte-identical dispatch
// count and full CPU/device snapshot; result is runner_resource_limit, never
// instruction_budget_exhausted.
void identical_runs_are_byte_identical() {
  auto run_once = [](GenesisRuntime *out_runtime) -> GenesisControlTransfer {
    GenesisRuntime runtime{};
    int step = 0;
    g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
      ++step;
      r->d[0] = static_cast<uint32_t>(step); // deterministic, guest-visible side effect
      return make_continue(0x00000B00U);
    };
    const auto result = genesis_runtime_run(&runtime, trampoline, 40U);
    *out_runtime = runtime;
    return result;
  };
  GenesisRuntime first_runtime{};
  GenesisRuntime second_runtime{};
  const auto first = run_once(&first_runtime);
  const auto second = run_once(&second_runtime);
  check(first.kind == GENESIS_RUNNER_RESOURCE_LIMIT && second.kind == GENESIS_RUNNER_RESOURCE_LIMIT,
        "both identical runs report runner_resource_limit, never instruction_budget_exhausted");
  check(first.runner_dispatch_count == second.runner_dispatch_count,
        "identical allowance on an identical fixture reports an identical dispatch count");
  check(std::memcmp(&first_runtime, &second_runtime, sizeof(GenesisRuntime)) == 0,
        "identical runs produce a byte-identical full runtime snapshot");
}

// Two different allowances sharing a common executable prefix: the common
// prefix's own guest-visible state (here, D0's deterministic step counter) is
// byte-identical regardless of which allowance eventually governs the run.
void common_prefix_is_identical_across_different_allowances() {
  GenesisRuntime common_prefix_snapshot_from_short_run{};
  GenesisRuntime common_prefix_snapshot_from_long_run{};
  auto run_with_allowance = [](uint32_t allowance, GenesisRuntime *out_runtime,
                               GenesisRuntime *common_prefix_snapshot) -> GenesisControlTransfer {
    GenesisRuntime runtime{};
    uint32_t step = 0;
    g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
      ++step;
      r->d[0] = step; // deterministic guest-visible progress
      // VBlank scheduler tick advances once per genesis_runtime_step call
      // regardless of allowance; snapshotting r->scheduler here (via the
      // whole-runtime snapshot below, taken strictly after this step's own
      // scheduler tick already ran inside genesis_runtime_step) lets the two
      // runs' common prefixes be compared byte-for-byte.
      if (step == 5U) *common_prefix_snapshot = *r;
      if (step == 10U) return make_stop(GENESIS_STOP_UNSUPPORTED_CPU_FORM, GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION);
      return make_continue(0x00000B00U);
    };
    const auto result = genesis_runtime_run(&runtime, trampoline, allowance);
    *out_runtime = runtime;
    return result;
  };
  GenesisRuntime short_runtime{};
  GenesisRuntime long_runtime{};
  // Short allowance: hits GENESIS_RUNNER_RESOURCE_LIMIT before the guest stop.
  const auto short_result = run_with_allowance(5U, &short_runtime, &common_prefix_snapshot_from_short_run);
  // Long allowance: long enough to reach the guest's own genuine stop.
  const auto long_result = run_with_allowance(20U, &long_runtime, &common_prefix_snapshot_from_long_run);
  check(short_result.kind == GENESIS_RUNNER_RESOURCE_LIMIT, "the short allowance hits the runner resource limit first");
  check(long_result.kind == GENESIS_STOP && long_result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "the long allowance reaches the guest's own genuine stop");
  // The common executable prefix (the whole runtime, including the VBlank
  // scheduler's own event sequence, snapshotted after exactly step 5 in both
  // runs) is byte-identical: the allowance only changes whether the host
  // runner permits a longer prefix, never retimes anything within the
  // common one.
  check(std::memcmp(&common_prefix_snapshot_from_short_run, &common_prefix_snapshot_from_long_run,
                     sizeof(GenesisRuntime)) == 0,
        "the common executed prefix's full state (including VBlank scheduler state) is byte-identical "
        "regardless of the eventual allowance");
}

// (guest) DBcc/DBF, EA/routed-access, VBlank/IRQ/exception, and RTE guest
// behavior are exercised by their own existing dedicated test files, unaffected
// by this runner/guest ownership split beyond the genesis_runtime_drive ->
// genesis_runtime_step/genesis_runtime_run rename.

} // namespace

int main() {
  run_completes_before_exhaustion();
  run_stops_before_exhaustion();
  run_reports_runner_resource_limit_with_zeroed_stop();
  large_allowance_reaches_genuine_completion_beyond_former_watchdog_magnitude();
  identical_runs_are_byte_identical();
  common_prefix_is_identical_across_different_allowances();
  if (failures != 0) {
    std::printf("genesis_runtime_run_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_runtime_run_test: all checks passed\n");
  return 0;
}
