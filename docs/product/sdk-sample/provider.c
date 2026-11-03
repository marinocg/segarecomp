/*
 * Sample Segarecomp Genesis native patch provider (a SYNTHETIC "game": project-authored, no commercial content).
 *
 * It shows the whole contract of platforms/genesis/sdk/segarecomp_genesis_patch.h:
 *   - it reads the work-RAM snapshot (here: a 16-bit "world column" counter at RAM offset 0x0100 and a magic at 0x0102),
 *   - it declines every frame whose state it cannot verify (prepare),
 *   - it contributes one scenery word for every off-screen cell (a solid pattern 1 stripe) and a small RGBA overlay.
 * Package it with:  python3 tools/segapatch.py create --source game.bin --id sample --name Sample --version 1.0.0 --out sample.segapatch \
 *                     --native-source provider.c --native-caps frame.work_ram,frame.vdp,scenery.words,overlay.rgba --author "You" --license MIT
 */
#include "segarecomp_genesis_patch.h"

static unsigned g_frames;

static void sample_prepare(void *state, const GenesisCompatFrame *frame, GenesisCompatDecision *out) {
  (void)state;
  ++g_frames;
  out->accepted = 0U;
  out->reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
  if (frame->work_ram == NULL || frame->lines == NULL || frame->line_count == 0U) { out->reason = GENESIS_COMPAT_BAD_STATE; return; }
  if (frame->work_ram[0x0102] != 0x5A) return;                    /* not the screen this provider was written for */
  out->accepted = frame->requested;
  out->reason = GENESIS_COMPAT_OK;
}

static int sample_scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  (void)state; (void)row; (void)x; (void)hscroll; (void)vscroll;
  *word = plane == GENESIS_COMPAT_PLANE_B ? 1U : 0U;   /* pattern 1, palette line 0, no flips; plane A stays blank */
  return 1;
}

static int sample_hud(void *state, GenesisCompatOverlay *out) {
  unsigned i;
  (void)state;
  if (out->capacity < 8U * 8U * 4U) return 0;
  out->width = 8U; out->height = 8U; out->fallback_x = 4U; out->fallback_y = 4U;
  for (i = 0U; i < 64U; ++i) { out->rgba[i * 4U] = 255U; out->rgba[i * 4U + 1U] = 255U; out->rgba[i * 4U + 2U] = 255U; out->rgba[i * 4U + 3U] = 255U; }
  return 1;
}

static const GenesisNativeCompat k_provider = {GENESIS_NATIVE_COMPAT_ABI, "sample.synthetic", 1U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, sample_prepare, sample_scenery_word, sample_hud};

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  *state_out = NULL;
  return &k_provider;
}
