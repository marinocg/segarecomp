/* SEG-063-T001 Checkpoint C: unit harness for the Sega CD overlay runtime (platforms/sega-cd/runtime/scd_overlay.c).
 *
 * Builds a machine in its power-on Gate Array state without a disc, attaches a hand-made registry whose candidate digests are
 * computed here with the runtime's own SHA-256 (the same rule the C++ emitter uses), and drives scd_overlay_dispatch through
 * ordinary bus writes (scd_write) of both processors. Candidate dispatchers are stubs that record which image ran. */
#include <stdio.h>
#include <string.h>

#include "runtime.h"
#include "scd_runtime.h"

static int failures;
static int ran_image;

static void check(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

/* Stub candidate dispatchers: like the generated ones, they answer only for their own compiled entries. */
static int run_a(ScdMachine *m) {
  const uint32_t pc = m->cpu[SCD_SUB].pc;
  if (pc != 0x20000U && pc != 0x20002U && pc != 0x80000U) return SCD_IMAGE_NO_ENTRY;
  ran_image = 2;
  return 0;
}
static int run_b(ScdMachine *m) {
  if (m->cpu[SCD_SUB].pc != 0x20000U) return SCD_IMAGE_NO_ENTRY;
  ran_image = 3;
  return 0;
}
static int run_m(ScdMachine *m) {
  if (m->cpu[SCD_MAIN].pc != 0x20000U) return SCD_IMAGE_NO_ENTRY;
  ran_image = 4;
  return 0;
}
static const uint32_t program_entries[] = {0x6000U};
static ScdProgram program;

#define LEN 128U
static uint8_t image_a[LEN], image_b[LEN];
static uint64_t dig_a[2], dig_b[2], dig_ma[2], dig_wa[2];
static const uint32_t entries_sub[] = {0x20000U, 0x20002U};
static const uint32_t entries_main[] = {0x20000U};
static const uint32_t entries_word[] = {0x80000U};
static const uint8_t *const no_bytes[] = {NULL, NULL};
static const uint8_t no_lengths[] = {0U, 0U};
static const uint32_t roots_sub[] = {0x20000U};
static const uint32_t roots_main[] = {0x20000U};
static const uint32_t roots_word[] = {0x80000U};

static void digests(const uint8_t *bytes, uint64_t out[2]) {
  unsigned g, i;
  for (g = 0; g < 2U; ++g) {
    GenesisSha256 sha;
    uint8_t d[32];
    genesis_sha256_init(&sha);
    genesis_sha256_update(&sha, bytes + 64U * g, 64U);
    genesis_sha256_final(&sha, d);
    out[g] = 0U;
    for (i = 0; i < 8U; ++i) out[g] = (out[g] << 8) | d[i];
  }
}

static ScdOverlayCandidate sub_candidates[2], main_candidates[1], word_candidates[1];
static ScdOverlayGroup groups[3];
static ScdOverlayProgram overlays = {groups, 3U, "harness"};
static ScdMachine machine;

static void candidate(ScdOverlayCandidate *c, uint32_t id, const char *name, uint32_t start, const uint64_t *d, const uint32_t *entries,
                      uint32_t entry_count, const uint32_t *roots, ScdDispatchFunction dispatch) {
  memset(c, 0, sizeof *c);
  c->image_id = id;
  c->name = name;
  c->start = start;
  c->length = LEN;
  c->granule_digests = d;
  c->granule_first = start >> 6;
  c->granule_count = 2U;
  c->entries = entries;
  c->entry_count = entry_count;
  c->entry_bytes = no_bytes;
  c->entry_lengths = no_lengths;
  c->roots = roots;
  c->root_count = 1U;
  c->dispatch = dispatch;
}

static void group(ScdOverlayGroup *g, uint32_t cpu, uint32_t start, const ScdOverlayCandidate *c, uint32_t count) {
  g->cpu = cpu;
  g->start = start;
  g->end = start + LEN;
  g->candidates = c;
  g->candidate_count = count;
  g->granule_first = start >> 6;
  g->granule_count = 2U;
}

static void power_on(ScdMachine *m) {
  memset(m, 0, sizeof *m);
  m->ga.ctl = 0x02U;
  m->ga.sub_halted = 1U;
  m->ga.prg_window = 1U;
  m->ga.mode = SCD_MODE_RET;
  scd_devices_power_on(m);
  program.processors[SCD_SUB].entries = program_entries;
  program.processors[SCD_SUB].entry_count = 1U;
  m->program = &program;
  scd_overlay_attach(m, &overlays);
}

static int dispatch(ScdMachine *m, uint32_t cpu, uint32_t pc) {
  ran_image = 0;
  m->cpu[cpu].pc = pc;
  return scd_overlay_dispatch(m, cpu);
}

static void write_bytes(ScdMachine *m, uint32_t cpu, uint32_t address, const uint8_t *bytes, uint32_t length) {
  uint32_t i;
  for (i = 0; i < length; i += 2U)
    if (scd_write(m, cpu, address + i, 2U, (uint32_t)((bytes[i] << 8) | bytes[i + 1U]), 0U) != 0) check(0, "harness write");
}

int main(void) {
  ScdMachine *m = &machine;
  unsigned i;
  for (i = 0; i < LEN; ++i) {
    image_a[i] = (uint8_t)(0xA0U + i);
    image_b[i] = (uint8_t)(i < 64U ? 0xA0U + i : 0xB0U + i); /* B shares A's first granule and differs in the second */
  }
  digests(image_a, dig_a);
  digests(image_b, dig_b);
  digests(image_a, dig_ma);
  digests(image_a, dig_wa);
  candidate(&sub_candidates[0], 2U, "a", 0x20000U, dig_a, entries_sub, 2U, roots_sub, run_a);
  candidate(&sub_candidates[1], 3U, "b", 0x20000U, dig_b, entries_sub, 1U, roots_sub, run_b);
  candidate(&main_candidates[0], 2U, "main_a", 0x20000U, dig_ma, entries_main, 1U, roots_main, run_m);
  candidate(&word_candidates[0], 4U, "word_a", 0x80000U, dig_wa, entries_word, 1U, roots_word, run_a);
  group(&groups[0], SCD_SUB, 0x20000U, sub_candidates, 2U);
  group(&groups[1], SCD_MAIN, 0x20000U, main_candidates, 1U);
  group(&groups[2], SCD_SUB, 0x80000U, word_candidates, 1U);

  /* --- sub PRG RAM group: selection by whole-image comparison, then the fast path --- */
  power_on(m);
  memcpy(&m->prg_ram[0x20000], image_a, LEN);
  check(dispatch(m, SCD_SUB, 0x20000U) == 0 && ran_image == 2 && m->overlay[0].selected == 0, "A's bytes select A");
  check(dispatch(m, SCD_SUB, 0x20002U) == 0 && ran_image == 2 && m->overlay[0].revalidations == 0U && m->overlay[0].selections == 1U,
        "an undisturbed selection runs without revalidation");
  write_bytes(m, SCD_SUB, 0x020200U, image_b, 2U);
  check(!m->overlay[0].any_dirty, "a store outside the group marks nothing");
  write_bytes(m, SCD_SUB, 0x020040U, image_a + 0x40U, 2U); /* the same value into A's second granule */
  check(m->overlay[0].any_dirty && m->overlay[0].dirty_lo == 1U && m->overlay[0].dirty_hi == 1U, "a store marks exactly its granule");
  check(dispatch(m, SCD_SUB, 0x20002U) == 0 && ran_image == 2 && m->overlay[0].revalidations == 1U && m->overlay[0].invalidations == 0U,
        "an unchanged granule revalidates; A stays selected");
  /* B over A through ordinary stores; the group is re-entered at B's root */
  write_bytes(m, SCD_SUB, 0x020040U, image_b + 0x40U, 64U);
  check(dispatch(m, SCD_SUB, 0x20000U) == 0 && ran_image == 3 && m->overlay[0].selected == 1 && m->overlay[0].invalidations == 1U,
        "B written over A by CPU stores: A is invalidated and B selected at its root");
  check(dispatch(m, SCD_SUB, 0x20002U) != 0 && m->stop.stop_class == SCD_STOP_IMAGE_ENTRY_NOT_COMPILED && ran_image == 0,
        "an address B never compiled stops image_entry_not_compiled (A's entry there is not used)");

  /* stale: A selected, overwritten with B, re-entered at a non-root address */
  power_on(m);
  memcpy(&m->prg_ram[0x20000], image_a, LEN);
  dispatch(m, SCD_SUB, 0x20000U);
  write_bytes(m, SCD_SUB, 0x020040U, image_b + 0x40U, 64U);
  check(dispatch(m, SCD_SUB, 0x20002U) != 0 && m->stop.stop_class == SCD_STOP_STALE_IMAGE_SELECTION && m->stop.address == 2U &&
            ran_image == 0,
        "re-entry outside a root after the overwrite stops stale_image_selection (no instruction of B runs)");

  /* exception frames are writes too */
  power_on(m);
  memcpy(&m->prg_ram[0x20000], image_a, LEN);
  dispatch(m, SCD_SUB, 0x20000U);
  m->cpu[SCD_SUB].a[7] = 0x20048U;
  m->cpu[SCD_SUB].sr = 0x2700U;
  m->prg_ram[0x68] = 0x00; m->prg_ram[0x69] = 0x00; m->prg_ram[0x6A] = 0x60; m->prg_ram[0x6B] = 0x00; /* handler: not compiled */
  scd_raise(m, SCD_SUB, 26U, 0x20002U, 44U, 0x20002U);
  check(m->overlay[0].any_dirty && m->overlay[0].dirty_lo == 1U, "an exception frame pushed into the image marks its granule");

  /* --- main PRG-RAM window group: a bank change is a view change --- */
  power_on(m);
  memcpy(&m->prg_ram[0x00000], image_a, LEN); /* bank 0 offset 0 is main $020000 */
  check(dispatch(m, SCD_MAIN, 0x20000U) == 0 && ran_image == 4, "main sees A through the PRG-RAM window (bank 0)");
  write_bytes(m, SCD_SUB, 0x000000U, image_a, 2U);
  check(m->overlay[1].any_dirty && !m->overlay[0].any_dirty, "a sub store to PRG $000000 dirties main's $020000 group only");
  m->overlay[1].any_dirty = 0U;
  memset(m->overlay[1].dirty, 0, sizeof m->overlay[1].dirty);
  m->overlay[1].dirty_lo = UINT32_MAX;
  m->overlay[1].dirty_hi = 0U;
  scd_write(m, SCD_MAIN, 0xA12003U, 1U, 0x41U, 0U); /* BK = 1, RET (unchanged) */
  check(m->overlay[1].all_dirty && m->overlay[2].all_dirty, "a PRG bank change marks every group for revalidation");
  check(dispatch(m, SCD_MAIN, 0x20000U) != 0 && m->stop.stop_class == SCD_STOP_IMAGE_UNKNOWN && ran_image == 0,
        "bank 1 holds no admitted image: main's selection is revalidated and refused");

  /* --- Word RAM group: writes invisible to the sub while main owns Word RAM, then the handover --- */
  power_on(m);
  write_bytes(m, SCD_MAIN, 0x200000U, image_a, LEN); /* main owns 2M Word RAM at power-on */
  check(!m->overlay[2].any_dirty, "main's Word RAM stores are not in the sub's view while main owns it");
  check(dispatch(m, SCD_SUB, 0x80000U) != 0 && m->stop.stop_class == SCD_STOP_IMAGE_UNKNOWN,
        "the sub cannot execute Word RAM it does not own (no candidate is live)");
  m->stop.stop_class = SCD_STOP_NONE;
  scd_write(m, SCD_MAIN, 0xA12003U, 1U, 0x02U, 0U); /* DMNA: Word RAM to the sub */
  check(m->overlay[2].all_dirty, "the Word RAM handover marks the groups");
  check(dispatch(m, SCD_SUB, 0x80000U) == 0 && ran_image == 2, "after DMNA the sub selects the image main wrote");
  m->word_ram[0x41] ^= 0x01U; /* a store that bypasses the bus would be a runtime bug; the per-instruction guard still catches it */
  check(scd_code_guard(m, SCD_SUB, 0x80041U, image_a + 0x41U, 1U) != 0 && m->stop.stop_class == SCD_STOP_CODE_BYTES_MISMATCH,
        "the per-instruction byte guard compares Word RAM code exactly");

  /* --- cost bound: a store marks O(groups), revalidation compares dirty granules only --- */
  power_on(m);
  memcpy(&m->prg_ram[0x20000], image_a, LEN);
  dispatch(m, SCD_SUB, 0x20000U);
  for (i = 0; i < 1000U; ++i) scd_write(m, SCD_SUB, 0x010000U + 2U * i, 2U, i, 0U); /* stores elsewhere */
  check(!m->overlay[0].any_dirty && dispatch(m, SCD_SUB, 0x20002U) == 0 && m->overlay[0].revalidations == 0U,
        "1000 stores outside the image cost no revalidation");

  printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
