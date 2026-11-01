/* The pause overlay: page construction, event application and drawing. Menu mechanics live in sv_menu.c (pure); everything here only
 * edits SvSettings (host presentation / input mapping) and never touches a guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_internal.h"


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
  if (i != NULL) { i->value = value != 0; sv_item_set_text(i, value ? "On" : "Off"); sv_item_set_hint(i, hint); }
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

static void add_note(SvPage *p, const char *text);

static void build_video_core(SvHost *h, SvPage *p, int full) {
  SvItem *item;
  static const char *const scale[] = {"Fit to Window", "Integer"}, *const aspect[] = {"Square pixels", "4:3 (corrected)"};
  static const char *const preset[] = {"Pixel Perfect", "CRT Soft", "CRT Classic", "CRT Arcade", "Custom"};
  static const char *const wsname[] = {"Original", "Extended (Level 2)", "Enhanced (Level 3)"};
  const SvVideoSettings *v = &h->settings.video;
  char ws_hint[400], ws_full[300];
  const char *ws_status;
  SvWidescreenCaps caps;
  int k;
  /* the picture and the window first, the CRT effect and its strengths after (a section of their own on the full page) */
  add_toggle(p, I_FULLSCREEN, "Fullscreen", v->fullscreen, "Borderless desktop fullscreen; no display mode change. F11 also toggles.");
  add_choice(p, I_SCALE, "Scaling", (int)v->scale, 2, scale, "Fit uses any size; Integer keeps whole-number pixel multiples when they fit.");
  add_choice(p, I_ASPECT, "Aspect Ratio", (int)v->aspect, 2, aspect, "Original keeps square pixels; 4:3 stretches to the aspect of a TV picture.");
  /* Widescreen Mode: the description of the selected mode and the honest requested-vs-effective status live only here, never on the game picture */
  sv_host_widescreen_caps(h, &caps);
  ws_status = sv_widescreen_status(v->widescreen, &caps);
  /* Level 3 falling back to a Partial Level 2 must still carry the platform's limitation wording (SEG-120: Master System repeats scenery) */
  if (v->widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY && sv_widescreen_effective(v->widescreen, &caps) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS &&
      caps.extended == SV_WS_PARTIAL && caps.partial_note != NULL && caps.partial_note[0] != '\0') {
    snprintf(ws_full, sizeof ws_full, "%s %s", ws_status, caps.partial_note);
    ws_status = ws_full;
  }
  /* the honest support status first: the footer shows two lines and cuts the rest, and the status must never be the part that is cut */
  snprintf(ws_hint, sizeof ws_hint, "%s%s%s", ws_status, ws_status[0] != '\0' ? " " : "", sv_widescreen_description(v->widescreen));
  add_choice(p, I_WIDESCREEN, "Widescreen Mode", (int)v->widescreen, SV_WIDESCREEN_COUNT, wsname, ws_hint);
  if (full && ws_status[0] != '\0') add_note(p, ws_status);
  if (full) {
    add_toggle(p, I_VSYNC, "VSync", v->vsync, "Syncs presentation to the display. Game speed is paced by the console clock either way.");
    add_toggle(p, I_FPS, "Show FPS Counter", v->show_fps, "Shows the presentation rate on the game screen.");
    item = sv_page_add(p, SV_ITEM_LABEL, 0, "CRT effect");
    if (item != NULL) sv_item_set_icon(item, "section");
  }
  add_choice(p, I_PRESET, "Video Effect", (int)v->preset, v->preset == SV_CRT_CUSTOM ? 5 : 4, preset, "Pixel Perfect is the untouched picture. CRT presets add real scanline/mask/glow/curve passes.");
  if (full) {
    add_slider(p, I_SCANLINES, "Scanlines", v->scanlines, 0, 100, 5, "%", "Darkens the gap between source lines (needs a window at least 1.5x the picture).");
    add_slider(p, I_MASK, "Phosphor mask", v->mask, 0, 100, 5, "%", "RGB stripe mask over the picture.");
    add_slider(p, I_CURVE, "Curvature", v->curvature, 0, 100, 5, "%", "Barrel distortion of the picture with corner falloff.");
    add_slider(p, I_BLOOM, "Bloom", v->bloom, 0, 100, 5, "%", "Soft glow around bright pixels.");
    add_slider(p, I_BRIGHT, "Brightness", v->brightness, 50, 150, 5, "%", "Picture brightness; values above 100 brighten to compensate for scanlines.");
    item = sv_page_add(p, SV_ITEM_BUTTON, I_VIDEO_DEFAULTS, "Restore video defaults");
    if (item != NULL) sv_item_set_icon(item, "reset");
    return;
  }
  /* Quick Settings: the design board's icon rows */
  add_toggle(p, I_VSYNC, "VSync", v->vsync, "Syncs presentation to the display. Game speed is paced by the console clock either way.");
  add_toggle(p, I_FPS, "Show FPS Counter", v->show_fps, "Shows the presentation rate on the game screen.");
  for (k = 0; k < p->count; ++k) {
    const int id = p->items[k].id;
    sv_item_set_icon(&p->items[k], id == I_FULLSCREEN ? "fullscreen" : (id == I_SCALE || id == I_ASPECT || id == I_WIDESCREEN ? "video" : (id == I_PRESET ? "settings" : (id == I_VSYNC ? "reset" : "info"))));
  }
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
  } else if (slot >= 0) {
    /* the device and what has been PROVEN about it (never "connected" alone) */
    SvDiagContext ctx;
    char txt[56];
    const SvDiagDevice *dev = NULL;
    int k;
    for (k = 0; k < sv_diag_count(h->diag); ++k) if (sv_diag_device(h->diag, k)->id == h->pads[slot].id) dev = sv_diag_device(h->diag, k);
    ctx.settings = &h->settings; ctx.system = h->cfg.system; ctx.player = player; ctx.nav_checked = 0;
    snprintf(txt, sizeof txt, "%.34s - %s", h->pads[slot].name, dev != NULL && sv_diag_state_of(h->diag, dev, &ctx) == SV_PAD_LIVE ? "input OK" : "press a button");
    sv_item_set_text(item, txt);
  } else if (h->pad_count > 0) {
    sv_item_set_text(item, "Keyboard (no spare pad)");
  } else {
    char txt[48];
    const SvPadState st = sv_diag_overall(h->diag, NULL);
    if (sv_diag_count(h->diag) == 0) snprintf(txt, sizeof txt, "Keyboard (no controller seen)");
    else snprintf(txt, sizeof txt, "Keyboard (controller: state %s)", sv_pad_state_code(st));
    sv_item_set_text(item, txt);
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
    sv_item_set_hint(item, a == SV_ACT_PAUSE ? "The console's own Pause button (a game input, never the menu). Left/Right: keyboard or gamepad column; Enter: press the new input."
                                              : "Left/Right: keyboard or gamepad column. Enter: press the new input. Backspace while listening clears it.");
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
  {
    char s2[1536];
    if (sv_diag_switch2_message(h->diag, &ctx, h->hid, h->hid_count > 0 ? h->hid_count : 0, s2, sizeof s2) != SV_S2_NOT_SEEN) {
      char *line = s2, *end;
      add_section(p, "Nintendo Switch 2 controller");
      while (*line != '\0') { /* one note per line: a note holds 299 characters */
        end = strchr(line, '\n');
        if (end != NULL) *end = '\0';
        add_note(p, line);
        if (end == NULL) break;
        line = end + 1;
      }
    }
  }
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
  if (item != NULL && h->hid_thread != NULL) sv_item_disable(item, "Scanning in the background; menus remain responsive.");
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
  item = sv_page_add(p, SV_ITEM_BUTTON, I_AUDIO_DEFAULTS, "Restore audio defaults");
  if (item != NULL) sv_item_set_icon(item, "reset");
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
  /* left column: the actions (all real); right column: the information */
  item = sv_page_add(p, SV_ITEM_BUTTON, I_SHOT, "Take Screenshot");
  if (item != NULL) { sv_item_set_icon(item, "screenshot"); sv_item_set_hint(item, h->last_screenshot[0] != '\0' ? h->last_screenshot : "Saves the game picture (without menus or effects) as an image."); }
  item = sv_page_add(p, SV_ITEM_BUTTON, I_RESET, "Reset Game");
  if (item != NULL) sv_item_set_icon(item, "reset");
  if (item != NULL && !h->cfg.can_reset) sv_item_disable(item, "Not available: this console's viewer has no proven reset seam.");
  else if (item != NULL) sv_item_set_hint(item, "Restarts the machine from power-on (the recorded input restarts too).");
  item = sv_page_add(p, SV_ITEM_BUTTON, I_GO_PADSTATUS, "Controller status & test");
  if (item != NULL) { sv_item_set_icon(item, "controller"); sv_item_set_hint(item, "What SDL sees (also controllers it cannot use), live buttons, and the exact reason a pad does not work."); }
  item = sv_page_add(p, SV_ITEM_TOGGLE, I_FPS, "Show FPS / status box");
  if (item != NULL) { item->value = h->settings.video.show_fps != 0; sv_item_set_text(item, item->value ? "On" : "Off"); sv_item_set_icon(item, "info"); sv_item_set_hint(item, "A small box on the game screen: system, frame rate, aspect and effect."); }
#define ROW(label, ...) do { item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, label); if (item != NULL) { item->column = 1; snprintf(buf, sizeof buf, __VA_ARGS__); sv_item_set_text(item, buf); } } while (0)
#define SECTION(title) do { item = sv_page_add(p, SV_ITEM_LABEL, 0, title); if (item != NULL) { item->column = 1; sv_item_set_icon(item, "section"); } } while (0)
  SECTION("Game Information");
  ROW("System", "%s", sv_system_name(h->cfg.system));
  if (h->cfg.rom_sha256 != NULL && strlen(h->cfg.rom_sha256) == 64u) {
    char half[40];
    snprintf(half, sizeof half, "%.16s", h->cfg.rom_sha256);
    item = sv_page_add(p, SV_ITEM_LABEL, I_INFO, "ROM SHA-256"); if (item != NULL) { item->column = 1; sv_item_set_text(item, half); }
  }
  ROW("Session Time", "%02u:%02u:%02u", secs / 3600u, (secs / 60u) % 60u, secs % 60u);
  SECTION("Runtime Information");
  ROW("Renderer", "%s", sv_host_renderer_name(h));
  ROW("Picture / Output", "%dx%d -> %dx%d", h->last_w, h->last_h, dest.w, dest.h);
  ROW("Frame Rate", "%.1f FPS (%.1f ms)", h->fps, h->frame_ms);
  if (d.audio_active) ROW("Audio Queue", "%u ms, %llu dropped", (unsigned)d.audio_queued_ms, (unsigned long long)d.audio_dropped);
  else ROW("Audio", "no device (silent)");
  {
    const SvPadState st = sv_diag_overall(h->diag, NULL);
    ROW("Controllers", "%d usable, state %s", h->pad_count, sv_pad_state_code(st));
  }
#undef SECTION
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
      item = sv_page_add(p, SV_ITEM_BUTTON, I_RESUME, "Resume Game");
      sv_item_set_icon(item, "play");
      sv_item_set_hint(item, "Esc or a gamepad's Back/Select (Nintendo -) opens this menu. Start is a game button.");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_QUICK, "Quick Settings"), "settings");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_VIDEO, "Video & Display"), "video");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_CONTROLS, "Controls"), "controller");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_AUDIO, "Audio"), "audio");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_BUTTON, I_GO_SYSTEM, "System"), "info");
      sv_item_set_icon(sv_page_add(p, SV_ITEM_LABEL, 0, ""), "divider");  /* leaving the game is set apart from the settings pages */
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
      case I_GO_VIDEO: open_page(h, PAGE_VIDEO, "VIDEO & DISPLAY"); return;
      case I_GO_CONTROLS: open_page(h, PAGE_CONTROLS, "CONTROLS"); return;
      case I_GO_PADSTATUS: open_page(h, PAGE_PADSTATUS, "CONTROLLER STATUS"); return;
      case I_PAD_RESCAN: sv_diag_scan(h->diag); sv_host_toast(h, "Rescanned controllers"); return;
      case I_PAD_HID: sv_host_start_hid_scan(h); return;
      case I_GO_AUDIO: open_page(h, PAGE_AUDIO, "AUDIO"); return;
      case I_AUDIO_DEFAULTS:
        h->settings.audio.volume = 100; h->settings.audio.muted = 0;
        sv_host_apply_audio(h); sv_host_mark_dirty(h); sv_host_toast(h, "Audio settings restored"); return;
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
    case I_WIDESCREEN: /* a presentation preference only: nothing here touches the guest, the run loop or the recording */
      v->widescreen = (SvWidescreenMode)ev.value;
      sv_settings_sanitize(&h->settings);
      if (v->widescreen != sv_host_widescreen_effective(h) && h->widescreen_fallbacks != UINT32_MAX) h->widescreen_fallbacks++;
      sv_host_mark_dirty(h);
      sv_pages_rebuild(h); /* refresh the explanation and the support status for the new selection */
      return;
    case I_PRESET: sv_apply_crt_preset(v, (SvCrtPreset)ev.value); sv_host_toast(h, sv_crt_preset_name(v->preset)); break;
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
