/*
 * SEG-032-T002 (ADR 0073): the generic Z80 image-epoch probe. Mirrors the SEG-026-T001 execution-coverage seam: the probe
 * build (tools/genesis_z80_materialization_probe.py) compiles the UNMODIFIED generated main TU with
 * -Dgenesis_runtime_run=genesis_z80_epoch_probe_hook_run, so the generated main hands its own GenesisRuntime, dispatcher
 * and runner allowance to this function. It installs a GenesisZ80EpochObserver, runs the generated-native program from
 * reset for a bounded number of virtual frames (and at most SEGARECOMP_Z80PROBE_STOP_AT epochs), and writes each epoch's
 * 8 KiB RAM snapshot and written-bytes bitmap to a private directory. Options arrive via environment variables so the
 * generated argv ABI is unchanged:
 *   SEGARECOMP_Z80PROBE_FRAMES   (> 0, the observation window in virtual frames; required)
 *   SEGARECOMP_Z80PROBE_STOP_AT  (0 = never, default 0; stop after this many epochs)
 *   SEGARECOMP_Z80PROBE_FILL     (0..255, the Z80 RAM power-on fill byte, default 0)
 *   SEGARECOMP_Z80PROBE_DIR      (private output directory; required)
 * Prints one EPOCH_PROBE_SUMMARY line of aggregates only (counts and virtual frame numbers; never a byte or an address).
 * Test/measurement seam only: it is not part of any shipped program.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

GenesisControlTransfer genesis_z80_epoch_probe_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                        uint32_t dispatch_allowance);

#define PROBE_MAX_EPOCHS 64U

typedef struct ProbeEpoch {
  uint32_t ordinal;
  uint64_t master_ticks;
  uint32_t written_bytes;
} ProbeEpoch;

typedef struct ProbeState {
  const char *dir;
  ProbeEpoch epochs[PROBE_MAX_EPOCHS];
  uint32_t count;
  uint64_t stop_at;
  int io_error;
  int stop_requested;
} ProbeState;

static int probe_parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  size_t index;
  if (text == NULL || text[0] == '\0') return 1;
  for (index = 0; text[index] != '\0'; ++index)
    if (text[index] < '0' || text[index] > '9') return 1;
  errno = 0;
  *out = (uint64_t)strtoull(text, &end, 10);
  return (errno != 0 || end == NULL || *end != '\0') ? 1 : 0;
}

static int probe_write(const char *dir, const char *kind, uint32_t ordinal, const uint8_t *data, size_t size) {
  char path[1024];
  FILE *file;
  int written;
  written = snprintf(path, sizeof(path), "%s/epoch-%03u.%s", dir, (unsigned)ordinal, kind);
  if (written < 0 || (size_t)written >= sizeof(path)) return 1;
  file = fopen(path, "wb");
  if (file == NULL) return 1;
  written = fwrite(data, 1U, size, file) == size ? 0 : 1;
  return (fclose(file) != 0) ? 1 : written;
}

static void probe_on_epoch(void *context, const GenesisZ80EpochEvent *event) {
  ProbeState *state = (ProbeState *)context;
  ProbeEpoch *slot;
  uint32_t index, bits = 0U;
  if (state->count >= PROBE_MAX_EPOCHS) { state->stop_requested = 1; return; }
  for (index = 0U; index < GENESIS_Z80_RAM_BYTES / 8U; ++index) {
    uint8_t byte = event->written_bitmap[index];
    while (byte != 0U) { bits += (uint32_t)(byte & 1U); byte >>= 1; }
  }
  slot = &state->epochs[state->count++];
  slot->ordinal = event->ordinal;
  slot->master_ticks = event->master_ticks;
  slot->written_bytes = bits;
  if (probe_write(state->dir, "ram", event->ordinal, event->ram, GENESIS_Z80_RAM_BYTES) != 0 ||
      probe_write(state->dir, "written", event->ordinal, event->written_bitmap, GENESIS_Z80_RAM_BYTES / 8U) != 0)
    state->io_error = 1;
  if (state->stop_at != 0U && state->count >= state->stop_at) state->stop_requested = 1;
}

GenesisControlTransfer genesis_z80_epoch_probe_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                        uint32_t dispatch_allowance) {
  static GenesisZ80EpochObserver observer;
  static ProbeState state;
  uint64_t frames = 0U, stop_at = 0U, fill = 0U, dispatches = 0U, window_ticks;
  const char *outcome = "budget_exhausted";
  GenesisControlTransfer transfer;
  uint32_t index;
  memset(&transfer, 0, sizeof(transfer));
  if (probe_parse_u64(getenv("SEGARECOMP_Z80PROBE_FRAMES"), &frames) != 0 || frames == 0U ||
      (getenv("SEGARECOMP_Z80PROBE_STOP_AT") != NULL && probe_parse_u64(getenv("SEGARECOMP_Z80PROBE_STOP_AT"), &stop_at) != 0) ||
      (getenv("SEGARECOMP_Z80PROBE_FILL") != NULL &&
       (probe_parse_u64(getenv("SEGARECOMP_Z80PROBE_FILL"), &fill) != 0 || fill > 255U)) ||
      getenv("SEGARECOMP_Z80PROBE_DIR") == NULL) {
    fprintf(stderr, "z80 epoch probe: malformed options\n");
    exit(3);
  }
  memset(runtime->devices.z80_bus.z80_ram, (int)fill, GENESIS_Z80_RAM_BYTES);
  state.dir = getenv("SEGARECOMP_Z80PROBE_DIR");
  state.stop_at = stop_at;
  observer.on_epoch = probe_on_epoch;
  observer.context = &state;
  runtime->z80_epoch_observer = &observer;
  window_ticks = frames * GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  while (dispatches < dispatch_allowance) {
    transfer = genesis_runtime_step(runtime, dispatch);
    ++dispatches;
    if (state.io_error) { outcome = "io_error"; break; }
    if (transfer.kind == GENESIS_STOP) { outcome = "guest_stop"; break; }
    if (transfer.kind == GENESIS_COMPLETE) { outcome = "guest_complete"; break; }
    if (state.stop_requested) { outcome = "epoch_cap"; break; }
    if (runtime->scheduler.master_ticks >= window_ticks) { outcome = "window_complete"; break; }
  }
  runtime->z80_epoch_observer = NULL;
  fprintf(stderr, "EPOCH_PROBE_SUMMARY {\"outcome\":\"%s\",\"window_frames\":%llu,\"virtual_frames\":%llu,\"dispatches\":%llu,"
                  "\"fill\":%llu,\"epochs\":[",
          outcome, (unsigned long long)frames,
          (unsigned long long)(runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME),
          (unsigned long long)dispatches, (unsigned long long)fill);
  for (index = 0U; index < state.count; ++index)
    fprintf(stderr, "%s{\"ordinal\":%u,\"frame\":%llu,\"written_bytes\":%u}", index == 0U ? "" : ",",
            (unsigned)state.epochs[index].ordinal,
            (unsigned long long)(state.epochs[index].master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME),
            (unsigned)state.epochs[index].written_bytes);
  fprintf(stderr, "]}\n");
  if (state.io_error) exit(4);
  if (strcmp(outcome, "window_complete") == 0 || strcmp(outcome, "epoch_cap") == 0 ||
      strcmp(outcome, "budget_exhausted") == 0) {
    memset(&transfer, 0, sizeof(transfer));
    transfer.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    transfer.next_pc = runtime->pc;
    transfer.runner_dispatch_count = (uint32_t)(dispatches >= UINT32_MAX ? UINT32_MAX - 1U : dispatches);
  }
  return transfer;
}
