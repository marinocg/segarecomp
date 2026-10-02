/*
 * SEG-032-T008 (ADR 0073): the build-time Z80 image materialization pass. `segarecomp build` links this file in place of
 * genesis_sound_hook.c (same generated units, same image registry, same runtime, same devices) and runs the program headless from
 * reset with no input under a finite instruction budget and a wall timeout. It observes only generic hardware events: the image
 * epochs (the runnable transition after a reset/upload epoch). A known image executes generated-native; an unknown activation
 * signature makes the Z80 machine stop with z80_unknown_image after this file has written the snapshot. The pass never decodes
 * a Z80 byte. Options arrive via environment variables so the generated argv ABI is unchanged:
 *   SEGARECOMP_MATERIALIZE_DIR     private work directory (required)
 *   SEGARECOMP_MATERIALIZE_FRAMES  observation window in virtual frames (> 0, required)
 * Files written to the directory: pass.report (line oriented, see below) and, only for an unknown image, unknown.ram (8,192 bytes)
 * and unknown.written (1,024 bytes). pass.report:
 *   outcome <window_complete|guest_stop|guest_complete|budget_exhausted|unknown_image|z80_code_mismatch|z80_stop|io_error>
 *   frames <virtual frames reached>      dispatches <retired dispatches>
 *   sound_fault z80_code_mismatch <epochs seen> <master ticks>     (only when the Z80 was isolated by a structural mutation)
 *   epoch <ordinal> <64 hex: activation signature> <extent count>      (one line per epoch, activation order)
 * SEG-028-T005 (ADR 0077): for outcome guest_stop the pass also writes the private stop record stop.ram: 4-byte big-endian stop PC,
 * 4-byte big-endian stop class, then the 64 KiB work RAM (the SEGARECOMP_STOP_WORK_RAM_DUMP layout of frame_capture_main_hook.c).
 * `segarecomp build` consumes it only to propose ADR 0049 immutable-copy aliases; it is never reported.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_sound.h"

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance);

#define PASS_MAX_EPOCHS 4096U

typedef struct PassEpoch {
  uint8_t signature[32];
  uint32_t extents;
} PassEpoch;

static struct {
  const char *dir;
  GenesisRuntime *runtime;
  PassEpoch epochs[PASS_MAX_EPOCHS];
  uint32_t count;
  int io_error;
  int overflow;
} g_pass;

static int pass_parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  size_t index;
  if (text == NULL || text[0] == '\0') return 1;
  for (index = 0; text[index] != '\0'; ++index)
    if (text[index] < '0' || text[index] > '9') return 1;
  errno = 0;
  *out = (uint64_t)strtoull(text, &end, 10);
  return (errno != 0 || end == NULL || *end != '\0') ? 1 : 0;
}

static int pass_write(const char *name, const uint8_t *data, size_t size) {
  char path[1024];
  FILE *file;
  int written = snprintf(path, sizeof(path), "%s/%s", g_pass.dir, name);
  if (written < 0 || (size_t)written >= sizeof(path)) return 1;
  file = fopen(path, "wb");
  if (file == NULL) return 1;
  written = fwrite(data, 1U, size, file) == size ? 0 : 1;
  return (fclose(file) != 0) ? 1 : written;
}

static void pass_on_epoch(void *context, const GenesisZ80EpochEvent *event) {
  PassEpoch *slot;
  (void)context;
  if (g_pass.count >= PASS_MAX_EPOCHS) { g_pass.overflow = 1; return; }
  slot = &g_pass.epochs[g_pass.count++];
  genesis_z80_activation_signature(event->ram, event->written_bitmap, &slot->extents, slot->signature);
}

static void pass_on_unknown(void *context, uint32_t epoch_ordinal, const uint8_t *ram, const uint8_t *written) {
  (void)context;
  (void)epoch_ordinal;
  if (pass_write("unknown.ram", ram, GENESIS_Z80_RAM_BYTES) != 0 ||
      pass_write("unknown.written", written, GENESIS_Z80_RAM_BYTES / 8U) != 0)
    g_pass.io_error = 1;
}

static int pass_write_stop_record(const GenesisRuntime *runtime, uint32_t stop_class) {
  static uint8_t record[8U + sizeof(runtime->work_ram)];
  const uint32_t pc = runtime->pc;
  record[0] = (uint8_t)(pc >> 24); record[1] = (uint8_t)(pc >> 16); record[2] = (uint8_t)(pc >> 8); record[3] = (uint8_t)pc;
  record[4] = (uint8_t)(stop_class >> 24); record[5] = (uint8_t)(stop_class >> 16);
  record[6] = (uint8_t)(stop_class >> 8); record[7] = (uint8_t)stop_class;
  memcpy(record + 8U, runtime->work_ram, sizeof(runtime->work_ram));
  return pass_write("stop.ram", record, sizeof(record));
}

static int pass_write_report(const char *outcome, uint64_t frames, uint64_t dispatches, const GenesisZ80Machine *machine) {
  char path[1024];
  FILE *file;
  uint32_t index, byte;
  int written = snprintf(path, sizeof(path), "%s/pass.report", g_pass.dir);
  if (written < 0 || (size_t)written >= sizeof(path)) return 1;
  file = fopen(path, "wb");
  if (file == NULL) return 1;
  fprintf(file, "outcome %s\nframes %llu\ndispatches %llu\n", outcome, (unsigned long long)frames, (unsigned long long)dispatches);
  if (genesis_z80_machine_sound_faulted(machine))  /* the Z80 was isolated (contract section 18); the run continued */
    fprintf(file, "sound_fault z80_code_mismatch %u %llu\n", (unsigned)machine->sound_fault_epoch,
            (unsigned long long)machine->sound_fault_master_ticks);
  for (index = 0U; index < g_pass.count; ++index) {
    fprintf(file, "epoch %u ", (unsigned)(index + 1U));
    for (byte = 0U; byte < 32U; ++byte) fprintf(file, "%02x", (unsigned)g_pass.epochs[index].signature[byte]);
    fprintf(file, " %u\n", (unsigned)g_pass.epochs[index].extents);
  }
  return fclose(file) != 0 ? 1 : 0;
}

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance) {
  uint64_t frames = 0U, dispatches = 0U, window_ticks;
  const char *outcome = "budget_exhausted";
  GenesisControlTransfer transfer;
  GenesisZ80Machine *machine;
  memset(&transfer, 0, sizeof(transfer));
  if (getenv("SEGARECOMP_MATERIALIZE_DIR") == NULL || pass_parse_u64(getenv("SEGARECOMP_MATERIALIZE_FRAMES"), &frames) != 0 ||
      frames == 0U) {
    fprintf(stderr, "materialization pass: malformed options\n");
    exit(3);
  }
  g_pass.dir = getenv("SEGARECOMP_MATERIALIZE_DIR");
  g_pass.runtime = runtime;
  machine = genesis_sound_attach(runtime);
  machine->on_unknown_image = pass_on_unknown;
  runtime->z80_epoch.on_epoch = pass_on_epoch;
  window_ticks = frames * GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  while (dispatches < dispatch_allowance) {
    transfer = genesis_runtime_step(runtime, dispatch);
    ++dispatches;
    if (g_pass.io_error || g_pass.overflow) { outcome = "io_error"; break; }
    if (transfer.kind == GENESIS_COMPLETE) { outcome = "guest_complete"; break; }
    if (transfer.kind == GENESIS_STOP) {
      if (transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION) {
        const int diagnostic = (int)transfer.stop.diagnostic_category;
        outcome = diagnostic == (int)GENESIS_DIAG_Z80_UNKNOWN_IMAGE ? "unknown_image"
                  : diagnostic == (int)GENESIS_DIAG_Z80_CODE_MISMATCH ? "z80_code_mismatch" : "z80_stop";
      } else {
        outcome = "guest_stop";
        if (pass_write_stop_record(runtime, (uint32_t)transfer.stop.stop_class) != 0) g_pass.io_error = 1;
      }
      break;
    }
    if (runtime->scheduler.master_ticks >= window_ticks) { outcome = "window_complete"; break; }
  }
  runtime->z80_epoch.on_epoch = NULL;
  machine->on_unknown_image = NULL;
  if (g_pass.io_error || g_pass.overflow) outcome = "io_error";
  if (pass_write_report(outcome, runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME, dispatches, machine) != 0) exit(4);
  if (strcmp(outcome, "io_error") == 0) exit(4);
  if (transfer.kind != GENESIS_STOP && transfer.kind != GENESIS_COMPLETE) {
    memset(&transfer, 0, sizeof(transfer));
    transfer.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    transfer.next_pc = runtime->pc;
    transfer.runner_dispatch_count = (uint32_t)(dispatches >= UINT32_MAX ? UINT32_MAX - 1U : dispatches);
  }
  return transfer;
}
