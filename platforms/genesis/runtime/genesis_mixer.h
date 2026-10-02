/*
 * SEG-032-T009 (ADR 0075, contract section 11): the deterministic Genesis audio mixer.
 *
 * Joins the two native sources on the single master-tick timeline and produces the frozen artifact: 44,100 Hz, 2 channels, signed
 * 16-bit little-endian. Source `native` samples arrive in order with the master tick of their START:
 *   - the YM2612: one stereo sample per YM2612_MASTER_TICKS_PER_SAMPLE (ymfm range, clipped to int16 on arrival);
 *   - the PSG: one level per tick (240 master ticks), 0..SN76489_FULL_SCALE.
 * Output frame k covers master ticks [T_k, T_(k+1)), T_k = ceil(k x master / 44,100). Each source is decimated independently by an
 * integer box filter (the truncating mean of every native whose start lies in the window; an empty window repeats the source's last
 * decimated value, initially 0); the PSG mean is then scaled to 0..8192 (floor(mean x 8192 / 131,068)) and added equally to L and R;
 * the sum with the YM value saturates at the s16 range and every saturated channel sample is counted. A frame is produced only once
 * BOTH sources are known to have delivered every native that starts inside its window (a source's `watermark` is the start of its
 * first undelivered native), so the stream is a pure function of the native streams and independent of how the host chunked, sliced
 * or synchronized execution. All state is plain integers; there is no host clock, no allocation and no I/O.
 *
 * The SHA-256 digest (and the optional frame-range digest) are accumulated frame by frame, independently of any consumer: the
 * consumer ring drops the oldest frames when it overruns (counted; the digest and the optional frame sink never see the drop).
 */
#ifndef SEGARECOMP_GENESIS_MIXER_H
#define SEGARECOMP_GENESIS_MIXER_H

#include <stdint.h>

#include "genesis_audio_format.h"
#include "runtime.h"
#include "segarecomp/device/sega/psg/sn76489.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"

/* ---- frozen format parameters (each appears exactly once in code; contract section 11) ---- */
/* 44,100 Hz, 2 channels, the PSG scale 8192 and the PSG clock divider 15 are in genesis_audio_format.h. */
#define GENESIS_MIXER_PSG_TICK_MASTER (SN76489_DEFAULT_DIVIDER * GENESIS_PSG_CLOCK_DIVIDER) /* 240 */
#define GENESIS_MIXER_YM_SAMPLE_MASTER YM2612_MASTER_TICKS_PER_SAMPLE                     /* 1,008 */

/* ---- capacities (host policy, not format) ---- */
#define GENESIS_MIXER_RING_FRAMES 8192U /* consumer ring; overrun drops the OLDEST frame and counts it */
#define GENESIS_MIXER_FIFO_WINDOWS 2048U /* decimated windows one source may run ahead of the other */
/* "Non-silent" (contract section 11): the mixed stream changes value and its peak-to-peak span is at least this many s16 units. */
#define GENESIS_MIXER_NONSILENT_MIN_SPAN 16

typedef enum GenesisMixerFault {
  GENESIS_MIXER_FAULT_NONE = 0,
  GENESIS_MIXER_FAULT_SOURCE_AHEAD = 1,    /* one source ran GENESIS_MIXER_FIFO_WINDOWS windows ahead of the other: the host did not synchronize */
  GENESIS_MIXER_FAULT_NON_MONOTONIC = 2,   /* a native arrived before the previous one ended */
  GENESIS_MIXER_FAULT_TIME_OVERFLOW = 3    /* the frame index would overflow the 64-bit window arithmetic */
} GenesisMixerFault;

typedef struct GenesisMixerSource {
  uint64_t window;     /* index of the window being accumulated */
  uint64_t window_end; /* T_(window + 1) */
  uint64_t watermark;  /* start of the first native not yet delivered */
  int64_t sum[2];
  uint32_t count;
  int32_t last[2];
  uint32_t fifo_head, fifo_count;
  int32_t fifo[GENESIS_MIXER_FIFO_WINDOWS][2];
} GenesisMixerSource;

/* Optional per-frame observer (the PCM file writer, a test tap). It cannot influence the mixer. */
typedef void (*GenesisMixerFrameSink)(void *context, uint64_t frame_index, int16_t left, int16_t right);

typedef struct GenesisMixer {
  GenesisMixerSource ym, psg;
  uint64_t frames;     /* output frames produced */
  uint64_t clipped;    /* saturated channel samples (L and R counted separately) */
  uint64_t changes;    /* frames whose (L, R) differs from the previous frame */
  int16_t min_sample, max_sample, prev[2];
  uint8_t have_prev;
  GenesisMixerFault fault;
  GenesisSha256 sha; /* the runtime's SHA-256 (runtime.h) */
  GenesisSha256 range_sha;
  uint64_t range_first, range_count, range_frames;
  GenesisMixerFrameSink sink;
  void *sink_context;
  /* consumer ring */
  int16_t ring[GENESIS_MIXER_RING_FRAMES][2];
  uint64_t ring_write, ring_read, ring_dropped;
} GenesisMixer;

/* Power-on state; time 0. */
void genesis_mixer_init(GenesisMixer *mixer);
void genesis_mixer_set_sink(GenesisMixer *mixer, GenesisMixerFrameSink sink, void *context);
/* Digest also the frames [first, first + count); count 0 disables it. Call before the first frame. */
void genesis_mixer_set_range(GenesisMixer *mixer, uint64_t first, uint64_t count);

/* Native deliveries, in order of start time per source. `ym` values are clipped to int16 here. */
void genesis_mixer_push_ym(GenesisMixer *mixer, uint64_t start_master_ticks, int32_t left, int32_t right);
void genesis_mixer_push_psg(GenesisMixer *mixer, uint64_t start_master_ticks, uint32_t level);

/* T_k in master ticks (frozen window start). */
uint64_t genesis_mixer_window_start(uint64_t k);

/* SHA-256 over every produced frame's canonical bytes (L then R, s16le) followed by u64le frame count. Non-destructive. */
void genesis_mixer_digest(const GenesisMixer *mixer, uint8_t out[32]);
void genesis_mixer_range_digest(const GenesisMixer *mixer, uint8_t out[32]);
/* Peak-to-peak span of the produced stream and the non-silent verdict (contract section 11). */
int32_t genesis_mixer_span(const GenesisMixer *mixer);
int genesis_mixer_non_silent(const GenesisMixer *mixer);

/* Consumer ring: frames not yet read; copies up to `max` interleaved L,R frames (2 x max int16) and returns the count. */
uint32_t genesis_mixer_ring_available(const GenesisMixer *mixer);
uint32_t genesis_mixer_ring_read(GenesisMixer *mixer, int16_t *out, uint32_t max_frames);

#endif
