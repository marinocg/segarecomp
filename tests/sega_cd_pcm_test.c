/* SEG-102 (ADR 0233): the RF5C164 PCM chip's running wave-address pointers and their CPU-visible readback.
 *
 * Project-authored fixtures drive scd_read/scd_write on a real ScdMachine through the sub processor's bus ($FF0000-$FF7FFF, /LDS only) with
 * the machine instant set explicitly, so every sample boundary is exercised exactly. Three layers of expectation:
 *   1. closed-form hand-derived rows (address after n sample events is st*2^19 + n*fd, modulo 2^27, with the loop rule), independent of the
 *      per-sample stepping the runtime uses;
 *   2. a table of operations replayed against the independent behavioural reference (tests/sega_cd_pcm_vectors.inc), under both clock profiles;
 *   3. determinism: the same operations with extra synchronizations at arbitrary instants (what a different deadline partition causes)
 *      produce identical pointers.
 * Plus the fail-closed classes: nothing but the pointer registers is readable. Sample period: 384 sub cycles (the stopwatch's unit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

typedef struct PcmVec {
  uint64_t cycle;
  uint8_t kind;
  uint16_t offset;
  uint8_t value;
} PcmVec;

#include "sega_cd_pcm_vectors.inc"

static int failures;

static void check(int ok, const char *label) {
  if (!ok) {
    printf("FAIL  %s\n", label);
    ++failures;
  }
}

static ScdMachine *machine;
static ScdProgram program;
static ScdAuthentic authentic;
static uint8_t bios[0x200];
static int line_locked = 1;

#define PERIOD 384U /* sub cycles per sample */

static uint64_t unit(void) { return line_locked ? SCD_SUB_CYCLE_TICKS_LINE_LOCKED : SCD_SUB_CYCLE_TICKS; }

static void fresh(void) {
  if (machine != NULL) scd_machine_release(machine);
  memset(machine, 0, sizeof *machine);
  bios[1] = 0xFFU;
  bios[5] = 0x02U;
  if (scd_machine_init_bios(machine, &program, &authentic, NULL, bios, sizeof bios, 1000000U) != 0) {
    printf("FAIL  machine init\n");
    exit(1);
  }
  if (!line_locked) machine->bios = NULL; /* the synthetic profile's exact 12.5 MHz sub cycle */
}

static void at(uint64_t sub_cycle) { machine->now = sub_cycle * unit(); }

/* Chip register offset -> the sub's byte-wide /LDS address ($FF0000 + 2 x offset + 1). */
static uint32_t lds(uint32_t chip_offset) { return 0xFF0001U + 2U * chip_offset; }

static void wr(uint64_t cycle, uint32_t chip_offset, uint32_t value) {
  at(cycle);
  if (scd_write(machine, SCD_SUB, lds(chip_offset), 1U, value, 0x100U) != 0) {
    printf("FAIL  write stopped (offset %X)\n", chip_offset);
    ++failures;
  }
}

static int rd(uint64_t cycle, uint32_t chip_offset, uint32_t *value) {
  at(cycle);
  return scd_read(machine, SCD_SUB, lds(chip_offset), 1U, value, 0x100U);
}

static uint32_t ptr_byte(uint64_t cycle, uint32_t channel, int high) {
  uint32_t v = 0xDEADU;
  if (rd(cycle, 0x10U + 2U * channel + (high ? 1U : 0U), &v) != 0) {
    printf("FAIL  pointer read stopped\n");
    ++failures;
  }
  return v;
}

/* The 16-bit wave byte address of a channel at `cycle`. */
static uint32_t ptr16(uint64_t cycle, uint32_t channel) {
  const uint32_t lo = ptr_byte(cycle, channel, 0), hi = ptr_byte(cycle, channel, 1);
  return (hi << 8U) | lo;
}

static void select_channel(uint64_t cycle, uint32_t channel) { wr(cycle, 0x07U, 0x40U | channel); } /* bit 7 clear: the chip enable is cleared too */
static void config(uint64_t cycle, uint32_t channel, uint32_t fd, uint32_t ls, uint32_t st) {
  select_channel(cycle, channel);
  wr(cycle, 0x02U, fd & 0xFFU);
  wr(cycle, 0x03U, fd >> 8U);
  wr(cycle, 0x04U, ls & 0xFFU);
  wr(cycle, 0x05U, ls >> 8U);
  wr(cycle, 0x06U, st);
}
static void bank_write(uint64_t cycle, uint32_t bank, uint32_t offset, uint32_t value, int keep_enabled) {
  wr(cycle, 0x07U, (keep_enabled ? 0x80U : 0x00U) | bank); /* bank select (bit 6 clear); bit 7 is the chip enable */
  wr(cycle, 0x1000U + offset, value);
}

/* ---- layer 1: closed form -------------------------------------------------------------------------------------------------- */
static void closed_form(void) {
  uint32_t c;
  /* Reset defaults: eight channels at address 0, chip disabled, every channel off. */
  fresh();
  for (c = 0; c < 8U; ++c) check(ptr16(1000U, c) == 0U, "reset: every channel pointer is 0");
  check(machine->pcm.running == 0U && machine->pcm.enabled == 0U, "reset: chip disabled, all channels off");
  /* A disabled chip does not move an ON channel; the sample grid (origin 0: events at k x 384) runs regardless. */
  fresh();
  config(10U, 0U, 0x0800U, 0U, 0x10U);
  wr(11U, 0x08U, 0xFEU); /* channel 0 ON (a clear bit), the others off; the chip is still disabled */
  check(ptr16(5000U, 0U) == 0x1000U, "chip disabled: an ON channel stays at its start address");
  wr(5001U, 0x07U, 0x80U); /* enable the chip: events 0..13 (13 x 384 = 4992) are behind; event 14 is at 5376 */
  check(ptr16(5376U, 0U) == 0x1000U, "a read exactly on a sample event sees the state before it");
  check(ptr16(5377U, 0U) == 0x1001U, "one cycle after the event: one step (fd = 1.0 byte)");
  check(ptr16(5376U + PERIOD, 0U) == 0x1001U, "exactly on the following event: still the state before it");
  check(ptr16(5376U + 3U * PERIOD + 1U, 0U) == 0x1004U, "three further periods: four steps in total");
  /* Fractional step: fd = $0400 is half a byte per sample. The ON write at cycle 1 follows event 0; events 1..n lie before 1 + n x 384. */
  fresh();
  config(0U, 0U, 0x0400U, 0U, 0x00U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  check(ptr16(1U + 10U * PERIOD, 0U) == 5U, "half-byte step: ten events give five bytes");
  check(ptr16(1U + 11U * PERIOD, 0U) == 5U, "half-byte step: eleven events 5.5 bytes -> 5");
  check(ptr16(1U + 12U * PERIOD, 0U) == 6U, "half-byte step: twelve events 6 bytes");
  /* Zero step. */
  fresh();
  config(0U, 2U, 0U, 0U, 0x20U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFBU);
  check(ptr16(1000000U, 2U) == 0x2000U, "frequency step 0: the pointer never moves");
  /* Large step with wraparound: st = $FF (byte address $FF00), 16 bytes per sample: sixteen events reach $10000 -> 0. */
  fresh();
  config(0U, 0U, 0x8000U, 0U, 0xFFU);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  check(ptr16(1U + 15U * PERIOD, 0U) == 0xFF00U + 15U * 16U, "large step: fifteen events");
  check(ptr16(1U + 16U * PERIOD, 0U) == 0U, "large step: sixteen events wrap the 64 KiB wave address to 0");
  check(ptr16(1U + 17U * PERIOD, 0U) == 16U, "large step: and continue");
  /* st = 0, fd = $0F00 (1.875 bytes): nine events give 16.875 bytes, the pointer reads the integer part. */
  fresh();
  config(0U, 5U, 0x0F00U, 0U, 0x00U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xDFU);
  check(ptr16(1U + 9U * PERIOD, 5U) == 16U, "the fraction is truncated in the pointer read");
  /* Several channels, independent steps. */
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x00U);
  config(0U, 1U, 0x1000U, 0U, 0x01U);
  config(0U, 7U, 0x0800U, 0U, 0x07U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0x7CU); /* channels 0, 1 and 7 on */
  check(ptr16(1U + 4U * PERIOD, 0U) == 4U, "channel 0 advances one byte per event");
  check(ptr16(1U + 4U * PERIOD, 1U) == 0x0100U + 8U, "channel 1 advances two bytes per event from its own start");
  check(ptr16(1U + 4U * PERIOD, 7U) == 0x0700U + 4U, "channel 7 advances independently");
  check(ptr16(1U + 4U * PERIOD, 3U) == 0U, "an off channel stays at its address");
}

static void loop_rules(void) {
  /* The wave RAM byte at address 5 is the loop marker; the loop address is 2: the pointer visits 0..5, then 2..5 forever (the reload event
   * does not step). Origin 0: the ON write at cycle 2 follows event 0; after n events the pointer is n (n <= 5) or 2 + (n - 6) % 4. */
  uint32_t n;
  fresh();
  bank_write(0U, 0U, 5U, 0xFFU, 0);
  config(1U, 0U, 0x0800U, 2U, 0x00U);
  wr(2U, 0x07U, 0x80U);
  wr(2U, 0x08U, 0xFEU);
  for (n = 0U; n <= 20U; ++n) {
    const uint32_t expect = n <= 5U ? n : 2U + (n - 6U) % 4U, got = ptr16(2U + n * PERIOD + 1U, 0U);
    if (got != expect) {
      printf("FAIL  loop: %u events, pointer %X != %X\n", n, got, expect);
      ++failures;
      break;
    }
  }
  /* An immediate marker at the loop start: the channel reloads the marker forever. */
  fresh();
  bank_write(0U, 0U, 7U, 0xFFU, 0);
  config(1U, 0U, 0x0800U, 7U, 0x00U);
  wr(2U, 0x07U, 0x80U);
  wr(2U, 0x08U, 0xFEU);
  check(ptr16(2U + 6U * PERIOD + 1U, 0U) == 6U, "immediate marker: steps up to it");
  check(ptr16(2U + 8U * PERIOD + 1U, 0U) == 7U, "immediate marker: reaches the loop address");
  check(ptr16(2U + 500U * PERIOD + 1U, 0U) == 7U, "immediate marker: stays on the loop address");
  /* Wave RAM written while the channel runs changes what the pointer sees next (the chip reads all 64 KiB, not the bank window). */
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x00U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  check(ptr16(1U + 3U * PERIOD, 0U) == 3U, "before the write: three events");
  bank_write(1U + 3U * PERIOD + 2U, 0U, 5U, 0xFFU, 1); /* the marker appears ahead of the pointer; the chip stays enabled */
  check(ptr16(1U + 5U * PERIOD + 1U, 0U) == 5U, "after the write: the pointer reaches the marker");
  check(ptr16(1U + 6U * PERIOD + 1U, 0U) == 0U, "the marker reloads the loop address (0)");
  check(ptr16(1U + 7U * PERIOD + 1U, 0U) == 1U, "and the channel continues from it");
  /* A marker in another 4 KiB bank is seen at its absolute wave address. */
  fresh();
  bank_write(0U, 1U, 5U, 0xFFU, 0); /* bank 1, offset 5: wave address $1005 */
  config(1U, 0U, 0x0800U, 0x1002U, 0x10U);
  wr(2U, 0x07U, 0x80U);
  wr(2U, 0x08U, 0xFEU);
  check(ptr16(2U + 5U * PERIOD + 1U, 0U) == 0x1005U, "bank 1 marker: the pointer reaches wave address $1005");
  check(ptr16(2U + 6U * PERIOD + 1U, 0U) == 0x1002U, "bank 1 marker: reload to the loop address $1002");
}

static void register_rules(void) {
  /* ON/OFF reload, START while ON and OFF. Origin 0; the ON write at cycle 1 follows event 0. */
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x10U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  check(ptr16(1U + 3U * PERIOD, 0U) == 0x1003U, "running from the start address");
  wr(1U + 3U * PERIOD + 10U, 0x06U, 0x40U); /* START written while ON: the running address is untouched (channel 0 is still selected) */
  check(ptr16(1U + 3U * PERIOD + 11U, 0U) == 0x1003U, "START while ON leaves the running address");
  wr(1U + 4U * PERIOD, 0x08U, 0xFFU); /* all off: reload from the (new) START */
  check(ptr16(1U + 4U * PERIOD + 1U, 0U) == 0x4000U, "OFF reloads the running address from START");
  check(ptr16(1U + 40U * PERIOD, 0U) == 0x4000U, "an OFF channel does not advance");
  wr(1U + 40U * PERIOD, 0x06U, 0x22U); /* START written while OFF reloads at once */
  check(ptr16(1U + 40U * PERIOD + 1U, 0U) == 0x2200U, "START while OFF reloads the running address");
  wr(1U + 41U * PERIOD + 5U, 0x08U, 0xFEU); /* ON again: resumes from START */
  check(ptr16(1U + 41U * PERIOD + 6U, 0U) == 0x2200U, "ON resumes from the start address");
  check(ptr16(1U + 43U * PERIOD + 6U, 0U) == 0x2202U, "and advances from there (events 42 and 43)");
  /* Registers belong to the selected channel. */
  fresh();
  config(0U, 3U, 0x0800U, 0U, 0x30U);
  select_channel(0U, 4U);
  wr(1U, 0x02U, 0x00U);
  wr(1U, 0x03U, 0x10U);
  wr(1U, 0x07U, 0x80U); /* chip on (bit 6 clear: a bank select; the channel selection is unchanged) */
  wr(1U, 0x08U, 0xE7U); /* channels 3 and 4 on */
  check(ptr16(1U + 2U * PERIOD, 3U) == 0x3002U, "channel 3 keeps its own step and start");
  check(ptr16(1U + 2U * PERIOD, 4U) == 4U, "channel 4 has the step written while it was selected (2.0 bytes)");
}

static void peripheral_reset(void) {
  const uint64_t t0 = 5U * PERIOD + 77U; /* a reset instant that is not on the old sample grid */
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x10U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  check(ptr16(5U * PERIOD, 0U) == 0x1004U, "running before the reset");
  at(t0);
  scd_pcm_reset(machine); /* scd_peripheral_reset() ends with this call (contract section 17) */
  check(ptr16(t0 + 1U, 0U) == 0U && machine->pcm.running == 0U && machine->pcm.enabled == 0U, "the reset clears the chip");
  config(t0 + 1U, 0U, 0x0800U, 0U, 0x00U);
  wr(t0 + 2U, 0x07U, 0x80U);
  wr(t0 + 2U, 0x08U, 0xFEU); /* the new grid has events at t0 + k x 384; event 0 (t0) precedes this write */
  check(ptr16(t0 + 2U + 3U * PERIOD, 0U) == 3U, "after the reset the sample grid restarts at the reset instant");
  check(ptr16(t0 + 4U * PERIOD, 0U) == 3U && ptr16(t0 + 4U * PERIOD + 1U, 0U) == 4U, "the grid phase is the reset instant");
  check(machine->pcm.pan[0] == 0xFFU && machine->pcm.pan[7] == 0xFFU, "the reset restores the panning defaults");
}

static void fail_closed(void) {
  uint32_t v = 0U;
  const uint32_t classes[] = {0x00U, 0x07U, 0x08U, 0x0FU, 0x20U, 0x7FU, 0x1000U, 0x1FFFU};
  unsigned i;
  for (i = 0; i < sizeof classes / sizeof classes[0]; ++i) {
    fresh();
    check(rd(10U, classes[i], &v) != 0 && machine->stop.stop_class == SCD_STOP_PCM_UNSUPPORTED, "non-pointer chip reads are the typed stop");
  }
  fresh();
  at(10U);
  check(scd_read(machine, SCD_SUB, 0xFF0022U, 1U, &v, 0x100U) != 0 && machine->stop.stop_class == SCD_STOP_PCM_UNSUPPORTED,
        "a byte read on the unused (even) lane is the typed stop");
  fresh();
  at(10U);
  check(scd_read(machine, SCD_SUB, 0xFF0022U, 2U, &v, 0x100U) == 0 && v == 0U, "a word read returns the chip byte in the low lane");
  /* A refused read leaves the chip state untouched and logged nothing for it. */
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x00U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  {
    const uint64_t before = machine->device_log;
    check(rd(1U + 9U * PERIOD, 0x1000U, &v) != 0, "wave RAM read refused");
    check(machine->device_log == before, "a refused read does not enter the device log");
  }
}

static void bus_forms(void) {
  uint32_t v = 0U, w = 0U, wl = 0xFFFFU, wh = 0xFFFFU, ml = 0U, mh = 0U;
  fresh();
  config(0U, 0U, 0x0800U, 0U, 0x00U);
  wr(1U, 0x07U, 0x80U);
  wr(1U, 0x08U, 0xFEU);
  at(1U + 300U * PERIOD); /* 300 events = 300 bytes = $012C */
  check(scd_read(machine, SCD_SUB, 0xFF0021U, 1U, &v, 0x100U) == 0 && scd_read(machine, SCD_SUB, 0xFF0023U, 1U, &w, 0x100U) == 0, "byte reads");
  check(v == 0x2CU && w == 0x01U, "byte reads: even chip offset = low byte, odd chip offset = high byte");
  check(scd_read(machine, SCD_SUB, 0xFF0020U, 2U, &wl, 0x100U) == 0 && scd_read(machine, SCD_SUB, 0xFF0022U, 2U, &wh, 0x100U) == 0, "word reads");
  check(wl == 0x2CU && wh == 0x01U, "word reads: the chip byte in the low lane, zero above it (the reference's convention)");
  check(scd_read(machine, SCD_SUB, 0xEF0021U, 1U, &ml, 0x100U) == 0 && scd_read(machine, SCD_SUB, 0xFF4021U, 1U, &mh, 0x100U) == 0, "mirrors read");
  check(ml == 0x2CU && mh == 0x2CU, "mirrors: the 1 MiB repeat and the 16 KiB chip repeat reach the same register");
  /* A word write reaches the chip's low byte only; a byte write on the even lane is dropped (as before). */
  fresh();
  at(100U);
  check(scd_write(machine, SCD_SUB, 0xFF0004U, 2U, 0xAB12U, 0x100U) == 0, "word write");
  check(machine->pcm.fd[0] == 0x0012U, "a word write reaches chip offset 2 (FD low) with its low byte only");
  check(scd_write(machine, SCD_SUB, 0xFF0004U, 1U, 0xEEU, 0x100U) == 0 && machine->pcm.fd[0] == 0x0012U, "a byte write on the even lane is dropped");
}

/* ---- layer 2/3: vectors and partition independence -------------------------------------------------------------------------- */
static unsigned run_vectors(unsigned set, uint64_t extra_sync_every, int *mismatch_at) {
  const PcmVec *rows = pcm_vec_sets[set].rows;
  unsigned n = pcm_vec_sets[set].count, i, reads = 0U;
  fresh();
  *mismatch_at = -1;
  for (i = 0U; i < n; ++i) {
    uint32_t got = 0U;
    if (extra_sync_every != 0U) { /* what another deadline partition does: synchronize at instants in between */
      uint64_t t = i == 0U ? 0U : rows[i - 1U].cycle;
      for (; t + extra_sync_every < rows[i].cycle; t += extra_sync_every) scd_pcm_sync(machine, (t + extra_sync_every) * unit());
    }
    if (rows[i].kind == 0U) {
      wr(rows[i].cycle, rows[i].offset, rows[i].value);
    } else {
      ++reads;
      if (rd(rows[i].cycle, rows[i].offset, &got) != 0 || got != rows[i].value) {
        printf("  row %u: cycle %llu offset %X expected %X got %X\n", i, (unsigned long long)rows[i].cycle, rows[i].offset, rows[i].value, got);
        printf("  state: running %02X enabled %u samples %llu addr3 %X st3 %02X fd3 %04X ls3 %04X\n", machine->pcm.running, machine->pcm.enabled, (unsigned long long)machine->pcm.samples, machine->pcm.addr[3], machine->pcm.st[3], machine->pcm.fd[3], machine->pcm.ls[3]);
        *mismatch_at = (int)i;
        return reads;
      }
    }
  }
  return reads;
}

static void vectors(void) {
  unsigned s, total = 0U;
  int profile;
  for (profile = 0; profile < 2; ++profile) {
    line_locked = profile;
    for (s = 0U; s < sizeof pcm_vec_sets / sizeof pcm_vec_sets[0]; ++s) {
      int bad;
      total += run_vectors(s, 0U, &bad);
      if (bad >= 0) {
        printf("FAIL  vector set %u (profile %d) differs from the reference at row %d\n", s, profile, bad);
        ++failures;
      }
      run_vectors(s, 97U, &bad);
      if (bad >= 0) {
        printf("FAIL  vector set %u (profile %d) with extra synchronizations differs at row %d\n", s, profile, bad);
        ++failures;
      }
      run_vectors(s, 1000U, &bad);
      check(bad < 0, "vector set with sparse extra synchronizations");
    }
  }
  line_locked = 1;
  printf("vectors: %u pointer reads compared in %u sets under two profiles\n", total / 2U, (unsigned)(sizeof pcm_vec_sets / sizeof pcm_vec_sets[0]));
  /* The vectors are not a stub's: several distinct values per set. */
  {
    uint32_t seen[256];
    unsigned distinct = 0U, i, j;
    for (s = 0U; s < sizeof pcm_vec_sets / sizeof pcm_vec_sets[0]; ++s)
      for (i = 0U; i < pcm_vec_sets[s].count; ++i)
        if (pcm_vec_sets[s].rows[i].kind == 1U) {
          for (j = 0U; j < distinct && seen[j] != pcm_vec_sets[s].rows[i].value; ++j) {}
          if (j == distinct && distinct < 256U) seen[distinct++] = pcm_vec_sets[s].rows[i].value;
        }
    check(distinct >= 60U, "the vector reads cover many distinct values (a constant stub cannot pass)");
  }
}

int main(void) {
  machine = (ScdMachine *)calloc(1U, sizeof *machine);
  if (machine == NULL) return 1;
  closed_form();
  loop_rules();
  register_rules();
  peripheral_reset();
  fail_closed();
  bus_forms();
  vectors();
  scd_machine_release(machine);
  free(machine);
  printf(failures == 0 ? "sega_cd_pcm_test: all checks passed\n" : "sega_cd_pcm_test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
