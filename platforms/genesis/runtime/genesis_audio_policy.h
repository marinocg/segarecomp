/* SEG-104: the mixer-free half of the audio presentation policy (ADR 0070, ADR 0075): the presenter state and the per-chunk queue, underrun and
 * mute rules for any one-way producer of interleaved L,R S16 frames at the mixer's rate (the Genesis mixer ring, and the Sega CD CD-DA stream).
 * Header-only and dependent on nothing but the sink contract, so a viewer that has no mixer can use it unchanged. */
#ifndef SEGARECOMP_GENESIS_AUDIO_POLICY_H
#define SEGARECOMP_GENESIS_AUDIO_POLICY_H

#include <stdint.h>
#include <string.h>

#include "genesis_audio_sink.h"

#define GENESIS_AUDIO_QUEUE_LIMIT_FRAMES 4410U
#define GENESIS_AUDIO_PRESENT_CHUNK_FRAMES 1024U

typedef struct GenesisAudioPresenter {
  const GenesisAudioSink *sink; /* NULL = no audio device */
  int muted;
  int started;
  uint64_t submitted;       /* frames accepted by the sink */
  uint64_t overrun_dropped; /* frames dropped because the host queue was full */
  uint64_t refused;         /* frames the sink refused */
  uint64_t discarded;       /* frames drained while muted or without a sink */
  uint64_t underruns;       /* times the host queue was found empty after the first accepted chunk */
} GenesisAudioPresenter;

/* SEG-104: the same queue / underrun / mute policy for one chunk of interleaved L,R frames from any one-way producer (the Sega CD CD-DA
 * stream uses it); genesis_audio_present applies it to every chunk the mixer ring yields. Header-inline so a viewer needs no further source. */
static inline void genesis_audio_present_chunk(GenesisAudioPresenter *presenter, const int16_t *chunk, uint32_t n) {
  const GenesisAudioSink *sink = presenter->sink;
  uint32_t queued;
  if (presenter->muted || sink == NULL || sink->put == NULL || sink->queued_frames == NULL) {
    presenter->discarded += n;
    return;
  }
  queued = sink->queued_frames(sink->context);
  if (queued == 0U && presenter->started) ++presenter->underruns;
  if ((uint64_t)queued + n > GENESIS_AUDIO_QUEUE_LIMIT_FRAMES) {
    presenter->overrun_dropped += n;
    return;
  }
  if (sink->put(sink->context, chunk, n) != 0) {
    presenter->refused += n;
    return;
  }
  presenter->started = 1;
  presenter->submitted += n;
}

static inline void genesis_audio_presenter_init(GenesisAudioPresenter *presenter, const GenesisAudioSink *sink, int muted) {
  memset(presenter, 0, sizeof(*presenter));
  presenter->sink = sink;
  presenter->muted = muted;
}

#endif
