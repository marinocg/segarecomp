/* SEG-032-T009 (contract section 11): the frozen audio artifact format, dependency-free so the viewer adapter can use it too.
 * Each value appears exactly once in code. */
#ifndef SEGARECOMP_GENESIS_AUDIO_FORMAT_H
#define SEGARECOMP_GENESIS_AUDIO_FORMAT_H

#include <stdint.h>

#define GENESIS_MIXER_RATE_HZ UINT64_C(44100)
#define GENESIS_MIXER_CHANNELS 2U
#define GENESIS_MIXER_PSG_SCALED_MAX UINT64_C(8192) /* the PSG's unipolar full scale after decimation */
#define GENESIS_PSG_CLOCK_DIVIDER 15U               /* master ticks per PSG input clock (= the Z80 clock) */

#endif
