#include "genesis_audio.h"

#include <string.h>

static uint64_t psg_cycles(uint64_t master_ticks) { return master_ticks / GENESIS_PSG_CLOCK_DIVIDER; }

static int audio_psg_write(void *context, GenesisRuntime *runtime, uint8_t value, uint64_t master_ticks) {
  GenesisAudio *audio = (GenesisAudio *)context;
  (void)runtime;
  /* sn76489_advance does nothing for a time in the past: that is the monotonic device-time clamp of contract section 9. */
  sn76489_advance(&audio->psg, NULL, psg_cycles(master_ticks));
  if (sn76489_write(&audio->psg, value) == SN76489_WRITE_DATA_BEFORE_LATCH) ++audio->psg_data_before_latch;
  if (audio->trace_count < GENESIS_AUDIO_TRACE_CAPACITY) {
    audio->trace_ticks[audio->trace_count] = master_ticks;
    audio->trace_bytes[audio->trace_count] = value;
    ++audio->trace_count;
  }
  ++audio->psg_writes;
  return 1;
}

void genesis_audio_attach(GenesisAudio *audio, GenesisRuntime *runtime) {
  Sn76489Config config;
  memset(audio, 0, sizeof(*audio));
  audio->runtime = runtime;
  config = sn76489_default_config();
  (void)sn76489_init(&audio->psg, &config);
  sn76489_reset(&audio->psg);
  audio->hooks.context = audio;
  audio->hooks.psg_write = audio_psg_write;
  runtime->audio_hooks = &audio->hooks;
}

void genesis_audio_psg_run_to(GenesisAudio *audio, uint64_t master_ticks) {
  sn76489_advance(&audio->psg, NULL, psg_cycles(master_ticks));
}

void genesis_audio_psg_state(const GenesisAudio *audio, uint8_t out[SN76489_STATE_BYTES]) {
  sn76489_state_bytes(&audio->psg, out);
}
