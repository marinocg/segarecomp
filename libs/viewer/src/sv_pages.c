/* The pause overlay: page construction, event application and drawing. Menu mechanics live in sv_menu.c (pure); everything here only
 * edits SvSettings (host presentation / input mapping) and never touches a guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_internal.h"

enum {
  PAGE_MAIN = 1, PAGE_QUICK, PAGE_VIDEO, PAGE_CONTROLS, PAGE_AUDIO, PAGE_SYSTEM, PAGE_CONFIRM_QUIT, PAGE_CONFIRM_RESET, PAGE_CONFIRM_VIDEO, PAGE_CONFIRM_CONTROLS
};
enum {
  I_RESUME = 1, I_GO_QUICK, I_GO_VIDEO, I_GO_CONTROLS, I_GO_AUDIO, I_GO_SYSTEM, I_QUIT,
  I_FULLSCREEN, I_SCALE, I_ASPECT, I_PRESET, I_VSYNC, I_FPS, I_SCANLINES, I_MASK, I_CURVE, I_BLOOM, I_BRIGHT, I_VIDEO_DEFAULTS,
  I_VOLUME, I_MUTE, I_AUDIO_NOTE,
  I_SHOT, I_RESET, I_INFO,
  I_PLAYER, I_PAD, I_PAD_ASSIGNED, I_DEADZONE, I_MENUKEY, I_CTRL_DEFAULTS,
  I_YES, I_NO,
  I_BIND_KEY = 100, /* + action */
  I_BIND_PAD = 200  /* + action */
};

static const SDL_Color c_text = {220, 232, 255, 255}, c_text2 = {169, 183, 221, 255}, c_dim = {120, 133, 170, 255}, c_panel = {8, 20, 47, 245},
                       c_edge = {35, 62, 120, 255}, c_hi = {77, 103, 167, 255}, c_shadow = {7, 16, 38, 255}, c_btn = {21, 87, 201, 255},
                       c_btn_hi = {64, 191, 245, 255}, c_btn_edge = {38, 141, 238, 255}, c_btn_shadow = {8, 41, 110, 255}, c_btn_text = {244, 247, 255, 255},
                       c_focus = {140, 224, 255, 255}, c_err = {255, 129, 119, 255};

/* ---- naming ---- */

static const char *pad_button_name(int b) {
  static const char *const names[] = {"South", "East", "West", "North", "Back", "Guide", "Start", "L-Stick", "R-Stick", "L-Shoulder", "R-Shoulder",
                                      "DPad Up", "DPad Down", "DPad Left", "DPad Right"};
  if (b < 0) return "Unbound";
  return b < (int)(sizeof names / sizeof names[0]) ? names[b] : "Button";
}

static const char *key_name(int sc) {
  const char *n;
  if (sc <= 0) return "Unbound";
  n = SDL_GetScancodeName((SDL_Scancode)sc);
  return (n != NULL && *n != '\0') ? n : "Key";
}

static void action_label(const SvHost *h, SvAction a, char *out, size_t cap) {
  snprintf(out, cap, "%s", sv_action_label(h->cfg.system, a));
}

/* ---- presets ---- */

static void reclassify_preset(SvVideoSettings *v) {
  SvCrtPreset p;
  for (p = SV_CRT_PIXEL_PERFECT; p < SV_CRT_CUSTOM; p = (SvCrtPreset)(p + 1)) {
    SvVideoSettings probe = *v;
    sv_apply_crt_preset(&probe, p);
    if (probe.scanlines == v->scanlines && probe.mask == v->mask && probe.curvature == v->curvature && probe.bloom == v->bloom && probe.brightness == v->brightness) {
      v->preset = p;
      return;
    }
  }
  v->preset = SV_CRT_CUSTOM;
}

/* ---- page construction ---- */

static SvItem *add_toggle(SvPage *p, int id, const char *label, int value, const char *hint) {
  SvItem *i = sv_page_add(p, SV_ITEM_TOGGLE, id, label);
  if (i != NULL) { i->value = value != 0; sv_item_set_text(i, value ? "ON" : "OFF"); sv_item_set_hint(i, hint); }
  return i;
}

static SvItem *add_slider(SvPage *p, int id, const char *label, int value, int lo, int hi, int step, const char *unit, const char *hint) {
  SvItem *i = sv_page_add(p, SV_ITEM_SLIDER, id, label);
  if (i != NULL) {
    char buf[24];
    i->value = value; i->min = lo; i->max = hi; i->step = step;
    snprintf(buf, sizeof buf, "%d%s", value, unit);
    sv_item_set_text(i, buf);
    sv_item_set_hint(i, hint);
  }
  return i;
}

static void add_choice(SvPage *p, int id, const char *label, int value, int count, const char *const *names, const char *hint) {
  SvItem *i = sv_page_add(p, SV_ITEM_CHOICE, id, label);
  if (i == NULL) return;
  i->min = 0; i->max = count - 1; i->value = value;
  sv_item_set_text(i, names[value]);
  sv_item_set_hint(i, hint);
}

static void build_video_core(SvHost *h, SvPage *p, int full) {
  static const char *const scale[] = {"Fit window", "Integer"}, *const aspect[] = {"Original", "4:3 corrected"};
  static const char *const preset[] = {"Pixel Perfect", "CRT Soft", "CRT Classic", "CRT Arcade", "Custom"};
  const SvVideoSettings *v = &h->settings.video;
  add_toggle(p, I_FULLSCREEN, "Fullscreen", v->fullscreen, "Borderless desktop fullscreen; no display mode change. F11 also toggles.");
  add_choice(p, I_SCALE, "Scaling", v->scale, 2, scale, "Fit uses any size; Integer keeps whole-number pixel multiples when they fit.");
  add_choice(p, I_ASPECT, "Aspect", v->aspect, 2, aspect, "Original keeps square pixels; 4:3 stretches to the aspect of a TV picture.");
  add_choice(p, I_PRESET, "Display preset", v->preset, v->preset == SV_CRT_CUSTOM ? 5 : 4, preset, "Pixel Perfect is the untouched picture. CRT presets add real scanline/mask/glow/curve passes.");
  if (full) {
    add_slider(p, I_SCANLINES, "Scanlines", v->scanlines, 0, 100, 5, "%", "Darkens the gap between source lines (needs a window at least 1.5x the picture).");
    add_slider(p, I_MASK, "Phosphor mask", v->mask, 0, 100, 5, "%", "RGB stripe mask over the picture.");
    add_slider(p, I_CURVE, "Curvature", v->curvature, 0, 100, 5, "%", "Barrel distortion of the picture with corner falloff.");
    add_slider(p, I_BLOOM, "Bloom", v->bloom, 0, 100, 5, "%", "Soft glow around bright pixels.");
    add_slider(p, I_BRIGHT, "Brightness", v->brightness, 50, 150, 5, "%", "Picture brightness; values above 100 brighten to compensate for scanlines.");
  }
  add_toggle(p, I_VSYNC, "VSync", v->vsync, "Syncs presentation to the display. Game speed is paced by the console clock either way.");
  add_toggle(p, I_FPS, "FPS counter", v->show_fps, "Shows the presentation rate on the game screen.");
  if (full) sv_page_add(p, SV_ITEM_BUTTON, I_VIDEO_DEFAULTS, "Restore video defaults");
}

static void build_controls(SvHost *h, SvPage *p) {
  static const char *const players[] = {"Player 1", "Player 2"};
  const SvBindings *b = &h->settings.bind[h->cfg.system];
  const int player = h->bind_player;
  char label[112], value[48];
  int a, slot;
  SvItem *item;
  add_choice(p, I_PLAYER, "Player", player, 2, players, "Each console has two controller ports.");
  if (player == 0) {
    int count = h->pad_count + 1, current = 0, i;
    const char *names[SV_MAX_PADS + 1];
    names[0] = "Auto (first connected)";
    for (i = 0; i < h->pad_count; ++i) { names[i + 1] = h->pads[i].name; if (strcmp(h->pads[i].name, h->settings.pad_name) == 0) current = i + 1; }
    if (h->pad_count == 0) { item = sv_page_add(p, SV_ITEM_LABEL, I_PAD, "Gamepad"); sv_item_set_text(item, "none connected"); }
    else add_choice(p, I_PAD, "Gamepad", current, count, names, "Choose which connected gamepad plays Player 1 (the next one plays Player 2).");
  }
  slot = sv_host_pad_assigned(h, player);
  item = sv_page_add(p, SV_ITEM_LABEL, I_PAD_ASSIGNED, "Assigned device");
  sv_item_set_text(item, slot >= 0 ? h->pads[slot].name : (h->pad_count == 0 ? "keyboard only" : "keyboard (no spare pad)"));
  for (a = 0; a < SV_ACT_COUNT; ++a) {
    if (!sv_action_available(h->cfg.system, player, (SvAction)a)) continue;
    action_label(h, (SvAction)a, value, sizeof value);
    snprintf(label, sizeof label, "%s key", value);
    item = sv_page_add(p, SV_ITEM_BINDING, I_BIND_KEY + a, label);
    if (item != NULL) { sv_item_set_text(item, key_name(b->key[player][a])); sv_item_set_hint(item, "Enter: press a new key. Backspace while listening clears it."); }
  }
  for (a = 0; a < SV_ACT_COUNT; ++a) {
    if (!sv_action_available(h->cfg.system, player, (SvAction)a)) continue;
    action_label(h, (SvAction)a, value, sizeof value);
    snprintf(label, sizeof label, "%s pad", value);
    item = sv_page_add(p, SV_ITEM_BINDING, I_BIND_PAD + a, label);
    if (item != NULL) {
      sv_item_set_text(item, pad_button_name(b->pad[player][a]));
      sv_item_set_hint(item, slot >= 0 ? "Accept, then press a gamepad button." : "Connect a gamepad to rebind its buttons.");
      if (slot < 0) item->enabled = 0;
    }
  }
  add_slider(p, I_DEADZONE, "Stick dead zone", h->settings.deadzone, 5, 60, 5, "%", "Analog stick travel ignored before a direction registers.");
  item = sv_page_add(p, SV_ITEM_BINDING, I_MENUKEY, "Menu key");
  if (item != NULL) { sv_item_set_text(item, key_name(h->settings.menu_key)); sv_item_set_hint(item, "Opens this menu. Esc always works as a fallback."); }
  sv_page_add(p, SV_ITEM_BUTTON, I_CTRL_DEFAULTS, "Restore control defaults");
}

static void build_audio(SvHost *h, SvPage *p) {
  SvItem *item;
  add_slider(p, I_VOLUME, "Volume", h->settings.audio.volume, 0, 100, 5, "%", "Playback gain only; the console's generated sound is not changed.");
  add_toggle(p, I_MUTE, "Mute", h->settings.audio.muted, "Silences playback without touching the emulated sound.");
  item = sv_page_add(p, SV_ITEM_LABEL, I_AUDIO_NOTE, "Output device");
  if (h->audio != NULL) {
    const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(h->audio);
    const char *name = dev != 0 ? SDL_GetAudioDeviceName(dev) : NULL;
    sv_item_set_text(item, name != NULL ? name : "system default");
  } else {
    sv_item_set_text(item, "none (silent)");
  }
  item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, "Device selection");
  sv_item_set_text(item, "follows the OS default");
}

static void build_system(SvHost *h, SvPage *p) {
  SvDiagnostics d;
  SvItem *item;
  char buf[64];
  int out_w = 0, out_h = 0;
  SvRect dest;
  memset(&d, 0, sizeof d);
  if (h->cfg.diagnostics != NULL) h->cfg.diagnostics(h->cfg.diagnostics_ctx, &d);
  SDL_GetCurrentRenderOutputSize(h->renderer, &out_w, &out_h);
  dest = sv_layout_dest(out_w, out_h, h->last_w, h->last_h, h->settings.video.aspect, h->settings.video.scale);
#define ROW(label, ...) do { item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, label); if (item != NULL) { snprintf(buf, sizeof buf, __VA_ARGS__); sv_item_set_text(item, buf); } } while (0)
  ROW("System", "%s", sv_system_name(h->cfg.system));
  if (h->cfg.rom_sha256 != NULL && strlen(h->cfg.rom_sha256) == 64u) {
    char half[40];
    snprintf(half, sizeof half, "%.32s", h->cfg.rom_sha256);
    item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, "ROM SHA-256"); sv_item_set_text(item, half);
    snprintf(half, sizeof half, "%.32s", h->cfg.rom_sha256 + 32);
    item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, ""); sv_item_set_text(item, half);
  }
  ROW("Presentation", "%.1f FPS, %.1f ms", h->fps, h->frame_ms);
  ROW("Renderer", "%s", sv_host_renderer_name(h));
  ROW("Picture", "%dx%d", h->last_w, h->last_h);
  ROW("Output", "%dx%d at %.2fx", dest.w, dest.h, h->last_h > 0 ? (double)dest.h / (double)h->last_h : 0.0);
  if (d.audio_active) {
    ROW("Audio queue", "%u ms", (unsigned)d.audio_queued_ms);
    ROW("Audio dropped", "%llu chunks", (unsigned long long)d.audio_dropped);
  } else {
    ROW("Audio", "no device (silent)");
  }
  ROW("Gamepads", "%d connected", h->pad_count);
  item = sv_page_add(p, SV_ITEM_BUTTON, I_SHOT, "Take screenshot");
  if (item != NULL) sv_item_set_hint(item, h->last_screenshot[0] != '\0' ? h->last_screenshot : "Saves the game picture (without menus or effects) as an image.");
  item = sv_page_add(p, SV_ITEM_BUTTON, I_RESET, "Reset game");
  if (item != NULL && !h->cfg.can_reset) sv_item_disable(item, "Not available: this console's viewer has no proven reset seam.");
  else if (item != NULL) sv_item_set_hint(item, "Restarts the machine from power-on (the recorded input restarts too).");
#undef ROW
}

static const char *exit_label(const SvHost *h) { (void)h; return SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Return to Launcher" : "Quit"; }

void sv_pages_rebuild(SvHost *h) {
  SvPage *p = sv_menu_top(&h->menu);
  int focus_id = 0, scroll;
  SvItem *item;
  if (p == NULL) return;
  if (p->focus >= 0 && p->focus < p->count) focus_id = p->items[p->focus].id;
  scroll = p->scroll;
  p->count = 0;
  switch (p->page_id) {
    case PAGE_MAIN:
      sv_page_add(p, SV_ITEM_BUTTON, I_RESUME, "Resume");
      sv_page_add(p, SV_ITEM_BUTTON, I_GO_QUICK, "Quick Settings");
      sv_page_add(p, SV_ITEM_BUTTON, I_GO_VIDEO, "Video & Display");
      sv_page_add(p, SV_ITEM_BUTTON, I_GO_CONTROLS, "Controls");
      sv_page_add(p, SV_ITEM_BUTTON, I_GO_AUDIO, "Audio");
      sv_page_add(p, SV_ITEM_BUTTON, I_GO_SYSTEM, "System & Diagnostics");
      item = sv_page_add(p, SV_ITEM_BUTTON, I_QUIT, exit_label(h));
      if (item != NULL) sv_item_set_hint(item, "The game is not saved: this viewer has no save states.");
      break;
    case PAGE_QUICK: build_video_core(h, p, 0); break;
    case PAGE_VIDEO: build_video_core(h, p, 1); break;
    case PAGE_CONTROLS: build_controls(h, p); break;
    case PAGE_AUDIO: build_audio(h, p); break;
    case PAGE_SYSTEM: build_system(h, p); break;
    case PAGE_CONFIRM_QUIT:
      item = sv_page_add(p, SV_ITEM_BUTTON, I_NO, "Cancel");
      (void)item;
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, exit_label(h));
      break;
    case PAGE_CONFIRM_RESET:
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "Cancel");
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Reset game");
      break;
    case PAGE_CONFIRM_VIDEO:
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "Cancel");
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Restore video defaults");
      break;
    case PAGE_CONFIRM_CONTROLS:
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "Cancel");
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Restore control defaults");
      break;
    default: break;
  }
  p->scroll = scroll;
  if (focus_id != 0) sv_page_restore_focus(p, focus_id); else sv_page_focus_first(p);
  if (p->focus >= p->count) sv_page_focus_first(p);
}

static void open_page(SvHost *h, int page_id, const char *title) {
  SvPage *p = sv_menu_push(&h->menu, page_id, title);
  if (p == NULL) return;
  sv_pages_rebuild(h);
  sv_page_focus_first(sv_menu_top(&h->menu));
}

void sv_pages_open_main(SvHost *h) {
  sv_menu_reset(&h->menu);
  memset(&h->listen, 0, sizeof h->listen);
  h->bind_player = 0;
  open_page(h, PAGE_MAIN, "PAUSED");
}

void sv_pages_close(SvHost *h) { h->menu_open = 0; }

void sv_pages_back(SvHost *h) {
  if (h->listen.kind != SV_LISTEN_NONE) { sv_pages_listen_cancel(h, "Binding cancelled"); return; }
  if (h->menu.depth > 1) { sv_menu_pop(&h->menu); sv_pages_rebuild(h); } else sv_pages_close(h);
}

int sv_pages_wants_preview(const SvHost *h) {
  const SvPage *p = h->menu.depth > 0 ? &h->menu.stack[h->menu.depth - 1] : NULL;
  return p != NULL && (p->page_id == PAGE_VIDEO || p->page_id == PAGE_QUICK) && h->listen.kind == SV_LISTEN_NONE;
}

/* ---- binding listener ---- */

static void begin_listen(SvHost *h, SvListenKind kind, SvAction action) {
  h->listen.kind = kind;
  h->listen.player = h->bind_player;
  h->listen.action = action;
  h->listen.started = SDL_GetTicks();
  h->listen.after_ts = h->event_ts; /* only an event strictly newer than the activating one can be captured */
}

void sv_pages_listen_cancel(SvHost *h, const char *why) {
  h->listen.kind = SV_LISTEN_NONE;
  if (why != NULL) sv_host_toast(h, why);
}

static void report_move(SvHost *h, const char *what, const SvBindingOwner *from) {
  char msg[96], from_label[24];
  snprintf(from_label, sizeof from_label, "%s", sv_action_label(h->cfg.system, from->action));
  snprintf(msg, sizeof msg, "Conflict: %s was %s P%d, which is now unbound", what, from_label, from->player + 1);
  sv_host_toast(h, msg);
}

void sv_pages_listen_key(SvHost *h, int sc) {
  char msg[96];
  int conflict = 0;
  SvBindingOwner moved = {0, SV_ACT_UP};
  if (h->listen.kind == SV_LISTEN_PAD) {
    if (sc == SDL_SCANCODE_ESCAPE) sv_pages_listen_cancel(h, "Binding cancelled");
    else if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
      sv_assign_pad(&h->settings, h->cfg.system, h->listen.player, h->listen.action, -1, NULL, NULL);
      h->listen.kind = SV_LISTEN_NONE; sv_host_mark_dirty(h); sv_host_toast(h, "Gamepad binding cleared");
    }
    return;
  }
  if (h->event_ts <= h->listen.after_ts) return;
  if (sc == SDL_SCANCODE_ESCAPE) { sv_pages_listen_cancel(h, "Binding cancelled"); return; }
  if (h->listen.kind == SV_LISTEN_MENU_KEY) {
    int sys, p, a;
    if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) { h->settings.menu_key = SV_KEY_ESCAPE; sv_host_toast(h, "Menu key restored to Esc"); }
    else {
      if (sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12) { sv_host_toast(h, "That key is a host hotkey"); h->listen.kind = SV_LISTEN_NONE; return; }
      h->settings.menu_key = sc;
      for (sys = 0; sys < SV_SYSTEM_COUNT; ++sys)
        for (p = 0; p < SV_PLAYERS; ++p)
          for (a = 0; a < SV_ACT_COUNT; ++a)
            if (h->settings.bind[sys].key[p][a] == sc) { h->settings.bind[sys].key[p][a] = 0; conflict = 1; }
      snprintf(msg, sizeof msg, conflict ? "Menu key set to %s; a game binding using it was cleared" : "Menu key set to %s (Esc still works)", key_name(sc));
      sv_host_toast(h, msg);
    }
    h->listen.kind = SV_LISTEN_NONE;
    sv_host_mark_dirty(h);
    return;
  }
  if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
    sv_assign_key(&h->settings, h->cfg.system, h->listen.player, h->listen.action, 0, NULL, NULL);
    sv_host_toast(h, "Key binding cleared");
  } else if (sv_assign_key(&h->settings, h->cfg.system, h->listen.player, h->listen.action, sc, &moved, &conflict) != 0) {
    snprintf(msg, sizeof msg, "%s is reserved by the viewer", key_name(sc));
    sv_host_toast(h, msg);
  } else if (conflict) report_move(h, key_name(sc), &moved);
  else { snprintf(msg, sizeof msg, "%s bound to %s", sv_action_label(h->cfg.system, h->listen.action), key_name(sc)); sv_host_toast(h, msg); }
  h->listen.kind = SV_LISTEN_NONE;
  sv_host_mark_dirty(h);
}

void sv_pages_listen_pad(SvHost *h, int button) {
  char msg[96];
  int conflict = 0;
  SvBindingOwner moved = {0, SV_ACT_UP};
  if (h->listen.kind != SV_LISTEN_PAD || h->event_ts <= h->listen.after_ts) return;
  if (button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_GUIDE) { sv_host_toast(h, "Back and Guide open the menu"); h->listen.kind = SV_LISTEN_NONE; return; }
  sv_assign_pad(&h->settings, h->cfg.system, h->listen.player, h->listen.action, button, &moved, &conflict);
  if (conflict) report_move(h, pad_button_name(button), &moved);
  else { snprintf(msg, sizeof msg, "%s bound to %s", sv_action_label(h->cfg.system, h->listen.action), pad_button_name(button)); sv_host_toast(h, msg); }
  h->listen.kind = SV_LISTEN_NONE;
  sv_host_mark_dirty(h);
}

/* ---- applying menu events ---- */

void sv_pages_apply(SvHost *h, SvMenuEvent ev) {
  SvVideoSettings *v = &h->settings.video;
  const SvPage *page = sv_menu_top(&h->menu);
  if (ev.kind == SV_EVENT_NONE || page == NULL) return;
  if (ev.kind == SV_EVENT_BACK) { sv_pages_back(h); return; }
  if (ev.kind == SV_EVENT_ACTIVATE) {
    switch (ev.item_id) {
      case I_RESUME: sv_pages_close(h); return;
      case I_GO_QUICK: open_page(h, PAGE_QUICK, "QUICK SETTINGS"); return;
      case I_GO_VIDEO: open_page(h, PAGE_VIDEO, "VIDEO & DISPLAY"); return;
      case I_GO_CONTROLS: open_page(h, PAGE_CONTROLS, "CONTROLS"); return;
      case I_GO_AUDIO: open_page(h, PAGE_AUDIO, "AUDIO"); return;
      case I_GO_SYSTEM: open_page(h, PAGE_SYSTEM, "SYSTEM & DIAGNOSTICS"); return;
      case I_QUIT: open_page(h, PAGE_CONFIRM_QUIT, SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "RETURN TO LAUNCHER?" : "QUIT?"); return;
      case I_SHOT: sv_host_take_screenshot(h); return;
      case I_RESET: if (h->cfg.can_reset) open_page(h, PAGE_CONFIRM_RESET, "RESET GAME?"); return;
      case I_VIDEO_DEFAULTS: open_page(h, PAGE_CONFIRM_VIDEO, "RESTORE VIDEO DEFAULTS?"); return;
      case I_CTRL_DEFAULTS: open_page(h, PAGE_CONFIRM_CONTROLS, "RESTORE CONTROL DEFAULTS?"); return;
      case I_MENUKEY: begin_listen(h, SV_LISTEN_MENU_KEY, SV_ACT_UP); return;
      case I_NO: sv_pages_back(h); return;
      case I_YES:
        switch (page->page_id) {
          case PAGE_CONFIRM_QUIT: h->quit = 1; sv_pages_close(h); break;
          case PAGE_CONFIRM_RESET: h->reset = 1; sv_pages_close(h); break;
          case PAGE_CONFIRM_VIDEO: {
            SvVideoSettings keep = h->settings.video;
            sv_video_defaults(&h->settings.video);
            h->settings.video.window_w = keep.window_w;
            h->settings.video.window_h = keep.window_h;
            sv_host_apply_video(h); sv_host_mark_dirty(h); sv_host_toast(h, "Video settings restored");
            sv_menu_pop(&h->menu); sv_pages_rebuild(h);
            break;
          }
          case PAGE_CONFIRM_CONTROLS:
            sv_bindings_defaults(&h->settings, h->cfg.system);
            h->settings.deadzone = 25; h->settings.menu_key = SV_KEY_ESCAPE; h->settings.pad_name[0] = '\0';
            sv_host_mark_dirty(h); sv_host_toast(h, "Controls restored to defaults");
            sv_menu_pop(&h->menu); sv_pages_rebuild(h);
            break;
          default: break;
        }
        return;
      default: break;
    }
    if (ev.item_id >= I_BIND_KEY && ev.item_id < I_BIND_KEY + SV_ACT_COUNT) begin_listen(h, SV_LISTEN_KEY, (SvAction)(ev.item_id - I_BIND_KEY));
    else if (ev.item_id >= I_BIND_PAD && ev.item_id < I_BIND_PAD + SV_ACT_COUNT) begin_listen(h, SV_LISTEN_PAD, (SvAction)(ev.item_id - I_BIND_PAD));
    return;
  }
  /* SV_EVENT_CHANGED */
  switch (ev.item_id) {
    case I_FULLSCREEN: v->fullscreen = ev.value; sv_host_apply_video(h); break;
    case I_SCALE: v->scale = (SvScaleMode)ev.value; break;
    case I_ASPECT: v->aspect = (SvAspect)ev.value; break;
    case I_PRESET: sv_apply_crt_preset(v, (SvCrtPreset)ev.value); break;
    case I_VSYNC: v->vsync = ev.value; sv_host_apply_video(h); break;
    case I_FPS: v->show_fps = ev.value; break;
    case I_SCANLINES: v->scanlines = ev.value; reclassify_preset(v); break;
    case I_MASK: v->mask = ev.value; reclassify_preset(v); break;
    case I_CURVE: v->curvature = ev.value; reclassify_preset(v); break;
    case I_BLOOM: v->bloom = ev.value; reclassify_preset(v); break;
    case I_BRIGHT: v->brightness = ev.value; reclassify_preset(v); break;
    case I_VOLUME: h->settings.audio.volume = ev.value; sv_host_apply_audio(h); break;
    case I_MUTE: h->settings.audio.muted = ev.value; sv_host_apply_audio(h); break;
    case I_PLAYER: h->bind_player = ev.value; break;
    case I_PAD: h->settings.pad_name[0] = '\0'; if (ev.value >= 1 && ev.value <= h->pad_count) snprintf(h->settings.pad_name, sizeof h->settings.pad_name, "%s", h->pads[ev.value - 1].name); break;
    case I_DEADZONE: h->settings.deadzone = ev.value; break;
    default: return;
  }
  sv_settings_sanitize(&h->settings);
  sv_host_mark_dirty(h);
}

/* ---- drawing ---- */

static void truncate_to(char *dst, size_t cap, const char *src, int max_chars) {
  size_t n = strlen(src);
  if (max_chars < 1) max_chars = 1;
  if ((int)n > max_chars) {
    n = (size_t)max_chars;
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    if (n >= 3u) { dst[n - 1u] = '.'; dst[n - 2u] = '.'; dst[n - 3u] = '.'; }
    dst[n] = '\0';
  } else {
    snprintf(dst, cap, "%s", src);
  }
}

void sv_pages_draw(SvHost *h, int out_w, int out_h) {
  SvPage *page = sv_menu_top(&h->menu);
  const int preview = sv_pages_wants_preview(h);
  int ui = sv_host_ui_scale(out_w, out_h);
  int cols, cw, row_h, pad, title_h, foot_h, panel_w, rows, visible, i, x, y, panel_h, value_x;
  char line[160];
  if (preview) { /* keep the picture visible next to the panel: at most ~46% of the window */
    const int fit = out_w * 46 / 100 / (40 * 8);
    if (fit < ui) ui = fit < 1 ? 1 : fit;
  }
  cw = 8 * ui; row_h = 13 * ui; pad = 6 * ui; title_h = 16 * ui; foot_h = 38 * ui;
  cols = preview ? 40 : ((page != NULL && (page->page_id == PAGE_MAIN || page->page_id >= PAGE_CONFIRM_QUIT)) ? 40 : 52);
  panel_w = cols * cw;
  if (page == NULL) return;
  if (panel_w > out_w - 2 * pad) panel_w = out_w - 2 * pad;
  rows = page->count;
  visible = (out_h - title_h - foot_h - 4 * pad) / row_h;
  if (visible < 3) visible = 3;
  if (visible > rows) visible = rows;
  sv_menu_ensure_visible(page, visible);
  panel_h = title_h + visible * row_h + foot_h + 2 * pad;
  x = preview ? pad : (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  if (y < pad) y = pad;
  sv_host_bevel(h, x, y, panel_w, panel_h, ui, c_panel, c_edge, c_hi, c_shadow);
  sv_host_draw_text(h, (float)(x + pad + 2 * ui), (float)(y + 5 * ui), ui, c_text, page->title);
  { const SDL_Color rule = {35, 62, 120, 255}; sv_host_fill(h, x + pad, y + title_h, panel_w - 2 * pad, ui, rule); }
  value_x = x + panel_w - pad - (cols >= 52 ? 24 : 22) * cw;
  h->ui_rows = 0;
  for (i = 0; i < visible; ++i) {
    const int idx = page->scroll + i;
    const SvItem *it;
    const int ry = y + title_h + pad + i * row_h, rx = x + pad, rw = panel_w - 2 * pad - (rows > visible ? 3 * ui : 0);
    int focused;
    SDL_Color tc;
    if (idx >= page->count) break;
    it = &page->items[idx];
    focused = idx == page->focus;
    h->ui_item_rect[h->ui_rows][0] = rx; h->ui_item_rect[h->ui_rows][1] = ry; h->ui_item_rect[h->ui_rows][2] = rw; h->ui_item_rect[h->ui_rows][3] = row_h;
    h->ui_rows++;
    tc = it->enabled ? c_text : c_dim;
    if (it->kind == SV_ITEM_LABEL) tc = c_text2;
    if (focused) {
      sv_host_bevel(h, rx - ui, ry - ui, rw + 2 * ui, row_h, ui, c_btn, c_btn_edge, c_btn_hi, c_btn_shadow);
      tc = c_btn_text;
      { SDL_FRect ring; ring.x = (float)(rx - 2 * ui); ring.y = (float)(ry - 2 * ui); ring.w = (float)(rw + 4 * ui); ring.h = (float)(row_h + 2 * ui);
        SDL_SetRenderDrawColor(h->renderer, c_focus.r, c_focus.g, c_focus.b, 255); SDL_RenderRect(h->renderer, &ring); }
    }
    truncate_to(line, sizeof line, it->label, (it->kind == SV_ITEM_LABEL ? 17 : (value_x - rx) / cw - 1));
    sv_host_draw_text(h, (float)(rx + 3 * ui), (float)(ry + 2 * ui), ui, tc, line);
    switch (it->kind) {
      case SV_ITEM_SLIDER: {
        const int track_w = 14 * cw, tx = value_x + 5 * cw;
        const int span = it->max - it->min > 0 ? it->max - it->min : 1;
        const int fill = (it->value - it->min) * track_w / span;
        const SDL_Color track = {7, 16, 38, 255}, bar = {64, 191, 245, 255}, knob = {244, 247, 255, 255};
        sv_host_fill(h, tx, ry + 3 * ui, track_w, 5 * ui, track);
        sv_host_fill(h, tx, ry + 3 * ui, fill, 5 * ui, bar);
        sv_host_fill(h, tx + fill - ui, ry + 1 * ui, 2 * ui, 9 * ui, knob);
        truncate_to(line, sizeof line, it->text, 5);
        sv_host_draw_text(h, (float)(value_x), (float)(ry + 2 * ui), ui, focused ? c_btn_text : c_text2, line);
        break;
      }
      case SV_ITEM_TOGGLE: {
        const SDL_Color on = {104, 224, 158, 255}, off = {120, 133, 170, 255};
        const int bx = value_x + 14 * cw;
        sv_host_fill(h, bx, ry + 1 * ui, 8 * ui, 10 * ui, it->value ? on : off);
        sv_host_draw_text(h, (float)(bx + 12 * ui), (float)(ry + 2 * ui), ui, focused ? c_btn_text : (it->value ? on : off), it->text);
        break;
      }
      case SV_ITEM_CHOICE:
        truncate_to(line, sizeof line, it->text, 21);
        { char shown[200]; snprintf(shown, sizeof shown, "< %s >", line);
          sv_host_draw_text(h, (float)(value_x), (float)(ry + 2 * ui), ui, focused ? c_btn_text : c_text2, shown); }
        break;
      case SV_ITEM_BINDING:
        truncate_to(line, sizeof line, it->text, 22);
        sv_host_draw_text(h, (float)(value_x), (float)(ry + 2 * ui), ui, focused ? c_btn_text : (it->enabled ? c_text2 : c_dim), line);
        break;
      case SV_ITEM_LABEL: {
        /* read-only rows get a wider value column (a 64-digit hash fits as two 32-digit rows) */
        const int lx = x + pad + 18 * cw;
        truncate_to(line, sizeof line, it->text, (panel_w - 2 * pad - 18 * cw) / cw - 1);
        sv_host_draw_text(h, (float)(lx), (float)(ry + 2 * ui), ui, c_text2, line);
        break;
      }
      case SV_ITEM_BUTTON: break;
    }
  }
  if (rows > visible && visible > 0) { /* scroll bar */
    const SDL_Color track = {7, 16, 38, 255}, knob = {77, 103, 167, 255};
    const int tx = x + panel_w - pad - 2 * ui, ty = y + title_h + pad, th = visible * row_h;
    const int kh = th * visible / rows > 6 * ui ? th * visible / rows : 6 * ui;
    sv_host_fill(h, tx, ty, 2 * ui, th, track);
    sv_host_fill(h, tx, ty + (th - kh) * page->scroll / (rows - visible), 2 * ui, kh, knob);
  }
  {  /* footer: the focused item's explanation, then input cues for the device last used */
    const int fy = y + panel_h - foot_h - pad + 2 * ui, max_chars = (panel_w - 2 * pad) / cw;
    const SvItem *cur = page->focus >= 0 && page->focus < page->count ? &page->items[page->focus] : NULL;
    if (h->listen.kind != SV_LISTEN_NONE) {
      const unsigned left = 8u - (unsigned)((SDL_GetTicks() - h->listen.started) / 1000u);
      snprintf(line, sizeof line, h->listen.kind == SV_LISTEN_PAD ? "Press a gamepad button (%us)  Esc cancel  Bksp clear" : "Press a key (%us)  Esc cancel  Bksp clear", left > 8u ? 0u : left);
      sv_host_draw_text(h, (float)(x + pad), (float)(fy + 2 * ui), ui, c_focus, line);
    } else {
      if (cur != NULL && cur->hint[0] != '\0') {
        /* the explanation of the focused item wraps onto a second line instead of being cut off */
        const char *hint = cur->hint;
        size_t len = strlen(hint), cut = len;
        const SDL_Color hc = cur->enabled ? c_text2 : c_err;
        if ((int)len > max_chars) {
          cut = (size_t)max_chars;
          while (cut > 8u && hint[cut] != ' ') --cut;
        }
        snprintf(line, sizeof line, "%.*s", (int)cut, hint);
        sv_host_draw_text(h, (float)(x + pad), (float)fy, ui, hc, line);
        if (cut < len) {
          const char *rest = hint + cut + (hint[cut] == ' ' ? 1 : 0);
          truncate_to(line, sizeof line, rest, max_chars);
          sv_host_draw_text(h, (float)(x + pad), (float)(fy + 11 * ui), ui, hc, line);
        }
      }
      {
        const int wide = max_chars >= 50;
        const char *cues = h->last_input_pad ? (wide ? "D-pad move & change  A select  B back  Back resume" : "D-pad  A select  B back")
                                              : (wide ? "Up/Down move  Left/Right change  Enter OK  Esc back" : "Up/Down  Enter OK  Esc back");
        truncate_to(line, sizeof line, cues, max_chars);
      }
      sv_host_draw_text(h, (float)(x + pad), (float)(fy + 24 * ui), ui, c_dim, line);
    }
  }
  /* confirm pages explain the consequence above the buttons */
  if (page->page_id == PAGE_CONFIRM_QUIT || page->page_id == PAGE_CONFIRM_RESET) {
    const char *msg = page->page_id == PAGE_CONFIRM_QUIT ? "The game is not saved." : "Restarts from power-on.";
    sv_host_draw_text(h, (float)(x + pad + 2 * ui), (float)(y + title_h + pad + 3 * row_h), ui, c_text2, msg);
  }
}

void sv_draw_stop_screen(SvHost *h, int out_w, int out_h, const char *title, const char *detail) {
  const int ui = sv_host_ui_scale(out_w, out_h), cw = 8 * ui, pad = 8 * ui;
  const int chars = (out_w * 9 / 10) / cw > 52 ? 52 : (out_w * 9 / 10) / cw - 4;
  const SDL_Color dim = {4, 8, 20, 190};
  char lines[12][64];
  int n = 0, i;
  const char *s = detail != NULL ? detail : "";
  int panel_w, panel_h, x, y;
  sv_host_fill(h, 0, 0, out_w, out_h, dim);
  while (*s != '\0' && n < 10) {
    size_t take = strlen(s);
    if ((int)take > chars) {
      take = (size_t)chars;
      while (take > 8u && s[take] != ' ') --take;
    }
    if (take >= sizeof lines[0]) take = sizeof lines[0] - 1u;
    memcpy(lines[n], s, take);
    lines[n][take] = '\0';
    ++n;
    s += take;
    while (*s == ' ') ++s;
  }
  panel_w = (chars + 4) * cw;
  panel_h = (n + 5) * 12 * ui + 2 * pad;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  sv_host_bevel(h, x, y, panel_w, panel_h, ui, c_panel, c_edge, c_hi, c_shadow);
  sv_host_draw_text(h, (float)(x + pad), (float)(y + pad), ui, c_err, title != NULL ? title : "The game stopped");
  for (i = 0; i < n; ++i) sv_host_draw_text(h, (float)(x + pad), (float)(y + pad + (2 + i) * 12 * ui), ui, c_text, lines[i]);
  sv_host_draw_text(h, (float)(x + pad), (float)(y + panel_h - pad - 10 * ui), ui, c_text2, "Enter / Esc: close   F12: screenshot");
}
