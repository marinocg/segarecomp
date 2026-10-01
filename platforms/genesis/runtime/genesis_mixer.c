#include "genesis_mixer.h"

#include <string.h>

uint64_t genesis_mixer_window_start(uint64_t k) {
  const uint64_t master = GENESIS_NTSC_MASTER_TICKS_PER_SECOND;
  return (k * master + (GENESIS_MIXER_RATE_HZ - 1U)) / GENESIS_MIXER_RATE_HZ;
}

static int window_in_range(uint64_t k) { return k < UINT64_MAX / GENESIS_NTSC_MASTER_TICKS_PER_SECOND - 2U; }

static void source_init(GenesisMixerSource *s) {
  memset(s, 0, sizeof(*s));
  s->window_end = genesis_mixer_window_start(1U);
}

void genesis_mixer_init(GenesisMixer *mixer) {
  memset(mixer, 0, sizeof(*mixer));
  source_init(&mixer->ym);
  source_init(&mixer->psg);
  genesis_sha256_init(&mixer->sha);
  genesis_sha256_init(&mixer->range_sha);
  mixer->min_sample = INT16_MAX;
  mixer->max_sample = INT16_MIN;
}

void genesis_mixer_set_sink(GenesisMixer *mixer, GenesisMixerFrameSink sink, void *context) {
  mixer->sink = sink;
  mixer->sink_context = context;
}

void genesis_mixer_set_range(GenesisMixer *mixer, uint64_t first, uint64_t count) {
  mixer->range_first = first;
  mixer->range_count = count;
}

static int16_t clip16(int32_t value, uint64_t *clipped) {
  if (value > INT16_MAX) { ++*clipped; return INT16_MAX; }
  if (value < INT16_MIN) { ++*clipped; return INT16_MIN; }
  return (int16_t)value;
}

static void put_le16(uint8_t *out, int16_t value) {
  out[0] = (uint8_t)((uint16_t)value & 0xFFU);
  out[1] = (uint8_t)((uint16_t)value >> 8);
}

static void emit_frame(GenesisMixer *mixer, const int32_t ym[2], const int32_t psg[2]) {
  uint8_t bytes[4];
  const uint64_t index = mixer->frames;
  const int16_t left = clip16(ym[0] + psg[0], &mixer->clipped);
  const int16_t right = clip16(ym[1] + psg[1], &mixer->clipped);
  put_le16(bytes, left);
  put_le16(bytes + 2, right);
  genesis_sha256_update(&mixer->sha, bytes, (uint32_t)sizeof bytes);
  if (mixer->range_count != 0U && index >= mixer->range_first && index - mixer->range_first < mixer->range_count) {
    genesis_sha256_update(&mixer->range_sha, bytes, (uint32_t)sizeof bytes);
    ++mixer->range_frames;
  }
  if (mixer->have_prev && (left != mixer->prev[0] || right != mixer->prev[1])) ++mixer->changes;
  mixer->prev[0] = left;
  mixer->prev[1] = right;
  mixer->have_prev = 1;
  if (left < mixer->min_sample) mixer->min_sample = left;
  if (right < mixer->min_sample) mixer->min_sample = right;
  if (left > mixer->max_sample) mixer->max_sample = left;
  if (right > mixer->max_sample) mixer->max_sample = right;
  if (mixer->sink != NULL) mixer->sink(mixer->sink_context, index, left, right);
  if (mixer->ring_write - mixer->ring_read == GENESIS_MIXER_RING_FRAMES) {  /* overrun: drop the oldest frame */
    ++mixer->ring_read;
    ++mixer->ring_dropped;
  }
  mixer->ring[mixer->ring_write % GENESIS_MIXER_RING_FRAMES][0] = left;
  mixer->ring[mixer->ring_write % GENESIS_MIXER_RING_FRAMES][1] = right;
  ++mixer->ring_write;
  mixer->frames = index + 1U;
}

static void pump(GenesisMixer *mixer) {
  while (mixer->fault == GENESIS_MIXER_FAULT_NONE && mixer->ym.fifo_count != 0U && mixer->psg.fifo_count != 0U) {
    int32_t ym[2], psg[2];
    memcpy(ym, mixer->ym.fifo[mixer->ym.fifo_head], sizeof ym);
    memcpy(psg, mixer->psg.fifo[mixer->psg.fifo_head], sizeof psg);
    mixer->ym.fifo_head = (mixer->ym.fifo_head + 1U) % GENESIS_MIXER_FIFO_WINDOWS;
    mixer->psg.fifo_head = (mixer->psg.fifo_head + 1U) % GENESIS_MIXER_FIFO_WINDOWS;
    --mixer->ym.fifo_count;
    --mixer->psg.fifo_count;
    emit_frame(mixer, ym, psg);
  }
}

/* Closes the current window of `s`: the decimated value goes to the source's FIFO. */
static void close_window(GenesisMixer *mixer, GenesisMixerSource *s, int is_psg) {
  int32_t value[2];
  if (s->count != 0U) {
    value[0] = (int32_t)(s->sum[0] / (int64_t)s->count);  /* truncating division */
    value[1] = (int32_t)(s->sum[1] / (int64_t)s->count);
    if (is_psg) {  /* unipolar level scaled to 0..8192, equal in L and R */
      value[0] = (int32_t)(((uint64_t)value[0] * GENESIS_MIXER_PSG_SCALED_MAX) / SN76489_FULL_SCALE);
      value[1] = value[0];
    }
    s->last[0] = value[0];
    s->last[1] = value[1];
  } else {
    value[0] = s->last[0];
    value[1] = s->last[1];
  }
  if (s->fifo_count == GENESIS_MIXER_FIFO_WINDOWS) {
    mixer->fault = GENESIS_MIXER_FAULT_SOURCE_AHEAD;
    return;
  }
  s->fifo[(s->fifo_head + s->fifo_count) % GENESIS_MIXER_FIFO_WINDOWS][0] = value[0];
  s->fifo[(s->fifo_head + s->fifo_count) % GENESIS_MIXER_FIFO_WINDOWS][1] = value[1];
  ++s->fifo_count;
  s->sum[0] = s->sum[1] = 0;
  s->count = 0U;
  ++s->window;
  if (!window_in_range(s->window)) {
    mixer->fault = GENESIS_MIXER_FAULT_TIME_OVERFLOW;
    return;
  }
  s->window_end = genesis_mixer_window_start(s->window + 1U);
}

static void deliver(GenesisMixer *mixer, GenesisMixerSource *s, int is_psg, uint64_t start, uint64_t period, int32_t a, int32_t b) {
  if (mixer->fault != GENESIS_MIXER_FAULT_NONE) return;
  if (start < s->watermark) {
    mixer->fault = GENESIS_MIXER_FAULT_NON_MONOTONIC;
    return;
  }
  while (mixer->fault == GENESIS_MIXER_FAULT_NONE && start >= s->window_end) close_window(mixer, s, is_psg);
  if (mixer->fault != GENESIS_MIXER_FAULT_NONE) return;
  s->sum[0] += a;
  s->sum[1] += b;
  ++s->count;
  s->watermark = start + period;
  /* every window ending at or before the watermark has all of its natives */
  while (mixer->fault == GENESIS_MIXER_FAULT_NONE && s->window_end <= s->watermark) close_window(mixer, s, is_psg);
  pump(mixer);
}

void genesis_mixer_push_ym(GenesisMixer *mixer, uint64_t start_master_ticks, int32_t left, int32_t right) {
  uint64_t ignored = 0U;
  deliver(mixer, &mixer->ym, 0, start_master_ticks, GENESIS_MIXER_YM_SAMPLE_MASTER, clip16(left, &ignored), clip16(right, &ignored));
}

void genesis_mixer_push_psg(GenesisMixer *mixer, uint64_t start_master_ticks, uint32_t level) {
  deliver(mixer, &mixer->psg, 1, start_master_ticks, GENESIS_MIXER_PSG_TICK_MASTER, (int32_t)level, (int32_t)level);
}

static void finish_digest(const GenesisSha256 *state, uint64_t frames, uint8_t out[32]) {
  GenesisSha256 copy = *state;
  uint8_t count[8];
  unsigned i;
  for (i = 0U; i < 8U; ++i) count[i] = (uint8_t)(frames >> (8U * i));  /* u64le */
  genesis_sha256_update(&copy, count, (uint32_t)sizeof count);
  genesis_sha256_final(&copy, out);
}

void genesis_mixer_digest(const GenesisMixer *mixer, uint8_t out[32]) { finish_digest(&mixer->sha, mixer->frames, out); }

void genesis_mixer_range_digest(const GenesisMixer *mixer, uint8_t out[32]) {
  finish_digest(&mixer->range_sha, mixer->range_frames, out);
}

int32_t genesis_mixer_span(const GenesisMixer *mixer) {
  return mixer->frames == 0U ? 0 : (int32_t)mixer->max_sample - (int32_t)mixer->min_sample;
}

int genesis_mixer_non_silent(const GenesisMixer *mixer) {
  return mixer->changes != 0U && genesis_mixer_span(mixer) >= GENESIS_MIXER_NONSILENT_MIN_SPAN;
}

uint32_t genesis_mixer_ring_available(const GenesisMixer *mixer) { return (uint32_t)(mixer->ring_write - mixer->ring_read); }

uint32_t genesis_mixer_ring_read(GenesisMixer *mixer, int16_t *out, uint32_t max_frames) {
  uint32_t n = genesis_mixer_ring_available(mixer), i;
  if (n > max_frames) n = max_frames;
  for (i = 0U; i < n; ++i) {
    out[2U * i] = mixer->ring[mixer->ring_read % GENESIS_MIXER_RING_FRAMES][0];
    out[2U * i + 1U] = mixer->ring[mixer->ring_read % GENESIS_MIXER_RING_FRAMES][1];
    ++mixer->ring_read;
  }
  return n;
}
