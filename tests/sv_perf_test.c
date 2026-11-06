/* SEG-125 (ADR 0273): the runtime collector with synthetic data and a fake clock. No SDL, no window, no guest.
 * Covered: histogram accuracy against an exact sort incl. the >64 ms tail (documented error bound); empty/single-sample histograms; first-slice accounting; frames crossed per
 * observation (none, one, several, uneven coalesced); activation after the game started and toggling off/on; the unified missed-deadline count (aggregate == timeline), unthrottled,
 * pause; bounded history for sessions longer than its capacity; idle = no clock reads; report shape and size. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_perf.h"

static uint64_t g_now, g_reads;
static uint64_t fake_clock(void) { ++g_reads; return g_now; }
static int g_failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static SvPerf perf; /* static: the struct is ~65 KiB */

static int cmp64(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : (x > y ? 1 : 0); }
static uint64_t exact_percentile(uint64_t *v, size_t n, double q) {
  size_t rank = (size_t)(q * (double)n);
  if ((double)rank < q * (double)n) ++rank;
  if (rank == 0) rank = 1;
  qsort(v, n, sizeof *v, cmp64);
  return v[rank - 1];
}
/* the documented bound: never below the true value; at most one bucket above it: 50 us under 64 ms, 19% in the tail; exact in the overflow bucket */
static int within_bound(uint64_t got, uint64_t want) {
  const uint64_t slack = want < 64000000ULL ? SV_PERF_BUCKET_NS : want < SV_PERF_RANGE_NS ? want / 5U : 0U;
  return got >= want && got <= want + slack;
}
static uint64_t lcg(uint64_t *s) { *s = *s * 6364136223846793005ULL + 1442695040888963407ULL; return *s >> 33; }

static void check_distribution(const char *what, const uint64_t *samples, size_t n) {
  static SvPerfHistogram h;
  static uint64_t copy[4096];
  const double qs[] = {0.50, 0.95, 0.99};
  unsigned k;
  size_t i;
  sv_perf_hist_reset(&h);
  for (i = 0; i < n; ++i) sv_perf_hist_add(&h, samples[i]);
  memcpy(copy, samples, n * sizeof *samples);
  for (k = 0; k < 3; ++k) {
    const uint64_t got = sv_perf_hist_percentile(&h, qs[k]);
    const uint64_t want = exact_percentile(copy, n, qs[k]);
    if (!within_bound(got, want)) { fprintf(stderr, "FAIL %s q=%.2f got %llu want %llu\n", what, qs[k], (unsigned long long)got, (unsigned long long)want); ++g_failures; }
  }
  CHECK(sv_perf_hist_percentile(&h, 0.50) <= sv_perf_hist_percentile(&h, 0.95) && sv_perf_hist_percentile(&h, 0.95) <= sv_perf_hist_percentile(&h, 0.99) &&
        sv_perf_hist_percentile(&h, 0.99) <= h.max_ns);
  CHECK(h.count == n && h.min_ns == copy[0] && h.max_ns == copy[n - 1]);
}

static void test_histogram_distributions(void) {
  static uint64_t v[1000];
  uint64_t seed = 99U;
  size_t i;
  for (i = 0; i < 1000; ++i) v[i] = 16000000U + lcg(&seed) % 1000000U;                       /* normal 16-17 ms */
  check_distribution("normal", v, 1000);
  for (i = 0; i < 1000; ++i) v[i] = (i % 25U == 0U) ? 30000000U + lcg(&seed) % 30000000U : 16000000U + lcg(&seed) % 1000000U;   /* 4% slow 30-60 ms */
  check_distribution("slow frames", v, 1000);
  for (i = 0; i < 1000; ++i) v[i] = (i % 12U == 0U) ? 70000000U + lcg(&seed) % 130000000U : 16000000U + lcg(&seed) % 1000000U;  /* 8% stalls 70-200 ms: p95 inside the tail */
  check_distribution("stalls", v, 1000);
  for (i = 0; i < 1000; ++i) v[i] = (i % 40U == 0U) ? 500000000U + lcg(&seed) % 700000000U : 16000000U + lcg(&seed) % 1000000U;  /* outliers above 500 ms */
  check_distribution("outliers", v, 1000);
  for (i = 0; i < 1000; ++i) v[i] = (i % 10U == 0U) ? 90000000U + (i * 1000000U) % 400000000U : 17000000U;                      /* long tail: p95 far from max */
  check_distribution("long tail", v, 1000);
  {   /* p95/p99 do not collapse to the maximum because of values > 64 ms: 100 samples, 6 of 100..140 ms, one 900 ms */
    static SvPerfHistogram h;
    sv_perf_hist_reset(&h);
    for (i = 0; i < 93; ++i) sv_perf_hist_add(&h, 16000000U);
    for (i = 0; i < 6; ++i) sv_perf_hist_add(&h, 100000000U + i * 8000000U);
    sv_perf_hist_add(&h, 900000000U);
    CHECK(sv_perf_hist_percentile(&h, 0.95) >= 100000000U && sv_perf_hist_percentile(&h, 0.95) < 150000000U);
    CHECK(sv_perf_hist_percentile(&h, 0.99) < 160000000U && h.max_ns == 900000000U);
  }
  {   /* the overflow bucket (>= 8.192 s) reports the exact maximum; empty and single-sample histograms */
    static SvPerfHistogram h;
    sv_perf_hist_reset(&h);
    CHECK(sv_perf_hist_percentile(&h, 0.5) == 0U && h.count == 0U);
    sv_perf_hist_add(&h, 10ULL * 1000U * 1000U * 1000U);
    CHECK(h.bucket[SV_PERF_BUCKETS - 1U] == 1U && sv_perf_hist_percentile(&h, 0.99) == 10ULL * 1000U * 1000U * 1000U);
    sv_perf_hist_reset(&h);
    sv_perf_hist_add(&h, 16683333U);
    CHECK(sv_perf_hist_percentile(&h, 0.01) == 16683333U && sv_perf_hist_percentile(&h, 0.99) == 16683333U && h.min_ns == h.max_ns);
    CHECK(sv_perf_bucket_of(63999999ULL) == SV_PERF_LIN_BUCKETS - 1U && sv_perf_bucket_of(64000000ULL) == SV_PERF_LIN_BUCKETS && sv_perf_bucket_of(SV_PERF_RANGE_NS - 1U) == SV_PERF_LIN_BUCKETS + SV_PERF_TAIL_BUCKETS - 1U &&
          sv_perf_bucket_of(SV_PERF_RANGE_NS) == SV_PERF_BUCKETS - 1U);
  }
}

static void start(int detailed) { sv_perf_init(&perf, fake_clock, 16683333U, detailed); g_now = 5000000U; sv_perf_enable(&perf); }

/* a guest slice of `ms` that ends at virtual frame `vf` (no pacer in these tests unless stated) */
static void slice(double ms, uint64_t vf, uint64_t sleeps) {
  uint64_t t;
  sv_perf_observe_begin(&perf, perf.vf_last);   /* the adapter passes the guest's frame count at slice start; for the baseline only the first call matters */
  t = sv_perf_span_begin(&perf);
  g_now += (uint64_t)(ms * 1e6);
  sv_perf_span_end(&perf, SV_SPAN_EMULATION, t);
  sv_perf_observe_end(&perf, vf, sleeps);
}

static void test_first_slice_and_crossings(void) {
  /* 1. a run that ends inside its first slice, no boundary: nothing recorded, emulation counted */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  { const uint64_t t = sv_perf_span_begin(&perf); g_now += 3000000U; sv_perf_span_end(&perf, SV_SPAN_EMULATION, t); sv_perf_observe_end(&perf, 0U, 0U); }
  CHECK(perf.virtual_frames == 0U && perf.history_total == 0U && perf.span[SV_SPAN_EMULATION].total_ns == 3000000U);
  /* 2. the first slice crosses exactly one boundary: it IS counted, as an exact frame (the window began at frame 0) */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  g_now += 16000000U; sv_perf_observe_end(&perf, 1U, 0U);
  CHECK(perf.virtual_frames == 1U && perf.interval.count == 1U && perf.interval.min_ns == 16000000U && perf.estimated_frames == 0U && perf.history_total == 1U);
  /* 3. the first slice crosses several: all frames counted, none individually timed, ONE observation, not in the histograms */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  g_now += 50000000U; sv_perf_observe_end(&perf, 3U, 0U);
  CHECK(perf.virtual_frames == 3U && perf.interval.count == 0U && perf.estimated_frames == 3U && perf.est_observations == 1U && perf.est_wall_ns == 50000000U);
  CHECK(perf.history_total == 1U && perf.history[0].frames == 3U && perf.history[0].interval_us == 50000U);
  CHECK(perf.virtual_frames == perf.interval.count + perf.estimated_frames);
  /* 4. a multi-slice run, uneven durations: coalesced 2-frame observation, then exact ones, then a no-boundary slice carried into the next frame */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  slice(10.0, 0U, 0U);          /* no boundary */
  slice(8.0, 1U, 0U);           /* boundary: the frame took 18 ms (both slices) */
  slice(40.0, 3U, 0U);          /* two boundaries in one slice: 40 ms, uneven vs the next */
  slice(15.0, 4U, 0U);
  CHECK(perf.virtual_frames == 4U && perf.interval.count == 2U && perf.estimated_frames == 2U);
  CHECK(perf.interval.min_ns == 15000000U && perf.interval.max_ns == 18000000U && perf.history_total == 3U);
  CHECK(perf.history[0].interval_us == 18000U && perf.history[0].emulation_us == 18000U && perf.history[1].frames == 2U && perf.history[1].interval_us == 40000U && perf.history[2].frames == 1U);
  CHECK(perf.virtual_frames == perf.interval.count + perf.estimated_frames);
  /* 5. profiling activated after the game started: the first boundary is a PARTIAL frame (counted, not timed); the next is exact; nothing before activation is attributed */
  sv_perf_init(&perf, fake_clock, 16683333U, 1);
  g_now = 9000000U;
  perf.active = 1;                                   /* what the overlay does (perf_sync) */
  sv_perf_observe_begin(&perf, 1000U);               /* the guest already ran 1000 frames */
  g_now += 7000000U; sv_perf_observe_end(&perf, 1001U, 0U);
  g_now += 16000000U; sv_perf_observe_end(&perf, 1002U, 0U);
  CHECK(perf.virtual_frames == 2U && perf.estimated_frames == 1U && perf.interval.count == 1U && perf.interval.min_ns == 16000000U && perf.history[0].partial == 1U);
  /* 6. toggled off and on: the inactive time is never counted, and no frame is counted twice */
  perf.active = 0;
  sv_perf_observe_begin(&perf, 1002U); sv_perf_observe_end(&perf, 1010U, 0U);   /* inactive entry points ignore everything */
  CHECK(perf.virtual_frames == 2U && !perf.observing);
  g_now += 5000000000ULL;                            /* five inactive seconds */
  perf.active = 1;
  sv_perf_observe_begin(&perf, 1500U);               /* the guest advanced 498 frames meanwhile */
  g_now += 9000000U; sv_perf_observe_end(&perf, 1501U, 0U);
  g_now += 16500000U; sv_perf_observe_end(&perf, 1502U, 0U);
  CHECK(perf.virtual_frames == 4U && perf.estimated_frames == 2U && perf.interval.count == 2U && perf.interval.max_ns == 16500000U && perf.est_wall_ns == 16000000U);
}

static void test_missed_deadlines(void) {
  uint64_t sleeps = 0U;
  uint32_t i;
  /* paced: every boundary sleeps except two consecutive ones; the verdict of a boundary is settled when its pacer has run */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  for (i = 1U; i <= 6U; ++i) {
    g_now += 16000000U;
    sv_perf_observe_end(&perf, i, sleeps);
    if (i != 3U && i != 4U) ++sleeps;                 /* the pacer ran after the boundary: a sleep happened, or not (deadline already past) */
    sv_perf_pace_settle(&perf, sleeps, 0);
  }
  CHECK(perf.missed_deadlines == 2U && perf.history[2].missed == 1U && perf.history[3].missed == 1U && perf.history[0].missed == 0U && perf.history[5].missed == 0U);
  {   /* aggregate == sum over the timeline (within its coverage) */
    uint64_t sum = 0U; for (i = 0U; i < perf.history_total; ++i) sum += perf.history[i].missed;
    CHECK(sum == perf.missed_deadlines);
  }
  /* unthrottled: no pacer, no deadlines */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  for (i = 1U; i <= 4U; ++i) { g_now += 1000000U; sv_perf_observe_end(&perf, i, 0U); sv_perf_pace_settle(&perf, 0U, 1); }
  CHECK(perf.missed_deadlines == 0U && perf.virtual_frames == 4U);
  /* a coalesced observation: 3 boundaries, the pacer slept for one of them: 2 missed, attributed to the observation; aggregate still equals the timeline */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  g_now += 48000000U; sv_perf_observe_end(&perf, 3U, 10U);
  sv_perf_pace_settle(&perf, 11U, 0);
  CHECK(perf.missed_deadlines == 2U && perf.history[0].missed == 2U && perf.history[0].frames == 3U);
  /* the last observation is settled at the end of the session */
  g_now += 16000000U; sv_perf_observe_end(&perf, 4U, 11U);
  sv_perf_pace_settle(&perf, 11U, 0);
  CHECK(perf.missed_deadlines == 3U && perf.history[1].missed == 1U);
  /* pause/resume: the pause is not a slow frame, the interrupted frame is partial (counted, not timed), the pacer's resync is not a miss by itself */
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  g_now += 16000000U; sv_perf_observe_end(&perf, 1U, 0U); sv_perf_pace_settle(&perf, 1U, 0);
  { const uint64_t from = sv_perf_span_begin(&perf); g_now += 60000000000ULL; sv_perf_resync(&perf, from); }
  g_now += 5000000U; sv_perf_observe_end(&perf, 2U, 1U); sv_perf_pace_settle(&perf, 2U, 0);
  CHECK(perf.paused_ns == 60000000000ULL && perf.pauses == 1U && perf.interval.max_ns == 16000000U && perf.estimated_frames == 1U && perf.history[1].partial == 1U && perf.missed_deadlines == 0U);
}

static void test_idle_reads_no_clock(void) {
  uint64_t t;
  sv_perf_init(&perf, fake_clock, 16683333U, 0);
  g_reads = 0;
  t = sv_perf_span_begin(&perf);
  CHECK(t == 0U && !perf.active);
  sv_perf_span_end(&perf, SV_SPAN_EMULATION, t);
  sv_perf_observe_begin(&perf, 5U);
  sv_perf_observe_end(&perf, 9U, 3U);
  sv_perf_pace_settle(&perf, 4U, 0);
  sv_perf_finish(&perf);
  sv_perf_resync(&perf, 0);
  CHECK(g_reads == 0U && perf.virtual_frames == 0U && !perf.observing);
}

static void test_bounded_history_and_json(void) {
  enum { FRAMES = SV_PERF_HISTORY * 5U + 17U };
  static char path[] = "sv_perf_test_report.json";
  static char text[96 * 1024];
  SvPerfMeta meta;
  FILE *f;
  long size;
  size_t got;
  uint32_t i;
  start(1);
  sv_perf_observe_begin(&perf, 0U);
  for (i = 0; i < FRAMES; ++i) {
    uint64_t t = sv_perf_span_begin(&perf);
    g_now += 9000000U; sv_perf_span_end(&perf, SV_SPAN_EMULATION, t);
    t = sv_perf_span_begin(&perf); g_now += 7000000U; sv_perf_span_end(&perf, SV_SPAN_PACING_WAIT, t);
    sv_perf_observe_end(&perf, i + 1U, i);
    sv_perf_pace_settle(&perf, i + 1U, 0);
  }
  sv_perf_finish(&perf);
  CHECK(perf.virtual_frames == FRAMES && perf.history_total == FRAMES && sizeof perf.history == SV_PERF_HISTORY * sizeof(SvPerfFrame));
  CHECK(perf.interval.min_ns == 16000000U && perf.interval.max_ns == 16000000U && perf.work.max_ns == 9000000U && perf.missed_deadlines == 0U);
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
  CHECK(size > 0 && size < 64 * 1024);
  fseek(f, 0, SEEK_SET); got = fread(text, 1, sizeof text - 1, f); text[got] = '\0';
  CHECK(strstr(text, "\"schema\":\"segarecomp.performance\"") != NULL && strstr(text, "\"timeline\"") != NULL && strstr(text, "\"dropped_oldest\":") != NULL);
  CHECK(strstr(text, "\"estimated\":0") != NULL && strstr(text, "\"range_ns\":8192000000") != NULL && strstr(text, "\"columns\":[\"frame\",\"interval_us\",\"work_us\",\"emulation_us\",\"present_us\",\"frames\",\"missed\",\"partial\"]") != NULL);
  CHECK(strstr(text, "\"unavailable\":[{\"metric\":\"gpu_time\"") != NULL);
  fclose(f);
  meta.privacy = "shareable";
  f = fopen(path, "wb"); sv_perf_write_json(&perf, &meta, f); fclose(f);
  f = fopen(path, "rb"); got = fread(text, 1, sizeof text - 1, f); text[got] = '\0'; fclose(f);
  CHECK(strstr(text, "\"timeline\"") == NULL && strstr(text, "\"p95_ns\"") != NULL);
  remove(path);
}

int main(void) {
  test_histogram_distributions();
  test_first_slice_and_crossings();
  test_missed_deadlines();
  test_idle_reads_no_clock();
  test_bounded_history_and_json();
  if (g_failures == 0) puts("sv_perf_test: ok");
  return g_failures == 0 ? 0 : 1;
}
