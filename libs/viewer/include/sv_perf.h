#ifndef SEGARECOMP_SV_PERF_H
#define SEGARECOMP_SV_PERF_H
/*
 * SEG-125 (ADR 0273): the runtime performance collector. Header-only, strict C11, SDL-free, allocation-free: one fixed-size struct
 * per session, owned by the console adapter (Genesis headless hook / Genesis viewer; the Master System and Sega CD viewers adopt the same
 * struct). It never touches guest state and is never linked into generated C. HOST time only: guest time (master ticks, dispatches) is
 * carried as separate counters and is never derived from the host clock.
 *
 * Cost model: the clock is read at frame/slice/operation boundaries (a handful of reads per virtual frame), never per instruction.
 * With `active == 0` every entry point returns after one branch, so a program that does not ask for profiling does no clock reads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SV_PERF_SCHEMA_VERSION 1
/* Frame-time histogram (fixed memory, no sample storage): 1280 linear buckets of 50 us cover 0..64 ms (a percentile there is within one bucket, <= 50 us above the true value);
 * 28 tail buckets, 4 per octave (ratio 2^(1/4)), cover 64 ms..8.192 s (a percentile there is at most 19% above the true value); the last bucket holds everything >= 8.192 s,
 * where a percentile is reported as the exact observed maximum. Bucket counters are 64-bit and cannot saturate in practice (2^64 samples); min, max, sum and count are exact. */
#define SV_PERF_BUCKET_NS 50000U
#define SV_PERF_LIN_BUCKETS 1280U
#define SV_PERF_TAIL_BUCKETS 28U
#define SV_PERF_BUCKETS (SV_PERF_LIN_BUCKETS + SV_PERF_TAIL_BUCKETS + 1U)
#define SV_PERF_RANGE_NS 8192000000ULL /* the largest duration with its own bucket */
#define SV_PERF_HISTORY 1024U         /* fixed-size recent-frame history (power of two) */
#define SV_PERF_COUNTERS 16U

typedef uint64_t (*SvPerfClock)(void);

/* Timed operations at frame/slice granularity. `inclusive` spans are contained in their parent; exclusive spans are disjoint. */
typedef enum SvPerfSpanId {
  SV_SPAN_EMULATION = 0, /* all guest execution slices: M68K, Z80 sync, devices, sound synthesis and frame composition (inclusive) */
  SV_SPAN_VDP_COMPOSE,   /* frame composition by the VDP producer, contained in SV_SPAN_EMULATION (inclusive) */
  SV_SPAN_RGB_CONVERT,   /* frame artifact -> RGB24 picture conversion by the console adapter, before the host present (exclusive) */
  SV_SPAN_TEXTURE_UPLOAD,/* host picture -> texture upload (format conversion + side treatment + update), contained in SV_SPAN_PRESENT (inclusive) */
  SV_SPAN_PRESENT,       /* host present call: includes the render/present submission; NOT GPU execution time (inclusive) */
  SV_SPAN_AUDIO_PRESENT, /* draining the mixer into the host audio sink (exclusive) */
  SV_SPAN_PACING_WAIT,   /* frame pacing wait including the host sleep (exclusive) */
  SV_SPAN_COUNT
} SvPerfSpanId;

typedef struct SvPerfSpan {
  uint64_t count, total_ns, max_ns;
} SvPerfSpan;

typedef struct SvPerfHistogram {
  uint64_t bucket[SV_PERF_BUCKETS];
  uint64_t count, sum_ns, min_ns, max_ns;
} SvPerfHistogram;

/* One OBSERVATION: the wall time between two observed points of the loop. `frames` == 1 and `partial` == 0: exactly one virtual frame, the interval is that frame's measured
 * time. Otherwise the observation covered `frames` virtual frames (a slice crossed several boundaries) or started inside a frame (profiling switched on mid-frame): `interval_us` is the
 * whole observed wall time and no per-frame time is claimed - a consumer may show interval/frames as an average, labelled as such. */
typedef struct SvPerfFrame {
  uint64_t frame;       /* virtual frame ordinal at the end of the observation */
  uint32_t interval_us; /* wall time of the whole observation (saturating) */
  uint32_t work_us;     /* interval minus pacing wait */
  uint32_t emulation_us, present_us;
  uint32_t frames;      /* virtual frames covered */
  uint32_t missed;      /* of those frames, boundaries whose pacer wait did not sleep (deadline already past); settled when the pacer has run */
  uint32_t partial;     /* 1: began inside a frame, so not an exact frame time even if frames == 1 */
} SvPerfFrame;

typedef struct SvPerf {
  int active;            /* 0: every entry point is a single-branch no-op */
  int detailed;          /* 0: summary mode (histograms + totals), 1: the report also exports the recent-frame timeline */
  SvPerfClock now;
  uint64_t target_period_ns; /* the console's native frame period at 1.0x (documented constant, not measured) */
  uint64_t start_ns, last_boundary_ns, end_ns;
  uint64_t virtual_frames, frames_presented, missed_deadlines, resyncs, pacing_waits;
  uint64_t estimated_frames;   /* virtual frames inside multi-frame/partial observations: counted, but not individually timed (not in the histograms) */
  uint64_t est_observations, est_wall_ns, est_max_wall_ns;
  /* observation state (a window starts when the collector becomes active; time while inactive is never attributed) */
  int observing;
  uint64_t vf_last;            /* virtual frame count at the end of the previous observation */
  int partial_next;            /* the window began inside a frame: the first boundary is not an exact frame time */
  uint64_t pending_frames, sleep_mark; /* boundaries whose pacer verdict is still to be settled; the pacer's sleep count when they were crossed */
  uint64_t paused_ns, pauses;  /* time spent in the pause menu: not part of any frame interval */
  int pinned;                  /* 1 = a report was requested: stays active; 0 = active only while the overlay is shown */
  uint64_t guest_master_ticks; /* deterministic guest time; reported separately from every host duration */
  uint64_t counters[SV_PERF_COUNTERS];
  SvPerfHistogram interval, work;
  SvPerfSpan span[SV_SPAN_COUNT];
  uint64_t frame_pacing_ns, frame_emulation_ns, frame_present_ns; /* accumulated since the last boundary */
  SvPerfFrame history[SV_PERF_HISTORY];
  uint64_t history_total; /* observations ever recorded; the ring holds the last SV_PERF_HISTORY */
} SvPerf;

static const char *const sv_perf_span_names[SV_SPAN_COUNT] = {"emulation", "vdp_compose", "rgb_convert", "texture_upload", "present", "audio_present", "pacing_wait"};
static const char *const sv_perf_span_class[SV_SPAN_COUNT] = {"inclusive", "inclusive", "exclusive", "inclusive", "inclusive", "exclusive", "exclusive"};
static const char *const sv_perf_span_parent[SV_SPAN_COUNT] = {"", "emulation", "", "present", "", "", ""};

static inline void sv_perf_hist_reset(SvPerfHistogram *h) {
  memset(h, 0, sizeof *h);
  h->min_ns = UINT64_MAX;
}

static const uint64_t sv_perf_tail_edge[SV_PERF_TAIL_BUCKETS + 1U] = {
    64000000ULL,   76109255ULL,   90509668ULL,   107634741ULL,  128000000ULL,  152218511ULL,  181019336ULL,  215269482ULL,  256000000ULL,  304437021ULL,
    362038672ULL,  430538965ULL,  512000000ULL,  608874043ULL,  724077344ULL,  861077929ULL,  1024000000ULL, 1217748086ULL, 1448154688ULL, 1722155858ULL,
    2048000000ULL, 2435496172ULL, 2896309376ULL, 3444311717ULL, 4096000000ULL, 4870992343ULL, 5792618751ULL, 6888623434ULL, 8192000000ULL};

static inline uint32_t sv_perf_bucket_of(uint64_t ns) {
  uint32_t j;
  if (ns < (uint64_t)SV_PERF_LIN_BUCKETS * SV_PERF_BUCKET_NS) return (uint32_t)(ns / SV_PERF_BUCKET_NS);
  for (j = 0U; j < SV_PERF_TAIL_BUCKETS; ++j)
    if (ns < sv_perf_tail_edge[j + 1U]) return SV_PERF_LIN_BUCKETS + j;
  return SV_PERF_BUCKETS - 1U;
}

static inline void sv_perf_hist_add(SvPerfHistogram *h, uint64_t ns) {
  ++h->bucket[sv_perf_bucket_of(ns)];
  ++h->count;
  h->sum_ns += ns;
  if (ns < h->min_ns) h->min_ns = ns;
  if (ns > h->max_ns) h->max_ns = ns;
}

/* The upper edge of the bucket holding the ceil(q * count)-th smallest sample, clamped to the observed [min, max]: never below the true value, at most one bucket above it
 * (<= 50 us below 64 ms, <= 19% in the tail); in the overflow bucket (>= 8.192 s) the exact maximum. q in (0,1]. */
static inline uint64_t sv_perf_hist_percentile(const SvPerfHistogram *h, double q) {
  uint64_t need, seen = 0U, v;
  uint32_t i;
  if (h->count == 0U) return 0U;
  need = (uint64_t)(q * (double)h->count);
  if ((double)need < q * (double)h->count) ++need;
  if (need == 0U) need = 1U;
  for (i = 0U; i < SV_PERF_BUCKETS; ++i) {
    seen += h->bucket[i];
    if (seen >= need) break;
  }
  if (i < SV_PERF_LIN_BUCKETS) v = ((uint64_t)i + 1U) * SV_PERF_BUCKET_NS;
  else if (i < SV_PERF_BUCKETS - 1U) v = sv_perf_tail_edge[i - SV_PERF_LIN_BUCKETS + 1U];
  else v = h->max_ns;
  if (v > h->max_ns) v = h->max_ns;
  if (v < h->min_ns) v = h->min_ns;
  return v;
}

static inline void sv_perf_init(SvPerf *p, SvPerfClock now, uint64_t target_period_ns, int detailed) {
  memset(p, 0, sizeof *p);
  p->now = now;
  p->target_period_ns = target_period_ns;
  p->detailed = detailed != 0;
  sv_perf_hist_reset(&p->interval);
  sv_perf_hist_reset(&p->work);
  p->active = 0; /* armed but idle: sv_perf_enable (report) or the viewer overlay switches it on */
}

/* Starts (or restarts) the measurement window at the current time; counts already taken are kept. */
static inline void sv_perf_start(SvPerf *p) {
  if (!p->active) return;
  p->start_ns = p->last_boundary_ns = p->now();
  p->frame_pacing_ns = p->frame_emulation_ns = p->frame_present_ns = 0U;
}

/* Cheap start-of-span stamp: 0 when inactive (the caller passes it back to sv_perf_span_end). */
static inline uint64_t sv_perf_span_begin(const SvPerf *p) { return p != NULL && p->active ? p->now() : 0U; }

static inline void sv_perf_span_end(SvPerf *p, SvPerfSpanId id, uint64_t began) {
  uint64_t d;
  if (p == NULL || !p->active || began == 0U) return;
  d = p->now() - began;
  p->span[id].count++;
  p->span[id].total_ns += d;
  if (d > p->span[id].max_ns) p->span[id].max_ns = d;
  if (id == SV_SPAN_PACING_WAIT) p->frame_pacing_ns += d;
  else if (id == SV_SPAN_EMULATION) p->frame_emulation_ns += d;
  else if (id == SV_SPAN_PRESENT) p->frame_present_ns += d;
}

static inline uint32_t sv_perf_us(uint64_t ns) { return ns / 1000U > UINT32_MAX ? UINT32_MAX : (uint32_t)(ns / 1000U); }

/* ---- observation API: one begin/end pair per guest slice (Genesis) or one end per poll (Master System, Sega CD) --------------------------------------------------------
 * begin: establishes the window's baseline BEFORE the guest runs (first call after the collector became active, so the first slice is counted and nothing that happened while it
 *        was inactive is attributed). A window that starts while the guest already has frames behind it begins inside a frame: its first boundary is not an exact frame time.
 * end:   the guest advanced to virtual frame `vf_now`. No boundary crossed: nothing is recorded (emulation/present time keeps accumulating toward the next boundary). One boundary
 *        crossed (and the window did not start mid-frame): an exact frame time goes into the histograms. Several crossed, or a partial first frame: the observation is recorded
 *        ONCE, the frames are counted as `estimated`, and no per-frame time is claimed or put in a histogram.
 * pace_settle: called after the pacer has run for the boundaries of the previous observation (the start of the next one, and the end of the session): each of those boundaries
 *        whose pacer wait did not sleep (`sleep_calls` unchanged) missed its deadline. The pacer's decisions are only observed, never influenced. */
static inline void sv_perf_observe_begin(SvPerf *p, uint64_t vf_now) {
  if (!p->active) { p->observing = 0; return; }
  if (p->observing) return;
  p->observing = 1;
  p->vf_last = vf_now;
  p->partial_next = vf_now != 0U;
  p->pending_frames = 0U;
  p->last_boundary_ns = p->now();   /* start_ns belongs to sv_perf_start / sv_perf_enable (the session), not to each window */
  p->frame_pacing_ns = p->frame_emulation_ns = p->frame_present_ns = 0U;
}

static inline void sv_perf_pace_settle(SvPerf *p, uint64_t sleep_calls, int unthrottled) {
  if (!p->active || !p->observing || p->pending_frames == 0U) return;
  if (!unthrottled) {
    const uint64_t slept = sleep_calls - p->sleep_mark;
    const uint64_t missed = p->pending_frames > slept ? p->pending_frames - slept : 0U;
    if (missed != 0U && p->history_total != 0U) {
      SvPerfFrame *f = &p->history[(p->history_total - 1U) % SV_PERF_HISTORY];
      f->missed = missed > UINT32_MAX ? UINT32_MAX : (uint32_t)missed;
    }
    p->missed_deadlines += missed;
  }
  p->pending_frames = 0U;
}

static inline void sv_perf_observe_end(SvPerf *p, uint64_t vf_now, uint64_t sleep_calls) {
  uint64_t t, interval, work, crossed;
  int exact;
  SvPerfFrame *f;
  if (!p->active) { p->observing = 0; return; }
  if (!p->observing || vf_now <= p->vf_last) return;
  crossed = vf_now - p->vf_last;
  p->vf_last = vf_now;
  t = p->now();
  interval = t > p->last_boundary_ns ? t - p->last_boundary_ns : 0U;
  work = interval > p->frame_pacing_ns ? interval - p->frame_pacing_ns : 0U;
  exact = crossed == 1U && !p->partial_next;
  p->partial_next = 0;
  p->virtual_frames += crossed;
  if (exact) {
    sv_perf_hist_add(&p->interval, interval);
    sv_perf_hist_add(&p->work, work);
  } else {
    p->estimated_frames += crossed;
    ++p->est_observations;
    p->est_wall_ns += interval;
    if (interval > p->est_max_wall_ns) p->est_max_wall_ns = interval;
  }
  f = &p->history[p->history_total % SV_PERF_HISTORY]; /* the fixed-size history is always kept (the overlay reads it); only the report decides whether it is exported */
  f->frame = vf_now;
  f->interval_us = sv_perf_us(interval);
  f->work_us = sv_perf_us(work);
  f->emulation_us = sv_perf_us(p->frame_emulation_ns);
  f->present_us = sv_perf_us(p->frame_present_ns);
  f->frames = crossed > UINT32_MAX ? UINT32_MAX : (uint32_t)crossed;
  f->missed = 0U;
  f->partial = exact ? 0U : (crossed == 1U ? 1U : 0U);
  ++p->history_total;
  p->pending_frames = crossed;
  p->sleep_mark = sleep_calls;
  p->last_boundary_ns = t;
  p->frame_pacing_ns = p->frame_emulation_ns = p->frame_present_ns = 0U;
}

/* A pause (menu, stop screen) began at `paused_from` (a span_begin stamp; 0 = was inactive): excluded from every frame interval, counted on its own. */
static inline void sv_perf_resync(SvPerf *p, uint64_t paused_from) {
  uint64_t t;
  if (p == NULL || !p->active || paused_from == 0U) return;
  t = p->now();
  p->paused_ns += t - paused_from;
  ++p->pauses;
  p->last_boundary_ns = t;
  p->partial_next = 1; /* the frame in progress was interrupted: the next boundary is not an exact frame time */
  p->frame_pacing_ns = p->frame_emulation_ns = p->frame_present_ns = 0U;
}

/* Called by the adapter to request a report: the collector stays active for the whole session. */
static inline void sv_perf_enable(SvPerf *p) {
  if (p->now == NULL) return;
  p->pinned = 1;
  if (!p->active) { p->active = 1; sv_perf_start(p); }
}

static inline void sv_perf_finish(SvPerf *p) {
  if (p->active) p->end_ns = p->now();
}

/* ---- request from the environment (shared by every console adapter) ------------------------------------------------------------------------ */

typedef struct SvPerfRequest {
  const char *path;     /* SEGARECOMP_PERF_REPORT, or NULL: no report requested */
  int detailed;         /* SEGARECOMP_PERF_MODE=detailed (default summary) */
  const char *privacy;  /* "private" (default) or "shareable" */
} SvPerfRequest;

/* Reads SEGARECOMP_PERF_REPORT / _MODE / _PRIVACY. Returns 0, or -1 when MODE or PRIVACY is malformed (the adapter then fails closed before the run, like every other
 * malformed run option). An empty path means "no report". */
static inline int sv_perf_request_from_env(SvPerfRequest *r) {
  const char *path = getenv("SEGARECOMP_PERF_REPORT"), *mode = getenv("SEGARECOMP_PERF_MODE"), *privacy = getenv("SEGARECOMP_PERF_PRIVACY");
  r->path = path != NULL && *path != '\0' ? path : NULL;
  r->detailed = 0;
  r->privacy = "private";
  if (mode != NULL && strcmp(mode, "summary") != 0 && strcmp(mode, "detailed") != 0) return -1;
  if (privacy != NULL && strcmp(privacy, "private") != 0 && strcmp(privacy, "shareable") != 0) return -1;
  r->detailed = mode != NULL && strcmp(mode, "detailed") == 0;
  if (privacy != NULL) r->privacy = privacy;
  return 0;
}

/* ---- JSON report ------------------------------------------------------------------------------------------------------------------ */

typedef struct SvPerfMeta {
  const char *platform;        /* "genesis" | "master_system" | "sega_cd" */
  const char *entry;           /* "headless" | "viewer" */
  const char *status;          /* "complete" | "incomplete" | "failed" */
  const char *outcome;         /* stable stop/outcome class, never a PC or address */
  const char *renderer;        /* host render backend name, or NULL (unavailable) */
  const char *privacy;         /* "private" | "shareable" */
  int paced;                   /* 1 = frame pacing active, 0 = unthrottled / headless free-run */
  uint64_t slice_dispatches;   /* runner slice size of the loop (comparability) */
  const char *counter_names[SV_PERF_COUNTERS]; /* parallel to SvPerf.counters; NULL = end */
  const char *unavailable[8][2];               /* {metric, reason}; NULL metric = end */
} SvPerfMeta;

static inline void sv_perf_json_hist(FILE *f, const char *name, const SvPerfHistogram *h) {
  fprintf(f, "\"%s\":{\"count\":%llu,\"min_ns\":%llu,\"avg_ns\":%llu,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,\"max_ns\":%llu,\"resolution_ns\":%u,\"range_ns\":%llu}", name,
          (unsigned long long)h->count, (unsigned long long)(h->count ? h->min_ns : 0U), (unsigned long long)(h->count ? h->sum_ns / h->count : 0U),
          (unsigned long long)sv_perf_hist_percentile(h, 0.50), (unsigned long long)sv_perf_hist_percentile(h, 0.95),
          (unsigned long long)sv_perf_hist_percentile(h, 0.99), (unsigned long long)h->max_ns, SV_PERF_BUCKET_NS, (unsigned long long)SV_PERF_RANGE_NS);
}

/* Writes one versioned report document. The runtime section only: no path, no title, no address, no ROM identity (ADR 0273). */
static inline void sv_perf_write_json(const SvPerf *p, const SvPerfMeta *m, FILE *f) {
  unsigned i;
  uint64_t n, first;
  fprintf(f, "{\"schema\":\"segarecomp.performance\",\"schema_version\":%u,\"kind\":\"runtime\",\"status\":\"%s\",\"privacy\":\"%s\",\"platform\":\"%s\",", SV_PERF_SCHEMA_VERSION,
          m->status, m->privacy, m->platform);
  fprintf(f, "\"config\":{\"entry\":\"%s\",\"mode\":\"%s\",\"paced\":%s,\"slice_dispatches\":%llu,\"target_frame_ns\":%llu,\"renderer\":", m->entry,
          p->detailed ? "detailed" : "summary", m->paced ? "true" : "false", (unsigned long long)m->slice_dispatches, (unsigned long long)p->target_period_ns);
  if (m->renderer != NULL) fprintf(f, "\"%s\"", m->renderer); else fputs("null", f);
  fprintf(f, ",\"host_os\":\"%s\"},",
#if defined(_WIN32)
          "windows"
#elif defined(__APPLE__)
          "macos"
#elif defined(__linux__)
          "linux"
#else
          "other"
#endif
  );
  fprintf(f, "\"runtime\":{\"outcome\":\"%s\",\"wall_ns\":%llu,\"paused_ns\":%llu,\"pauses\":%llu,", m->outcome,
          (unsigned long long)(p->end_ns > p->start_ns ? p->end_ns - p->start_ns : 0U), (unsigned long long)p->paused_ns, (unsigned long long)p->pauses);
  /* frames.estimated: virtual frames inside multi-frame or partial observations - counted, never individually timed, absent from the histograms (virtual = histogram count + estimated). */
  fprintf(f, "\"frames\":{\"virtual\":%llu,\"presented\":%llu,\"estimated\":%llu},\"estimated_observations\":{\"count\":%llu,\"wall_ns\":%llu,\"max_wall_ns\":%llu},",
          (unsigned long long)p->virtual_frames, (unsigned long long)p->frames_presented, (unsigned long long)p->estimated_frames, (unsigned long long)p->est_observations,
          (unsigned long long)p->est_wall_ns, (unsigned long long)p->est_max_wall_ns);
  sv_perf_json_hist(f, "frame_interval", &p->interval);
  fputc(',', f);
  sv_perf_json_hist(f, "frame_work", &p->work);
  fprintf(f, ",\"pacing\":{\"waits\":%llu,\"wait_ns\":%llu,\"missed_deadlines\":%llu,\"resyncs\":%llu},", (unsigned long long)p->span[SV_SPAN_PACING_WAIT].count,
          (unsigned long long)p->span[SV_SPAN_PACING_WAIT].total_ns, (unsigned long long)p->missed_deadlines, (unsigned long long)p->resyncs);
  fputs("\"spans\":[", f);
  for (i = 0U; i < SV_SPAN_COUNT; ++i)
    fprintf(f, "%s{\"name\":\"%s\",\"class\":\"%s\",\"parent\":\"%s\",\"count\":%llu,\"total_ns\":%llu,\"max_ns\":%llu}", i ? "," : "", sv_perf_span_names[i],
            sv_perf_span_class[i], sv_perf_span_parent[i], (unsigned long long)p->span[i].count, (unsigned long long)p->span[i].total_ns,
            (unsigned long long)p->span[i].max_ns);
  fprintf(f, "],\"guest\":{\"master_ticks\":%llu},\"counters\":{", (unsigned long long)p->guest_master_ticks);
  for (i = 0U; i < SV_PERF_COUNTERS && m->counter_names[i] != NULL; ++i)
    fprintf(f, "%s\"%s\":%llu", i ? "," : "", m->counter_names[i], (unsigned long long)p->counters[i]);
  fputc('}', f);
  if (p->detailed && strcmp(m->privacy, "private") == 0) {
    n = p->history_total < SV_PERF_HISTORY ? p->history_total : SV_PERF_HISTORY;
    first = p->history_total - n;
    fprintf(f, ",\"timeline\":{\"capacity\":%u,\"recorded\":%llu,\"dropped_oldest\":%llu,\"frames\":[", SV_PERF_HISTORY, (unsigned long long)n, (unsigned long long)first);
    for (i = 0U; i < n; ++i) {
      const SvPerfFrame *x = &p->history[(first + i) % SV_PERF_HISTORY];
      fprintf(f, "%s[%llu,%u,%u,%u,%u,%u,%u]", i ? "," : "", (unsigned long long)x->frame, x->interval_us, x->work_us, x->emulation_us, x->present_us, x->frames, x->missed);
    }
    fputs("],\"columns\":[\"frame\",\"interval_us\",\"work_us\",\"emulation_us\",\"present_us\",\"frames\",\"missed\"],\"note\":\"one row per observation; frames > 1 rows cover several virtual frames and carry no per-frame time\"}", f);
  }
  fputs("},\"unavailable\":[", f);
  for (i = 0U; i < 8U && m->unavailable[i][0] != NULL; ++i)
    fprintf(f, "%s{\"metric\":\"%s\",\"reason\":\"%s\"}", i ? "," : "", m->unavailable[i][0], m->unavailable[i][1]);
  fputs("]}\n", f);
}

/* Writes the report to `path`. A report that cannot be written is reported on stderr and never changes the guest's result. Returns 0 or -1. */
static inline int sv_perf_write_file(const SvPerf *p, const SvPerfMeta *m, const char *path) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) { fprintf(stderr, "perf: cannot write the performance report\n"); return -1; }
  sv_perf_write_json(p, m, f);
  if (fclose(f) != 0) { fprintf(stderr, "perf: the performance report could not be written completely\n"); return -1; }
  return 0;
}

#endif
