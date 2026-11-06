/* SEG-125 (ADR 0273): the runtime collector with synthetic data and a fake clock: percentile accuracy against an exact sort, bounded history, idle = no clock
 * reads, coalesced slices, pauses, and the shape/size of the JSON report. No SDL, no window, no guest. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_perf.h"

static uint64_t g_now, g_reads;
static uint64_t fake_clock(void) { ++g_reads; return g_now; }

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static int cmp64(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : (x > y ? 1 : 0); }

static uint64_t exact_percentile(uint64_t *v, size_t n, double q) {
  size_t rank = (size_t)(q * (double)n);
  if ((double)rank < q * (double)n) ++rank;
  if (rank == 0) rank = 1;
  qsort(v, n, sizeof *v, cmp64);
  return v[rank - 1];
}

static SvPerf perf; /* static: the struct is ~100 KiB */

static void test_percentiles(void) {
  enum { N = 5000 };
  static uint64_t samples[N];
  uint64_t state = 12345U, i;
  const double qs[] = {0.50, 0.95, 0.99};
  unsigned k;
  sv_perf_init(&perf, fake_clock, 16683333U, 0);
  sv_perf_enable(&perf);
  for (i = 0; i < N; ++i) { /* a 16.7 ms base with 1-in-50 slow frames of 25..45 ms */
    uint64_t v;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    v = 16000000U + (state >> 40) % 1500000U;
    if (i % 50U == 7U) v = 25000000U + (state >> 33) % 20000000U;
    samples[i] = v;
    sv_perf_hist_add(&perf.interval, v);
  }
  CHECK(perf.interval.count == N);
  for (k = 0; k < 3; ++k) {
    const uint64_t got = sv_perf_hist_percentile(&perf.interval, qs[k]);
    const uint64_t want = exact_percentile(samples, N, qs[k]);
    CHECK(got >= want);                               /* upper edge of the bucket holding the exact value */
    CHECK(got - want <= SV_PERF_BUCKET_NS);           /* never more than one bucket above it */
  }
  {
    uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
    for (i = 0; i < N; ++i) { if (samples[i] < lo) lo = samples[i]; if (samples[i] > hi) hi = samples[i]; sum += samples[i]; }
    CHECK(perf.interval.min_ns == lo && perf.interval.max_ns == hi && perf.interval.sum_ns == sum); /* exact, not bucketed */
  }
}

static void test_overflow_bucket(void) {
  SvPerfHistogram h;
  sv_perf_hist_reset(&h);
  sv_perf_hist_add(&h, 10ULL * 1000U * 1000U * 1000U); /* 10 s: lands in the last bucket but max stays exact */
  CHECK(h.bucket[SV_PERF_BUCKETS - 1U] == 1U);
  CHECK(sv_perf_hist_percentile(&h, 0.99) == 10ULL * 1000U * 1000U * 1000U);
  sv_perf_hist_reset(&h);
  CHECK(sv_perf_hist_percentile(&h, 0.5) == 0U);
}

static void test_idle_reads_no_clock(void) {
  uint64_t t;
  sv_perf_init(&perf, fake_clock, 16683333U, 0);
  g_reads = 0;
  CHECK(!perf.active);
  t = sv_perf_span_begin(&perf);
  CHECK(t == 0U);
  sv_perf_span_end(&perf, SV_SPAN_EMULATION, t);
  sv_perf_frames(&perf, 1, 1, 0);
  sv_perf_finish(&perf);
  sv_perf_resync(&perf, 0);
  CHECK(g_reads == 0U);                     /* idle collector: not a single clock read */
  CHECK(perf.virtual_frames == 0U);
}

static void test_bounded_history_and_json(void) {
  enum { FRAMES = SV_PERF_HISTORY * 5U + 17U };
  static char path[] = "sv_perf_test_report.json";
  SvPerfMeta meta;
  FILE *f;
  long size;
  uint32_t i;
  sv_perf_init(&perf, fake_clock, 16683333U, 1);
  g_now = 1000; sv_perf_enable(&perf);
  for (i = 0; i < FRAMES; ++i) {
    uint64_t t;
    t = sv_perf_span_begin(&perf); g_now += 9000000U; sv_perf_span_end(&perf, SV_SPAN_EMULATION, t);
    t = sv_perf_span_begin(&perf); g_now += 7000000U; sv_perf_span_end(&perf, SV_SPAN_PACING_WAIT, t);
    sv_perf_frames(&perf, i + 1U, 1, 0);
  }
  sv_perf_finish(&perf);
  CHECK(perf.virtual_frames == FRAMES);
  CHECK(perf.history_total == FRAMES);       /* total counted, ring bounded */
  CHECK(sizeof perf.history == SV_PERF_HISTORY * sizeof(SvPerfFrame));
  CHECK(perf.interval.min_ns == 16000000U && perf.interval.max_ns == 16000000U);
  CHECK(perf.work.max_ns == 9000000U);       /* work = interval - pacing wait */
  CHECK(perf.span[SV_SPAN_PACING_WAIT].total_ns == (uint64_t)FRAMES * 7000000U);
  CHECK(sv_perf_recent_interval_ns(&perf, 120) == 16000000U);
  memset(&meta, 0, sizeof meta);
  meta.platform = "genesis"; meta.entry = "headless"; meta.status = "complete"; meta.outcome = "runner_resource_limit"; meta.privacy = "private"; meta.paced = 1;
  meta.counter_names[0] = "m68k_dispatches"; perf.counters[0] = 42U;
  meta.unavailable[0][0] = "gpu_time"; meta.unavailable[0][1] = "not measured";
  f = fopen(path, "wb"); CHECK(f != NULL);
  if (f == NULL) return;
  sv_perf_write_json(&perf, &meta, f);
  fclose(f);
  f = fopen(path, "rb"); CHECK(f != NULL);
  fseek(f, 0, SEEK_END); size = ftell(f);
  CHECK(size > 0 && size < 64 * 1024);       /* a bounded report however long the session ran */
  { static char text[64 * 1024]; size_t got; fseek(f, 0, SEEK_SET); got = fread(text, 1, sizeof text - 1, f); text[got] = '\0';
    CHECK(strstr(text, "\"schema\":\"segarecomp.performance\"") != NULL);
    CHECK(strstr(text, "\"timeline\"") != NULL);               /* detailed + private exports the recent timeline */
    CHECK(strstr(text, "\"dropped_oldest\":") != NULL);
    CHECK(strstr(text, "\"unavailable\":[{\"metric\":\"gpu_time\"") != NULL); }
  fclose(f);
  /* shareable: no timeline, same aggregates */
  meta.privacy = "shareable";
  f = fopen(path, "wb"); sv_perf_write_json(&perf, &meta, f); fclose(f);
  f = fopen(path, "rb");
  { static char text[64 * 1024]; size_t got = fread(text, 1, sizeof text - 1, f); text[got] = '\0'; CHECK(strstr(text, "\"timeline\"") == NULL); CHECK(strstr(text, "\"p95_ns\"") != NULL); }
  fclose(f);
  remove(path);
}

static void test_coalesced_and_pause(void) {
  sv_perf_init(&perf, fake_clock, 16683333U, 0);
  g_now = 5000; sv_perf_enable(&perf);
  g_now += 40000000U;
  sv_perf_frames(&perf, 3, 3, 0);            /* one slice crossed three boundaries */
  CHECK(perf.virtual_frames == 3U && perf.coalesced_frames == 2U);
  CHECK(perf.interval.max_ns == 40000000U / 3U);
  {
    const uint64_t from = sv_perf_span_begin(&perf);
    g_now += 5000000000ULL;                  /* a 5 s pause in the menu */
    sv_perf_resync(&perf, from);
  }
  g_now += 16000000U;
  sv_perf_frames(&perf, 4, 1, 0);
  CHECK(perf.interval.max_ns == 16000000U);  /* the pause never became a frame time */
  CHECK(perf.paused_ns == 5000000000ULL && perf.pauses == 1U);
}

int main(void) {
  test_percentiles();
  test_overflow_bucket();
  test_idle_reads_no_clock();
  test_bounded_history_and_json();
  test_coalesced_and_pause();
  if (g_failures == 0) puts("sv_perf_test: ok");
  return g_failures == 0 ? 0 : 1;
}
