#ifndef SEGARECOMP_DEVICE_SEGA_PSG_SN76489_H
#define SEGARECOMP_DEVICE_SEGA_PSG_SN76489_H

#include <stddef.h>
#include <stdint.h>

/* Sega integrated SN76489-family PSG and its deterministic PCM stream (SEG-009-T007; machine contract section 10).
 *
 * Platform-neutral plain C11: no platform, CPU or runtime dependency. Time is an absolute master-clock count supplied by
 * the caller (for the Master System, Z80 T-states); the device owns only the internal clock division.
 *
 * Model. One chip tick = `divider` master clocks (16). `sn76489_advance(chip, cycles)` executes every whole tick that
 * ends at or before `cycles` (tick j covers [j x divider, (j + 1) x divider)); a register write is applied between
 * ticks, so the result depends only on the write timestamps, never on how a host slices execution. After each tick the
 * mixer level is `sum over channels of (output ? level[attenuation] : 0)` (0..131,068 with the default table).
 *
 * Tone channel: 10-bit counter; on a tick `if counter == 0 or --counter == 0 { counter = period; output ^= 1 }`, so
 * periods 0 and 1 both toggle every tick (project decision U10, departing from SMS Power!'s "constant +1").
 * Noise channel: same counter with reload $10/$20/$40 or the tone 2 register (rate 3); the LFSR shifts on the 0 -> 1
 * flip-flop edge (every second expiry); white noise feeds parity(lfsr & taps) into the top bit, periodic feeds bit 0;
 * the noise output is bit 0 after the shift (U4); any noise-register write reloads the LFSR seed. Variant parameters
 * exist only where a public difference exists: LFSR width/taps (16 bit, taps $0009 for the Sega chip; 15 bit, taps
 * $0003 for the TI original). */
#ifdef __cplusplus
extern "C" {
#endif

#define SN76489_CHANNELS 4
#define SN76489_TONE_CHANNELS 3
#define SN76489_DEFAULT_DIVIDER 16u
#define SN76489_DEFAULT_LFSR_BITS 16u
#define SN76489_DEFAULT_TAPS 0x0009u
#define SN76489_FULL_SCALE 131068u /* 4 x level[0]: the largest tick level with the default table */
#define SN76489_STATE_BYTES 34u

/* The SMS Power! integer attenuation table: 2 dB per step, index 15 = silence. */
extern const uint16_t sn76489_default_levels[16];

typedef struct Sn76489Config {
  uint16_t noise_taps;    /* white-noise feedback taps over the LFSR (bit 0 = LSB) */
  uint8_t lfsr_bits;      /* LFSR width, 2..16 */
  uint8_t divider;        /* master clocks per chip tick, >= 1 */
  const uint16_t *levels; /* 16 tick levels by attenuation (must outlive the chip) */
} Sn76489Config;

Sn76489Config sn76489_default_config(void);

typedef enum Sn76489WriteResult {
  SN76489_WRITE_OK = 0,
  SN76489_WRITE_DATA_BEFORE_LATCH = 1 /* a data byte with no preceding latch byte: state unchanged (U5) */
} Sn76489WriteResult;

typedef struct Sn76489 {
  Sn76489Config config;
  uint16_t tone_period[SN76489_TONE_CHANNELS];
  uint16_t tone_counter[SN76489_TONE_CHANNELS];
  uint8_t tone_output[SN76489_TONE_CHANNELS];
  uint8_t attenuation[SN76489_CHANNELS];
  uint8_t noise_control; /* bits 1-0 rate, bit 2 white */
  uint16_t noise_counter;
  uint8_t noise_flip;
  uint16_t lfsr;
  int8_t latch; /* -1 none yet; else channel x 2 + (1 = attenuation) */
  uint64_t ticks; /* chip ticks executed since reset */
} Sn76489;

/* ---- PCM stream ----------------------------------------------------------------------------------------------- */

/* Deterministic box-filter decimation of the tick levels to 16-bit PCM (integer arithmetic only). Output sample k
 * starts at master clock T_k = ceil(k x rate_num / rate_den) and covers every chip tick whose start lies in
 * [T_k, T_(k+1)); its value is `floor(floor(sum / count) x 65,535 / full_scale) - 32,768`. `k x rate_num` must fit in
 * 64 bits. */
typedef struct Sn76489PcmConfig {
  uint64_t rate_num;
  uint64_t rate_den;
  uint32_t full_scale;
} Sn76489PcmConfig;

/* Master System: 44,100 Hz against the 39,375,000 / 11 Hz CPU clock: rate_num/rate_den = 39,375,000 / 485,100. */
Sn76489PcmConfig sn76489_pcm_config_44100_sms(void);

/* Whole completed samples, in order, `count` of them starting at absolute sample index `first_index`. */
typedef void (*Sn76489PcmSink)(void *context, uint64_t first_index, const int16_t *samples, uint32_t count);

#define SN76489_PCM_BATCH 64u

typedef struct Sn76489Pcm {
  Sn76489PcmConfig config;
  Sn76489PcmSink sink;
  void *sink_context;
  uint64_t index;      /* absolute index of the sample being accumulated */
  uint64_t boundary;   /* T_(index + 1) */
  uint64_t sum;
  uint32_t count;
  uint32_t last_mean;
  uint64_t batch_first;
  uint32_t batch_count;
  int16_t batch[SN76489_PCM_BATCH];
} Sn76489Pcm;

void sn76489_pcm_init(Sn76489Pcm *pcm, const Sn76489PcmConfig *config, Sn76489PcmSink sink, void *sink_context);
void sn76489_pcm_reset(Sn76489Pcm *pcm);
/* T_k in master clocks. */
uint64_t sn76489_pcm_sample_start(const Sn76489PcmConfig *config, uint64_t k);
/* Delivers batched samples to the sink now (samples are otherwise delivered in batches of SN76489_PCM_BATCH). */
void sn76489_pcm_flush(Sn76489Pcm *pcm);

/* ---- device --------------------------------------------------------------------------------------------------- */

/* Returns 0 when the configuration is unusable (width outside 2..16, zero divider, NULL levels). */
int sn76489_init(Sn76489 *chip, const Sn76489Config *config);
/* Power-on / machine reset state: tone periods and noise control 0, attenuations $F, counters and outputs 0, LFSR seed
 * `1 << (lfsr_bits - 1)`, no register latched (U5), time 0. */
void sn76489_reset(Sn76489 *chip);
/* Runs every whole tick ending at or before `cycles` (absolute master clocks since reset); `pcm` may be NULL. A time
 * in the past does nothing. */
void sn76489_advance(Sn76489 *chip, Sn76489Pcm *pcm, uint64_t cycles);
/* Runs one tick and returns its mixer level (also delivered to `pcm`). */
uint32_t sn76489_tick(Sn76489 *chip, Sn76489Pcm *pcm);
/* Applies a data-port byte at the current tick position (call `sn76489_advance` to the write's timestamp first). */
Sn76489WriteResult sn76489_write(Sn76489 *chip, uint8_t value);
/* Mixer level of the current channel outputs. */
uint32_t sn76489_level(const Sn76489 *chip);
/* Canonical little-endian state serialization for digests. */
void sn76489_state_bytes(const Sn76489 *chip, uint8_t out[SN76489_STATE_BYTES]);

/* ---- consumer ring -------------------------------------------------------------------------------------------- */

/* A fixed-capacity FIFO of whole samples for a host consumer (T009). Usable directly as a `Sn76489PcmSink`. When the
 * consumer falls behind the OLDEST samples are dropped and counted in `dropped`; guest state never depends on it. */
typedef struct Sn76489Ring {
  int16_t *data;
  uint32_t capacity;
  uint64_t write_index; /* total samples pushed */
  uint64_t read_index;  /* next sample to read; write_index - read_index <= capacity */
  uint64_t dropped;
} Sn76489Ring;

void sn76489_ring_init(Sn76489Ring *ring, int16_t *storage, uint32_t capacity);
void sn76489_ring_clear(Sn76489Ring *ring);
void sn76489_ring_sink(void *ring, uint64_t first_index, const int16_t *samples, uint32_t count);
uint32_t sn76489_ring_available(const Sn76489Ring *ring);
/* Copies up to `max` samples out, returns the number copied; `first_index` (optional) receives the absolute index of
 * the first copied sample. */
uint32_t sn76489_ring_read(Sn76489Ring *ring, int16_t *out, uint32_t max, uint64_t *first_index);

#ifdef __cplusplus
}
#endif
#endif
