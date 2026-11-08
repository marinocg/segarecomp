#include "scd_viewer_state.h"

#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "scd_snapshot.h"
#include "vdp_render.h"

struct ScdViewerState {
  ScdViewerStateConfig cfg;
  SvStateService *service;
  const struct GenesisFrameArtifact *latest;
};

/* Nearest-neighbour reduction of the frame the guest itself published (never a window capture, never CRT/UI output). */
static int make_thumbnail(const GenesisFrameArtifact *frame, uint8_t *out, size_t capacity, uint16_t *width, uint16_t *height) {
  static uint8_t row_rgb[GENESIS_FRAME_MAX_WIDTH * 3U];
  unsigned src_w, src_h, ox, oy, cached_row = UINT32_MAX;
  int tw, th;
  if (frame == NULL || !genesis_frame_artifact_is_populated(frame)) return 0;
  src_w = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
  src_h = GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
  if (src_w == 0U || src_h == 0U || src_w > GENESIS_FRAME_MAX_WIDTH || src_h > GENESIS_FRAME_MAX_HEIGHT) return 0;
  sv_state_thumb_size((int)src_w, (int)src_h, &tw, &th);
  if (tw <= 0 || th <= 0 || (size_t)tw * (size_t)th * 2U > capacity) return 0;
  for (oy = 0U; oy < (unsigned)th; ++oy) {
    const unsigned sy = (unsigned)(((2U * oy + 1U) * src_h) / (2U * (unsigned)th));
    if (sy != cached_row) {
      if (genesis_vdp_frame_row_rgb888(frame, sy, row_rgb) != 0) return 0;
      cached_row = sy;
    }
    for (ox = 0U; ox < (unsigned)tw; ++ox) {
      const unsigned sx = (unsigned)(((2U * ox + 1U) * src_w) / (2U * (unsigned)tw));
      const uint8_t *px = &row_rgb[sx * 3U];
      sv_state_put_rgb565(&out[((size_t)oy * (size_t)tw + ox) * 2U], px[0], px[1], px[2]);
    }
  }
  *width = (uint16_t)tw;
  *height = (uint16_t)th;
  return 1;
}

static void set_text(char *out, size_t cap, const char *text) {
  if (out != NULL && cap != 0U) { strncpy(out, text, cap - 1U); out[cap - 1U] = '\0'; }
}

static int op_capture(void *ctx, uint8_t **data, uint32_t *size, char *why, size_t cap) {
  ScdViewerState *s = (ScdViewerState *)ctx;
  uint8_t thumb[SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2U];
  ScdSnapshotThumbnail thumbnail;
  ScdSnapshotBuffer buffer;
  ScdSnapshotStatus st;
  memset(&thumbnail, 0, sizeof thumbnail);
  if (make_thumbnail(s->latest, thumb, sizeof thumb, &thumbnail.width, &thumbnail.height)) thumbnail.rgb565 = thumb;
  st = scd_snapshot_capture(s->cfg.machine, thumbnail.rgb565 != NULL ? &thumbnail : NULL, &buffer);
  if (st == SCD_SNAPSHOT_OK) { *data = buffer.data; *size = buffer.size; return SV_CAPTURE_OK; }
  if (st == SCD_SNAPSHOT_NOT_CAPTURABLE) {
    const char *reason = scd_snapshot_refusal(s->cfg.machine);
    set_text(why, cap, reason != NULL ? reason : scd_snapshot_status_text(st));
    return SV_CAPTURE_SKIP;
  }
  set_text(why, cap, scd_snapshot_status_text(st));
  return st == SCD_SNAPSHOT_NO_MEMORY ? SV_CAPTURE_NO_MEMORY : SV_CAPTURE_FAILED;
}

static void fill_info(const ScdSnapshotInfo *in, SvStateInfo *out) {
  if (out == NULL) return;
  out->guest_ms = scd_snapshot_ticks_to_ms(in->guest_ticks);
  out->thumb_w = in->thumb_width;
  out->thumb_h = in->thumb_height;
  out->thumb = in->thumb_rgb565;
}

static int op_peek(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info) {
  ScdSnapshotInfo i;
  (void)ctx;
  if (scd_snapshot_inspect(data, size, &i) != SCD_SNAPSHOT_OK) return -1;
  fill_info(&i, info);
  return 0;
}

static int op_validate(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info, char *why, size_t cap) {
  ScdViewerState *s = (ScdViewerState *)ctx;
  ScdSnapshotInfo i;
  const ScdSnapshotStatus st = scd_snapshot_validate(s->cfg.machine, data, size, &i);
  if (st != SCD_SNAPSHOT_OK) {
    const char *reason = st == SCD_SNAPSHOT_NOT_CAPTURABLE ? scd_snapshot_refusal(s->cfg.machine) : NULL;
    set_text(why, cap, reason != NULL ? reason : scd_snapshot_status_text(st));
    return -1;
  }
  fill_info(&i, info);
  return 0;
}

static int op_restore(void *ctx, const uint8_t *data, size_t size, char *why, size_t cap) {
  ScdViewerState *s = (ScdViewerState *)ctx;
  const ScdSnapshotStatus st = scd_snapshot_restore(s->cfg.machine, data, size);
  if (st != SCD_SNAPSHOT_OK) { set_text(why, cap, scd_snapshot_status_text(st)); return -1; }
  return 0;
}

static void op_after_restore(void *ctx) {
  ScdViewerState *s = (ScdViewerState *)ctx;
  s->latest = NULL;   /* the picture of the abandoned future is not the thumbnail of a state saved next */
  if (s->cfg.after_restore != NULL) s->cfg.after_restore(s->cfg.after_restore_context);
}

static uint64_t op_guest_ms(void *ctx) { return scd_snapshot_ticks_to_ms(((ScdViewerState *)ctx)->cfg.machine->now); }

ScdViewerState *scd_viewer_state_create(const ScdViewerStateConfig *config) {
  ScdViewerState *s;
  SvStateServiceConfig sc;
  if (config == NULL || config->machine == NULL) return NULL;
  s = (ScdViewerState *)calloc(1U, sizeof *s);
  if (s == NULL) return NULL;
  s->cfg = *config;
  memset(&sc, 0, sizeof sc);
  sc.ops.ctx = s;
  sc.ops.capture = op_capture;
  sc.ops.peek = op_peek;
  sc.ops.validate = op_validate;
  sc.ops.restore = op_restore;
  sc.ops.after_restore = op_after_restore;
  sc.ops.guest_ms = op_guest_ms;
  sc.state_dir = config->state_dir;
  sc.unavailable_reason = config->unavailable_reason;
  sc.rewind_unavailable_reason = config->rewind_unavailable_reason;
  sc.max_file_bytes = SCD_SNAPSHOT_MAX_FILE_BYTES;
  sc.rewind_budget_bytes = config->rewind_budget_bytes;
  sc.rewind_interval_ms = config->rewind_interval_ms;
  sc.rewind_target_ms = config->rewind_target_ms;
  s->service = sv_state_service_create(&sc);
  if (s->service == NULL) { free(s); return NULL; }
  return s;
}

void scd_viewer_state_destroy(ScdViewerState *s) {
  if (s == NULL) return;
  sv_state_service_destroy(s->service);
  free(s);
}

void scd_viewer_state_provider(ScdViewerState *s, SvStateProvider *out) { sv_state_service_provider(s->service, out); }
void scd_viewer_state_note_frame(ScdViewerState *s, const struct GenesisFrameArtifact *frame) { if (s != NULL) s->latest = frame; }
void scd_viewer_state_after_slice(ScdViewerState *s) { if (s != NULL) sv_state_service_after_slice(s->service); }
void scd_viewer_state_stats(const ScdViewerState *s, SvStateServiceStats *out) { sv_state_service_stats(s != NULL ? s->service : NULL, out); }

int scd_viewer_state_canonical_sha256(const ScdViewerState *s, uint8_t out[32]) {
  ScdSnapshotBuffer buffer;
  GenesisSha256 sha;
  if (scd_snapshot_capture(s->cfg.machine, NULL, &buffer) != SCD_SNAPSHOT_OK) return -1;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, buffer.data, buffer.size);
  genesis_sha256_final(&sha, out);
  scd_snapshot_buffer_free(&buffer);
  return 0;
}
