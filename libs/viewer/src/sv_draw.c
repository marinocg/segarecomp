/* Drawing of the pause overlay (the design boards' branded pages): stepped blue frames, a real bitmap font whose widths come from glyph metrics,
 * aligned controls sized from the available pixels (never from a fixed character cell), focus states, and the bottom navigation cues.
 * Everything is drawn from SvPage data; nothing here changes a setting. */
#include <stdio.h>
#include <string.h>

#include "sv_internal.h"

static const SDL_Color K_TEXT = {220, 232, 255, 255}, K_TEXT2 = {169, 183, 221, 255}, K_DIM = {120, 133, 170, 255}, K_CYAN = {64, 191, 245, 255}, K_WHITE = {244, 247, 255, 255},
                       K_FOCUS = {140, 224, 255, 255}, K_ERR = {255, 129, 119, 255}, K_WARN = {255, 197, 109, 255},
                       K_PANEL = {6, 16, 40, 242}, K_EDGE = {35, 62, 120, 255}, K_LIGHT = {77, 103, 167, 255}, K_ROW = {10, 24, 56, 255}, K_ROW_EDGE = {34, 70, 140, 255},
                       K_SEL = {31, 98, 214, 255}, K_SEL_EDGE = {140, 224, 255, 255}, K_SEL_LIGHT = {110, 190, 255, 255}, K_SEL_SOFT = {16, 46, 108, 255}, K_FIELD = {5, 12, 30, 255},
                       K_ON = {52, 140, 245, 255}, K_OFF = {22, 38, 76, 255}, K_BLUE = {21, 87, 201, 255}, K_RULE = {34, 70, 140, 255}, K_BANNER = {64, 50, 14, 255};

/* ---- small shapes ---- */

static void text_bold(SvHost *h, int u, int x, int y, SDL_Color c, const char *text) {
  sv_ui_text(h, SV_FONT_BODY, u, x, y, c, text);
  sv_ui_text(h, SV_FONT_BODY, u, x + u, y, c, text);
}

/* A pixel chevron: seven rows, widest in the middle. dir < 0 points left, > 0 points right. */
static void chevron(SvHost *h, int x, int y, int dir, int u, SDL_Color c) {
  static const int widths[7] = {1, 2, 3, 4, 3, 2, 1};
  int k;
  for (k = 0; k < 7; ++k) {
    const int w = widths[k] * u;
    sv_host_fill(h, dir < 0 ? x + 4 * u - w : x, y + k * u, w, u, c);
  }
}

static void rule(SvHost *h, int x, int y, int w, int u) { sv_host_fill(h, x, y, w, u, K_RULE); }

static int ellipsized(const char *src, int u, int max_px, char *out, int cap, SvFontId face) { return sv_font_ellipsize(face, src, u, max_px < 8 ? 8 : max_px, out, cap); }

static void text_in(SvHost *h, SvFontId face, int u, int x, int y, int max_px, SDL_Color c, const char *text) {
  char buf[160];
  ellipsized(text, u, max_px, buf, (int)sizeof buf, face);
  sv_ui_text(h, face, u, x, y, c, buf);
}

static void text_right(SvHost *h, int u, int right, int y, int max_px, SDL_Color c, const char *text) {
  char buf[160];
  const int w = ellipsized(text, u, max_px, buf, (int)sizeof buf, SV_FONT_BODY);
  sv_ui_text(h, SV_FONT_BODY, u, right - w, y, c, buf);
}

static int nintendo_pad_present(const SvHost *h) {
  int i;
  for (i = 0; i < sv_diag_count(h->diag); ++i) {
    const SvDiagDevice *d = sv_diag_device(h->diag, i);
    if (d != NULL && (d->vendor == 0x057eU || d->gamepad_type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)) return 1;
  }
  return 0;
}

/* ---- navigation cues ---- */

static int draw_cue(SvHost *h, int right, int cy, int u, int pad_mode, int nin, int accept) {
  /* returns the x of the cue's left edge; draws [glyph] label right-aligned at `right` */
  const char *label = accept ? "Select" : "Close";
  char glyph[8];
  int gw, lw = sv_ui_text_w(SV_FONT_BODY, u, label), x, d = 15 * u;
  SDL_Color col = accept ? (SDL_Color){64, 200, 140, 255} : (SDL_Color){235, 80, 80, 255};
  if (pad_mode) {
    snprintf(glyph, sizeof glyph, "%s", accept ? (nin ? "B" : "A") : (nin ? "A" : "B"));
    gw = d;
    x = right - lw - 5 * u - gw;
    sv_ui_chamfer(h, x, cy - d / 2, d, d, 5 * u, col);
    sv_ui_text(h, SV_FONT_BODY, u, x + (d - sv_ui_text_w(SV_FONT_BODY, u, glyph)) / 2, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_WHITE, glyph);
  } else {
    snprintf(glyph, sizeof glyph, "%s", accept ? "Enter" : "Esc");
    gw = sv_ui_text_w(SV_FONT_BODY, u, glyph) + 10 * u;
    x = right - lw - 5 * u - gw;
    sv_ui_frame(h, x, cy - d / 2, gw, d, u, (SDL_Color){22, 40, 84, 255}, K_ROW_EDGE, K_LIGHT, 0);
    sv_ui_text(h, SV_FONT_BODY, u, x + 5 * u, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_TEXT, glyph);
  }
  sv_ui_text(h, SV_FONT_BODY, u, right - lw, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_TEXT2, label);
  return x;
}

static int draw_cues(SvHost *h, int right, int cy, int u, int back_label) {
  const int pad_mode = h->last_input_pad, nin = pad_mode && nintendo_pad_present(h);
  int x;
  (void)back_label;
  {  /* Back/Close: the second cue sits on the right, Select before it */
    char label[16];
    const char *bk = back_label ? "Back" : "Close";
    int lw, gw, bx, d = 15 * u;
    snprintf(label, sizeof label, "%s", bk);
    lw = sv_ui_text_w(SV_FONT_BODY, u, label);
    if (pad_mode) {
      const char *glyph = nin ? "A" : "B";
      gw = d;
      bx = right - lw - 5 * u - gw;
      sv_ui_chamfer(h, bx, cy - d / 2, d, d, 5 * u, (SDL_Color){235, 80, 80, 255});
      sv_ui_text(h, SV_FONT_BODY, u, bx + (d - sv_ui_text_w(SV_FONT_BODY, u, glyph)) / 2, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_WHITE, glyph);
    } else {
      gw = sv_ui_text_w(SV_FONT_BODY, u, "Esc") + 10 * u;
      bx = right - lw - 5 * u - gw;
      sv_ui_frame(h, bx, cy - d / 2, gw, d, u, (SDL_Color){22, 40, 84, 255}, K_ROW_EDGE, K_LIGHT, 0);
      sv_ui_text(h, SV_FONT_BODY, u, bx + 5 * u, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_TEXT, "Esc");
    }
    sv_ui_text(h, SV_FONT_BODY, u, right - lw, cy - sv_ui_line_h(SV_FONT_BODY, u) / 2 + u, K_TEXT2, label);
    x = bx;
  }
  x = draw_cue(h, x - 14 * u, cy, u, pad_mode, nin, 1);
  return x;
}

/* ---- items ---- */

static int is_note(const SvItem *it) { return it->kind == SV_ITEM_LABEL && strcmp(it->icon, "note") == 0; }
static int is_section(const SvItem *it) { return it->kind == SV_ITEM_LABEL && strcmp(it->icon, "section") == 0; }

static int row_h(int u) { return 26 * u; }

static int item_height(const SvItem *it, int u, int inner_w) {
  if (is_section(it)) return 24 * u;
  if (is_note(it)) {
    int start[8], len[8], truncated, n = sv_font_wrap(SV_FONT_BODY, it->hint, u, inner_w - 40 * u, 8, start, len, &truncated);
    if (n < 1) n = 1;
    return n * sv_ui_line_h(SV_FONT_BODY, u) + 16 * u;
  }
  return row_h(u);
}

static void row_background(SvHost *h, int x, int y, int w, int rh, int u, int focused, int always, int enabled) {
  if (focused && enabled) {
    sv_ui_chamfer(h, x, y, w, rh - 2 * u, 2 * u, (SDL_Color){4, 10, 26, 255});
    sv_ui_chamfer(h, x + u, y + u, w - 2 * u, rh - 4 * u, 2 * u, K_SEL_EDGE);
    sv_ui_chamfer(h, x + 2 * u, y + 2 * u, w - 4 * u, rh - 6 * u, u, K_SEL);
    sv_host_fill(h, x + 4 * u, y + 2 * u, w - 8 * u, u, K_SEL_LIGHT);
  } else if (always) {
    sv_ui_chamfer(h, x, y, w, rh - 2 * u, 2 * u, K_ROW_EDGE);
    sv_ui_chamfer(h, x + u, y + u, w - 2 * u, rh - 4 * u, u, K_ROW);
  }
}

static void switch_widget(SvHost *h, int x, int cy, int u, int on, int focused) {
  const int w = 34 * u, hh = 16 * u, y = cy - hh / 2;
  sv_ui_chamfer(h, x, y, w, hh, 3 * u, (SDL_Color){4, 10, 26, 255});
  sv_ui_chamfer(h, x + u, y + u, w - 2 * u, hh - 2 * u, 2 * u, on ? K_BLUE : K_OFF);
  if (on) sv_host_fill(h, x + 3 * u, y + u, w - 6 * u, u, K_SEL_LIGHT);
  {
    const int kx = on ? x + w - hh + u : x + u;
    sv_ui_chamfer(h, kx, y + u, hh - 2 * u, hh - 2 * u, 2 * u, on ? K_WHITE : (SDL_Color){150, 168, 205, 255});
  }
  (void)focused;
}

static void draw_item(SvHost *h, const SvPage *page, int idx, int x, int y, int w, int u, int is_confirm, int *rect_out) {
  const SvItem *it = &page->items[idx];
  const int rh = item_height(it, u, w), focused = idx == page->focus;
  const int lh = sv_ui_line_h(SV_FONT_BODY, u);
  const int ty = y + (row_h(u) - 14 * u) / 2 - u;
  const int cx = x + 10 * u, enabled = it->enabled;
  const SDL_Color base = enabled ? K_TEXT : K_DIM, tc = focused && enabled ? K_WHITE : base;
  rect_out[0] = x; rect_out[1] = y; rect_out[2] = w; rect_out[3] = rh;
  if (is_section(it)) {
    sv_ui_text(h, SV_FONT_BODY, u, x + 2 * u, y + 5 * u, K_FOCUS, it->label);
    sv_host_fill(h, x, y + 20 * u, w, u, K_RULE);
    return;
  }
  if (is_note(it)) {
    int start[8], len[8], truncated, n, i;
    char line[200];
    n = sv_font_wrap(SV_FONT_BODY, it->hint, u, w - 40 * u, 8, start, len, &truncated);
    sv_ui_chamfer(h, x, y + u, w, rh - 2 * u, 2 * u, K_ROW_EDGE);
    sv_ui_chamfer(h, x + u, y + 2 * u, w - 2 * u, rh - 4 * u, u, (SDL_Color){10, 26, 60, 255});
    sv_host_icon_draw(h, "info", (float)(x + 10 * u), (float)(y + 8 * u), (float)(16 * u), K_FOCUS);
    for (i = 0; i < n; ++i) {
      snprintf(line, sizeof line, "%.*s", len[i] < 190 ? len[i] : 190, it->hint + start[i]);
      sv_ui_text(h, SV_FONT_BODY, u, x + 34 * u, y + 8 * u + i * lh, K_TEXT2, line);
    }
    return;
  }
  row_background(h, x, y, w, row_h(u), u, focused, it->kind == SV_ITEM_BUTTON, enabled);
  {
    int lx = cx;
    int label_w;
    if (it->icon[0] != '\0') {
      sv_host_icon_draw(h, it->icon, (float)lx, (float)(y + (row_h(u) - 2 * u - 16 * u) / 2 + u), (float)(16 * u), focused && enabled ? K_WHITE : (enabled ? K_TEXT2 : K_DIM));
      lx += 24 * u;
    }
    label_w = (it->kind == SV_ITEM_BUTTON ? w : (it->kind == SV_ITEM_TOGGLE ? w - 34 * u - 50 * u : w * 45 / 100)) - (lx - x) - 8 * u;
    text_in(h, SV_FONT_BODY, u, lx, ty, label_w, it->kind == SV_ITEM_LABEL ? K_TEXT2 : tc, it->label);
    if (it->kind == SV_ITEM_BUTTON && it->icon[0] == '\0') { /* nothing else */ }
  }
  {
    const int vx = x + w * 45 / 100, right = x + w - 10 * u, vw = right - vx;
    switch (it->kind) {
      case SV_ITEM_TOGGLE: {
        const int sw = 34 * u;
        switch_widget(h, right - sw, y + (row_h(u) - 2 * u) / 2, u, it->value, focused);
        text_right(h, u, right - sw - 8 * u, ty, vw, focused && enabled ? K_WHITE : K_TEXT2, it->text);
        break;
      }
      case SV_ITEM_CHOICE: {
        if (it->id == I_PLAYER) { /* player tabs */
          int t;
          const int tw = (vw - 6 * u) / 2;
          for (t = 0; t < 2; ++t) {
            const int tx = vx + t * (tw + 6 * u), on = t == it->value;
            sv_ui_chamfer(h, tx, y + 2 * u, tw, row_h(u) - 6 * u, 2 * u, on ? K_SEL_EDGE : K_ROW_EDGE);
            sv_ui_chamfer(h, tx + u, y + 3 * u, tw - 2 * u, row_h(u) - 8 * u, u, on ? K_SEL : K_FIELD);
            {
              const char *name = t == 0 ? "Player 1" : "Player 2";
              sv_ui_text(h, SV_FONT_BODY, u, tx + (tw - sv_ui_text_w(SV_FONT_BODY, u, name)) / 2, ty, on ? K_WHITE : K_TEXT2, name);
            }
          }
        } else {
          char buf[160];
          const int ch = 7 * u, cy = y + (row_h(u) - 2 * u - ch) / 2;
          const int inner_l = vx + 6 * u, inner_r = right - 4 * u;
          const SDL_Color arrow = focused && enabled ? K_WHITE : K_DIM;
          const int w2 = ellipsized(it->text, u, vw - 36 * u, buf, (int)sizeof buf, SV_FONT_BODY);
          chevron(h, inner_l, cy, -1, u, arrow);
          chevron(h, inner_r - 4 * u, cy, 1, u, arrow);
          sv_ui_text(h, SV_FONT_BODY, u, (inner_l + inner_r) / 2 - w2 / 2, ty, tc, buf);
        }
        break;
      }
      case SV_ITEM_SLIDER: {
        const int val_w = 7 * 7 * u, tx = vx, tw = vw - val_w - 6 * u, ty2 = y + (row_h(u) - 2 * u) / 2;
        const int span = it->max - it->min > 0 ? it->max - it->min : 1, fill_w = (it->value - it->min) * (tw - 2 * u) / span;
        sv_ui_chamfer(h, tx, ty2 - 3 * u, tw, 6 * u, 2 * u, (SDL_Color){4, 10, 26, 255});
        sv_ui_chamfer(h, tx + u, ty2 - 2 * u, tw - 2 * u, 4 * u, u, K_OFF);
        if (fill_w > 0) sv_ui_chamfer(h, tx + u, ty2 - 2 * u, fill_w, 4 * u, u, K_ON);
        {
          int kx = tx + u + fill_w - 4 * u;
          if (kx < tx) kx = tx;
          sv_ui_chamfer(h, kx, ty2 - 7 * u, 8 * u, 14 * u, 2 * u, (SDL_Color){4, 10, 26, 255});
          sv_ui_chamfer(h, kx + u, ty2 - 6 * u, 6 * u, 12 * u, u, focused && enabled ? K_WHITE : K_TEXT);
        }
        text_right(h, u, right, ty, val_w, tc, it->text);
        break;
      }
      case SV_ITEM_LABEL:
        text_right(h, u, right, ty, w * 62 / 100, K_TEXT, it->text);
        break;
      case SV_ITEM_BINDING: {
        const int cell_w = (vw - 6 * u) / 2;
        int t;
        for (t = 0; t < 2; ++t) {
          const int tx = vx + t * (cell_w + 6 * u), on = focused && it->value == t;
          const char *txt = t == 0 ? it->text : it->text2;
          char buf[96];
          /* binding capture: the cell being edited says so (and the footer counts down) */
          const int capture = h->listen.kind != SV_LISTEN_NONE && ((it->id == I_MENUKEY && h->listen.kind == SV_LISTEN_MENU_KEY && t == 0) ||
                              (it->id == I_BIND + (int)h->listen.action && ((h->listen.kind == SV_LISTEN_KEY && t == 0) || (h->listen.kind == SV_LISTEN_PAD && t == 1))));
          int tw2;
          if (t == 1 && it->text2[0] == '\0') break;
          sv_ui_chamfer(h, tx, y + 2 * u, cell_w, row_h(u) - 6 * u, 2 * u, capture ? K_WARN : (on ? K_SEL_EDGE : K_ROW_EDGE));
          sv_ui_chamfer(h, tx + u, y + 3 * u, cell_w - 2 * u, row_h(u) - 8 * u, u, capture ? K_BANNER : (on ? K_SEL_SOFT : K_FIELD));
          tw2 = ellipsized(capture ? "Press now..." : txt, u, cell_w - 12 * u, buf, (int)sizeof buf, SV_FONT_BODY);
          sv_ui_text(h, SV_FONT_BODY, u, tx + (cell_w - tw2) / 2, ty, capture ? K_WARN : (on ? K_WHITE : K_TEXT2), buf);
        }
        break;
      }
      case SV_ITEM_BUTTON: break;
    }
  }
  (void)is_confirm;
  (void)lh;
}

/* ---- Video page: live preview ---- */

static void draw_preview_column(SvHost *h, int x, int y, int w, int body_h, int u) {
  static const char *const desc[] = {"The untouched picture: nearest-neighbour pixels, no filter.", "Subtle scanline gaps and a gentle glow.",
                                     "Scanlines, an RGB mask, glow and slight curvature.", "A stronger arcade-monitor look: heavy scanlines, mask and curve."};
  const int lh = sv_ui_line_h(SV_FONT_BODY, u);
  int py = y, p, pic_h;
  sv_ui_text(h, SV_FONT_BODY, u, x, py, K_FOCUS, "LIVE PREVIEW");
  py += lh + 4 * u;
  pic_h = w * 5 / 8;
  if (pic_h > body_h - 100 * u) pic_h = body_h - 100 * u;
  if (pic_h < 40 * u) pic_h = 40 * u;
  sv_ui_chamfer(h, x, py, w, pic_h, 2 * u, K_ROW_EDGE);
  sv_ui_chamfer(h, x + u, py + u, w - 2 * u, pic_h - 2 * u, u, (SDL_Color){0, 0, 0, 255});
  sv_host_draw_preview(h, x + 2 * u, py + 2 * u, w - 4 * u, pic_h - 4 * u);
  {
    const char *tag = h->settings.video.preset == SV_CRT_PIXEL_PERFECT ? "centre crop, untouched" : "centre crop, effect applied";
    const int tw = sv_ui_text_w(SV_FONT_BODY, u, tag) + 8 * u;
    sv_host_fill(h, x + 2 * u, py + pic_h - lh - 4 * u, tw, lh + 2 * u, (SDL_Color){4, 8, 20, 190});
    sv_ui_text(h, SV_FONT_BODY, u, x + 6 * u, py + pic_h - lh - 3 * u, K_TEXT2, tag);
  }
  py += pic_h + 6 * u;
  for (p = 0; p < 4; ++p) {
    const int bw = (w - 6 * u) / 2, bx = x + (p % 2) * (bw + 6 * u), by = py + (p / 2) * (22 * u);
    const int on = (int)h->settings.video.preset == p;
    sv_ui_chamfer(h, bx, by, bw, 20 * u, 2 * u, on ? K_SEL_EDGE : K_ROW_EDGE);
    sv_ui_chamfer(h, bx + u, by + u, bw - 2 * u, 18 * u, u, on ? K_SEL : K_ROW);
    {
      const char *nm = sv_crt_preset_name((SvCrtPreset)p);
      char buf[48];
      const int tw = ellipsized(nm, u, bw - 8 * u, buf, (int)sizeof buf, SV_FONT_BODY);
      sv_ui_text(h, SV_FONT_BODY, u, bx + (bw - tw) / 2, by + (20 * u - 14 * u) / 2 - u / 2, on ? K_WHITE : K_TEXT2, buf);
    }
    h->ui_preset_rect[p][0] = bx; h->ui_preset_rect[p][1] = by; h->ui_preset_rect[p][2] = bw; h->ui_preset_rect[p][3] = 20 * u;
  }
  py += 2 * 22 * u + 6 * u;
  {
    const char *name = sv_crt_preset_name(h->settings.video.preset);
    const char *d = h->settings.video.preset == SV_CRT_CUSTOM ? "Custom: your own scanline, mask, curvature, bloom and brightness values." : desc[h->settings.video.preset];
    int start[6], len[6], truncated, n, i;
    char line[160];
    text_bold(h, u, x, py, K_TEXT, name);
    py += lh + 2 * u;
    n = sv_font_wrap(SV_FONT_BODY, d, u, w, 5, start, len, &truncated);
    for (i = 0; i < n && py + lh < y + body_h; ++i) {
      snprintf(line, sizeof line, "%.*s", len[i] < 150 ? len[i] : 150, d + start[i]);
      sv_ui_text(h, SV_FONT_BODY, u, x, py, K_TEXT2, line);
      py += lh;
    }
  }
}

/* ---- the page ---- */

static int page_units(int page_id) {
  switch (page_id) {
    case PAGE_MAIN: return 330;
    case PAGE_QUICK: return 440;
    case PAGE_VIDEO: return 640;
    case PAGE_CONTROLS: return 600;
    case PAGE_AUDIO: return 520;
    case PAGE_SYSTEM: return 640;
    case PAGE_PADSTATUS: return 600;
    default: return 400;
  }
}

static void draw_header(SvHost *h, int x, int y, int w, int u, const char *title) {
  const int pad = 14 * u;
  const int hs = 2 * u;
  int title_w, avail;
  int wm_w = 0;
  sv_host_icon_draw(h, "controller", (float)(x + pad), (float)(y + 9 * u), (float)(18 * u), K_CYAN);
  {
    SDL_Texture *wm = sv_ui_wordmark(h);
    if (wm != NULL) {
      float tw = 0, th = 0;
      SDL_FRect d, src;
      SDL_GetTextureSize(wm, &tw, &th);
      /* the visible part of wordmark.png (its transparent margins are cropped away) */
      src.x = tw * 38.0f / 1200.0f; src.y = th * 74.0f / 400.0f; src.w = tw * 1125.0f / 1200.0f; src.h = th * 295.0f / 400.0f;
      d.h = (float)(24 * u);
      d.w = d.h * src.w / src.h;
      d.x = (float)(x + pad + 26 * u);
      d.y = (float)(y + 9 * u);
      SDL_RenderTexture(h->renderer, wm, &src, &d);
      wm_w = (int)d.w;
    } else {
      wm_w = sv_ui_text_w(SV_FONT_HEAD, hs, "SEGARECOMP");
      sv_ui_text_shadow(h, SV_FONT_HEAD, hs, x + pad + 26 * u, y + 12 * u, K_FOCUS, "SEGARECOMP");
    }
  }
  avail = w - 2 * pad - 24 * u - wm_w - 16 * u;
  title_w = sv_ui_text_w(SV_FONT_HEAD, u * 2, title);
  if (title_w > avail) {  /* a long page title falls back to the small heading size, then is cut */
    char buf[64];
    const int w2 = sv_font_ellipsize(SV_FONT_HEAD, title, u, avail, buf, (int)sizeof buf);
    sv_ui_text(h, SV_FONT_HEAD, u, x + w - pad - w2, y + 14 * u, K_TEXT2, buf);
  } else {
    sv_ui_text(h, SV_FONT_HEAD, u * 2, x + w - pad - title_w, y + 12 * u, K_TEXT2, title);
  }
  rule(h, x + pad, y + 36 * u, w - 2 * pad, u);
}

void sv_pages_draw(SvHost *h, int out_w, int out_h) {
  SvPage *page = sv_menu_top(&h->menu);
  const int preview = sv_pages_wants_preview(h);
  int u = sv_host_ui_scale(out_w, out_h);
  const int is_confirm = page != NULL && page->page_id >= PAGE_CONFIRM_QUIT;
  const int video = page != NULL && page->page_id == PAGE_VIDEO;
  const int system = page != NULL && page->page_id == PAGE_SYSTEM;
  int units, panel_w, panel_h, x, y, pad, head_h, foot_h, body_x, body_w, body_y, body_h, i, drawn = 0, used = 0, avail_h, banner_h = 0, left_w, lh;
  int two_col;
  if (page == NULL) return;
  units = page_units(page->page_id);
  while (u > 1 && units * u > out_w - 16 * u) --u;
  lh = sv_ui_line_h(SV_FONT_BODY, u);
  pad = 14 * u;
  head_h = is_confirm ? 0 : 42 * u;
  foot_h = is_confirm ? 12 * u : 2 * lh + 36 * u;
  panel_w = units * u;
  if (panel_w > out_w - 12 * u) panel_w = out_w - 12 * u;
  two_col = (video || system) && panel_w >= 440 * u;
  left_w = video ? (panel_w - 2 * pad) * 56 / 100 : (system ? (panel_w - 2 * pad) * 46 / 100 : panel_w - 2 * pad);
  body_x = 0; body_w = panel_w - 2 * pad;
  if (h->conflict_msg[0] != '\0' && page->page_id == PAGE_CONTROLS) banner_h = 30 * u;
  avail_h = out_h - 12 * u - head_h - foot_h - 2 * pad - banner_h;
  if (avail_h < 4 * row_h(u)) avail_h = 4 * row_h(u);
  (void)body_x;
  h->ui_rows = 0;
  memset(h->ui_preset_rect, 0, sizeof h->ui_preset_rect);
  if (two_col) {
    /* left column items first (column 0), right column items (column 1); the page does not scroll */
    int lh_used = 0, rh_used = 0;
    for (i = 0; i < page->count; ++i) {
      const SvItem *it = &page->items[i];
      const int w = video ? left_w : (it->column ? body_w - left_w - 14 * u : left_w);
      if (video || !it->column) lh_used += item_height(it, u, w); else rh_used += item_height(it, u, w);
    }
    used = lh_used > rh_used ? lh_used : rh_used;
    if (video) { const int min_prev = 150 * u; if (used < min_prev) used = min_prev; }
    drawn = page->count;
  } else {
    int est = 0, k;
    for (k = page->scroll, used = 0; k < page->count && used + item_height(&page->items[k], u, body_w) <= avail_h; ++k) { used += item_height(&page->items[k], u, body_w); ++est; }
    if (est < 1) est = 1;
    sv_menu_ensure_visible(page, est);
    for (k = page->scroll, used = 0, drawn = 0; k < page->count && used + item_height(&page->items[k], u, body_w) <= avail_h; ++k) { used += item_height(&page->items[k], u, body_w); ++drawn; }
    if (drawn < 1) drawn = 1;
  }
  body_h = used;
  if (is_confirm) {
    int n = page->count;
    panel_h = 60 * u + n * (row_h(u) + 6 * u) + 14 * u;
    if (page->page_id == PAGE_CONFIRM_QUIT || page->page_id == PAGE_CONFIRM_RESET || page->page_id == PAGE_CONFIRM_VIDEO || page->page_id == PAGE_CONFIRM_CONTROLS) panel_h += 24 * u;
  } else {
    panel_h = head_h + pad + body_h + banner_h + pad + foot_h;
  }
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  if (y < 6 * u) y = 6 * u;
  if (preview && !video && panel_w < out_w * 2 / 3) x = 10 * u;  /* Quick Settings leaves the picture visible next to it */
  sv_ui_frame(h, x, y, panel_w, panel_h, u, K_PANEL, K_EDGE, K_LIGHT, 1);
  body_y = y + head_h + pad;
  if (!is_confirm) draw_header(h, x, y, panel_w, u, page->title);
  if (is_confirm) {
    const char *msg = page->page_id == PAGE_CONFIRM_QUIT ? (SDL_getenv("SEGARECOMP_LAUNCHED") != NULL ? "Any unsaved progress will be lost. Return to the launcher and close the game?" : "Any unsaved progress will be lost: there are no save states.") :
                      page->page_id == PAGE_CONFIRM_RESET ? "Restart the game from power-on? Progress since the last power-on is lost." : "Reset these settings to their defaults? Your other settings are kept.";
    int start[6], len[6], truncated, n, k, ty = y + 16 * u;
    char line[200];
    sv_host_icon_draw(h, "warning", (float)(x + pad), (float)(y + 14 * u), (float)(22 * u), K_WARN);
    text_bold(h, u, x + pad + 30 * u, y + 20 * u, K_WHITE, page->title);
    n = sv_font_wrap(SV_FONT_BODY, msg, u, panel_w - 2 * pad, 4, start, len, &truncated);
    ty = y + 48 * u;
    for (k = 0; k < n; ++k) {
      snprintf(line, sizeof line, "%.*s", len[k] < 190 ? len[k] : 190, msg + start[k]);
      sv_ui_text(h, SV_FONT_BODY, u, x + pad, ty, K_TEXT2, line);
      ty += lh;
    }
    body_y = ty + 8 * u;
    for (i = 0; i < page->count; ++i) {
      int rect[4];
      draw_item(h, page, i, x + pad, body_y, panel_w - 2 * pad, u, 1, rect);
      if (h->ui_rows < SV_MENU_MAX_ITEMS) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = i; h->ui_rows++; }
      body_y += row_h(u) + 6 * u;
    }
  } else if (two_col) {
    int ly = body_y, ry = body_y;
    const int lx = x + pad, rx = x + pad + left_w + 14 * u, rw = body_w - left_w - 14 * u;
    for (i = 0; i < page->count; ++i) {
      int rect[4];
      const SvItem *it = &page->items[i];
      if (video || !it->column) {
        draw_item(h, page, i, lx, ly, left_w, u, 0, rect);
        ly += item_height(it, u, left_w);
      } else {
        draw_item(h, page, i, rx, ry, rw, u, 0, rect);
        ry += item_height(it, u, rw);
      }
      if (h->ui_rows < SV_MENU_MAX_ITEMS && it->kind != SV_ITEM_LABEL) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = i; h->ui_rows++; }
    }
    if (video) {
      sv_host_fill(h, rx - 8 * u, body_y, u, body_h, K_RULE);
      draw_preview_column(h, rx, body_y, rw, body_h, u);
    }
  } else {
    int cy = body_y;
    for (i = 0; i < drawn; ++i) {
      int rect[4];
      const int idx = page->scroll + i;
      draw_item(h, page, idx, x + pad, cy, body_w, u, 0, rect);
      if (h->ui_rows < SV_MENU_MAX_ITEMS && page->items[idx].kind != SV_ITEM_LABEL) { memcpy(h->ui_item_rect[h->ui_rows], rect, sizeof rect); h->ui_item_index[h->ui_rows] = idx; h->ui_rows++; }
      cy += item_height(&page->items[idx], u, body_w);
    }
    if (page->count > drawn && drawn > 0) { /* scroll bar */
      const int tx = x + panel_w - 5 * u, ty = body_y, th = used;
      int kh = th * drawn / page->count;
      if (kh < 10 * u) kh = 10 * u;
      sv_host_fill(h, tx, ty, 2 * u, th, K_FIELD);
      sv_host_fill(h, tx, ty + (th - kh) * page->scroll / (page->count - drawn), 2 * u, kh, K_EDGE);
    }
  }
  if (banner_h > 0) {  /* Controls: the last binding conflict, in amber, until the next change */
    const int by = body_y + body_h + 4 * u;
    char buf[160];
    sv_ui_chamfer(h, x + pad, by, panel_w - 2 * pad, banner_h - 4 * u, 2 * u, K_WARN);
    sv_ui_chamfer(h, x + pad + u, by + u, panel_w - 2 * pad - 2 * u, banner_h - 6 * u, u, K_BANNER);
    sv_host_icon_draw(h, "warning", (float)(x + pad + 8 * u), (float)(by + (banner_h - 4 * u - 16 * u) / 2), (float)(16 * u), K_WARN);
    sv_ui_text(h, SV_FONT_BODY, u, x + pad + 32 * u, by + (banner_h - 4 * u - 14 * u) / 2 - u, K_TEXT, (ellipsized(h->conflict_msg, u, panel_w - 2 * pad - 44 * u, buf, (int)sizeof buf, SV_FONT_BODY), buf));
  }
  /* footer: the focused item's explanation (up to two lines, full width), then the navigation cues at the right */
  if (!is_confirm) {
    const int fy = y + panel_h - foot_h;
    const SvItem *cur = page->focus >= 0 && page->focus < page->count ? &page->items[page->focus] : NULL;
    const int cue_y = y + panel_h - 14 * u;
    rule(h, x + pad, fy, panel_w - 2 * pad, u);
    if (h->listen.kind != SV_LISTEN_NONE) {
      const unsigned left = 8u - (unsigned)((SDL_GetTicks() - h->listen.started) / 1000u);
      char msg[160];
      snprintf(msg, sizeof msg, h->listen.kind == SV_LISTEN_PAD ? "Press a gamepad button (%us).  Esc cancels, Backspace clears." : "Press a key (%us).  Esc cancels, Backspace clears.", left > 8u ? 0u : left);
      text_in(h, SV_FONT_BODY, u, x + pad, fy + 7 * u, panel_w - 2 * pad, K_FOCUS, msg);
    } else {
      int start[3], len[3], truncated = 0, n = 0, k;
      draw_cues(h, x + panel_w - pad, cue_y, u, h->menu.depth > 1);
      if (cur != NULL && cur->hint[0] != '\0' && !is_note(cur)) {
        char line[200];
        const SDL_Color hc = cur->enabled ? K_TEXT2 : K_ERR;
        n = sv_font_wrap(SV_FONT_BODY, cur->hint, u, panel_w - 2 * pad, 2, start, len, &truncated);
        for (k = 0; k < n; ++k) {
          snprintf(line, sizeof line, "%.*s", len[k] < 190 ? len[k] : 190, cur->hint + start[k]);
          if (k == n - 1 && truncated) text_in(h, SV_FONT_BODY, u, x + pad, fy + 7 * u + k * lh, panel_w - 2 * pad, hc, cur->hint + start[k]); /* the rest of the text, cut with "..." */
          else sv_ui_text(h, SV_FONT_BODY, u, x + pad, fy + 7 * u + k * lh, hc, line);
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
      sv_settings_sanitize(&h->settings);
      sv_host_mark_dirty(h);
      return 1;
    }
  }
  return 0;
}

/* The stop screen: the guest ended; the last picture stays behind a dimmed panel until a key, a button or the window close. */
void sv_draw_stop_screen(SvHost *h, int out_w, int out_h, const char *title, const char *detail) {
  const int u = sv_host_ui_scale(out_w, out_h), pad = 16 * u, lh = sv_ui_line_h(SV_FONT_BODY, u);
  int panel_w = 440 * u, panel_h, x, y, n, i, ty;
  int start[10], len[10], truncated;
  char line[200];
  const SDL_Color dim = {4, 8, 20, 190};
  if (panel_w > out_w - 12 * u) panel_w = out_w - 12 * u;
  sv_host_fill(h, 0, 0, out_w, out_h, dim);
  n = sv_font_wrap(SV_FONT_BODY, detail != NULL ? detail : "", u, panel_w - 2 * pad, 10, start, len, &truncated);
  panel_h = 60 * u + n * lh + 40 * u;
  x = (out_w - panel_w) / 2;
  y = (out_h - panel_h) / 2;
  sv_ui_frame(h, x, y, panel_w, panel_h, u, K_PANEL, K_EDGE, K_LIGHT, 1);
  sv_host_icon_draw(h, "warning", (float)(x + pad), (float)(y + pad), (float)(22 * u), K_WARN);
  text_bold(h, u, x + pad + 30 * u, y + pad + 6 * u, K_WHITE, title != NULL ? title : "The game stopped");
  ty = y + pad + 34 * u;
  for (i = 0; i < n; ++i) {
    snprintf(line, sizeof line, "%.*s", len[i] < 190 ? len[i] : 190, detail + start[i]);
    sv_ui_text(h, SV_FONT_BODY, u, x + pad, ty, K_TEXT2, line);
    ty += lh;
  }
  draw_cues(h, x + panel_w - pad, y + panel_h - pad - 4 * u, u, 0);
}

int sv_host_ui_preset_rect(const SvHost *h, int index, int rect[4]) {
  int k;
  if (h == NULL || index < 0 || index > 3 || h->ui_preset_rect[index][2] <= 0) return 0;
  for (k = 0; k < 4; ++k) rect[k] = h->ui_preset_rect[index][k];
  return 1;
}
