#include "segarecomp/device/sega/psg/sn76489.h"

#include <string.h>

const uint16_t sn76489_default_levels[16] = {32767, 26028, 20675, 16422, 13045, 10362, 8231, 6568,
                                             5193,  4125,  3277,  2603,  2067,  1642,  1304, 0};

Sn76489Config sn76489_default_config(void) {
  Sn76489Config c;
  c.noise_taps = SN76489_DEFAULT_TAPS;
  c.lfsr_bits = SN76489_DEFAULT_LFSR_BITS;
  c.divider = SN76489_DEFAULT_DIVIDER;
  c.levels = sn76489_default_levels;
  return c;
}

static uint16_t lfsr_seed(const Sn76489 *chip) { return (uint16_t)(1u << (chip->config.lfsr_bits - 1u)); }

int sn76489_init(Sn76489 *chip, const Sn76489Config *config) {
  if (config->lfsr_bits < 2u || config->lfsr_bits > 16u || config->divider == 0u || config->levels == NULL) return 0;
  memset(chip, 0, sizeof *chip);
  chip->config = *config;
  sn76489_reset(chip);
  return 1;
}

void sn76489_reset(Sn76489 *chip) {
  const Sn76489Config config = chip->config;
  memset(chip, 0, sizeof *chip);
  chip->config = config;
  chip->attenuation[0] = chip->attenuation[1] = chip->attenuation[2] = chip->attenuation[3] = 0x0Fu;
  chip->lfsr = lfsr_seed(chip);
  chip->latch = -1;
}

uint32_t sn76489_level(const Sn76489 *chip) {
  uint32_t level = 0;
  unsigned i;
  for (i = 0; i < SN76489_TONE_CHANNELS; ++i)
    if (chip->tone_output[i]) level += chip->config.levels[chip->attenuation[i] & 15u];
  if (chip->lfsr & 1u) level += chip->config.levels[chip->attenuation[3] & 15u];
  return level;
}

static void shift_lfsr(Sn76489 *chip) {
  const uint16_t lfsr = chip->lfsr;
  uint16_t feedback;
  if (chip->noise_control & 4u) {
    uint16_t bits = (uint16_t)(lfsr & chip->config.noise_taps);
    bits ^= (uint16_t)(bits >> 8);
    bits ^= (uint16_t)(bits >> 4);
    bits ^= (uint16_t)(bits >> 2);
    bits ^= (uint16_t)(bits >> 1);
    feedback = (uint16_t)(bits & 1u);
  } else {
    feedback = (uint16_t)(lfsr & 1u);
  }
  chip->lfsr = (uint16_t)((lfsr >> 1) | (feedback << (chip->config.lfsr_bits - 1u)));
}

static void pcm_push_tick(Sn76489Pcm *pcm, uint64_t end, uint32_t level);

uint32_t sn76489_tick(Sn76489 *chip, Sn76489Pcm *pcm) {
  unsigned i;
  uint32_t level;
  for (i = 0; i < SN76489_TONE_CHANNELS; ++i) {
    if (chip->tone_counter[i] == 0u || --chip->tone_counter[i] == 0u) {
      chip->tone_counter[i] = chip->tone_period[i];
      chip->tone_output[i] ^= 1u;
    }
  }
  if (chip->noise_counter == 0u || --chip->noise_counter == 0u) {
    const unsigned rate = chip->noise_control & 3u;
    chip->noise_counter = rate == 3u ? chip->tone_period[2] : (uint16_t)(0x10u << rate);
    chip->noise_flip ^= 1u;
    if (chip->noise_flip) shift_lfsr(chip);
  }
  chip->ticks += 1u;
  level = sn76489_level(chip);
  if (pcm != NULL) {
    pcm_push_tick(pcm, chip->ticks * chip->config.divider, level);
  }
  return level;
}

void sn76489_advance(Sn76489 *chip, Sn76489Pcm *pcm, uint64_t cycles) {
  const uint64_t target = cycles / chip->config.divider;
  while (chip->ticks < target) (void)sn76489_tick(chip, pcm);
}

static void write_noise_control(Sn76489 *chip, uint8_t value) {
  chip->noise_control = (uint8_t)(value & 7u);
  chip->lfsr = lfsr_seed(chip);
}

Sn76489WriteResult sn76489_write(Sn76489 *chip, uint8_t value) {
  if (value & 0x80u) {
    chip->latch = (int8_t)((value >> 4) & 7u);
    {
      const unsigned channel = (unsigned)chip->latch >> 1;
      if (chip->latch & 1) chip->attenuation[channel] = (uint8_t)(value & 15u);
      else if (channel < SN76489_TONE_CHANNELS)
        chip->tone_period[channel] = (uint16_t)((chip->tone_period[channel] & 0x3F0u) | (value & 15u));
      else write_noise_control(chip, value);
    }
    return SN76489_WRITE_OK;
  }
  if (chip->latch < 0) return SN76489_WRITE_DATA_BEFORE_LATCH;
  {
    const unsigned channel = (unsigned)chip->latch >> 1;
    if (chip->latch & 1) chip->attenuation[channel] = (uint8_t)(value & 15u);
    else if (channel < SN76489_TONE_CHANNELS)
      chip->tone_period[channel] = (uint16_t)((chip->tone_period[channel] & 0x00Fu) | ((value & 0x3Fu) << 4));
    else write_noise_control(chip, value);
  }
  return SN76489_WRITE_OK;
}

void sn76489_state_bytes(const Sn76489 *chip, uint8_t out[SN76489_STATE_BYTES]) {
  size_t n = 0;
  unsigned i;
  for (i = 0; i < SN76489_TONE_CHANNELS; ++i) {
    out[n++] = (uint8_t)chip->tone_period[i];
    out[n++] = (uint8_t)(chip->tone_period[i] >> 8);
  }
  for (i = 0; i < SN76489_TONE_CHANNELS; ++i) {
    out[n++] = (uint8_t)chip->tone_counter[i];
    out[n++] = (uint8_t)(chip->tone_counter[i] >> 8);
  }
  for (i = 0; i < SN76489_TONE_CHANNELS; ++i) out[n++] = chip->tone_output[i];
  for (i = 0; i < SN76489_CHANNELS; ++i) out[n++] = chip->attenuation[i];
  out[n++] = chip->noise_control;
  out[n++] = (uint8_t)chip->noise_counter;
  out[n++] = (uint8_t)(chip->noise_counter >> 8);
  out[n++] = chip->noise_flip;
  out[n++] = (uint8_t)chip->lfsr;
  out[n++] = (uint8_t)(chip->lfsr >> 8);
  out[n++] = (uint8_t)chip->latch;
  for (i = 0; i < 8; ++i) out[n++] = (uint8_t)(chip->ticks >> (8u * i));
}

/* ---- PCM stream ----------------------------------------------------------------------------------------------- */

Sn76489PcmConfig sn76489_pcm_config_44100_sms(void) {
  Sn76489PcmConfig c;
  c.rate_num = 39375000u;
  c.rate_den = 485100u;
  c.full_scale = SN76489_FULL_SCALE;
  return c;
}

uint64_t sn76489_pcm_sample_start(const Sn76489PcmConfig *config, uint64_t k) {
  const uint64_t product = k * config->rate_num;
  return product / config->rate_den + (product % config->rate_den != 0u ? 1u : 0u);
}

void sn76489_pcm_reset(Sn76489Pcm *pcm) {
  pcm->index = 0;
  pcm->boundary = sn76489_pcm_sample_start(&pcm->config, 1);
  pcm->sum = 0;
  pcm->count = 0;
  pcm->last_mean = 0;
  pcm->batch_first = 0;
  pcm->batch_count = 0;
}

void sn76489_pcm_init(Sn76489Pcm *pcm, const Sn76489PcmConfig *config, Sn76489PcmSink sink, void *sink_context) {
  memset(pcm, 0, sizeof *pcm);
  pcm->config = *config;
  pcm->sink = sink;
  pcm->sink_context = sink_context;
  sn76489_pcm_reset(pcm);
}

void sn76489_pcm_flush(Sn76489Pcm *pcm) {
  if (pcm->batch_count != 0u && pcm->sink != NULL) pcm->sink(pcm->sink_context, pcm->batch_first, pcm->batch, pcm->batch_count);
  pcm->batch_first += pcm->batch_count;
  pcm->batch_count = 0;
}

static void emit_sample(Sn76489Pcm *pcm) {
  const uint32_t mean = pcm->count != 0u ? (uint32_t)(pcm->sum / pcm->count) : pcm->last_mean;
  const int32_t value = (int32_t)(((uint64_t)mean * 65535u) / pcm->config.full_scale) - 32768;
  pcm->batch[pcm->batch_count++] = (int16_t)value;
  pcm->last_mean = mean;
  pcm->sum = 0;
  pcm->count = 0;
  pcm->index += 1u;
  pcm->boundary = sn76489_pcm_sample_start(&pcm->config, pcm->index + 1u);
  if (pcm->batch_count == SN76489_PCM_BATCH) sn76489_pcm_flush(pcm);
}

/* Step called by sn76489_tick: `end` is the master clock at the end of the tick just executed. A sample is
 * complete exactly when every tick starting before its end has run, i.e. when end >= T_(k+1). */
static void pcm_push_tick(Sn76489Pcm *pcm, uint64_t end, uint32_t level) {
  pcm->sum += level;
  pcm->count += 1u;
  while (end >= pcm->boundary) emit_sample(pcm);
}

/* ---- ring ----------------------------------------------------------------------------------------------------- */

void sn76489_ring_init(Sn76489Ring *ring, int16_t *storage, uint32_t capacity) {
  ring->data = storage;
  ring->capacity = capacity;
  sn76489_ring_clear(ring);
}

void sn76489_ring_clear(Sn76489Ring *ring) {
  ring->write_index = 0;
  ring->read_index = 0;
  ring->dropped = 0;
}

void sn76489_ring_sink(void *context, uint64_t first_index, const int16_t *samples, uint32_t count) {
  Sn76489Ring *ring = (Sn76489Ring *)context;
  uint32_t i;
  (void)first_index;
  if (ring->capacity == 0u) return;
  for (i = 0; i < count; ++i) {
    if (ring->write_index - ring->read_index == ring->capacity) {
      ring->read_index += 1u;
      ring->dropped += 1u;
    }
    ring->data[ring->write_index % ring->capacity] = samples[i];
    ring->write_index += 1u;
  }
}

uint32_t sn76489_ring_available(const Sn76489Ring *ring) { return (uint32_t)(ring->write_index - ring->read_index); }

uint32_t sn76489_ring_read(Sn76489Ring *ring, int16_t *out, uint32_t max, uint64_t *first_index) {
  uint32_t n = sn76489_ring_available(ring);
  uint32_t i;
  if (n > max) n = max;
  if (first_index != NULL) *first_index = ring->read_index;
  for (i = 0; i < n; ++i) out[i] = ring->data[(ring->read_index + i) % ring->capacity];
  ring->read_index += n;
  return n;
}
