/* SEG-021-T031: headless bounded live-frame capture (see frame_capture.h). */
#include "frame_capture.h"

#include <stdio.h>
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

GenesisFrameCaptureResult genesis_frame_capture_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                    const GenesisFrameCaptureOptions *options,
                                                    uint64_t total_dispatch_allowance) {
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
