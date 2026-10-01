#include "genesis_audio_present.h"

#include <string.h>

void genesis_audio_presenter_init(GenesisAudioPresenter *presenter, const GenesisAudioSink *sink, int muted) {
  memset(presenter, 0, sizeof(*presenter));
  presenter->sink = sink;
  presenter->muted = muted;
}

void genesis_audio_present(GenesisAudioPresenter *presenter, GenesisMixer *mixer) {
  int16_t chunk[GENESIS_AUDIO_PRESENT_CHUNK_FRAMES * GENESIS_MIXER_CHANNELS];
  uint32_t n;
  while ((n = genesis_mixer_ring_read(mixer, chunk, GENESIS_AUDIO_PRESENT_CHUNK_FRAMES)) != 0U) {
    const GenesisAudioSink *sink = presenter->sink;
    uint32_t queued;
    if (presenter->muted || sink == NULL || sink->put == NULL || sink->queued_frames == NULL) {
      presenter->discarded += n;
      continue;
    }
    queued = sink->queued_frames(sink->context);
    if (queued == 0U && presenter->started) ++presenter->underruns;
    if ((uint64_t)queued + n > GENESIS_AUDIO_QUEUE_LIMIT_FRAMES) {
      presenter->overrun_dropped += n;
      continue;
    }
    if (sink->put(sink->context, chunk, n) != 0) {
      presenter->refused += n;
      continue;
    }
    presenter->started = 1;
    presenter->submitted += n;
  }
}
