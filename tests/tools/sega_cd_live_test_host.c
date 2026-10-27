/* SEG-078-T001 (ADR 0204): a project-authored test host for the Sega CD live driver. The viewer variant of a generated program is linked
 * with this file instead of the SDL3 viewer (tools/sega_cd_bios_build.py --viewer-host-source): it defines scd_viewer_main_bios, registers
 * a host whose "keyboard" is a script, and runs the program's ordinary driver. No window, no wall clock.
 *
 *   SCD_TEST_HOST_SCRIPT  one line per poll of the driver, in order: `<pad hex byte> [P][S][Q]` - the pad mask held, P = pause,
 *                         S = single step (with P), Q = quit. After the last line every poll quits.
 *   SCD_TEST_HOST_LOG     written at the end: `F <onset> <canonical frame sha256>` per presented frame, `P <poll> <boundary>` per poll that
 *                         was not a quit, `T <frame ticks> <ticks per second>` per pace call, `E <outcome>` at the end.
 *   SCD_TEST_HOST_FAIL    `poll`, `present` or `pace`: that callback fails (exit 24).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "scd_live.h"
#include "scd_viewer.h"

#define MAX_LINES 20000U
#define MAX_LOG 200000U

typedef struct Line {
  uint8_t pad, pause, step, quit;
} Line;

static Line lines[MAX_LINES];
static size_t line_count;
static char *log_lines;
static size_t log_used;
static const char *fail;

static void log_add(const char *format, unsigned long long a, unsigned long long b, const char *text) {
  char buffer[160];
  int n;
  if (text != NULL) n = snprintf(buffer, sizeof buffer, format, text);
  else n = snprintf(buffer, sizeof buffer, format, a, b);
  if (n > 0 && log_used + (size_t)n < MAX_LOG * 80U) {
    memcpy(log_lines + log_used, buffer, (size_t)n);
    log_used += (size_t)n;
  }
}

static int host_poll(void *ctx, uint64_t boundary, ScdLivePoll *out) {
  static size_t poll_index;
  (void)ctx;
  if (fail != NULL && strcmp(fail, "poll") == 0) return -1;
  if (poll_index >= line_count) {
    out->quit = 1U;
    return 0;
  }
  out->pad1 = lines[poll_index].pad;
  out->pause = lines[poll_index].pause;
  out->step = lines[poll_index].step;
  out->quit = lines[poll_index].quit;
  if (!out->quit) log_add("P %llu %llu\n", (unsigned long long)poll_index, (unsigned long long)boundary, NULL);
  ++poll_index;
  return 0;
}

static int host_present(void *ctx, const struct GenesisFrameArtifact *frame, uint64_t onset) {
  char digest[65];
  (void)ctx;
  if (fail != NULL && strcmp(fail, "present") == 0) return -1;
  scd_frame_sha256(frame, digest);
  {
    char buffer[160];
    const int n = snprintf(buffer, sizeof buffer, "F %llu %s\n", (unsigned long long)onset, digest);
    if (n > 0 && log_used + (size_t)n < MAX_LOG * 80U) {
      memcpy(log_lines + log_used, buffer, (size_t)n);
      log_used += (size_t)n;
    }
  }
  return 0;
}

static int host_pace(void *ctx, uint64_t frame_ticks, uint64_t ticks_per_second) {
  (void)ctx;
  if (fail != NULL && strcmp(fail, "pace") == 0) return -1;
  log_add("T %llu %llu\n", (unsigned long long)frame_ticks, (unsigned long long)ticks_per_second, NULL);
  return 0;
}

static void host_finished(void *ctx, const struct ScdLiveSummary *summary, const ScdMachine *machine) {
  (void)ctx;
  (void)machine;
  log_add("E %s\n", 0U, 0U, scd_live_outcome_name(summary->outcome));
}

static void load_script(const char *path) {
  FILE *f = fopen(path, "r");
  char text[128];
  if (f == NULL) return;
  while (line_count < MAX_LINES && fgets(text, sizeof text, f) != NULL) {
    unsigned pad = 0U;
    char flags[16] = "";
    const int n = sscanf(text, "%x %15s", &pad, flags);
    Line *l = &lines[line_count];
    if (n < 1) continue;
    memset(l, 0, sizeof *l);
    l->pad = (uint8_t)pad;
    l->pause = strchr(flags, 'P') != NULL;
    l->step = strchr(flags, 'S') != NULL;
    l->quit = strchr(flags, 'Q') != NULL;
    ++line_count;
  }
  fclose(f);
}

int scd_viewer_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv) {
  static ScdLiveHost host;
  const char *script = getenv("SCD_TEST_HOST_SCRIPT");
  const char *log_path = getenv("SCD_TEST_HOST_LOG");
  int rc;
  fail = getenv("SCD_TEST_HOST_FAIL");
  log_lines = (char *)calloc(MAX_LOG, 80U);
  if (log_lines == NULL) return 3;
  if (script != NULL) load_script(script);
  host.poll = host_poll;
  host.present = host_present;
  host.pace = host_pace;
  host.finished = host_finished;
  scd_live_host_registered = &host;
  rc = scd_program_main_bios(program, authentic, argc, argv);
  scd_live_host_registered = NULL;
  if (log_path != NULL) {
    FILE *f = fopen(log_path, "w");
    if (f != NULL) {
      fwrite(log_lines, 1U, log_used, f);
      fclose(f);
    }
  }
  free(log_lines);
  return rc;
}
