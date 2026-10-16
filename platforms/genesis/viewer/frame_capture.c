/* SEG-021-T031: headless bounded live-frame capture (see frame_capture.h). */
#include "frame_capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_export.h"
#include "vdp_render.h"

typedef struct GenesisFrameCaptureSession {
  const GenesisFrameCaptureOptions *options;
  GenesisFrameCaptureResult *result;
  uint64_t produced;
  int io_error;
} GenesisFrameCaptureSession;

static GenesisFrameCaptureSession *g_capture_session;

/* Records the frame when its publication ordinal is inside the window. The runtime publishes (and increments its
   sequence) only after a successful render, so `produced` equals the publication ordinal. */
static int genesis_frame_capture_record(int status, GenesisFrameArtifact *frame_out) {
  GenesisFrameCaptureSession *session = g_capture_session;
  if (status != 0 || session == NULL) return status;
  session->produced++;
  /* After a failed write nothing more is captured, so digests stay aligned with selected ordinals. */
  if (!session->io_error && session->produced >= session->options->first_frame &&
      (session->produced - session->options->first_frame) % session->options->frame_stride == 0U &&
      session->result->frames_captured < session->options->frame_count) {
    char path[GENESIS_FRAME_CAPTURE_PATH_MAX];
    const int written = snprintf(path, sizeof(path), "%s/frame-%08llu.ppm", session->options->out_dir,
                                 (unsigned long long)session->produced);
    if (written <= 0 || (size_t)written >= sizeof(path) ||
        genesis_frame_export_ppm_to_path(frame_out, path) != GENESIS_FRAME_EXPORT_STATUS_OK) {
      session->io_error = 1;
    } else {
      memcpy(session->result->digests[session->result->frames_captured], frame_out->frame_digest, 32U);
      session->result->frames_captured++;
    }
  }
  return status;
}

/* Producer wrapper: renders exactly like the viewer's producer (the scanline-aware one, SEG-055), then records. */
static int genesis_frame_capture_raster_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                                 const GenesisRasterFrameState *raster,
                                                 GenesisFrameArtifact *frame_out) {
  return genesis_frame_capture_record(genesis_vdp_produce_frame_raster(vram, raster, frame_out), frame_out);
}

/* The legacy single-snapshot producer remains the observer's own `producer` (used when no raster producer is set). */
static int genesis_frame_capture_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                          const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                          const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                                          const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                          GenesisFrameArtifact *frame_out) {
  return genesis_frame_capture_record(genesis_vdp_produce_frame(vram, vsram, cram, registers, frame_out), frame_out);
}

static GenesisFrameCaptureResult genesis_frame_capture_finish(GenesisRuntime *runtime, GenesisFrameCaptureResult r,
                                                              GenesisFrameCaptureOutcome outcome,
                                                              const GenesisLiveFrameObserver *observer) {
  if (observer != NULL) r.frames_published = observer->sequence;
  if (runtime != NULL) {
    if (runtime->live_raster != NULL) {
      r.render_rejections = runtime->live_raster->render_rejections;
      r.vram_journal_overflows = runtime->live_raster->vram_journal_overflows;
    }
    runtime->live_frame_observer = NULL;
    runtime->live_raster = NULL;
  }
  g_capture_session = NULL;
  r.outcome = outcome;
  return r;
}

static int genesis_pad_script_valid(const GenesisPadScriptEntry *script, uint32_t count) {
  uint32_t index;
  for (index = 0U; index < count; ++index) {
    if (script[index].player != 1U && script[index].player != 2U) return 0;
    if (index != 0U && script[index].frame < script[index - 1U].frame) return 0;
  }
  return 1;
}

/* Applies, in script order, every not-yet-applied entry whose keyed frame has been reached by the guest's virtual time. Called
   only between runner slices, so it can never alter what a slice executes. */
static void genesis_pad_script_apply(GenesisRuntime *runtime, const GenesisFrameCaptureOptions *options, uint32_t *cursor,
                                     GenesisFrameCaptureResult *result) {
  const uint64_t frame = runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  while (*cursor < options->pad_script_count && options->pad_script[*cursor].frame <= frame) {
    const GenesisPadScriptEntry *entry = &options->pad_script[*cursor];
    if (entry->player == 1U) genesis_runtime_set_pad1(runtime, entry->mask);
    else genesis_runtime_set_pad2(runtime, entry->mask);
    ++*cursor;
    result->pad_entries_applied = *cursor;
  }
}

GenesisFrameCaptureResult genesis_frame_capture_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                    const GenesisFrameCaptureOptions *options,
                                                    uint64_t total_dispatch_allowance) {
  uint32_t pad_cursor = 0U;
  GenesisFrameCaptureResult r;
  GenesisLiveFrameObserver observer;
  GenesisFrameArtifact latest;
  GenesisFrameCaptureSession session;
  GenesisRasterCapture raster_capture; /* SEG-055: host-owned per-scanline capture buffer */
  memset(&r, 0, sizeof(r));
  if (runtime == NULL || dispatch == NULL || options == NULL || options->out_dir == NULL ||
      options->out_dir[0] == '\0' || options->first_frame == 0U || options->frame_count == 0U ||
      options->frame_count > GENESIS_FRAME_CAPTURE_MAX_FRAMES || options->slice_dispatches == 0U ||
      options->frame_stride == 0U ||
      options->first_frame > UINT64_MAX - (uint64_t)options->frame_count * options->frame_stride ||
      total_dispatch_allowance == 0U ||
      (options->pad_script_count != 0U && options->pad_script == NULL) ||
      options->pad_script_count > GENESIS_PAD_SCRIPT_MAX_ENTRIES ||
      !genesis_pad_script_valid(options->pad_script, options->pad_script_count) ||
      runtime->live_frame_observer != NULL || g_capture_session != NULL) {
    r.outcome = GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT;
    return r;
  }
  memset(&latest, 0, sizeof(latest));
  memset(&session, 0, sizeof(session));
  memset(&raster_capture, 0, sizeof(raster_capture));
  raster_capture.producer = genesis_frame_capture_raster_producer;
  session.options = options;
  session.result = &r;
  observer.producer = genesis_frame_capture_producer;
  observer.latest = &latest;
  observer.sequence = 0U;
  g_capture_session = &session;
  runtime->live_frame_observer = &observer;
  runtime->live_raster = &raster_capture;
  for (;;) {
    const uint64_t remaining = total_dispatch_allowance - r.dispatches;
    uint32_t slice;
    GenesisControlTransfer t;
    if (session.io_error) return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_IO_ERROR, &observer);
    if (r.frames_captured == options->frame_count)
      return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE, &observer);
    if (remaining == 0U)
      return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED, &observer);
    slice = remaining < options->slice_dispatches ? (uint32_t)remaining : options->slice_dispatches;
    genesis_pad_script_apply(runtime, options, &pad_cursor, &r);
    t = genesis_runtime_run(runtime, dispatch, slice);
    r.transfer = t;
    if (t.kind == GENESIS_RUNNER_RESOURCE_LIMIT) {
      r.dispatches += t.runner_dispatch_count;
      continue;
    }
    r.dispatches += 1U; /* terminal slice: count is only a lower bound */
    if (session.io_error) return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_IO_ERROR, &observer);
    if (r.frames_captured == options->frame_count)
      return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE, &observer);
    if (t.kind == GENESIS_STOP)
      return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP, &observer);
    if (t.kind == GENESIS_COMPLETE)
      return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_COMPLETE, &observer);
    return genesis_frame_capture_finish(runtime, r, GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT, &observer);
  }
}

/* ---- SEG-068-T007: environment-driven driver shared by every generated-main hook ---- */
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

/* Parses "<frame>:<player>:<mask>[,<frame>:<player>:<mask>...]"; a mask is decimal or 0x-hex (<= 255). Returns the entry count or
   -1 when malformed / too long / out of order. */
static int genesis_pad_script_parse(const char *text, GenesisPadScriptEntry *out) {
  uint32_t count = 0U;
  const char *cursor = text;
  while (*cursor != '\0') {
    char *end = NULL;
    unsigned long long frame, player, mask;
    if (count == GENESIS_PAD_SCRIPT_MAX_ENTRIES || *cursor < '0' || *cursor > '9') return -1;
    errno = 0;
    frame = strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || *end != ':') return -1;
    cursor = end + 1;
    if (*cursor < '0' || *cursor > '9') return -1;
    player = strtoull(cursor, &end, 10);
    if (end == cursor || *end != ':' || (player != 1U && player != 2U)) return -1;
    cursor = end + 1;
    if (*cursor < '0' || *cursor > '9') return -1;
    errno = 0;
    mask = strtoull(cursor, &end, 0); /* base 0: decimal or 0x-hex */
    if (errno != 0 || end == cursor || mask > 255U) return -1;
    cursor = end;
    if (*cursor == ',') ++cursor;
    else if (*cursor != '\0') return -1;
    out[count].frame = (uint64_t)frame;
    out[count].player = (uint8_t)player;
    out[count].mask = (uint8_t)mask;
    ++count;
  }
  return genesis_pad_script_valid(out, count) ? (int)count : -1;
}

int genesis_frame_capture_requested(void) {
  return getenv("SEGARECOMP_CAPTURE_DIR") != NULL;
}

GenesisControlTransfer genesis_frame_capture_run_from_environment(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                                  uint32_t dispatch_allowance) {
  static GenesisPadScriptEntry pad_script[GENESIS_PAD_SCRIPT_MAX_ENTRIES];
  GenesisFrameCaptureOptions options;
  GenesisFrameCaptureResult r;
  uint64_t first = 0U, count = 0U, slice = 65536U, stride = 1U;
  const char *slice_text = getenv("SEGARECOMP_CAPTURE_SLICE");
  const char *stride_text = getenv("SEGARECOMP_CAPTURE_STRIDE");
  const char *script_text = getenv("SEGARECOMP_PAD_SCRIPT");
  const char *name;
  uint32_t index;
  int script_count = 0;
  memset(&options, 0, sizeof(options));
  options.out_dir = getenv("SEGARECOMP_CAPTURE_DIR");
  if (script_text != NULL && (script_count = genesis_pad_script_parse(script_text, pad_script)) < 0) {
    fprintf(stderr, "capture: malformed SEGARECOMP_PAD_SCRIPT\n");
    exit(3);
  }
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
  options.pad_script = script_count > 0 ? pad_script : NULL;
  options.pad_script_count = (uint32_t)script_count;
  r = genesis_frame_capture_run(runtime, dispatch, &options, dispatch_allowance);
  name = r.outcome == GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE                ? "window_complete"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED ? "incomplete_runner_resource_limit"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP       ? "incomplete_guest_stop"
         : r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_COMPLETE   ? "incomplete_guest_complete"
         : r.outcome == GENESIS_FRAME_CAPTURE_IO_ERROR                    ? "io_error"
                                                                          : "invalid_argument";
  if (r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP && getenv("SEGARECOMP_STOP_WORK_RAM_DUMP") != NULL) {
    FILE *dump = fopen(getenv("SEGARECOMP_STOP_WORK_RAM_DUMP"), "wb");
    unsigned char header[8];
    const uint32_t stop_class = (uint32_t)r.transfer.stop.stop_class;
    header[0] = (unsigned char)(runtime->pc >> 24); header[1] = (unsigned char)(runtime->pc >> 16);
    header[2] = (unsigned char)(runtime->pc >> 8);  header[3] = (unsigned char)runtime->pc;
    header[4] = (unsigned char)(stop_class >> 24);  header[5] = (unsigned char)(stop_class >> 16);
    header[6] = (unsigned char)(stop_class >> 8);   header[7] = (unsigned char)stop_class;
    if (dump == NULL || fwrite(header, 1, sizeof(header), dump) != sizeof(header) ||
        fwrite(runtime->work_ram, 1, sizeof(runtime->work_ram), dump) != sizeof(runtime->work_ram) ||
        fclose(dump) != 0) {
      fprintf(stderr, "capture: cannot write stop work-RAM dump\n");
      exit(4);
    }
  }
  fprintf(stderr, "CAPTURE_SUMMARY {\"outcome\":\"%s\",\"first_frame\":%llu,\"frame_count\":%u,\"frame_stride\":%u,"
                  "\"frames_captured\":%u,\"frames_published\":%llu,\"dispatches\":%llu,\"pad_entries_applied\":%u,\"digests\":[",
          name, (unsigned long long)first, (unsigned)options.frame_count, (unsigned)options.frame_stride,
          (unsigned)r.frames_captured,
          (unsigned long long)r.frames_published, (unsigned long long)r.dispatches, (unsigned)r.pad_entries_applied);
  for (index = 0; index < r.frames_captured; ++index) {
    unsigned byte;
    fprintf(stderr, "%s\"", index == 0U ? "" : ",");
    for (byte = 0; byte < 32U; ++byte) fprintf(stderr, "%02x", (unsigned)r.digests[index][byte]);
    fprintf(stderr, "\"");
  }
  fprintf(stderr, "]}\n");
  genesis_runtime_write_notes(runtime, r.render_rejections, r.vram_journal_overflows, stderr); /* SEG-055: region / undelivered H-interrupts / rejected frames, when notable */
  if (r.outcome == GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT || r.outcome == GENESIS_FRAME_CAPTURE_IO_ERROR) exit(4);
  if (r.outcome == GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE ||
      r.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED) {
    GenesisControlTransfer t;
    memset(&t, 0, sizeof(t));
    t.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    t.next_pc = runtime->pc;
    t.runner_dispatch_count = (uint32_t)(r.dispatches >= UINT32_MAX ? UINT32_MAX - 1U : r.dispatches) /* UINT32_MAX is reserved: "runner allowance exhausted" */;
    return t;
  }
  return r.transfer;
}
