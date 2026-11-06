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
#include <string.h>

#define SV_PERF_SCHEMA_VERSION 1
#define SV_PERF_BUCKET_NS 50000U      /* 50 us resolution of the frame-time histograms */
#define SV_PERF_BUCKETS 1281U         /* 0 .. 64 ms in 50 us steps, the last bucket is "64 ms or more" */
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
  uint32_t bucket[SV_PERF_BUCKETS];
  uint64_t count, sum_ns, min_ns, max_ns;
} SvPerfHistogram;

typedef struct SvPerfFrame {
  uint64_t frame;       /* virtual frame ordinal at the boundary */
  uint32_t interval_us; /* wall time since the previous boundary (saturating) */
  uint32_t work_us;     /* interval minus pacing wait */
  uint32_t emulation_us, present_us;
  uint8_t missed;       /* the pacer did not wait: the deadline had already passed */
} SvPerfFrame;

typedef struct SvPerf {
  int active;            /* 0: every entry point is a single-branch no-op */
  int detailed;          /* 0: summary mode (histograms + totals), 1: the report also exports the recent-frame timeline */
  SvPerfClock now;
  uint64_t target_period_ns; /* the console's native frame period at 1.0x (documented constant, not measured) */
  uint64_t start_ns, last_boundary_ns, end_ns;
  uint64_t virtual_frames, frames_presented, coalesced_frames, missed_deadlines, resyncs, pacing_waits;
  uint64_t paused_ns, pauses;  /* time spent in the pause menu: not part of any frame interval */
  int pinned;                  /* 1 = a report was requested: stays active; 0 = active only while the overlay is shown */
  uint64_t guest_master_ticks; /* deterministic guest time; reported separately from every host duration */
  uint64_t counters[SV_PERF_COUNTERS];
  SvPerfHistogram interval, work;
  SvPerfSpan span[SV_SPAN_COUNT];
  uint64_t frame_pacing_ns, frame_emulation_ns, frame_present_ns; /* accumulated since the last boundary */
  SvPerfFrame history[SV_PERF_HISTORY];
  uint64_t history_total; /* frames ever recorded; the ring holds the last SV_PERF_HISTORY */
} SvPerf;

static const char *const sv_perf_span_names[SV_SPAN_COUNT] = {"emulation", "vdp_compose", "rgb_convert", "texture_upload", "present", "audio_present", "pacing_wait"};
static const char *const sv_perf_span_class[SV_SPAN_COUNT] = {"inclusive", "inclusive", "exclusive", "inclusive", "inclusive", "exclusive", "exclusive"};
static const char *const sv_perf_span_parent[SV_SPAN_COUNT] = {"", "emulation", "", "present", "", "", ""};

static inline void sv_perf_hist_reset(SvPerfHistogram *h) {
  memset(h, 0, sizeof *h);
  h->min_ns = UINT64_MAX;
}

static inline void sv_perf_hist_add(SvPerfHistogram *h, uint64_t ns) {
  uint64_t b = ns / SV_PERF_BUCKET_NS;
  if (b >= SV_PERF_BUCKETS) b = SV_PERF_BUCKETS - 1U;
  ++h->bucket[b];
  ++h->count;
  h->sum_ns += ns;
  if (ns < h->min_ns) h->min_ns = ns;
  if (ns > h->max_ns) h->max_ns = ns;
}

/* The smallest value v (upper edge of a bucket, clamped to the observed max) such that at least ceil(q * count) samples are <= v. q in (0,1]. */
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
  v = i >= SV_PERF_BUCKETS - 1U ? h->max_ns : ((uint64_t)i + 1U) * SV_PERF_BUCKET_NS; /* the overflow bucket has no upper edge: report the exact max */
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

/* `crossed` (>0) virtual-frame boundaries were crossed since the last call (a slice can cross several: the interval is shared evenly and
 * the extra frames are counted as coalesced). `missed` = the pacer had no time to wait for the deadline of the last frame. */
static inline void sv_perf_frames(SvPerf *p, uint64_t virtual_frame, uint64_t crossed, int missed) {
  uint64_t t, interval, work, per, per_work, i;
  if (!p->active || crossed == 0U) return;
  t = p->now();
  interval = t > p->last_boundary_ns ? t - p->last_boundary_ns : 0U;
  work = interval > p->frame_pacing_ns ? interval - p->frame_pacing_ns : 0U;
  per = interval / crossed;
  per_work = work / crossed;
  for (i = 0U; i < crossed; ++i) {
    sv_perf_hist_add(&p->interval, per);
    sv_perf_hist_add(&p->work, per_work);
  }
  p->virtual_frames += crossed;
  if (crossed > 1U) p->coalesced_frames += crossed - 1U;
  if (missed) ++p->missed_deadlines;
  { /* the fixed-size history is always kept (the live overlay reads it); only the report decides whether it is exported */
    SvPerfFrame *f = &p->history[p->history_total % SV_PERF_HISTORY];
    f->frame = virtual_frame;
    f->interval_us = sv_perf_us(interval);
    f->work_us = sv_perf_us(work);
    f->emulation_us = sv_perf_us(p->frame_emulation_ns);
    f->present_us = sv_perf_us(p->frame_present_ns);
    f->missed = missed ? 1U : 0U;
    ++p->history_total;
  }
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

/* Recent window for a live view (the overlay): average interval of the last `n` recorded frames, in ns; 0 without detail history. */
static inline uint64_t sv_perf_recent_interval_ns(const SvPerf *p, uint32_t n) {
  uint64_t sum = 0U, got = 0U, k;
  if (p->history_total == 0U) return 0U;
  if (n > SV_PERF_HISTORY) n = SV_PERF_HISTORY;
  for (k = 0U; k < n && k < p->history_total; ++k) {
    sum += (uint64_t)p->history[(p->history_total - 1U - k) % SV_PERF_HISTORY].interval_us * 1000U;
    ++got;
  }
  return got == 0U ? 0U : sum / got;
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
  fprintf(f, "\"%s\":{\"count\":%llu,\"min_ns\":%llu,\"avg_ns\":%llu,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,\"max_ns\":%llu,\"resolution_ns\":%u}", name,
          (unsigned long long)h->count, (unsigned long long)(h->count ? h->min_ns : 0U), (unsigned long long)(h->count ? h->sum_ns / h->count : 0U),
          (unsigned long long)sv_perf_hist_percentile(h, 0.50), (unsigned long long)sv_perf_hist_percentile(h, 0.95),
          (unsigned long long)sv_perf_hist_percentile(h, 0.99), (unsigned long long)h->max_ns, SV_PERF_BUCKET_NS);
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
  fprintf(f, "\"frames\":{\"virtual\":%llu,\"presented\":%llu,\"coalesced\":%llu},", (unsigned long long)p->virtual_frames, (unsigned long long)p->frames_presented,
          (unsigned long long)p->coalesced_frames);
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
  fputs("},\"unavailable\":[", f);
  for (i = 0U; i < 8U && m->unavailable[i][0] != NULL; ++i)
    fprintf(f, "%s{\"metric\":\"%s\",\"reason\":\"%s\"}", i ? "," : "", m->unavailable[i][0], m->unavailable[i][1]);
  fputc(']', f);
  if (p->detailed && strcmp(m->privacy, "private") == 0) {
    n = p->history_total < SV_PERF_HISTORY ? p->history_total : SV_PERF_HISTORY;
    first = p->history_total - n;
    fprintf(f, ",\"timeline\":{\"capacity\":%u,\"recorded\":%llu,\"dropped_oldest\":%llu,\"frames\":[", SV_PERF_HISTORY, (unsigned long long)n, (unsigned long long)first);
    for (i = 0U; i < n; ++i) {
      const SvPerfFrame *x = &p->history[(first + i) % SV_PERF_HISTORY];
      fprintf(f, "%s[%llu,%u,%u,%u,%u,%u]", i ? "," : "", (unsigned long long)x->frame, x->interval_us, x->work_us, x->emulation_us, x->present_us, (unsigned)x->missed);
    }
    fputs("],\"columns\":[\"frame\",\"interval_us\",\"work_us\",\"emulation_us\",\"present_us\",\"missed\"]}", f);
  }
  fputs("}}\n", f);
}

#endif
