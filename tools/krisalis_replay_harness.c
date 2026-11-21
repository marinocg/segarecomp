/*
 * Replay harness of the native Krisalis replacement (ADR 0318): runs krisalis_native.c, without the game, against the 68000 stores and Z80 bus-hold windows that a real run
 * logged (SEGARECOMP_AUDIO_WRITE_LOG with SEGARECOMP_AUDIO_WRITE_LOG_RAM=all), and writes the chip operations and the commands it consumes in the same log format. The
 * independent reference (tools/krisalis_reference_check.py) is driven by the very same stores, so the two are compared on identical stimulus and a change of the replacement can
 * be measured in seconds. Validation tool only: it is not part of any program. Local data (the log, the ROM) is never committed.
 *
 *   krisalis_replay_harness <script> <rom> > replay.log
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "krisalis_native.h"

typedef struct Context {
  uint8_t ram[8192];
  uint8_t *rom;
  size_t rom_size;
  uint32_t bank;
  KrisalisNative driver;
} Context;

static int ctx_rom_read(void *c, uint32_t address, uint8_t *destination, uint32_t count) {
  const Context *x = (const Context *)c;
  if ((size_t)address + count > x->rom_size) return 0;
  memcpy(destination, x->rom + address, count);
  return 1;
}
static void ctx_ym_write(void *c, uint32_t port, uint8_t value, uint64_t ticks) {
  (void)c;
  printf("y %u %02X %llu\n", (unsigned)port, (unsigned)value, (unsigned long long)ticks);
}
static uint32_t ctx_bank_get(void *c) { return ((const Context *)c)->bank; }
static void ctx_bank_write(void *c, uint8_t bit, uint64_t ticks) {
  Context *x = (Context *)c;
  printf("K %u 00 %llu\n", (unsigned)(bit & 1u), (unsigned long long)ticks);   /* the driver's own bank-register write, same record as the game's validation log */
  x->bank = ((uint32_t)(bit & 1u) << 8) | (x->bank >> 1);
}

static void ctx_command_tap(void *c, const uint8_t *bytes, unsigned count, uint64_t ticks) {
  unsigned i;
  (void)c;
  for (i = 0; i < count; ++i) printf("C %u %02X %llu\n", i, (unsigned)bytes[i], (unsigned long long)ticks);
}

/* The 68000 of the logged run, as a closed loop (tools/krisalis_reference_check.py makes the script; the oracle runs the same loop over the original driver): the sound-RAM image at
 * activation, then one transaction per bus hold. A transaction that stores a command waits, polling with short holds, until the replacement has cleared the previous one. */
#define POLL_HOLD 1200ull
#define POLL_GAP 900ull

typedef struct Transaction { unsigned long long t, hold; int cmd; unsigned count; unsigned *offsets; unsigned *values; } Transaction;

int main(int argc, char **argv) {
  FILE *script, *rom_file;
  Context *x = (Context *)calloc(1, sizeof *x);
  KrisalisNativeHost host;
  Transaction *tx = NULL;
  size_t tx_count = 0, tx_capacity = 0, i;
  unsigned revision = 0, bank = 0, conflicts = 0, mailbox_cmd;
  unsigned long long activation = 0, horizon = 0, now;
  char kind;
  if (argc != 3 || x == NULL) { fprintf(stderr, "usage: krisalis_replay_harness <script> <rom>\n"); return 2; }
  script = fopen(argv[1], "r");
  rom_file = fopen(argv[2], "rb");
  if (script == NULL || rom_file == NULL) { fprintf(stderr, "cannot open inputs\n"); return 2; }
  fseek(rom_file, 0, SEEK_END);
  x->rom_size = (size_t)ftell(rom_file);
  fseek(rom_file, 0, SEEK_SET);
  x->rom = (uint8_t *)malloc(x->rom_size);
  if (x->rom == NULL || fread(x->rom, 1, x->rom_size, rom_file) != x->rom_size) return 2;
  while (fscanf(script, " %c", &kind) == 1) {
    unsigned a, b, c;
    unsigned long long t, h;
    if (kind == 'A') { if (fscanf(script, "%u %u %llu", &revision, &bank, &activation) != 3) return 2; }
    else if (kind == 'I') { if (fscanf(script, "%u %u", &a, &b) != 2) return 2; x->ram[a & 0x1FFFu] = (uint8_t)b; }
    else if (kind == 'E') { if (fscanf(script, "%llu", &horizon) != 1) return 2; }
    else if (kind == 'T') {
      Transaction *t_;
      unsigned j;
      if (fscanf(script, "%llu %llu %u %u", &t, &h, &a, &c) != 4) return 2;
      if (tx_count == tx_capacity) { tx_capacity = tx_capacity ? tx_capacity * 2 : 1024; tx = (Transaction *)realloc(tx, tx_capacity * sizeof *tx); if (tx == NULL) return 2; }
      t_ = &tx[tx_count++];
      t_->t = t; t_->hold = h; t_->cmd = (int)a; t_->count = c;
      t_->offsets = (unsigned *)malloc(c * sizeof(unsigned)); t_->values = (unsigned *)malloc(c * sizeof(unsigned));
      for (j = 0; j < c; ++j) if (fscanf(script, "%u %u", &t_->offsets[j], &t_->values[j]) != 2) return 2;
    } else return 2;
  }
  mailbox_cmd = revision == 1u ? 0x0E58u : revision == 2u ? 0x0F58u : 0x0FACu;
  memset(&host, 0, sizeof host);
  host.context = x;
  host.ram = x->ram;
  host.rom_read = ctx_rom_read;
  host.ym_write = ctx_ym_write;
  host.bank_get = ctx_bank_get;
  host.bank_write = ctx_bank_write;
  host.command_tap = ctx_command_tap;
  x->bank = bank;
  printf("A %u 00 %llu\nB %u 00 %llu\n", revision, activation, bank, activation);
  krisalis_native_start(&x->driver, &host, (uint8_t)revision, activation);
  now = activation;
  for (i = 0; i < tx_count; ++i) {
    unsigned j;
    unsigned long long t = tx[i].t > now ? tx[i].t : now;
    (void)krisalis_native_run_to(&x->driver, t);
    if (tx[i].cmd) {
      while (x->ram[mailbox_cmd] != 0u) {   /* the 68000 polls until the Z80 has taken the previous command */
        (void)krisalis_native_run_to(&x->driver, t);
        krisalis_native_suspend(&x->driver, t);
        printf("S 0 00 %llu\n", t);
        printf("U 0 00 %llu\n", t + POLL_HOLD);
        krisalis_native_resume(&x->driver, t + POLL_HOLD);
        t += POLL_HOLD + POLL_GAP;
        (void)krisalis_native_run_to(&x->driver, t);
        if (t > horizon + 1000000000ull) { ++conflicts; break; }
      }
    }
    (void)krisalis_native_run_to(&x->driver, t);
    krisalis_native_suspend(&x->driver, t);
    printf("S 0 00 %llu\n", t);
    for (j = 0; j < tx[i].count; ++j) {
      x->ram[tx[i].offsets[j] & 0x1FFFu] = (uint8_t)tx[i].values[j];
      printf("W %u %02X %llu\n", tx[i].offsets[j], tx[i].values[j], t);
    }
    printf("U 0 00 %llu\n", t + tx[i].hold);
    krisalis_native_resume(&x->driver, t + tx[i].hold);
    now = t + tx[i].hold;
  }
  if (horizon > now) (void)krisalis_native_run_to(&x->driver, horizon);
  printf("E 0 00 %llu\n", horizon);
  fprintf(stderr, "stalled_polls=%u fault=%s commands=%llu rows=%llu ticks=%llu dac=%llu samples=%llu keys=%llu\n", conflicts, krisalis_native_fault_name(x->driver.fault),
          (unsigned long long)x->driver.count_commands, (unsigned long long)x->driver.count_rows, (unsigned long long)x->driver.count_ticks,
          (unsigned long long)x->driver.count_dac_samples, (unsigned long long)x->driver.count_samples_started, (unsigned long long)x->driver.count_key_events);
  if (getenv("KRISALIS_DUMP_MAILBOX")) { unsigned q; fprintf(stderr, "mailbox:"); for (q = 0; q < 0x48u; ++q) fprintf(stderr, " %02x", x->ram[(mailbox_cmd + q) & 0x1FFFu]); fprintf(stderr, "\n"); }
  return x->driver.fault == KRISALIS_FAULT_NONE ? 0 : 1;
}
