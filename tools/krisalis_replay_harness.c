/*
 * Replay harness of the native Krisalis replacement (ADR 0317): runs krisalis_native.c, without the game, against the 68000 stores and Z80 bus-hold windows that a real run
 * logged (SEGARECOMP_AUDIO_WRITE_LOG with SEGARECOMP_AUDIO_WRITE_LOG_RAM=all), and writes the chip operations and the commands it consumes in the same log format. The
 * independent reference (tools/krisalis_reference_check.py) is driven by the very same stores, so the two are compared on identical stimulus and a change of the replacement can
 * be measured in seconds. Validation tool only: it is not part of any program. Local data (the log, the ROM) is never committed.
 *
 *   krisalis_replay_harness <log> <rom> > replay.log
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
static void ctx_bank_write(void *c, uint8_t bit) {
  Context *x = (Context *)c;
  x->bank = ((uint32_t)(bit & 1u) << 8) | (x->bank >> 1);
}
static void ctx_command_tap(void *c, const uint8_t *bytes, unsigned count, uint64_t ticks) {
  unsigned i;
  (void)c;
  for (i = 0; i < count; ++i) printf("C %u %02X %llu\n", i, (unsigned)bytes[i], (unsigned long long)ticks);
}

int main(int argc, char **argv) {
  FILE *log, *rom_file;
  Context *x = (Context *)calloc(1, sizeof *x);
  KrisalisNativeHost host;
  char kind;
  unsigned port, value;
  unsigned long long ticks;
  unsigned revision = 0;
  int active = 0;
  if (argc != 3 || x == NULL) { fprintf(stderr, "usage: krisalis_replay_harness <log> <rom>\n"); return 2; }
  log = fopen(argv[1], "r");
  rom_file = fopen(argv[2], "rb");
  if (log == NULL || rom_file == NULL) { fprintf(stderr, "cannot open inputs\n"); return 2; }
  fseek(rom_file, 0, SEEK_END);
  x->rom_size = (size_t)ftell(rom_file);
  fseek(rom_file, 0, SEEK_SET);
  x->rom = (uint8_t *)malloc(x->rom_size);
  if (x->rom == NULL || fread(x->rom, 1, x->rom_size, rom_file) != x->rom_size) return 2;
  memset(&host, 0, sizeof host);
  host.context = x;
  host.ram = x->ram;
  host.rom_read = ctx_rom_read;
  host.ym_write = ctx_ym_write;
  host.bank_get = ctx_bank_get;
  host.bank_write = ctx_bank_write;
  host.command_tap = ctx_command_tap;
  while (fscanf(log, " %c %u %x %llu", &kind, &port, &value, &ticks) == 4) {
    switch (kind) {
      case 'W':
        x->ram[port & 0x1FFFu] = (uint8_t)value;
        printf("W %u %02X %llu\n", port, value, ticks);
        break;
      case 'A':
        revision = port;
        printf("A %u 00 %llu\n", port, ticks);
        break;
      case 'B':
        x->bank = port;
        printf("B %u 00 %llu\n", port, ticks);
        krisalis_native_start(&x->driver, &host, (uint8_t)revision, ticks);
        active = 1;
        break;
      case 'S':
        if (active) { (void)krisalis_native_run_to(&x->driver, ticks); krisalis_native_suspend(&x->driver, ticks); }
        printf("S 0 00 %llu\n", ticks);
        break;
      case 'U':
        if (active) krisalis_native_resume(&x->driver, ticks);
        printf("U 0 00 %llu\n", ticks);
        break;
      case 'E':
        if (active) (void)krisalis_native_run_to(&x->driver, ticks);
        printf("E 0 00 %llu\n", ticks);
        fprintf(stderr, "fault=%s commands=%llu rows=%llu ticks=%llu dac=%llu samples=%llu keys=%llu\n", krisalis_native_fault_name(x->driver.fault),
                (unsigned long long)x->driver.count_commands, (unsigned long long)x->driver.count_rows, (unsigned long long)x->driver.count_ticks,
                (unsigned long long)x->driver.count_dac_samples, (unsigned long long)x->driver.count_samples_started, (unsigned long long)x->driver.count_key_events);
        break;
      default: break;
    }
  }
  return x->driver.fault == KRISALIS_FAULT_NONE ? 0 : 1;
}
