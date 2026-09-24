#include "viewer.h"

#include <stddef.h>
#include <string.h>

#define NS_PER_SEC UINT64_C(1000000000)
#define PERIOD_NUM (GENESIS_NTSC_MASTER_TICKS_PER_FRAME * NS_PER_SEC)

uint64_t genesis_pacer_period_floor_ns(void) {
  return PERIOD_NUM / GENESIS_NTSC_MASTER_TICKS_PER_SECOND;
}

static int parse_u32(const char *s, uint32_t *out) {
  uint64_t v = 0;
  if (s == NULL || *s == '\0') return -1;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10u + (uint64_t)(*s - '0');
    if (v > UINT32_MAX) return -1;
  }
  if (v == 0) return -1;
  *out = (uint32_t)v;
  return 0;
}

int genesis_viewer_options_parse(int argc, const char *const *argv, GenesisViewerOptions *out) {
  if (out == NULL || argc < 0 || (argc > 0 && argv == NULL)) return -1;
  GenesisViewerOptions o;
  o.unthrottled = 0;
  o.slice_dispatches = GENESIS_VIEWER_DEFAULT_SLICE;
  for (int i = 0; i < argc; ++i) {
    if (argv[i] == NULL) return -1;
    if (strcmp(argv[i], "--viewer-unthrottled") == 0) {
      o.unthrottled = 1;
    } else if (strcmp(argv[i], "--viewer-slice") == 0) {
      if (i + 1 >= argc || parse_u32(argv[i + 1], &o.slice_dispatches) != 0) return -1;
      ++i;
    }
  }
  *out = o;
  return 0;
}

void genesis_pacer_init(GenesisPacer *pacer, int unthrottled) {
  if (pacer == NULL) return;
  memset(pacer, 0, sizeof(*pacer));
  pacer->unthrottled = unthrottled ? 1u : 0u;
}

uint8_t genesis_viewer_pad_from_keys(const GenesisViewerKeys *k) {
  if (k == NULL) return 0U;
  return (uint8_t)((k->up ? GENESIS_PAD_UP : 0U) | (k->down ? GENESIS_PAD_DOWN : 0U) |
                   (k->left ? GENESIS_PAD_LEFT : 0U) | (k->right ? GENESIS_PAD_RIGHT : 0U) |
                   (k->a ? GENESIS_PAD_A : 0U) | (k->b ? GENESIS_PAD_B : 0U) |
                   (k->c ? GENESIS_PAD_C : 0U) | (k->start ? GENESIS_PAD_START : 0U));
}

int genesis_pacer_wait(GenesisPacer *pacer, const GenesisViewerHost *host) {
  if (pacer == NULL || host == NULL || host->now_ns == NULL) return -1;
  if (!pacer->unthrottled && host->sleep_ns == NULL) return -1;
  uint64_t now = host->now_ns(host->ctx);
  if (pacer->started && now < pacer->last_now_ns) return -1; /* clock went backwards */
  uint64_t period = genesis_pacer_period_floor_ns();
  if (!pacer->started) {
    pacer->started = 1;
    pacer->deadline_ns = now;
    pacer->rem = 0;
  } else if (now > pacer->deadline_ns &&
             now - pacer->deadline_ns > GENESIS_VIEWER_MAX_LATE_PERIODS * (period + 1u)) {
    pacer->deadline_ns = now; /* bounded late-stall resync: no burst, no drift */
    pacer->rem = 0;
    pacer->resyncs++;
  }
  if (!pacer->unthrottled && now < pacer->deadline_ns) {
    host->sleep_ns(host->ctx, pacer->deadline_ns - now);
    pacer->sleep_calls++;
  }
  pacer->last_now_ns = now;
  /* advance accumulated deadline by the exact rational period */
  pacer->deadline_ns += period;
  pacer->rem += PERIOD_NUM % GENESIS_NTSC_MASTER_TICKS_PER_SECOND;
  if (pacer->rem >= GENESIS_NTSC_MASTER_TICKS_PER_SECOND) {
    pacer->rem -= GENESIS_NTSC_MASTER_TICKS_PER_SECOND;
    pacer->deadline_ns++;
  }
  return 0;
}

static GenesisViewerResult finish(GenesisViewerResult r, GenesisViewerOutcome o) {
  r.outcome = o;
  return r;
}

GenesisViewerResult genesis_viewer_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                       const GenesisViewerOptions *options,
                                       const GenesisViewerHost *host, GenesisPacer *pacer,
                                       uint64_t total_dispatch_allowance) {
  GenesisViewerResult r;
  memset(&r, 0, sizeof(r));
  if (runtime == NULL || dispatch == NULL || options == NULL || host == NULL || pacer == NULL ||
      host->present == NULL || host->window_closed == NULL || options->slice_dispatches == 0 ||
      total_dispatch_allowance == 0 || runtime->live_frame_observer == NULL ||
      runtime->live_frame_observer->latest == NULL)
    return finish(r, GENESIS_VIEWER_INVALID_ARGUMENT);
  const GenesisLiveFrameObserver *obs = runtime->live_frame_observer;
  uint64_t presented_seq = obs->sequence;
  for (;;) {
    if (host->window_closed(host->ctx)) return finish(r, GENESIS_VIEWER_WINDOW_CLOSED);
    genesis_runtime_set_pad1(runtime, host->pad1 != NULL ? host->pad1(host->ctx) : 0U);
    uint64_t remaining = total_dispatch_allowance - r.dispatches;
    if (remaining == 0) return finish(r, GENESIS_VIEWER_RUNNER_EXHAUSTED);
    uint32_t slice = remaining < options->slice_dispatches ? (uint32_t)remaining
                                                            : options->slice_dispatches;
    GenesisControlTransfer t = genesis_runtime_run(runtime, dispatch, slice);
    r.transfer = t;
    r.slices++;
    if (t.kind == GENESIS_RUNNER_RESOURCE_LIMIT)
      r.dispatches += t.runner_dispatch_count;
    else
      r.dispatches += 1; /* terminal slice: count is only a lower bound */
    /* Present the newest completed frame, if a new one exists. */
    if (obs->sequence != presented_seq && genesis_frame_artifact_is_populated(obs->latest)) {
      if (genesis_pacer_wait(pacer, host) != 0) return finish(r, GENESIS_VIEWER_HOST_ERROR);
      if (host->present(host->ctx, obs->latest) != 0) return finish(r, GENESIS_VIEWER_HOST_ERROR);
      presented_seq = obs->sequence;
      r.frames_presented++;
    }
    if (t.kind == GENESIS_STOP) return finish(r, GENESIS_VIEWER_GUEST_STOP);
    if (t.kind == GENESIS_COMPLETE) return finish(r, GENESIS_VIEWER_GUEST_COMPLETE);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) return finish(r, GENESIS_VIEWER_INVALID_ARGUMENT);
  }
}
