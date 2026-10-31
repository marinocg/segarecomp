/* SEG-111 T002 (ADR 0255): the build pass's bounded RAM write-stream observer, tested on a real ScdMachine through the bus entry points and
 * independently of any image admission.
 *
 * "One instruction" is a run of bus accesses between increments of the processor's retired-instruction count (what scd_retire does), so the
 * tests state exactly which accesses belong to the same instruction instance. Checks:
 *   pairing        a memory-to-memory move pairs (source and destination, per instruction instance) and a loop of them is one paired, contiguous stream
 *   not a pair     equal bytes with no read; a read in an earlier instruction (register-staged); two reads in one instruction; a different value; the same cell
 *   retry          the same read repeated inside one instruction is not a second read
 *   streams        a gap in the source, a longer stream than the bound, a stream overwritten later (bus, write-protect drop, exception frame), a device read while it
 *                  is produced, bounded eviction, main Work RAM mirrors normalised, sub PRG RAM, main write into the PRG window
 *   inert          the machine's memory, counters and instruction counts are identical with and without the observer; a machine without it has no table */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *machine;
static ScdProgram program;
static ScdAuthentic authentic;
static uint8_t bios[0x200];

static void check(int ok, const char *label) {
  printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

static void fresh(int observe) {
  if (machine != NULL) scd_machine_release(machine);
  memset(machine, 0, sizeof *machine);
  bios[1] = 0xFFU;
  bios[5] = 0x02U;
  if (scd_machine_init_bios(machine, &program, &authentic, NULL, bios, sizeof bios, 1000000U) != 0) {
    printf("FAIL  machine init\n");
    exit(1);
  }
  machine->materialize = 1U;
  if (observe && scd_reloc_attach(machine) != 0) exit(1);
}

static void next(uint32_t cpu) { machine->cpu[cpu].retired += 1U; } /* the instruction retired */

static char json[1U << 16];

static const char *report(void) {
  static const char name[] = "scd_reloc_observer_test.tmp"; /* a named scratch file: tmpfile() needs the drive root on some Windows C runtimes */
  FILE *f = fopen(name, "w+b");
  size_t n;
  if (f == NULL) exit(1);
  scd_reloc_report(machine, f);
  rewind(f);
  n = fread(json, 1U, sizeof json - 1U, f);
  json[n] = 0;
  fclose(f);
  remove(name);
  return json;
}

static int has(const char *text) { return strstr(report(), text) != NULL; }
static int streams(void) {
  const char *p = report();
  int n = 0;
  while ((p = strstr(p, "{\"writer_cpu\"")) != NULL) { ++n; ++p; }
  return n;
}

/* move.<w> (src),(dst): one instruction */
static void move(uint32_t cpu, uint32_t src, uint32_t dst, uint32_t width, uint32_t pc) {
  uint32_t v = 0U;
  if (scd_read(machine, cpu, src, width, &v, pc) != 0 || scd_write(machine, cpu, dst, width, v, pc) != 0) {
    printf("FAIL  access stopped\n");
    exit(1);
  }
  next(cpu);
}

static void put_byte(uint32_t cpu, uint32_t address, uint8_t v) {
  if (scd_write(machine, cpu, address, 1U, v, 0x50U) != 0) exit(1);
  next(cpu);
}

static void fill_source(uint32_t base, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; ++i) machine->prg_ram[base + i] = (uint8_t)(0x31U + 7U * i);
}

int main(void) {
  uint32_t i;
  machine = (ScdMachine *)calloc(1U, sizeof *machine);
  if (machine == NULL) return 1;

  /* ---- a copy loop: sub PRG RAM to PRG RAM, byte moves, one writer PC ---- */
  fresh(1);
  fill_source(0x1000U, 32U);
  for (i = 0; i < 21U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  check(streams() == 1 && has("\"writer_cpu\":1,\"memory\":\"prg\",\"destination\":196608,\"length\":21,\"pc\":512,\"paired\":1,\"source\":4096,\"source_class\":2,"),
        "a loop of memory-to-memory moves is ONE paired stream with its source range");
  check(has("\"source_gap\":0,\"clobbered\":0,\"dropped\":0,\"too_large\":0,\"volatile\":0") && has("\"evicted\":0"), "a clean stream carries no flag");
  check(has("\"bytes\":\"3138") && !has("\"too_large\":1"), "the stream's bytes are the written bytes");

  /* word and long moves pair too; the source class is the reading processor's */
  fresh(1);
  fill_source(0x2000U, 64U);
  for (i = 0; i < 4U; ++i) move(SCD_SUB, 0x2000U + 2U * i, 0x31000U + 2U * i, 2U, 0x210U);
  for (i = 0; i < 2U; ++i) move(SCD_SUB, 0x2008U + 4U * i, 0x31008U + 4U * i, 4U, 0x210U);
  check(streams() == 1 && has("\"length\":16,") && has("\"paired\":1,\"source\":8192"), "word and long moves pair and continue the same stream");

  /* ---- not a pair ---- */
  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 8U; ++i) put_byte(SCD_SUB, 0x30000U + i, machine->prg_ram[0x1000U + i]); /* equal bytes, no read at all */
  check(streams() == 1 && has("\"paired\":0"), "equal bytes with no read are NOT a pair");

  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 8U; ++i) {
    uint32_t v = 0U;
    (void)scd_read(machine, SCD_SUB, 0x1000U + i, 1U, &v, 0x200U);
    next(SCD_SUB); /* the move to the register retired */
    (void)scd_write(machine, SCD_SUB, 0x30000U + i, 1U, v, 0x202U);
    next(SCD_SUB);
  }
  check(streams() == 1 && has("\"paired\":0"), "a register-staged copy (read and write in different instructions) is NOT a pair");

  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 8U; ++i) {
    uint32_t v = 0U, w = 0U;
    (void)scd_read(machine, SCD_SUB, 0x1000U + i, 1U, &v, 0x200U);
    (void)scd_read(machine, SCD_SUB, 0x1100U + i, 1U, &w, 0x200U);
    (void)scd_write(machine, SCD_SUB, 0x30000U + i, 1U, v, 0x200U);
    next(SCD_SUB);
  }
  check(streams() == 1 && has("\"paired\":0"), "two data reads in one instruction are ambiguous: NOT a pair");

  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 8U; ++i) {
    uint32_t v = 0U;
    (void)scd_read(machine, SCD_SUB, 0x1000U + i, 1U, &v, 0x200U);
    (void)scd_write(machine, SCD_SUB, 0x30000U + i, 1U, v ^ 0x5AU, 0x200U); /* a transform */
    next(SCD_SUB);
  }
  check(streams() == 1 && has("\"paired\":0"), "a read whose value differs from the written value (a computation) is NOT a pair");

  fresh(1);
  for (i = 0; i < 4U; ++i) move(SCD_SUB, 0x3000U + i, 0x3000U + i, 1U, 0x200U);
  check(has("\"paired\":0"), "reading and writing the same cell is NOT a copy");

  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 8U; ++i) {
    uint32_t v = 0U;
    (void)scd_read(machine, SCD_SUB, 0x1000U + i, 1U, &v, 0x200U);
    (void)scd_read(machine, SCD_SUB, 0x1000U + i, 1U, &v, 0x200U); /* a retried instruction repeats the same read */
    (void)scd_write(machine, SCD_SUB, 0x30000U + i, 1U, v, 0x200U);
    next(SCD_SUB);
  }
  check(streams() == 1 && has("\"paired\":1"), "the same read repeated inside one instruction (a retry) is not a second read");

  /* ---- stream properties ---- */
  fresh(1);
  fill_source(0x1000U, 8U);
  fill_source(0x1100U, 8U);
  for (i = 0; i < 4U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  for (i = 0; i < 4U; ++i) move(SCD_SUB, 0x1100U + i, 0x30004U + i, 1U, 0x200U);
  check(streams() == 1 && has("\"source_gap\":1"), "a destination run fed from two source runs is flagged source_gap");

  fresh(1);
  for (i = 0; i < 4100U; ++i) put_byte(SCD_SUB, 0x30000U + i, (uint8_t)i);
  check(streams() == 1 && has("\"length\":4100,") && has("\"too_large\":1"), "a stream past the 4 KiB bound is flagged too_large and counted, never stored whole");

  fresh(1);
  fill_source(0x1000U, 16U);
  for (i = 0; i < 8U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  put_byte(SCD_SUB, 0x30003U, 0x77U);
  check(has("\"clobbered\":1"), "a later bus write inside a stream marks it clobbered");

  fresh(1);
  fill_source(0x1000U, 16U);
  for (i = 0; i < 8U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  scd_reloc_note_foreign(machine, SCD_MEMORY_PRG, 0x30002U, 2U);
  check(has("\"clobbered\":1"), "an exception-frame write inside a stream marks it clobbered");

  fresh(1);
  fill_source(0x1000U, 16U);
  for (i = 0; i < 8U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  for (i = 0; i < 8U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U); /* the same loop run again */
  check(streams() == 2 && has("\"clobbered\":1") && has("\"clobbered\":0"), "a repeated loop leaves the new stream clean and the old one clobbered");

  fresh(1);
  machine->ga.write_protect = 1U; /* PRG RAM below $200 is protected */
  put_byte(SCD_SUB, 0x100U, 0x55U);
  check(has("\"dropped\":1") && machine->prg_ram[0x100] == 0U, "a write the machine dropped (write protect) is flagged dropped");

  fresh(1);
  fill_source(0x1000U, 16U);
  {
    uint32_t v = 0U;
    move(SCD_SUB, 0x1000U, 0x30000U, 1U, 0x200U);
    (void)scd_read(machine, SCD_SUB, 0xFF8000U + 0x20U, 2U, &v, 0x204U); /* a Gate Array register: a device read */
    next(SCD_SUB);
    move(SCD_SUB, 0x1001U, 0x30001U, 1U, 0x200U);
  }
  check(has("\"volatile\":1"), "a device register read while a stream is produced marks it volatile");

  fresh(1);
  fill_source(0x1000U, 64U);
  for (i = 0; i < 9U; ++i) move(SCD_SUB, 0x1000U + i, 0x40000U + 0x100U * i, 1U, 0x300U + 2U * i); /* nine distinct writers: more than the table holds */
  check(streams() == 8 && has("\"evicted\":1"), "the table is bounded: the ninth stream evicts the oldest and the eviction is counted");

  /* ---- main processor: Work RAM, mirrors, the PRG window ---- */
  fresh(1);
  for (i = 0; i < 8U; ++i) machine->work_ram[0x2000U + i] = (uint8_t)(0x90U + i);
  for (i = 0; i < 8U; ++i) move(SCD_MAIN, 0xE02000U + i, 0xFF4000U + i, 1U, 0xFF0100U); /* read through one mirror, write through another */
  check(streams() == 1 && has("\"writer_cpu\":0,\"memory\":\"work\",\"destination\":16384,\"length\":8,\"pc\":16711936,\"paired\":1,\"source\":16719872,\"source_class\":1,"),
        "main Work RAM mirrors are one memory: the source is normalised to the $FFxxxx window");
  fresh(1);
  machine->ga.prg_window = 1U;
  machine->ga.prg_bank = 0U;
  for (i = 0; i < 4U; ++i) move(SCD_MAIN, 0xE02000U + i, 0x20100U + i, 1U, 0xFF0100U);
  check(streams() == 1 && has("\"writer_cpu\":0,\"memory\":\"prg\",\"destination\":256,"), "a main write into the PRG-RAM window is recorded with its writer processor (the driver never uses it for a sub frontier)");
  fresh(1);
  fill_source(0x1000U, 8U);
  for (i = 0; i < 4U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
  machine->ga.prg_window = 1U;
  put_byte(SCD_MAIN, 0x20000U, 0x11U); /* a main write to another PRG cell does not touch it */
  check(has("\"clobbered\":0"), "a write to another cell does not clobber a stream");

  /* ---- Word RAM sources and destinations ---- */
  fresh(1);
  machine->ga.mode = 0U; /* 2M, sub owns */
  for (i = 0; i < 8U; ++i) {
    uint32_t v = 0U;
    machine->word_ram[i] = (uint8_t)(0x44U + i);
    (void)scd_read(machine, SCD_SUB, 0x80000U + i, 1U, &v, 0x200U);
    (void)scd_write(machine, SCD_SUB, 0x30000U + i, 1U, v, 0x200U);
    next(SCD_SUB);
  }
  check(streams() == 1 && has("\"source_class\":3"), "a Word RAM read can be the source of a PRG RAM stream (classified as Word RAM)");
  fresh(1);
  machine->ga.mode = 0U;
  put_byte(SCD_SUB, 0x80010U, 0x12U);
  check(streams() == 0, "a Word RAM destination is not a supported stream");

  /* ---- inert: identical machine with and without the observer ---- */
  {
    uint8_t prg_a[SCD_PRG_RAM_BYTES];
    uint64_t retired_a, ignored_a, log_a;
    fresh(1);
    fill_source(0x1000U, 32U);
    for (i = 0; i < 21U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
    put_byte(SCD_SUB, 0x30003U, 0x99U);
    memcpy(prg_a, machine->prg_ram, sizeof prg_a);
    retired_a = machine->cpu[SCD_SUB].retired;
    ignored_a = machine->ignored_writes;
    log_a = machine->device_log;
    fresh(0);
    fill_source(0x1000U, 32U);
    for (i = 0; i < 21U; ++i) move(SCD_SUB, 0x1000U + i, 0x30000U + i, 1U, 0x200U);
    put_byte(SCD_SUB, 0x30003U, 0x99U);
    check(machine->reloc == NULL && memcmp(prg_a, machine->prg_ram, sizeof prg_a) == 0 && retired_a == machine->cpu[SCD_SUB].retired &&
              ignored_a == machine->ignored_writes && log_a == machine->device_log,
          "the observer changes nothing: memory, retirement, ignored-write count and the device log are identical without it");
  }
  scd_machine_release(machine);
  free(machine);
  printf("%d failures\n", failures);
  return failures != 0;
}
