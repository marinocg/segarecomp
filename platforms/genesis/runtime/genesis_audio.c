#include "genesis_audio.h"

#include <string.h>

/* Runs every whole PSG tick that ends at or before `master_ticks` (tick j starts at j x 240 master ticks); a time in the past does
 * nothing, which is the monotonic device-time clamp of contract section 9. Each tick's level goes to the mixer. */
static void psg_advance(GenesisAudio *audio, uint64_t master_ticks) {
  const uint64_t target = (master_ticks / GENESIS_PSG_CLOCK_DIVIDER) / audio->psg.config.divider;
  while (audio->psg.ticks < target) {
    const uint64_t start = audio->psg.ticks * GENESIS_MIXER_PSG_TICK_MASTER;
    genesis_mixer_push_psg(&audio->mixer, start, sn76489_tick(&audio->psg, NULL));
  }
}

static int audio_psg_write(void *context, GenesisRuntime *runtime, uint8_t value, uint64_t master_ticks) {
  GenesisAudio *audio = (GenesisAudio *)context;
  (void)runtime;
  psg_advance(audio, master_ticks);
  if (sn76489_write(&audio->psg, value) == SN76489_WRITE_DATA_BEFORE_LATCH) ++audio->psg_data_before_latch;
  if (audio->trace_count < GENESIS_AUDIO_TRACE_CAPACITY) {
    audio->trace_ticks[audio->trace_count] = master_ticks;
    audio->trace_bytes[audio->trace_count] = value;
    ++audio->trace_count;
  }
  ++audio->psg_writes;
  return 1;
}

static void ym_sink(void *context, uint64_t start_master_ticks, int32_t left, int32_t right) {
  GenesisAudio *audio = (GenesisAudio *)context;
  uint32_t bytes[2];
  unsigned i;
  genesis_mixer_push_ym(&audio->mixer, start_master_ticks, left, right);
  bytes[0] = (uint32_t)left;
  bytes[1] = (uint32_t)right;
  for (i = 0U; i < 8U; ++i) audio->ym_sample_fnv = (audio->ym_sample_fnv ^ ((const uint8_t *)bytes)[i]) * UINT64_C(0x100000001b3);
  ++audio->ym_samples;
}

static void ym_trace(GenesisAudio *audio, uint64_t ticks, uint8_t port, uint8_t value) {
  if (audio->ym_trace_count >= GENESIS_AUDIO_YM_TRACE_CAPACITY) return;
  audio->ym_trace_ticks[audio->ym_trace_count] = ticks;
  audio->ym_trace_port[audio->ym_trace_count] = port;
  audio->ym_trace_value[audio->ym_trace_count] = value;
  ++audio->ym_trace_count;
}

static int audio_ym_read(void *context, GenesisRuntime *runtime, uint32_t port, uint64_t master_ticks, uint8_t *value) {
  GenesisAudio *audio = (GenesisAudio *)context;
  (void)runtime;
  *value = ym2612_read(audio->ym, port, master_ticks);
  return 1;
}

static int audio_ym_write(void *context, GenesisRuntime *runtime, uint32_t port, uint8_t value, uint64_t master_ticks) {
  GenesisAudio *audio = (GenesisAudio *)context;
  (void)runtime;
  ym2612_write(audio->ym, port, value, master_ticks);
  ym_trace(audio, master_ticks, (uint8_t)port, value);
  ++audio->ym_writes;
  if ((port & 1U) == 0U) {
    audio->ym_latched[(port >> 1) & 1U] = value;
    ++audio->ym_class_address;
  } else {
    const uint8_t reg = audio->ym_latched[(port >> 1) & 1U];
    if (port == 1U && reg == 0x2AU) ++audio->ym_class_dac;
    else if (port == 1U && reg == 0x2BU) ++audio->ym_class_dac_enable;
    else if (port == 1U && reg == 0x28U) ++audio->ym_class_key;
    else if (port == 1U && reg >= 0x24U && reg <= 0x27U) ++audio->ym_class_timer;
    else if (reg >= 0x30U) ++audio->ym_class_operator;
    else ++audio->ym_class_global;
  }
  return 1;
}

static void audio_ym_reset(void *context, GenesisRuntime *runtime, uint64_t master_ticks) {
  GenesisAudio *audio = (GenesisAudio *)context;
  (void)runtime;
  ym2612_reset(audio->ym, master_ticks);
  ym_trace(audio, master_ticks, 0xFFU, 0U);
}

void genesis_audio_sync(GenesisAudio *audio, uint64_t master_ticks) {
  if (audio->ym != NULL) ym2612_advance(audio->ym, master_ticks);
  psg_advance(audio, master_ticks);
}

static void audio_sync_hook(void *context, GenesisRuntime *runtime, uint64_t master_ticks) {
  (void)runtime;
  genesis_audio_sync((GenesisAudio *)context, master_ticks);
}

void genesis_audio_attach(GenesisAudio *audio, GenesisRuntime *runtime) {
  Sn76489Config config;
  memset(audio, 0, sizeof(*audio));
  genesis_mixer_init(&audio->mixer);
  audio->runtime = runtime;
  config = sn76489_default_config();
  (void)sn76489_init(&audio->psg, &config);
  sn76489_reset(&audio->psg);
  audio->ym = ym2612_create();
  audio->ym_sample_fnv = UINT64_C(0xcbf29ce484222325);
  if (audio->ym != NULL) ym2612_set_sink(audio->ym, ym_sink, audio);
  audio->hooks.context = audio;
  audio->hooks.psg_write = audio_psg_write;
  audio->hooks.ym_read = audio_ym_read;
  audio->hooks.ym_write = audio_ym_write;
  audio->hooks.ym_reset = audio_ym_reset;
  audio->hooks.sync = audio_sync_hook;
  runtime->audio_hooks = &audio->hooks;
}

void genesis_audio_detach(GenesisAudio *audio) {
  if (audio->runtime != NULL && audio->runtime->audio_hooks == &audio->hooks) audio->runtime->audio_hooks = NULL;
  ym2612_destroy(audio->ym);
  audio->ym = NULL;
}

void genesis_audio_ym_run_to(GenesisAudio *audio, uint64_t master_ticks) { ym2612_advance(audio->ym, master_ticks); }

void genesis_audio_psg_run_to(GenesisAudio *audio, uint64_t master_ticks) { psg_advance(audio, master_ticks); }

void genesis_audio_psg_state(const GenesisAudio *audio, uint8_t out[SN76489_STATE_BYTES]) {
  sn76489_state_bytes(&audio->psg, out);
}
