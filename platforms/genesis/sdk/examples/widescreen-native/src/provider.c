/* Widescreen + gameplay native example (ABI 2, strict C11; SEG-149-T001). The "game" (fixture_rom.py) keeps, as big-endian words of work RAM: the camera (0xFF0012; plane A scrolls by -(camera * 8)), an actor's
 * world x (0xFF0014) and a power value (0xFF0016, 0..5). This ONE provider:
 *   - GAMEPLAY (changes the game): when button C is pressed (edge) while the power is a verified in-game value (1..4), power is restored to 5 through the declared read-write range;
 *   - SCENERY (presentation only): authors the off-screen name-table cells beside the picture as a per-frame window (typed `scenery_window`, capability scenery.words);
 *   - ACTOR (presentation only): one virtual sprite at the actor's screen x (world x - camera * 8), which crosses the right edge of the picture into the widescreen margin;
 *   - HUD (presentation only): a small "P+" panel counting restorations.
 * Everything read comes from declared ranges at frame-end; render uses only what frame-end stored (nothing live). */
#include <segarecomp_mod_genesis.h>

#define CAMERA 0x12U
#define ACTOR 0x14U
#define POWER 0x16U
#define POWER_MAX 5U
#define C_BUTTON 32U
#define COLS 128U   /* the window's widest allowed width: the 32:9 margins need camera-30 .. camera+69; the rest is slack for a camera that moved since the frame-end read */
#define ROWS 28U

static uint32_t camera, actor, restored, previous_c;
static uint16_t words[COLS * ROWS], blank[COLS * ROWS];
static GenesisVirtualSpriteSet sprites;

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void submit_scenery(const SegaGenesisRenderApi *render) {
  const int32_t x0 = (int32_t)camera - 48;
  uint32_t r, c;
  for (r = 0U; r < ROWS; ++r)
    for (c = 0U; c < COLS; ++c) {
      const int32_t cell = x0 + (int32_t)c;
      words[r * COLS + c] = (cell >= 24 && cell < 64) ? 1U : ((cell & 1) == 0 ? 0x2002U : 0x2003U);   /* a pure function of the world cell */
    }
  (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);
  (void)render->scenery_window(1U, -48, 0, COLS, ROWS, blank);   /* plane B never scrolls in this fixture (its own scroll is 0): blank cells over every column the margins can ask for */
}

static void submit_actor(const SegaGenesisRenderApi *render) {
  GenesisVirtualSprite *s = &sprites.sprites[0];
  const int32_t x = (int32_t)actor - 8 * (int32_t)camera;
  uint32_t i;
  for (i = 0U; i < 8U; ++i) {   /* tile 1: solid colour 1 of the authored palette line 0 */
    sprites.authored_vram[32U + 4U * i] = 0x11U;
    sprites.authored_vram[32U + 4U * i + 1U] = 0x11U;
    sprites.authored_vram[32U + 4U * i + 2U] = 0x11U;
    sprites.authored_vram[32U + 4U * i + 3U] = 0x11U;
  }
  sprites.authored_cram[2] = 0x0EU;   /* colour 1 = 0x0E0E */
  sprites.authored_cram[3] = 0x0EU;
  sprites.space = 1U;   /* presentation space: may draw beside the picture */
  sprites.count = 1U;
  s->provenance = 2U;   /* mod-authored */
  s->layer = 2U;        /* above everything */
  s->width_cells = 1U;
  s->height_cells = 1U;
  s->tile_index = 1U;
  s->x = (int16_t)x;
  s->y = 150;
  (void)render->virtual_sprites(&sprites);
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_RESET) {
    camera = actor = restored = previous_c = 0U;
  } else if (host->phase == SEGA_MOD_PHASE_POST_INPUT) {
    uint32_t pad = 0U, power = 0U;
    const uint32_t c_now = host->input_get(1U, &pad) == 0 && (pad / C_BUTTON) % 2U == 1U ? 1U : 0U;
    if (c_now != 0U && previous_c == 0U && host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, POWER, 2U, &power) == 0 && power >= 1U && power < POWER_MAX) {   /* edge, and a verified in-game value */
      if (host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, POWER, 2U, POWER_MAX) == 0) ++restored;
    }
    previous_c = c_now;
  } else if (host->phase == SEGA_MOD_PHASE_FRAME_END) {
    uint32_t value = 0U;
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, CAMERA, 2U, &value) == 0) camera = value;
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, ACTOR, 2U, &value) == 0) actor = value;
  } else if (host->phase == SEGA_MOD_PHASE_RENDER) {
    const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
    const SegaGenesisRenderApi *render;
    if (features == NULL || features->size < sizeof(SegaGenesisFeatures) || features->render == NULL) return;
    render = features->render;
    (void)render->scene("main", 4U);
    submit_scenery(render);
    submit_actor(render);
    (void)render->overlay_rect(0, 0, 24, 8, 0x000000C0U);
    (void)render->overlay_text(2, 0, "P+", 2U, 0xFFD040FFU);
    if (restored != 0U) (void)render->overlay_rect(0, 8, (int32_t)(restored % 24U + 1U), 2, 0xFFD040FFU);
  }
}

static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  uint32_t i;
  const uint32_t values[4] = {camera, actor, restored, previous_c};
  (void)state;
  if (cap < 16U) return 1;
  for (i = 0U; i < 16U; ++i) out[i] = (uint8_t)(values[i / 4U] >> (24U - 8U * (i % 4U)));
  *len = 16U;
  return 0;
}

static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  uint32_t values[4], i;
  (void)state;
  if (len != 16U) return 1;
  for (i = 0U; i < 4U; ++i) values[i] = ((uint32_t)in[4U * i] << 24U) | ((uint32_t)in[4U * i + 1U] << 16U) | ((uint32_t)in[4U * i + 2U] << 8U) | (uint32_t)in[4U * i + 3U];
  camera = values[0];
  actor = values[1];
  restored = values[2];
  previous_c = values[3];
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "widescreen-gameplay-example", 1U, provider_init, provider_phase, NULL, provider_save, provider_load,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &camera;
  return &provider;
}
