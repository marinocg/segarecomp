/* Master System headless driver: the generated program's `main` (SEG-009-T003; ADR 0064 sections 4, 5 and 8).
 *
 *   <game> [--cycle-budget <T>] [--frames <N>] [--input <script>] [--artifacts <dir>] [--bios <file>]
 *          [--slice-cycles <T>] [--slice-seed <S>]
 *
 * `--cycle-budget` stops at the first instruction boundary with `cycles >= T`; `--frames` stops at `T >= N x 59,736`
 * (both are absolute T-state deadlines for z80_run; there is no instruction counter and no `--instruction-budget`).
 * Automation always passes a finite bound; with no bound the program runs until the guest halts with interrupts
 * disabled (the consumer default). `--slice-cycles`/`--slice-seed` split the run into fixed or pseudo-random slices
 * (test hooks: the state digest must not change). `--artifacts <dir>` (directory must exist) receives `status.json`,
 * `state.sha256`, `irq.trace`, `mapper.trace` and `ram.bin`, plus whatever the linked device wiring adds through
 * `sms_write_device_artifacts` (the VDP unit: `vdp.trace`, `vram.bin`, `cram.bin`, `vdp.json`). `--bios` is rejected: the
 * BIOS is never executed.
 *
 * Exit status: 0 frame target reached (or the guest stopped normally with no bound); 2 cycle budget reached first;
 * 3 fail-closed Z80 outcome; 4 SMS_ERROR_*; 64 usage. Plain C11; the ROM is the build-time embedded array. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_audio.h"
#include "sms_machine.h"
#include "sms_psg.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;
void sms_install_devices(SmsMachine *machine);
int sms_write_device_artifacts(SmsMachine *machine, const char *dir); /* device artifacts (VDP trace, VRAM, ...) */

#define IRQ_CAPACITY 65536u
#define MAPPER_CAPACITY 65536u
#define INPUT_CAPACITY 65536u
#define INPUT_MAX_BYTES (1u << 20)

static SmsMachine machine;
static SmsPsg psg_device;         /* T007: attached after sms_install_devices unless that unit installed a PSG */
static SmsAudioCapture audio;
static SmsIrqTraceEntry irq_buffer[IRQ_CAPACITY];
static SmsMapperTraceEntry mapper_buffer[MAPPER_CAPACITY];
static SmsInputEvent input_events[INPUT_CAPACITY];

static int parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long v;
  if (text == NULL || *text < '0' || *text > '9') return 0;
  v = strtoull(text, &end, 10);
  if (end == NULL || *end != '\0') return 0;
  *out = (uint64_t)v;
  return 1;
}

static int write_file(const char *dir, const char *name, const void *data, size_t size) {
  char path[1024];
  FILE *f;
  int ok;
  if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
  f = fopen(path, "wb");
  if (f == NULL) return 0;
  ok = size == 0 || fwrite(data, 1, size, f) == size;
  return (fclose(f) == 0) && ok;
}

static int write_text(const char *dir, const char *name, const char *text) { return write_file(dir, name, text, strlen(text)); }

static void hex(const uint8_t *bytes, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; ++i) {
    out[2u * i] = digits[bytes[i] >> 4];
    out[2u * i + 1u] = digits[bytes[i] & 15u];
  }
  out[2u * n] = '\0';
}

static int write_artifacts(const char *dir, const SmsStop *stop, const char *digest_hex) {
  static char buffer[IRQ_CAPACITY * 48u > MAPPER_CAPACITY * 32u ? IRQ_CAPACITY * 48u : MAPPER_CAPACITY * 32u];
  static const char *const source_names[3] = {"frame", "line", "pause"};
  static const char *const event_names[3] = {"asserted", "accepted", "deasserted"};
  char text[1024];
  size_t used = 0;
  uint32_t i;
  int ok = 1;
  snprintf(text, sizeof text,
           "{\"stop\":\"%s\",\"resumable\":%s,\"cycles\":%llu,\"frames_completed\":%llu,\"z80_outcome\":\"%s\","
           "\"pc\":%u,\"image_identity\":%u,\"sms_error\":\"%s\",\"error_address\":%u,\"error_value\":%u,"
           "\"error_cycles\":%llu,\"irq_trace_entries\":%u,\"irq_trace_dropped\":%u,\"mapper_trace_entries\":%u,"
           "\"mapper_trace_dropped\":%u}\n",
           sms_stop_kind_name(stop->kind), sms_stop_is_resumable(stop->kind) ? "true" : "false",
           (unsigned long long)stop->cycles, (unsigned long long)stop->frame, z80_outcome_name(stop->z80_outcome),
           (unsigned)stop->pc, (unsigned)stop->image_identity, sms_error_name(stop->sms_error),
           (unsigned)stop->error_address, (unsigned)stop->error_value, (unsigned long long)stop->error_cycles,
           machine.irq_count, machine.irq_dropped, machine.mapper_count, machine.mapper_dropped);
  ok &= write_text(dir, "status.json", text);
  snprintf(text, sizeof text, "%s\n", digest_hex);
  ok &= write_text(dir, "state.sha256", text);
  for (i = 0; i < machine.irq_count; ++i) {
    const SmsIrqTraceEntry *e = &irq_buffer[i];
    used += (size_t)snprintf(buffer + used, sizeof buffer - used, "%llu %s %s\n", (unsigned long long)e->cycles,
                             source_names[e->source % 3u], event_names[e->event % 3u]);
  }
  ok &= write_file(dir, "irq.trace", buffer, used);
  used = 0;
  for (i = 0; i < machine.mapper_count; ++i) {
    const SmsMapperTraceEntry *e = &mapper_buffer[i];
    used += (size_t)snprintf(buffer + used, sizeof buffer - used, "%llu %04X %02X\n", (unsigned long long)e->cycles,
                             (unsigned)(SMS_MAPPER_REG_BASE + e->reg), (unsigned)e->value);
  }
  ok &= write_file(dir, "mapper.trace", buffer, used);
  ok &= write_file(dir, "ram.bin", machine.mem.ram, sizeof machine.mem.ram);
  ok &= sms_write_device_artifacts(&machine, dir);
  return ok;
}

static int load_input(const char *path) {
  static char text[INPUT_MAX_BYTES];
  FILE *f = fopen(path, "rb");
  size_t size;
  uint32_t count = 0;
  unsigned bad;
  if (f == NULL) return 0;
  size = fread(text, 1, sizeof text, f);
  if (!feof(f)) {
    fclose(f);
    return 0;
  }
  fclose(f);
  bad = sms_input_parse(text, size, input_events, INPUT_CAPACITY, &count);
  if (bad != 0u) {
    fprintf(stderr, "%s: malformed input script at line %u\n", path, bad);
    return 0;
  }
  sms_machine_set_input(&machine, input_events, count);
  return 1;
}

static int is_terminal(const SmsStop *stop) { return !sms_stop_is_resumable(stop->kind) || stop->kind == SMS_STOP_HALT_IDLE; }

int main(int argc, char **argv) {
  uint64_t cycle_budget = SMS_NO_LIMIT, frames = SMS_NO_LIMIT, slice_cycles = 0, slice_seed = 0;
  int have_slice_seed = 0;
  int psg_attached = 0;
  const char *input_path = NULL;
  const char *artifacts = NULL;
  SmsStop stop;
  SmsError init;
  uint8_t digest[32];
  char digest_hex[65];
  int i;
  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--cycle-budget") == 0 && parse_u64(value, &cycle_budget) && cycle_budget != 0u) ++i;
    else if (strcmp(arg, "--frames") == 0 && parse_u64(value, &frames)) ++i;
    else if (strcmp(arg, "--slice-cycles") == 0 && parse_u64(value, &slice_cycles) && slice_cycles != 0u) ++i;
    else if (strcmp(arg, "--slice-seed") == 0 && parse_u64(value, &slice_seed)) {
      have_slice_seed = 1;
      ++i;
    } else if (strcmp(arg, "--input") == 0 && value != NULL) input_path = argv[++i];
    else if (strcmp(arg, "--artifacts") == 0 && value != NULL) artifacts = argv[++i];
    else if (strcmp(arg, "--bios") == 0) {
      printf("sms_error %s\n", sms_error_name(SMS_ERROR_BIOS_UNSUPPORTED)); /* the BIOS is never executed or supplied */
      return 4;
    } else {
      fprintf(stderr, "usage: %s [--cycle-budget <T>] [--frames <N>] [--input <script>] [--artifacts <dir>]\n", argv[0]);
      return 64;
    }
  }
  init = sms_machine_init(&machine, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family);
  if (init != SMS_OK) {
    printf("sms_error %s\n", sms_error_name(init));
    return 4;
  }
  sms_machine_set_traces(&machine, irq_buffer, IRQ_CAPACITY, mapper_buffer, MAPPER_CAPACITY);
  sms_install_devices(&machine);
  if (machine.psg.write == NULL) {
    const Sn76489PcmConfig pcm_config = sn76489_pcm_config_44100_sms();
    const int capture = artifacts != NULL && sms_audio_capture_open(&audio, artifacts, &pcm_config, SMS_CYCLES_PER_FRAME);
    if (artifacts != NULL && !capture) {
      fprintf(stderr, "cannot write artifacts to %s\n", artifacts);
      return 64;
    }
    psg_attached = sms_psg_attach(&psg_device, &machine, capture ? sms_audio_capture_sink : NULL, &audio);
  }
  if (input_path != NULL && !load_input(input_path)) return 64;
  machine.stop_on_halt_idle = cycle_budget == SMS_NO_LIMIT && frames == SMS_NO_LIMIT;
  sms_machine_reset(&machine); /* device resets run after the devices are attached */
  if (frames != SMS_NO_LIMIT && frames > UINT64_MAX / SMS_CYCLES_PER_FRAME) return 64;
  {
    const uint64_t frame_limit = frames == SMS_NO_LIMIT ? SMS_NO_LIMIT : frames * SMS_CYCLES_PER_FRAME;
    const uint64_t final_cycle = cycle_budget < frame_limit ? cycle_budget : frame_limit;
    uint64_t rng = slice_seed * 6364136223846793005ull + 1442695040888963407ull;
    if (slice_cycles != 0u || have_slice_seed) {
      stop = sms_run_until_cycle(&machine, 0);
      while (!is_terminal(&stop) && machine.rt.state.cycles < final_cycle) {
        uint64_t chunk = slice_cycles;
        uint64_t target;
        if (have_slice_seed) {
          rng = rng * 6364136223846793005ull + 1442695040888963407ull;
          chunk = 1u + (rng >> 33) % 70000u;
        }
        target = machine.rt.state.cycles + chunk;
        if (target > final_cycle || target < machine.rt.state.cycles) target = final_cycle;
        stop = sms_run_until_cycle(&machine, target);
      }
      if (!is_terminal(&stop)) stop = sms_run_bounded(&machine, cycle_budget, frames);
    } else {
      stop = sms_run_bounded(&machine, cycle_budget, frames);
    }
  }
  if (psg_attached) sms_psg_sync(&psg_device, stop.cycles); /* PCM completes to the stop boundary; part of the digest */
  sms_machine_digest(&machine, digest);
  hex(digest, sizeof digest, digest_hex);
  printf("stop %s cycles %llu frames %llu digest %s\n", sms_stop_kind_name(stop.kind), (unsigned long long)stop.cycles,
         (unsigned long long)stop.frame, digest_hex);
  if (stop.kind == SMS_STOP_Z80_ERROR) printf("z80_outcome %s pc %04X identity %u\n", z80_outcome_name(stop.z80_outcome), (unsigned)stop.pc, (unsigned)stop.image_identity);
  if (stop.kind == SMS_STOP_PLATFORM_ERROR)
    printf("sms_error %s address %04X value %02X cycles %llu pc %04X\n", sms_error_name(stop.sms_error),
           (unsigned)stop.error_address, (unsigned)stop.error_value, (unsigned long long)stop.error_cycles, (unsigned)stop.pc);
  if (psg_attached && audio.pcm_file != NULL && !sms_audio_capture_close(&audio)) {
    fprintf(stderr, "cannot write audio artifacts to %s\n", artifacts);
    return 64;
  }
  if (artifacts != NULL && !write_artifacts(artifacts, &stop, digest_hex)) {
    fprintf(stderr, "cannot write artifacts to %s\n", artifacts);
    return 64;
  }
  switch (stop.kind) {
    case SMS_STOP_Z80_ERROR: return 3;
    case SMS_STOP_PLATFORM_ERROR: return 4;
    case SMS_STOP_CYCLE_BUDGET: return 2;
    default: return 0;
  }
}
