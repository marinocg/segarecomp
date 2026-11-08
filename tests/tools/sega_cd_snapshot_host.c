/* SEG-132 (ADR 0279): a project-authored test host for the Sega CD live driver that exercises the state codec at the frame barrier. The viewer variant of a generated
 * program is linked with this file instead of the SDL3 viewer (tools/sega_cd_bios_build.py --viewer-host-source): it defines scd_viewer_main_bios, registers a host whose
 * "player" is a plan file, and runs the program's ordinary driver. No window, no wall clock.
 *
 *   SCD_SNAP_PLAN  one line per operation: `<poll> <op> [argument]`, executed at that poll of the driver (the frame barrier), in file order.
 *       snap N            capture into slot N                          restore N       restore from slot N       validate N   staged validation only
 *       dump N <path>     write slot N to a file                       slurp N <path>  read a file into slot N
 *       pad <hex>         the pad held from this poll on               quit            leave the run
 *       dirty             corrupt every restorable region of the machine (RAMs, backup RAM, PCM RAM, CDC buffer, registers, mixer, graphics ASIC ...); logs `X <sha>` of the dirty state
 *   SCD_SNAP_LOG   written at the end. Every operation: `S <poll> <op> <status>`. At every poll, after its operations: `H <onsets> <ticks> <sha256 of a fresh canonical capture>`.
 *       `F <onset> <frame sha256>` per presented frame; `A <onsets> <frames> <sha256>` for the audio delivered in the interval that began at the poll with that frame count;
 *       `R <onsets> <backup sha256> <work-ram sha256>` at every poll (the in-memory backup RAM and the main work RAM).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "scd_live.h"
#include "scd_snapshot.h"
#include "scd_viewer.h"

#define MAX_OPS 4096U
#define SLOTS 64U

typedef struct Op {
  uint64_t poll;
  char op[16];
  char arg[256];
  char token[32];
  unsigned number;
} Op;

static Op ops[MAX_OPS];
static size_t op_count, op_next;
static ScdSnapshotBuffer slots[SLOTS];
static ScdMachine *machine;
static FILE *logf;
static uint8_t pad;
static uint64_t polls, last_onsets;
static GenesisSha256 audio_sha;
static uint64_t audio_frames;
static int audio_open;

static void hexline(const uint8_t *p, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) fprintf(logf, "%02x", (unsigned)p[i]);
}

static void sha_of(const uint8_t *d, size_t n, uint8_t out[32]) {
  GenesisSha256 s;
  genesis_sha256_init(&s);
  genesis_sha256_update(&s, d, (uint32_t)n);
  genesis_sha256_final(&s, out);
}

static void flush_audio(void) {
  uint8_t digest[32];
  if (!audio_open && audio_frames == 0U) return;
  genesis_sha256_final(&audio_sha, digest);
  fprintf(logf, "A %llu %llu ", (unsigned long long)last_onsets, (unsigned long long)audio_frames);
  hexline(digest, 32U);
  fputc('\n', logf);
  genesis_sha256_init(&audio_sha);
  audio_frames = 0U;
  audio_open = 0;
}

static void load_plan(const char *path) {
  FILE *f = fopen(path, "r");
  char text[400];
  if (f == NULL) return;
  while (op_count < MAX_OPS && fgets(text, sizeof text, f) != NULL) {
    Op *o = &ops[op_count];
    unsigned long long poll = 0U;
    if (sscanf(text, "%llu %15s %31s %255s", &poll, o->op, o->token, o->arg) < 2) continue;
    o->poll = (uint64_t)poll;
    o->number = (unsigned)strtoul(o->token, NULL, 10);
    ++op_count;
  }
  fclose(f);
}

static void started(void *ctx, ScdMachine *m) {
  (void)ctx;
  machine = m;
}

static int host_poll(void *ctx, uint64_t boundary, ScdLivePoll *out) {
  uint8_t fresh[32], backup[32], work[32];
  ScdSnapshotBuffer buf;
  (void)ctx;
  (void)boundary;
  flush_audio();
  while (op_next < op_count && ops[op_next].poll <= polls) {
    Op *o = &ops[op_next++];
    const unsigned n = o->number < SLOTS ? o->number : 0U;
    if (strcmp(o->op, "snap") == 0) {
      ScdSnapshotStatus st;
      scd_snapshot_buffer_free(&slots[n]);
      st = scd_snapshot_capture(machine, NULL, &slots[n]);
      fprintf(logf, "S %llu snap %u %s %u\n", (unsigned long long)polls, n, scd_snapshot_status_text(st), (unsigned)slots[n].size);
    } else if (strcmp(o->op, "restore") == 0 || strcmp(o->op, "validate") == 0) {
      const ScdSnapshotStatus st = o->op[0] == 'r' ? scd_snapshot_restore(machine, slots[n].data, slots[n].size) : scd_snapshot_validate(machine, slots[n].data, slots[n].size, NULL);
      fprintf(logf, "S %llu %s %u %s\n", (unsigned long long)polls, o->op, n, scd_snapshot_status_text(st));
      if (o->op[0] == 'r' && st == SCD_SNAPSHOT_OK) {   /* the host side of a restore: nothing queued for the abandoned future */
        audio_open = 0; audio_frames = 0U; genesis_sha256_init(&audio_sha);
      }
    } else if (strcmp(o->op, "dump") == 0 || strcmp(o->op, "slurp") == 0) {
      FILE *f = fopen(o->arg, o->op[0] == 'd' ? "wb" : "rb");
      if (f == NULL) { fprintf(logf, "S %llu %s %u io-error\n", (unsigned long long)polls, o->op, n); continue; }
      if (o->op[0] == 'd') {
        fwrite(slots[n].data, 1U, slots[n].size, f);
      } else {
        uint8_t *data = (uint8_t *)malloc(SCD_SNAPSHOT_MAX_FILE_BYTES + 1U);
        const size_t got = data != NULL ? fread(data, 1U, SCD_SNAPSHOT_MAX_FILE_BYTES + 1U, f) : 0U;
        scd_snapshot_buffer_free(&slots[n]);
        slots[n].data = (uint8_t *)realloc(data, got != 0U ? got : 1U);
        slots[n].size = (uint32_t)got;
      }
      fclose(f);
      fprintf(logf, "S %llu %s %u ok\n", (unsigned long long)polls, o->op, n);
    } else if (strcmp(o->op, "dirty") == 0) {
      ScdSnapshotBuffer d;
      uint8_t x[32];
      machine->prg_ram[0x1234] ^= 0xFFU; machine->prg_ram[SCD_PRG_RAM_BYTES - 1U] ^= 0x5AU;
      machine->word_ram[0x2000] ^= 0xA5U; machine->work_ram[0x100] ^= 0x33U;
      if (machine->backup_ram_present) machine->backup_ram[7] ^= 0xFFU;
      machine->pcm.ram[0x321] ^= 0x77U; machine->pcm.env[3] ^= 0x11U; machine->pcm.addr[2] ^= 0x1000U;
      machine->cdc.buffer[0x55] ^= 0xC3U; machine->cdc.dac ^= 0x0101U; machine->cdd.rx[4] ^= 0x0FU;
      machine->ga.comcmd[3] ^= 0xBEEFU; machine->ga.comstat[5] ^= 0x1234U; machine->ga.led ^= 0x3U;
      machine->cpu[0].d[3] ^= 0xDEADBEEFU; machine->cpu[1].a[2] ^= 0x1000U; machine->cpu[1].sr ^= 0x0700U;
      machine->mix.held[0] ^= 0x1234; machine->mix.chunk_count = machine->mix.chunk_count > 3U ? 3U : machine->mix.chunk_count + 1U;
      machine->cdda.fader_volume = (uint16_t)(machine->cdda.fader_volume ^ 5U);
      machine->gfx.lines_left ^= 3U; machine->hint_counter ^= 0x11U;
      if (scd_snapshot_capture(machine, NULL, &d) == SCD_SNAPSHOT_OK) {
        sha_of(d.data, d.size, x);
        fprintf(logf, "X ");
        hexline(x, 32U);
        fputc('\n', logf);
        scd_snapshot_buffer_free(&d);
      }
    } else if (strcmp(o->op, "pad") == 0) {
      pad = (uint8_t)strtoul(o->token, NULL, 16);
    } else if (strcmp(o->op, "quit") == 0) {
      out->quit = 1U;
    }
  }
  last_onsets = machine->vdp_frames;
  if (scd_snapshot_capture(machine, NULL, &buf) == SCD_SNAPSHOT_OK) {
    sha_of(buf.data, buf.size, fresh);
    fprintf(logf, "H %llu %llu ", (unsigned long long)machine->vdp_frames, (unsigned long long)machine->now);
    hexline(fresh, 32U);
    fputc('\n', logf);
    scd_snapshot_buffer_free(&buf);
  } else {
    fprintf(logf, "H %llu %llu none\n", (unsigned long long)machine->vdp_frames, (unsigned long long)machine->now);
  }
  sha_of(machine->backup_ram, SCD_BACKUP_RAM_BYTES, backup);
  sha_of(machine->work_ram, SCD_WORK_RAM_BYTES, work);
  fprintf(logf, "R %llu ", (unsigned long long)machine->vdp_frames);
  hexline(backup, 32U);
  fputc(' ', logf);
  hexline(work, 32U);
  fputc('\n', logf);
  out->pad1 = pad;
  ++polls;
  return 0;
}

static int host_present(void *ctx, const struct GenesisFrameArtifact *frame, uint64_t onset) {
  char digest[65];
  (void)ctx;
  scd_frame_sha256(frame, digest);
  fprintf(logf, "F %llu %s\n", (unsigned long long)onset, digest);
  return 0;
}

static int host_pace(void *ctx, uint64_t frame_ticks, uint64_t ticks_per_second) {
  (void)ctx; (void)frame_ticks; (void)ticks_per_second;
  return 0;
}

static int host_audio(void *ctx, const int16_t *frames, uint32_t count) {
  (void)ctx;
  if (!audio_open) { genesis_sha256_init(&audio_sha); audio_open = 1; }
  genesis_sha256_update(&audio_sha, (const uint8_t *)frames, count * 4U);
  audio_frames += count;
  return 0;
}

int scd_viewer_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv) {
  static ScdLiveHost host;
  const char *plan = getenv("SCD_SNAP_PLAN");
  const char *log_path = getenv("SCD_SNAP_LOG");
  int rc;
  size_t i;
  logf = log_path != NULL ? fopen(log_path, "w") : stderr;
  if (logf == NULL) return 3;
  genesis_sha256_init(&audio_sha);
  if (plan != NULL) load_plan(plan);
  memset(&host, 0, sizeof host);
  host.poll = host_poll;
  host.present = host_present;
  host.pace = host_pace;
  host.audio = host_audio;
  host.started = started;
  scd_live_host_registered = &host;
  rc = scd_program_main_bios(program, authentic, argc, argv);
  scd_live_host_registered = NULL;
  flush_audio();
  for (i = 0; i < SLOTS; ++i) scd_snapshot_buffer_free(&slots[i]);
  if (logf != stderr) fclose(logf);
  return rc;
}
