/* SEG-100-T003 (ADR 0223): the runtime binding of a file-first dynamic image to the guest memory the original game loaded it into.
 *
 * A project-authored cooked track and hand-built dynamic-image tables drive scd_dynamic_dispatch on a real ScdMachine: the machine's RAM is
 * filled the way a loader would (byte copies), the delivery ledger is set the way the CDC would, and the test observes which body ran, the
 * binding state, the typed refusal and the stop class. The bodies here are stubs that only count and advance the PC: the byte guard of a real
 * body is covered by the end-to-end tests (tests/sega_cd_file_first_test.py).
 *
 * Cases: bind at a base given nowhere; the same image at two bases, on both processors; replacement of one file by another at the same base;
 * a data write inside a module that is never fetched; a variable the module changes next to code it still runs (the authentic case); an instruction that
 * is overwritten; a partial overwrite; a bank (view) change; ambiguity (a tie is refused, the longest run wins); too short a run; no delivery
 * evidence; an incomplete ledger; an unknown or unmapped fetch; an odd PC; an image with no body at the offset; a transfer's 24-bit wrap; the binding
 * table's bound; computed-control authority; determinism. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"
#include "runtime.h"

static int failures;

static void check(int ok, const char *label) {
  printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

#define SECTORS 64U
static uint8_t track[SECTORS * 2048U];

static void fill(uint8_t *out, uint32_t n, uint32_t seed) {
  uint32_t x = seed * 2654435761U + 12345U, i;
  for (i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = (uint8_t)(x >> 11);
  }
}

static uint64_t calls, last_pc;
static uint32_t last_cpu;

static int stub_body(ScdMachine *m) {
  ScdCpu *const cpu = &m->cpu[m->dyn.cpu];
  ++calls;
  last_pc = cpu->pc;
  last_cpu = m->dyn.cpu;
  cpu->pc += 2U;
  return 0;
}

static const ScdDispatchFunction slice0[2] = {stub_body, NULL};
static const ScdDispatchFunction *const slices[1] = {slice0};
static const uint8_t body_lengths[1] = {2U};   /* the stub body verifies a two-byte instruction */

/* image 1: LBA 4-5, 3000 bytes; image 2: LBA 10, 2048; image 3: LBA 20-21, 3000 (first 1024 bytes equal image 1's); image 4: LBA 30, 48 bytes
 * (shorter than the minimum run); image 5: LBA 40, 4096 zero bytes (every anchor occurrence equal: more candidates than the bound); image 6:
 * LBA 50, 2048 bytes whose map has a hole. */
static uint32_t map1[1500], map2[1024], map3[1500], map4[24], map5[2048], map6[1024];
static ScdDynamicImage images[6];
static ScdDynamicProgram dynamic_program;
static ScdDiscFile disc_files[1];
static ScdDiscTrack disc_tracks[1];
static ScdProgram program;
static ScdAuthentic authentic;
static uint8_t bios[0x200];
static char sha_hex[65];

static void sha_of(const uint8_t *data, uint32_t n, char out[65]) {
  GenesisSha256 sha;
  uint8_t digest[32];
  unsigned i;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, data, n);
  genesis_sha256_final(&sha, digest);
  for (i = 0; i < 32U; ++i) snprintf(out + 2U * i, 3U, "%02x", digest[i]);
}

static void set_map(uint32_t *map, uint32_t count) {
  uint32_t i;
  for (i = 0; i < count; ++i) map[i] = 1U;
}

static uint8_t img[6][4096];
static uint32_t img_len[6] = {3000U, 2048U, 3000U, 48U, 4096U, 2048U};
static uint32_t img_lba[6] = {4U, 10U, 20U, 30U, 40U, 50U};

static ScdMachine *machine;
static ScdDisc disc;

static void reset_machine(void) {
  scd_machine_release(machine);
  if (scd_machine_init_bios(machine, &program, &authentic, &disc, bios, sizeof bios, 1000000U) != 0) {
    printf("FAIL  machine init\n");
    exit(1);
  }
  scd_dynamic_attach(machine);
  calls = 0U;
  last_pc = 0U;
}

static void deliver(uint32_t lba, uint32_t count) {
  machine->delivered[machine->delivered_count].first_lba = lba;
  machine->delivered[machine->delivered_count].count = count;
  machine->delivered_count += 1U;
}

static void deliver_image(uint32_t i) { deliver(img_lba[i], (img_len[i] + 2047U) / 2048U); }

static void put(uint32_t cpu, uint32_t address, const uint8_t *bytes, uint32_t n) {
  uint32_t k;
  for (k = 0; k < n; ++k) *(uint8_t *)scd_code_byte(machine, cpu, address + k) = bytes[k];
}

static int dispatch(uint32_t cpu, uint32_t pc) {
  memset(&machine->stop, 0, sizeof machine->stop);
  machine->cpu[cpu].pc = pc;
  return scd_dynamic_dispatch(machine, cpu, SCD_STOP_ENTRY_NOT_COMPILED);
}

static ScdStopClass stop_class(void) { return machine->stop.stop_class; }

static uint32_t active_bindings(uint32_t cpu) {
  uint32_t k, n = 0U;
  for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) n += machine->dyn.binding[cpu][k].active;
  return n;
}

static const ScdDynamicBinding *binding_at(uint32_t cpu, uint32_t pc) { /* the most recently used binding whose extent contains pc */
  const ScdDynamicBinding *best = NULL;
  uint32_t k;
  for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
    const ScdDynamicBinding *b = &machine->dyn.binding[cpu][k];
    if (b->active && pc >= b->lo && pc < b->hi && (best == NULL || b->order > best->order)) best = b;
  }
  return best;
}

int main(void) {
  static const char name[] = "scd_dyn_binding_test_track.bin";
  FILE *f;
  uint32_t i;
  uint64_t events_a = 0U, events_b = 0U;
  memset(track, 0xE5, sizeof track);
  for (i = 0; i < 6U; ++i) {
    memset(img[i], 0, sizeof img[i]);
    if (i != 4U) fill(img[i], img_len[i], 100U + i);
  }
  memcpy(img[2], img[0], 1024U); /* image 3 shares its first 1024 bytes with image 1 */
  for (i = 0; i < 6U; ++i) memcpy(track + img_lba[i] * 2048U, img[i], img_len[i]);
  f = fopen(name, "wb");
  if (f == NULL || fwrite(track, 1U, sizeof track, f) != sizeof track) {
    printf("FAIL  cannot write the scratch track\n");
    return 1;
  }
  fclose(f);
  sha_of(track, sizeof track, sha_hex);
  disc_files[0].name = name;
  disc_files[0].size = sizeof track;
  disc_files[0].sha256 = sha_hex;
  disc_tracks[0].number = 1U;
  disc_tracks[0].type = 0U;
  disc_tracks[0].file_index = 0U;
  disc_tracks[0].sector_size = 2048U;
  disc_tracks[0].file_offset = 0U;
  disc_tracks[0].first_lba = 0U;
  disc_tracks[0].sector_count = SECTORS;
  program.disc.files = disc_files;
  program.disc.file_count = 1U;
  program.disc.tracks = disc_tracks;
  program.disc.track_count = 1U;
  program.disc.identity = "test";
  set_map(map1, 1500U);
  set_map(map2, 1024U);
  set_map(map3, 1500U);
  set_map(map4, 24U);
  set_map(map5, 2048U);
  set_map(map6, 1024U);
  map6[0x100] = 0U; /* a hole: offset 0x200 has no precompiled body */
  for (i = 0; i < 6U; ++i) {
    images[i].id = i + 1U;
    images[i].first_lba = img_lba[i];
    images[i].length = img_len[i];
    images[i].sha256 = "";
  }
  images[0].body_map = map1;
  images[1].body_map = map2;
  images[2].body_map = map3;
  images[3].body_map = map4;
  images[4].body_map = map5;
  images[5].body_map = map6;
  dynamic_program.images = images;
  dynamic_program.image_count = 6U;
  dynamic_program.body_slices = slices;
  dynamic_program.body_slice_size = 2U;
  dynamic_program.body_count = 1U;
  dynamic_program.body_lengths = body_lengths;
  dynamic_program.policy = "test";
  program.dynamic = &dynamic_program;
  memset(bios, 0, sizeof bios);
  bios[1] = 0xFFU; /* SSP */
  bios[5] = 0x02U; /* reset PC $000200 */
  disc.files = disc_files;
  machine = (ScdMachine *)calloc(1U, sizeof *machine);
  if (machine == NULL) return 1;
  disc = program.disc;
  if (scd_disc_open(&disc, ".") != SCD_DISC_OK) {
    printf("FAIL  cannot open the scratch track\n");
    return 1;
  }

  /* ---- bind at a base given nowhere; the same image at a second base; both processors ---- */
  reset_machine();
  deliver_image(0);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10040U) == 0 && calls == 1U && last_pc == 0x10040U && last_cpu == SCD_SUB, "a PC no static case owns is bound to the delivered image the live bytes copy, and its body runs at the actual PC");
  check(machine->dyn.binds == 1U && binding_at(SCD_SUB, 0x10040U) != NULL && binding_at(SCD_SUB, 0x10040U)->base == 0x10000U && binding_at(SCD_SUB, 0x10040U)->lo == 0x10000U &&
            binding_at(SCD_SUB, 0x10040U)->hi == 0x10000U + 3000U,
        "the binding records the image's base, derived from the live bytes, and the verified run");
  {
    const uint64_t scans = machine->dyn.scans;
    check(dispatch(SCD_SUB, 0x10042U) == 0 && calls == 2U && machine->dyn.binds == 1U && machine->dyn.scans == scans, "an instruction inside a clean binding costs a table lookup: no new search");
  }
  put(SCD_SUB, 0x30000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x30080U) == 0 && machine->dyn.binds == 2U && binding_at(SCD_SUB, 0x30080U)->base == 0x30000U && binding_at(SCD_SUB, 0x10040U)->base == 0x10000U,
        "the same image at a second base is a second binding; the first stays");
  put(SCD_MAIN, 0xFF2000U, img[0], 3000U);
  check(dispatch(SCD_MAIN, 0xFF2000U + 0x100U) == 0 && last_cpu == SCD_MAIN && binding_at(SCD_MAIN, 0xFF2100U) != NULL && binding_at(SCD_MAIN, 0xFF2100U)->base == 0xFF2000U,
        "the body is processor-neutral: the same image binds on the main processor, which then runs it");
  check(active_bindings(SCD_SUB) == 2U && active_bindings(SCD_MAIN) == 1U, "bindings are per processor");
  check(scd_dynamic_lookup(SCD_SUB, 0x10100U) != NULL && scd_dynamic_lookup(SCD_SUB, 0x20000U) == NULL, "computed control: a bound address is a compiled entry, an unbound one is not");
  check(dispatch(SCD_SUB, 0x1010040U) == 0 && last_pc == 0x10040U, "the 24-bit address bus: a PC with bits above bit 23 set is the same address");

  /* ---- the longest run wins; a tie is ambiguous ---- */
  reset_machine();
  deliver_image(0);
  deliver_image(2);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10040U) == 0 && binding_at(SCD_SUB, 0x10040U)->image == 0U, "two delivered images share a prefix: the one the live bytes explain longest is chosen");
  reset_machine();
  deliver_image(0);
  deliver_image(2);
  put(SCD_SUB, 0x10000U, img[0], 1024U);
  check(dispatch(SCD_SUB, 0x10040U) != 0 && stop_class() == SCD_STOP_AMBIGUOUS_IMAGE && machine->dyn.refused[SCD_DYN_REFUSE_AMBIGUOUS] == 1U && active_bindings(SCD_SUB) == 0U,
        "an equal explanation by two distinct images is refused (ambiguous_image), never decided by order");

  reset_machine();
  deliver_image(0);
  deliver_image(2);
  put(SCD_SUB, 0x10000U, img[0], 40U);
  check(dispatch(SCD_SUB, 0x10000U) != 0 && stop_class() == SCD_STOP_ENTRY_NOT_COMPILED && machine->dyn.refused[SCD_DYN_REFUSE_TOO_SHORT] == 1U && machine->dyn.refused[SCD_DYN_REFUSE_AMBIGUOUS] == 0U,
        "two images tie on a coincidence shorter than the minimum run: too_short (no identity), not ambiguous");

  /* ---- a region boundary: main work RAM is mirrored below $FF0000, and a module's un-loaded header lies before the RAM ---- */
  reset_machine();
  deliver_image(1);
  memcpy(machine->work_ram + 0xFF00U, img[1], 0x100U);   /* the top of work RAM happens to hold the image's first 256 bytes: the mirror below $FF0000 shows them */
  put(SCD_MAIN, 0xFF0000U, img[1] + 0x100U, 2048U - 0x100U);   /* the code part of the module, as the loader copies it */
  check(dispatch(SCD_MAIN, 0xFF0000U) == 0 && binding_at(SCD_MAIN, 0xFF0000U) != NULL && binding_at(SCD_MAIN, 0xFF0000U)->base == 0xFEFF00U &&
            binding_at(SCD_MAIN, 0xFF0000U)->run_lo == 0xFF0000U && binding_at(SCD_MAIN, 0xFF0000U)->run_hi == 0xFF0000U + 2048U - 0x100U,
        "a run never crosses a region boundary: the mirror of work RAM's top below $FF0000 is another memory, not the module's header");
  reset_machine();
  deliver_image(1);
  deliver_image(4);                                /* an image of zeros: an all-zero anchor would occur everywhere in it */
  memset(machine->work_ram + 0xFFE0U, 0, 0x20U);   /* zeros before the PC, in the mirrored view */
  put(SCD_MAIN, 0xFF0000U, img[0], 64U);           /* bytes no delivered image has */
  check(dispatch(SCD_MAIN, 0xFF0000U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_NO_SOURCE] == 1U && machine->dyn.refused[SCD_DYN_REFUSE_AMBIGUOUS] == 0U,
        "an anchor before the PC is taken only from the PC's own memory: the mirror's zeros never make a lookup ambiguous");

  /* ---- evidence and typing of refusals ---- */
  reset_machine();
  put(SCD_SUB, 0x10000U, img[1], 2048U);
  check(dispatch(SCD_SUB, 0x10000U) != 0 && stop_class() == SCD_STOP_ENTRY_NOT_COMPILED && machine->dyn.refused[SCD_DYN_REFUSE_NO_DELIVERY] == 1U,
        "no candidate sector was delivered by the CDC: the typed uncompiled-entry stop, reason no_delivery");
  deliver_image(1);
  check(dispatch(SCD_SUB, 0x10000U) == 0 && machine->dyn.binds == 1U, "the same bytes bind once their blocks are in the delivery ledger");
  reset_machine();
  deliver_image(1);
  machine->delivered_overflow = 1U;
  put(SCD_SUB, 0x10000U, img[1], 2048U);
  check(dispatch(SCD_SUB, 0x10000U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_LEDGER_OVERFLOW] == 1U, "an incomplete delivery ledger is never used as evidence");
  reset_machine();
  deliver_image(0);
  fill(machine->prg_ram + 0x10000U, 4096U, 777U);
  check(dispatch(SCD_SUB, 0x10040U) != 0 && stop_class() == SCD_STOP_ENTRY_NOT_COMPILED && machine->dyn.refused[SCD_DYN_REFUSE_NO_SOURCE] == 1U, "live bytes no delivered image explains: no source");
  reset_machine();
  deliver_image(3);
  put(SCD_SUB, 0x10000U, img[3], 48U);
  check(dispatch(SCD_SUB, 0x10000U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_TOO_SHORT] == 1U, "a run shorter than the minimum is refused (a short coincidence is not an identity)");
  reset_machine();
  deliver_image(4);
  put(SCD_SUB, 0x10000U, img[4], 4096U);
  check(dispatch(SCD_SUB, 0x10040U) != 0 && stop_class() == SCD_STOP_AMBIGUOUS_IMAGE && machine->dyn.refused[SCD_DYN_REFUSE_AMBIGUOUS] == 1U,
        "more anchor occurrences than the bound is ambiguity, not a search that runs on");
  reset_machine();
  deliver_image(0);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10041U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_NOT_EXECUTABLE] == 1U && calls == 0U, "an odd PC never binds");
  check(dispatch(SCD_MAIN, 0x100000U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_NOT_EXECUTABLE] == 2U, "a PC outside the executable memory a processor can fetch from never binds");
  check(dispatch(SCD_SUB, 0x7FFFEU + 0x10000U) != 0, "a PC beyond the processor's RAM never binds");

  /* ---- a hole in the image's body map ---- */
  reset_machine();
  deliver_image(5);
  put(SCD_SUB, 0x20000U, img[5], 2048U);
  check(dispatch(SCD_SUB, 0x20000U + 0x200U) != 0 && stop_class() == SCD_STOP_ENTRY_NOT_COMPILED && machine->dyn.refused[SCD_DYN_REFUSE_BODY_NOT_COMPILED] == 1U && calls == 0U,
        "bound, but no precompiled body at that source offset: the typed uncompiled-entry stop (the build driver's frontier), reason body_not_compiled");
  check(dispatch(SCD_SUB, 0x20000U + 0x202U) == 0, "the next offset has its body");

  /* ---- stale mappings, replacement and data writes ---- */
  reset_machine();
  deliver_image(0);
  deliver_image(1);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10040U) == 0 && binding_at(SCD_SUB, 0x10040U)->image == 0U, "replacement: image 1 is bound");
  put(SCD_SUB, 0x10000U, img[1], 2048U);
  scd_dynamic_note_write(machine, SCD_MEMORY_PRG, 0x10000U, 2048U);
  check(dispatch(SCD_SUB, 0x10040U) == 0 && binding_at(SCD_SUB, 0x10040U) != NULL && binding_at(SCD_SUB, 0x10040U)->image == 1U && machine->dyn.invalidations == 1U && machine->dyn.binds == 2U,
        "another file loaded over the same base: the old mapping no longer explains the instruction (skipped), and the new file is identified at the first fetch");
  {
    const ScdDynamicBinding *b = binding_at(SCD_SUB, 0x10040U);
    check(b->base == 0x10000U && b->lo == 0x10000U && b->hi == 0x10000U + 2048U && b->run_hi == b->hi && machine->dyn.replacements == 1U, "the replacement covers exactly the new file; the overlap is counted");
  }
  /* a data write inside a module's extent, in a granule never fetched: nothing is re-verified */
  reset_machine();
  deliver_image(0);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10040U) == 0, "data write: bound");
  machine->prg_ram[0x10000U + 2501U] ^= 0xFFU;
  scd_dynamic_note_write(machine, SCD_MEMORY_PRG, 0x10000U + 2501U, 1U);
  check(dispatch(SCD_SUB, 0x10042U) == 0 && machine->dyn.rechecks == 0U && machine->dyn.granule_compares == 0U && machine->dyn.invalidations == 0U,
        "a write to a granule the fetch does not touch costs nothing and keeps the mapping");
  /* the authentic case: a variable the module changes sits next to code that is still the image's - the granule is mutated, the instruction is not */
  check(dispatch(SCD_SUB, 0x10000U + 2496U) == 0 && last_pc == 0x10000U + 2496U && machine->dyn.invalidations == 0U && machine->dyn.recheck_failures == 0U && machine->dyn.granule_compares == 1U,
        "a fetch in a granule whose DATA changed, from an instruction that is still the image's, runs: the granule is found mutated once, never trusted whole");
  check(dispatch(SCD_SUB, 0x10000U + 2498U) == 0 && machine->dyn.granule_compares == 1U && machine->dyn.rechecks == 2U,
        "an instruction in a mutated granule is compared with the image's bytes (two bytes), not the whole granule again");
  check(dispatch(SCD_SUB, 0x10000U + 2500U) != 0 && machine->dyn.recheck_failures == 2U && machine->dyn.invalidations == 1U && machine->dyn.refused[SCD_DYN_REFUSE_INSTRUCTION_DIFFERS] == 1U,
        "an instruction whose own bytes changed is not served by the old mapping; a fresh search identifies the image from the run before it and finds the instruction is still not the image's: typed refusal");
  check(machine->stop.stop_class == SCD_STOP_ENTRY_NOT_COMPILED, "the changed instruction is not executed: a typed stop, not the old body");
  check(dispatch(SCD_SUB, 0x10042U) == 0 && machine->dyn.binds == 2U, "the module's other instructions still run (the identifying run was refreshed, not lost)");
  /* a write that restores the same bytes leaves the granule equal: compared once, trusted again */
  reset_machine();
  deliver_image(0);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  check(dispatch(SCD_SUB, 0x10040U) == 0, "restore: bound");
  scd_dynamic_note_write(machine, SCD_MEMORY_PRG, 0x10040U, 4U);
  check(dispatch(SCD_SUB, 0x10042U) == 0 && machine->dyn.granule_compares == 1U && machine->dyn.granule_upgrades == 1U && machine->dyn.rechecks == 0U && machine->dyn.invalidations == 0U && machine->dyn.binds == 1U,
        "a dirty granule that still equals the image is compared once and trusted again");
  /* beyond the exact-copy run (a module longer than the run that identified it): granules are unknown until seen equal */
  reset_machine();
  deliver_image(0);
  put(SCD_SUB, 0x10000U, img[0], 3000U);
  machine->prg_ram[0x10000U + 1000U] ^= 0xFFU;   /* the run that identifies the image at 0x10040 ends here */
  check(dispatch(SCD_SUB, 0x10040U) == 0 && binding_at(SCD_SUB, 0x10040U)->run_hi == 0x10000U + 1000U && binding_at(SCD_SUB, 0x10040U)->hi == 0x10000U + 3000U,
        "the identifying run is shorter than the image: the binding still serves the image's whole extent");
  check(dispatch(SCD_SUB, 0x10000U + 2000U) == 0 && machine->dyn.binds == 1U && machine->dyn.granule_upgrades >= 1U, "an instruction beyond the run is served once its granule is seen equal to the image");
  check(dispatch(SCD_SUB, 0x10000U + 1000U) != 0 && machine->dyn.refused[SCD_DYN_REFUSE_NO_SOURCE] == 1U, "the changed byte itself is not served: no image explains an instruction that starts on it");
  /* a partial overwrite: first half image 1, second half image 2 */
  reset_machine();
  deliver_image(0);
  deliver_image(1);
  put(SCD_SUB, 0x10000U, img[0], 1536U);
  put(SCD_SUB, 0x10000U + 1536U, img[1], 1024U);
  check(dispatch(SCD_SUB, 0x10040U) == 0 && binding_at(SCD_SUB, 0x10040U)->image == 0U && binding_at(SCD_SUB, 0x10040U)->run_hi == 0x10000U + 1536U, "a partly overwritten module binds the part the live bytes still explain");
  check(dispatch(SCD_SUB, 0x10000U + 1600U) == 0 && binding_at(SCD_SUB, 0x10000U + 1600U) != NULL && binding_at(SCD_SUB, 0x10000U + 1600U)->image == 1U &&
            binding_at(SCD_SUB, 0x10000U + 1600U)->base == 0x10000U + 1536U && binding_at(SCD_SUB, 0x10040U)->image == 0U,
        "the other part is identified as the other file at its own base: each part runs only through the image its bytes copy");
  check(dispatch(SCD_SUB, 0x10042U) == 0 && machine->dyn.binds == 2U, "the first part still runs through the first mapping, which was skipped, not dropped, for the other part");

  /* ---- a bank (view) change ---- */
  reset_machine();
  deliver_image(0);
  machine->ga.prg_window = 1U;
  machine->ga.prg_bank = 1U;
  put(SCD_MAIN, 0x21000U, img[0], 3000U); /* main sees PRG RAM bank 1 at $020000 */
  check(dispatch(SCD_MAIN, 0x21040U) == 0 && binding_at(SCD_MAIN, 0x21040U) != NULL, "view change: bound through the PRG-RAM window");
  machine->ga.prg_bank = 0U;
  scd_dynamic_note_view_change(machine);
  check(dispatch(SCD_MAIN, 0x21040U) != 0 && machine->dyn.invalidations == 1U && machine->dyn.recheck_failures >= 1U, "after a bank change nothing is trusted: the next fetch is compared and the mapping no longer explains it - it never executes other memory");
  machine->ga.prg_bank = 1U;
  scd_dynamic_note_view_change(machine);
  check(dispatch(SCD_MAIN, 0x21040U) == 0 && machine->dyn.binds == 1U, "the window back where the image is: the same mapping explains the instruction again");

  /* ---- the binding table's bound ---- */
  reset_machine();
  deliver_image(1);
  for (i = 0; i < SCD_DYN_MAX_BINDINGS + 3U; ++i) {
    put(SCD_SUB, 0x1000U + i * 0x1000U, img[1], 2048U);
    if (dispatch(SCD_SUB, 0x1000U + i * 0x1000U) != 0) break;
  }
  check(i == SCD_DYN_MAX_BINDINGS + 3U && active_bindings(SCD_SUB) == SCD_DYN_MAX_BINDINGS && binding_at(SCD_SUB, 0x1000U) == NULL, "the table is bounded: the oldest mapping is evicted, and fetching it again re-identifies it");
  check(dispatch(SCD_SUB, 0x1000U) == 0 && binding_at(SCD_SUB, 0x1000U) != NULL, "an evicted mapping is re-identified from the live bytes");

  /* ---- determinism ---- */
  for (i = 0; i < 2U; ++i) {
    uint64_t sum = 0U;
    uint32_t e;
    reset_machine();
    deliver_image(0);
    deliver_image(1);
    put(SCD_SUB, 0x10000U, img[0], 3000U);
    put(SCD_MAIN, 0xFF4000U, img[1], 2048U);
    (void)dispatch(SCD_SUB, 0x10040U);
    (void)dispatch(SCD_MAIN, 0xFF4000U);
    put(SCD_SUB, 0x10000U, img[1], 2048U);
    scd_dynamic_note_write(machine, SCD_MEMORY_PRG, 0x10000U, 2048U);
    (void)dispatch(SCD_SUB, 0x10040U);
    for (e = 0; e < machine->dyn.events && e < SCD_DYN_LOG; ++e) sum = sum * 1000003U + machine->dyn.log[e].kind * 131U + machine->dyn.log[e].pc + machine->dyn.log[e].image * 7U + machine->dyn.log[e].base;
    if (i == 0U) events_a = sum + machine->dyn.scan_bytes;
    else events_b = sum + machine->dyn.scan_bytes;
  }
  check(events_a == events_b && events_a != 0U, "the same sequence gives the same events, in the same order, with the same search cost");

  /* ---- the report ---- */
  {
    static const char report_name[] = "scd_dyn_binding_test_report.tmp";
    FILE *out = fopen(report_name, "w+b"); /* not tmpfile(): on Windows it writes to the drive root */
    char buffer[8192];
    size_t n;
    if (out != NULL) {
      scd_dynamic_report(machine, out);
      rewind(out);
      n = fread(buffer, 1U, sizeof buffer - 1U, out);
      buffer[n] = '\0';
      check(strstr(buffer, "\"dynamic\":{") != NULL && strstr(buffer, "\"binds\":") != NULL && strstr(buffer, "\"events\":[") != NULL, "the end-of-run report carries the dynamic section");
      fclose(out);
      remove(report_name);
    }
  }

  scd_machine_release(machine);
  scd_disc_close(&disc);
  free(machine);
  remove(name);
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
