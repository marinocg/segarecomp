/* SEG-152-T005: the ABI 1 twin of the bitmap probe, a FABRICATED ABI 1 native provider writing the SAME art straight into GenesisCompatOverlay (fixed overlay, one clipped layer, fallback) and the same
 * placement, so the end-to-end test compares the real compositor's output for the ABI 2 transport with the ABI 1 transport. Test infrastructure, not an example of provider style. */
#include <segarecomp_genesis_patch.h>

#include <string.h>

#include "bitmap_art.h"

static void prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  (void)state;
  out->accepted = f->requested & GENESIS_COMPAT_HUD;
  if (f->side > 0U) { out->left_px = (int32_t)(f->side / 2U); out->has_left_px = 1U; }
}

static int scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  (void)state; (void)plane; (void)row; (void)x; (void)hscroll; (void)vscroll; (void)word;
  return GENESIS_COMPAT_WORD_GENERIC;
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

static const GenesisNativeCompat table = {GENESIS_NATIVE_COMPAT_ABI, "bitmap-twin", 1U, GENESIS_COMPAT_HUD, prepare, scenery_word, hud};

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &table;
}
