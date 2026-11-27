/* Master System gameplay example, native C11 form (SEG-147-T011, ADR 0325): the SAME rules and the SAME presentation as ../../gameplay/scripts/main.luau, through the same host services. A test requires
 * the two programs to produce identical guest state, write witness and presented pictures. A native provider is trusted code compiled into the program; it is not sandboxed. */
#include <segarecomp_mod_sms.h>

#define SCORE 0x10U
#define POSITION 0x12U
#define SPEED 0x16U
#define SKY 0x30U /* the game's backdrop colour value */

typedef struct State {
  uint32_t running, run_frames;
} State;

static State g_state;

/* Appends `text`, or the decimal form of `value`, to a bounded line (the SDK include allowlist has no <stdio.h>). */
static void append(char line[64], uint32_t *at, const char *text) {
  while (*text != '\0' && *at < 63U) line[(*at)++] = *text++;
  line[*at] = '\0';
}
static void append_number(char line[64], uint32_t *at, uint32_t value) {
  char digits[11];
  uint32_t i = 10U;
  digits[10] = '\0';
  do {
    digits[--i] = (char)('0' + value % 10U);
    value /= 10U;
  } while (value != 0U);
  append(line, at, &digits[i]);
}

static void draw_text(const SegaSmsRenderApi *r, int32_t y, const char *text, uint32_t length, uint32_t rgba) { (void)r->overlay_text(8, y, text, length, rgba); }

static void render(const State *s, const SegaModHost *host) {
  const SegaMasterSystemFeatures *features = (const SegaMasterSystemFeatures *)host->platform_features;
  const SegaSmsRenderApi *r = features->render;
  uint32_t score = 0U, position = 0U;
  char line[64];
  uint32_t at = 0U;
  if (r == NULL || host->read(SEGA_SMS_SPACE_Z80_WORK_RAM, SCORE, 2U, &score) != SEGA_MOD_OK || host->read(SEGA_SMS_SPACE_Z80_WORK_RAM, POSITION, 2U, &position) != SEGA_MOD_OK) return;
  (void)r->overlay_rect(4, 4, 150, 28, 0x000000c0U);
  append(line, &at, "SCORE ");
  append_number(line, &at, score);
  append(line, &at, "  POS ");
  append_number(line, &at, position);
  draw_text(r, 5, line, at, 0xffffffffU);
  at = 0U;
  append(line, &at, s->running != 0U ? "RUN " : "WALK ");
  append_number(line, &at, s->run_frames);
  draw_text(r, 18, line, at, 0xffff40ffU);
  if (s->running != 0U) {
    static const uint8_t colours[16] = {0x00U, 0x0FU};
    static const uint8_t arrow[32] = {0x00, 0x01, 0x10, 0x00, 0x00, 0x01, 0x11, 0x00, 0x11, 0x11, 0x11, 0x10, 0x11, 0x11, 0x11, 0x11,
                                      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x10, 0x00, 0x01, 0x11, 0x00, 0x00, 0x01, 0x10, 0x00};
    (void)r->palette_color(SKY, 0x602000U);
    (void)r->scroll(1, 0);
    (void)r->sprite_colors(colours);
    (void)r->sprite_pattern(0U, arrow);
    (void)r->sprite_add((int32_t)(8U + position % 232U), 96, 1U, 1U, 0U, 0U);
  }
}

static void phase(void *state, const SegaModHost *host) {
  State *s = (State *)state;
  uint32_t pad = 0U;
  switch (host->phase) {
    case SEGA_MOD_PHASE_RESET:
      s->running = 0U;
      s->run_frames = 0U;
      host->log("sms gameplay example ready", 26U);
      break;
    case SEGA_MOD_PHASE_INPUT:
      if (host->input_get(1U, &pad) != SEGA_MOD_OK) return;
      s->running = (pad & SEGA_SMS_BUTTON_2) != 0U ? 1U : 0U;
      if (s->running != 0U) (void)host->input_set(1U, (pad & (0x3FU - SEGA_SMS_BUTTON_2)) | SEGA_SMS_BUTTON_RIGHT);
      break;
    case SEGA_MOD_PHASE_POST_INPUT:
      if (host->write(SEGA_SMS_SPACE_Z80_WORK_RAM, SPEED, 1U, s->running != 0U ? 3U : 1U) != SEGA_MOD_OK) return;
      if (s->running != 0U) ++s->run_frames;
      break;
    case SEGA_MOD_PHASE_RENDER:
      render(s, host);
      break;
    default:
      break;
  }
}

/* The mod's own saved state: two little-endian words. */
static int save_state(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  const State *s = (const State *)state;
  uint32_t i;
  if (cap < 8U) return 1;
  for (i = 0U; i < 4U; ++i) {
    out[i] = (uint8_t)(s->running >> (8U * i));
    out[4U + i] = (uint8_t)(s->run_frames >> (8U * i));
  }
  *len = 8U;
  return 0;
}

static int load_state(void *state, const uint8_t *in, uint32_t len) {
  State *s = (State *)state;
  uint32_t i, running = 0U, frames = 0U;
  if (len != 8U) return 1;
  for (i = 0U; i < 4U; ++i) {
    running |= (uint32_t)in[i] << (8U * i);
    frames |= (uint32_t)in[4U + i] << (8U * i);
  }
  if (running > 1U) return 1;
  s->running = running;
  s->run_frames = frames;
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 5U, SEGA_MOD_PLATFORM, "sms-runner-native", 1U, NULL, phase, NULL, save_state, load_state, {NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &g_state;
  return &provider;
}
