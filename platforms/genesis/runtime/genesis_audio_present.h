/*
 * SEG-032-T009 (ADR 0075, ADR 0070 policy): the host-neutral audio presentation policy shared by the viewer and its tests.
 *
 * It drains the mixer's consumer ring after every viewer slice into a host sink (the SDL3 audio stream). Everything here is a
 * one-way flow from the mixer to the host: it reads no host clock, mutates nothing in the runtime or the devices, and a refusing,
 * failing, muted or absent sink only changes the counters below (the mixer digest never sees it). Policy (ADR 0070):
 *   - queue: at most GENESIS_AUDIO_QUEUE_LIMIT_FRAMES (about 100 ms) may be queued in the host; a chunk that would exceed it is
 *     dropped (overrun, counted);
 *   - underrun: a host queue found empty after the first accepted chunk is counted (the host plays silence meanwhile);
 *   - mute / no sink: the ring is still drained so it never overruns, and the frames are counted as discarded.
 */
#ifndef SEGARECOMP_GENESIS_AUDIO_PRESENT_H
#define SEGARECOMP_GENESIS_AUDIO_PRESENT_H

#include <stdint.h>

#include "genesis_audio_sink.h"
#include "genesis_mixer.h"

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

void genesis_audio_presenter_init(GenesisAudioPresenter *presenter, const GenesisAudioSink *sink, int muted);
/* Drains everything the mixer produced since the last call. */
void genesis_audio_present(GenesisAudioPresenter *presenter, GenesisMixer *mixer);

#endif
