#include "genesis_audio_present.h"

#include <string.h>

void genesis_audio_present(GenesisAudioPresenter *presenter, GenesisMixer *mixer) {
  int16_t chunk[GENESIS_AUDIO_PRESENT_CHUNK_FRAMES * GENESIS_MIXER_CHANNELS];
  uint32_t n;
  while ((n = genesis_mixer_ring_read(mixer, chunk, GENESIS_AUDIO_PRESENT_CHUNK_FRAMES)) != 0U) genesis_audio_present_chunk(presenter, chunk, n);
}
