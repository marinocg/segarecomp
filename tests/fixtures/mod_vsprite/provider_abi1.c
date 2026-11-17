/* A FABRICATED ABI 1 native provider for the virtual-sprite viewer tests (SEG-140-T008, ADR 0306). It is test infrastructure, not an example of good provider style: it reads its case from the environment
 * (host code; the approval of the exact package is the boundary), so one built program can exercise many submissions. It submits only virtual sprites (no fixed HUD, no layers).
 * Strict C11 against the public SDK header; getenv/strcmp are the only library calls. */
#include <segarecomp_genesis_patch.h>

#include <stdlib.h>
#include <string.h>

static const char *kase;
static unsigned frame;
static int side; /* the extension width per side the test run expects (VSPRITE_SIDE): positions at the outer boundaries are derived from it */

static void pattern_solid(GenesisVirtualSpriteSet *s, unsigned tile, unsigned colour) {
  unsigned i;
  for (i = 0U; i < 32U; ++i) s->authored_vram[tile * 32U + i] = (uint8_t)(colour << 4 | colour);
}
static void pattern_checker(GenesisVirtualSpriteSet *s, unsigned tile, unsigned a, unsigned b) { /* the fixture's own checker: even rows a,b,a,b..., odd rows b,a,b,a... */
  unsigned row, col;
  for (row = 0U; row < 8U; ++row)
    for (col = 0U; col < 4U; ++col) s->authored_vram[tile * 32U + row * 4U + col] = (row % 2U == 0U) ? (uint8_t)(a << 4 | b) : (uint8_t)(b << 4 | a);
}
static void pattern_corners(GenesisVirtualSpriteSet *s, unsigned tile) { /* colour 1 at the top-left pixel, colour 2 at the bottom-right pixel, the rest transparent */
  memset(&s->authored_vram[tile * 32U], 0, 32U);
  s->authored_vram[tile * 32U] = 0x10U;
  s->authored_vram[tile * 32U + 31U] = 0x02U;
}
static void colour(GenesisVirtualSpriteSet *s, unsigned line, unsigned entry, unsigned word) {
  s->authored_cram[line * 32U + entry * 2U] = (uint8_t)(word >> 8);
  s->authored_cram[line * 32U + entry * 2U + 1U] = (uint8_t)word;
}
static GenesisVirtualSprite *add(GenesisVirtualSpriteSet *s, int provenance, int layer, int x, int y, int w, int h, unsigned tile, int palette) {
  GenesisVirtualSprite *c = &s->sprites[s->count++];
  c->provenance = (uint8_t)provenance;
  c->layer = (uint8_t)layer;
  c->width_cells = (uint8_t)w;
  c->height_cells = (uint8_t)h;
  c->palette = (uint8_t)palette;
  c->tile_index = (uint16_t)tile;
  c->x = (int16_t)x;
  c->y = (int16_t)y;
  return c;
}
#define AUTH GENESIS_VSPRITE_MOD_AUTHORED
#define VERIF GENESIS_VSPRITE_GAME_STATE_VERIFIED
#define BEHIND GENESIS_VSPRITE_LAYER_BEHIND_PLANES
#define BETWEEN GENESIS_VSPRITE_LAYER_BETWEEN
#define ABOVE GENESIS_VSPRITE_LAYER_ABOVE_ALL

static void prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  (void)state;
  out->accepted = (f->requested & GENESIS_COMPAT_HUD) != 0U ? GENESIS_COMPAT_HUD : 0U;
  if (kase == NULL) {
    const char *e = getenv("VSPRITE_SIDE");
    kase = getenv("VSPRITE_CASE");
    if (kase == NULL) kase = "none";
    side = e != NULL ? atoi(e) : 0;
  }
}

static int scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  (void)state; (void)plane; (void)row; (void)x; (void)hscroll; (void)vscroll; (void)word;
  return 0;
}

static int hud(void *state, GenesisCompatOverlay *out) {
  GenesisVirtualSpriteSet *s = out->vsprites;
  const unsigned n = frame++;
  (void)state;
  if (s == NULL || strcmp(kase, "none") == 0) return 0;
  pattern_solid(s, 1U, 1U);   /* the authored pool: pattern 1 solid colour 1, pattern 2 solid colour 2 (the fixture's own pattern 2 is a checker) */
  pattern_solid(s, 2U, 2U);
  pattern_corners(s, 3U);
  pattern_solid(s, 6U, 1U); pattern_solid(s, 7U, 2U); pattern_checker(s, 8U, 1U, 2U); pattern_solid(s, 9U, 1U);   /* a 2x2 block: column 0 = 6 over 7, column 1 = 8 over 9 */
  colour(s, 0U, 1U, 0x0E0EU); /* magenta */
  colour(s, 0U, 2U, 0x00EEU); /* yellow */
  if (strcmp(kase, "authored") == 0) {
    add(s, AUTH, ABOVE, 200, 150, 1, 1, 1U, 0);
    add(s, AUTH, ABOVE, 216, 150, 1, 1, 2U, 0);
  } else if (strcmp(kase, "both") == 0 || strcmp(kase, "verified_gated") == 0) {
    add(s, AUTH, ABOVE, 200, 150, 1, 1, 2U, 0);   /* authored tile 2 = the pool's solid colour 2, whatever VRAM holds */
    add(s, VERIF, ABOVE, 216, 150, 1, 1, 2U, 0);  /* verified tile 2 = the frame's own VRAM pattern (the checker) with the frame's own palette */
  } else if (strcmp(kase, "layers") == 0) {
    add(s, AUTH, BEHIND, 20, 12, 1, 1, 1U, 0);
    add(s, AUTH, BETWEEN, 60, 4, 1, 1, 1U, 0);
    add(s, AUTH, ABOVE, 100, 4, 1, 1, 1U, 0);
  } else if (strcmp(kase, "budget_limit") == 0 || strcmp(kase, "budget_extended") == 0) {
    s->budget_mode = strcmp(kase, "budget_extended") == 0 ? (uint8_t)GENESIS_VSPRITE_BUDGET_EXTENDED : (uint8_t)GENESIS_VSPRITE_BUDGET_LIMIT;
    add(s, AUTH, ABOVE, 200, 96, 1, 1, 1U, 0);    /* rows 96-103 already carry the hardware sprites: no per-line slot is left in LIMIT mode */
    add(s, AUTH, ABOVE, 200, 150, 1, 1, 1U, 0);   /* a free row: drawn in both modes */
  } else if (strcmp(kase, "flip") == 0) {
    add(s, AUTH, ABOVE, 40, 150, 1, 1, 3U, 0);
    { GenesisVirtualSprite *c = add(s, AUTH, ABOVE, 60, 150, 1, 1, 3U, 0); c->h_flip = 1U; }
    { GenesisVirtualSprite *c = add(s, AUTH, ABOVE, 80, 150, 1, 1, 3U, 0); c->v_flip = 1U; }
    { GenesisVirtualSprite *c = add(s, AUTH, ABOVE, 100, 150, 1, 1, 3U, 0); c->h_flip = 1U; c->v_flip = 1U; }
    add(s, AUTH, ABOVE, 140, 150, 2, 2, 6U, 0);   /* column-major 2x2 block */
  } else if (strcmp(kase, "clip") == 0) {
    { GenesisVirtualSprite *c = add(s, AUTH, ABOVE, 200, 150, 2, 1, 1U, 0); c->has_clip = 1U; c->clip_x = 204; c->clip_y = 150; c->clip_w = 8U; c->clip_h = 4U; }
  } else if (strcmp(kase, "transparent") == 0) {
    add(s, AUTH, ABOVE, 200, 150, 1, 1, 5U, 0);   /* pool pattern 5 was never written: fully transparent */
  } else if (strcmp(kase, "fully_clipped") == 0) {
    GenesisVirtualSprite *c = add(s, AUTH, ABOVE, 200, 150, 1, 1, 1U, 0);
    c->has_clip = 1U; c->clip_x = 0; c->clip_y = 0; c->clip_w = 4U; c->clip_h = 4U;
  } else if (strcmp(kase, "occluded") == 0) {
    add(s, AUTH, BEHIND, 20, 0, 1, 1, 1U, 0);     /* wholly over low-priority plane pixels: behind them */
  } else if (strcmp(kase, "limit_dropped") == 0) {
    add(s, AUTH, ABOVE, 200, 96, 1, 1, 1U, 0);
  } else if (strcmp(kase, "lifetime") == 0) {
    if (n % 2U == 0U) add(s, AUTH, ABOVE, 200, 150, 1, 1, 1U, 0);
  } else if (strncmp(kase, "wide_", 5) == 0) {   /* SEG-140-T009: the PRESENTATION space, positions in the native picture's coordinates (x < 0 left of it, x >= 320 right of it) */
    s->space = (uint8_t)GENESIS_VSPRITE_SPACE_PRESENTATION;
    if (strcmp(kase, "wide_left") == 0) add(s, AUTH, ABOVE, -24, 150, 1, 1, 1U, 0);
    else if (strcmp(kase, "wide_right") == 0) add(s, AUTH, ABOVE, 336, 150, 1, 1, 2U, 0);
    else if (strcmp(kase, "wide_straddle") == 0) { add(s, AUTH, ABOVE, -4, 150, 1, 1, 1U, 0); add(s, AUTH, ABOVE, 316, 170, 1, 1, 2U, 0); }
    else if (strcmp(kase, "wide_outer") == 0) {   /* flush with the outer boundary of the presented surface, and partly beyond it (clipped there, never moved) */
      add(s, AUTH, ABOVE, -side, 150, 1, 1, 1U, 0);
      add(s, AUTH, ABOVE, 320 + side - 8, 150, 1, 1, 2U, 0);
      add(s, AUTH, ABOVE, -side - 4, 180, 1, 1, 1U, 0);
      add(s, AUTH, ABOVE, 320 + side - 4, 180, 1, 1, 2U, 0);
    } else if (strcmp(kase, "wide_layers_above") == 0) { add(s, AUTH, ABOVE, -4, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_layers_between") == 0) { add(s, AUTH, ABOVE, 100, 150, 1, 1, 1U, 0); add(s, AUTH, BETWEEN, -4, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_layers_behind") == 0) { add(s, AUTH, BEHIND, 316, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_layers_classes") == 0) {   /* ADR 0312: margin sprites against the three priority classes of the plane A rows (0-7 low, 8-15 high, the rest backdrop) */
      add(s, AUTH, BETWEEN, -24, 4, 1, 1, 1U, 0);       /* left margin, rows 4..11: drawn on the low rows 4..7, hidden by the high rows 8..11 */
      add(s, AUTH, BETWEEN, 336, 6, 1, 1, 1U, 0);       /* right margin, rows 6..13 */
      add(s, AUTH, BEHIND, -40, 4, 1, 1, 1U, 0);        /* left margin, rows 4..11: behind the planes everywhere here, so never visible */
      add(s, AUTH, BEHIND, 330, 12, 1, 1, 1U, 0);       /* right margin, rows 12..19: hidden on the high rows 12..15, drawn on the backdrop rows 16..19 */
      add(s, AUTH, ABOVE, 348, 6, 1, 1, 1U, 0);         /* right margin, above everything */
    }
    else if (strcmp(kase, "wide_layers_inside") == 0) { add(s, AUTH, BEHIND, 20, 150, 1, 1, 1U, 0); add(s, AUTH, BETWEEN, 60, 150, 1, 1, 2U, 0); add(s, AUTH, ABOVE, -8, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_picture_space") == 0) { s->space = (uint8_t)GENESIS_VSPRITE_SPACE_PICTURE; add(s, AUTH, ABOVE, -4, 150, 1, 1, 1U, 0); add(s, AUTH, ABOVE, 316, 170, 1, 1, 2U, 0); }
    else if (strcmp(kase, "wide_budget_limit") == 0 || strcmp(kase, "wide_budget_extended") == 0) {
      s->budget_mode = strcmp(kase, "wide_budget_extended") == 0 ? (uint8_t)GENESIS_VSPRITE_BUDGET_EXTENDED : (uint8_t)GENESIS_VSPRITE_BUDGET_LIMIT;
      add(s, AUTH, ABOVE, -24, 96, 1, 1, 1U, 0);    /* rows 96-103 carry the 20 hardware sprites: no slot is left whether the sprite is in the picture or beside it */
      add(s, AUTH, ABOVE, 336, 150, 1, 1, 1U, 0);
    } else if (strcmp(kase, "wide_verified") == 0) { add(s, VERIF, ABOVE, 330, 150, 1, 1, 2U, 0); add(s, AUTH, ABOVE, -24, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_clip") == 0) { GenesisVirtualSprite *c = add(s, AUTH, ABOVE, -12, 150, 2, 1, 1U, 0); c->has_clip = 1U; c->clip_x = -6; c->clip_y = 150; c->clip_w = 10U; c->clip_h = 4U; }
    else if (strcmp(kase, "wide_bad_space") == 0) { s->space = 2U; add(s, AUTH, ABOVE, -24, 150, 1, 1, 1U, 0); }
    else if (strcmp(kase, "wide_lifetime") == 0) { if (n % 2U == 0U) add(s, AUTH, ABOVE, -24, 150, 1, 1, 1U, 0); }
  } else if (strncmp(kase, "bad_", 4) == 0) {
    GenesisVirtualSprite *c;
    add(s, AUTH, ABOVE, 100, 150, 1, 1, 1U, 0);   /* a valid command first: it must not be drawn when the set is rejected */
    c = add(s, AUTH, ABOVE, 120, 150, 1, 1, 1U, 0);
    if (strcmp(kase, "bad_size") == 0) c->width_cells = 5U;
    else if (strcmp(kase, "bad_provenance") == 0) c->provenance = 3U;
    else if (strcmp(kase, "bad_tile") == 0) c->tile_index = 2048U;
    else if (strcmp(kase, "bad_palette") == 0) c->palette = 4U;
    else if (strcmp(kase, "bad_layer") == 0) c->layer = 3U;
    else if (strcmp(kase, "bad_position") == 0) c->x = 2000;
    else if (strcmp(kase, "bad_clip") == 0) { c->has_clip = 1U; c->clip_w = 0U; c->clip_h = 4U; }
    else if (strcmp(kase, "bad_reserved") == 0) c->flags = 0x80U;
    else if (strcmp(kase, "bad_budget") == 0) s->budget_mode = 2U;
    else if (strcmp(kase, "bad_too_many") == 0) s->count = 65U;
  }
  return 1;
}

static const GenesisNativeCompat table = {GENESIS_NATIVE_COMPAT_ABI, "vsprite.synthetic", 1U, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SPRITES, prepare, scenery_word, hud};
static int storage;

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  *state_out = &storage;
  return &table;
}
