/* SEG-065-T002 (ADR 0147): table-driven checks of the internal backup RAM machine input, driven through the runtime's own bus ABI
 * (scd_read / scd_write, exactly as generated code issues the accesses). Expectations are derived by hand from the documented
 * behaviour (contract section 4): an 8 KiB part on the odd byte lane of sub $E0000-$EFFFF (mod 1 MiB), mirrored every 16 KiB of
 * address; odd byte = the cell, even byte reads $FF and ignores writes, a word reads $FF00 | cell and its write stores the low
 * byte, a long is two words; without the machine input every access is the typed stop backup_ram_unsupported with the state
 * untouched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *m;

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static uint8_t image[SCD_BACKUP_RAM_BYTES];

static void fresh(int present) {
  unsigned i;
  memset(m, 0, sizeof *m);
  m->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  for (i = 0; i < SCD_BACKUP_RAM_BYTES; ++i) image[i] = (uint8_t)(i * 7U + 3U);
  if (present && scd_backup_ram_install(m, image, sizeof image) != 0) row(0, "install");
}

static int clear_stop(int status) {
  const int s = status == 0 ? 0 : (int)m->stop.stop_class;
  m->stop.stop_class = SCD_STOP_NONE;
  return s;
}
static int bus_write(uint32_t address, uint32_t width, uint32_t value) { return clear_stop(scd_write(m, SCD_SUB, address, width, value, 0x6000U)); }
static int bus_read(uint32_t address, uint32_t width, uint32_t *value) { return clear_stop(scd_read(m, SCD_SUB, address, width, value, 0x6000U)); }

int main(void) {
  uint32_t v;
  int s;
  char label[160];
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* absent: typed stop, state untouched, both directions and widths, every mirror */
  {
    static const uint32_t addrs[] = {0xFE0001U, 0xFE0000U, 0x0E0001U, 0xFEFFFFU, 0x1E2001U};
    static const uint32_t widths[] = {1U, 2U, 4U};
    unsigned a, w, d;
    for (a = 0; a < sizeof addrs / sizeof addrs[0]; ++a)
      for (w = 0; w < 3U; ++w)
        for (d = 0; d < 2U; ++d) {
          uint32_t addr = addrs[a] & (widths[w] == 1U ? 0xFFFFFFU : 0xFFFFFEU);
          fresh(0);
          v = 0xDEADBEEFU;
          s = d ? clear_stop(scd_write(m, SCD_SUB, addr, widths[w], 0x5AU, 0x6123U)) : clear_stop(scd_read(m, SCD_SUB, addr, widths[w], &v, 0x6123U));
          snprintf(label, sizeof label, "absent: %s.%u $%06X -> backup_ram_unsupported at cpu 1 pc $6123 (%s), value untouched",
                   d ? "write" : "read", widths[w], addr, scd_stop_class_name((ScdStopClass)s));
          row(s == SCD_STOP_BACKUP_RAM_UNSUPPORTED && m->stop.cpu == SCD_SUB && m->stop.pc == 0x6123U && m->stop.address == addr &&
                  m->stop.width == (widths[w] == 4U ? 2U : widths[w]) && m->stop.is_write == d && (d || v == 0xDEADBEEFU) &&
                  m->backup_ram_reads == 0U && m->backup_ram_writes == 0U,
              label);
        }
  }

  /* install: only exactly 8 KiB */
  fresh(0);
  row(scd_backup_ram_install(m, image, sizeof image - 1U) == -1 && !m->backup_ram_present, "install refuses 8191 bytes");
  row(scd_backup_ram_install(m, image, sizeof image + 1U) == -1 && !m->backup_ram_present, "install refuses 8193 bytes");
  row(scd_backup_ram_install(m, NULL, sizeof image) == -1 && !m->backup_ram_present, "install refuses a NULL image");
  row(scd_backup_ram_install(m, image, sizeof image) == 0 && m->backup_ram_present && memcmp(m->backup_ram, image, sizeof image) == 0,
      "install accepts exactly 8192 bytes and copies them");

  /* present: reads */
  fresh(1);
  row(bus_read(0xFE0001U, 1U, &v) == 0 && v == image[0], "odd byte $FE0001 reads cell 0");
  row(bus_read(0xFE0003U, 1U, &v) == 0 && v == image[1], "odd byte $FE0003 reads cell 1");
  row(bus_read(0xFE3FFFU, 1U, &v) == 0 && v == image[0x1FFF], "odd byte $FE3FFF reads the last cell");
  row(bus_read(0xFE4001U, 1U, &v) == 0 && v == image[0], "$FE4001 mirrors cell 0 (16 KiB repetition)");
  row(bus_read(0xFEC003U, 1U, &v) == 0 && v == image[1], "$FEC003 mirrors cell 1");
  row(bus_read(0x0E0001U, 1U, &v) == 0 && v == image[0], "$0E0001 (1 MiB mirror) reads cell 0");
  row(bus_read(0x5E2001U, 1U, &v) == 0 && v == image[0x1000], "$5E2001 (1 MiB mirror) reads cell $1000");
  row(bus_read(0xFE0000U, 1U, &v) == 0 && v == 0xFFU, "even byte $FE0000 reads $FF");
  row(bus_read(0xFE0002U, 2U, &v) == 0 && v == (0xFF00U | image[1]), "word $FE0002 reads $FF00 | cell 1");
  row(bus_read(0xFE0004U, 4U, &v) == 0 && v == ((0xFF00U | image[2]) << 16 | 0xFF00U | image[3]), "long $FE0004 reads two words (cells 2, 3)");
  row(m->backup_ram_reads == 11U && m->backup_ram_writes == 0U, "11 bus reads counted (a long is two)");

  /* present: writes */
  fresh(1);
  row(bus_write(0xFE0011U, 1U, 0xA5U) == 0 && m->backup_ram[8] == 0xA5U, "odd byte write $FE0011 stores cell 8");
  row(bus_write(0xFE0012U, 1U, 0x3CU) == 0 && m->backup_ram[9] == image[9] && m->ignored_writes == 1U,
      "even byte write $FE0012 is ignored (cell 9 unchanged, one ignored write)");
  row(bus_write(0xFE0014U, 2U, 0x12C3U) == 0 && m->backup_ram[10] == 0xC3U, "word write $FE0014 stores its low byte in cell 10");
  row(bus_write(0xFE0018U, 4U, 0x11223344U) == 0 && m->backup_ram[12] == 0x22U && m->backup_ram[13] == 0x44U,
      "long write $FE0018 stores the two low bytes in cells 12 and 13");
  row(bus_write(0x1E4021U, 1U, 0x77U) == 0 && m->backup_ram[16] == 0x77U, "write through a mirror ($1E4021) stores cell 16");
  row(bus_read(0xFE0021U, 1U, &v) == 0 && v == 0x77U, "and reads back at $FE0021");
  row(m->backup_ram_writes == 6U, "6 bus writes counted (the ignored even byte included, a long is two)");
  {
    unsigned i;
    int others = 1;
    for (i = 0; i < SCD_BACKUP_RAM_BYTES; ++i)
      if (i != 8U && i != 10U && i != 12U && i != 13U && i != 16U && m->backup_ram[i] != image[i]) others = 0;
    row(others, "no other cell changed");
  }
  /* other sub areas are unaffected by the input: PCM stays its own device, PRG RAM its own memory */
  fresh(1);
  row(bus_write(0x000101U, 1U, 0x99U) == 0 && m->prg_ram[0x101] == 0x99U && m->backup_ram_writes == 0U, "PRG RAM is not backup RAM");

  printf("%d failures\n", failures);
  free(m);
  return failures == 0 ? 0 : 1;
}
