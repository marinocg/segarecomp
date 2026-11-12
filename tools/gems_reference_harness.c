/*
 * Offline validation aid for the native GEMS replacement (ADR 0291, docs/testing/genesis-native-gems-validation.md).
 *
 * It drives an INDEPENDENT reference implementation of the GEMS 2.5 driver (the third-party "GEMSPlay" C port by Valley Bell, which is NOT part of this
 * repository and is never linked into any product; its path is given on the compiler command line) with the same sound commands a game sent, and writes
 * the chip accesses it produces in the same line format as the native driver's SEGARECOMP_AUDIO_WRITE_LOG, so tools/gems_reference_compare.py can compare
 * the two operation by operation. Nothing here is compiled by the product build or CI.
 *
 *   cc -std=gnu11 -w -D__cdecl= -include string.h -I<GEMSPlay dir> -o harness tools/gems_reference_harness.c <GEMSPlay dir>/GemsPlay.c
 *   harness <rom> <command script> <end master ticks> <output log> [<start master ticks>]
 * The optional start is the guest time the driver was booted at (the native driver's activation): the reference's VBlank ticks, and with them its
 * sub-beat accumulator, begin there, exactly as the real driver's do.
 *
 * Command script, one line per completed command: "<master ticks> <hex bytes of the FIFO command, start byte first>", and one line per window in which the 68000
 * held the Z80 bus: "H <start> <end>" (the Z80 does not run in it: work due inside waits for <end>, and a VBlank interrupt edge that falls inside it by at least one
 * scanline is lost, like the real Z80's). The harness schedules, on the
 * guest master clock: the VBlank interrupt, the commands, and the DAC routine at every YM2612 Timer A expiry (a minimal timer model of its own: it is the
 * reference side of the comparison, so it deliberately shares nothing with the native device).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stdtype.h"
#include "GemsPlay.h"

#define FRAME 896040ULL    /* 3420 x 262 */
#define ONSET 766080ULL    /* 3420 x 224 */
#define SAMPLE_TICKS 1008ULL

static FILE *g_log;
static uint64_t g_now;
static uint8_t g_latched[2];
static uint16_t g_timer_a;
static uint64_t g_expiry;      /* the next Timer A overflow, 0 = stopped; the timer free-runs: clearing its flag does not move it */
static int g_flag;
static int g_dac_idle;         /* the DAC routine left the flag set twice in a row: nothing paces on it until the timer / DAC registers are written again */

static void log_line(char kind, unsigned port, unsigned value) { fprintf(g_log, "%c %u %02X %llu\n", kind, port, value, (unsigned long long)g_now); }
static uint64_t timer_period(void) { return (uint64_t)(1024U - g_timer_a) * SAMPLE_TICKS; }
static void timer_advance(void) { while (g_expiry != 0 && g_now >= g_expiry) { g_flag = 1; g_expiry += timer_period(); } }

void YM2612_Write(UINT8 addr, UINT8 data) {
  const unsigned side = (addr >> 1) & 1U;
  timer_advance();
  log_line('Y', addr & 3U, data);
  if ((addr & 1U) == 0U) { g_latched[side] = data; return; }
  if (side == 0U) {
    const uint8_t reg = g_latched[0];
    if (reg == 0x24) g_timer_a = (uint16_t)((g_timer_a & 3U) | ((uint16_t)data << 2));
    else if (reg == 0x25) g_timer_a = (uint16_t)((g_timer_a & ~3U) | (data & 3U));
    else if (reg == 0x27) {
      if (data & 0x10U) g_flag = 0;
      if (!(data & 0x01U)) g_expiry = 0;
      else if (g_expiry == 0) g_expiry = (g_now / 7) * 7 + timer_period();   /* the chip counts in YM clocks (7 master ticks) */   /* the load bit going 0 -> 1 starts the count; a write while it runs does not restart it */
      g_dac_idle = 0;
    } else if (reg == 0x2B) g_dac_idle = 0;
  }
}
void YM2612_WriteA(UINT8 addr, UINT8 data) { YM2612_Write(addr, data); }
UINT8 YM2612_Read(UINT8 addr) {
  (void)addr;
  timer_advance();
  return (UINT8)g_flag;
}
void PSG_Write(UINT8 data) { log_line('P', 0, data); }
void DACPlay(UINT8 s, UINT8 f) { (void)s; (void)f; }
void DACStop(UINT8 f) { (void)f; }
void StartSignal(UINT8 n) { (void)n; }
void StopSignal(void) {}

/* The real driver loops continuously; here one pass handles at most one FIFO command, so a command or a tick needs as many passes as work is pending. */
static void run_loop(int passes) { int i; for (i = 0; i < passes; ++i) gems_loop(); }

typedef struct Command { uint64_t at; uint8_t bytes[32]; unsigned count; } Command;
typedef struct Hold { uint64_t start, end; } Hold;
#define LINE_TICKS 3420ULL

/* the end of the hold that contains t (0 when the Z80 is running at t) */
static uint64_t held_until(const Hold *holds, unsigned count, uint64_t t) {
  unsigned low = 0, high = count;   /* the last hold that starts at or before t (the windows are ordered and disjoint) */
  while (low < high) {
    const unsigned middle = low + (high - low) / 2;
    if (holds[middle].start <= t) low = middle + 1; else high = middle;
  }
  return low > 0 && t < holds[low - 1].end ? holds[low - 1].end : 0;
}

int main(int argc, char **argv) {
  FILE *rom_file, *script;
  uint8_t *rom;
  long rom_size;
  Command commands[4096];
  static Hold holds[400000];
  unsigned command_count = 0, next_command = 0, hold_count = 0;
  uint64_t end, next_vblank_index = 0;
  char line[512];
  if (argc != 5 && argc != 6) { fprintf(stderr, "usage: harness <rom> <script> <end master ticks> <log>\n"); return 2; }
  rom_file = fopen(argv[1], "rb");
  if (rom_file == NULL) return 3;
  fseek(rom_file, 0, SEEK_END); rom_size = ftell(rom_file); fseek(rom_file, 0, SEEK_SET);
  rom = (uint8_t *)malloc((size_t)rom_size + 16U);
  if (rom == NULL || fread(rom, 1, (size_t)rom_size, rom_file) != (size_t)rom_size) return 3;
  memset(rom + rom_size, 0, 16U);
  fclose(rom_file);
  script = fopen(argv[2], "r");
  if (script == NULL) return 3;
  while (fgets(line, sizeof line, script) != NULL && command_count < 4096U) {
    char *cursor = line;
    unsigned long long at;
    if (line[0] == 'H') {
      unsigned long long a = 0, b = 0;
      if (sscanf(line + 1, "%llu %llu", &a, &b) == 2 && hold_count < 400000U) { holds[hold_count].start = a; holds[hold_count].end = b; ++hold_count; }
      continue;
    }
    at = strtoull(cursor, &cursor, 10);
    Command *c = &commands[command_count];
    c->at = at; c->count = 0;
    while (*cursor != '\0' && c->count < 32U) {
      unsigned value;
      while (*cursor == ' ') ++cursor;
      if (sscanf(cursor, "%2x", &value) != 1) break;
      c->bytes[c->count++] = (uint8_t)value; cursor += 2;
    }
    if (c->count != 0U) ++command_count;
  }
  fclose(script);
  end = strtoull(argv[3], NULL, 10);
  g_log = fopen(argv[4], "wb");
  if (g_log == NULL) return 3;
  SetROMFile(rom);
  g_now = argc == 6 ? strtoull(argv[5], NULL, 10) : 0;
  { const uint64_t start = g_now; while (next_vblank_index * FRAME + ONSET < start) ++next_vblank_index; }
  gems_init();
  for (;;) {
    uint64_t vblank = next_vblank_index * FRAME + ONSET;
    uint64_t command = next_command < command_count ? commands[next_command].at : UINT64_MAX;
    uint64_t dac = (g_expiry != 0 && !g_dac_idle) ? g_expiry : UINT64_MAX;
    uint64_t t;
    int kind;
    if (held_until(holds, hold_count, vblank) != 0) {   /* the Z80 does not run while the 68000 holds its bus */
      const uint64_t resume = held_until(holds, hold_count, vblank);
      if (resume >= vblank + LINE_TICKS) { ++next_vblank_index; continue; }   /* the interrupt line fell inside the hold: lost */
      vblank = resume;
    }
    if (dac != UINT64_MAX && held_until(holds, hold_count, dac) != 0) dac = held_until(holds, hold_count, dac);
    /* at one instant: the DAC routine, then the tick, then a command (the driver's order) */
    t = dac; kind = 2;
    if (vblank < t) { t = vblank; kind = 0; }
    if (command < t) { t = command; kind = 1; }
    if (t == UINT64_MAX || t > end) break;
    g_now = t;
    timer_advance();
    if (kind == 0) { VBLINT(); ++next_vblank_index; run_loop(40); }
    else if (kind == 1) {
      unsigned i;
      for (i = 0; i < commands[next_command].count; ++i) WriteFIFOCommand(commands[next_command].bytes[i]);
      FinishCommandFIFO();
      ++next_command;
      run_loop(40);
    } else {
      DACME();
      if (g_flag) DACME();   /* at slow rates the routine skips every other call (it toggles itself); the main loop calls it again at once */
      if (g_flag) g_dac_idle = 1;
      run_loop(2);
    }
  }
  fclose(g_log);
  return 0;
}
