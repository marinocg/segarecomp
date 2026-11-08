#include "sv_state_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sv_state_store.h"

#define REWIND_MAX_ENTRIES 512U
#define SLOT_NAME_MAX 64U

/* One rolling-history entry: a complete container (its thumbnail rides inside it) plus the guest clock it was taken at. */
typedef struct RewindEntry {
  uint8_t *data;
  uint32_t size;
  uint64_t guest_ms;
  uint16_t thumb_w, thumb_h;
  const uint8_t *thumb;   /* points into `data` */
} RewindEntry;

struct SvStateService {
  SvStateServiceConfig cfg;
  char state_dir[1024];
  char unavailable[120];
  RewindEntry history[REWIND_MAX_ENTRIES];   /* oldest first; bounded by age AND by the byte budget; in memory only, never on disk */
  uint32_t history_count;
  uint64_t history_bytes;
  uint64_t last_capture_ms, interval_ms, target_ms;
  char rewind_off[120];           /* non-empty: rewind is unavailable and this is why */
  char history_limit_note[120];   /* non-empty: the retained history is shorter than the target (memory pressure) */
  uint64_t anchor_ms;             /* the live guest time referenced while the rewind surface is open */
  int anchor_open;
  struct { uint8_t pixels[SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2]; } slot_thumb[SV_STATE_SLOTS];   /* copies handed to the viewer by slot_info */
  SvStateServiceStats stats;
};

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

static int states_ready(const SvStateService *s, char *why, size_t cap) {
  if (s->unavailable[0] != '\0') { set_text(why, cap, s->unavailable); return 0; }
  if (s->state_dir[0] == '\0') { set_text(why, cap, "No save location is configured for this session."); return 0; }
  return 1;
}

void sv_state_thumb_size(int src_w, int src_h, int *out_w, int *out_h) {
  unsigned scale_w, scale_h, scale, w, h;
  if (src_w <= 0 || src_h <= 0) { *out_w = 0; *out_h = 0; return; }
  scale_w = ((unsigned)SV_STATE_THUMB_MAX_W * 1024U) / (unsigned)src_w;
  scale_h = ((unsigned)SV_STATE_THUMB_MAX_H * 1024U) / (unsigned)src_h;
  scale = scale_w < scale_h ? scale_w : scale_h;
  w = ((unsigned)src_w * scale) / 1024U;
  h = ((unsigned)src_h * scale) / 1024U;
  if (w == 0U) w = 1U;
  if (h == 0U) h = 1U;
  if (w > SV_STATE_THUMB_MAX_W) w = SV_STATE_THUMB_MAX_W;
  if (h > SV_STATE_THUMB_MAX_H) h = SV_STATE_THUMB_MAX_H;
  *out_w = (int)w;
  *out_h = (int)h;
}

/* ---- rewind engine ---- */

static void entry_free(RewindEntry *e) {
  free(e->data);
  memset(e, 0, sizeof *e);
}

static void history_drop_oldest(SvStateService *s) {
  uint32_t i;
  if (s->history_count == 0U) return;
  s->history_bytes -= s->history[0].size;
  entry_free(&s->history[0]);
  for (i = 1U; i < s->history_count; ++i) s->history[i - 1U] = s->history[i];
  memset(&s->history[s->history_count - 1U], 0, sizeof(RewindEntry));
  --s->history_count;
  ++s->stats.evictions;
}

static void history_truncate_to(SvStateService *s, uint32_t keep) {   /* drops every entry newer than index keep-1 (the branch point) */
  while (s->history_count > keep) {
    RewindEntry *last = &s->history[s->history_count - 1U];
    s->history_bytes -= last->size;
    entry_free(last);
    --s->history_count;
  }
}

static void history_clear(SvStateService *s) { history_truncate_to(s, 0U); s->history_limit_note[0] = '\0'; }

static int rewind_enabled(const SvStateService *s) { return s->cfg.rewind_budget_bytes != 0U && s->rewind_off[0] == '\0'; }

static void history_refresh_stats(SvStateService *s) {
  s->stats.history_entries = s->history_count;
  s->stats.history_bytes = s->history_bytes;
}

static uint64_t now_ms(const SvStateService *s) { return s->cfg.ops.guest_ms(s->cfg.ops.ctx); }

void sv_state_service_after_slice(SvStateService *s) {
  uint64_t now;
  uint8_t *data = NULL;
  uint32_t size = 0U;
  char why[160] = "";
  int rc;
  SvStateInfo info;
  RewindEntry *entry;
  struct timespec t0, t1;
  if (s == NULL || !rewind_enabled(s)) return;
  now = now_ms(s);
  if (now < s->last_capture_ms) s->last_capture_ms = now;                   /* the guest clock never runs backwards except through a restore, which re-bases it */
  if (now - s->last_capture_ms < s->interval_ms) return;
  timespec_get(&t0, TIME_UTC);
  rc = s->cfg.ops.capture(s->cfg.ops.ctx, &data, &size, why, sizeof why);
  timespec_get(&t1, TIME_UTC);
  {
    const uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    s->stats.capture_ns_total += ns;
    if (ns > s->stats.capture_ns_max) s->stats.capture_ns_max = ns;
  }
  s->last_capture_ms = now;   /* a failed attempt is not retried every slice */
  if (rc == SV_CAPTURE_SKIP) return;   /* a latched stop: the guest is about to fail closed anyway */
  if (rc != SV_CAPTURE_OK) {
    ++s->stats.capture_failures;
    if (rc == SV_CAPTURE_NO_MEMORY) {   /* keep the game running; shorten the history and say so */
      while (s->history_count > s->history_count / 2U + 1U) history_drop_oldest(s);
      snprintf(s->history_limit_note, sizeof s->history_limit_note, "The computer is short of memory: only the last %u moments are kept.", (unsigned)s->history_count);
    } else {
      snprintf(s->rewind_off, sizeof s->rewind_off, "Rewind stopped: %.90s.", why[0] != '\0' ? why : "the game could not be saved");
      history_clear(s);
    }
    history_refresh_stats(s);
    return;
  }
  s->stats.last_capture_bytes = size;
  if (size > s->cfg.rewind_budget_bytes) {   /* a single snapshot that cannot fit even in an empty history: rewind is unavailable, the game is not affected */
    snprintf(s->rewind_off, sizeof s->rewind_off, "Rewind is unavailable: one saved moment (%u KB) does not fit the %u MB memory limit.", (unsigned)(size / 1024U),
             (unsigned)(s->cfg.rewind_budget_bytes / (1024U * 1024U)));
    free(data);
    history_clear(s);
    history_refresh_stats(s);
    return;
  }
  while (s->history_count != 0U && (s->history_bytes + size > s->cfg.rewind_budget_bytes || s->history_count >= REWIND_MAX_ENTRIES)) history_drop_oldest(s);   /* by budget */
  while (s->history_count != 0U && now - s->history[0].guest_ms > s->target_ms) history_drop_oldest(s);                                                       /* by age */
  memset(&info, 0, sizeof info);
  if (s->cfg.ops.peek(s->cfg.ops.ctx, data, size, &info) != 0) { free(data); ++s->stats.capture_failures; return; }
  entry = &s->history[s->history_count++];
  entry->data = data;
  entry->size = size;
  entry->guest_ms = info.guest_ms;
  entry->thumb_w = info.thumb_w;
  entry->thumb_h = info.thumb_h;
  entry->thumb = info.thumb;
  s->history_bytes += size;
  ++s->stats.captures;
  /* the retained span is shorter than the target only when the budget (not the age) is what evicted: tell the player the real figure */
  if (s->history_count != 0U && s->stats.evictions != 0U && now - s->history[0].guest_ms < s->target_ms - s->target_ms / 10U &&
      s->history_bytes + size > s->cfg.rewind_budget_bytes - size)
    snprintf(s->history_limit_note, sizeof s->history_limit_note, "Memory limit reached: the last %u seconds are kept.", (unsigned)((now - s->history[0].guest_ms) / 1000U));
  else
    s->history_limit_note[0] = '\0';
  s->stats.retained_span_ms = s->history_count != 0U ? now - s->history[0].guest_ms : 0U;
  history_refresh_stats(s);
}

static int p_rewind_available(void *ctx, char *why, size_t cap) {
  SvStateService *s = (SvStateService *)ctx;
  if (s->cfg.rewind_budget_bytes == 0U) { set_text(why, cap, "Rewind is turned off."); return 0; }
  if (s->rewind_off[0] != '\0') { set_text(why, cap, s->rewind_off); return 0; }
  return 1;
}

static int p_history_count(void *ctx) { return (int)((SvStateService *)ctx)->history_count; }

static int p_history_info(void *ctx, int index, SvRewindEntry *out) {
  SvStateService *s = (SvStateService *)ctx;
  const uint64_t now = now_ms(s);
  const RewindEntry *e;
  if (index < 0 || (uint32_t)index >= s->history_count || out == NULL) return -1;
  e = &s->history[index];
  memset(out, 0, sizeof *out);
  out->age_ms = (int64_t)(now >= e->guest_ms ? now - e->guest_ms : 0U);
  out->thumb.w = e->thumb_w;
  out->thumb.h = e->thumb_h;
  out->thumb.rgb565 = e->thumb;
  return 0;
}

static int p_restore_entry(void *ctx, int index, char *error, size_t cap) {
  SvStateService *s = (SvStateService *)ctx;
  char why[160] = "";
  if (index < 0 || (uint32_t)index >= s->history_count) { set_text(error, cap, "that moment is no longer available"); return -1; }
  if (s->cfg.ops.restore(s->cfg.ops.ctx, s->history[index].data, s->history[index].size, why, sizeof why) != 0) {   /* two-phase restore: the running game is unchanged */
    set_text(error, cap, why);
    log_event("rewind", index, "failed", why);
    return -1;
  }
  history_truncate_to(s, (uint32_t)index + 1U);   /* the new future is a new branch: no entry newer than the chosen one survives */
  s->last_capture_ms = now_ms(s);
  s->anchor_open = 0;
  ++s->stats.restores;
  history_refresh_stats(s);
  log_event("rewind", index, "ok", "restored");
  if (s->cfg.ops.after_restore != NULL) s->cfg.ops.after_restore(s->cfg.ops.ctx);
  return 0;
}

static void p_rewind_opened(void *ctx) {
  SvStateService *s = (SvStateService *)ctx;
  s->anchor_ms = now_ms(s);   /* the live machine IS the anchor: nothing runs while the surface is open */
  s->anchor_open = 1;
}

static void p_rewind_cancelled(void *ctx) {
  SvStateService *s = (SvStateService *)ctx;
  if (s->anchor_open && now_ms(s) != s->anchor_ms) log_event("rewind", -1, "anchor_moved", "the machine changed while browsing");   /* never expected: reported rather than hidden */
  s->anchor_open = 0;
}

static int p_history_note(void *ctx, char *note, size_t cap) {
  SvStateService *s = (SvStateService *)ctx;
  if (s->history_limit_note[0] == '\0') return 0;
  set_text(note, cap, s->history_limit_note);
  return 1;
}

/* ---- provider: slots ---- */

static int p_states_available(void *ctx, char *why, size_t cap) { return states_ready((SvStateService *)ctx, why, cap); }

static int p_slot_info(void *ctx, int slot, SvStateSlotInfo *out) {
  SvStateService *s = (SvStateService *)ctx;
  char name[SLOT_NAME_MAX];
  uint8_t *data = NULL;
  size_t size = 0U;
  SvStoreStatus read;
  if (!slot_valid(slot) || out == NULL) return -1;
  memset(out, 0, sizeof *out);
  if (!states_ready(s, NULL, 0U)) return 0;
  slot_file_name(slot, name, sizeof name);
  read = sv_store_read_file(s->state_dir, name, s->cfg.max_file_bytes, &data, &size);
  if (read == SV_STORE_FRESH) return 0;
  if (read != SV_STORE_OK) {
    out->damaged = 1;
    set_text(out->note, sizeof out->note, sv_store_status_text(read));
    return 0;
  }
  {
    SvStateInfo info;
    char why[sizeof out->note] = "";
    memset(&info, 0, sizeof info);
    if (s->cfg.ops.validate(s->cfg.ops.ctx, data, size, &info, why, sizeof why) != 0) {
      out->damaged = 1;
      set_text(out->note, sizeof out->note, why);
    } else {
      out->occupied = 1;
      out->guest_ms = info.guest_ms;
      if (info.thumb != NULL && info.thumb_w != 0U && (size_t)info.thumb_w * info.thumb_h * 2U <= sizeof s->slot_thumb[slot].pixels) {
        memcpy(s->slot_thumb[slot].pixels, info.thumb, (size_t)info.thumb_w * info.thumb_h * 2U);
        out->thumb.w = info.thumb_w;
        out->thumb.h = info.thumb_h;
        out->thumb.rgb565 = s->slot_thumb[slot].pixels;
      }
    }
  }
  free(data);
  return 0;
}

static int p_save_slot(void *ctx, int slot, char *error, size_t cap) {
  SvStateService *s = (SvStateService *)ctx;
  char name[SLOT_NAME_MAX], why[160] = "";
  uint8_t *data = NULL;
  uint32_t size = 0U;
  int rc;
  SvStoreStatus written;
  if (!slot_valid(slot)) { set_text(error, cap, "invalid slot"); return -1; }
  if (!states_ready(s, why, sizeof why)) { set_text(error, cap, why); log_event("save", slot, "unavailable", why); return -1; }
  rc = s->cfg.ops.capture(s->cfg.ops.ctx, &data, &size, why, sizeof why);
  if (rc != SV_CAPTURE_OK) {
    if (why[0] == '\0') set_text(why, sizeof why, "the game cannot be saved right now");
    set_text(error, cap, why);
    log_event("save", slot, "failed", why);
    return -1;
  }
  /* the candidate must parse and belong to this machine before it can replace anything */
  why[0] = '\0';
  if (s->cfg.ops.validate(s->cfg.ops.ctx, data, size, NULL, why, sizeof why) != 0) {
    free(data);
    set_text(error, cap, why);
    log_event("save", slot, "failed", why);
    return -1;
  }
  slot_file_name(slot, name, sizeof name);
  written = sv_store_ensure_dir(s->state_dir);
  if (written == SV_STORE_OK) written = sv_store_replace_file(s->state_dir, name, data, size, 0);
  free(data);
  if (written != SV_STORE_OK) { set_text(error, cap, sv_store_status_text(written)); log_event("save", slot, "failed", sv_store_status_text(written)); return -1; }
  log_event("save", slot, "ok", name);
  return 0;
}

static int p_load_slot(void *ctx, int slot, char *error, size_t cap) {
  SvStateService *s = (SvStateService *)ctx;
  char name[SLOT_NAME_MAX], why[160] = "";
  uint8_t *data = NULL;
  size_t size = 0U;
  SvStoreStatus read;
  if (!slot_valid(slot)) { set_text(error, cap, "invalid slot"); return -1; }
  if (!states_ready(s, why, sizeof why)) { set_text(error, cap, why); log_event("load", slot, "unavailable", why); return -1; }
  slot_file_name(slot, name, sizeof name);
  read = sv_store_read_file(s->state_dir, name, s->cfg.max_file_bytes, &data, &size);
  if (read == SV_STORE_FRESH) { set_text(error, cap, "this slot is empty"); log_event("load", slot, "failed", "this slot is empty"); return -1; }
  if (read != SV_STORE_OK) { set_text(error, cap, sv_store_status_text(read)); log_event("load", slot, "failed", sv_store_status_text(read)); return -1; }
  if (s->cfg.ops.restore(s->cfg.ops.ctx, data, size, why, sizeof why) != 0) {
    free(data);
    set_text(error, cap, why);
    log_event("load", slot, "failed", why);
    return -1;
  }
  free(data);
  log_event("load", slot, "ok", name);
  history_clear(s);   /* the rolling history belonged to another timeline */
  s->last_capture_ms = now_ms(s);
  history_refresh_stats(s);
  ++s->stats.restores;
  if (s->cfg.ops.after_restore != NULL) s->cfg.ops.after_restore(s->cfg.ops.ctx);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */

SvStateService *sv_state_service_create(const SvStateServiceConfig *config) {
  SvStateService *s;
  if (config == NULL || config->ops.capture == NULL || config->ops.peek == NULL || config->ops.validate == NULL || config->ops.restore == NULL || config->ops.guest_ms == NULL) return NULL;
  s = (SvStateService *)calloc(1U, sizeof *s);
  if (s == NULL) return NULL;
  s->cfg = *config;
  if (config->state_dir != NULL) snprintf(s->state_dir, sizeof s->state_dir, "%s", config->state_dir);
  if (config->unavailable_reason != NULL) snprintf(s->unavailable, sizeof s->unavailable, "%s", config->unavailable_reason);
  if (config->rewind_unavailable_reason != NULL) snprintf(s->rewind_off, sizeof s->rewind_off, "%s", config->rewind_unavailable_reason);
  if (s->cfg.max_file_bytes == 0U) s->cfg.max_file_bytes = 4U * 1024U * 1024U;
  s->interval_ms = config->rewind_interval_ms != 0U ? config->rewind_interval_ms : SV_STATE_REWIND_INTERVAL_MS;
  s->target_ms = config->rewind_target_ms != 0U ? config->rewind_target_ms : SV_STATE_REWIND_TARGET_MS;
  s->last_capture_ms = config->ops.guest_ms(config->ops.ctx);
  return s;
}

void sv_state_service_destroy(SvStateService *s) {
  if (s == NULL) return;
  history_clear(s);
  free(s);
}

void sv_state_service_provider(SvStateService *s, SvStateProvider *out) {
  memset(out, 0, sizeof *out);
  out->ctx = s;
  out->states_available = p_states_available;
  out->slot_info = p_slot_info;
  out->save_slot = p_save_slot;
  out->load_slot = p_load_slot;
  out->rewind_available = p_rewind_available;
  out->history_count = p_history_count;
  out->history_info = p_history_info;
  out->restore_entry = p_restore_entry;
  out->rewind_opened = p_rewind_opened;
  out->rewind_cancelled = p_rewind_cancelled;
  out->history_note = p_history_note;
}

void sv_state_service_stats(const SvStateService *s, SvStateServiceStats *out) {
  memset(out, 0, sizeof *out);
  if (s != NULL) *out = s->stats;
}
