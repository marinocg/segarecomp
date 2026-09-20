// SEG-007-T252 / ADR-0040 correction: the bounded 64-entry diagnostic-only
// `GenesisRuntime.recent_pc_history` circular buffer. Project-authored
// synthetic dispatchers only -- no commercial/runtime PC value anywhere in
// this file. See the field's own doc comment in platforms/genesis/runtime/runtime.h
// for the recorded convention (PC-about-to-dispatch, recorded at the top of
// genesis_runtime_step, before dispatch() runs).

#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "host_io.hpp"

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

GenesisControlTransfer make_continue(uint32_t next_pc) {
  GenesisControlTransfer transfer{};
  transfer.kind = GENESIS_CONTINUE_AT_PC;
  transfer.next_pc = next_pc;
  return transfer;
}

std::vector<uint32_t> ordered_history(const GenesisRuntime &runtime) {
  std::vector<uint32_t> ordered;
  const uint8_t count = runtime.recent_pc_history_count;
  const uint8_t oldest = static_cast<uint8_t>(
      count < GENESIS_RECENT_PC_HISTORY_CAPACITY ? 0U : runtime.recent_pc_history_next);
  for (uint8_t index = 0U; index < count; ++index) {
    const uint8_t slot = static_cast<uint8_t>((oldest + index) % GENESIS_RECENT_PC_HISTORY_CAPACITY);
    ordered.push_back(runtime.recent_pc_history[slot]);
  }
  return ordered;
}

// A synthetic dispatcher that always advances PC by 4 (a simple, honest,
// deterministic PC sequence 0x1000, 0x1004, 0x1008, ...) so the "PC about to
// dispatch" convention is directly checkable: step N's recorded PC must equal
// the PC value genesis_runtime_step observed BEFORE calling dispatch(), i.e.
// the runtime's PC as of the start of that step.
void drive_linear(GenesisRuntime *runtime, uint32_t step_count, uint32_t base_pc) {
  uint32_t step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    return make_continue(r->pc + 4U);
  };
  runtime->pc = base_pc;
  genesis_runtime_run(runtime, trampoline, step_count);
}

// (1) Fewer than 64 steps returns exactly the available ordered PCs.
void fewer_than_capacity_returns_exact_available_pcs() {
  GenesisRuntime runtime{};
  const uint32_t base = 0x00001000U;
  drive_linear(&runtime, 10U, base);
  const auto history = ordered_history(runtime);
  check(history.size() == 10U, "fewer-than-capacity history has exactly the steps taken");
  for (uint32_t index = 0U; index < history.size(); ++index) {
    check(history[index] == base + index * 4U, "fewer-than-capacity entries are the correct PC-about-to-dispatch values");
  }
}

// (2) Exactly 64 steps returns exactly 64 values.
void exactly_capacity_returns_exactly_64() {
  GenesisRuntime runtime{};
  const uint32_t base = 0x00002000U;
  drive_linear(&runtime, GENESIS_RECENT_PC_HISTORY_CAPACITY, base);
  const auto history = ordered_history(runtime);
  check(history.size() == GENESIS_RECENT_PC_HISTORY_CAPACITY, "exactly-capacity history has exactly 64 entries");
  for (uint32_t index = 0U; index < history.size(); ++index) {
    check(history[index] == base + index * 4U, "exactly-capacity entries are oldest->newest in order");
  }
}

// (3)+(4) More than 64 steps retains only the most recent 64, oldest->newest,
// with circular wrap preserving correct order.
void more_than_capacity_retains_most_recent_64_in_order() {
  GenesisRuntime runtime{};
  const uint32_t base = 0x00003000U;
  const uint32_t total_steps = 200U; // far beyond the 64-entry capacity
  drive_linear(&runtime, total_steps, base);
  const auto history = ordered_history(runtime);
  check(history.size() == GENESIS_RECENT_PC_HISTORY_CAPACITY,
        "more-than-capacity history saturates at exactly 64 entries");
  const uint32_t expected_oldest_pc = base + (total_steps - GENESIS_RECENT_PC_HISTORY_CAPACITY) * 4U;
  for (uint32_t index = 0U; index < history.size(); ++index) {
    check(history[index] == expected_oldest_pc + index * 4U,
          "circular wrap preserves oldest->newest order for the most recent 64 PCs");
  }
}

// (5) Repeated-PC loops are represented honestly: duplicates are not
// deduplicated.
void repeated_pc_loop_is_not_deduplicated() {
  GenesisRuntime runtime{};
  const uint32_t looped_pc = 0x00004000U;
  int step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    return make_continue(looped_pc); // an unconditional self-loop shape
  };
  runtime.pc = looped_pc;
  genesis_runtime_run(&runtime, trampoline, 20U);
  const auto history = ordered_history(runtime);
  check(history.size() == 20U, "a repeated-PC loop still records one entry per step");
  for (uint32_t value : history) {
    check(value == looped_pc, "every recorded entry honestly reflects the repeated PC, never deduplicated/collapsed");
  }
}

// (6) GENESIS_RUNNER_RESOURCE_LIMIT disjointness/zeroed-stop semantics are
// otherwise unchanged by adding history recording.
void runner_resource_limit_semantics_are_unchanged() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *) -> GenesisControlTransfer {
    ++step;
    return make_continue(0x00005000U);
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 16U);
  check(result.kind == GENESIS_RUNNER_RESOURCE_LIMIT, "runner-resource-limit outcome is unaffected by history recording");
  check(result.runner_dispatch_count == 16U, "the deterministic dispatch count is unaffected by history recording");
  GenesisRuntimeStop zero_stop{};
  check(std::memcmp(&result.stop, &zero_stop, sizeof(zero_stop)) == 0,
        "GENESIS_RUNNER_RESOURCE_LIMIT still leaves .stop fully zeroed/structurally meaningless");
}

// (7) Recording the history cannot alter runtime/device state: two identical
// fixtures (one within capacity, one wrapping well past it) produce
// byte-identical CPU-visible state for their common prefix, and the
// deterministic guest-visible side effect (d[0]) is exactly the step count
// regardless of how many times the history buffer has wrapped -- proving
// history bookkeeping is a pure side-channel append with no other effect.
void recording_history_does_not_alter_guest_state() {
  auto run_with_steps = [](uint32_t step_count, GenesisRuntime *out) {
    GenesisRuntime runtime{};
    uint32_t step = 0;
    g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
      ++step;
      r->d[0] = step; // deterministic guest-visible progress
      return make_continue(0x00006000U);
    };
    genesis_runtime_run(&runtime, trampoline, step_count);
    *out = runtime;
  };
  GenesisRuntime short_run{};
  GenesisRuntime long_run{}; // wraps the 64-entry history buffer several times
  run_with_steps(10U, &short_run);
  run_with_steps(300U, &long_run);
  check(short_run.d[0] == 10U, "guest-visible state after a short run is exactly the step count");
  check(long_run.d[0] == 300U, "guest-visible state after a long, history-wrapping run is still exactly the step count");
  check(short_run.pc == long_run.pc, "PC (guest-owned, unrelated to history bookkeeping) is unaffected by wraparound");
  check(short_run.sr == long_run.sr, "SR (guest-owned) is unaffected by history bookkeeping/wraparound");
}

// (8) Absence from stable schemas: genesis_write_sanitized_report never
// includes the history, and a genuine guest stop's full report (which never
// carries recent_pc_history) still validates exactly like before.
void history_is_absent_from_sanitized_and_guest_stop_full_reports() {
  GenesisRuntime runtime{};
  runtime.pc = 0x00007000U;
  GenesisControlTransfer result{};
  result.kind = GENESIS_STOP;
  result.stop.stop_class = GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY;
  result.stop.diagnostic_category = GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY;

  char sanitized_buffer[4096];
  FILE *sanitized_stream = std::tmpfile();
  check(sanitized_stream != nullptr, "tmpfile for sanitized report must open");
  if (sanitized_stream != nullptr) {
    const char rom_sha256[65] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const GenesisReportMetadata metadata = {GENESIS_CPU_DIMENSIONS_NONE};
    int saved_stdout_fd = host_io::dup_fd(host_io::fileno_of(stdout));
    host_io::dup2_fd(host_io::fileno_of(sanitized_stream), host_io::fileno_of(stdout));
    const int status = genesis_write_sanitized_report(&result, rom_sha256, &metadata);
    std::fflush(stdout);
    host_io::dup2_fd(saved_stdout_fd, host_io::fileno_of(stdout));
    host_io::close_fd(saved_stdout_fd);
    check(status == 0, "genesis_write_sanitized_report must succeed for a genuine guest stop");
    std::rewind(sanitized_stream);
    const size_t read_count = std::fread(sanitized_buffer, 1, sizeof(sanitized_buffer) - 1, sanitized_stream);
    sanitized_buffer[read_count] = '\0';
    std::fclose(sanitized_stream);
    check(std::strstr(sanitized_buffer, "recent_pc_history") == nullptr,
          "a sanitized report never contains recent_pc_history, for any result kind");
  }

  char full_buffer[8192];
  FILE *full_stream = std::tmpfile();
  check(full_stream != nullptr, "tmpfile for full report must open");
  if (full_stream != nullptr) {
    const char rom_sha256[65] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const GenesisReportMetadata metadata = {GENESIS_CPU_DIMENSIONS_NONE};
    const int status = genesis_write_full_report(full_stream, &runtime, &result, rom_sha256, &metadata);
    check(status == 0, "genesis_write_full_report must succeed for a genuine guest stop");
    std::rewind(full_stream);
    const size_t read_count = std::fread(full_buffer, 1, sizeof(full_buffer) - 1, full_stream);
    full_buffer[read_count] = '\0';
    std::fclose(full_stream);
    check(std::strstr(full_buffer, "recent_pc_history") == nullptr,
          "a genuine guest stop's full report never carries recent_pc_history "
          "(reserved for GENESIS_RUNNER_RESOURCE_LIMIT only)");
  }
}

// (9) The defect this correction closes: a GENESIS_RUNNER_RESOURCE_LIMIT full
// report (the one case that previously DID leak the history) must now be
// free of it, whether written to an in-memory FILE* or to a real filesystem
// path -- the exact persistence surface the original defect exposed.
void history_is_absent_from_runner_resource_limit_full_report() {
  GenesisRuntime runtime{};
  const uint32_t base = 0x00008000U;
  drive_linear(&runtime, GENESIS_RECENT_PC_HISTORY_CAPACITY, base);
  const auto history = ordered_history(runtime);
  check(history.size() == GENESIS_RECENT_PC_HISTORY_CAPACITY,
        "setup: the driven runtime must actually have populated history to make this a meaningful test");

  GenesisControlTransfer result{};
  result.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
  result.runner_dispatch_count = GENESIS_RECENT_PC_HISTORY_CAPACITY;
  const char rom_sha256[65] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const GenesisReportMetadata metadata = {GENESIS_CPU_DIMENSIONS_NONE};

  // In-memory FILE* case.
  char full_buffer[131072];
  FILE *full_stream = std::tmpfile();
  check(full_stream != nullptr, "tmpfile for runner-resource-limit full report must open");
  if (full_stream != nullptr) {
    const int status = genesis_write_full_report(full_stream, &runtime, &result, rom_sha256, &metadata);
    check(status == 0, "genesis_write_full_report must succeed for GENESIS_RUNNER_RESOURCE_LIMIT");
    std::rewind(full_stream);
    const size_t read_count = std::fread(full_buffer, 1, sizeof(full_buffer) - 1, full_stream);
    full_buffer[read_count] = '\0';
    std::fclose(full_stream);
    check(std::strstr(full_buffer, "recent_pc_history") == nullptr,
          "a GENESIS_RUNNER_RESOURCE_LIMIT full report must never carry recent_pc_history (the original defect)");
    check(std::strstr(full_buffer, "runner_dispatch_count") != nullptr,
          "runner_dispatch_count remains present and the report remains otherwise valid without any history array");
  }

  // Real filesystem path case, exercising the same persistence surface the
  // generated bridge's `--full-report-path` argument writes through.
  const std::string path_storage = host_io::make_temp_file("genesis_recent_pc_history_test.full");
  const char *path_template = path_storage.c_str();
  check(!path_storage.empty(), "temp file for real-file runner-resource-limit full report must succeed");
  if (!path_storage.empty()) {
    FILE *file_stream = std::fopen(path_template, "w");
    check(file_stream != nullptr, "fopen for real-file full report must succeed");
    if (file_stream != nullptr) {
      const int status = genesis_write_full_report(file_stream, &runtime, &result, rom_sha256, &metadata);
      check(status == 0, "genesis_write_full_report must succeed when writing to a real file");
      std::fclose(file_stream);
      FILE *reopened = std::fopen(path_template, "r");
      check(reopened != nullptr, "the written full-report file must be reopenable");
      if (reopened != nullptr) {
        char file_buffer[131072];
        const size_t read_count = std::fread(file_buffer, 1, sizeof(file_buffer) - 1, reopened);
        file_buffer[read_count] = '\0';
        std::fclose(reopened);
        check(std::strstr(file_buffer, "recent_pc_history") == nullptr,
              "a real-file GENESIS_RUNNER_RESOURCE_LIMIT full report must never carry recent_pc_history");
      }
    }
    std::remove(path_template);
  }
}

// (10) genesis_write_ephemeral_pc_history itself, called directly on a
// runtime/result pair, DOES emit the expected hex-formatted history entries
// -- the C-level analog of "the diagnostic FD path receives the recent-PC
// history". A non-runner_resource_limit result yields an empty array.
void ephemeral_writer_emits_expected_history() {
  GenesisRuntime runtime{};
  const uint32_t base = 0x00009000U;
  drive_linear(&runtime, 5U, base);
  const auto history = ordered_history(runtime);
  check(history.size() == 5U, "setup: exactly 5 steps must be recorded");

  GenesisControlTransfer result{};
  result.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
  result.runner_dispatch_count = 5U;

  char buffer[4096];
  FILE *stream = std::tmpfile();
  check(stream != nullptr, "tmpfile for ephemeral writer must open");
  if (stream != nullptr) {
    const int status = genesis_write_ephemeral_pc_history(stream, &runtime, &result);
    check(status == 0, "genesis_write_ephemeral_pc_history must succeed");
    std::rewind(stream);
    const size_t read_count = std::fread(buffer, 1, sizeof(buffer) - 1, stream);
    buffer[read_count] = '\0';
    std::fclose(stream);
    char expected[64];
    std::snprintf(expected, sizeof(expected), "\"0x%08x\"", base);
    check(std::strstr(buffer, expected) != nullptr,
          "the ephemeral writer's output must contain the expected hex-formatted oldest history entry");
    std::snprintf(expected, sizeof(expected), "\"0x%08x\"", base + 4U * 4U);
    check(std::strstr(buffer, expected) != nullptr,
          "the ephemeral writer's output must contain the expected hex-formatted newest history entry");
  }

  // A non-runner_resource_limit result must yield an empty array, never the
  // recorded history (this diagnostic-only writer still respects the same
  // result-kind gate the removed full-report branch used to).
  GenesisControlTransfer other_result{};
  other_result.kind = GENESIS_STOP;
  char empty_buffer[64];
  FILE *empty_stream = std::tmpfile();
  check(empty_stream != nullptr, "tmpfile for empty-case ephemeral writer must open");
  if (empty_stream != nullptr) {
    const int status = genesis_write_ephemeral_pc_history(empty_stream, &runtime, &other_result);
    check(status == 0, "genesis_write_ephemeral_pc_history must succeed even for a non-resource-limit result");
    std::rewind(empty_stream);
    const size_t read_count = std::fread(empty_buffer, 1, sizeof(empty_buffer) - 1, empty_stream);
    empty_buffer[read_count] = '\0';
    std::fclose(empty_stream);
    check(std::strstr(empty_buffer, "[]") != nullptr,
          "a non-GENESIS_RUNNER_RESOURCE_LIMIT result yields a bare empty JSON array");
  }
}

} // namespace

int main() {
  fewer_than_capacity_returns_exact_available_pcs();
  exactly_capacity_returns_exactly_64();
  more_than_capacity_retains_most_recent_64_in_order();
  repeated_pc_loop_is_not_deduplicated();
  runner_resource_limit_semantics_are_unchanged();
  recording_history_does_not_alter_guest_state();
  history_is_absent_from_sanitized_and_guest_stop_full_reports();
  history_is_absent_from_runner_resource_limit_full_report();
  ephemeral_writer_emits_expected_history();
  if (failures != 0) {
    std::printf("genesis_recent_pc_history_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_recent_pc_history_test: all checks passed\n");
  return 0;
}
