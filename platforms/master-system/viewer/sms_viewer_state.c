#include "sms_viewer_state.h"

#include <stdlib.h>
#include <string.h>

struct SmsViewerState {
  SmsViewerStateConfig cfg;
  SvStateService *service;
};

int sms_viewer_state_make_thumbnail(const uint8_t *fb, uint32_t height, uint8_t *out, size_t capacity, uint16_t *width, uint16_t *height_out) {
  int tw, th, ox, oy;
  if (fb == NULL || out == NULL || width == NULL || height_out == NULL || (height != 192U && height != 224U)) return 0;
  sv_state_thumb_size((int)SMS_FB_WIDTH, (int)height, &tw, &th);
  if (tw <= 0 || th <= 0 || (size_t)tw * (size_t)th * 2U > capacity) return 0;
  for (oy = 0; oy < th; ++oy) {
    const uint32_t sy = (uint32_t)(((2 * oy + 1) * (int)height) / (2 * th));
    for (ox = 0; ox < tw; ++ox) {
      const uint32_t sx = (uint32_t)(((2 * ox + 1) * (int)SMS_FB_WIDTH) / (2 * tw));
      const uint32_t rgb = sms_render_rgb888(fb[sy * SMS_FB_WIDTH + sx]);
      sv_state_put_rgb565(&out[((size_t)oy * (size_t)tw + (size_t)ox) * 2U], (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    }
  }
  *width = (uint16_t)tw;
  *height_out = (uint16_t)th;
  return 1;
}

static SmsSnapshotMachine snap_of(const SmsViewerState *s) {
  SmsSnapshotMachine m;
  memset(&m, 0, sizeof m);
  m.machine = &s->cfg.rig->machine;
  m.vdp = &s->cfg.rig->vdp;
  m.renderer = &s->cfg.rig->renderer;
  m.psg = &s->cfg.rig->psg;
  m.pad = &s->cfg.rig->pad;
  m.script_input = 0;   /* the viewer's input is live (and recording sessions are refused before this adapter exists) */
  return m;
}

static void set_text(char *out, size_t cap, const char *text) {
  if (out != NULL && cap != 0U) strncpy(out, text, cap - 1U), out[cap - 1U] = '\0';
}

static int op_capture(void *ctx, uint8_t **data, uint32_t *size, char *why, size_t cap) {
  SmsViewerState *s = (SmsViewerState *)ctx;
  const SmsSnapshotMachine m = snap_of(s);
  uint8_t thumb[SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2U];
  SmsSnapshotThumbnail thumbnail;
  SmsSnapshotBuffer buffer;
  SmsSnapshotStatus st;
  memset(&thumbnail, 0, sizeof thumbnail);
  if (sms_viewer_state_make_thumbnail(m.renderer->last, m.renderer->last_height, thumb, sizeof thumb, &thumbnail.width, &thumbnail.height)) thumbnail.rgb565 = thumb;
  st = sms_snapshot_capture(&m, &s->cfg.identity, thumbnail.rgb565 != NULL ? &thumbnail : NULL, &buffer);
  if (st == SMS_SNAPSHOT_OK) { *data = buffer.data; *size = buffer.size; return SV_CAPTURE_OK; }
  set_text(why, cap, sms_snapshot_status_text(st));
  return st == SMS_SNAPSHOT_NOT_CAPTURABLE ? SV_CAPTURE_SKIP : st == SMS_SNAPSHOT_NO_MEMORY ? SV_CAPTURE_NO_MEMORY : SV_CAPTURE_FAILED;
}

static void fill_info(const SmsSnapshotInfo *in, SvStateInfo *out) {
  if (out == NULL) return;
  out->guest_ms = sms_snapshot_cycles_to_ms(in->guest_cycles);
  out->thumb_w = in->thumb_width;
  out->thumb_h = in->thumb_height;
  out->thumb = in->thumb_rgb565;
}

static int op_peek(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info) {
  SmsSnapshotInfo i;
  (void)ctx;
  if (sms_snapshot_inspect(data, size, &i) != SMS_SNAPSHOT_OK) return -1;
  fill_info(&i, info);
  return 0;
}

static int op_validate(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info, char *why, size_t cap) {
  SmsViewerState *s = (SmsViewerState *)ctx;
  const SmsSnapshotMachine m = snap_of(s);
  SmsSnapshotInfo i;
  const SmsSnapshotStatus st = sms_snapshot_validate(&m, &s->cfg.identity, data, size, &i);
  if (st != SMS_SNAPSHOT_OK) { set_text(why, cap, sms_snapshot_status_text(st)); return -1; }
  fill_info(&i, info);
  return 0;
}

static int op_restore(void *ctx, const uint8_t *data, size_t size, char *why, size_t cap) {
  SmsViewerState *s = (SmsViewerState *)ctx;
  const SmsSnapshotMachine m = snap_of(s);
  const SmsSnapshotStatus st = sms_snapshot_restore(&m, &s->cfg.identity, data, size);
  if (st != SMS_SNAPSHOT_OK) { set_text(why, cap, sms_snapshot_status_text(st)); return -1; }
  return 0;
}

/* The rig-side consequences of a restore, at the barrier: host audio of the abandoned future is dropped, the presentation-only extension is re-derived by the next
 * frame, the input recorder agrees with the restored pad state, and the first frame after the restore is always presented. */
static void op_after_restore(void *ctx) {
  SmsViewerState *s = (SmsViewerState *)ctx;
  SmsViewerRig *rig = s->cfg.rig;
  sn76489_ring_clear(&rig->ring);
  sms_extension_reset(rig->extension);
  rig->recorded = rig->machine.input;
  rig->presented = UINT64_MAX;
  if (s->cfg.after_restore != NULL) s->cfg.after_restore(s->cfg.after_restore_context);
}

static uint64_t op_guest_ms(void *ctx) { return sms_snapshot_cycles_to_ms(((SmsViewerState *)ctx)->cfg.rig->machine.rt.state.cycles); }

SmsViewerState *sms_viewer_state_create(const SmsViewerStateConfig *config) {
  SmsViewerState *s;
  SvStateServiceConfig sc;
  if (config == NULL || config->rig == NULL) return NULL;
  s = (SmsViewerState *)calloc(1U, sizeof *s);
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
  sc.max_file_bytes = SMS_SNAPSHOT_MAX_FILE_BYTES;
  sc.rewind_budget_bytes = config->rewind_budget_bytes;
  sc.rewind_interval_ms = config->rewind_interval_ms;
  sc.rewind_target_ms = config->rewind_target_ms;
  s->service = sv_state_service_create(&sc);
  if (s->service == NULL) { free(s); return NULL; }
  return s;
}

void sms_viewer_state_destroy(SmsViewerState *s) {
  if (s == NULL) return;
  sv_state_service_destroy(s->service);
  free(s);
}

void sms_viewer_state_provider(SmsViewerState *s, SvStateProvider *out) { sv_state_service_provider(s->service, out); }
void sms_viewer_state_after_slice(SmsViewerState *s) { if (s != NULL) sv_state_service_after_slice(s->service); }
void sms_viewer_state_stats(const SmsViewerState *s, SvStateServiceStats *out) { sv_state_service_stats(s != NULL ? s->service : NULL, out); }

int sms_viewer_state_canonical_sha256(const SmsViewerState *s, uint8_t out[32]) {
  const SmsSnapshotMachine m = snap_of(s);
  SmsSnapshotBuffer buffer;
  SmsSha256 sha;
  if (sms_snapshot_capture(&m, &s->cfg.identity, NULL, &buffer) != SMS_SNAPSHOT_OK) return -1;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, buffer.data, buffer.size);
  sms_sha256_final(&sha, out);
  sms_snapshot_buffer_free(&buffer);
  return 0;
}
