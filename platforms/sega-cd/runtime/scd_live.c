/* SEG-078-T001 (ADR 0204): see scd_live.h. */
#include "scd_live.h"

#include <string.h>

/* The machine run to `target`, split into deadline slices exactly as the headless driver does (the result must not depend on them). */
typedef struct ScdSlicer {
  uint64_t deadline, state;
} ScdSlicer;

static ScdRunResult scd_live_run_to(ScdMachine *m, uint64_t target, const ScdLiveConfig *config, ScdSlicer *slicer) {
  ScdRunResult result;
  if (config->slice_ticks == 0U && !config->have_seed) return scd_machine_run(m, target);
  do {
    uint64_t step = config->slice_ticks;
    if (config->have_seed) { /* a deterministic pseudo-random partition: 1 tick .. ~2^40 ticks */
      slicer->state = slicer->state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      step = (slicer->state >> 24) & ((UINT64_C(1) << ((slicer->state >> 58) & 0x3FU) % 41U) - 1U);
      step += 1U;
    }
    slicer->deadline = slicer->deadline > UINT64_MAX - step ? UINT64_MAX : slicer->deadline + step;
    if (slicer->deadline > target) slicer->deadline = target;
    result = scd_machine_run(m, slicer->deadline);
  } while (result == SCD_RUN_DEADLINE && slicer->deadline < target);
  return result;
}

const char *scd_live_outcome_name(ScdLiveOutcome outcome) {
  switch (outcome) {
  case SCD_LIVE_QUIT: return "quit";
  case SCD_LIVE_MACHINE_STOP: return "machine_stop";
  case SCD_LIVE_DEADLINE: return "deadline";
  case SCD_LIVE_HOST_ERROR: return "host_error";
  case SCD_LIVE_RECORDING_CAPACITY: return "recording_capacity";
  case SCD_LIVE_RECORDING_REFUSED: return "recording_refused";
  }
  return "unknown";
}

static int scd_live_present(ScdMachine *m, const ScdLiveHost *host, ScdLiveSummary *s) {
  uint64_t onset = 0U;
  const struct GenesisFrameArtifact *frame = scd_vdp_present_take(m, &onset);
  if (frame == NULL) return 0;
  if (host->present != NULL && host->present(host->ctx, frame, onset) != 0) return -1;
  s->frames_presented += 1U;
  return 0;
}

static ScdLiveSummary scd_live_loop(ScdMachine *m, const ScdLiveHost *host, const ScdLiveConfig *config) {
  ScdLiveSummary s;
  ScdSlicer slicer;
  memset(&s, 0, sizeof s);
  memset(&slicer, 0, sizeof slicer);
  slicer.state = config->slice_seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
  s.result = SCD_RUN_DEADLINE;
  if (scd_vdp_present_enable(m) != 0 || host == NULL || host->poll == NULL) {
    s.outcome = SCD_LIVE_HOST_ERROR;
    return s;
  }
  for (;;) {
    ScdLivePoll poll;
    uint64_t boundary, target, ticks;
    memset(&poll, 0, sizeof poll);
    if (host->poll(host->ctx, s.boundaries, &poll) != 0) {
      s.outcome = SCD_LIVE_HOST_ERROR;
      return s;
    }
    if (poll.quit) {
      s.outcome = SCD_LIVE_QUIT;
      return s;
    }
    if (host->extension != NULL) (void)scd_vdp_present_set_extension(m, host->extension(host->ctx));
    if (poll.pause && !poll.step) { /* held: the machine, its input and its recording are untouched */
      s.paused_polls += 1U;
      continue;
    }
    if (poll.pause) s.steps += 1U;
    boundary = m->vdp_onset; /* nothing at or after this tick has run: the run below stops strictly before it */
    if (config->sample_pad) {
      const ScdLiveSample sample = scd_input_live_sample(m, boundary, poll.pad1);
      if (sample != SCD_LIVE_SAMPLE_OK) {
        s.outcome = sample == SCD_LIVE_SAMPLE_CAPACITY ? SCD_LIVE_RECORDING_CAPACITY : SCD_LIVE_RECORDING_REFUSED;
        return s;
      }
    }
    ticks = scd_frame_ticks();
    target = boundary > UINT64_MAX - ticks ? UINT64_MAX : boundary + ticks;
    if (target > config->until_tick) target = config->until_tick;
    s.result = scd_live_run_to(m, target, config, &slicer);
    s.boundaries += 1U;
    if (scd_live_present(m, host, &s) != 0) {
      s.outcome = SCD_LIVE_HOST_ERROR;
      return s;
    }
    if (s.result != SCD_RUN_DEADLINE) {
      s.outcome = SCD_LIVE_MACHINE_STOP;
      return s;
    }
    if (target >= config->until_tick) {
      s.outcome = SCD_LIVE_DEADLINE;
      return s;
    }
    if (host->pace != NULL && host->pace(host->ctx, ticks, SCD_TICKS_PER_SECOND) != 0) {
      s.outcome = SCD_LIVE_HOST_ERROR;
      return s;
    }
  }
}

ScdLiveSummary scd_live_run(ScdMachine *m, const ScdLiveHost *host, const ScdLiveConfig *config) {
  const ScdLiveSummary summary = scd_live_loop(m, host, config);
  if (host != NULL && host->finished != NULL) host->finished(host->ctx, &summary, m);
  return summary;
}
