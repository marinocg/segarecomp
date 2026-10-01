/*
 * SEG-032-T004 (ADR 0072): drives the Genesis Z80 view (platforms/genesis/runtime/z80_machine.c) with a generated RAM-backed
 * Z80 image on the REAL Genesis runtime, and exercises the 68K side of the Z80 area through genesis_route_access.
 *
 *   genesis_z80_view_harness <ram.hex> <scenario> <z80 cycles>
 * The scenario byte is stored at Z80 RAM $1F00 before the run (the test program branches on it). A synthetic project-authored
 * 128 KiB "cartridge" (byte i = (i * 31 + 7) & $FF) is the only owned region. Prints one RESULT line of decimal / hex fields.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80_machine.h"

static uint8_t rom[0x20000];
static GenesisOwnedCartridgeRegion region;
static GenesisRuntime runtime;
static GenesisZ80Machine machine;

static int route(uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction, uint32_t *value, GenesisDiagnosticCategory *diag) {
  GenesisRuntimeStop stop;
  memset(&stop, 0, sizeof stop);
  if (genesis_route_access(&runtime, address, width, direction, value, &stop) == GENESIS_ACCESS_OK) { *diag = (GenesisDiagnosticCategory)0; return 1; }
  *diag = stop.diagnostic_category;
  return 0;
}

int main(int argc, char **argv) {
  unsigned long scenario, cycles;
  size_t index;
  FILE *file;
  char pair[3] = {0, 0, 0};
  Z80Outcome outcome;
  if (argc < 4) return 2;
  scenario = strtoul(argv[2], NULL, 10);
  cycles = strtoul(argv[3], NULL, 10);
  for (index = 0; index < sizeof rom; ++index) rom[index] = (uint8_t)((index * 31U + 7U) & 0xFFU);
  region.begin = 0U; region.end = (uint32_t)sizeof rom; region.data = rom; region.length = (uint32_t)sizeof rom;
  runtime.owned_regions = &region;
  runtime.owned_region_count = 1U;
  file = fopen(argv[1], "r");
  if (file == NULL) return 2;
  for (index = 0; index < GENESIS_Z80_RAM_BYTES; ++index) {
    if (fread(pair, 1, 2, file) != 2) return 2;
    runtime.devices.z80_bus.z80_ram[index] = (uint8_t)strtoul(pair, NULL, 16);
  }
  fclose(file);
  runtime.devices.z80_bus.z80_ram[0x1F00] = (uint8_t)scenario;
  genesis_z80_machine_init(&machine, &runtime);
  machine.bound_ordinal = 1U;
  outcome = z80_run(&machine.cpu, cycles);
  printf("RESULT outcome=%s view_stop=%d cycles=%llu bank=%03x work10=%02x z80ram=", z80_outcome_name(outcome), (int)machine.view_stop,
         (unsigned long long)machine.cpu.state.cycles, (unsigned)runtime.devices.z80_bus.bank, (unsigned)runtime.work_ram[0x10]);
  for (index = 0x1000; index < 0x1010; ++index) printf("%02x", runtime.devices.z80_bus.z80_ram[index]);
  printf(" psg_tone0=%u\n", (unsigned)runtime.devices.psg.tone_period[0]);

  /* ---- the 68K side of the Z80 area (contract section 3) ---- */
  {
    GenesisDiagnosticCategory diag;
    uint32_t value;
    memset(&runtime.devices.z80_bus, 0, sizeof runtime.devices.z80_bus);
    value = 0x12U;
    {
      const int accepted = route(0xA00010U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &diag);
      printf("M68K without_bus_write=%d diag=%d\n", accepted, (int)diag);
    }
    value = 0x0100U;
    route(0xA11200U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &diag); /* release /RESET: the grant needs it */
    value = 0x0100U;
    route(0xA11100U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &diag); /* BUSREQ */
    value = 0x12U; route(0xA00010U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &diag);
    value = 0U; route(0xA02010U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &diag);          /* mirror */
    printf("M68K mirror_read=%02x", (unsigned)value);
    value = 0xABCDU; route(0xA00020U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &diag);   /* high byte only */
    value = 0U; route(0xA00020U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &diag);
    printf(" word_write_byte=%02x", (unsigned)value);
    value = 0U; route(0xA00020U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &value, &diag);
    printf(" word_read=%04x", (unsigned)value);
    value = 0U;
    {
      const int accepted = route(0xA00020U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &value, &diag);
      printf(" long_read=%d long_diag=%d", accepted, (int)diag);
    }
    /* bank register through the 68K: nine bytes, 1 0 0 0 0 0 0 0 1 -> LSB first: bank = 0x101 */
    {
      static const unsigned bits[9] = {1, 0, 0, 0, 0, 0, 0, 0, 1};
      unsigned i;
      for (i = 0; i < 9; ++i) { value = bits[i]; route(0xA06000U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &diag); }
      printf(" bank68=%03x", (unsigned)runtime.devices.z80_bus.bank);
      value = 0x0100U; route(0xA06000U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &diag); /* word write: D8 of the word */
      printf(" bank68w=%03x", (unsigned)runtime.devices.z80_bus.bank);
    }
    value = 0U;
    {
      const int accepted = route(0xA06000U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &diag);
      printf(" bankreg_read=%d bankreg_diag=%d", accepted, (int)diag);
    }
    printf("\n");
  }
  return 0;
}
