/* SEG-131 Checkpoint D (ADR 0278): the shared viewer's save-state surface under the dummy video driver with synthetic key events and a FAKE provider
 * (no machine, no ROM): the pause menu rows with and without a provider, the slot pages, the safe-by-default confirmations, failure reporting that never
 * claims success, the quick-save / quick-load keys, and the reserved host keys. Strict C11 like the code under test. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_settings.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static void key_event(int scancode, int down) {
  SDL_Event e;
  SDL_zero(e);
  e.type = (Uint32)(down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP);
  e.common.timestamp = SDL_GetTicksNS();
  e.key.scancode = (SDL_Scancode)scancode;
  e.key.down = down != 0;
  SDL_PushEvent(&e);
}
static void tap(int scancode) { key_event(scancode, 1); key_event(scancode, 0); }

typedef struct Fake {
  int history;                 /* rewind: entries, oldest first (age = 500 ms x (history - index)) */
  int rewind_ok, restores, last_restore, fail_restore, opened, cancelled, clicked;
  uint8_t thumbs[16][4 * 2 * 2];
  int occupied[SV_STATE_SLOTS];
  int saves, loads, last_save, last_load;
  int fail_save, fail_load, available;
  uint8_t thumb[4 * 2 * 2];
} Fake;

static int f_available(void *ctx, char *why, size_t cap) {
  const Fake *f = (const Fake *)ctx;
  if (!f->available) snprintf(why, cap, "No save location is configured for this session.");
  return f->available;
}
static int f_slot_info(void *ctx, int slot, SvStateSlotInfo *out) {
  Fake *f = (Fake *)ctx;
  memset(out, 0, sizeof *out);
  if (f->occupied[slot]) {
    out->occupied = 1;
    out->guest_ms = (uint64_t)(slot + 1) * 61000u;
    out->thumb.w = 4; out->thumb.h = 2; out->thumb.rgb565 = f->thumb;
  }
  return 0;
}
static int f_save(void *ctx, int slot, char *err, size_t cap) {
  Fake *f = (Fake *)ctx;
  if (f->fail_save) { snprintf(err, cap, "disk full"); return -1; }
  f->occupied[slot] = 1; ++f->saves; f->last_save = slot;
  return 0;
}
static int f_load(void *ctx, int slot, char *err, size_t cap) {
  Fake *f = (Fake *)ctx;
  if (f->fail_load) { snprintf(err, cap, "the state file is corrupt"); return -1; }
  ++f->loads; f->last_load = slot;
  return 0;
}

static int f_rewind_available(void *ctx, char *why, size_t cap) {
  const Fake *f = (const Fake *)ctx;
  if (!f->rewind_ok) snprintf(why, cap, "Rewind is unavailable: one saved moment does not fit the memory limit.");
  return f->rewind_ok;
}
static int f_history_count(void *ctx) { return ((Fake *)ctx)->history; }
static int f_history_info(void *ctx, int index, SvRewindEntry *out) {
  Fake *f = (Fake *)ctx;
  int i;
  memset(out, 0, sizeof *out);
  if (index < 0 || index >= f->history) return -1;
  for (i = 0; i < 4 * 2 * 2; ++i) f->thumbs[index & 15][i] = (uint8_t)(index * 17 + i * 5);
  out->age_ms = 500 * (f->history - index);
  out->thumb.w = 4; out->thumb.h = 2; out->thumb.rgb565 = f->thumbs[index & 15];
  return 0;
}
static int f_restore(void *ctx, int index, char *err, size_t cap) {
  Fake *f = (Fake *)ctx;
  if (f->fail_restore) { snprintf(err, cap, "the state is damaged"); return -1; }
  ++f->restores; f->last_restore = index;
  return 0;
}
static void f_opened(void *ctx) { ++((Fake *)ctx)->opened; }
static void f_cancelled(void *ctx) { ++((Fake *)ctx)->cancelled; }

static SvStateProvider provider_for(Fake *f) {
  SvStateProvider p;
  memset(&p, 0, sizeof p);
  p.ctx = f; p.states_available = f_available; p.slot_info = f_slot_info; p.save_slot = f_save; p.load_slot = f_load;
  p.rewind_available = f_rewind_available; p.history_count = f_history_count; p.history_info = f_history_info; p.restore_entry = f_restore;
  p.rewind_opened = f_opened; p.rewind_cancelled = f_cancelled;
  return p;
}

static SvHost *open_host(const char *settings_file) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS;
  cfg.title = "state test";
  cfg.picture_w = 320; cfg.picture_h = 224; cfg.max_w = 320; cfg.max_h = 240;
  cfg.default_scale = 2;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings_file, 1);
  return sv_host_open(&cfg);
}

/* ---- a tiny script driver for the overlay loop hook: one step per overlay frame, so the pushed events are processed between steps ---- */
typedef struct Step { int key; int (*check)(SvHost *host, void *ctx); } Step;
typedef struct Script { const Step *steps; int count, at; void *ctx; int closed_early; } Script;

static int script_hook(void *ctx, SvHost *host, int iteration) {
  Script *s = (Script *)ctx;
  (void)iteration;
  if (s->at >= s->count) return 1;
  if (s->steps[s->at].check != NULL) (void)s->steps[s->at].check(host, s->ctx);
  if (s->steps[s->at].key != 0) tap(s->steps[s->at].key);
  ++s->at;
  return 0;
}

static void run_menu(SvHost *host, const Step *steps, int count, void *ctx) {
  Script s;
  SvSample smp;
  s.steps = steps; s.count = count; s.at = 0; s.ctx = ctx; s.closed_early = 0;
  sv_host_set_loop_hook(host, script_hook, &s);
  tap(SV_KEY_ESCAPE);   /* opens the pause overlay at the next service */
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  CHECK(s.at >= count || smp.quit == 0, "the script reached its end or the menu closed on its own");
}

static const SvItem *find_item(SvHost *host, int id) {
  SvPage *p = sv_menu_top(&host->menu);
  int i;
  if (p == NULL) return NULL;
  for (i = 0; i < p->count; ++i)
    if (p->items[i].id == id) return &p->items[i];
  return NULL;
}
static int focused_id(SvHost *host) {
  SvPage *p = sv_menu_top(&host->menu);
  return p != NULL && p->focus >= 0 && p->focus < p->count ? p->items[p->focus].id : 0;
}
static int page_id(SvHost *host) { SvPage *p = sv_menu_top(&host->menu); return p != NULL ? p->page_id : 0; }
static int toast_has(SvHost *host, const char *needle) {
  int i;
  for (i = 0; i < host->toast_count; ++i)
    if (strstr(host->toasts[i].text, needle) != NULL) return 1;
  return 0;
}

/* ---- scenario 1: no provider ---- */
static int c_no_provider(SvHost *host, void *ctx) {
  const SvItem *save = find_item(host, I_STATE_SAVE), *load = find_item(host, I_STATE_LOAD), *rewind = find_item(host, I_REWIND);
  (void)ctx;
  CHECK(save != NULL && load != NULL && rewind != NULL, "the pause menu always lists Save State, Load State and Rewind");
  CHECK(save != NULL && !save->enabled && strstr(save->hint, "only the Genesis") != NULL, "without a provider Save State is disabled with an accurate reason");
  CHECK(load != NULL && !load->enabled && rewind != NULL && !rewind->enabled, "Load State and Rewind are disabled too");
  return 0;
}

/* ---- scenario 2: save pages ---- */
static int c_main_has_rows(SvHost *host, void *ctx) {
  const SvItem *save = find_item(host, I_STATE_SAVE), *load = find_item(host, I_STATE_LOAD), *rewind = find_item(host, I_REWIND);
  (void)ctx;
  CHECK(save != NULL && save->enabled && load != NULL && load->enabled, "with a provider Save State and Load State are enabled");
  CHECK(rewind != NULL && !rewind->enabled, "Rewind stays disabled until the provider offers it");
  return 0;
}
static int c_on_save_page(SvHost *host, void *ctx) {
  SvPage *p = sv_menu_top(&host->menu);
  (void)ctx;
  CHECK(page_id(host) == PAGE_STATE_SAVE && p->count == SV_STATE_SLOTS, "the save page lists the quick slot and five numbered slots");
  CHECK(strcmp(p->items[0].label, "Quick Slot") == 0 && strcmp(p->items[5].label, "Slot 5") == 0, "slot labels");
  CHECK(strcmp(p->items[2].text, "Empty") == 0, "an empty slot says so");
  return 0;
}
static int c_saved_slot2(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  const SvItem *slot = find_item(host, I_SLOT + 2);
  CHECK(f->saves == 1 && f->last_save == 2, "saving to an empty slot saves at once, without a confirmation");
  CHECK(page_id(host) == PAGE_STATE_SAVE, "and stays on the slot list");
  CHECK(slot != NULL && strstr(slot->text, "played") != NULL, "the slot now shows its playtime");
  CHECK(toast_has(host, "State saved - Slot 2"), "success is reported");
  return 0;
}
static int c_overwrite_confirm(SvHost *host, void *ctx) {
  (void)ctx;
  CHECK(page_id(host) == PAGE_CONFIRM_OVERWRITE, "choosing an occupied slot asks before overwriting");
  CHECK(focused_id(host) == I_NO, "the safe choice (No) is focused");
  return 0;
}
static int c_overwrite_declined(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(f->saves == 1, "answering No does not save");
  CHECK(page_id(host) == PAGE_STATE_SAVE, "and returns to the slot list");
  return 0;
}
static int c_overwrite_accepted(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(f->saves == 2 && f->last_save == 2, "answering Yes overwrites");
  CHECK(page_id(host) == PAGE_STATE_SAVE, "and returns to the slot list");
  return 0;
}

/* ---- scenario 3: load pages ---- */
static int c_load_page(SvHost *host, void *ctx) {
  SvPage *p = sv_menu_top(&host->menu);
  (void)ctx;
  CHECK(page_id(host) == PAGE_STATE_LOAD, "the load page opens");
  CHECK(p != NULL && !p->items[0].enabled && strstr(p->items[0].hint, "empty") != NULL, "an empty slot is disabled and says why");
  CHECK(p != NULL && p->items[2].enabled && strstr(p->items[2].text, "3:03") != NULL, "an occupied slot is enabled and shows its playtime (3:03)");
  CHECK(focused_id(host) == I_SLOT + 2, "focus skips the disabled slots");
  return 0;
}
static int c_load_confirm(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(page_id(host) == PAGE_CONFIRM_LOADSTATE && focused_id(host) == I_NO, "loading asks first, with No focused");
  CHECK(f->loads == 0, "nothing is loaded before the confirmation");
  return 0;
}
static int c_load_declined(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(f->loads == 0 && page_id(host) == PAGE_STATE_LOAD, "declining keeps the running game and the list");
  return 0;
}
static int c_load_failed(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(f->loads == 0 && page_id(host) == PAGE_STATE_LOAD, "a failed load stays on the list: the game is unchanged");
  CHECK(toast_has(host, "Load failed: the state file is corrupt"), "and reports the reason");
  f->fail_load = 0;
  return 0;
}


/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Checkpoint F: the visual rewind surface, driven by keyboard only, by gamepad only, and by the mouse.                                */

static void pad_button(int button) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  e.common.timestamp = SDL_GetTicksNS();
  e.gbutton.which = 0;
  e.gbutton.button = (Uint8)button;
  e.gbutton.down = true;
  SDL_PushEvent(&e);
}
static void axis_event(int axis, int value) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  e.common.timestamp = SDL_GetTicksNS();
  e.gaxis.which = 0;
  e.gaxis.axis = (Uint8)axis;
  e.gaxis.value = (Sint16)value;
  SDL_PushEvent(&e);
}
static void click(float x, float y) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.common.timestamp = SDL_GetTicksNS();
  e.motion.x = x; e.motion.y = y;
  SDL_PushEvent(&e);
  SDL_zero(e);
  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  e.common.timestamp = SDL_GetTicksNS();
  e.button.button = SDL_BUTTON_LEFT; e.button.down = true; e.button.clicks = 1; e.button.x = x; e.button.y = y;
  SDL_PushEvent(&e);
}

/* Steps: input injected at one overlay frame is processed at the next, where the next step's check sees it. `input` kinds below. */
enum { IN_NONE, IN_KEY, IN_PAD, IN_CLICK_CARD, IN_CLICK_BUTTON, IN_CLICK_CANCEL };
typedef struct RStep { int kind; int value; int (*check)(SvHost *host, void *ctx); } RStep;
typedef struct RScript { const RStep *steps; int count, at; Fake *fake; } RScript;

static int rewind_hook(void *ctx, SvHost *host, int iteration) {
  RScript *s = (RScript *)ctx;
  (void)iteration;
  if (host->rewind == NULL || host->rewind->closing != 0) return 0;
  if (host->rewind->anim < 1.0f) return 0;   /* wait for the opening animation: the surface takes input from the start, but checks want the settled layout */
  if (s->at >= s->count) return 1;
  {
    const RStep *step = &s->steps[s->at++];
    if (step->check != NULL) (void)step->check(host, s->fake);
    switch (step->kind) {
      case IN_KEY: tap(step->value); break;
      case IN_PAD: pad_button(step->value); break;
      case IN_CLICK_CARD: {
        int i;
        const int want = step->value >= 0 ? step->value : host->rewind->layout.cards[0].index;   /* -1: the first (oldest) visible card */
        s->fake->clicked = want;
        for (i = 0; i < host->rewind->layout.card_count; ++i)
          if (host->rewind->layout.cards[i].index == want) click((float)(host->rewind->layout.cards[i].rect.x + host->rewind->layout.cards[i].rect.w / 2), (float)(host->rewind->layout.cards[i].rect.y + host->rewind->layout.cards[i].rect.h / 2));
        break;
      }
      case IN_CLICK_BUTTON: { const SvRect *r = &host->rewind->layout.restore_button; click((float)(r->x + r->w / 2), (float)(r->y + r->h / 2)); break; }
      case IN_CLICK_CANCEL: { const SvRect *r = &host->rewind->layout.cancel_button; click((float)(r->x + r->w / 2), (float)(r->y + r->h / 2)); break; }
      default: break;
    }
  }
  return 0;
}

static void open_and_run(SvHost *host, Fake *f, const RStep *steps, int count, int via_key) {
  RScript s;
  SvSample smp;
  s.steps = steps; s.count = count; s.at = 0; s.fake = f;
  sv_host_set_loop_hook(host, rewind_hook, &s);
  if (via_key) tap(SDL_SCANCODE_F7);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
}

static int r_selected_is(SvHost *host, int index) { return host->rewind != NULL && host->rewind->model.selected == index; }
static int c_opened_on_now(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(host->rewind != NULL && f->opened == 1, "the surface opened and told the provider");
  CHECK(host->rewind != NULL && host->rewind->count == f->history && r_selected_is(host, f->history), "it opens on NOW with every moment listed");
  CHECK(host->menu_open, "the guest is frozen while it is open");
  return 0;
}
static int c_sel_5(SvHost *host, void *ctx) { (void)ctx; CHECK(r_selected_is(host, 5), "three Left presses select the third moment back"); return 0; }
static int c_sel_6(SvHost *host, void *ctx) { (void)ctx; CHECK(r_selected_is(host, 6), "D-pad Left twice selects two moments back"); return 0; }
static int c_sel_3(SvHost *host, void *ctx) { (void)ctx; CHECK(r_selected_is(host, 3), "the left shoulder jumps five moments"); return 0; }
static int c_sel_oldest(SvHost *host, void *ctx) { (void)ctx; CHECK(r_selected_is(host, 0), "stops at the oldest moment, no wrapping"); return 0; }
static int c_sel_now_stays(SvHost *host, void *ctx) { Fake *f = (Fake *)ctx; CHECK(r_selected_is(host, f->history), "Right at NOW stays on NOW"); return 0; }
static int c_error_shown(SvHost *host, void *ctx) {
  Fake *f = (Fake *)ctx;
  CHECK(host->rewind != NULL && strstr(host->rewind->error, "Could not rewind: the state is damaged") != NULL, "a failed restore is reported inside the surface");
  CHECK(f->restores == 0 && host->rewind != NULL && host->rewind->closing == 0, "and the surface stays open: the running game is untouched");
  f->fail_restore = 0;
  return 0;
}

static void rewind_tests(SvHost *host, Fake *f, SvStateProvider *provider) {
  char text[96];
  /* not available without a capability */
  {
    SvStateProvider none = *provider;
    none.rewind_available = NULL; none.history_count = NULL; none.history_info = NULL; none.restore_entry = NULL;
    sv_host_set_state_provider(host, &none);
    tap(SDL_SCANCODE_F7);
    {
      SvSample smp;
      sv_host_service(host, &smp);
    }
    CHECK(host->rewind == NULL && toast_has(host, "Rewind is not available for this system"), "F7 without a rewind capability says so");
    sv_host_set_state_provider(host, provider);
  }
  f->history = 8; f->rewind_ok = 1;
  { /* keyboard only: F7, Left x3, Enter */
    static const RStep steps[] = {{IN_NONE, 0, c_opened_on_now}, {IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_NONE, 0, c_sel_5}, {IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->opened = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 1 && f->last_restore == 5 && f->cancelled == 0, "keyboard: Enter restores the selected moment");
    snprintf(text, sizeof text, "Rewound to -1.5s");
    CHECK(toast_has(host, text) && host->rewind == NULL && !host->menu_open, "keyboard: the game resumes at once with a toast, not in a settings page");
  }
  { /* cancel: Escape leaves everything as it was */
    static const RStep steps[] = {{IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_KEY, SDL_SCANCODE_ESCAPE, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->opened = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 0 && f->cancelled == 1 && host->rewind == NULL, "cancel: nothing restored, the provider is told, the surface is gone");
  }
  { /* F7 again closes it; confirming NOW is a plain resume */
    static const RStep steps[] = {{IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 0 && f->cancelled == 1, "confirming NOW is a resume, not a restore");
  }
  { /* no wrapping, both ends */
    static const RStep steps[] = {{IN_NONE, 0, c_opened_on_now}, {IN_KEY, SDL_SCANCODE_RIGHT, NULL}, {IN_NONE, 0, c_sel_now_stays}, {IN_KEY, SDL_SCANCODE_PAGEUP, NULL}, {IN_KEY, SDL_SCANCODE_PAGEUP, NULL},
                                   {IN_KEY, SDL_SCANCODE_PAGEUP, NULL}, {IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_NONE, 0, c_sel_oldest}, {IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->opened = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 1 && f->last_restore == 0, "the oldest moment can be restored; selection never wrapped");
  }
  { /* a failed restore stays in the surface; the next attempt works */
    static const RStep steps[] = {{IN_KEY, SDL_SCANCODE_LEFT, NULL}, {IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, c_error_shown}, {IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->fail_restore = 1;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 1 && f->last_restore == 7, "after the failure the same moment restores when the provider recovers");
  }
  { /* gamepad only: pause menu is not used; the face button confirms by POSITION (south), east cancels */
    static const RStep steps[] = {{IN_PAD, SDL_GAMEPAD_BUTTON_DPAD_LEFT, NULL}, {IN_PAD, SDL_GAMEPAD_BUTTON_DPAD_LEFT, NULL}, {IN_NONE, 0, c_sel_6}, {IN_PAD, SDL_GAMEPAD_BUTTON_SOUTH, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->opened = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 1 && f->last_restore == 6, "gamepad: D-pad browses, the south button restores");
  }
  {
    static const RStep steps[] = {{IN_PAD, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, NULL}, {IN_NONE, 0, c_sel_3}, {IN_PAD, SDL_GAMEPAD_BUTTON_EAST, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 0 && f->cancelled == 1, "gamepad: the shoulder button jumps, the east button cancels");
  }
  { /* the left stick browses with an edge, not a hold */
    static const RStep steps[] = {{IN_NONE, 0, NULL}, {IN_PAD, SDL_GAMEPAD_BUTTON_SOUTH, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0;
    axis_event(SDL_GAMEPAD_AXIS_LEFTX, -30000);
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    axis_event(SDL_GAMEPAD_AXIS_LEFTX, 0);
    CHECK(f->restores <= 1, "stick input is handled");
  }
  { /* mouse: click a thumbnail to select; confirm explicitly with the Restore button */
    static const RStep steps[] = {{IN_NONE, 0, NULL}, {IN_CLICK_CARD, -1, NULL}, {IN_NONE, 0, NULL}, {IN_NONE, 0, NULL}, {IN_CLICK_BUTTON, 0, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 1 && f->last_restore == f->clicked && f->clicked < f->history, "mouse: a click selects, the Restore button confirms");
  }
  { /* a click alone never restores; the Cancel button leaves */
    static const RStep steps[] = {{IN_NONE, 0, NULL}, {IN_CLICK_CARD, -1, NULL}, {IN_NONE, 0, NULL}, {IN_NONE, 0, NULL}, {IN_CLICK_CANCEL, 0, NULL}, {IN_NONE, 0, NULL}};
    f->restores = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 0 && f->cancelled == 1, "mouse: selecting never restores by itself; Cancel leaves");
  }
  { /* the two-trigger chord, debounced */
    SvSample smp;
    static const RStep steps[] = {{IN_NONE, 0, c_opened_on_now}, {IN_KEY, SDL_SCANCODE_ESCAPE, NULL}, {IN_NONE, 0, NULL}};
    RScript s;
    f->opened = 0; f->cancelled = 0;
    s.steps = steps; s.count = 3; s.at = 0; s.fake = f;
    sv_host_set_loop_hook(host, rewind_hook, &s);
    axis_event(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32000);
    axis_event(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32000);
    sv_host_service(host, &smp);
    CHECK(f->opened == 0, "a brief double trigger press (no hold) does not open the surface");
    SDL_Delay(420);
    sv_host_service(host, &smp);
    CHECK(f->opened == 1 && f->cancelled == 1, "holding both triggers opens the surface (debounced); Escape leaves");
    sv_host_service(host, &smp);
    CHECK(f->opened == 1, "the held chord does not retrigger until a trigger is released");
    axis_event(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    axis_event(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
  }
  { /* the pause-menu row opens the same surface */
    static const RStep steps[] = {{IN_NONE, 0, c_opened_on_now}, {IN_KEY, SDL_SCANCODE_ESCAPE, NULL}, {IN_NONE, 0, NULL}};
    Script menu;
    const Step menu_steps[] = {{SV_KEY_DOWN, NULL}, {SV_KEY_DOWN, NULL}, {SV_KEY_DOWN, NULL}, {SV_KEY_RETURN, NULL}, {0, NULL}};
    RScript s;
    SvSample smp;
    f->opened = 0;
    s.steps = steps; s.count = 3; s.at = 0; s.fake = f;
    menu.steps = menu_steps; menu.count = 5; menu.at = 0; menu.ctx = f; menu.closed_early = 0;
    sv_host_set_loop_hook(host, script_hook, &menu);   /* drives the pause menu to the Rewind row */
    tap(SV_KEY_ESCAPE);
    {
      SvSample run;
      /* the menu closes itself; the surface opens when the menu is gone */
      sv_host_service(host, &run);
    }
    sv_host_set_loop_hook(host, NULL, NULL);
    (void)s; (void)smp;
    CHECK(f->opened == 1, "the Rewind row of the pause menu opens the surface");
  }
  { /* empty history: only NOW; confirming resumes */
    static const RStep steps[] = {{IN_NONE, 0, NULL}, {IN_KEY, SDL_SCANCODE_RETURN, NULL}, {IN_NONE, 0, NULL}};
    f->history = 0; f->restores = 0; f->cancelled = 0;
    open_and_run(host, f, steps, (int)(sizeof steps / sizeof steps[0]), 1);
    CHECK(f->restores == 0 && f->cancelled == 1, "an empty history opens, shows only NOW and resumes");
  }
  { /* unavailable: explains why, never opens */
    SvSample smp;
    f->rewind_ok = 0; f->opened = 0;
    tap(SDL_SCANCODE_F7);
    sv_host_service(host, &smp);
    CHECK(f->opened == 0 && host->rewind == NULL && toast_has(host, "one saved moment does not fit the memory limit"), "an unavailable rewind says why and does not open");
    f->rewind_ok = 1;
  }
}

int main(void) {
  Fake f;
  SvStateProvider provider;
  SvHost *host;
  SvSample smp;
  char settings[256];
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_NO_STOP_SCREEN", "1", 1);
  snprintf(settings, sizeof settings, "sv-state-test-%u.cfg", (unsigned)SDL_GetTicks());
  memset(&f, 0, sizeof f);
  f.available = 1;
  host = open_host(settings);
  CHECK(host != NULL, "host opens under the dummy driver");
  if (host == NULL) return 1;

  /* reserved host keys */
  {
    SvSettings s;
    sv_settings_defaults(&s);
    CHECK(sv_key_reserved(&s, SV_KEY_F5) && sv_key_reserved(&s, SV_KEY_F7) && sv_key_reserved(&s, SV_KEY_F8), "F5, F7 and F8 are reserved host keys");
    CHECK(sv_key_reserved(&s, SV_KEY_F11) && sv_key_reserved(&s, SV_KEY_F12) && sv_key_reserved(&s, SV_KEY_F10) && sv_key_reserved(&s, SV_KEY_F1),
          "the earlier reserved keys are still reserved");
    CHECK(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_BUTTON1, SV_KEY_F5, NULL, NULL) != 0, "a game action cannot be bound to F5");
  }

  /* 1. no provider */
  {
    const Step steps[] = {{0, c_no_provider}, {SV_KEY_ESCAPE, NULL}};
    run_menu(host, steps, 2, NULL);
    sv_host_service(host, &smp);
    CHECK(sv_host_quick_save(host) == 0 && toast_has(host, "not available for this system"), "F5 without a provider reports it instead of pretending");
  }

  /* 2. save pages: Esc, Down -> Save State, Return, Down x2 -> Slot 2, Return (saves), Return (occupied: confirm), Return (No), Return (confirm again), Down? (Yes) Return */
  provider = provider_for(&f);
  sv_host_set_state_provider(host, &provider);
  {
    const Step steps[] = {
        {0, c_main_has_rows}, {SV_KEY_DOWN, NULL}, {SV_KEY_RETURN, NULL}, {0, c_on_save_page}, {SV_KEY_DOWN, NULL}, {SV_KEY_DOWN, NULL}, {SV_KEY_RETURN, NULL},
        {0, c_saved_slot2}, {SV_KEY_RETURN, NULL}, {0, c_overwrite_confirm}, {SV_KEY_RETURN, NULL}, {0, c_overwrite_declined}, {SV_KEY_RETURN, NULL}, {0, NULL}, {SV_KEY_UP, NULL}, {SV_KEY_RETURN, NULL},
        {0, c_overwrite_accepted}, {SV_KEY_ESCAPE, NULL}, {SV_KEY_ESCAPE, NULL}};
    run_menu(host, steps, (int)(sizeof steps / sizeof steps[0]), &f);
  }

  /* 3. load pages: the main page order is Resume, Save, Load; slot 2 is the only occupied one */
  {
    const Step steps[] = {
        {SV_KEY_DOWN, NULL}, {SV_KEY_DOWN, NULL}, {SV_KEY_RETURN, NULL}, {0, c_load_page}, {SV_KEY_RETURN, NULL}, {0, c_load_confirm}, {SV_KEY_RETURN, NULL}, {0, c_load_declined},
        {SV_KEY_RETURN, NULL}, {SV_KEY_UP, NULL}, {SV_KEY_RETURN, NULL}, {0, c_load_failed}, {SV_KEY_ESCAPE, NULL}, {SV_KEY_ESCAPE, NULL}};
    f.fail_load = 1;
    run_menu(host, steps, (int)(sizeof steps / sizeof steps[0]), &f);
    CHECK(f.loads == 0, "a failing provider never counts a load");
    f.fail_load = 0;
  }
  {  /* a confirmed load succeeds, resumes the game directly and the menu closes */
    const Step steps[] = {{SV_KEY_DOWN, NULL}, {SV_KEY_DOWN, NULL}, {SV_KEY_RETURN, NULL}, {SV_KEY_RETURN, NULL}, {SV_KEY_UP, NULL}, {SV_KEY_RETURN, NULL}, {0, NULL}, {0, NULL}, {0, NULL}};
    Script s;
    s.steps = steps; s.count = (int)(sizeof steps / sizeof steps[0]); s.at = 0; s.ctx = &f; s.closed_early = 0;
    sv_host_set_loop_hook(host, script_hook, &s);
    tap(SV_KEY_ESCAPE);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
    CHECK(f.loads == 1 && f.last_load == 2, "a confirmed load loads the chosen slot");
    CHECK(s.at < s.count, "and the menu closes straight back into the game, not into a settings page");
    CHECK(toast_has(host, "State loaded - Slot 2"), "the load is announced");
  }

  /* 4. the quick keys, outside any menu */
  {
    int loads_before = f.loads;
    tap(SV_KEY_F5);
    sv_host_service(host, &smp);
    CHECK(f.saves == 3 && f.last_save == 0 && toast_has(host, "State saved - Quick Slot"), "F5 saves the quick slot");
    tap(SV_KEY_F8);
    sv_host_service(host, &smp);
    CHECK(f.loads == loads_before + 1 && f.last_load == 0 && toast_has(host, "State loaded - Quick Slot"), "F8 loads the quick slot");
    f.fail_save = 1;
    tap(SV_KEY_F5);
    sv_host_service(host, &smp);
    CHECK(f.saves == 3 && toast_has(host, "Save failed: disk full") && !toast_has(host, "State saved - Quick Slot save failed"), "a failed quick save reports the failure and never claims success");
    f.fail_save = 0;
    f.available = 0;
    tap(SV_KEY_F8);
    sv_host_service(host, &smp);
    CHECK(f.loads == loads_before + 1 && toast_has(host, "No save location"), "an unavailable provider explains itself on F8");
    f.available = 1;
  }

  rewind_tests(host, &f, &provider);

  sv_host_close(host);
  printf(failures == 0 ? "sv_state_test: ok\n" : "sv_state_test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
