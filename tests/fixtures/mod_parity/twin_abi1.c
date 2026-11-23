/* SEG-152-T008: the ABI 1 twin of the viewer-level parity gate: a FABRICATED ABI 1 native provider written to the same logic (parity_world.h, bitmap_art.h) as the ABI 2 provider (provider.c). It answers scenery with
 * the registered world (and the VDP's own frame-start word on the rows from MASK_FROM: the own_plane shape of the real providers), draws the bitmap art straight into GenesisCompatOverlay, declares the placement and
 * refuses like the logic says. Test infrastructure, not an example of provider style. */
#include <segarecomp_genesis_patch.h>

#include <string.h>

#include "bitmap_art.h"
#include "parity_world.h"

static const uint8_t *frame_vram;

static void prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  (void)state;
  frame_vram = f->vram;
#if DECLINE == 2
  out->accepted = 0U;
  out->reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
  return;
#else
  out->accepted = f->requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY);
#if DECLINE == 1
  out->accepted &= ~(unsigned)GENESIS_COMPAT_HUD;
  out->reason = GENESIS_COMPAT_BAD_STATE;   /* the refusal's diagnostic (only reported when nothing else is accepted, as the same-logic ABI 2 decline reports it) */
#endif
  memcpy(out->scene, "main", 5U);
  if (f->side > 0U) { out->left_px = (int32_t)(f->side / LEFT_DIV); out->has_left_px = 1U; }
#endif
}

static int scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  const int cx = floor8(x - hscroll), cy = floor8((int)row + vscroll);
  (void)state;
  if (plane > 1U) return 0;
  if (plane == 0U && (int)row >= MASK_FROM) { *word = vdp_word(frame_vram, cx, cy); return 1; }   /* own_plane: the VDP's word of the frame-start VRAM, as a provider word */
  *word = world_word(plane, cx);
  return 1;
}

static int hud(void *state, GenesisCompatOverlay *out) {
  static uint8_t fixed_art[40U * 16U * 4U];
  unsigned y;
  GenesisCompatLayer *l;
  (void)state;
  art_fill(fixed_art, 40U, 16U, 160U, 0U);
  for (y = 0U; y < 16U; ++y) memcpy(out->rgba + y * 128U * 4U, fixed_art + y * 160U, 160U);
  out->width = 128U;
  out->height = 256U;
  out->fallback_x = 30U;
  out->fallback_y = 60U;
  art_fill(out->layer_rgba, 64U, 48U, 256U, 1U);
  out->layer_count = 1U;
  l = &out->layers[0];
  memset(l, 0, sizeof *l);
  l->width = 64U; l->height = 48U; l->anchor = 2U; l->reference = 0U; l->z = 0U; l->dx = -4; l->dy = 100;
  l->has_clip = 1U; l->clip_x = 8U; l->clip_y = 8U; l->clip_w = 32U; l->clip_h = 24U; l->offset = 0U;
  return 1;
}

static const GenesisNativeCompat table = {GENESIS_NATIVE_COMPAT_ABI, "parity-twin", 1U, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY, prepare, scenery_word, hud};

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &table;
}
