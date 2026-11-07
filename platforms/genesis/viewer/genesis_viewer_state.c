#include "genesis_viewer_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_persistence.h"
#include "vdp_render.h"

#define SLOT_FILE_MAX 64U

struct GenesisViewerState {
  GenesisViewerStateConfig cfg;
  char state_dir[1024];
  char unavailable[96];
  /* Thumbnail copies handed to the viewer by slot_info (stable until the next slot_info call for that slot). */
  struct { uint8_t pixels[SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2]; } slot_thumb[SV_STATE_SLOTS];
  GenesisViewerStateStats stats;
};

uint64_t genesis_viewer_state_ticks_to_ms(uint64_t master_ticks) {
  const uint64_t rate = GENESIS_NTSC_MASTER_TICKS_PER_SECOND;
  return (master_ticks / rate) * 1000U + (master_ticks % rate) * 1000U / rate;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Thumbnails: nearest-neighbour reduction of the frame the guest itself published (never a window capture, never CRT/UI output).    */

int genesis_viewer_state_make_thumbnail(const GenesisFrameArtifact *frame, uint8_t *rgb565_out, size_t capacity, uint16_t *width, uint16_t *height) {
  static uint8_t row_rgb[GENESIS_FRAME_MAX_WIDTH * 3U];
  unsigned src_w, src_h, out_w, out_h, ox, oy, cached_row = UINT32_MAX;
  if (frame == NULL || rgb565_out == NULL || width == NULL || height == NULL || !genesis_frame_artifact_is_populated(frame)) return 0;
  src_w = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
  src_h = GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
  if (src_w == 0U || src_h == 0U || src_w > GENESIS_FRAME_MAX_WIDTH || src_h > GENESIS_FRAME_MAX_HEIGHT) return 0;
  /* fit inside 160x112 keeping the source aspect: the smaller of the two integer-ratio scales, in 1/1024 steps */
  {
    const unsigned scale_w = (SV_STATE_THUMB_MAX_W * 1024U) / src_w, scale_h = (SV_STATE_THUMB_MAX_H * 1024U) / src_h;
    const unsigned scale = scale_w < scale_h ? scale_w : scale_h;
    out_w = (src_w * scale) / 1024U;
    out_h = (src_h * scale) / 1024U;
    if (out_w == 0U) out_w = 1U;
    if (out_h == 0U) out_h = 1U;
    if (out_w > SV_STATE_THUMB_MAX_W) out_w = SV_STATE_THUMB_MAX_W;
    if (out_h > SV_STATE_THUMB_MAX_H) out_h = SV_STATE_THUMB_MAX_H;
  }
  if ((size_t)out_w * out_h * 2U > capacity) return 0;
  for (oy = 0U; oy < out_h; ++oy) {
    const unsigned sy = (unsigned)(((2U * oy + 1U) * src_h) / (2U * out_h));
    if (sy != cached_row) {
      if (genesis_vdp_frame_row_rgb888(frame, sy, row_rgb) != 0) return 0;
      cached_row = sy;
    }
    for (ox = 0U; ox < out_w; ++ox) {
      const unsigned sx = (unsigned)(((2U * ox + 1U) * src_w) / (2U * out_w));
      const uint8_t *px = &row_rgb[sx * 3U];
      const uint16_t v = (uint16_t)(((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3));
      uint8_t *dst = &rgb565_out[((size_t)oy * out_w + ox) * 2U];
      dst[0] = (uint8_t)(v & 0xFFU);
      dst[1] = (uint8_t)(v >> 8);
    }
  }
  *width = (uint16_t)out_w;
  *height = (uint16_t)out_h;
  return 1;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */

/* One line per state operation in the run log (the launcher and the tests read it): what happened, never any state content. */
static void log_event(const char *event, int slot, const char *status, const char *detail) {
  fprintf(stderr, "VIEWER_STATE {\"event\":\"%s\",\"slot\":%d,\"status\":\"%s\",\"detail\":\"%s\"}\n", event, slot, status, detail);
}

static void set_text(char *out, size_t cap, const char *text) {
  if (out != NULL && cap != 0U) snprintf(out, cap, "%s", text);
}

static void slot_file_name(int slot, char *out, size_t cap) {
  if (slot == 0) snprintf(out, cap, "quick.sgstate");
  else snprintf(out, cap, "slot-%d.sgstate", slot);
}

static int slot_valid(int slot) { return slot >= 0 && slot < SV_STATE_SLOTS; }

static int states_ready(const GenesisViewerState *s, char *why, size_t cap) {
  if (s->unavailable[0] != '\0') { set_text(why, cap, s->unavailable); return 0; }
  if (s->state_dir[0] == '\0') { set_text(why, cap, "No save location is configured for this session."); return 0; }
  return 1;
}

/* ---- provider: slots ---- */

static int p_states_available(void *ctx, char *why, size_t cap) { return states_ready((GenesisViewerState *)ctx, why, cap); }

static int p_slot_info(void *ctx, int slot, SvStateSlotInfo *out) {
  GenesisViewerState *s = (GenesisViewerState *)ctx;
  char name[SLOT_FILE_MAX];
  uint8_t *data = NULL;
  size_t size = 0U;
  GenesisPersistStatus read;
  if (!slot_valid(slot) || out == NULL) return -1;
  memset(out, 0, sizeof *out);
  if (!states_ready(s, NULL, 0U)) return 0;
  slot_file_name(slot, name, sizeof name);
  read = genesis_persist_read_file(s->state_dir, name, GENESIS_SNAPSHOT_MAX_FILE_BYTES, &data, &size);
  if (read == GENESIS_PERSIST_FRESH) return 0;
  if (read != GENESIS_PERSIST_OK) {
    out->damaged = 1;
    set_text(out->note, sizeof out->note, genesis_persist_status_text(read));
    return 0;
  }
  {
    GenesisSnapshotInfo info;
    const GenesisSnapshotStatus st = genesis_snapshot_validate(&s->cfg.machine, &s->cfg.identity, data, size, &info);
    if (st != GENESIS_SNAPSHOT_OK) {
      out->damaged = 1;
      set_text(out->note, sizeof out->note, genesis_snapshot_status_text(st));
    } else {
      out->occupied = 1;
      out->guest_ms = genesis_viewer_state_ticks_to_ms(info.guest_master_ticks);
      if (info.thumb_rgb565 != NULL && info.thumb_width != 0U) {
        memcpy(s->slot_thumb[slot].pixels, info.thumb_rgb565, (size_t)info.thumb_width * info.thumb_height * 2U);
        out->thumb.w = info.thumb_width;
        out->thumb.h = info.thumb_height;
        out->thumb.rgb565 = s->slot_thumb[slot].pixels;
      }
    }
  }
  free(data);
  return 0;
}

static int p_save_slot(void *ctx, int slot, char *error, size_t cap) {
  GenesisViewerState *s = (GenesisViewerState *)ctx;
  char name[SLOT_FILE_MAX], why[96];
  uint8_t thumb[SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2U];
  GenesisSnapshotThumbnail thumbnail;
  GenesisSnapshotBuffer buffer;
  GenesisSnapshotStatus st;
  GenesisPersistStatus written;
  if (!slot_valid(slot)) { set_text(error, cap, "invalid slot"); return -1; }
  if (!states_ready(s, why, sizeof why)) { set_text(error, cap, why); log_event("save", slot, "unavailable", why); return -1; }
  memset(&thumbnail, 0, sizeof thumbnail);
  if (genesis_viewer_state_make_thumbnail(s->cfg.latest_frame, thumb, sizeof thumb, &thumbnail.width, &thumbnail.height)) thumbnail.rgb565 = thumb;
  st = genesis_snapshot_capture(&s->cfg.machine, &s->cfg.identity, thumbnail.rgb565 != NULL ? &thumbnail : NULL, &buffer);
  if (st != GENESIS_SNAPSHOT_OK) { set_text(error, cap, genesis_snapshot_status_text(st)); log_event("save", slot, "failed", genesis_snapshot_status_text(st)); return -1; }
  /* the candidate must parse and belong to this machine before it can replace anything */
  st = genesis_snapshot_validate(&s->cfg.machine, &s->cfg.identity, buffer.data, buffer.size, NULL);
  if (st != GENESIS_SNAPSHOT_OK) { genesis_snapshot_buffer_free(&buffer); set_text(error, cap, genesis_snapshot_status_text(st)); log_event("save", slot, "failed", genesis_snapshot_status_text(st)); return -1; }
  slot_file_name(slot, name, sizeof name);
  written = genesis_persist_ensure_dir(s->state_dir);
  if (written == GENESIS_PERSIST_OK) written = genesis_persist_replace_file(s->state_dir, name, buffer.data, buffer.size, 0);
  genesis_snapshot_buffer_free(&buffer);
  if (written != GENESIS_PERSIST_OK) { set_text(error, cap, genesis_persist_status_text(written)); log_event("save", slot, "failed", genesis_persist_status_text(written)); return -1; }
  log_event("save", slot, "ok", name);
  return 0;
}

static int p_load_slot(void *ctx, int slot, char *error, size_t cap) {
  GenesisViewerState *s = (GenesisViewerState *)ctx;
  char name[SLOT_FILE_MAX], why[96];
  uint8_t *data = NULL;
  size_t size = 0U;
  GenesisPersistStatus read;
  GenesisSnapshotStatus st;
  if (!slot_valid(slot)) { set_text(error, cap, "invalid slot"); return -1; }
  if (!states_ready(s, why, sizeof why)) { set_text(error, cap, why); log_event("load", slot, "unavailable", why); return -1; }
  slot_file_name(slot, name, sizeof name);
  read = genesis_persist_read_file(s->state_dir, name, GENESIS_SNAPSHOT_MAX_FILE_BYTES, &data, &size);
  if (read == GENESIS_PERSIST_FRESH) { set_text(error, cap, "this slot is empty"); log_event("load", slot, "failed", "this slot is empty"); return -1; }
  if (read != GENESIS_PERSIST_OK) { set_text(error, cap, genesis_persist_status_text(read)); log_event("load", slot, "failed", genesis_persist_status_text(read)); return -1; }
  st = genesis_snapshot_restore(&s->cfg.machine, &s->cfg.identity, data, size);
  free(data);
  if (st != GENESIS_SNAPSHOT_OK) { set_text(error, cap, genesis_snapshot_status_text(st)); log_event("load", slot, "failed", genesis_snapshot_status_text(st)); return -1; }
  log_event("load", slot, "ok", name);
  ++s->stats.restores;
  if (s->cfg.after_restore != NULL) s->cfg.after_restore(s->cfg.after_restore_context);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */

GenesisViewerState *genesis_viewer_state_create(const GenesisViewerStateConfig *config) {
  GenesisViewerState *s;
  if (config == NULL || config->machine.runtime == NULL) return NULL;
  s = (GenesisViewerState *)calloc(1U, sizeof *s);
  if (s == NULL) return NULL;
  s->cfg = *config;
  if (config->state_dir != NULL) snprintf(s->state_dir, sizeof s->state_dir, "%s", config->state_dir);
  if (config->unavailable_reason != NULL) snprintf(s->unavailable, sizeof s->unavailable, "%s", config->unavailable_reason);
  return s;
}

void genesis_viewer_state_destroy(GenesisViewerState *state) { free(state); }

void genesis_viewer_state_provider(GenesisViewerState *state, SvStateProvider *out) {
  memset(out, 0, sizeof *out);
  out->ctx = state;
  out->states_available = p_states_available;
  out->slot_info = p_slot_info;
  out->save_slot = p_save_slot;
  out->load_slot = p_load_slot;
}

void genesis_viewer_state_after_slice(GenesisViewerState *state) { (void)state; }

void genesis_viewer_state_stats(const GenesisViewerState *state, GenesisViewerStateStats *out) {
  memset(out, 0, sizeof *out);
  if (state != NULL) *out = state->stats;
}
