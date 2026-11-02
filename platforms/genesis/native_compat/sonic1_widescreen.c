#include "sonic1_widescreen.h"

#include <stdlib.h>
#include <string.h>

#include "vdp_render.h"

/* Work-RAM map of the verified image (offsets into the 64 KiB work RAM; verified against the unmodified image's level code and a live generated run, class-level). */
#define S1_CHUNKS 0x0000U       /* 256x256 chunk table: 0x200 bytes per chunk (16x16 block words) */
#define S1_CHUNK_COUNT 0x52U    /* 0xA400 / 0x200 */
#define S1_LAYOUT 0xA400U       /* level layout: 8 rows of 0x80 bytes; foreground = first 0x40, background = next 0x40; low 7 bits = chunk index + 1 */
#define S1_LAYOUT_ROWS 8U
#define S1_BLOCKS 0xB000U       /* 16x16 block table: 8 bytes (4 name words: TL TR BL BR) */
#define S1_BLOCK_MAX 0x3FFU
#define S1_GAMEMODE 0xF600U     /* byte: 0x0C running level (0x8C = the level start sequence) */
#define S1_CAMX 0xF700U         /* words: foreground camera x/y */
#define S1_CAMY 0xF704U
#define S1_BGX 0xF708U          /* background camera x/y */
#define S1_BGY 0xF70CU
#define S1_HSCROLL 0xCC00U      /* per-line horizontal scroll buffer the game copies to the VDP at VBlank: 224 x (foreground word, background word), signed, NOT wrapped */
#define S1_HSCROLL_LINES 224U
#define S1_FRAMECOUNT_FLASH 0xFE05U  /* byte: bit 3 is the flash phase the original HUD uses */
#define S1_TIME 0xFE22U         /* long: 0, minutes (binary), seconds (binary), frames (binary) */
#define S1_SCORE 0xFE26U        /* long, binary */
#define S1_ZONE 0xFE10U         /* bytes: zone, act */
#define S1_LIVES 0xFE12U        /* byte */
#define S1_RINGS 0xFE20U        /* word */
#define S1_WORLD_W 0x4000       /* 64 chunks of 256 px */

static unsigned u16(const uint8_t *r, unsigned a) { return ((unsigned)r[a & 0xFFFFU] << 8) | r[(a + 1U) & 0xFFFFU]; }

/* Name-table word of 8x8 cell (cx, cy) of layout `plane` (0 fg, 1 bg). Bounded: every address below is < 0x10000 by construction. */
int sonic1_world_cell(const uint8_t *ram, unsigned plane, int cx, int cy) {
  unsigned x, y, chunk, block_word, block, flip_x, flip_y, col, row, word;
  if (ram == NULL || plane > 1U) return -1;
  if (cx < 0 || cy < 0 || cx >= S1_WORLD_W / 8 || cy >= (int)(S1_LAYOUT_ROWS * 32U)) return 0;   /* outside the level: no world, a blank cell */
  x = (unsigned)cx * 8U;
  y = (unsigned)cy * 8U;
  chunk = ram[S1_LAYOUT + plane * 0x40U + (y >> 8) * 0x80U + (x >> 8)] & 0x7FU;
  if (chunk == 0U) return 0;
  if (chunk > S1_CHUNK_COUNT) return -1;
  block_word = u16(ram, S1_CHUNKS + (chunk - 1U) * 0x200U + (((y & 0xFFU) >> 4) * 16U + ((x & 0xFFU) >> 4)) * 2U);
  block = block_word & S1_BLOCK_MAX;
  flip_x = (block_word >> 11) & 1U;
  flip_y = (block_word >> 12) & 1U;
  col = (x & 15U) >> 3;
  row = (y & 15U) >> 3;
  if (flip_x) col ^= 1U;
  if (flip_y) row ^= 1U;
  word = u16(ram, S1_BLOCKS + block * 8U + (row * 2U + col) * 2U);
  if (flip_x) word ^= 0x0800U;
  if (flip_y) word ^= 0x1000U;
  return (int)word;
}

/* ---------------------------------------------------------------------------------------------------------------- per-frame state */

typedef struct Sonic1Frame {
  const uint8_t *ram;
  unsigned accepted;
  int camy, bgy;
  int off_fg[S1_HSCROLL_LINES], off_bg[S1_HSCROLL_LINES];   /* world x of screen column 0 per output line (exact: the game's own unwrapped scroll) */
  unsigned rings, lives, score, minutes, seconds, flash;
} Sonic1Frame;

static Sonic1Frame g_frame;

/* Congruent value of `v` (mod `period`) nearest to `ref` within [ref - period/2, ref + period/2). */
static int unwrap(int v, int ref, int period) {
  int d = (v - ref) % period;
  if (d < 0) d += period;
  if (d >= period / 2) d -= period;
  return ref + d;
}

static int layout_valid(const uint8_t *ram) {
  for (unsigned row = 0U; row < S1_LAYOUT_ROWS; ++row)
    for (unsigned i = 0U; i < 0x80U; ++i)
      if ((ram[S1_LAYOUT + row * 0x80U + i] & 0x7FU) > S1_CHUNK_COUNT) return 0;
  return 1;
}

/* Scenery verification (Green Hill Zone act 1 only): returns GENESIS_COMPAT_OK or the reason it is declined. */
static unsigned scenery_check(Sonic1Frame *f, const GenesisCompatFrame *frame, const uint8_t *ram) {
  if (ram[S1_ZONE] != 0U || ram[S1_ZONE + 1U] != 0U) return GENESIS_COMPAT_UNSUPPORTED_SCENE;
  f->camy = (int)u16(ram, S1_CAMY);
  f->bgy = (int)u16(ram, S1_BGY);
  if (frame->height > S1_HSCROLL_LINES || f->camy >= (int)(S1_LAYOUT_ROWS * 256U) || f->bgy >= (int)(S1_LAYOUT_ROWS * 256U) || u16(ram, S1_CAMX) >= S1_WORLD_W || u16(ram, S1_BGX) >= S1_WORLD_W)
    return GENESIS_COMPAT_BAD_STATE;
  if (!layout_valid(ram)) return GENESIS_COMPAT_BAD_STATE;
  /* The display must be the verified configuration (64x32 planes, per-line scroll, full-screen vertical scroll) and the scroll the VDP applies (copied from the game's buffer at the
   * previous VBlank) must equal the game's own unwrapped values in work RAM line by line: RAM and VDP then describe the same frame. A level that is still being set up, or any
   * other disagreement, is declined. */
  for (unsigned l = 0U; l < frame->height; ++l) {
    const GenesisRasterLine *line = &frame->raster->lines[l];
    int32_t ha = 0, hb = 0, va = 0, vb = 0;
    const int ram_a = (int16_t)u16(ram, S1_HSCROLL + l * 4U), ram_b = (int16_t)u16(ram, S1_HSCROLL + l * 4U + 2U);
    if ((line->registers[16] & 0x33U) != 0x01U || genesis_vdp_vscroll_mode_is_2cell(line->registers)) return GENESIS_COMPAT_BAD_SCROLL;
    if (genesis_vdp_read_hscroll(frame->vram, line->registers, GENESIS_VDP_PLANE_A, l, &ha) != 0 || genesis_vdp_read_hscroll(frame->vram, line->registers, GENESIS_VDP_PLANE_B, l, &hb) != 0 ||
        genesis_vdp_read_vscroll(line->vsram, line->registers, GENESIS_VDP_PLANE_A, 0U, &va) != 0 || genesis_vdp_read_vscroll(line->vsram, line->registers, GENESIS_VDP_PLANE_B, 0U, &vb) != 0)
      return GENESIS_COMPAT_BAD_SCROLL;
    if ((((int)ha - ram_a) & 0x3FF) != 0 || (((int)hb - ram_b) & 0x3FF) != 0 || (((int)va - f->camy) & 0x3FF) != 0 || (((int)vb - f->bgy) & 0x3FF) != 0) return GENESIS_COMPAT_BAD_SCROLL;
    f->off_fg[l] = -ram_a;                                       /* the foreground scrolls exactly with the camera */
    f->off_bg[l] = unwrap(-ram_b, (int)u16(ram, S1_BGX), 512);   /* a background band is only defined modulo the 512-pixel plane: the alias nearest the background camera */
  }
  return GENESIS_COMPAT_OK;
}

static void sonic1_prepare(void *state, const GenesisCompatFrame *frame, GenesisCompatDecision *out) {
  Sonic1Frame *f = (Sonic1Frame *)state;
  const uint8_t *ram = frame->work_ram;
  unsigned scenery_reason, accepted = 0U;
  memset(f, 0, sizeof(*f));
  out->accepted = 0U;
  out->reason = GENESIS_COMPAT_BAD_STATE;
  if (ram == NULL || frame->raster == NULL || frame->vram == NULL || frame->height == 0U || frame->height > GENESIS_FRAME_MAX_HEIGHT) return;
  /* verified scope: a running level (0x0C). The level start sequence (0x8C: title card over a level that is still being drawn), the title, special stage and every other screen are declined. */
  if (ram[S1_GAMEMODE] != 0x0CU) { out->reason = GENESIS_COMPAT_UNSUPPORTED_SCENE; return; }
  f->rings = u16(ram, S1_RINGS);
  f->lives = ram[S1_LIVES];
  f->score = (unsigned)((u16(ram, S1_SCORE) << 16) | u16(ram, S1_SCORE + 2U));
  f->minutes = ram[S1_TIME + 1U];
  f->seconds = ram[S1_TIME + 2U];
  f->flash = (ram[S1_FRAMECOUNT_FLASH] >> 3) & 1U;
  /* The HUD only needs plausible counters (any zone). The scenery additionally needs the verified Green Hill Zone act 1 world state. */
  if (f->minutes <= 9U && f->seconds <= 59U && f->score <= 999999U) accepted |= GENESIS_COMPAT_HUD;
  scenery_reason = scenery_check(f, frame, ram);
  if (scenery_reason == GENESIS_COMPAT_OK) { accepted |= GENESIS_COMPAT_SCENERY; f->ram = ram; }
  accepted &= frame->requested;
  if ((accepted & GENESIS_COMPAT_SCENERY) == 0U) f->ram = NULL;
  f->accepted = accepted;
  out->accepted = accepted;
  out->reason = (uint8_t)(accepted != 0U ? GENESIS_COMPAT_OK : (scenery_reason != GENESIS_COMPAT_OK ? scenery_reason : GENESIS_COMPAT_BAD_STATE));
}

static int sonic1_scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  const Sonic1Frame *f = (const Sonic1Frame *)state;
  int off, voff, wx, wy, w;
  if (f->ram == NULL || (f->accepted & GENESIS_COMPAT_SCENERY) == 0U || plane > 1U || word == NULL || row >= S1_HSCROLL_LINES) return 0;
  off = plane == 0U ? f->off_fg[row] : f->off_bg[row];               /* world x of screen column 0 on this line (the game's own scroll) */
  voff = unwrap(vscroll, plane == 0U ? f->camy : f->bgy, 256);       /* world y of output line 0 */
  if ((((-hscroll) - off) & 511) != 0) return 0;                    /* the scroll the VDP applied to this line is not the one prepare verified: fail the cell, never guess */
  wx = x + off;
  wy = (int)row + voff;
  w = sonic1_world_cell(f->ram, plane, wx >= 0 ? wx / 8 : -((-wx + 7) / 8), wy >= 0 ? wy / 8 : -((-wy + 7) / 8));
  if (w < 0) return 0;
  *word = (uint16_t)w;
  return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- HUD (3x5 glyphs) */
/* A compact version of the original HUD (yellow labels, white counters, the label turning red while there are no rings or the clock is at 9 minutes, in the original's flash phase),
 * sized for the side strip of a 16:9 picture. The panel is translucent so it reads over any scenery. */

static const uint8_t k_digits[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
                                        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
/* letters used by the labels */
static const uint8_t k_C[5] = {3, 4, 4, 4, 3}, k_E[5] = {7, 4, 6, 4, 7}, k_G[5] = {3, 4, 5, 5, 3}, k_I[5] = {7, 2, 2, 2, 7}, k_M[5] = {5, 7, 7, 5, 5}, k_N[5] = {6, 5, 5, 5, 5}, k_O[5] = {2, 5, 5, 5, 2},
                              k_R[5] = {6, 5, 6, 5, 5}, k_S[5] = {3, 4, 2, 1, 6}, k_T[5] = {7, 2, 2, 2, 2}, k_X[5] = {5, 5, 2, 5, 5}, k_COLON[5] = {0, 2, 0, 2, 0};

#define HUD_W 32U
#define HUD_H 58U

static void px(GenesisCompatOverlay *o, unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  uint8_t *p;
  if (x >= o->width || y >= o->height) return;
  p = o->rgba + ((size_t)y * o->width + x) * 4U;
  p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
static void glyph(GenesisCompatOverlay *o, const uint8_t rows[5], unsigned x, unsigned y, const uint8_t c[3]) {
  for (unsigned j = 0U; j < 5U; ++j)
    for (unsigned i = 0U; i < 3U; ++i)
      if ((rows[j] >> (2U - i)) & 1U) px(o, x + i, y + j, c[0], c[1], c[2], 255U);
}
static void label(GenesisCompatOverlay *o, const uint8_t *const *letters, unsigned count, unsigned y, int red) {
  static const uint8_t yellow[3] = {255, 204, 0}, red_c[3] = {224, 32, 32};
  for (unsigned i = 0U; i < count; ++i) glyph(o, letters[i], 2U + i * 4U, y, red ? red_c : yellow);
}
/* Right-aligned in a field of `digits` cells with the leading zeros left blank (the last digit is always drawn), like the original's counters. */
static void number(GenesisCompatOverlay *o, unsigned value, unsigned digits, unsigned x, unsigned y) {
  static const uint8_t white[3] = {255, 255, 255};
  unsigned div = 1U;
  int started = 0;
  for (unsigned i = 1U; i < digits; ++i) div *= 10U;
  for (unsigned i = 0U; i < digits; ++i, div /= 10U) {
    const unsigned d = (value / div) % 10U;
    if (d != 0U || i + 1U == digits) started = 1;
    if (started) glyph(o, k_digits[d], x + i * 4U, y, white);
  }
}
/* A fixed-width number (the clock's seconds keep their leading zero). */
static void number_zero(GenesisCompatOverlay *o, unsigned value, unsigned digits, unsigned x, unsigned y) {
  static const uint8_t white[3] = {255, 255, 255};
  unsigned div = 1U;
  for (unsigned i = 1U; i < digits; ++i) div *= 10U;
  for (unsigned i = 0U; i < digits; ++i, div /= 10U) glyph(o, k_digits[(value / div) % 10U], x + i * 4U, y, white);
}

static int sonic1_hud(void *state, GenesisCompatOverlay *o) {
  const Sonic1Frame *f = (const Sonic1Frame *)state;
  static const uint8_t white[3] = {255, 255, 255};
  const unsigned rings = f->rings > 999U ? 999U : f->rings, lives = f->lives > 99U ? 99U : f->lives, score = f->score > 999999U ? 9999990U : f->score * 10U;   /* the game keeps the score divided by ten; the original shows it with the trailing zero */
  if ((f->accepted & GENESIS_COMPAT_HUD) == 0U || o->rgba == NULL || o->capacity < (size_t)HUD_W * HUD_H * 4U) return 0;
  o->width = (uint16_t)HUD_W;
  o->height = (uint16_t)HUD_H;
  o->fallback_x = 4U;
  o->fallback_y = 4U;
  for (unsigned y = 0U; y < HUD_H; ++y)
    for (unsigned x = 0U; x < HUD_W; ++x) px(o, x, y, 0U, 0U, 24U, 150U);   /* translucent panel */
  {
    const uint8_t *const score_l[5] = {k_S, k_C, k_O, k_R, k_E}, *const time_l[4] = {k_T, k_I, k_M, k_E}, *const rings_l[5] = {k_R, k_I, k_N, k_G, k_S}, *const lives_l[5] = {k_S, k_O, k_N, k_I, k_C};
    label(o, score_l, 5U, 3U, 0);
    number(o, score, 7U, 2U, 9U);
    label(o, time_l, 4U, 16U, f->minutes == 9U && f->flash);                 /* the original flashes the label red at 9 minutes */
    number(o, f->minutes, 1U, 2U, 22U);
    glyph(o, k_COLON, 6U, 22U, white);
    number_zero(o, f->seconds, 2U, 10U, 22U);
    label(o, rings_l, 5U, 29U, rings == 0U && f->flash);                      /* ... and the RINGS label while there are none */
    number(o, rings, 3U, 2U, 35U);
    label(o, lives_l, 5U, 43U, 0);
    glyph(o, k_X, 2U, 50U, white);
    number(o, lives, 2U, 8U, 50U);
  }
  return 1;
}

static const GenesisNativeCompat k_sonic1 = {GENESIS_NATIVE_COMPAT_ABI, SONIC1_NATIVE_CAPABILITY, SONIC1_NATIVE_IMPLEMENTATION, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY,
                                             sonic1_prepare, sonic1_scenery_word, sonic1_hud};

const GenesisNativeCompat *sonic1_widescreen_provider(void **state_out) {
  if (state_out != NULL) *state_out = &g_frame;
  return &k_sonic1;
}
