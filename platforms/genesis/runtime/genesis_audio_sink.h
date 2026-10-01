/* SEG-032-T009: the host audio sink contract (plain C, no dependencies), implemented by the SDL3 adapter and by test fakes. */
#ifndef SEGARECOMP_GENESIS_AUDIO_SINK_H
#define SEGARECOMP_GENESIS_AUDIO_SINK_H

#include <stdint.h>

typedef struct GenesisAudioSink {
  void *context;
  uint32_t (*queued_frames)(void *context);                         /* frames waiting in the host queue */
  int (*put)(void *context, const int16_t *frames, uint32_t count); /* interleaved L,R; 0 = accepted */
} GenesisAudioSink;

#endif
