/*
 * SEG-021-T031: production seam between a generated Genesis bridge program and the headless
 * frame capture (frame_capture.h). Mirrors the SEG-007-T254 viewer seam: the capture build
 * (tools/genesis_startup_bridge.py --capture-frames) compiles the UNMODIFIED generated main TU
 * with -Dgenesis_runtime_run=genesis_frame_capture_hook_run, so the generated main hands its own
 * GenesisRuntime, dispatcher and runner allowance to this function. No SDL, no decoding, no
 * second dispatcher. Options arrive via environment variables (already validated by the Python
 * driver and re-validated here) so the generated argv ABI is unchanged:
 *   SEGARECOMP_CAPTURE_DIR, SEGARECOMP_CAPTURE_FIRST, SEGARECOMP_CAPTURE_COUNT,
 *   SEGARECOMP_CAPTURE_STRIDE (optional, default 1), SEGARECOMP_CAPTURE_SLICE (optional, default 65536).
 * Prints one CAPTURE_SUMMARY line (ordinals/counts/digests of synthetic-or-local frames only;
 * callers decide what, if anything, becomes durable evidence).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_capture.h"
#include "runtime.h"

GenesisControlTransfer genesis_frame_capture_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                      uint32_t dispatch_allowance);

static int genesis_capture_parse_u64(const char *text, uint64_t *out) {
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

GenesisControlTransfer genesis_frame_capture_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                      uint32_t dispatch_allowance) {
  GenesisFrameCaptureOptions options;
  GenesisFrameCaptureResult r;
  uint64_t first = 0U, count = 0U, slice = 65536U, stride = 1U;
  const char *slice_text = getenv("SEGARECOMP_CAPTURE_SLICE");
  const char *stride_text = getenv("SEGARECOMP_CAPTURE_STRIDE");
  const char *name;
  uint32_t index;
  memset(&options, 0, sizeof(options));
  options.out_dir = getenv("SEGARECOMP_CAPTURE_DIR");
  if (options.out_dir == NULL || genesis_capture_parse_u64(getenv("SEGARECOMP_CAPTURE_FIRST"), &first) != 0 ||
      genesis_capture_parse_u64(getenv("SEGARECOMP_CAPTURE_COUNT"), &count) != 0 ||
      (slice_text != NULL && genesis_capture_parse_u64(slice_text, &slice) != 0) ||
      (stride_text != NULL && genesis_capture_parse_u64(stride_text, &stride) != 0) ||
      count > GENESIS_FRAME_CAPTURE_MAX_FRAMES || slice == 0U || slice > UINT32_MAX || stride == 0U ||
      stride > UINT32_MAX) {
    fprintf(stderr, "capture: malformed capture options\n");
    exit(3);
  }
  options.first_frame = first;
  options.frame_count = (uint32_t)count;
  options.slice_dispatches = (uint32_t)slice;
  options.frame_stride = (uint32_t)stride;
  r = genesis_frame_capture_run(runtime, dispatch, &options, dispatch_allowance);
  name = r.outcome == GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE                ? "window_complete"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED ? "incomplete_runner_resource_limit"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP       ? "incomplete_guest_stop"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_COMPLETE   ? "incomplete_guest_complete"
         : r.outcome == GENESIS_FRAME_CAPTURE_IO_ERROR                    ? "io_error"
                                                                          : "invalid_argument";
  fprintf(stderr, "CAPTURE_SUMMARY {\"outcome\":\"%s\",\"first_frame\":%llu,\"frame_count\":%u,\"frame_stride\":%u,"
                  "\"frames_captured\":%u,\"frames_published\":%llu,\"dispatches\":%llu,\"digests\":[",
          name, (unsigned long long)first, (unsigned)options.frame_count, (unsigned)options.frame_stride,
          (unsigned)r.frames_captured,
          (unsigned long long)r.frames_published, (unsigned long long)r.dispatches);
  for (index = 0; index < r.frames_captured; ++index) {
    unsigned byte;
    fprintf(stderr, "%s\"", index == 0U ? "" : ",");
    for (byte = 0; byte < 32U; ++byte) fprintf(stderr, "%02x", (unsigned)r.digests[index][byte]);
    fprintf(stderr, "\"");
  }
  fprintf(stderr, "]}\n");
  if (r.outcome == GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT || r.outcome == GENESIS_FRAME_CAPTURE_IO_ERROR) exit(4);
  if (r.outcome == GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE ||
      r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED) {
    GenesisControlTransfer t;
    memset(&t, 0, sizeof(t));
    t.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    t.next_pc = runtime->pc;
    t.runner_dispatch_count = (uint32_t)(r.dispatches > UINT32_MAX ? UINT32_MAX : r.dispatches);
    return t;
  }
  return r.transfer;
}
