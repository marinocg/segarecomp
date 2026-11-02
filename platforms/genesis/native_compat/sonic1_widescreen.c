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
#define S1_BG2X 0xF710U         /* word: the second background camera (the lower background region is streamed from it) */
#define S1_BG_SPLIT 0xF7F0U     /* word: world y (background layout) where the lower background region starts */
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
  const uint8_t *vram;                    /* the frame's own VRAM (the HUD tiles) and raster state (its palettes): valid while the producer calls hud() */
  const GenesisRasterFrameState *raster;
} Sonic1Frame;

static Sonic1Frame g_frame;

/* Congruent value of `v` (mod `period`) nearest to `ref` within [ref - period/2, ref + period/2). */
static int unwrap(int v, int ref, int period) {
  int d = (v - ref) % period;
  if (d < 0) d += period;
  if (d >= period / 2) d -= period;
  return ref + d;
}

/* The zones and acts the scenery component is validated for: bit `act` of entry `zone` (zone ids: 0 Green Hill, 1 Labyrinth, 2 Marble, 3 Star Light, 4 Spring Yard, 5 Scrap Brain; Labyrinth's
 * act 3 slot is the third Scrap Brain act). Every frame is additionally identity-checked, so an entry only widens the scope; it never relaxes a check. */
static const uint8_t k_scenery_acts[6] = {0x07, 0x00, 0x00, 0x00, 0x00, 0x00};   /* Green Hill acts 1-3 */

int sonic1_scenery_scope(unsigned zone, unsigned act) { return zone < 6U && act < 8U && ((k_scenery_acts[zone] >> act) & 1U) != 0U; }

static int layout_valid(const uint8_t *ram) {
  for (unsigned row = 0U; row < S1_LAYOUT_ROWS; ++row)
    for (unsigned i = 0U; i < 0x80U; ++i)
      if ((ram[S1_LAYOUT + row * 0x80U + i] & 0x7FU) > S1_CHUNK_COUNT) return 0;
  return 1;
}

/* Scenery verification: returns GENESIS_COMPAT_OK or the reason it is declined. */
static unsigned scenery_check(Sonic1Frame *f, const GenesisCompatFrame *frame, const uint8_t *ram) {
  if (!sonic1_scenery_scope(ram[S1_ZONE], ram[S1_ZONE + 1U])) return GENESIS_COMPAT_UNSUPPORTED_SCENE;
  f->camy = (int)u16(ram, S1_CAMY);
  f->bgy = (int)u16(ram, S1_BGY);
  if (frame->height > S1_HSCROLL_LINES || f->camy >= (int)(S1_LAYOUT_ROWS * 256U) || f->bgy >= (int)(S1_LAYOUT_ROWS * 256U) || u16(ram, S1_CAMX) >= S1_WORLD_W || u16(ram, S1_BGX) >= S1_WORLD_W || u16(ram, S1_BG2X) >= S1_WORLD_W)
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
    /* A background band is only defined modulo the 512-pixel plane. The game streams the upper background region from the first background camera and the region below
     * `S1_BG_SPLIT` from the second one, so the band's world position is the alias nearest the camera of ITS region. */
    f->off_bg[l] = unwrap(-ram_b, (int)(((int)l + f->bgy) >= (int)u16(ram, S1_BG_SPLIT) ? u16(ram, S1_BG2X) : u16(ram, S1_BGX)), 512);
  }
  /* Frame identity: the provider's own cells for the part of the world the picture shows must agree with what the VDP nametables hold there (the game drew them). A gross disagreement
   * (a fade, a level transition, another level than the state claims) means RAM and picture are not the same scene: the frame is declined, never half-served. Cells the game has not (yet)
   * streamed (a background band beyond its window, left of the layout) are not compared. */
  {
    unsigned fg_total = 0U, fg_bad = 0U, bg_total = 0U, bg_bad = 0U;
    for (unsigned l = 0U; l < frame->height; l += 8U) {
      const GenesisRasterLine *line = &frame->raster->lines[l];
      int32_t hs[2] = {0, 0};
      (void)genesis_vdp_read_hscroll(frame->vram, line->registers, GENESIS_VDP_PLANE_A, l, &hs[0]);
      (void)genesis_vdp_read_hscroll(frame->vram, line->registers, GENESIS_VDP_PLANE_B, l, &hs[1]);
      for (unsigned plane = 0U; plane < 2U; ++plane) {
        const unsigned base = plane == 0U ? (unsigned)(line->registers[2] & 0x38U) << 10 : (unsigned)(line->registers[4] & 7U) << 13;
        const int off = plane == 0U ? f->off_fg[l] : f->off_bg[l];
        const int ycam = plane == 0U ? f->camy : f->bgy;
        const int cam = plane == 0U ? (int)u16(ram, S1_CAMX) : (((int)l + f->bgy) >= (int)u16(ram, S1_BG_SPLIT) ? (int)u16(ram, S1_BG2X) : (int)u16(ram, S1_BGX));
        for (int x = 0; x < (int)frame->width; x += 8) {
          const int wx = x + off, wy = (int)l + ycam;
          unsigned px = (unsigned)(x - (int)hs[plane]) & 511U, py = (unsigned)(wy) & 255U, vw;
          int w;
          if (wx < 0 || wx < cam - 16 || wx >= cam + 336) continue;   /* outside what the game streams for this plane/region */
          w = sonic1_world_cell(ram, plane, wx / 8, wy / 8);
          vw = ((unsigned)frame->vram[(base + ((py >> 3) * 64U + (px >> 3)) * 2U) & 0xFFFFU] << 8) | frame->vram[(base + ((py >> 3) * 64U + (px >> 3)) * 2U + 1U) & 0xFFFFU];
          if (w < 0) return GENESIS_COMPAT_BAD_STATE;
          if (plane == 0U) { ++fg_total; if ((unsigned)w != vw && !((vw & 0x7FFU) == 0U && ((unsigned)w & 0x7FFU) == 0U)) ++fg_bad; }
          else { ++bg_total; if ((unsigned)w != vw && !((vw & 0x7FFU) == 0U && ((unsigned)w & 0x7FFU) == 0U)) ++bg_bad; }
        }
      }
    }
    if (fg_total == 0U || fg_bad * 10U > fg_total || bg_bad * 7U > bg_total) return GENESIS_COMPAT_MISMATCH;
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
  f->vram = frame->vram;
  f->raster = frame->raster;
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

/* ---------------------------------------------------------------------------------------------------------------- HUD */
/* The game's own HUD, relocated: the patch stops the HUD object queueing its sprites, and this draws exactly the same sprite pieces (same tiles, same palette lines, same rows) from the frame's
 * own VRAM and CRAM, so the font, the layout (SCORE / TIME / RINGS on top, the Sonic icon with the lives at the bottom) and the digits are the original's and there is no panel behind them. Only the
 * horizontal position differs: the host anchors the overlay at the left edge of the visible surface (or at the original place when there is no side). The overlay covers the picture rows 1:1
 * (row y of the overlay is picture row y) and the picture columns from HUD_X0 (the original's left edge). The pieces' geometry (position, size, first tile) is the HUD object's own layout. */

#define HUD_X0 16U
#define HUD_W 96U     /* picture columns 16..111 */
#define HUD_H 224U    /* picture rows 0..223 */

typedef struct HudPiece { uint8_t x, y; uint8_t cells_w, cells_h; uint16_t tile; uint8_t red_group; } HudPiece;   /* red_group: 1 = the TIME label, 2 = the RINGS label */
static const HudPiece k_hud_pieces[] = {
    {16, 8, 4, 2, 0x6CAU, 0}, {48, 8, 4, 2, 0x6E2U, 0}, {80, 8, 4, 2, 0x6EAU, 0},    /* SCORE and its digits */
    {16, 24, 4, 2, 0x6DAU, 1}, {56, 24, 4, 2, 0x6F2U, 0},                              /* TIME and its digits */
    {16, 40, 4, 2, 0x6D2U, 2}, {48, 40, 1, 2, 0x6CAU, 2}, {64, 40, 3, 2, 0x6FAU, 0},  /* RINGS and its digits */
    {16, 200, 2, 2, 0x7D4U, 0}, {32, 200, 4, 2, 0x7D8U, 0},                            /* the Sonic icon, "SONIC x" and the lives */
};

static int sonic1_hud(void *state, GenesisCompatOverlay *o) {
  const Sonic1Frame *f = (const Sonic1Frame *)state;
  /* the object's frame selection: with rings none; otherwise the labels flash red in the flash phase (TIME too at 9 minutes) */
  unsigned red = 0U;
  if ((f->accepted & GENESIS_COMPAT_HUD) == 0U || o->rgba == NULL || f->vram == NULL || f->raster == NULL || o->capacity < (size_t)HUD_W * HUD_H * 4U) return 0;
  if (f->rings == 0U && !f->flash) red = (1U << 2) | (f->minutes == 9U ? (1U << 1) : 0U);   /* bit 2: RINGS label, bit 1: TIME label */
  o->width = (uint16_t)HUD_W;
  o->height = (uint16_t)HUD_H;
  o->fallback_x = (uint16_t)HUD_X0;
  o->fallback_y = 0U;
  memset(o->rgba, 0, (size_t)HUD_W * HUD_H * 4U);   /* fully transparent: no background */
  for (unsigned n = 0U; n < sizeof(k_hud_pieces) / sizeof(k_hud_pieces[0]); ++n) {
    const HudPiece *pc = &k_hud_pieces[n];
    const unsigned pal = ((pc->red_group == 2U && (red & 4U) != 0U) || (pc->red_group == 1U && (red & 2U) != 0U)) ? 1U : 0U;
    for (unsigned cx = 0U; cx < pc->cells_w; ++cx)
      for (unsigned cy = 0U; cy < pc->cells_h; ++cy) {
        const unsigned tile = pc->tile + cx * pc->cells_h + cy;   /* a sprite's cells are numbered down each column */
        for (unsigned row = 0U; row < 8U; ++row) {
          const unsigned py = pc->y + cy * 8U + row;
          const GenesisRasterLine *line;
          if (py >= HUD_H) continue;
          line = &f->raster->lines[py < GENESIS_RASTER_MAX_LINES ? py : GENESIS_RASTER_MAX_LINES - 1U];
          for (unsigned col = 0U; col < 8U; ++col) {
            const unsigned px_ = pc->x + cx * 8U + col;
            const uint8_t byte = f->vram[(tile * 32U + row * 4U + col / 2U) & 0xFFFFU];
            const unsigned index = (col & 1U) == 0U ? (unsigned)(byte >> 4) : (unsigned)(byte & 0x0FU);
            GenesisRgb888 c;
            uint8_t *d;
            if (index == 0U || px_ < HUD_X0 || px_ - HUD_X0 >= HUD_W) continue;
            if (genesis_vdp_decode_cram_entry(line->cram, pal * 16U + index, &c) != 0) continue;
            d = o->rgba + ((size_t)py * HUD_W + (px_ - HUD_X0)) * 4U;
            d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = 255U;
          }
        }
      }
  }
  return 1;
}

static const GenesisNativeCompat k_sonic1 = {GENESIS_NATIVE_COMPAT_ABI, SONIC1_NATIVE_CAPABILITY, SONIC1_NATIVE_IMPLEMENTATION, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY,
                                             sonic1_prepare, sonic1_scenery_word, sonic1_hud};

const GenesisNativeCompat *sonic1_widescreen_provider(void **state_out) {
  if (state_out != NULL) *state_out = &g_frame;
  return &k_sonic1;
}
