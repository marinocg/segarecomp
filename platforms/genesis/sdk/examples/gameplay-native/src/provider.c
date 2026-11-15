/* Native gameplay example (ABI 2, strict C11; SEG-138-T011). Rule: while button C is held, every frame adds BONUS to the score the fixture program keeps in work RAM at 0xFF0010 (a typed, bounded,
 * big-endian 16-bit access the manifest declares); a HUD panel shows how many bonuses were granted. The provider keeps that counter, so it saves and restores it (state.mod).
 * It is the exact twin of ../gameplay/scripts/main.luau: the same host services, the same phases, the same overlay requests. */
#include <segarecomp_mod_genesis.h>

#define SCORE 0x10U
#define BONUS 10U
#define C_BUTTON 32U
#define WHITE 0xFFFFFFFFU
#define GOLD 0xFFD040FFU
#define PANEL 0x000000C0U

static uint32_t granted;

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static const SegaGenesisRenderApi *render_api(const SegaModHost *host) {
  const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
  if (features == NULL || features->size < sizeof(SegaGenesisFeatures)) return NULL;
  return features->render;
}

static uint32_t decimal(char *out, uint32_t value) { /* writes the digits of value, returns their count */
  char digits[10];
  uint32_t n = 0U, i;
  do {
    digits[n++] = (char)('0' + value % 10U);
    value /= 10U;
  } while (value != 0U);
  for (i = 0U; i < n; ++i) out[i] = digits[n - 1U - i];
  return n;
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_RESET) {
    static const char ready[] = "gameplay example ready";
    granted = 0U;
    host->log(ready, (uint32_t)(sizeof ready - 1U));
  } else if (host->phase == SEGA_MOD_PHASE_POST_INPUT) {
    uint32_t pad = 0U, score = 0U;
    if (host->input_get(1U, &pad) == 0 && (pad / C_BUTTON) % 2U == 1U) {
      if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, SCORE, 2U, &score) == 0) host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, SCORE, 2U, (score + BONUS) % 65536U);
      ++granted;
    }
  } else if (host->phase == SEGA_MOD_PHASE_RENDER) {
    const SegaGenesisRenderApi *render = render_api(host);
    char counter[12];
    uint32_t n;
    if (render == NULL) return;
    counter[0] = 'x';
    n = 1U + decimal(counter + 1, granted);
    render->overlay_rect(0, 0, 76, 24, PANEL);
    render->overlay_text(4, 3, "BONUS", 5U, WHITE);
    render->overlay_text(4, 12, counter, n, GOLD);
    render->overlay_rect(0, 24, (int32_t)(granted % 76U + 1U), 3, GOLD);
  }
}

static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  (void)state;
  if (cap < 4U) return 1;
  out[0] = (uint8_t)(granted >> 24U);
  out[1] = (uint8_t)(granted >> 16U);
  out[2] = (uint8_t)(granted >> 8U);
  out[3] = (uint8_t)granted;
  *len = 4U;
  return 0;
}

static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  (void)state;
  if (len != 4U) return 1;
  granted = ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) | ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "gameplay-example-native", 1U, provider_init, provider_phase, NULL, provider_save, provider_load,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &granted;
  return &provider;
}
