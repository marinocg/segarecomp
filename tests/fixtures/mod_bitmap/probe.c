/* SEG-152-T005/T006 (ADR 0320 decisions 6 and 7): a FABRICATED exact-frame ABI 2 provider that draws bitmap art into the fixed overlay and a clipped positioned bitmap layer, sets the ABI 1 fallback
 * position and declares a placement. The test marks (text edits) turn it into the refusing variant: `@@REFUSE@@` declines the HUD, `@@SPRITE@@` submits one authored virtual sprite (an independent component). */
#include <segarecomp_mod_genesis.h>

#include "bitmap_art.h"

static uint8_t fixed_art[40U * 16U * 4U], layer_art[64U * 48U * 4U];
static GenesisVirtualSpriteSet vs;

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void provider_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;
  const SegaGenesisRenderApi *r = f->render;
  const SegaGenesisFrameView *v = f->frame;
  unsigned c;
  (void)state;
  if (host->phase != SEGA_MOD_PHASE_RENDER || r == NULL || v == NULL) return;
  art_fill(fixed_art, 40U, 16U, 160U, 0U);
  art_fill(layer_art, 64U, 48U, 256U, 1U);
  (void)r->overlay_fallback(30U, 60U);
  (void)r->overlay_bitmap(0, 0, 40U, 16U, fixed_art, 160U);
  (void)r->overlay_layer_bitmap(1U, 2U, 0U, -4, 100, 64U, 48U, 0U);
  (void)r->overlay_target(1U);
  (void)r->overlay_bitmap(0, 0, 64U, 48U, layer_art, 256U);
  (void)r->overlay_layer_clip(1U, 8U, 8U, 32U, 24U);
  (void)r->overlay_target(0U);
  if (v->side > 0U) (void)r->view_left(v->side / 2U, v->side);
  /*@@SPRITE@@*/
  /*@@REFUSE@@*/
  (void)c;
  (void)vs;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 1U, SEGA_MOD_PLATFORM, "bitmap-probe", 1U, provider_init, provider_phase, NULL, NULL, NULL,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &provider;
}
