/* Drawing of the pause overlay (the design boards' branded pages): stepped blue frames, a real bitmap font whose widths come from glyph metrics,
 * aligned controls sized from the available pixels (never from a fixed character cell), focus states, and the bottom navigation cues.
 * Everything is drawn from SvPage data; nothing here changes a setting.
 *
 * One measured system for every page (all values in whole UI units `u`, so the overlay stays crisp at any integer display scale):
 *   - three page classes: HERO (pause menu, confirmations: few large rows, labels at the larger "label" text scale), STANDARD (Quick Settings, Audio)
 *     and DENSE (Video, Controls, System, Controller status: body text, more rows, wider two-column panels);
 *   - one row rhythm per class, one header (icon + wordmark + page title), one footer (focused item's explanation + input cues);
 *   - focus is always the same filled blue row with a light edge; destructive confirmations use the danger red, never the default focus. */
#include <stdio.h>
#include <string.h>

#include "sv_internal.h"

static const SDL_Color K_TEXT = {220, 232, 255, 255}, K_TEXT2 = {169, 183, 221, 255}, K_DIM = {120, 133, 170, 255}, K_CYAN = {64, 191, 245, 255}, K_WHITE = {244, 247, 255, 255},
                       K_FOCUS = {140, 224, 255, 255}, K_ERR = {255, 129, 119, 255}, K_WARN = {255, 197, 109, 255},
                       K_PANEL = {6, 16, 40, 244}, K_EDGE = {35, 62, 120, 255}, K_LIGHT = {77, 103, 167, 255}, K_ROW = {10, 24, 56, 255}, K_ROW_EDGE = {28, 56, 112, 255},
                       K_SEL = {31, 98, 214, 255}, K_SEL_EDGE = {140, 224, 255, 255}, K_SEL_LIGHT = {110, 190, 255, 255}, K_SEL_SOFT = {16, 46, 108, 255}, K_FIELD = {5, 12, 30, 255},
                       K_ON = {52, 140, 245, 255}, K_OFF = {22, 38, 76, 255}, K_BLUE = {21, 87, 201, 255}, K_RULE = {34, 70, 140, 255}, K_BANNER = {64, 50, 14, 255},
                       K_HEADER = {4, 11, 30, 255}, K_DANGER = {176, 44, 58, 255}, K_DANGER_EDGE = {255, 150, 140, 255}, K_DANGER_SOFT = {58, 16, 26, 255},
                       K_OUTLINE = {4, 10, 26, 255};

/* ---- the page's measured style (set once per draw) ---- */

typedef enum PageClass { CLASS_HERO, CLASS_STANDARD, CLASS_DENSE } PageClass;

static int s_ls = 1;          /* label text scale (HERO pages: larger than the body scale u) */
static int s_rh = 26;         /* row height in pixels */
static int s_first_bind = -1; /* index of the first binding row (it carries the Keyboard / Gamepad column header) */

static PageClass page_class(int page_id) {
  switch (page_id) {
    case PAGE_MAIN: case PAGE_CONFIRM_QUIT: case PAGE_CONFIRM_RESET: case PAGE_CONFIRM_VIDEO: case PAGE_CONFIRM_CONTROLS: return CLASS_HERO;
    case PAGE_QUICK: case PAGE_AUDIO: return CLASS_STANDARD;
    default: return CLASS_DENSE;
  }
}

/* Preferred and minimum panel widths in units (the design boards' proportions at 1080x740: pause ~500, settings pages 700-840). */
static void page_widths(int page_id, int *want, int *least) {
  switch (page_id) {
    case PAGE_MAIN: *want = 500; *least = 330; break;
    case PAGE_QUICK: *want = 460; *least = 330; break;
    case PAGE_VIDEO: *want = 840; *least = 460; break;
    case PAGE_CONTROLS: *want = 700; *least = 460; break;
    case PAGE_AUDIO: *want = 600; *least = 400; break;
    case PAGE_SYSTEM: *want = 780; *least = 460; break;
    case PAGE_PADSTATUS: *want = 720; *least = 460; break;
    default: *want = 500; *least = 330; break; /* confirmations */
  }
}

static int header_h(int u);
static int footer_h(int u, int out_h, int lh);

static int label_scale_for(int u) { return (3 * u + 1) / 2; } /* 1 -> 2, 2 -> 3, 3 -> 5, 4 -> 6: the hero labels grow in whole steps with the display */

/* ---- small shapes ---- */

static void text_bold(SvHost *h, int s, int x, int y, SDL_Color c, const char *text) {
  sv_ui_text(h, SV_FONT_BODY, s, x, y, c, text);
  sv_ui_text(h, SV_FONT_BODY, s, x + (s > 1 ? s / 2 : 1), y, c, text);
}

/* A pixel chevron: seven rows, widest in the middle. dir: -1 left, 1 right, -2 up, 2 down. */
static void chevron(SvHost *h, int x, int y, int dir, int u, SDL_Color c) {
  static const int widths[7] = {1, 2, 3, 4, 3, 2, 1};
  int k;
  for (k = 0; k < 7; ++k) {
    const int w = widths[k] * u;
    if (dir == -1 || dir == 1) sv_host_fill(h, dir < 0 ? x + 4 * u - w : x, y + k * u, w, u, c);
    else sv_host_fill(h, x + k * u, dir < 0 ? y + 4 * u - w : y, u, w, c);
  }
}

static void rule(SvHost *h, int x, int y, int w, int u) { sv_host_fill(h, x, y, w, u, K_RULE); }

static int ellipsized(const char *src, int s, int max_px, char *out, int cap, SvFontId face) { return sv_font_ellipsize(face, src, s, max_px < 8 ? 8 : max_px, out, cap); }

static void text_in(SvHost *h, SvFontId face, int s, int x, int y, int max_px, SDL_Color c, const char *text) {
  char buf[160];
  ellipsized(text, s, max_px, buf, (int)sizeof buf, face);
  sv_ui_text(h, face, s, x, y, c, buf);
}

static void text_right(SvHost *h, int s, int right, int y, int max_px, SDL_Color c, const char *text) {
  char buf[160];
  const int w = ellipsized(text, s, max_px, buf, (int)sizeof buf, SV_FONT_BODY);
  sv_ui_text(h, SV_FONT_BODY, s, right - w, y, c, buf);
}

/* Top of a body-font line of scale `s` vertically centred in a box of height `box_h` starting at `y` (the glyphs' visible band, not the leading). */
static int text_top(int y, int box_h, int s) { return y + (box_h - 13 * s) / 2 - s; }

static int nintendo_pad_present(const SvHost *h) {
  int i;
  for (i = 0; i < sv_diag_count(h->diag); ++i) {
    const SvDiagDevice *d = sv_diag_device(h->diag, i);
    if (d != NULL && (d->vendor == 0x057eU || d->gamepad_type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)) return 1;
  }
  return 0;
}

/* ---- navigation cues: [glyph] label, right-aligned; keyboard keycaps or gamepad face-button discs ---- */

static int draw_cue(SvHost *h, int right, int cy, int u, int pad_mode, const char *glyph, SDL_Color disc, const char *label) {
  const int lw = sv_ui_text_w(SV_FONT_BODY, u, label), d = 17 * u, lty = cy - 7 * u;
  int x, gw;
  if (pad_mode) {
    gw = d;
    x = right - lw - 6 * u - gw;
    sv_ui_chamfer(h, x, cy - d / 2, d, d, 5 * u, disc);
    sv_ui_text(h, SV_FONT_BODY, u, x + (d - sv_ui_text_w(SV_FONT_BODY, u, glyph)) / 2 + u / 2, lty, K_WHITE, glyph);
  } else {
    gw = sv_ui_text_w(SV_FONT_BODY, u, glyph) + 12 * u;
    x = right - lw - 6 * u - gw;
    sv_ui_frame(h, x, cy - d / 2, gw, d, u, (SDL_Color){22, 40, 84, 255}, K_ROW_EDGE, K_LIGHT, 0);
    sv_ui_text(h, SV_FONT_BODY, u, x + 6 * u, lty, K_TEXT, glyph);
  }
  sv_ui_text(h, SV_FONT_BODY, u, right - lw, lty, K_TEXT2, label);
  return x;
}

static int draw_cues(SvHost *h, int right, int cy, int u, int back_label) {
  const int pad_mode = h->last_input_pad, nin = pad_mode && nintendo_pad_present(h);
  const SDL_Color green = {48, 176, 120, 255}, red = {214, 70, 78, 255};
  int x = draw_cue(h, right, cy, u, pad_mode, pad_mode ? (nin ? "A" : "B") : "Esc", red, back_label ? "Back" : "Close");
  return draw_cue(h, x - 16 * u, cy, u, pad_mode, pad_mode ? (nin ? "B" : "A") : "Enter", green, "Select");
}

/* ---- items ---- */

static int is_note(const SvItem *it) { return it->kind == SV_ITEM_LABEL && strcmp(it->icon, "note") == 0; }
static int is_section(const SvItem *it) { return it->kind == SV_ITEM_LABEL && strcmp(it->icon, "section") == 0; }
static int is_divider(const SvItem *it) { return it->kind == SV_ITEM_LABEL && strcmp(it->icon, "divider") == 0; }
static int is_hero_row(const SvItem *it) { return it->id == I_RESUME || it->id == I_VOLUME; }

static int column_header_h(int u) { return 20 * u; }

static int item_height(const SvItem *it, int idx, int u, int inner_w) {
  if (is_section(it)) return 26 * u;
  if (is_divider(it)) return 14 * u;
  if (is_note(it)) {
    int start[8], len[8], truncated, n = sv_font_wrap(SV_FONT_BODY, it->hint, u, inner_w - 44 * u, 8, start, len, &truncated);
    if (n < 1) n = 1;
    return n * sv_ui_line_h(SV_FONT_BODY, u) + 20 * u;
  }
  if (it->id == I_VOLUME) return s_rh + 14 * u;
  if (it->id == I_PLAYER) return s_rh + 6 * u;
  if (idx == s_first_bind) return s_rh + column_header_h(u);
  return s_rh;
}

static void row_background(SvHost *h, int x, int y, int w, int rh, int u, int focused, int always, int enabled, int hero, int danger) {
  if (focused && enabled) {
    sv_ui_chamfer(h, x, y, w, rh - 2 * u, 2 * u, K_OUTLINE);
    sv_ui_chamfer(h, x + u, y + u, w - 2 * u, rh - 4 * u, 2 * u, danger ? K_DANGER_EDGE : K_SEL_EDGE);
    sv_ui_chamfer(h, x + 2 * u, y + 2 * u, w - 4 * u, rh - 6 * u, u, danger ? K_DANGER : K_SEL);
    sv_host_fill(h, x + 4 * u, y + 2 * u, w - 8 * u, u, danger ? (SDL_Color){230, 110, 110, 255} : K_SEL_LIGHT);
  } else if (hero && enabled) { /* the primary action stays visibly first even without focus: a blue frame */
    sv_ui_chamfer(h, x, y, w, rh - 2 * u, 2 * u, K_BLUE);
    sv_ui_chamfer(h, x + u, y + u, w - 2 * u, rh - 4 * u, u, K_SEL_SOFT);
  } else if (always) {
    sv_ui_chamfer(h, x, y, w, rh - 2 * u, 2 * u, danger ? (SDL_Color){110, 44, 56, 255} : K_ROW_EDGE);
    sv_ui_chamfer(h, x + u, y + u, w - 2 * u, rh - 4 * u, u, danger ? K_DANGER_SOFT : K_ROW);
  }
}

static void switch_widget(SvHost *h, int x, int cy, int u, int on) {
  const int w = 36 * u, hh = 18 * u, y = cy - hh / 2;
  sv_ui_chamfer(h, x, y, w, hh, 4 * u, K_OUTLINE);
  sv_ui_chamfer(h, x + u, y + u, w - 2 * u, hh - 2 * u, 3 * u, on ? K_ON : K_OFF);
  if (on) sv_host_fill(h, x + 4 * u, y + u, w - 8 * u, u, K_SEL_LIGHT);
  {
    const int kx = on ? x + w - hh + u : x + u;
    sv_ui_chamfer(h, kx + u, y + 2 * u, hh - 4 * u, hh - 4 * u, 3 * u, on ? K_WHITE : (SDL_Color){150, 168, 205, 255});
  }
}

/* The glyph of a console input in the Controls table: a pixel arrow for a direction, a disc with the button's letter, a pill for Start/Pause. */
static void action_glyph(SvHost *h, int x, int cy, int u, const char *label, SDL_Color c) {
  const int up = strcmp(label, "Up") == 0, down = strcmp(label, "Down") == 0, left = strcmp(label, "Left") == 0, right = strcmp(label, "Right") == 0;
  if (up || down || left || right) {
    sv_ui_chamfer(h, x, cy - 8 * u, 16 * u, 16 * u, 3 * u, K_FIELD);
    if (left || right) chevron(h, x + 6 * u, cy - 3 * u - u / 2 - 1, left ? -1 : 1, u, c);
    else chevron(h, x + 4 * u + u / 2, cy - 2 * u, up ? -2 : 2, u, c);
  } else if (strcmp(label, "Start") == 0 || strcmp(label, "Pause") == 0 || strcmp(label, "Menu key") == 0) {
    sv_ui_chamfer(h, x - u, cy - 5 * u, 18 * u, 10 * u, 4 * u, K_FIELD);
    sv_host_fill(h, x + 4 * u, cy - u, 8 * u, 2 * u, c);
  } else {
    char g[3] = {0, 0, 0};
    g[0] = label[0];
    if (strncmp(label, "Button ", 7) == 0) g[0] = label[7];
    sv_ui_chamfer(h, x, cy - 8 * u, 16 * u, 16 * u, 6 * u, K_FIELD);
    sv_ui_text(h, SV_FONT_BODY, u, x + (16 * u - sv_ui_text_w(SV_FONT_BODY, u, g)) / 2 + u / 2, cy - 7 * u, c, g);
  }
}

static void draw_item(SvHost *h, const SvPage *page, int idx, int x, int y, int w, int u, int *rect_out) {
  const SvItem *it = &page->items[idx];
  const int rh_total = item_height(it, idx, u, w), focused = idx == page->focus;
  const int lh = sv_ui_line_h(SV_FONT_BODY, u);
  const int enabled = it->enabled, danger = it->id == I_YES && page->page_id >= PAGE_CONFIRM_QUIT;
  const SDL_Color base = enabled ? K_TEXT : K_DIM, tc = focused && enabled ? K_WHITE : base;
  int row_y = y, rh = rh_total;
  const int ls = (it->kind == SV_ITEM_BUTTON || it->kind == SV_ITEM_TOGGLE || it->kind == SV_ITEM_CHOICE || it->kind == SV_ITEM_SLIDER) ? s_ls : u;
  rect_out[0] = x; rect_out[1] = y; rect_out[2] = w; rect_out[3] = rh_total;
  if (is_divider(it)) {
    sv_host_fill(h, x + 4 * u, y + 6 * u, w - 8 * u, u, K_RULE);
    return;
  }
  if (is_section(it)) {
    sv_ui_text(h, SV_FONT_BODY, u, x + 2 * u, y + 7 * u, K_FOCUS, it->label);
    sv_host_fill(h, x, y + 22 * u, w, u, K_RULE);
    return;
  }
  if (is_note(it)) {
    int start[8], len[8], truncated, n, i;
    char line[200];
    n = sv_font_wrap(SV_FONT_BODY, it->hint, u, w - 44 * u, 8, start, len, &truncated);
    sv_ui_chamfer(h, x, y + 2 * u, w, rh_total - 4 * u, 2 * u, K_ROW_EDGE);
    sv_ui_chamfer(h, x + u, y + 3 * u, w - 2 * u, rh_total - 6 * u, u, (SDL_Color){10, 26, 60, 255});
    sv_host_icon_draw(h, "info", (float)(x + 12 * u), (float)(y + 10 * u), (float)(16 * u), K_FOCUS);
    for (i = 0; i < n; ++i) {
      snprintf(line, sizeof line, "%.*s", len[i] < 190 ? len[i] : 190, it->hint + start[i]);
      sv_ui_text(h, SV_FONT_BODY, u, x + 38 * u, y + 10 * u + i * lh, K_TEXT2, line);
    }
    return;
  }
  if (idx == s_first_bind) { /* the binding table's column header, above its first row */
    const int vx = x + w * 40 / 100, vw = x + w - 10 * u - vx, cell_w = (vw - 8 * u) / 2;
    sv_host_icon_draw(h, "controller", (float)(vx + cell_w + 8 * u + 4 * u), (float)(y + 2 * u), (float)(16 * u), K_DIM);
    sv_ui_text(h, SV_FONT_BODY, u, vx + 4 * u, y + 3 * u, K_FOCUS, "Keyboard");
    sv_ui_text(h, SV_FONT_BODY, u, vx + cell_w + 8 * u + 24 * u, y + 3 * u, K_FOCUS, "Gamepad");
    row_y = y + column_header_h(u);
    rh = rh_total - column_header_h(u);
  }
  if (it->id == I_PLAYER) { /* Controls: Player 1 / Player 2 as two large tabs across the page */
    int t;
    const int tw = (w - 8 * u) / 2;
    for (t = 0; t < 2; ++t) {
      const int tx = x + t * (tw + 8 * u), on = t == it->value;
      const char *name = t == 0 ? "Player 1" : "Player 2";
      sv_ui_chamfer(h, tx, row_y, tw, rh - 6 * u, 2 * u, on ? (focused ? K_SEL_EDGE : K_BLUE) : K_ROW_EDGE);
      sv_ui_chamfer(h, tx + u, row_y + u, tw - 2 * u, rh - 8 * u, u, on ? K_SEL : K_FIELD);
      if (on) sv_host_fill(h, tx + 3 * u, row_y + u, tw - 6 * u, u, K_SEL_LIGHT);
      sv_ui_text(h, SV_FONT_BODY, u, tx + (tw - sv_ui_text_w(SV_FONT_BODY, u, name)) / 2, text_top(row_y, rh - 6 * u, u), on ? K_WHITE : K_TEXT2, name);
    }
    return;
  }
  row_background(h, x, row_y, w, rh, u, focused, it->kind == SV_ITEM_BUTTON, enabled, it->id == I_RESUME, danger);
  {
    const int ty = text_top(row_y, rh - 2 * u, ls);
    int lx = x + 12 * u, label_w;
    const SDL_Color ic = focused && enabled ? K_WHITE : (enabled ? (it->id == I_QUIT ? K_ERR : K_TEXT2) : K_DIM);
    if (it->kind == SV_ITEM_BINDING) {
      action_glyph(h, lx, row_y + (rh - 2 * u) / 2, u, it->label, focused ? K_WHITE : K_FOCUS);
      lx += 26 * u;
    } else if (it->icon[0] != '\0') {
      const int isz = 16 * (ls > u ? ls : u);
      sv_host_icon_draw(h, it->icon, (float)lx, (float)(row_y + (rh - 2 * u - isz) / 2), (float)isz, ic);
      lx += isz + 10 * u;
    }
    label_w = (it->kind == SV_ITEM_BUTTON ? w - 8 * u : (it->kind == SV_ITEM_TOGGLE ? w - 100 * u : (it->kind == SV_ITEM_BINDING ? w * 40 / 100 : w * 45 / 100))) - (lx - x) - 8 * u;
    if (is_hero_row(it) && enabled) {
      char buf[160];
      ellipsized(it->label, ls, label_w, buf, (int)sizeof buf, SV_FONT_BODY);
      text_bold(h, ls, lx, ty, focused ? K_WHITE : (it->id == I_RESUME ? K_WHITE : K_TEXT), buf);
    } else {
      text_in(h, SV_FONT_BODY, ls, lx, ty, label_w, it->kind == SV_ITEM_LABEL ? K_TEXT2 : tc, it->label);
    }
  }
  {
    const int vx = x + (it->kind == SV_ITEM_BINDING ? w * 40 / 100 : w * 45 / 100), right = x + w - 12 * u, vw = right - vx;
    const int ty = text_top(row_y, rh - 2 * u, u), cy = row_y + (rh - 2 * u) / 2;
    switch (it->kind) {
      case SV_ITEM_TOGGLE: {
        const int sw = 36 * u;
        switch_widget(h, right - sw, cy, u, it->value);
        text_right(h, u, right - sw - 10 * u, ty, vw, focused && enabled ? K_WHITE : (it->value ? K_TEXT : K_DIM), it->value ? "On" : "Off");
        break;
      }
      case SV_ITEM_CHOICE: {
        char buf[160];
        const int ch = 7 * u, ccy = cy - ch / 2;
        const int inner_l = vx + 6 * u, inner_r = right - 4 * u;
        const SDL_Color arrow = focused && enabled ? K_WHITE : K_DIM;
        const int w2 = ellipsized(it->text, u, vw - 40 * u, buf, (int)sizeof buf, SV_FONT_BODY);
        if (focused && enabled) sv_ui_chamfer(h, inner_l - 4 * u, cy - 10 * u, inner_r - inner_l + 12 * u, 20 * u, 2 * u, (SDL_Color){18, 70, 170, 255});
        chevron(h, inner_l, ccy, -1, u, arrow);
        chevron(h, inner_r - 4 * u, ccy, 1, u, arrow);
        sv_ui_text(h, SV_FONT_BODY, u, (inner_l + inner_r) / 2 - w2 / 2, ty, tc, buf);
        break;
      }
      case SV_ITEM_SLIDER: {
        const int big = it->id == I_VOLUME;
        const int val_w = 7 * 6 * u, tx = vx, tw = vw - val_w - 10 * u, th = big ? 8 * u : 6 * u;
        const int span = it->max - it->min > 0 ? it->max - it->min : 1, fill_w = (it->value - it->min) * (tw - 2 * u) / span;
        const int kw = big ? 10 * u : 8 * u, kh = big ? 20 * u : 14 * u;
        sv_ui_chamfer(h, tx, cy - th / 2, tw, th, 2 * u, K_OUTLINE);
        sv_ui_chamfer(h, tx + u, cy - th / 2 + u, tw - 2 * u, th - 2 * u, u, K_OFF);
        if (fill_w > 0) sv_ui_chamfer(h, tx + u, cy - th / 2 + u, fill_w, th - 2 * u, u, K_ON);
        {
          int kx = tx + u + fill_w - kw / 2;
          if (kx < tx) kx = tx;
          sv_ui_chamfer(h, kx, cy - kh / 2, kw, kh, 2 * u, K_OUTLINE);
          sv_ui_chamfer(h, kx + u, cy - kh / 2 + u, kw - 2 * u, kh - 2 * u, u, focused && enabled ? K_WHITE : K_TEXT);
        }
        if (big) text_bold(h, u, right - sv_ui_text_w(SV_FONT_BODY, u, it->text) - u, ty, tc, it->text);
        else text_right(h, u, right, ty, val_w, tc, it->text);
        break;
      }
      case SV_ITEM_LABEL:
        text_right(h, u, right, ty, w * 62 / 100, K_TEXT, it->text);
        break;
      case SV_ITEM_BINDING: {
        const int cell_w = (vw - 8 * u) / 2;
        int t;
        for (t = 0; t < 2; ++t) {
          const int cx = vx + t * (cell_w + 8 * u), on = focused && it->value == t;
          const char *txt = t == 0 ? it->text : it->text2;
          char buf[96];
          /* binding capture: the cell being edited says so (and the footer counts down) */
          const int capture = h->listen.kind != SV_LISTEN_NONE && ((it->id == I_MENUKEY && h->listen.kind == SV_LISTEN_MENU_KEY && t == 0) ||
                              (it->id == I_BIND + (int)h->listen.action && ((h->listen.kind == SV_LISTEN_KEY && t == 0) || (h->listen.kind == SV_LISTEN_PAD && t == 1))));
          const int unbound = strcmp(txt, "Unbound") == 0 || strcmp(txt, "No pad") == 0;
          int tw2;
          if (t == 1 && it->text2[0] == '\0') break;
          sv_ui_chamfer(h, cx, row_y + 3 * u, cell_w, rh - 8 * u, 2 * u, capture ? K_WARN : (on ? K_SEL_EDGE : K_ROW_EDGE));
          sv_ui_chamfer(h, cx + u, row_y + 4 * u, cell_w - 2 * u, rh - 10 * u, u, capture ? K_BANNER : (on ? K_SEL : K_FIELD));
          tw2 = ellipsized(capture ? "Press now..." : txt, u, cell_w - 14 * u, buf, (int)sizeof buf, SV_FONT_BODY);
          sv_ui_text(h, SV_FONT_BODY, u, cx + (cell_w - tw2) / 2, ty, capture ? K_WARN : (on ? K_WHITE : (unbound ? K_DIM : K_TEXT)), buf);
        }
        break;
      }
      case SV_ITEM_BUTTON: break;
    }
  }
}

/* ---- Video page: live preview, presets and the preset's description ---- */

static int preview_column_h(int u, int pic_h) { return sv_ui_line_h(SV_FONT_BODY, u) + 6 * u + pic_h + 10 * u + 2 * 30 * u + 8 * u + 4 * sv_ui_line_h(SV_FONT_BODY, u) + 4 * u; }

static void draw_preview_column(SvHost *h, int x, int y, int w, int pic_h, int body_h, int u) {
  static const char *const desc[] = {"The untouched picture: nearest-neighbour pixels, no filter.", "Subtle scanline gaps and a gentle glow.",
                                     "Scanlines, an RGB mask, glow and slight curvature.", "A stronger arcade-monitor look: heavy scanlines, mask and curve."};
  const int lh = sv_ui_line_h(SV_FONT_BODY, u);
  int py = y, p;
  sv_ui_text(h, SV_FONT_BODY, u, x, py, K_FOCUS, "Live preview");
  py += lh + 6 * u;
  sv_ui_chamfer(h, x, py, w, pic_h, 2 * u, K_SEL_EDGE);
  sv_ui_chamfer(h, x + u, py + u, w - 2 * u, pic_h - 2 * u, u, K_OUTLINE);
  sv_host_draw_preview(h, x + 3 * u, py + 3 * u, w - 6 * u, pic_h - 6 * u);
  {
    const char *tag = h->settings.video.preset == SV_CRT_PIXEL_PERFECT ? "centre crop, untouched" : "centre crop, effect applied";
    const int tw = sv_ui_text_w(SV_FONT_BODY, u, tag) + 10 * u;
    sv_host_fill(h, x + 3 * u, py + pic_h - lh - 7 * u, tw, lh + 4 * u, (SDL_Color){4, 8, 20, 200});
    sv_ui_text(h, SV_FONT_BODY, u, x + 8 * u, py + pic_h - lh - 6 * u, K_TEXT2, tag);
  }
  py += pic_h + 10 * u;
  for (p = 0; p < 4; ++p) {
    const int bw = (w - 8 * u) / 2, bx = x + (p % 2) * (bw + 8 * u), by = py + (p / 2) * (30 * u), bh = 26 * u;
    const int on = (int)h->settings.video.preset == p;
    char buf[48];
    const char *nm = sv_crt_preset_name((SvCrtPreset)p);
    const int tw = ellipsized(nm, u, bw - 30 * u, buf, (int)sizeof buf, SV_FONT_BODY);
    sv_ui_chamfer(h, bx, by, bw, bh, 2 * u, on ? K_SEL_EDGE : K_ROW_EDGE);
    sv_ui_chamfer(h, bx + u, by + u, bw - 2 * u, bh - 2 * u, u, on ? K_SEL : K_ROW);
    if (on) {
      sv_host_fill(h, bx + 3 * u, by + u, bw - 6 * u, u, K_SEL_LIGHT);
      sv_host_icon_draw(h, "check", (float)(bx + 7 * u), (float)(by + 5 * u), (float)(16 * u), K_WHITE);
    }
    sv_ui_text(h, SV_FONT_BODY, u, bx + (bw - tw) / 2 + (on ? 8 * u : 0), text_top(by, bh, u), on ? K_WHITE : K_TEXT2, buf);
    h->ui_preset_rect[p][0] = bx; h->ui_preset_rect[p][1] = by; h->ui_preset_rect[p][2] = bw; h->ui_preset_rect[p][3] = bh;
  }
  py += 2 * 30 * u + 8 * u;
  {
    const char *name = sv_crt_preset_name(h->settings.video.preset);
    const char *d = h->settings.video.preset == SV_CRT_CUSTOM ? "Custom: your own scanline, mask, curvature, bloom and brightness values." : desc[h->settings.video.preset];
    int start[6], len[6], truncated, n, i;
    char line[160];
    text_bold(h, u, x, py, K_WHITE, name);
    py += lh + 4 * u;
    n = sv_font_wrap(SV_FONT_BODY, d, u, w, 3, start, len, &truncated);
    for (i = 0; i < n && py + lh <= y + body_h; ++i) {
      snprintf(line, sizeof line, "%.*s", len[i] < 150 ? len[i] : 150, d + start[i]);
      sv_ui_text(h, SV_FONT_BODY, u, x, py, K_TEXT2, line);
      py += lh;
    }
  }
}

/* ---- header ---- */

static int header_h(int u) { return 44 * u; }

static void draw_header(SvHost *h, int x, int y, int w, int u, const char *title) {
  const int pad = 16 * u;
  int title_w, avail, wm_w = 0;
  sv_host_fill(h, x + 2 * u, y + 2 * u, w - 4 * u, header_h(u) - 4 * u, K_HEADER);
  sv_host_icon_draw(h, "controller", (float)(x + pad), (float)(y + 12 * u), (float)(20 * u), K_CYAN);
  {
    SDL_Texture *wm = sv_ui_wordmark(h);
    if (wm != NULL) {
      float tw = 0, th = 0;
      SDL_FRect d, src;
      SDL_GetTextureSize(wm, &tw, &th);
      /* the visible part of wordmark.png (its transparent margins are cropped away) */
      src.x = tw * 38.0f / 1200.0f; src.y = th * 74.0f / 400.0f; src.w = tw * 1125.0f / 1200.0f; src.h = th * 295.0f / 400.0f;
      d.h = (float)(22 * u);
      d.w = d.h * src.w / src.h;
      d.x = (float)(x + pad + 28 * u);
      d.y = (float)(y + 11 * u);
      SDL_RenderTexture(h->renderer, wm, &src, &d);
      wm_w = (int)d.w;
    } else {
      wm_w = sv_ui_text_w(SV_FONT_HEAD, 2 * u, "SEGARECOMP");
      sv_ui_text_shadow(h, SV_FONT_HEAD, 2 * u, x + pad + 28 * u, y + 14 * u, K_FOCUS, "SEGARECOMP");
    }
  }
  avail = w - 2 * pad - 28 * u - wm_w - 16 * u;
  title_w = sv_ui_text_w(SV_FONT_HEAD, 2 * u, title);
  if (title_w <= avail) {
    sv_ui_text(h, SV_FONT_HEAD, 2 * u, x + w - pad - title_w, y + 15 * u, K_TEXT2, title);
  } else {  /* a long title falls back to the small heading size, then is cut */
    char buf[64];
    const int w2 = sv_font_ellipsize(SV_FONT_HEAD, title, u, avail, buf, (int)sizeof buf);
    sv_ui_text(h, SV_FONT_HEAD, u, x + w - pad - w2, y + 18 * u, K_TEXT2, buf);
  }
  sv_host_fill(h, x + 2 * u, y + header_h(u) - 2 * u, w - 4 * u, u, K_RULE);
}

/* ---- the page ---- */

static int footer_h(int u, int out_h, int lh) { return out_h < 360 * u ? lh + 34 * u : 2 * lh + 40 * u; }

/* Confirmations: warning icon and bold question, the consequence, then the two answers as full-width rows (No is the default focus). */
static void draw_confirm(SvHost *h, SvPage *page, int out_w, int out_h, int u) {
  const char *msg = page->page_id == PAGE_CONFIRM_QUIT ? (SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Any unsaved progress will be lost. Return to the launcher and close the game?" : "Any unsaved progress will be lost: there are no save states.") :
                    page->page_id == PAGE_CONFIRM_RESET ? "Restart the game from power-on? Progress since the last power-on is lost." : "Reset these settings to their defaults? Your other settings are kept.";
  int want, least, panel_w, panel_h, x, y, pad = 18 * u, n, k, i, ty, title_s, lh = sv_ui_line_h(SV_FONT_BODY, u);
  int start[6], len[6], truncated;
  char line[200];
  page_widths(page->page_id, &want, &least);
  panel_w = want * u > out_w - 24 * u ? out_w - 24 * u : want * u;
  title_s = s_ls;
  if (sv_ui_text_w(SV_FONT_BODY, title_s, page->title) > panel_w - 2 * pad - 40 * u) title_s = u;
  n = sv_font_wrap(SV_FONT_BODY, msg, u, panel_w - 2 * pad, 4, start, len, &truncated);
  panel_h = pad + sv_ui_line_h(SV_FONT_BODY, title_s) + 12 * u + n * lh + 16 * u + page->count * (s_rh + 6 * u) + 6 * u + 34 * u;
  if (panel_h > out_h - 12 * u) { s_rh = 30 * u; panel_h = pad + sv_ui_line_h(SV_FONT_BODY, title_s) + 12 * u + n * lh + 16 * u + page->count * (s_rh + 6 * u) + 6 * u + 34 * u; }
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  if (y < 6 * u) y = 6 * u;
  sv_ui_frame(h, x, y, panel_w, panel_h, u, K_PANEL, K_EDGE, K_LIGHT, 1);
  sv_host_fill(h, x + 2 * u, y + 2 * u, panel_w - 4 * u, 3 * u, K_WARN); /* a thin amber band: this is a decision, not a settings page */
  {
    const int isz = 16 * (title_s > u ? title_s : u) + (title_s > u ? 0 : 6 * u);
    const int tline = sv_ui_line_h(SV_FONT_BODY, title_s);
    sv_host_icon_draw(h, "warning", (float)(x + pad), (float)(y + pad + (tline - isz) / 2 + u), (float)isz, K_WARN);
    text_bold(h, title_s, x + pad + isz + 12 * u, y + pad, K_WHITE, page->title);
    ty = y + pad + tline + 12 * u;
  }
  for (k = 0; k < n; ++k) {
    snprintf(line, sizeof line, "%.*s", len[k] < 190 ? len[k] : 190, msg + start[k]);
    sv_ui_text(h, SV_FONT_BODY, u, x + pad, ty, K_TEXT2, line);
    ty += lh;
  }
  ty += 16 * u;
  for (i = 0; i < page->count; ++i) {
    int rect[4];
    draw_item(h, page, i, x + pad, ty, panel_w - 2 * pad, u, rect);
    if (h->ui_rows < SV_MENU_MAX_ITEMS) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = i; h->ui_rows++; }
    ty += s_rh + 6 * u;
  }
  draw_cues(h, x + panel_w - pad, y + panel_h - 18 * u, u, 1);
}

void sv_pages_draw(SvHost *h, int out_w, int out_h) {
  SvPage *page = sv_menu_top(&h->menu);
  const int preview = sv_pages_wants_preview(h);
  int u = sv_host_ui_scale(out_w, out_h);
  int want, least, panel_w, panel_h, x, y, pad, head_h, foot_h, body_w, body_y, body_h, i, k, drawn = 0, used = 0, avail_h, banner_h = 0, left_w, lh, gap;
  int two_col, video, system, pic_h = 0;
  PageClass cls;
  if (page == NULL) return;
  cls = page_class(page->page_id);
  video = page->page_id == PAGE_VIDEO;
  system = page->page_id == PAGE_SYSTEM;
  page_widths(page->page_id, &want, &least);
  while (u > 1 && least * u > out_w - 16 * u) --u;
  s_first_bind = -1;
  for (k = 0; k < page->count; ++k) if (page->items[k].kind == SV_ITEM_BINDING) { s_first_bind = k; break; }
  h->ui_rows = 0;
  memset(h->ui_preset_rect, 0, sizeof h->ui_preset_rect);
  /* text and row rhythm of the page class; the hero labels fall back to the body scale when they do not fit */
  s_ls = cls == CLASS_HERO ? label_scale_for(u) : u;
  for (;;) {
    s_rh = cls == CLASS_HERO ? sv_ui_line_h(SV_FONT_BODY, s_ls) + 18 * u : (cls == CLASS_STANDARD ? 34 * u : 28 * u);
    if (cls != CLASS_HERO || s_ls == u) break;
    {
      int need = 0;
      for (k = 0; k < page->count; ++k) need += page->page_id == PAGE_MAIN ? item_height(&page->items[k], k, u, want * u) : s_rh + 6 * u;
      if (need + header_h(u) + 32 * u + footer_h(u, out_h, sv_ui_line_h(SV_FONT_BODY, u)) + 12 * u <= out_h && sv_ui_text_w(SV_FONT_BODY, s_ls, "Return to Launcher") + 90 * u <= (want * u < out_w - 24 * u ? want * u : out_w - 24 * u)) break;
    }
    s_ls = u;
  }
  if (page->page_id >= PAGE_CONFIRM_QUIT) { draw_confirm(h, page, out_w, out_h, u); return; }
layout_retry:
  lh = sv_ui_line_h(SV_FONT_BODY, u);
  pad = 16 * u;
  gap = 18 * u;
  head_h = header_h(u);
  foot_h = footer_h(u, out_h, lh);
  panel_w = want * u;
  if (panel_w > out_w - 16 * u) panel_w = out_w - 16 * u;
  body_w = panel_w - 2 * pad;
  two_col = (video || system) && panel_w >= 440 * u;
  left_w = video ? (body_w - gap) / 2 : (system ? (body_w - gap) * 44 / 100 : body_w);
  if (h->conflict_msg[0] != '\0' && page->page_id == PAGE_CONTROLS) banner_h = 34 * u;
  avail_h = out_h - 12 * u - head_h - foot_h - 2 * pad - banner_h;
  if (avail_h < s_rh) avail_h = s_rh;
  if (two_col) {
    int lh_used = 0, rh_used = 0;
    const int rw = body_w - left_w - gap;
    for (i = 0; i < page->count; ++i) {
      const SvItem *it = &page->items[i];
      const int w = (video || !it->column) ? left_w : rw;
      if (video || !it->column) lh_used += item_height(it, i, u, w); else rh_used += item_height(it, i, u, w);
    }
    if (video) { /* the preview takes what the column allows: a 4:3-ish picture as wide as the column, smaller when the window is short */
      const int fixed = preview_column_h(u, 0);
      pic_h = rw * 3 / 4;
      if (pic_h + fixed > avail_h) pic_h = avail_h - fixed;
      if (pic_h < 60 * u) pic_h = 60 * u;
      rh_used = pic_h + fixed;
    }
    used = lh_used > rh_used ? lh_used : rh_used;
    if (used > avail_h && u > 1) { --u; s_ls = u; s_rh = cls == CLASS_STANDARD ? 34 * u : 28 * u; goto layout_retry; }
    if (used > avail_h) two_col = 0; /* still too tall at 1x: one scrolling column */
    else drawn = page->count;
  }
  if (!two_col) {
    int est = 0;
    for (k = page->scroll, used = 0; k < page->count && used + item_height(&page->items[k], k, u, body_w) <= avail_h; ++k) { used += item_height(&page->items[k], k, u, body_w); ++est; }
    if (est < 1) est = 1;
    sv_menu_ensure_visible(page, est);
    for (k = page->scroll, used = 0, drawn = 0; k < page->count && used + item_height(&page->items[k], k, u, body_w) <= avail_h; ++k) { used += item_height(&page->items[k], k, u, body_w); ++drawn; }
    if (drawn < 1) drawn = 1;
  }
  body_h = used;
  panel_h = head_h + pad + body_h + banner_h + pad + foot_h;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  if (y < 6 * u) y = 6 * u;
  if (preview && !video && panel_w < out_w * 2 / 3) x = 16 * u;  /* Quick Settings: anchored left, the picture stays visible beside it */
  sv_ui_frame(h, x, y, panel_w, panel_h, u, K_PANEL, K_EDGE, K_LIGHT, 1);
  body_y = y + head_h + pad;
  draw_header(h, x, y, panel_w, u, page->title);
  if (two_col) {
    int ly = body_y, ry = body_y;
    const int lx = x + pad, rx = x + pad + left_w + gap, rw = body_w - left_w - gap;
    for (i = 0; i < page->count; ++i) {
      int rect[4];
      const SvItem *it = &page->items[i];
      if (video || !it->column) {
        draw_item(h, page, i, lx, ly, left_w, u, rect);
        ly += item_height(it, i, u, left_w);
      } else {
        draw_item(h, page, i, rx, ry, rw, u, rect);
        ry += item_height(it, i, u, rw);
      }
      if (h->ui_rows < SV_MENU_MAX_ITEMS && it->kind != SV_ITEM_LABEL) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = i; h->ui_rows++; }
    }
    sv_host_fill(h, rx - gap / 2, body_y, u, body_h, K_RULE);
    if (video) draw_preview_column(h, rx, body_y, rw, pic_h, body_h, u);
  } else {
    int cy = body_y;
    for (i = 0; i < drawn; ++i) {
      int rect[4];
      const int idx = page->scroll + i;
      draw_item(h, page, idx, x + pad, cy, body_w, u, rect);
      if (h->ui_rows < SV_MENU_MAX_ITEMS && page->items[idx].kind != SV_ITEM_LABEL) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = idx; h->ui_rows++; }
      cy += item_height(&page->items[idx], idx, u, body_w);
    }
    if (page->count > drawn && drawn > 0) { /* scroll bar */
      const int tx = x + panel_w - 7 * u, ty = body_y, th = used;
      int kh = th * drawn / page->count;
      if (kh < 12 * u) kh = 12 * u;
      sv_host_fill(h, tx, ty, 3 * u, th, K_FIELD);
      sv_host_fill(h, tx, ty + (th - kh) * page->scroll / (page->count - drawn), 3 * u, kh, K_LIGHT);
    }
  }
  if (banner_h > 0) {  /* Controls: the last binding conflict, in amber, until the next change */
    const int by = body_y + body_h + 4 * u, bh = banner_h - 4 * u;
    char buf[160];
    sv_ui_chamfer(h, x + pad, by, body_w, bh, 2 * u, K_WARN);
    sv_ui_chamfer(h, x + pad + u, by + u, body_w - 2 * u, bh - 2 * u, u, K_BANNER);
    sv_host_icon_draw(h, "warning", (float)(x + pad + 10 * u), (float)(by + (bh - 16 * u) / 2), (float)(16 * u), K_WARN);
    ellipsized(h->conflict_msg, u, body_w - 48 * u, buf, (int)sizeof buf, SV_FONT_BODY);
    text_bold(h, u, x + pad + 34 * u, text_top(by, bh, u), K_WARN, buf);
  }
  /* footer: the focused item's explanation (up to two lines, full width), then the navigation cues at the right */
  {
    const int fy = y + panel_h - foot_h;
    const SvItem *cur = page->focus >= 0 && page->focus < page->count ? &page->items[page->focus] : NULL;
    const int cue_y = y + panel_h - 18 * u;
    rule(h, x + pad, fy, body_w, u);
    if (h->listen.kind != SV_LISTEN_NONE) {
      const unsigned left = 8u - (unsigned)((SDL_GetTicks() - h->listen.started) / 1000u);
      char msg[160];
      snprintf(msg, sizeof msg, h->listen.kind == SV_LISTEN_PAD ? "Press a gamepad button (%us).  Esc cancels, Backspace clears." : "Press a key (%us).  Esc cancels, Backspace clears.", left > 8u ? 0u : left);
      text_in(h, SV_FONT_BODY, u, x + pad, fy + 9 * u, body_w, K_WARN, msg);
    } else {
      int start[3], len[3], truncated = 0, n = 0;
      draw_cues(h, x + panel_w - pad, cue_y, u, h->menu.depth > 1);
      if (cur != NULL && cur->hint[0] != '\0' && !is_note(cur)) {
        char line[200];
        const SDL_Color hc = cur->enabled ? K_TEXT2 : K_ERR;
        n = sv_font_wrap(SV_FONT_BODY, cur->hint, u, body_w, out_h < 360 * u ? 1 : 2, start, len, &truncated);
        for (k = 0; k < n; ++k) {
          snprintf(line, sizeof line, "%.*s", len[k] < 190 ? len[k] : 190, cur->hint + start[k]);
          if (k == n - 1 && truncated) text_in(h, SV_FONT_BODY, u, x + pad, fy + 9 * u + k * lh, body_w, hc, cur->hint + start[k]); /* the rest of the text, cut with "..." */
          else sv_ui_text(h, SV_FONT_BODY, u, x + pad, fy + 9 * u + k * lh, hc, line);
        }
      }
    }
  }
}

int sv_pages_click(SvHost *h, float mx, float my) {
  int p;
  for (p = 0; p < 4; ++p) {
    const int *r = h->ui_preset_rect[p];
    if (r[2] > 0 && mx >= (float)r[0] && mx < (float)(r[0] + r[2]) && my >= (float)r[1] && my < (float)(r[1] + r[3])) {
      sv_apply_crt_preset(&h->settings.video, (SvCrtPreset)p);
      sv_host_toast(h, sv_crt_preset_name((SvCrtPreset)p));
      sv_settings_sanitize(&h->settings);
      sv_host_mark_dirty(h);
      return 1;
    }
  }
  return 0;
}

/* The stop screen: the guest ended; the last picture stays behind a dimmed panel until a key, a button or the window close. */
void sv_draw_stop_screen(SvHost *h, int out_w, int out_h, const char *title, const char *detail) {
  const int u = sv_host_ui_scale(out_w, out_h), pad = 18 * u, lh = sv_ui_line_h(SV_FONT_BODY, u);
  int panel_w = 480 * u, panel_h, x, y, n, i, ty;
  int start[10], len[10], truncated;
  char line[200];
  const SDL_Color dim = {4, 8, 20, 200};
  if (panel_w > out_w - 12 * u) panel_w = out_w - 12 * u;
  sv_host_fill(h, 0, 0, out_w, out_h, dim);
  n = sv_font_wrap(SV_FONT_BODY, detail != NULL ? detail : "", u, panel_w - 2 * pad, 10, start, len, &truncated);
  panel_h = 64 * u + n * lh + 44 * u;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  sv_ui_frame(h, x, y, panel_w, panel_h, u, K_PANEL, K_EDGE, K_LIGHT, 1);
  sv_host_fill(h, x + 2 * u, y + 2 * u, panel_w - 4 * u, 3 * u, K_WARN);
  sv_host_icon_draw(h, "warning", (float)(x + pad), (float)(y + pad), (float)(22 * u), K_WARN);
  text_bold(h, u, x + pad + 32 * u, y + pad + 5 * u, K_WHITE, title != NULL ? title : "The game stopped");
  ty = y + pad + 36 * u;
  for (i = 0; i < n; ++i) {
    snprintf(line, sizeof line, "%.*s", len[i] < 190 ? len[i] : 190, detail + start[i]);
    sv_ui_text(h, SV_FONT_BODY, u, x + pad, ty, K_TEXT2, line);
    ty += lh;
  }
  draw_cues(h, x + panel_w - pad, y + panel_h - pad - 2 * u, u, 0);
}

int sv_host_ui_preset_rect(const SvHost *h, int index, int rect[4]) {
  int k;
  if (h == NULL || index < 0 || index > 3 || h->ui_preset_rect[index][2] <= 0) return 0;
  for (k = 0; k < 4; ++k) rect[k] = h->ui_preset_rect[index][k];
  return 1;
}
