/* SEG-140-T011: a native ABI 2 provider that authors the offscreen scenery of the scrolling fixture (scenery_fixture_rom.py). The "game" keeps a camera word at work RAM 0xFF0012 and scrolls plane A by -(camera * 8).
 * Frame-end reads the camera (declared read-only range); render submits one 112 x 28 name-table window per frame, in the unwrapped plane-cell coordinates the host producer queries. Cells 24..63, the
 * ones the program's own plane holds, are tile 1; every other cell is "level data" the original program never loaded: columns alternate tile 2 / tile 3 of palette line 1. */
#include <segarecomp_mod_genesis.h>

#define CAMERA 0x12U
#define COLS 112U
#define ROWS 28U

static uint32_t camera;
static uint16_t words[COLS * ROWS], blank[COLS * ROWS];   /* plane B holds nothing off-screen: an explicit window of blank cells */

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_FRAME_END) {
    uint32_t value = 0U;
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, CAMERA, 2U, &value) == 0) camera = value;
  } else if (host->phase == SEGA_MOD_PHASE_RENDER) {
    const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
    const SegaGenesisRenderApi *render;
    uint32_t r, c;
    int32_t x0;
    if (features == NULL || features->size < sizeof(SegaGenesisFeatures) || features->render == NULL) return;
    render = features->render;
    x0 = (int32_t)camera - 40;
    for (r = 0U; r < ROWS; ++r)
      for (c = 0U; c < COLS; ++c) {
        const int32_t cell = x0 + (int32_t)c;
        const int inside = cell >= 24 && cell < 64;   /* the columns the program's own plane holds: a pure function of the cell, never of the camera */
        words[r * COLS + c] = inside ? ((cell & 1) == 0 ? 1U : 3U) : ((cell & 1) == 0 ? 0x2002U : 0x2003U);
      }
    (void)render->scene("main", 4U);
    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);
    /* plane B does not scroll in the fixture: its cells are asked for at their own, camera-independent coordinates */
    (void)render->scenery_window(1U, -40, 0, COLS, ROWS, blank);
    (void)blank;   /* referenced even by the variants that drop the plane B window */
    (void)render->overlay_rect(0, 0, 24, 8, 0x000000C0U);
  }
}

static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  (void)state;
  if (cap < 4U) return 1;
  out[0] = (uint8_t)(camera >> 24U);
  out[1] = (uint8_t)(camera >> 16U);
  out[2] = (uint8_t)(camera >> 8U);
  out[3] = (uint8_t)camera;
  *len = 4U;
  return 0;
}

static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  (void)state;
  if (len != 4U) return 1;
  camera = ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) | ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "scenery-fixture-native", 1U, provider_init, provider_phase, NULL, provider_save, provider_load,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &camera;
  return &provider;
}
