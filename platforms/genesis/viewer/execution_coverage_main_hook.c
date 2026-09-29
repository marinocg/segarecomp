/*
 * SEG-026-T001: production seam between a generated Genesis bridge program and the headless execution-PC
 * coverage run (execution_coverage.h). Mirrors the SEG-021-T031 frame-capture seam: the coverage build
 * (tools/genesis_startup_bridge.py --execution-coverage) compiles the UNMODIFIED generated main TU with
 * -Dgenesis_runtime_run=genesis_execution_coverage_hook_run, so the generated main hands its own
 * GenesisRuntime, dispatcher and runner allowance to this function. Options arrive via environment variables
 * (validated by the Python driver and re-validated here) so the generated argv ABI is unchanged:
 *   SEGARECOMP_COVERAGE_FRAMES (> 0), SEGARECOMP_COVERAGE_EPOCH_FRAMES (> 0, default = FRAMES),
 *   SEGARECOMP_COVERAGE_ENABLED (0 or 1, default 1), SEGARECOMP_COVERAGE_WITNESS_CAPACITY (default 1048576),
 *   SEGARECOMP_COVERAGE_DIR (private output directory; required when enabled).
 * Prints one COVERAGE_SUMMARY line of aggregates only (counts and digests; never a PC).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "execution_coverage.h"
#include "runtime.h"

GenesisControlTransfer genesis_execution_coverage_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                           uint32_t dispatch_allowance);

static int genesis_coverage_parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long value;
  size_t index;
  if (text == NULL || text[0] == '\0') return 1;
  for (index = 0; text[index] != '\0'; ++index)
    if (text[index] < '0' || text[index] > '9') return 1;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || end == NULL || *end != '\0') return 1;
  *out = (uint64_t)value;
  return 0;
}

static void genesis_coverage_hex(const uint8_t digest[32]) {
  unsigned byte;
  for (byte = 0; byte < 32U; ++byte) fprintf(stderr, "%02x", (unsigned)digest[byte]);
}

static GenesisExecutionCoverageResult g_result;

GenesisControlTransfer genesis_execution_coverage_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                           uint32_t dispatch_allowance) {
  GenesisExecutionCoverageOptions options;
  GenesisExecutionCoverageResult *r = &g_result;
  uint64_t frames = 0U, epoch = 0U, enabled = 1U, capacity = UINT64_C(1048576);
  const char *epoch_text = getenv("SEGARECOMP_COVERAGE_EPOCH_FRAMES");
  const char *enabled_text = getenv("SEGARECOMP_COVERAGE_ENABLED");
  const char *capacity_text = getenv("SEGARECOMP_COVERAGE_WITNESS_CAPACITY");
  const char *name;
  uint32_t index;
  memset(&options, 0, sizeof(options));
  if (genesis_coverage_parse_u64(getenv("SEGARECOMP_COVERAGE_FRAMES"), &frames) != 0 || frames == 0U ||
      (epoch_text != NULL && genesis_coverage_parse_u64(epoch_text, &epoch) != 0) ||
      (enabled_text != NULL && (genesis_coverage_parse_u64(enabled_text, &enabled) != 0 || enabled > 1U)) ||
      (capacity_text != NULL && genesis_coverage_parse_u64(capacity_text, &capacity) != 0) ||
      (enabled == 1U && getenv("SEGARECOMP_COVERAGE_DIR") == NULL)) {
    fprintf(stderr, "coverage: malformed coverage options\n");
    exit(3);
  }
  options.target_frames = frames;
  options.epoch_frames = epoch == 0U ? frames : epoch;
  options.coverage_enabled = (int)enabled;
  options.witness_capacity = capacity;
  options.private_dir = enabled == 1U ? getenv("SEGARECOMP_COVERAGE_DIR") : NULL;
  genesis_execution_coverage_run(runtime, dispatch, &options, dispatch_allowance, r);
  name = r->outcome == GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED    ? "frames_reached"
         : r->outcome == GENESIS_EXECUTION_COVERAGE_RUNNER_EXHAUSTED ? "runner_resource_limit"
         : r->outcome == GENESIS_EXECUTION_COVERAGE_GUEST_STOP       ? "guest_stop"
         : r->outcome == GENESIS_EXECUTION_COVERAGE_GUEST_COMPLETE   ? "guest_complete"
         : r->outcome == GENESIS_EXECUTION_COVERAGE_IO_ERROR         ? "io_error"
                                                                     : "invalid_argument";
  fprintf(stderr,
          "COVERAGE_SUMMARY {\"outcome\":\"%s\",\"coverage_enabled\":%s,\"target_frames\":%llu,\"epoch_frames\":%llu,"
          "\"frames_published\":%llu,\"dispatches\":%llu,\"stop_class\":%d,\"retirements\":%llu,"
          "\"distinct_pc_count\":%llu,\"witness_count\":%llu,\"witness_overflow\":%llu,"
          "\"unknown_retirements\":%llu,\"odd_pc_retirements\":%llu,\"wide_pc_retirements\":%llu,"
          "\"interrupt_redirects\":%llu,\"interrupt_resumptions\":%llu,\"interrupt_depth_overflow\":%llu,\"coverage_digest\":\"",
          name, enabled ? "true" : "false", (unsigned long long)options.target_frames,
          (unsigned long long)options.epoch_frames, (unsigned long long)r->frames_published,
          (unsigned long long)r->dispatches,
          r->outcome == GENESIS_EXECUTION_COVERAGE_GUEST_STOP ? (int)r->transfer.stop.stop_class : 0,
          (unsigned long long)r->coverage.retirement_count, (unsigned long long)r->coverage.distinct_count,
          (unsigned long long)r->coverage.witness_count, (unsigned long long)r->coverage.witness_overflow,
          (unsigned long long)r->coverage.unknown_retirement_count, (unsigned long long)r->coverage.odd_pc_count,
          (unsigned long long)r->coverage.wide_pc_count, (unsigned long long)r->coverage.interrupt_redirects,
          (unsigned long long)r->coverage.interrupt_resumptions,
          (unsigned long long)r->coverage.interrupt_depth_overflow);
  genesis_coverage_hex(r->coverage_digest);
  fprintf(stderr, "\",\"frame_stream_digest\":\"");
  genesis_coverage_hex(r->frame_stream_digest);
  fprintf(stderr, "\",\"final_state_digest\":\"");
  genesis_coverage_hex(r->final_state_digest);
  fprintf(stderr, "\",\"epochs\":[");
  for (index = 0; index < r->epoch_count; ++index) {
    const GenesisExecutionCoverageEpoch *e = &r->epochs[index];
    fprintf(stderr, "%s{\"frame\":%llu,\"dispatches\":%llu,\"retirements\":%llu,\"distinct\":%llu,\"digest\":\"",
            index == 0U ? "" : ",", (unsigned long long)e->frame, (unsigned long long)e->dispatches,
            (unsigned long long)e->retirements, (unsigned long long)e->distinct);
    genesis_coverage_hex(e->digest);
    fprintf(stderr, "\"}");
  }
  fprintf(stderr, "]}\n");
  if (r->outcome == GENESIS_EXECUTION_COVERAGE_INVALID_ARGUMENT || r->outcome == GENESIS_EXECUTION_COVERAGE_IO_ERROR)
    exit(4);
  if (r->outcome == GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED ||
      r->outcome == GENESIS_EXECUTION_COVERAGE_RUNNER_EXHAUSTED) {
    GenesisControlTransfer t;
    memset(&t, 0, sizeof(t));
    t.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    t.next_pc = runtime->pc;
    t.runner_dispatch_count = (uint32_t)(r->dispatches >= UINT32_MAX ? UINT32_MAX - 1U : r->dispatches);
    return t;
  }
  return r->transfer;
}
