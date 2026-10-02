/*
 * SEG-032-T002 (ADR 0073): replays a raw 68K access script through the REAL Genesis runtime router
 * (genesis_route_access) with the generic Z80 image-epoch observer installed, and prints every epoch.
 *   genesis_z80_epoch_script_harness <script> [fill]
 * script lines: `w16 <hex-address> <hex-value>` or `w8 <hex-address> <hex-value>`; `#` starts a comment.
 * output lines: `EPOCH <ordinal> <ram hex> <written-bitmap hex>`; then `END <epoch count>`.
 * Test tool only: the hex dumps are of project-authored synthetic data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

static void on_epoch(void *context, const GenesisZ80EpochEvent *event) {
  unsigned i;
  (void)context;
  printf("EPOCH %u ", (unsigned)event->ordinal);
  for (i = 0; i < GENESIS_Z80_RAM_BYTES; ++i) printf("%02X", event->ram[i]);
  printf(" ");
  for (i = 0; i < GENESIS_Z80_RAM_BYTES / 8U; ++i) printf("%02X", event->written_bitmap[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  static GenesisRuntime runtime;
  char line[256];
  FILE *script;
  unsigned long fill = 0;
  if (argc < 2) return 2;
  if (argc > 2) fill = strtoul(argv[2], NULL, 16);
  script = fopen(argv[1], "r");
  if (script == NULL) return 2;
  memset(runtime.devices.z80_bus.z80_ram, (int)fill, GENESIS_Z80_RAM_BYTES);
  runtime.z80_epoch.on_epoch = on_epoch;
  while (fgets(line, sizeof line, script) != NULL) {
    char kind[8];
    unsigned long address, value;
    uint32_t routed;
    GenesisRuntimeStop stop;
    GenesisAccessWidth width;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%7s %lx %lx", kind, &address, &value) != 3) return 3;
    width = strcmp(kind, "w16") == 0 ? GENESIS_ACCESS_WORD : GENESIS_ACCESS_BYTE;
    routed = (uint32_t)value;
    memset(&stop, 0, sizeof stop);
    if (genesis_route_access(&runtime, (uint32_t)address, width, GENESIS_ACCESS_WRITE, &routed, &stop) !=
        GENESIS_ACCESS_OK) {
      fprintf(stderr, "access rejected: %s %lx\n", kind, address);
      return 4;
    }
  }
  fclose(script);
  printf("END %u\n", (unsigned)runtime.z80_epoch.epoch_count);
  return 0;
}
