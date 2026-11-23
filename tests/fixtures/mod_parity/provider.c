/* SEG-152-T008: the ABI 2 provider of the viewer-level parity gate (minor 2: exact-frame view, bitmap submission, refusal, per-row generic mask), written to the same logic (parity_world.h, bitmap_art.h) as the ABI 1 twin
 * (twin_abi1.c). FABRICATED. The test applies text edits for the falsification mutants (marks @@STALE@@, @@SKIP@@, @@NODECLINE@@, @@BIG@@). */
#include <segarecomp_mod_genesis.h>

#include "bitmap_art.h"
#include "parity_world.h"

#define COLS 112U
#define ROWS 28U

static uint16_t words[COLS * ROWS], blank[COLS * ROWS];
static uint8_t fixed_art[40U * 16U * 4U], layer_art[64U * 48U * 4U];
static uint8_t big_art[257U * 16U * 4U];

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void provider_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;
  const SegaGenesisRenderApi *r;
  const SegaGenesisFrameView *v;
  uint32_t left, c, row;
  int32_t hs = 0, camera, x0;
  (void)state;
  (void)art_fill;
  (void)fixed_art;
  (void)layer_art;
  (void)big_art;   /* shared header helpers and the mutant buffer stay referenced in every variant */
  (void)vdp_word;
  (void)floor8;
  if (host->phase != SEGA_MOD_PHASE_RENDER || f == NULL || f->render == NULL || f->frame == NULL || f->vdp == NULL) return;
  /*@@SKIP@@*/
  r = f->render;
  v = f->frame;
#if DECLINE == 2
  /*@@NODECLINE@@*/ (void)r->decline(SEGA_GEN_COMPONENT_HUD | SEGA_GEN_COMPONENT_SCENERY, SEGA_GEN_REASON_UNSUPPORTED_SCENE);
  return;
#endif
#if DECLINE == 1
  /*@@NODECLINE@@*/ (void)r->decline(SEGA_GEN_COMPONENT_HUD, SEGA_GEN_REASON_BAD_STATE);
#endif
  (void)r->scene("main", 4U);
  if (v->side > 0U) {
    if (f->vdp->read_hscroll(v->vram, v->lines[0].registers, 0U, 0U, &hs) != 0) return;
    camera = -hs / 8;   /*@@STALE@@*/
    left = v->side / LEFT_DIV;
    x0 = camera - (int32_t)(left / 8U) - 3;
    for (row = 0U; row < ROWS; ++row)
      for (c = 0U; c < COLS; ++c) words[row * COLS + c] = world_word(0U, x0 + (int32_t)c);
    (void)r->scenery_window(0U, x0, 0, COLS, ROWS, words);
    (void)r->scenery_window(1U, -(int32_t)(left / 8U) - 3, 0, COLS, ROWS, blank);
    if (MASK_FROM < 224) {   /* plane A rows from MASK_FROM are answered by the VDP's own frame-start word (the ABI 1 own_plane rows) */
      uint8_t mask[28] = {0};
      for (row = MASK_FROM; row < 224U; ++row) mask[row >> 3U] = (uint8_t)(mask[row >> 3U] | (1U << (row & 7U)));
      (void)r->scenery_generic_rows(0U, mask, 224U, 28U);
    }
    (void)r->view_left(left, v->side);
  }
#if DECLINE != 1
  art_fill(fixed_art, 40U, 16U, 160U, 0U);
  art_fill(layer_art, 64U, 48U, 256U, 1U);
  (void)r->overlay_fallback(30U, 60U);
  (void)r->overlay_bitmap(0, 0, 40U, 16U, fixed_art, 160U);
  (void)r->overlay_layer_bitmap(1U, 2U, 0U, -4, 100, 64U, 48U, 0U);
  (void)r->overlay_target(1U);
  (void)r->overlay_bitmap(0, 0, 64U, 48U, layer_art, 256U);
  (void)r->overlay_layer_clip(1U, 8U, 8U, 32U, 24U);
  (void)r->overlay_target(0U);
  /*@@BIG@@*/
#endif
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 2U, SEGA_MOD_PLATFORM, "parity-gate", 1U, provider_init, provider_phase, NULL, NULL, NULL,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &provider;
}
