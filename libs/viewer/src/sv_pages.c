/* The pause overlay: page construction, event application and drawing. Menu mechanics live in sv_menu.c (pure); everything here only
 * edits SvSettings (host presentation / input mapping) and never touches a guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_internal.h"

enum {
  PAGE_MAIN = 1, PAGE_QUICK, PAGE_VIDEO, PAGE_CONTROLS, PAGE_AUDIO, PAGE_SYSTEM, PAGE_PADSTATUS, PAGE_CONFIRM_QUIT, PAGE_CONFIRM_RESET, PAGE_CONFIRM_VIDEO, PAGE_CONFIRM_CONTROLS
};
enum {
  I_RESUME = 1, I_GO_QUICK, I_GO_VIDEO, I_GO_CONTROLS, I_GO_AUDIO, I_GO_SYSTEM, I_QUIT,
  I_FULLSCREEN, I_SCALE, I_ASPECT, I_PRESET, I_VSYNC, I_FPS, I_SCANLINES, I_MASK, I_CURVE, I_BLOOM, I_BRIGHT, I_VIDEO_DEFAULTS,
  I_VOLUME, I_MUTE, I_AUDIO_NOTE,
  I_SHOT, I_RESET, I_INFO,
  I_PLAYER, I_PAD, I_PAD_ASSIGNED, I_DEADZONE, I_MENUKEY, I_CTRL_DEFAULTS, I_GO_PADSTATUS, I_PAD_RESCAN, I_PAD_HID,
  I_YES, I_NO,
  I_BIND = 100 /* + action (one row per action: keyboard and gamepad columns) */
};

static const SDL_Color c_text = {220, 232, 255, 255}, c_text2 = {169, 183, 221, 255}, c_dim = {120, 133, 170, 255}, c_btn_hi = {64, 191, 245, 255}, c_btn_text = {244, 247, 255, 255},
                       c_focus = {140, 224, 255, 255}, c_err = {255, 129, 119, 255};

/* ---- naming ---- */

static const char *pad_button_name(int b) {
  if (b < 0) return "Unbound";
  return sv_pad_position_name(b);
}

static const char *key_name(int sc) {
  const char *n;
  if (sc <= 0) return "Unbound";
  n = SDL_GetScancodeName((SDL_Scancode)sc);
  return (n != NULL && *n != '\0') ? n : "Key";
}

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
  static const char *const scale[] = {"Fit to Window", "Integer"}, *const aspect[] = {"Square pixels", "4:3 (corrected)"};
  static const char *const preset[] = {"Pixel Perfect", "CRT Soft", "CRT Classic", "CRT Arcade", "Custom"};
  const SvVideoSettings *v = &h->settings.video;
  add_toggle(p, I_FULLSCREEN, "Fullscreen", v->fullscreen, "Borderless desktop fullscreen; no display mode change. F11 also toggles.");
  add_choice(p, I_SCALE, "Scaling", (int)v->scale, 2, scale, "Fit uses any size; Integer keeps whole-number pixel multiples when they fit.");
  add_choice(p, I_ASPECT, "Aspect Ratio", (int)v->aspect, 2, aspect, "Original keeps square pixels; 4:3 stretches to the aspect of a TV picture.");
  add_choice(p, I_PRESET, "Video Effect / CRT", (int)v->preset, v->preset == SV_CRT_CUSTOM ? 5 : 4, preset, "Pixel Perfect is the untouched picture. CRT presets add real scanline/mask/glow/curve passes.");
  if (full) {
    add_slider(p, I_SCANLINES, "Scanlines", v->scanlines, 0, 100, 5, "%", "Darkens the gap between source lines (needs a window at least 1.5x the picture).");
    add_slider(p, I_MASK, "Phosphor mask", v->mask, 0, 100, 5, "%", "RGB stripe mask over the picture.");
    add_slider(p, I_CURVE, "Curvature", v->curvature, 0, 100, 5, "%", "Barrel distortion of the picture with corner falloff.");
    add_slider(p, I_BLOOM, "Bloom", v->bloom, 0, 100, 5, "%", "Soft glow around bright pixels.");
    add_slider(p, I_BRIGHT, "Brightness", v->brightness, 50, 150, 5, "%", "Picture brightness; values above 100 brighten to compensate for scanlines.");
  }
  add_toggle(p, I_VSYNC, "VSync", v->vsync, "Syncs presentation to the display. Game speed is paced by the console clock either way.");
  add_toggle(p, I_FPS, "Show FPS Counter", v->show_fps, "Shows the presentation rate on the game screen.");
  if (full) sv_page_add(p, SV_ITEM_BUTTON, I_VIDEO_DEFAULTS, "Restore video defaults");
}

static void build_controls(SvHost *h, SvPage *p) {
  static const char *const players[] = {"Player 1", "Player 2"};
  const SvBindings *b = &h->settings.bind[h->cfg.system];
  const int player = h->bind_player;
  int a, slot, i;
  SvItem *item;
  add_choice(p, I_PLAYER, "Player", player, 2, players, "Each console has two controller ports. Left/Right switches player.");
  slot = sv_host_pad_assigned(h, player);
  item = sv_page_add(p, SV_ITEM_LABEL, I_PAD_ASSIGNED, "Device");
  if (player == 0 && h->pad_count > 1) {
    const char *names[SV_MAX_PADS + 1];
    int current = 0;
    names[0] = "Auto (first connected)";
    for (i = 0; i < h->pad_count; ++i) { names[i + 1] = h->pads[i].name; if (strcmp(h->pads[i].name, h->settings.pad_name) == 0) current = i + 1; }
    item->kind = SV_ITEM_CHOICE; item->enabled = 1; item->id = I_PAD; item->min = 0; item->max = h->pad_count; item->value = current;
    sv_item_set_text(item, names[current]);
    sv_item_set_hint(item, "Choose which connected gamepad plays Player 1 (the next one plays Player 2).");
  } else {
    if (slot >= 0) sv_item_set_text(item, h->pads[slot].name);
    else if (h->pad_count > 0) sv_item_set_text(item, "Keyboard (no spare pad)");
    else {
      char txt[48];
      const SvPadState st = sv_diag_overall(h->diag, NULL);
      if (sv_diag_count(h->diag) == 0) snprintf(txt, sizeof txt, "Keyboard (no controller seen)");
      else snprintf(txt, sizeof txt, "Keyboard (controller: state %s)", sv_pad_state_code(st));
      sv_item_set_text(item, txt);
    }
  }
  item = sv_page_add(p, SV_ITEM_BUTTON, I_GO_PADSTATUS, "Controller status & test");
  if (item != NULL) { sv_item_set_icon(item, "controller"); sv_item_set_hint(item, "Shows what SDL sees (also controllers it cannot use), live buttons and the exact reason a pad does not work."); }
  for (a = 0; a < SV_ACT_COUNT; ++a) {
    char label[24];
    if (!sv_action_available(h->cfg.system, player, (SvAction)a)) continue;
    item = sv_page_add(p, SV_ITEM_BINDING, I_BIND + a, "");
    snprintf(label, sizeof label, "%s", sv_action_label(h->cfg.system, (SvAction)a));
    snprintf(item->label, sizeof item->label, "%s", label);
    sv_item_set_text(item, key_name(b->key[player][a]));
    sv_item_set_text2(item, slot >= 0 || b->pad[player][a] >= 0 ? pad_button_name(b->pad[player][a]) : "No pad");
    sv_item_set_hint(item, "Left/Right: keyboard or gamepad column. Enter: press the new input. Backspace while listening clears it.");
  }
  add_slider(p, I_DEADZONE, "Stick dead zone", h->settings.deadzone, 5, 60, 5, "%", "Analog stick travel ignored before a direction registers.");
  item = sv_page_add(p, SV_ITEM_BINDING, I_MENUKEY, "Menu key");
  if (item != NULL) { sv_item_set_text(item, key_name(h->settings.menu_key)); sv_item_set_text2(item, "Back"); sv_item_set_hint(item, "Opens this menu. Esc and the gamepad Back button always work as fallbacks."); }
  item = sv_page_add(p, SV_ITEM_BUTTON, I_CTRL_DEFAULTS, "Restore defaults");
  if (item != NULL) sv_item_set_icon(item, "reset");
}

static void add_section(SvPage *p, const char *title);
static void add_note(SvPage *p, const char *text);

static SvItem *status_row(SvPage *p, const char *label, const char *text) {
  SvItem *i = sv_page_add(p, SV_ITEM_LABEL, I_INFO, label);
  if (i != NULL) sv_item_set_text(i, text);
  return i;
}

static const char *conn_name(int c) {
  switch (c) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired (USB)";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    case SV_DIAG_NOT_OPENED: return "not opened";
    default: return "unknown";
  }
}

static void build_padstatus(SvHost *h, SvPage *p) {
  SvDiagContext ctx;
  SvPadState overall;
  char buf[96];
  int i, b;
  SvItem *item;
  ctx.settings = &h->settings; ctx.system = h->cfg.system; ctx.player = h->bind_player; ctx.nav_checked = 0;
  overall = sv_diag_overall(h->diag, &ctx);
  add_section(p, "Controller status");
  snprintf(buf, sizeof buf, "[%s] %s", sv_pad_state_code(overall), sv_pad_state_title(overall));
  item = status_row(p, "State", buf);
  add_note(p, sv_pad_state_advice(overall));
  sv_diag_sdl_version(buf, sizeof buf);
  status_row(p, "SDL", buf);
  if (!sv_diag_gamepad_subsystem_ok(h->diag)) {
    snprintf(buf, sizeof buf, "FAILED: %.80s", sv_diag_gamepad_init_error(h->diag)[0] != '\0' ? sv_diag_gamepad_init_error(h->diag) : "no SDL error text");
    item = status_row(p, "Gamepad subsystem", buf);
  }
  if (sv_diag_count(h->diag) == 0) status_row(p, "Devices", "none");
  for (i = 0; i < sv_diag_count(h->diag) && i < 2; ++i) {
    const SvDiagDevice *d = sv_diag_device(h->diag, i);
    const SvPadState st = sv_diag_state_of(h->diag, d, &ctx);
    char held[96] = "";
    add_section(p, d->name);
    snprintf(buf, sizeof buf, "[%s] %s", sv_pad_state_code(st), d->is_gamepad ? (d->open_ok ? "gamepad, open" : "gamepad, OPEN FAILED") : "raw joystick (no mapping)");
    status_row(p, "State", buf);
    snprintf(buf, sizeof buf, "%04x:%04x  %s", d->vendor, d->product, conn_name(d->connection));
    status_row(p, "VID:PID / link", buf);
    snprintf(buf, sizeof buf, "%.32s", d->guid);
    status_row(p, "GUID", buf);
    if (d->open_error[0] != '\0') { snprintf(buf, sizeof buf, "%.46s", d->open_error); status_row(p, "SDL error", buf); }
    snprintf(buf, sizeof buf, "%lu event(s)%s", d->is_gamepad ? d->events : d->raw_events, d->is_gamepad ? "" : " (raw)");
    status_row(p, "Input", buf);
    for (b = 0; b < 21; ++b) {
      if (!((d->buttons >> (unsigned)b) & 1u)) continue;
      if (strlen(held) + 20u < sizeof held) {
        const char *nl = sv_pad_nintendo_label(b);
        snprintf(held + strlen(held), sizeof held - strlen(held), "%s%s", held[0] != '\0' ? ", " : "", sv_pad_position_name(b));
        if (nl != NULL && strlen(held) + 6u < sizeof held) snprintf(held + strlen(held), sizeof held - strlen(held), "(%s)", nl);
      }
    }
    status_row(p, "Pressed now", held[0] != '\0' ? held : "(nothing: press a button)");
    snprintf(buf, sizeof buf, "x %+d%%  y %+d%%", d->axis[SDL_GAMEPAD_AXIS_LEFTX] * 100 / 32767, d->axis[SDL_GAMEPAD_AXIS_LEFTY] * 100 / 32767);
    status_row(p, "Left stick", buf);
  }
  add_section(p, "Menu button");
  add_note(p, "The pause menu opens with Back (Select, Nintendo -) or Guide (Home), or Esc. Start (Nintendo +) is a game button: Genesis Start / Master System Pause. "
              "Positions are SDL's: South/East are Nintendo B/A.");
  item = sv_page_add(p, SV_ITEM_BUTTON, I_PAD_RESCAN, "Rescan controllers");
  if (item != NULL) sv_item_set_icon(item, "reset");
  item = sv_page_add(p, SV_ITEM_BUTTON, I_PAD_HID, "Scan OS devices (Nintendo)");
  if (item != NULL) sv_item_set_hint(item, "Read-only: lists Nintendo HID devices the OS exposes (USB or Bluetooth), even ones SDL does not offer.");
  for (i = 0; i < h->hid_count && i < 4; ++i) {
    const SvDiagHid *hd = &h->hid[i];
    snprintf(buf, sizeof buf, "%04x:%04x %s %s%s", hd->vendor, hd->product, hd->bus == 1 ? "USB" : (hd->bus == 2 ? "Bluetooth" : "bus ?"), hd->family[0] != '\0' ? hd->family : hd->product_string,
             hd->matched_joystick ? " (seen by SDL)" : " (NOT a joystick in SDL)");
    status_row(p, "OS device", buf);
  }
  if (h->hid_scanned) {
    if (h->hid_count < 0) status_row(p, "OS scan", "failed");
    else if (h->hid_count == 0) status_row(p, "OS scan", "no Nintendo HID device found");
  }
}

static void add_section(SvPage *p, const char *title) {
  SvItem *i = sv_page_add(p, SV_ITEM_LABEL, 0, title);
  if (i != NULL) sv_item_set_icon(i, "section");
}

static void add_note(SvPage *p, const char *text) {
  SvItem *i = sv_page_add(p, SV_ITEM_LABEL, 0, "");
  if (i != NULL) { sv_item_set_icon(i, "note"); sv_item_set_hint(i, text); }
}

static void build_audio(SvHost *h, SvPage *p) {
  SvItem *item;
  add_slider(p, I_VOLUME, "Master Volume", h->settings.audio.volume, 0, 100, 5, "%", "Playback gain only; the console's generated sound is not changed.");
  add_toggle(p, I_MUTE, "Mute", h->settings.audio.muted, "Silences playback without touching the emulated sound.");
  item = sv_page_add(p, SV_ITEM_LABEL, I_AUDIO_NOTE, "Output Device");
  if (h->audio != NULL) {
    const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(h->audio);
    const char *name = dev != 0 ? SDL_GetAudioDeviceName(dev) : NULL;
    sv_item_set_text(item, name != NULL ? name : "System default");
  } else {
    sv_item_set_text(item, "None (silent)");
  }
  add_note(p, "These settings only change how the sound is played on your device (gain). The console's sound itself is unchanged. The output device follows the OS default.");
}

static void build_system(SvHost *h, SvPage *p) {
  SvDiagnostics d;
  SvItem *item;
  char buf[64];
  int out_w = 0, out_h = 0;
  SvRect dest;
  const unsigned secs = (unsigned)((SDL_GetTicks() - h->session_start) / 1000u);
  memset(&d, 0, sizeof d);
  if (h->cfg.diagnostics != NULL) h->cfg.diagnostics(h->cfg.diagnostics_ctx, &d);
  SDL_GetCurrentRenderOutputSize(h->renderer, &out_w, &out_h);
  dest = sv_layout_dest(out_w, out_h, h->last_w, h->last_h, h->settings.video.aspect, h->settings.video.scale);
#define ROW(label, ...) do { item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, label); if (item != NULL) { snprintf(buf, sizeof buf, __VA_ARGS__); sv_item_set_text(item, buf); } } while (0)
  add_section(p, "Game Information");
  ROW("System", "%s", sv_system_name(h->cfg.system));
  if (h->cfg.rom_sha256 != NULL && strlen(h->cfg.rom_sha256) == 64u) {
    char half[40];
    snprintf(half, sizeof half, "%.32s", h->cfg.rom_sha256);
    item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, "ROM SHA-256"); sv_item_set_text(item, half);
    snprintf(half, sizeof half, "%.32s", h->cfg.rom_sha256 + 32);
    item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, ""); sv_item_set_text(item, half);
  }
  ROW("Session Time", "%02u:%02u:%02u", secs / 3600u, (secs / 60u) % 60u, secs % 60u);
  add_section(p, "Runtime Information");
  ROW("Renderer", "%s", sv_host_renderer_name(h));
  ROW("Picture / Output", "%dx%d -> %dx%d", h->last_w, h->last_h, dest.w, dest.h);
  ROW("Frame Rate", "%.1f FPS (%.1f ms)", h->fps, h->frame_ms);
  if (d.audio_active) ROW("Audio Queue", "%u ms, %llu dropped", (unsigned)d.audio_queued_ms, (unsigned long long)d.audio_dropped);
  else ROW("Audio", "no device (silent)");
  ROW("Gamepads", "%d connected", h->pad_count);
  add_section(p, "Actions");
  item = sv_page_add(p, SV_ITEM_BUTTON, I_SHOT, "Take Screenshot");
  if (item != NULL) { sv_item_set_icon(item, "screenshot"); sv_item_set_hint(item, h->last_screenshot[0] != '\0' ? h->last_screenshot : "Saves the game picture (without menus or effects) as an image."); }
  item = sv_page_add(p, SV_ITEM_BUTTON, I_RESET, "Reset Game");
  if (item != NULL) sv_item_set_icon(item, "reset");
  if (item != NULL && !h->cfg.can_reset) sv_item_disable(item, "Not available: this console's viewer has no proven reset seam.");
  else if (item != NULL) sv_item_set_hint(item, "Restarts the machine from power-on (the recorded input restarts too).");
#undef ROW
}

static const char *exit_label(const SvHost *h) { (void)h; return SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Return to Launcher" : "Quit"; }

void sv_pages_rebuild(SvHost *h) {
  SvPage *p = sv_menu_top(&h->menu);
  char line_buf[64];
  int focus_id = 0, scroll;
  SvItem *item;
  if (p == NULL) return;
  if (p->focus >= 0 && p->focus < p->count) focus_id = p->items[p->focus].id;
  scroll = p->scroll;
  { int k; for (k = 0; k < p->count; ++k) { const int bi = p->items[k].id == I_MENUKEY ? SV_ACT_COUNT : p->items[k].id - I_BIND; if (p->items[k].kind == SV_ITEM_BINDING && bi >= 0 && bi <= SV_ACT_COUNT) h->bind_col[bi] = (unsigned char)p->items[k].value; } }
  p->count = 0;
  switch (p->page_id) {
    case PAGE_MAIN:
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_RESUME, "Resume Game"), "play");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_QUICK, "Quick Settings"), "settings");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_VIDEO, "Video & Display"), "video");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_CONTROLS, "Controls"), "controller");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_AUDIO, "Audio"), "audio");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_SYSTEM, "System"), "info");
      item = sv_page_add(p, SV_ITEM_BUTTON, I_QUIT, exit_label(h));
      if (item != NULL) sv_item_set_icon(item, "quit");
      if (item != NULL) sv_item_set_hint(item, "The game is not saved: this viewer has no save states.");
      break;
    case PAGE_QUICK: build_video_core(h, p, 0); break;
    case PAGE_VIDEO: build_video_core(h, p, 1); break;
    case PAGE_CONTROLS: build_controls(h, p); break;
    case PAGE_PADSTATUS: build_padstatus(h, p); break;
    case PAGE_AUDIO: build_audio(h, p); break;
    case PAGE_SYSTEM: build_system(h, p); break;
    case PAGE_CONFIRM_QUIT:
      snprintf(line_buf, sizeof line_buf, "Yes, %s", exit_label(h));
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, line_buf);
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "No, Stay in Game");
      break;
    case PAGE_CONFIRM_RESET:
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Yes, Reset Game");
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "No, Keep Playing");
      break;
    case PAGE_CONFIRM_VIDEO:
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Yes, Restore Video Defaults");
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "No, Keep My Settings");
      break;
    case PAGE_CONFIRM_CONTROLS:
      sv_page_add(p, SV_ITEM_BUTTON, I_YES, "Yes, Restore Control Defaults");
      sv_page_add(p, SV_ITEM_BUTTON, I_NO, "No, Keep My Controls");
      break;
    default: break;
  }
  { int k; for (k = 0; k < p->count; ++k) { const int bi = p->items[k].id == I_MENUKEY ? SV_ACT_COUNT : p->items[k].id - I_BIND; if (p->items[k].kind == SV_ITEM_BINDING && bi >= 0 && bi <= SV_ACT_COUNT) p->items[k].value = h->bind_col[bi]; } }
  p->scroll = scroll;
  if (focus_id != 0) sv_page_restore_focus(p, focus_id); else sv_page_focus_first(p);
  if (p->focus >= p->count) sv_page_focus_first(p);
}

static void open_page(SvHost *h, int page_id, const char *title) {
  SvPage *p = sv_menu_push(&h->menu, page_id, title);
  if (p == NULL) return;
  sv_pages_rebuild(h);
  sv_page_focus_first(sv_menu_top(&h->menu));
  if (page_id >= PAGE_CONFIRM_QUIT) sv_page_restore_focus(sv_menu_top(&h->menu), I_NO); /* a destructive confirmation never starts on "yes" */
}

void sv_pages_open_main(SvHost *h) {
  sv_menu_reset(&h->menu);
  memset(&h->listen, 0, sizeof h->listen);
  h->bind_player = 0;
  open_page(h, PAGE_MAIN, "GAME PAUSED");
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
  snprintf(msg, sizeof msg, "Conflict: %s was %s (P%d); that binding is now unbound", what, from_label, from->player + 1);
  snprintf(h->conflict_msg, sizeof h->conflict_msg, "%s", msg);
  sv_host_toast(h, msg);
}

void sv_pages_listen_key(SvHost *h, int sc) {
  char msg[96];
  int conflict = 0;
  SvBindingOwner moved = {0, SV_ACT_UP};
  h->conflict_msg[0] = '\0';
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
  h->conflict_msg[0] = '\0';
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
      case I_GO_VIDEO: open_page(h, PAGE_VIDEO, "VIDEO & DISPLAY SETTINGS"); return;
      case I_GO_CONTROLS: open_page(h, PAGE_CONTROLS, "CONTROLS"); return;
      case I_GO_PADSTATUS: open_page(h, PAGE_PADSTATUS, "CONTROLLER STATUS"); return;
      case I_PAD_RESCAN: sv_diag_scan(h->diag); sv_host_toast(h, "Rescanned controllers"); return;
      case I_PAD_HID: {
        char err[96];
        h->hid_count = sv_diag_hid_scan(h->diag, h->hid, SV_DIAG_MAX_HID, err, sizeof err);
        h->hid_scanned = 1;
        if (h->hid_count < 0) sv_host_toast(h, err[0] != '\0' ? err : "OS device scan failed");
        return;
      }
      case I_GO_AUDIO: open_page(h, PAGE_AUDIO, "AUDIO"); return;
      case I_GO_SYSTEM: open_page(h, PAGE_SYSTEM, "SYSTEM / DIAGNOSTICS"); return;
      case I_QUIT: open_page(h, PAGE_CONFIRM_QUIT, SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Return to Launcher?" : "Quit the game?"); return;
      case I_SHOT: sv_host_take_screenshot(h); return;
      case I_RESET: if (h->cfg.can_reset) open_page(h, PAGE_CONFIRM_RESET, "Reset the game?"); return;
      case I_VIDEO_DEFAULTS: open_page(h, PAGE_CONFIRM_VIDEO, "Restore video defaults?"); return;
      case I_CTRL_DEFAULTS: open_page(h, PAGE_CONFIRM_CONTROLS, "Restore control defaults?"); return;
      case I_MENUKEY: if (ev.value == 1) sv_host_toast(h, "Back always opens the menu on a gamepad"); else begin_listen(h, SV_LISTEN_MENU_KEY, SV_ACT_UP); return;
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
    if (ev.item_id >= I_BIND && ev.item_id < I_BIND + SV_ACT_COUNT) {
      if (ev.value == 1 && sv_host_pad_assigned(h, h->bind_player) < 0) sv_host_toast(h, "Connect a gamepad to rebind its buttons");
      else begin_listen(h, ev.value == 1 ? SV_LISTEN_PAD : SV_LISTEN_KEY, (SvAction)(ev.item_id - I_BIND));
    }
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

/* ---- drawing (the concept boards: branded rounded panel, icon rows, switches, knobs, button-glyph footer) ---- */

static const SDL_Color k_panel = {6, 16, 40, 238}, k_row = {12, 28, 62, 255}, k_cell = {5, 12, 30, 255}, k_line = {34, 70, 140, 255}, k_sel = {52, 128, 238, 255},
                       k_sel_edge = {120, 190, 255, 255}, k_glow = {46, 104, 214, 255}, k_on = {52, 140, 245, 255}, k_green = {64, 200, 140, 255}, k_red = {235, 80, 80, 255},
                       k_warn = {255, 197, 109, 255};

static int text_w(const char *s, int ui) { return (int)strlen(s) * 8 * ui; }

static void panel_frame(SvHost *h, float x, float y, float w, float hh, int ui) {
  const SDL_Color soft = {46, 104, 214, 60};
  sv_host_round_fill(h, x - (float)ui, y - (float)ui, w + 2.0f * (float)ui, hh + 2.0f * (float)ui, (float)(6 * ui), soft);
  sv_host_round_fill(h, x, y, w, hh, (float)(5 * ui), k_panel);
  sv_host_round_stroke(h, x, y, w, hh, (float)(5 * ui), ui > 1 ? 2 : 1, k_glow);
}

static void keycap(SvHost *h, int x, int y, const char *label, int ui) {
  const int w = text_w(label, ui) + 8 * ui, hh = 12 * ui;
  sv_host_round_fill(h, (float)x, (float)y, (float)w, (float)hh, (float)(2 * ui), k_line);
  sv_host_draw_text(h, (float)(x + 4 * ui), (float)(y + 2 * ui), ui, c_text, label);
}

/* Right-aligned footer cues: coloured A/B circles after gamepad input, key caps after keyboard input. */
static void draw_cues(SvHost *h, int right, int y, int ui, int back_label) {
  const char *sel = "Select", *bk = back_label ? "Back" : "Close";
  const int pad_mode = h->last_input_pad;
  const int glyph1 = pad_mode ? 11 * ui : text_w("Enter", ui) + 8 * ui, glyph2 = pad_mode ? 11 * ui : text_w("Esc", ui) + 8 * ui;
  const int total = glyph1 + 4 * ui + text_w(sel, ui) + 10 * ui + glyph2 + 4 * ui + text_w(bk, ui);
  int x = right - total;
  if (pad_mode) {
    sv_host_round_fill(h, (float)x, (float)y, (float)glyph1, (float)glyph1, (float)(5 * ui), k_green);
    sv_host_draw_text(h, (float)(x + 2 * ui + ui / 2), (float)(y + 2 * ui), ui, c_btn_text, "A");
  } else keycap(h, x, y, "Enter", ui);
  x += glyph1 + 4 * ui;
  sv_host_draw_text(h, (float)x, (float)(y + 2 * ui), ui, c_text2, sel);
  x += text_w(sel, ui) + 10 * ui;
  if (pad_mode) {
    sv_host_round_fill(h, (float)x, (float)y, (float)glyph2, (float)glyph2, (float)(5 * ui), k_red);
    sv_host_draw_text(h, (float)(x + 2 * ui + ui / 2), (float)(y + 2 * ui), ui, c_btn_text, "B");
  } else keycap(h, x, y, "Esc", ui);
  x += glyph2 + 4 * ui;
  sv_host_draw_text(h, (float)x, (float)(y + 2 * ui), ui, c_text2, bk);
}

static int item_rows(const SvItem *it) { return strcmp(it->icon, "note") == 0 ? 5 : 1; }

void sv_pages_draw(SvHost *h, int out_w, int out_h) {
  SvPage *page = sv_menu_top(&h->menu);
  const int preview = sv_pages_wants_preview(h);
  int ui = sv_host_ui_scale(out_w, out_h), cols, cw, row_h, pad, head_h, foot_h, panel_w, panel_h, x, y, i, drawn, avail, used, rx, rwid, left_w;
  const int is_confirm = page != NULL && page->page_id >= PAGE_CONFIRM_QUIT;
  const int video = page != NULL && page->page_id == PAGE_VIDEO;
  char line[160];
  if (page == NULL) return;
  cols = is_confirm ? 60 : (page->page_id == PAGE_MAIN ? 40 : (video ? 76 : (page->page_id == PAGE_CONTROLS ? 56 : (page->page_id == PAGE_SYSTEM ? 54 : 50))));
  {
    const int fit = out_w * 94 / 100 / (cols * 8);
    if (fit < ui) ui = fit < 1 ? 1 : fit;
  }
  cw = 8 * ui; row_h = 15 * ui; pad = 8 * ui; head_h = is_confirm ? 0 : 22 * ui; foot_h = is_confirm ? 8 * ui : 30 * ui;
  panel_w = cols * cw;
  if (panel_w > out_w - 2 * pad) panel_w = out_w - 2 * pad;
  left_w = video ? panel_w * 57 / 100 : panel_w;
  /* how many rows fit */
  avail = out_h - 2 * pad - head_h - foot_h - 2 * pad;
  if (avail < 4 * row_h) avail = 4 * row_h;
  {
    int est = 0;
    for (i = page->scroll, used = 0; i < page->count && used + item_rows(&page->items[i]) * row_h <= avail; ++i) { used += item_rows(&page->items[i]) * row_h; ++est; }
    if (est < 1) est = 1;
    sv_menu_ensure_visible(page, est);
  }
  for (i = page->scroll, used = 0, drawn = 0; i < page->count && used + item_rows(&page->items[i]) * row_h <= avail; ++i) { used += item_rows(&page->items[i]) * row_h; ++drawn; }
  if (drawn < 1) drawn = 1;
  panel_h = head_h + used + foot_h + 2 * pad;
  if (is_confirm) panel_h = 46 * ui + (page->count) * (row_h + 4 * ui) + 10 * ui;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  if (preview && !video) { x = pad; }
  if (y < pad) y = pad;
  panel_frame(h, (float)x, (float)y, (float)panel_w, (float)panel_h, ui);
  h->ui_rows = 0;
  if (!is_confirm) {
    sv_host_icon_draw(h, "controller", (float)(x + pad), (float)(y + 4 * ui), (float)(16 * ui), c_btn_hi);
    sv_host_draw_text(h, (float)(x + pad + 20 * ui), (float)(y + 8 * ui), ui, c_text, "SEGARECOMP");
    sv_host_draw_text(h, (float)(x + pad + 20 * ui + 1), (float)(y + 8 * ui), ui, c_text, "SEGARECOMP");
    sv_host_draw_text(h, (float)(x + panel_w - pad - text_w(page->title, ui)), (float)(y + 8 * ui), ui, c_text2, page->title);
    { const SDL_Color rule = {34, 70, 140, 255}; sv_host_fill(h, x + pad, y + head_h, panel_w - 2 * pad, ui, rule); }
  }
  rwid = left_w - 2 * pad;
  rx = x + pad;
  if (is_confirm) {
    const char *msg = page->page_id == PAGE_CONFIRM_QUIT ? (SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Return to the launcher and close the game?" : "Close the game?") :
                      page->page_id == PAGE_CONFIRM_RESET ? "Restart the game from power-on?" : "Reset these settings to their defaults?";
    const char *sub = page->page_id == PAGE_CONFIRM_QUIT ? "Any unsaved progress will be lost: there are no save states." : (page->page_id == PAGE_CONFIRM_RESET ? "Progress since the last power-on is lost." : "Your other settings are kept.");
    sv_host_icon_draw(h, "warning", (float)(x + 14 * ui), (float)(y + 10 * ui), (float)(16 * ui), k_warn);
    sv_host_draw_text(h, (float)(x + 34 * ui), (float)(y + 14 * ui), ui, c_text, page->title);
    sv_host_draw_text(h, (float)(x + 14 * ui), (float)(y + 30 * ui), ui, c_text2, sub);
    (void)msg;
  }
  {
    int cy = is_confirm ? y + 46 * ui : y + head_h + pad;
    for (i = 0; i < drawn; ++i) {
      const int idx = page->scroll + i;
      const SvItem *it = &page->items[idx];
      const int rows = item_rows(it), hh = rows * row_h;
      const int focused = idx == page->focus;
      SDL_Color tc = it->enabled ? c_text : c_dim;
      const int ty = cy + (row_h - 8 * ui) / 2;
      const int is_note = strcmp(it->icon, "note") == 0, is_section = strcmp(it->icon, "section") == 0;
      if (h->ui_rows < SV_MENU_MAX_ITEMS) { h->ui_item_rect[h->ui_rows][0] = rx; h->ui_item_rect[h->ui_rows][1] = cy; h->ui_item_rect[h->ui_rows][2] = rwid; h->ui_item_rect[h->ui_rows][3] = hh; h->ui_rows++; }
      if (is_note) {
        sv_host_round_fill(h, (float)rx, (float)cy + (float)ui, (float)rwid, (float)(hh - 2 * ui), (float)(3 * ui), k_row);
        sv_host_icon_draw(h, "info", (float)(rx + 6 * ui), (float)(cy + 6 * ui), (float)(14 * ui), c_focus);
        {
          const char *s = it->hint;
          int line_no = 0;
          const int maxc = (rwid - 32 * ui) / cw;
          while (*s != '\0' && line_no < 6) {
            size_t take = strlen(s);
            if ((int)take > maxc) { take = (size_t)maxc; while (take > 8u && s[take] != ' ') --take; }
            snprintf(line, sizeof line, "%.*s", (int)take, s);
            sv_host_draw_text(h, (float)(rx + 26 * ui), (float)(cy + 6 * ui + line_no * 10 * ui), ui, c_text2, line);
            s += take; while (*s == ' ') ++s;
            ++line_no;
          }
        }
        cy += hh;
        continue;
      }
      if (is_section) {
        sv_host_draw_text(h, (float)(rx + 2 * ui), (float)(ty + ui), ui, c_focus, it->label);
        { const SDL_Color r = {34, 70, 140, 255}; sv_host_fill(h, rx, cy + row_h - 2 * ui, rwid, ui, r); }
        cy += hh;
        continue;
      }
      if (focused && it->kind != SV_ITEM_LABEL) {
        sv_host_round_fill(h, (float)rx, (float)cy, (float)rwid, (float)(row_h - ui), (float)(3 * ui), k_sel);
        sv_host_round_stroke(h, (float)rx, (float)cy, (float)rwid, (float)(row_h - ui), (float)(3 * ui), 1, k_sel_edge);
        tc = c_btn_text;
      } else if (it->kind == SV_ITEM_BUTTON) {
        sv_host_round_fill(h, (float)rx, (float)cy, (float)rwid, (float)(row_h - ui), (float)(3 * ui), k_row);
      }
      {
        int lx = rx + 6 * ui;
        if (it->icon[0] != '\0') { sv_host_icon_draw(h, it->icon, (float)lx, (float)(cy + (row_h - 12 * ui) / 2 - ui / 2), (float)(12 * ui), focused ? c_btn_text : c_text); lx += 18 * ui; }
        else if (it->kind == SV_ITEM_BUTTON) lx += 2 * ui;
        { const int lab_w = (it->kind == SV_ITEM_BUTTON ? rwid : rwid * 46 / 100) - (lx - rx) - 4 * ui; truncate_to(line, sizeof line, it->label, lab_w / cw);
          sv_host_draw_text(h, (float)lx, (float)ty, ui, it->kind == SV_ITEM_LABEL ? c_text2 : tc, line); }
      }
      {
        const int vx0 = rx + rwid * 48 / 100, vw = rx + rwid - 6 * ui - vx0;
        switch (it->kind) {
          case SV_ITEM_TOGGLE: {
            const int sw = 22 * ui, sh = 11 * ui, sx = rx + rwid - 8 * ui - sw, sy = cy + (row_h - sh) / 2 - ui / 2;
            sv_host_round_fill(h, (float)sx, (float)sy, (float)sw, (float)sh, (float)(sh / 2), it->value ? k_on : k_line);
            sv_host_round_fill(h, (float)(it->value ? sx + sw - sh + ui : sx + ui), (float)(sy + ui), (float)(sh - 2 * ui), (float)(sh - 2 * ui), (float)(sh / 2), c_btn_text);
            sv_host_draw_text(h, (float)(sx - 8 * ui - text_w(it->text, ui)), (float)ty, ui, focused ? c_btn_text : c_text2, it->text);
            break;
          }
          case SV_ITEM_CHOICE:
            if (it->id == I_PLAYER) {  /* player tabs */
              int t;
              const int tw = vw / 2;
              for (t = 0; t < 2; ++t) {
                const int tx = vx0 + t * tw;
                const SDL_Color f = t == it->value ? k_sel : k_cell;
                sv_host_round_fill(h, (float)(tx + ui), (float)(cy + ui), (float)(tw - 2 * ui), (float)(row_h - 3 * ui), (float)(3 * ui), f);
                sv_host_round_stroke(h, (float)(tx + ui), (float)(cy + ui), (float)(tw - 2 * ui), (float)(row_h - 3 * ui), (float)(3 * ui), 1, t == it->value ? k_sel_edge : k_line);
                sv_host_draw_text(h, (float)(tx + (tw - text_w(t == 0 ? "Player 1" : "Player 2", ui)) / 2), (float)ty, ui, t == it->value ? c_btn_text : c_text2, t == 0 ? "Player 1" : "Player 2");
              }
            } else {
              const int cxm = vx0 + vw / 2;
              truncate_to(line, sizeof line, it->text, (vw - 24 * ui) / cw);
              sv_host_draw_text(h, (float)(cxm - text_w(line, ui) / 2), (float)ty, ui, focused ? c_btn_text : c_text, line);
              sv_host_draw_text(h, (float)(vx0 + 2 * ui), (float)ty, ui, focused ? c_btn_text : c_dim, "<");
              sv_host_draw_text(h, (float)(vx0 + vw - 10 * ui), (float)ty, ui, focused ? c_btn_text : c_dim, ">");
            }
            break;
          case SV_ITEM_SLIDER: {
            const int pct_w = 6 * cw, tx = vx0, tw = vw - pct_w - 4 * ui, ty2 = cy + row_h / 2 - ui;
            const int span = it->max - it->min > 0 ? it->max - it->min : 1, fillw = (it->value - it->min) * tw / span;
            sv_host_round_fill(h, (float)tx, (float)(ty2 - ui), (float)tw, (float)(3 * ui), (float)ui, k_line);
            sv_host_round_fill(h, (float)tx, (float)(ty2 - ui), (float)fillw, (float)(3 * ui), (float)ui, k_on);
            sv_host_round_fill(h, (float)(tx + fillw - 3 * ui), (float)(ty2 - 3 * ui), (float)(7 * ui), (float)(7 * ui), (float)(3 * ui), c_btn_text);
            truncate_to(line, sizeof line, it->text, 6);
            sv_host_draw_text(h, (float)(rx + rwid - 6 * ui - text_w(line, ui)), (float)ty, ui, focused ? c_btn_text : c_text, line);
            break;
          }
          case SV_ITEM_LABEL:
            truncate_to(line, sizeof line, it->text, (rwid * 7 / 10) / cw);
            sv_host_draw_text(h, (float)(rx + rwid - 6 * ui - text_w(line, ui)), (float)ty, ui, c_text, line);
            break;
          case SV_ITEM_BINDING: {
            const int cell_w = (vw - 6 * ui) / 2;
            int t;
            for (t = 0; t < 2; ++t) {
              const int tx = vx0 + t * (cell_w + 6 * ui);
              const int on = focused && it->value == t;
              const char *txt = t == 0 ? it->text : it->text2;
              if (it->text2[0] == '\0' && t == 1) break;
              sv_host_round_fill(h, (float)tx, (float)(cy + ui), (float)cell_w, (float)(row_h - 3 * ui), (float)(3 * ui), on ? k_cell : k_cell);
              sv_host_round_stroke(h, (float)tx, (float)(cy + ui), (float)cell_w, (float)(row_h - 3 * ui), (float)(3 * ui), 1, on ? k_sel_edge : k_line);
              truncate_to(line, sizeof line, txt, (cell_w - 8 * ui) / cw);
              sv_host_draw_text(h, (float)(tx + (cell_w - text_w(line, ui)) / 2), (float)ty, ui, on ? c_btn_text : c_text2, line);
            }
            break;
          }
          case SV_ITEM_BUTTON: break;
        }
      }
      cy += hh + (is_confirm ? 4 * ui : 0);
    }
    if (page->count > drawn && drawn > 0) {  /* scroll bar */
      const SDL_Color track = {7, 16, 38, 255};
      const int tx = x + panel_w - 4 * ui, ty = y + head_h + pad, th = used, kh = th * drawn / page->count > 8 * ui ? th * drawn / page->count : 8 * ui;
      sv_host_fill(h, tx, ty, 2 * ui, th, track);
      sv_host_fill(h, tx, ty + (th - kh) * page->scroll / (page->count - drawn), 2 * ui, kh, k_glow);
    }
  }
  if (video) {  /* LIVE PREVIEW column */
    const int px0 = x + left_w, pw = panel_w - left_w - pad, py0 = y + head_h + pad;
    const int pic_h = pw * 9 / 16 > 70 * ui ? 70 * ui : pw * 9 / 16;
    int p;
    sv_host_fill(h, px0 - ui, py0, ui, used, (SDL_Color){34, 70, 140, 255});
    sv_host_draw_text(h, (float)(px0 + 6 * ui), (float)py0, ui, c_focus, "LIVE PREVIEW");
    sv_host_round_fill(h, (float)(px0 + 6 * ui), (float)(py0 + 12 * ui), (float)(pw - 6 * ui), (float)pic_h, (float)(2 * ui), k_cell);
    sv_host_draw_picture(h, (float)(px0 + 8 * ui), (float)(py0 + 14 * ui), (float)(pw - 10 * ui), (float)(pic_h - 4 * ui));
    sv_host_round_stroke(h, (float)(px0 + 6 * ui), (float)(py0 + 12 * ui), (float)(pw - 6 * ui), (float)pic_h, (float)(2 * ui), 1, k_glow);
    for (p = 0; p < 4; ++p) {
      const int bw = (pw - 6 * ui - 4 * ui) / 2, bx = px0 + 6 * ui + (p % 2) * (bw + 4 * ui), by = py0 + 16 * ui + pic_h + (p / 2) * (14 * ui);
      const int on = (int)h->settings.video.preset == p;
      sv_host_round_fill(h, (float)bx, (float)by, (float)bw, (float)(12 * ui), (float)(2 * ui), on ? k_sel : k_row);
      sv_host_round_stroke(h, (float)bx, (float)by, (float)bw, (float)(12 * ui), (float)(2 * ui), 1, on ? k_sel_edge : k_line);
      { const char *nm = sv_crt_preset_name((SvCrtPreset)p); truncate_to(line, sizeof line, nm, (bw - 4 * ui) / cw); sv_host_draw_text(h, (float)(bx + (bw - text_w(line, ui)) / 2), (float)(by + 2 * ui), ui, on ? c_btn_text : c_text2, line); }
    }
    {
      static const char *const desc[] = {"The untouched picture: nearest-neighbour pixels, no filter.", "Subtle scanline gaps and a gentle glow.", "Scanlines, an RGB mask, glow and slight curvature.", "A stronger arcade-monitor look: heavy scanlines, mask and curve."};
      const char *d = h->settings.video.preset == SV_CRT_CUSTOM ? "Custom: your own scanline, mask, curvature, bloom and brightness values." : desc[h->settings.video.preset];
      const int maxc = (pw - 6 * ui) / cw;
      int ly = py0 + 62 * ui + pic_h;
      sv_host_draw_text(h, (float)(px0 + 6 * ui), (float)(ly - 14 * ui), ui, c_text, sv_crt_preset_name(h->settings.video.preset));
      while (*d != '\0') {
        size_t take = strlen(d);
        if ((int)take > maxc) { take = (size_t)maxc; while (take > 8u && d[take] != ' ') --take; }
        snprintf(line, sizeof line, "%.*s", (int)take, d);
        sv_host_draw_text(h, (float)(px0 + 6 * ui), (float)ly, ui, c_text2, line);
        d += take; while (*d == ' ') ++d;
        ly += 10 * ui;
      }
    }
  }
  /* footer */
  {
    const int fy = y + panel_h - foot_h + (is_confirm ? 0 : 2 * ui), max_chars = (panel_w - 2 * pad) / cw;
    const SvItem *cur = page->focus >= 0 && page->focus < page->count ? &page->items[page->focus] : NULL;
    if (!is_confirm) {
      const SDL_Color rule = {34, 70, 140, 255};
      sv_host_fill(h, x + pad, y + panel_h - foot_h, panel_w - 2 * pad, ui, rule);
    }
    if (h->listen.kind != SV_LISTEN_NONE) {
      const unsigned left = 8u - (unsigned)((SDL_GetTicks() - h->listen.started) / 1000u);
      snprintf(line, sizeof line, h->listen.kind == SV_LISTEN_PAD ? "Press a gamepad button (%us)   Esc cancel   Backspace clear" : "Press a key (%us)   Esc cancel   Backspace clear", left > 8u ? 0u : left);
      sv_host_draw_text(h, (float)(x + pad), (float)(fy + 8 * ui), ui, c_focus, line);
    } else if (!is_confirm) {
      const int cues_w = 70 * ui + text_w("Select", ui) + text_w("Close", ui) + 20 * ui;
      if (h->conflict_msg[0] != '\0' && page->page_id == PAGE_CONTROLS) {
        sv_host_round_fill(h, (float)(x + pad), (float)(fy - 24 * ui), (float)(panel_w - 2 * pad), (float)(20 * ui), (float)(3 * ui), (SDL_Color){70, 54, 14, 255});
        sv_host_round_stroke(h, (float)(x + pad), (float)(fy - 24 * ui), (float)(panel_w - 2 * pad), (float)(20 * ui), (float)(3 * ui), 1, k_warn);
        sv_host_icon_draw(h, "warning", (float)(x + pad + 4 * ui), (float)(fy - 21 * ui), (float)(12 * ui), k_warn);
        truncate_to(line, sizeof line, h->conflict_msg, (panel_w - 2 * pad - 24 * ui) / cw);
        sv_host_draw_text(h, (float)(x + pad + 20 * ui), (float)(fy - 18 * ui), ui, c_text, line);
      }
      if (cur != NULL && cur->hint[0] != '\0') {
        const char *hint = cur->hint;
        const int maxh = (panel_w - 2 * pad - cues_w) / cw;
        size_t len = strlen(hint), cut = len;
        const SDL_Color hc = cur->enabled ? c_text2 : c_err;
        if ((int)len > maxh) { cut = (size_t)(maxh > 8 ? maxh : 8); while (cut > 8u && hint[cut] != ' ') --cut; }
        snprintf(line, sizeof line, "%.*s", (int)cut, hint);
        sv_host_draw_text(h, (float)(x + pad), (float)(fy + 2 * ui), ui, hc, line);
        if (cut < len) { const char *rest = hint + cut + (hint[cut] == ' ' ? 1 : 0); truncate_to(line, sizeof line, rest, maxh); sv_host_draw_text(h, (float)(x + pad), (float)(fy + 12 * ui), ui, hc, line); }
      }
      (void)max_chars;
      draw_cues(h, x + panel_w - pad, fy + 8 * ui, ui, h->menu.depth > 1);
    }
  }
}

void sv_draw_stop_screen(SvHost *h, int out_w, int out_h, const char *title, const char *detail) {
  const int ui = sv_host_ui_scale(out_w, out_h), cw = 8 * ui, pad = 10 * ui;
  const int chars = (out_w * 9 / 10) / cw > 56 ? 56 : (out_w * 9 / 10) / cw - 4;
  const SDL_Color dim = {4, 8, 20, 190};
  char lines[12][72];
  int n = 0, i, panel_w, panel_h, x, y;
  const char *s = detail != NULL ? detail : "";
  sv_host_fill(h, 0, 0, out_w, out_h, dim);
  while (*s != '\0' && n < 10) {
    size_t take = strlen(s);
    if ((int)take > chars) { take = (size_t)chars; while (take > 8u && s[take] != ' ') --take; }
    if (take >= sizeof lines[0]) take = sizeof lines[0] - 1u;
    memcpy(lines[n], s, take);
    lines[n][take] = '\0';
    ++n;
    s += take;
    while (*s == ' ') ++s;
  }
  panel_w = (chars + 6) * cw;
  panel_h = (n + 5) * 11 * ui + 2 * pad;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  panel_frame(h, (float)x, (float)y, (float)panel_w, (float)panel_h, ui);
  sv_host_icon_draw(h, "warning", (float)(x + pad), (float)(y + pad - 2 * ui), (float)(16 * ui), k_warn);
  sv_host_draw_text(h, (float)(x + pad + 22 * ui), (float)(y + pad + 2 * ui), ui, c_text, title != NULL ? title : "The game stopped");
  for (i = 0; i < n; ++i) sv_host_draw_text(h, (float)(x + pad), (float)(y + pad + (3 + i) * 11 * ui), ui, c_text2, lines[i]);
  draw_cues(h, x + panel_w - pad, y + panel_h - pad - 12 * ui, ui, 0);
}
