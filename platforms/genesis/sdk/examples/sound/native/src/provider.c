/* SEG-145-T008: the C11 twin of ../luau/scripts/main.luau (same inputs, same commands, same log lines). Sound only through the package's declared mappings. */
#include <segarecomp_mod_genesis.h>
#include <stddef.h>
#include <stdint.h>

typedef struct State {
  uint32_t held;
  char last[SEGA_MOD_AUDIO_NAME_BYTES];
} State;
static State g_state;

static const SegaModAudioApi *audio_of(const SegaModHost *host) {
  const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
  if (host->platform_features_size < offsetof(SegaGenesisFeatures, audio) + sizeof features->audio) return 0;
  return features->audio;
}

static int init(void *state, const SegaModHost *host) {
  (void)state;
  return audio_of(host) != 0 ? 0 : 1;
}

static void phase(void *state, const SegaModHost *host) {
  State *s = (State *)state;
  const SegaModAudioApi *audio = audio_of(host);
  if (audio == 0) return;
  if (host->phase == SEGA_MOD_PHASE_POST_INPUT) {
    uint32_t pad = 0U, pressed;
    if (host->input_get(1U, &pad) != SEGA_MOD_OK) return;
    pressed = pad & ~s->held;
    s->held = pad;
    if ((pressed & 0x20U) != 0U) (void)audio->play_sfx("drum");
    if ((pressed & 0x08U) != 0U) (void)audio->play_sfx("chime");
    if (host->frame == 24U) (void)audio->play_music("alt");
    if (host->frame == 44U) (void)audio->stop_music();
  } else if (host->phase == SEGA_MOD_PHASE_FRAME_END) {
    char now[SEGA_MOD_AUDIO_NAME_BYTES];
    uint32_t i = 0U;
    int same = 1;
    if (audio->read_event("accepted", now) != SEGA_MOD_OK) return;
    for (i = 0U; i < SEGA_MOD_AUDIO_NAME_BYTES; ++i) {
      if (now[i] != s->last[i]) same = 0;
      if (now[i] == '\0') break;
    }
    if (!same) {
      static const char prefix[] = "accepted ";
      char line[64];
      uint32_t n = 0U;
      for (i = 0U; prefix[i] != '\0'; ++i) line[n++] = prefix[i];
      for (i = 0U; i < SEGA_MOD_AUDIO_NAME_BYTES && now[i] != '\0'; ++i) line[n++] = now[i];
      for (i = 0U; i < SEGA_MOD_AUDIO_NAME_BYTES; ++i) s->last[i] = now[i];
      host->log(line, n);
    }
  }
}

static int save_state(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  const State *s = (const State *)state;
  uint32_t i;
  if (cap < 1U + SEGA_MOD_AUDIO_NAME_BYTES) return 1;
  out[0] = (uint8_t)s->held;
  for (i = 0U; i < SEGA_MOD_AUDIO_NAME_BYTES; ++i) out[1U + i] = (uint8_t)s->last[i];
  *len = 1U + SEGA_MOD_AUDIO_NAME_BYTES;
  return 0;
}

static int load_state(void *state, const uint8_t *in, uint32_t len) {
  State *s = (State *)state;
  uint32_t i;
  if (len != 1U + SEGA_MOD_AUDIO_NAME_BYTES || in[len - 1U] != 0U) return 1;
  s->held = in[0];
  for (i = 0U; i < SEGA_MOD_AUDIO_NAME_BYTES; ++i) s->last[i] = (char)in[1U + i];
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 4U, SEGA_MOD_PLATFORM, "sound-native-example", 1U, init, phase, NULL, save_state, load_state, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  *state_out = &g_state;
  return &provider;
}
