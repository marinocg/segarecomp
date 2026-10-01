/*
 * SEG-032-T006 (ADR 0072, contract sections 9 and 11): the Genesis sound devices as attached machine parts.
 *
 * `GenesisAudio` owns the one Sega PSG (`Sn76489`, libs/device/sega/psg) that BOTH CPUs write: the 68000 at `$C00011/13/15/17`
 * through the runtime's PSG port and the Z80 at `$7F11/13/15/17` through the Z80 view (z80_machine.c). Both reach it through
 * `runtime->audio_hooks`, so there is one device, one clock and one write order. Time is the guest master clock: the PSG runs on the
 * Z80 clock (master / 15), a write at master time T is applied after every chip tick that ends at or before `T / 15`, and the device
 * clock is monotonic (a write timestamped earlier than the device clock is applied at the device clock, contract section 9).
 * The YM2612 (libs/device/sega/ym2612: vendored ymfm behind a C ABI) is the second device, reached by both CPUs the same way; one YM clock
 * is 7 master ticks and its host-driven timers and busy flag run on that clock. The mixer joins in SEG-032-T009.
 *
 * Plain C11; compiled only into programs that carry sound (the runtime itself never includes this header).
 */
#ifndef SEGARECOMP_GENESIS_AUDIO_H
#define SEGARECOMP_GENESIS_AUDIO_H

#include "runtime.h"
#include "segarecomp/device/sega/psg/sn76489.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"

#define GENESIS_AUDIO_TRACE_CAPACITY 64U
#define GENESIS_AUDIO_YM_TRACE_CAPACITY 512U
#define GENESIS_PSG_CLOCK_DIVIDER 15U /* master ticks per PSG input clock (= the Z80 clock) */

typedef struct GenesisAudio {
  Sn76489 psg;
  Ym2612 *ym;                    /* the YM2612 (NULL only if creation failed) */
  GenesisAudioHooks hooks;       /* installed into runtime->audio_hooks by genesis_audio_attach */
  GenesisRuntime *runtime;
  uint64_t psg_writes;           /* bytes delivered to the device from either CPU */
  uint64_t psg_data_before_latch;/* bytes the device ignored because no register was latched yet */
  uint64_t ym_writes;            /* YM2612 port writes delivered from either CPU */
  uint64_t ym_samples;           /* native stereo samples generated so far */
  uint64_t ym_sample_fnv;        /* FNV-1a 64 over the native sample stream (determinism evidence) */
  /* Bounded diagnostic tap (determinism evidence, never audio input): the first GENESIS_AUDIO_TRACE_CAPACITY PSG deliveries in
   * arrival order, with the guest time each carried. */
  uint32_t trace_count;
  uint64_t trace_ticks[GENESIS_AUDIO_TRACE_CAPACITY];
  uint8_t trace_bytes[GENESIS_AUDIO_TRACE_CAPACITY];
  /* The same for the YM2612: port writes in arrival order (`port` 0xFF = a chip reset). */
  uint32_t ym_trace_count;
  uint64_t ym_trace_ticks[GENESIS_AUDIO_YM_TRACE_CAPACITY];
  uint8_t ym_trace_port[GENESIS_AUDIO_YM_TRACE_CAPACITY];
  uint8_t ym_trace_value[GENESIS_AUDIO_YM_TRACE_CAPACITY];
} GenesisAudio;

/* Initializes the PSG (power-on state, the library's default Sega variant) and installs the hooks. */
void genesis_audio_attach(GenesisAudio *audio, GenesisRuntime *runtime);

/* Releases the YM2612. */
void genesis_audio_detach(GenesisAudio *audio);

/* Runs the YM2612 to guest time `master_ticks` (no access). */
void genesis_audio_ym_run_to(GenesisAudio *audio, uint64_t master_ticks);

/* Runs the PSG to guest time `master_ticks` (no write); used by the mixer and by tests. */
void genesis_audio_psg_run_to(GenesisAudio *audio, uint64_t master_ticks);

/* Canonical PSG state serialization (determinism evidence). */
void genesis_audio_psg_state(const GenesisAudio *audio, uint8_t out[SN76489_STATE_BYTES]);

#endif
