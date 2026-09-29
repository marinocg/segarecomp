/* SEG-026-T001: headless frame-bounded execution-PC coverage run (see execution_coverage.h). */
#include "execution_coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vdp_render.h"

typedef struct GenesisExecutionCoverageSession {
  GenesisSha256 frame_stream;
  uint64_t produced;
  int skip_render;
} GenesisExecutionCoverageSession;

static GenesisExecutionCoverageSession *g_coverage_session;

/* Renders exactly like the capture/viewer producer; additionally folds each published frame digest into the
   session's frame-stream digest. Never reads or writes guest state. */
static int genesis_execution_coverage_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                               GenesisFrameArtifact *frame_out) {
  int status;
  if (g_coverage_session != NULL && g_coverage_session->skip_render) {
    /* Count the genuine virtual frame boundary without rendering: the runtime publishes nothing on a nonzero
       status and mutates nothing, exactly as for a failed render. */
    g_coverage_session->produced++;
    return 1;
  }
  status = genesis_vdp_produce_frame(vram, vsram, cram, registers, frame_out);
  if (status == 0 && g_coverage_session != NULL) {
    genesis_sha256_update(&g_coverage_session->frame_stream, frame_out->frame_digest, 32U);
    g_coverage_session->produced++;
  }
  return status;
}

static void genesis_execution_coverage_bitmap_digest(const uint8_t *bitmap, uint8_t out[32]) {
  GenesisSha256 sha;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, bitmap, GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES);
  genesis_sha256_final(&sha, out);
}

static void genesis_execution_coverage_state_digest(const GenesisRuntime *runtime, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t sr[2];
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)runtime->d, (uint32_t)sizeof(runtime->d));
  genesis_sha256_update(&sha, (const uint8_t *)runtime->a, (uint32_t)sizeof(runtime->a));
  genesis_sha256_update(&sha, (const uint8_t *)&runtime->usp, (uint32_t)sizeof(runtime->usp));
  sr[0] = (uint8_t)(runtime->sr >> 8);
  sr[1] = (uint8_t)runtime->sr;
  genesis_sha256_update(&sha, sr, 2U);
  genesis_sha256_update(&sha, (const uint8_t *)&runtime->pc, (uint32_t)sizeof(runtime->pc));
  genesis_sha256_update(&sha, runtime->work_ram, (uint32_t)sizeof(runtime->work_ram));
  genesis_sha256_update(&sha, (const uint8_t *)&runtime->devices, (uint32_t)sizeof(runtime->devices));
  genesis_sha256_update(&sha, (const uint8_t *)&runtime->scheduler, (uint32_t)sizeof(runtime->scheduler));
  genesis_sha256_final(&sha, out);
}

static int genesis_execution_coverage_write_private(const char *dir, const GenesisExecutionCoverage *coverage) {
  char path[GENESIS_EXECUTION_COVERAGE_PATH_MAX];
  FILE *file;
  uint64_t index;
  int written = snprintf(path, sizeof(path), "%s/coverage.bitmap", dir);
  if (written <= 0 || (size_t)written >= sizeof(path)) return 1;
  file = fopen(path, "wb");
  if (file == NULL) return 1;
  if (fwrite(coverage->bitmap, 1U, GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES, file) !=
          GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES ||
      fclose(file) != 0)
    return 1;
  written = snprintf(path, sizeof(path), "%s/witnesses.txt", dir);
  if (written <= 0 || (size_t)written >= sizeof(path)) return 1;
  file = fopen(path, "wb");
  if (file == NULL) return 1;
  /* One line per first entry: retirement ordinal, previous retired PC, new PC, cause. */
  for (index = 0U; index < coverage->witness_count; ++index) {
    const GenesisExecutionCoverageWitness *w = &coverage->witnesses[index];
    if (fprintf(file, "%llu %08x %08x %u\n", (unsigned long long)w->retirement_ordinal, (unsigned)w->previous_pc,
                (unsigned)w->pc, (unsigned)w->cause) < 0) {
      fclose(file);
      return 1;
    }
  }
  return fclose(file) != 0;
}

static void genesis_execution_coverage_epoch(GenesisExecutionCoverageResult *r, uint64_t frame, uint64_t dispatches,
                                             const GenesisExecutionCoverage *coverage) {
  GenesisExecutionCoverageEpoch *epoch;
  if (r->epoch_count >= GENESIS_EXECUTION_COVERAGE_MAX_EPOCHS) return;
  epoch = &r->epochs[r->epoch_count++];
  epoch->frame = frame;
  epoch->dispatches = dispatches;
  epoch->retirements = coverage != NULL ? coverage->retirement_count : 0U;
  epoch->distinct = coverage != NULL ? coverage->distinct_count : 0U;
  if (coverage != NULL) genesis_execution_coverage_bitmap_digest(coverage->bitmap, epoch->digest);
}

void genesis_execution_coverage_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                    const GenesisExecutionCoverageOptions *options,
                                    uint64_t total_dispatch_allowance, GenesisExecutionCoverageResult *r) {
  GenesisLiveFrameObserver observer;
  GenesisFrameArtifact latest;
  GenesisExecutionCoverageSession session;
  GenesisExecutionCoverage coverage;
  GenesisExecutionCoverage *attached = NULL;
  uint64_t next_epoch;
  if (r == NULL) return;
  memset(r, 0, sizeof(*r));
  if (runtime == NULL || dispatch == NULL || options == NULL || options->target_frames == 0U ||
      options->epoch_frames == 0U || total_dispatch_allowance == 0U || runtime->execution_coverage != NULL ||
      runtime->live_frame_observer != NULL || g_coverage_session != NULL) {
    r->outcome = GENESIS_EXECUTION_COVERAGE_INVALID_ARGUMENT;
    return;
  }
  memset(&coverage, 0, sizeof(coverage));
  if (options->coverage_enabled) {
    coverage.bitmap = (uint8_t *)calloc(GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES, 1U);
    if (options->witness_capacity != 0U) {
      coverage.witnesses = (GenesisExecutionCoverageWitness *)calloc((size_t)options->witness_capacity,
                                                                     sizeof(GenesisExecutionCoverageWitness));
      coverage.witness_capacity = coverage.witnesses != NULL ? options->witness_capacity : 0U;
    }
    if (coverage.bitmap == NULL || (options->witness_capacity != 0U && coverage.witnesses == NULL)) {
      free(coverage.bitmap);
      free(coverage.witnesses);
      r->outcome = GENESIS_EXECUTION_COVERAGE_IO_ERROR;
      return;
    }
    attached = &coverage;
  }
  memset(&latest, 0, sizeof(latest));
  memset(&session, 0, sizeof(session));
  genesis_sha256_init(&session.frame_stream);
  session.skip_render = options->skip_render;
  observer.producer = genesis_execution_coverage_producer;
  observer.latest = &latest;
  observer.sequence = 0U;
  g_coverage_session = &session;
  runtime->live_frame_observer = &observer;
  runtime->execution_coverage = attached;
  next_epoch = options->epoch_frames;
  r->outcome = GENESIS_EXECUTION_COVERAGE_RUNNER_EXHAUSTED;
  while (r->dispatches < total_dispatch_allowance) {
    r->transfer = genesis_runtime_step(runtime, dispatch);
    r->dispatches++;
    while (session.produced >= next_epoch && next_epoch <= options->target_frames) {
      genesis_execution_coverage_epoch(r, next_epoch, r->dispatches, attached);
      next_epoch += options->epoch_frames;
    }
    if (r->transfer.kind == GENESIS_STOP) { r->outcome = GENESIS_EXECUTION_COVERAGE_GUEST_STOP; break; }
    if (r->transfer.kind == GENESIS_COMPLETE) { r->outcome = GENESIS_EXECUTION_COVERAGE_GUEST_COMPLETE; break; }
    if (session.produced >= options->target_frames) { r->outcome = GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED; break; }
  }
  runtime->execution_coverage = NULL;
  runtime->live_frame_observer = NULL;
  g_coverage_session = NULL;
  r->frames_published = session.produced;
  genesis_sha256_final(&session.frame_stream, r->frame_stream_digest);
  genesis_execution_coverage_state_digest(runtime, r->final_state_digest);
  if (attached != NULL) {
    genesis_execution_coverage_bitmap_digest(coverage.bitmap, r->coverage_digest);
    if (options->private_dir != NULL && genesis_execution_coverage_write_private(options->private_dir, &coverage) != 0)
      r->outcome = GENESIS_EXECUTION_COVERAGE_IO_ERROR;
    r->coverage = coverage;
    r->coverage.bitmap = NULL;
    r->coverage.witnesses = NULL;
    free(coverage.bitmap);
    free(coverage.witnesses);
  }
}
