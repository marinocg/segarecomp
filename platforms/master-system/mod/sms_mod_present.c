/* SEG-147-T006 (ADR 0325): see sms_mod_present.h. Strict C11; no allocation, no clock. The overlay blend is the same integer source-over as the shared HUD overlay; the bitmap font is libs/viewer's sv_font. */
#include "sms_mod_present.h"

#include <string.h>

#include "sv_font.h"

void sms_mod_present_clear(SmsModPresentation *p) {
  p->draws = 0U;
  p->sprite_count = 0U;
  p->has_overlay = 0U;
  p->has_palette = 0U;
  p->has_scroll = 0U;
  p->dx = 0;
  p->dy = 0;
  memset(p->mapped, 0, sizeof p->mapped);
  memset(p->colour, 0, sizeof p->colour);
  memset(p->patterns, 0, sizeof p->patterns);
  memset(p->sprite_colours, 0, sizeof p->sprite_colours);
}

int sms_mod_present_is_empty(const SmsModPresentation *p) { return p->has_overlay == 0U && p->has_palette == 0U && p->has_scroll == 0U && p->sprite_count == 0U; }

/* Straight-alpha source-over in integers. */
static void blend(uint8_t *dst, uint32_t rgba) {
  const uint64_t sr = (rgba >> 24U) & 255U, sg = (rgba >> 16U) & 255U, sb = (rgba >> 8U) & 255U, sa = rgba & 255U;
  uint64_t da, scaled;
  if (sa == 0U) return;
  if (sa == 255U) {
    dst[0] = (uint8_t)sr;
    dst[1] = (uint8_t)sg;
    dst[2] = (uint8_t)sb;
    dst[3] = 255U;
    return;
  }
  da = dst[3];
  scaled = sa * 255U + da * (255U - sa);
  dst[0] = (uint8_t)((sr * sa * 255U + dst[0] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[1] = (uint8_t)((sg * sa * 255U + dst[1] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[2] = (uint8_t)((sb * sa * 255U + dst[2] * da * (255U - sa) + scaled / 2U) / scaled);
  dst[3] = (uint8_t)((scaled + 127U) / 255U);
}

static void overlay_pixel(SmsModPresentation *p, int32_t x, int32_t y, uint32_t rgba) {
  if (x < 0 || y < 0 || x >= (int32_t)SEGA_SMS_SCREEN_WIDTH || y >= (int32_t)SEGA_SMS_SCREEN_MAX_HEIGHT) return;
  blend(&p->overlay[((uint32_t)y * SEGA_SMS_SCREEN_WIDTH + (uint32_t)x) * 4U], rgba);
}

/* One accepted draw request against the per-callback budget. */
static int draw_begin(SmsModPresentation *p) {
  if (p->draws >= SEGA_SMS_OVERLAY_DRAWS_MAX) return SEGA_MOD_E_BUDGET;
  ++p->draws;
  if (p->has_overlay == 0U) {
    memset(p->overlay, 0, sizeof p->overlay);
    p->has_overlay = 1U;
  }
  return SEGA_MOD_OK;
}

static int coord_ok(int32_t v) { return v >= -SMS_MOD_COORD_MAX && v <= SMS_MOD_COORD_MAX; }

int sms_mod_present_rect(SmsModPresentation *p, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgba) {
  int32_t x0, y0, x1, y1, cx, cy, status;
  if (!coord_ok(x) || !coord_ok(y) || w < 0 || w > SMS_MOD_COORD_MAX || h < 0 || h > SMS_MOD_COORD_MAX) return SEGA_MOD_E_RANGE;
  x0 = x > 0 ? x : 0;
  y0 = y > 0 ? y : 0;
  x1 = x + w < (int32_t)SEGA_SMS_SCREEN_WIDTH ? x + w : (int32_t)SEGA_SMS_SCREEN_WIDTH;
  y1 = y + h < (int32_t)SEGA_SMS_SCREEN_MAX_HEIGHT ? y + h : (int32_t)SEGA_SMS_SCREEN_MAX_HEIGHT;
  if (x1 <= x0 || y1 <= y0) return SEGA_MOD_OK; /* nothing visible: not a request that draws */
  status = draw_begin(p);
  if (status != SEGA_MOD_OK) return status;
  for (cy = y0; cy < y1; ++cy)
    for (cx = x0; cx < x1; ++cx) overlay_pixel(p, cx, cy, rgba);
  return SEGA_MOD_OK;
}

int sms_mod_present_text(SmsModPresentation *p, int32_t x, int32_t y, const char *ascii, uint32_t len, uint32_t rgba) {
  const SvFontFace *face = &sv_font_faces[SV_FONT_BODY];
  const int row_bytes = (sv_font_atlas_w + 7) / 8;
  int32_t pen, status;
  uint32_t i;
  if (!coord_ok(x) || !coord_ok(y) || (ascii == NULL && len != 0U)) return SEGA_MOD_E_RANGE;
  if (len > SEGA_SMS_OVERLAY_TEXT_BYTES) len = SEGA_SMS_OVERLAY_TEXT_BYTES; /* a long line is cut, not a fault */
  x = x < 0 ? 0 : x >= (int32_t)SEGA_SMS_SCREEN_WIDTH ? (int32_t)SEGA_SMS_SCREEN_WIDTH - 1 : x;
  y = y < 0 ? 0 : y >= (int32_t)SEGA_SMS_SCREEN_MAX_HEIGHT ? (int32_t)SEGA_SMS_SCREEN_MAX_HEIGHT - 1 : y;
  status = draw_begin(p);
  if (status != SEGA_MOD_OK) return status;
  pen = x;
  for (i = 0U; i < len; ++i) {
    const unsigned char ch = (unsigned char)(ascii[i] >= 0x20 && ascii[i] <= 0x7E ? ascii[i] : '?');
    const SvGlyph *g = sv_font_glyph(SV_FONT_BODY, ch);
    int32_t gx, gy;
    for (gy = 0; gy < g->h; ++gy)
      for (gx = 0; gx < g->w; ++gx) {
        const int ax = g->x + (int)gx, ay = g->y + (int)gy;
        if (((sv_font_atlas_bits[ay * row_bytes + (ax >> 3)] >> (7 - (ax & 7))) & 1) != 0) overlay_pixel(p, pen + g->xoff + gx, y + face->ascent + g->yoff + gy, rgba);
      }
    pen += g->advance;
  }
  return SEGA_MOD_OK;
}

int sms_mod_present_palette(SmsModPresentation *p, uint32_t cram_value, uint32_t rgb) {
  if (cram_value > 63U || rgb > 0xFFFFFFU) return SEGA_MOD_E_RANGE;
  p->mapped[cram_value] = 1U;
  p->colour[cram_value] = rgb;
  p->has_palette = 1U;
  return SEGA_MOD_OK;
}

int sms_mod_present_scroll(SmsModPresentation *p, int32_t dx, int32_t dy) {
  if (dx < -SEGA_SMS_SCROLL_MAX || dx > SEGA_SMS_SCROLL_MAX || dy < -SEGA_SMS_SCROLL_MAX || dy > SEGA_SMS_SCROLL_MAX) return SEGA_MOD_E_RANGE;
  p->dx = dx;
  p->dy = dy;
  p->has_scroll = (uint8_t)(dx != 0 || dy != 0);
  return SEGA_MOD_OK;
}

int sms_mod_present_sprite_pattern(SmsModPresentation *p, uint32_t index, const uint8_t rows[32]) {
  if (index >= SEGA_SMS_SPRITE_PATTERNS || rows == NULL) return SEGA_MOD_E_RANGE;
  memcpy(&p->patterns[index * 32U], rows, 32U);
  return SEGA_MOD_OK;
}

int sms_mod_present_sprite_colors(SmsModPresentation *p, const uint8_t cram[16]) {
  uint32_t i;
  if (cram == NULL) return SEGA_MOD_E_RANGE;
  for (i = 0U; i < 16U; ++i)
    if (cram[i] > 63U) return SEGA_MOD_E_RANGE;
  memcpy(p->sprite_colours, cram, 16U);
  return SEGA_MOD_OK;
}

int sms_mod_present_sprite_add(SmsModPresentation *p, int32_t x, int32_t y, uint32_t width_cells, uint32_t height_cells, uint32_t tile, uint32_t flags) {
  SmsModSprite *s;
  if (!coord_ok(x) || !coord_ok(y) || width_cells < 1U || width_cells > 4U || height_cells < 1U || height_cells > 4U || tile >= SEGA_SMS_SPRITE_PATTERNS ||
      tile + width_cells * height_cells > SEGA_SMS_SPRITE_PATTERNS || (flags & ~(SEGA_SMS_SPRITE_HFLIP | SEGA_SMS_SPRITE_VFLIP)) != 0U)
    return SEGA_MOD_E_RANGE;
  if (p->sprite_count >= SEGA_SMS_SPRITES_MAX) return SEGA_MOD_E_BUDGET;
  s = &p->sprites[p->sprite_count++];
  s->x = x;
  s->y = y;
  s->w = (uint8_t)width_cells;
  s->h = (uint8_t)height_cells;
  s->flags = (uint8_t)flags;
  s->tile = (uint16_t)tile;
  return SEGA_MOD_OK;
}

static void put_rgb(uint8_t *dst, uint32_t rgb) {
  dst[0] = (uint8_t)(rgb >> 16U);
  dst[1] = (uint8_t)(rgb >> 8U);
  dst[2] = (uint8_t)rgb;
}

void sms_mod_present_compose(const SmsModPresentation *p, const uint8_t *framebuffer, uint32_t height, uint8_t *rgb) {
  const int32_t width = (int32_t)SEGA_SMS_SCREEN_WIDTH;
  int32_t x, y, lines;
  uint32_t i;
  if (height > SEGA_SMS_SCREEN_MAX_HEIGHT) height = SEGA_SMS_SCREEN_MAX_HEIGHT;
  lines = (int32_t)height;
  for (y = 0; y < lines; ++y)
    for (x = 0; x < width; ++x) {
      const int32_t sx = p != NULL ? x - p->dx : x, sy = p != NULL ? y - p->dy : y;
      uint32_t colour = 0U;
      if (sx >= 0 && sx < width && sy >= 0 && sy < lines) {
        const uint8_t v = (uint8_t)(framebuffer[sy * width + sx] & 63U);
        colour = p != NULL && p->mapped[v] != 0U ? p->colour[v] : sms_render_rgb888(v);
      }
      put_rgb(&rgb[(y * width + x) * 3], colour);
    }
  if (p == NULL) return;
  for (i = 0U; i < p->sprite_count; ++i) {
    const SmsModSprite *s = &p->sprites[i];
    const int32_t pw = 8 * (int32_t)s->w, ph = 8 * (int32_t)s->h;
    for (y = 0; y < ph; ++y)
      for (x = 0; x < pw; ++x) {
        const int32_t tx = (s->flags & SEGA_SMS_SPRITE_HFLIP) != 0U ? pw - 1 - x : x, ty = (s->flags & SEGA_SMS_SPRITE_VFLIP) != 0U ? ph - 1 - y : y;
        const int32_t px = s->x + x, py = s->y + y;
        const uint32_t pattern = (uint32_t)s->tile + (uint32_t)(ty / 8) * s->w + (uint32_t)(tx / 8);
        const uint8_t pair = p->patterns[pattern * 32U + (uint32_t)(ty % 8) * 4U + (uint32_t)(tx % 8) / 2U];
        const uint8_t index = (uint8_t)((tx % 2) == 0 ? pair >> 4U : pair & 15U);
        if (index == 0U || px < 0 || py < 0 || px >= width || py >= lines) continue;
        put_rgb(&rgb[(py * width + px) * 3], sms_render_rgb888(p->sprite_colours[index]));
      }
  }
  if (p->has_overlay == 0U) return;
  for (y = 0; y < lines; ++y)
    for (x = 0; x < width; ++x) {
      const uint8_t *o = &p->overlay[(y * width + x) * 4];
      uint8_t *d = &rgb[(y * width + x) * 3];
      const uint32_t a = o[3];
      uint32_t c;
      if (a == 0U) continue;
      for (c = 0U; c < 3U; ++c) d[c] = (uint8_t)((o[c] * a + d[c] * (255U - a) + 127U) / 255U);
    }
}
