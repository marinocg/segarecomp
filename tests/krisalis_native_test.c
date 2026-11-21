/*
 * Unit tests of the native Krisalis replacement core (ADR 0318) against project-authored synthetic data: no commercial data and no original driver code is involved.
 * Expected chip accesses are derived by hand from the published behaviour of the verified driver lineage (mailbox protocol, 32-byte voices written in register-table
 * order, block/F-number words, key-off-then-on re-trigger, carrier attenuation, the DAC player) - never from this implementation's output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "krisalis_native.h"

static int g_failures;
static const char *g_test = "";
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; fprintf(stderr, "FAIL [%s] line %d: ", g_test, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct Access { uint8_t port, value; uint64_t ticks; } Access;

typedef struct Fixture {
  uint8_t rom[0x20000];
  uint8_t ram[8192];
  uint32_t bank;
  Access log[40000];
  size_t count;
  KrisalisNative driver;
  uint8_t revision;
  uint8_t commands[64][13];
  unsigned command_count;
} Fixture;

static int fx_rom_read(void *c, uint32_t address, uint8_t *destination, uint32_t count) {
  Fixture *f = (Fixture *)c;
  if ((size_t)address + count > sizeof f->rom) return 0;
  memcpy(destination, f->rom + address, count);
  return 1;
}
static void fx_ym_write(void *c, uint32_t port, uint8_t value, uint64_t ticks) {
  Fixture *f = (Fixture *)c;
  if (f->count < sizeof f->log / sizeof f->log[0]) { f->log[f->count].port = (uint8_t)port; f->log[f->count].value = value; f->log[f->count++].ticks = ticks; }
}
static uint32_t fx_bank_get(void *c) { return ((Fixture *)c)->bank; }
static void fx_bank_write(void *c, uint8_t bit, uint64_t ticks) { Fixture *f = (Fixture *)c; (void)ticks; f->bank = ((uint32_t)(bit & 1u) << 8) | (f->bank >> 1); }
static void fx_command_tap(void *c, const uint8_t *bytes, unsigned count, uint64_t ticks) {
  Fixture *f = (Fixture *)c;
  (void)ticks;
  if (f->command_count < 64u && count == 13u) memcpy(f->commands[f->command_count++], bytes, 13u);
}

static void fx_host(Fixture *f, KrisalisNativeHost *host) {
  memset(host, 0, sizeof *host);
  host->context = f;
  host->ram = f->ram;
  host->rom_read = fx_rom_read;
  host->ym_write = fx_ym_write;
  host->bank_get = fx_bank_get;
  host->bank_write = fx_bank_write;
  host->command_tap = fx_command_tap;
}

static Fixture *fx_new(uint8_t revision) {
  Fixture *f = (Fixture *)calloc(1, sizeof *f);
  KrisalisNativeHost host;
  f->revision = revision;
  fx_host(f, &host);
  krisalis_native_start(&f->driver, &host, revision, 0u);
  return f;
}

static unsigned mailbox(const Fixture *f) { return f->revision == 1u ? 0x0E58u : f->revision == 2u ? 0x0F58u : 0x0FACu; }
static uint8_t *mbox(Fixture *f, unsigned off) { return &f->ram[mailbox(f) + off]; }
static uint8_t *voice(Fixture *f, unsigned n) { return &f->ram[0x400 + n * 32u]; }

/* the host as the 68000 side: run to a time, put a command and its cells in the mailbox (the driver sees them when the bus is back) */
static void run_to(Fixture *f, uint64_t t) { CHECK(krisalis_native_run_to(&f->driver, t) == KRISALIS_FAULT_NONE, "fault %s", krisalis_native_fault_name(f->driver.fault)); }
#define MS(n) ((uint64_t)(n) * 53693u)   /* master ticks per millisecond, NTSC (53.693 MHz / 1000) */

/* the 68000 takes the bus at `t`, stores the cells and the command, and gives it back at `t`: the driver notices the command only from then on */
static void send_at(Fixture *f, uint64_t t, uint8_t command, const uint8_t *cells) {
  (void)krisalis_native_run_to(&f->driver, t);
  krisalis_native_suspend(&f->driver, t);
  if (cells != NULL) memcpy(mbox(f, 2), cells, 12u);
  *mbox(f, 0) = command;
  krisalis_native_resume(&f->driver, t);
}

static void make_voices(Fixture *f) {   /* voice n: byte i = n * 16 + i (so every register value is traceable); TL bytes small, ALG/FB byte 0x18 = 7 for voice 1 */
  unsigned n, i;
  for (n = 0; n < 64u; ++n)
    for (i = 0; i < 32u; ++i) voice(f, n)[i] = (uint8_t)((n * 16u + i) & 0x7Fu);
  voice(f, 1)[0x18] = 0x07u;   /* algorithm 7: every operator a carrier */
  voice(f, 2)[0x18] = 0x02u;   /* algorithm 2: only operator 4 a carrier */
  voice(f, 1)[0x19] = 0xC5u;
}

/* the register list of a voice on channel c (0..5), as published: $30..$3C, $40..$4C, ... $80..$8C, $B0, $B4, each plus (c % 3), part II for c >= 3 */
static const uint8_t k_regs[26] = {0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0xB0, 0xB4};

/* collects the register writes (part, register, value) of the log from index `from`, skipping nothing */
typedef struct Reg { unsigned part, reg; uint8_t value; } Reg;
static size_t registers(const Fixture *f, size_t from, Reg *out, size_t capacity) {
  unsigned latch[2] = {0, 0};
  size_t i, n = 0;
  for (i = from; i < f->count && n < capacity; ++i) {
    const unsigned part = f->log[i].port >> 1;
    if ((f->log[i].port & 1u) == 0u) latch[part] = f->log[i].value;
    else { out[n].part = part; out[n].reg = latch[part]; out[n++].value = f->log[i].value; }
  }
  return n;
}

static void test_start(void) {
  unsigned rev;
  static const uint16_t expect[4] = {0, 0x0E58, 0x0F58, 0x0FAC};
  for (rev = 1; rev <= 3u; rev += 2u) {
    Fixture *f = fx_new((uint8_t)rev);
    Reg r[8];
    size_t n;
    run_to(f, MS(100));
    CHECK(f->ram[4] == (expect[rev] & 0xFFu) && f->ram[5] == (expect[rev] >> 8), "rev %u: the mailbox address is published at $0004/$0005 (got %02X%02X)", rev, f->ram[5], f->ram[4]);
    CHECK(f->ram[1] == 0xC3u && f->ram[2] == 0x00u && f->ram[3] == 0x0Cu, "rev %u: the reset vector is nop / jp $0C00", rev);
    n = registers(f, 0, r, 8);
    CHECK(n == 2u && r[0].part == 0u && r[0].reg == 0x22u && r[0].value == 0x0Cu && r[1].reg == 0x27u && r[1].value == 0u, "rev %u: start-up writes LFO on (reg $22 = $0C) and timers off (reg $27 = 0), %zu writes", rev, n);
    free(f);
  }
}

static void test_ack_protocol(void) {
  Fixture *f3 = fx_new(3u), *f1 = fx_new(1u);
  const uint8_t cell[12] = {0};
  run_to(f3, MS(50)); run_to(f1, MS(50));
  send_at(f3, MS(50), 1u, cell); send_at(f1, MS(50), 1u, cell);
  run_to(f3, MS(50) + 50u * 15u * 4u);   /* shortly after the bus is back */
  CHECK(*mbox(f3, 0) == 0u, "rev 3 acknowledges when it READS the command");
  run_to(f1, MS(50) + 40u * 15u);
  CHECK(*mbox(f1, 0) != 0u, "rev 1 has not acknowledged a command it has only just seen");
  run_to(f1, MS(60));
  CHECK(*mbox(f1, 0) == 0u, "rev 1 acknowledges when the work is done");
  CHECK(f3->driver.count_commands == 1u && f1->driver.count_commands == 1u, "each command is consumed exactly once (%llu, %llu)", (unsigned long long)f3->driver.count_commands, (unsigned long long)f1->driver.count_commands);
  free(f3); free(f1);
}

/* cell = lo, hi; N in hi bits 7-1, instrument I = (hi bit0 : lo bits 7-4), effect E = lo bits 3-0 */
static void cell_set(uint8_t *cells, unsigned channel, unsigned note, unsigned instrument, unsigned effect) {
  cells[2u * channel] = (uint8_t)(((instrument & 0x0Fu) << 4) | (effect & 0x0Fu));
  cells[2u * channel + 1u] = (uint8_t)((note << 1) | ((instrument >> 4) & 1u));
}

static void test_note_with_voice(unsigned rev) {
  Fixture *f = fx_new((uint8_t)rev);
  uint8_t cells[12] = {0};
  Reg r[200];
  size_t n, i, base;
  make_voices(f);
  run_to(f, MS(5));
  base = f->count;
  cell_set(cells, 0, 3u, 2u, 0u);   /* note 3 (block 0, F-number $28D), instrument 2 = voice 1 */
  send_at(f, MS(5), 10u, cells);
  run_to(f, MS(200));
  n = registers(f, base, r, 200);
  /* 26 voice registers (rev 3 with the music volume 0: nothing between), then A4/A0 and key off, then A4/A0 and key on */
  CHECK(n == 26u + 6u, "rev %u: %zu register writes (expected 26 voice + 6 pitch/key)", rev, n);
  for (i = 0; i < 26u && i < n; ++i) {
    CHECK(r[i].part == 0u && r[i].reg == k_regs[i], "rev %u: voice register %zu is $%02X (got %02X)", rev, i, k_regs[i], r[i].reg);
    CHECK(r[i].value == voice(f, 1)[i], "rev %u: voice byte %zu", rev, i);
  }
  if (n >= 32u) {
    CHECK(r[26].reg == 0xA4u && r[26].value == 0x02u, "A4 = block 0, F-number high ($028D >> 8)");
    CHECK(r[27].reg == 0xA0u && r[27].value == 0x8Du, "A0 = F-number low");
    CHECK(r[28].part == 0u && r[28].reg == 0x28u && r[28].value == 0x00u, "key off first (the note re-triggers)");
    CHECK(r[29].reg == 0xA4u && r[30].reg == 0xA0u, "the frequency is written again");
    CHECK(r[31].reg == 0x28u && r[31].value == 0xF0u, "key on, all four operators, channel 0");
  }
  free(f);
}

static void test_channel_mapping(void) {
  Fixture *f = fx_new(3u);
  uint8_t cells[12] = {0};
  Reg r[200];
  size_t n, base;
  make_voices(f);
  run_to(f, MS(5));
  base = f->count;
  cell_set(cells, 4, 14u, 3u, 0u);   /* FM5: part II, channel offset 1 */
  send_at(f, MS(5), 10u, cells);
  run_to(f, MS(200));
  n = registers(f, base, r, 200);
  CHECK(n == 32u, "%zu writes", n);
  if (n == 32u) {
    CHECK(r[0].part == 1u && r[0].reg == 0x31u, "FM5 voice goes to part II, register $31");
    CHECK(r[26].part == 1u && r[26].reg == 0xA5u, "FM5 frequency: A4 + 1 in part II");
    CHECK(r[28].part == 0u && r[28].reg == 0x28u && r[28].value == 0x05u, "key off of FM5 is register $28 with channel code 5 (the key code skips 3), got %02X", r[28].value);
  }
  free(f);
}

static void test_volume_attenuation(void) {
  Fixture *f = fx_new(3u);
  uint8_t cells[12] = {0};
  Reg r[200];
  size_t n, base, i;
  make_voices(f);
  voice(f, 1)[4] = 0x70u; voice(f, 1)[5] = 0x10u; voice(f, 1)[6] = 0x20u; voice(f, 1)[7] = 0x30u;
  *mbox(f, 0x3A) = 0x20u;   /* music attenuation */
  run_to(f, MS(5));
  base = f->count;
  cell_set(cells, 0, 3u, 2u, 0u);
  send_at(f, MS(5), 10u, cells);
  run_to(f, MS(300));
  n = registers(f, base, r, 200);
  (void)i;
  /* algorithm 7: every TL gets +$20, clamped at $7F (0x70 + 0x20 -> $7F) */
  if (n > 8u) {
    CHECK(r[4].reg == 0x40u && r[4].value == 0x7Fu, "TL1 clamps at $7F (got %02X)", r[4].value);
    CHECK(r[5].reg == 0x44u && r[5].value == 0x30u, "TL3 = $10 + $20");
    CHECK(r[6].reg == 0x48u && r[6].value == 0x40u, "TL2 = $20 + $20");
    CHECK(r[7].reg == 0x4Cu && r[7].value == 0x50u, "TL4 = $30 + $20");
  } else CHECK(0, "too few writes: %zu", n);
  free(f);
}

static void test_dac_sample(void) {
  Fixture *f = fx_new(3u);
  Reg r[64];
  size_t n, base, i;
  unsigned seen = 0u;
  make_voices(f);
  /* an SFX sample: bank 1, window address $8010, 20 bytes, rate $FF */
  for (i = 0; i < 20u; ++i) f->rom[(1u << 15) + 0x10u + i] = (uint8_t)(i * 3u);
  *mbox(f, 0x10) = 0x10u; *mbox(f, 0x11) = 0x80u; *mbox(f, 0x12) = 0xFFu; *mbox(f, 0x14) = 20u; *mbox(f, 0x15) = 0u; *mbox(f, 0x42) = 1u; *mbox(f, 0x3E) = 0u;
  run_to(f, MS(5));
  base = f->count;
  (void)krisalis_native_run_to(&f->driver, MS(5));
  krisalis_native_suspend(&f->driver, MS(5));
  *mbox(f, 0x16) = 1u;   /* trigger */
  krisalis_native_resume(&f->driver, MS(5));
  run_to(f, MS(20));
  CHECK(f->bank == 1u, "the sample's bank is selected by nine bank-register writes (bank %u)", f->bank);
  CHECK(*mbox(f, 0x16) == 0u, "the trigger is cleared");
  CHECK(*mbox(f, 0x38) == 0u, "the SFX flag is clear again after the sample ended");
  n = registers(f, base, r, 64);
  CHECK(n >= 3u && r[0].part == 1u && r[0].reg == 0xB6u && r[0].value == 0xC0u, "FM6 pan set to L+R first");
  CHECK(r[1].part == 0u && r[1].reg == 0x2Bu && r[1].value == 0x80u, "DAC enabled");
  /* the first eight and the last four bytes are skipped: 20 - 12 = 8 bytes, window offsets 8..15; each is written as signed -> unsigned */
  for (i = 2; i < n; ++i) {
    if (r[i].part == 0u && r[i].reg == 0x2Au) {
      CHECK(seen < 8u && r[i].value == (uint8_t)((f->rom[(1u << 15) + 0x10u + 8u + seen]) + 0x80u), "DAC byte %u", seen);
      ++seen;
    }
  }
  CHECK(seen == 8u, "8 DAC bytes (got %u)", seen);
  CHECK(n >= 2u && r[n - 1].reg == 0x2Bu && r[n - 1].value == 0u, "DAC switched off at the end");
  free(f);
}

static void test_faults_and_hold(void) {
  Fixture *f = fx_new(3u);
  Fixture *g = fx_new(3u);
  size_t before;
  uint8_t cells[12] = {0};
  make_voices(f); make_voices(g);
  /* a sample outside the owned cartridge: typed fault, nothing more is written, nothing acknowledged */
  *mbox(f, 0x10) = 0x00u; *mbox(f, 0x11) = 0x80u; *mbox(f, 0x12) = 0x80u; *mbox(f, 0x14) = 64u; *mbox(f, 0x42) = 255u;   /* bank 255: beyond the 128 KiB fixture */
  run_to(f, MS(5));
  krisalis_native_suspend(&f->driver, MS(5));
  *mbox(f, 0x16) = 1u;
  krisalis_native_resume(&f->driver, MS(5));
  (void)krisalis_native_run_to(&f->driver, MS(40));
  CHECK(f->driver.fault == KRISALIS_FAULT_UNMAPPED_RESOURCE, "a sample read outside the cartridge faults (%s)", krisalis_native_fault_name(f->driver.fault));
  before = f->count;
  (void)krisalis_native_run_to(&f->driver, MS(80));
  CHECK(f->count == before, "a faulted driver stays silent");
  CHECK(krisalis_native_state_put(&f->driver, (uint8_t *)calloc(1, 4096), 4096u) == 0u, "a faulted driver has no state to save");
  /* a bus hold delays pending work by its length */
  cell_set(cells, 0, 3u, 2u, 0u);
  run_to(g, MS(5));
  send_at(g, MS(5), 10u, cells);
  run_to(g, MS(5) + 3000u);
  krisalis_native_suspend(&g->driver, MS(5) + 3000u);
  before = g->count;
  run_to(g, MS(25));
  CHECK(g->count == before, "nothing runs while the bus is held");
  krisalis_native_resume(&g->driver, MS(25));
  run_to(g, MS(60));
  CHECK(g->count > before && g->log[before].ticks >= MS(25), "work resumes after the hold, not during it");
  free(f); free(g);
}

static void test_cell_commands_rev3(void) {
  Fixture *f = fx_new(3u);
  uint8_t cells[12] = {0};
  Reg r[200];
  size_t n, base;
  make_voices(f);
  run_to(f, MS(5));
  /* $7D with bit 7 selects the upper voice bank: the next instrument is voice 32 + (I - 1) */
  cell_set(cells, 0, 0x7Du, 0u, 0u); cells[0] = 0x80u;
  send_at(f, MS(5), 10u, cells);
  run_to(f, MS(30));
  CHECK(*mbox(f, 0x0E) == 0xFFu, "the upper-bank flag is set in the mailbox");
  memset(cells, 0, sizeof cells);
  cell_set(cells, 0, 3u, 2u, 0u);
  base = f->count;
  send_at(f, MS(30), 10u, cells);
  run_to(f, MS(300));
  n = registers(f, base, r, 200);
  CHECK(n >= 26u && r[0].value == voice(f, 33)[0], "instrument 2 of the upper bank is voice 33");
  /* $7B pan override: right only (1) -> pan bits $40 in the voice's $B4 byte */
  memset(cells, 0, sizeof cells);
  cell_set(cells, 0, 0x7Bu, 0u, 1u);
  base = f->count;
  send_at(f, MS(300), 10u, cells);
  run_to(f, MS(600));
  n = registers(f, base, r, 200);
  CHECK(n >= 26u, "the voice is reloaded with the override (%zu writes)", n);
  if (n >= 26u) CHECK(r[25].reg == 0xB4u && (r[25].value & 0xC0u) == 0x40u, "pan = right: %02X", r[25].value);
  free(f);
}

/* revision 2: the acknowledge comes when the work is done (like revision 1), and every DAC poll rewrites the FM6 pan before the byte */
static void test_revision_2(void) {
  Fixture *f = fx_new(2u);
  Reg r[200];
  size_t n, base, i;
  unsigned bytes = 0u, pans = 0u;
  const uint8_t cell[12] = {0};
  run_to(f, MS(50));
  send_at(f, MS(50), 1u, cell);
  run_to(f, MS(50) + 40u * 15u);
  CHECK(*mbox(f, 0) != 0u, "rev 2 has not acknowledged a command it has only just seen");
  run_to(f, MS(60));
  CHECK(*mbox(f, 0) == 0u, "rev 2 acknowledges when the work is done");
  for (i = 0; i < 20u; ++i) f->rom[(2u << 15) + 0x10u + i] = (uint8_t)(i * 5u);
  *mbox(f, 0x10) = 0x10u; *mbox(f, 0x11) = 0x80u; *mbox(f, 0x12) = 0x80u; *mbox(f, 0x14) = 20u; *mbox(f, 0x42) = 2u; *mbox(f, 0x3E) = 0x80u;
  (void)krisalis_native_run_to(&f->driver, MS(70));
  krisalis_native_suspend(&f->driver, MS(70));
  *mbox(f, 0x16) = 1u;
  krisalis_native_resume(&f->driver, MS(70));
  base = f->count;
  run_to(f, MS(90));
  n = registers(f, base, r, 200);
  CHECK(f->bank == 2u, "the sample's bank is selected");
  for (i = 0; i < n; ++i) {
    if (r[i].part == 1u && r[i].reg == 0xB6u && r[i].value == 0x80u) ++pans;
    if (r[i].part == 0u && r[i].reg == 0x2Au) {
      CHECK(bytes < 8u && r[i].value == (uint8_t)(f->rom[(2u << 15) + 0x10u + 8u + bytes] + 0x80u), "rev 2 DAC byte %u", bytes);
      ++bytes;
    }
  }
  CHECK(bytes == 8u, "8 DAC bytes (got %u)", bytes);
  CHECK(pans > bytes, "every poll rewrites the FM6 pan (the SFX pan $80), %u pan writes for %u bytes", pans, bytes);
  CHECK(*mbox(f, 0x38) == 0u, "the SFX flag is clear after the sample");
  free(f);
}

static void test_state(void) {
  Fixture *a = fx_new(3u), *b = fx_new(3u);
  uint8_t payload[KRISALIS_NATIVE_STATE_MAX_BYTES];
  uint32_t length;
  uint8_t cells[12] = {0};
  size_t i, base;
  KrisalisNative stage;
  make_voices(a);
  cell_set(cells, 0, 3u, 2u, 0u);
  cell_set(cells, 1, 20u, 2u, 3u);
  run_to(a, MS(5));
  send_at(a, MS(5), 10u, cells);
  run_to(a, MS(5) + 120u * 15u * 20u);   /* mid-command: effects pending in the queue */
  length = krisalis_native_state_put(&a->driver, payload, sizeof payload);
  CHECK(length > 0u && length <= KRISALIS_NATIVE_STATE_MAX_BYTES, "state length %u", length);
  /* a different, running driver adopts the state and the RAM, and both continue identically */
  memcpy(b->ram, a->ram, sizeof a->ram);
  b->bank = a->bank;
  stage = b->driver;
  CHECK(krisalis_native_state_get(&stage, payload, length, MS(5) + 120u * 15u * 20u), "the state is accepted");
  { KrisalisNativeHost host; fx_host(b, &host); stage.host = host; b->driver = stage; }
  base = a->count;
  b->count = 0u;
  run_to(a, MS(400));
  run_to(b, MS(400));
  CHECK(a->count - base == b->count && b->count > 0u, "same number of writes after the restore (%zu vs %zu)", a->count - base, b->count);
  for (i = 0; i < b->count && i + base < a->count; ++i)
    CHECK(a->log[base + i].port == b->log[i].port && a->log[base + i].value == b->log[i].value && a->log[base + i].ticks == b->log[i].ticks, "write %zu differs after the restore", i);
  /* refusals */
  stage = b->driver;
  CHECK(!krisalis_native_state_get(&stage, payload, 0u, MS(500)), "empty payload refused");
  CHECK(!krisalis_native_state_get(&stage, payload, length - 1u, MS(500)), "truncated payload refused");
  {
    uint8_t longer[KRISALIS_NATIVE_STATE_MAX_BYTES + 1];
    memcpy(longer, payload, length);
    longer[length] = 0u;
    CHECK(!krisalis_native_state_get(&stage, longer, length + 1u, MS(500)), "a trailing byte is refused");
  }
  payload[0] = 9u;
  CHECK(!krisalis_native_state_get(&stage, payload, length, MS(500)), "an unknown revision is refused");
  payload[0] = 3u;
  CHECK(!krisalis_native_state_get(&stage, payload, length, 1000u), "a state from the future of this guest clock is refused");
  free(a); free(b);
}


/* ---- corrupt snapshots: structurally valid payloads of states the driver could never be in --------------------------------------------------------------------- */
typedef void (*Mutation)(KrisalisNative *k);

static void prepare_busy(Fixture *f) {   /* a revision-3 driver with an active sample and a partially processed row */
  uint8_t cells[12] = {0};
  size_t i;
  make_voices(f);
  for (i = 0; i < 2000u; ++i) f->rom[(1u << 15) + 0x10u + i] = (uint8_t)(i * 3u);
  *mbox(f, 0x10) = 0x10u; *mbox(f, 0x11) = 0x80u; *mbox(f, 0x12) = 0x80u; *mbox(f, 0x14) = 0xD0u; *mbox(f, 0x15) = 0x07u; *mbox(f, 0x42) = 1u;
  run_to(f, MS(5));
  (void)krisalis_native_run_to(&f->driver, MS(5));
  krisalis_native_suspend(&f->driver, MS(5));
  *mbox(f, 0x16) = 1u;
  krisalis_native_resume(&f->driver, MS(5));
  run_to(f, MS(20));
  cell_set(cells, 0, 3u, 2u, 0u);
  cell_set(cells, 1, 20u, 3u, 0u);
  send_at(f, MS(20), 10u, cells);
  run_to(f, MS(20) + 300u * 15u * 8u);   /* mid-row */
}

static void m_wrong_revision(KrisalisNative *k) { k->revision = KRISALIS_REVISION_1; }
static void m_unknown_revision(KrisalisNative *k) { k->revision = 9u; }
static void m_zero_revision(KrisalisNative *k) { k->revision = 0u; }
static void m_rate_zero_finite_due(KrisalisNative *k) { k->dac_rate = 0u; k->dac_due = k->floor_ticks + 1000u; }
static void m_active_no_left(KrisalisNative *k) { k->dac_left = 0u; }
static void m_active_no_start(KrisalisNative *k) { k->count_samples_started = 0u; }
static void m_active_no_due(KrisalisNative *k) { k->dac_due = UINT64_MAX; }
static void m_due_far_future(KrisalisNative *k) { k->dac_due = UINT64_MAX - 5u; }
static void m_op_kind(KrisalisNative *k) { k->queue[k->queue_head].kind = 77u; }
static void m_ym_port(KrisalisNative *k) { size_t i; for (i = 0; i < k->queue_count; ++i) { KrisalisOp *o = &k->queue[(k->queue_head + i) % KRISALIS_QUEUE_MAX]; if (o->kind == 1u) { o->a = 9u; return; } } }
static void m_ym_operand_b(KrisalisNative *k) { size_t i; for (i = 0; i < k->queue_count; ++i) { KrisalisOp *o = &k->queue[(k->queue_head + i) % KRISALIS_QUEUE_MAX]; if (o->kind == 1u) { o->b = 1u; return; } } }
static void m_bank_operand(KrisalisNative *k) { KrisalisOp *o = &k->queue[(k->queue_head + k->queue_count - 1u) % KRISALIS_QUEUE_MAX]; o->kind = 3u; o->a = 0u; o->b = 0u; o->c = 7u; }
static void m_ram_operand(KrisalisNative *k) { KrisalisOp *o = &k->queue[(k->queue_head + k->queue_count - 1u) % KRISALIS_QUEUE_MAX]; o->kind = 2u; o->a = 0u; o->b = 0xFFu; o->c = 1u; }   /* offset $FF00: outside the 8 KiB RAM */
static void m_gap_overflow(KrisalisNative *k) { k->queue_base = UINT64_MAX - 10u; k->queue[(k->queue_head + 1u) % KRISALIS_QUEUE_MAX].dt = 0xFFFFFu; }
static void m_gap_huge(KrisalisNative *k) { k->queue[(k->queue_head + 1u) % KRISALIS_QUEUE_MAX].dt = 0xFFFFFFFFu; }
static void m_head_gap(KrisalisNative *k) { k->queue[k->queue_head].dt = 5u; }
static void m_idle_ahead(KrisalisNative *k) { k->idle_since = UINT64_MAX - 1u; }
static void m_held_future(KrisalisNative *k) { k->held_since = UINT64_MAX - 3u; }
static void m_idle_phase(KrisalisNative *k) { k->idle_phase = 100000u; }
static void m_pair_open_empty(KrisalisNative *k) { k->queue_count = 0u; k->pair_open = 1u; }
static void m_pair_unexpected(KrisalisNative *k) { k->pair_open = (uint8_t)(k->pair_open ? 0u : 1u); }
static void m_data_without_address(KrisalisNative *k) { size_t i; for (i = 1; i < k->queue_count; ++i) { KrisalisOp *o = &k->queue[(k->queue_head + i) % KRISALIS_QUEUE_MAX]; if (o->kind == 1u && (o->a & 1u) == 0u) { o->kind = 1u; o->a = 1u; return; } } }
static void m_open_pair_at_end(KrisalisNative *k) { KrisalisOp *o = &k->queue[(k->queue_head + k->queue_count - 1u) % KRISALIS_QUEUE_MAX]; o->kind = 1u; o->a = 0u; o->b = 0u; o->c = 0u; }
static void m_counters(KrisalisNative *k) { k->count_rows = k->count_commands + 5u; }
static void m_not_started(KrisalisNative *k) { k->started = 0u; }
static void m_floor_future(KrisalisNative *k) { k->floor_ticks = UINT64_MAX / 2u; }

static void test_corrupt_states(void) {
  static const struct { const char *name; Mutation mutate; } cases[] = {
      {"another supported revision", m_wrong_revision}, {"an unknown revision", m_unknown_revision}, {"revision zero", m_zero_revision},
      {"revision 3, active DAC, rate 0, finite due time", m_rate_zero_finite_due}, {"an active sample with nothing left", m_active_no_left}, {"an active sample that was never started", m_active_no_start},
      {"an active sample with no scheduled poll", m_active_no_due}, {"a poll due at the end of time", m_due_far_future}, {"an invalid operation kind", m_op_kind}, {"a YM port above 3", m_ym_port},
      {"a YM operation with a stray operand", m_ym_operand_b}, {"a bank-register bit above 1", m_bank_operand}, {"a sound-RAM write outside the RAM", m_ram_operand}, {"a queue timestamp overflow", m_gap_overflow},
      {"an impossible gap between two effects", m_gap_huge}, {"a gap in front of the head", m_head_gap}, {"an idle time at the end of time", m_idle_ahead}, {"a bus hold that starts in the future", m_held_future},
      {"an impossible wait-loop phase", m_idle_phase}, {"an open register pair with an empty queue", m_pair_open_empty}, {"an inconsistent open-pair flag", m_pair_unexpected},
      {"a data write whose address write is not in the queue", m_data_without_address}, {"a queue that ends inside a register pair", m_open_pair_at_end}, {"counters no run can produce", m_counters},
      {"a driver that never started", m_not_started}, {"a floor time in the future", m_floor_future}};
  Fixture *live = fx_new(3u), *twin = fx_new(3u), *source = fx_new(3u);
  uint8_t payload[KRISALIS_NATIVE_STATE_MAX_BYTES], good[KRISALIS_NATIVE_STATE_MAX_BYTES], before[KRISALIS_NATIVE_STATE_MAX_BYTES], after[KRISALIS_NATIVE_STATE_MAX_BYTES];
  uint32_t length, good_length, before_length, after_length;
  const uint64_t now = MS(20) + 300u * 15u * 8u;
  size_t c, accepted = 0u, tested = 0u;
  prepare_busy(live); prepare_busy(twin); prepare_busy(source);
  CHECK(live->driver.queue_count > 4u && live->driver.dac_active && live->driver.pair_open <= 1u, "the fixture is mid-row with an active sample (%u effects pending)", live->driver.queue_count);
  good_length = krisalis_native_state_put(&source->driver, good, sizeof good);
  CHECK(good_length > 0u, "the source state serialises");
  {   /* the unmutated payload is accepted (so every rejection below is about its mutation) */
    KrisalisNative stage = live->driver;
    CHECK(krisalis_native_state_get(&stage, good, good_length, now), "the unmutated state is accepted");
  }
  for (c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
    KrisalisNative mutated = source->driver, stage = live->driver;
    const size_t writes = live->count;
    uint8_t ram[8192];
    memcpy(ram, live->ram, sizeof ram);
    before_length = krisalis_native_state_put(&live->driver, before, sizeof before);
    cases[c].mutate(&mutated);
    length = krisalis_native_state_put(&mutated, payload, sizeof payload);
    if (length == 0u) continue;   /* a mutation the encoder itself refuses (not an accepted state either) */
    ++tested;
    if (krisalis_native_state_get(&stage, payload, length, now)) { ++accepted; CHECK(0, "REJECTED? no: accepted a corrupt state: %s", cases[c].name); }
    CHECK(memcmp(&stage, &live->driver, sizeof stage) == 0, "a refused state leaves the staged driver unchanged: %s", cases[c].name);
    after_length = krisalis_native_state_put(&live->driver, after, sizeof after);
    CHECK(after_length == before_length && memcmp(before, after, before_length) == 0 && memcmp(ram, live->ram, sizeof ram) == 0, "the live driver and its RAM are unchanged: %s", cases[c].name);
    CHECK(live->count == writes, "no chip write: %s", cases[c].name);
  }
  CHECK(accepted == 0u && tested == sizeof cases / sizeof cases[0], "every mutation was encoded and refused (%zu of %zu)", tested, sizeof cases / sizeof cases[0]);
  /* a wrong revision for the bound driver, and a payload for another revision into a started revision-1 driver */
  {
    Fixture *r1 = fx_new(1u);
    KrisalisNative stage = r1->driver;
    run_to(r1, MS(5));
    stage = r1->driver;
    CHECK(!krisalis_native_state_get(&stage, good, good_length, now), "a revision-3 payload is refused by a bound revision-1 driver");
    CHECK(memcmp(&stage, &r1->driver, sizeof stage) == 0, "...and nothing was staged");
    free(r1);
  }
  /* truncations and extensions of the valid payload */
  for (c = 0; c < good_length; c += (c < 200u ? 1u : 61u)) {
    KrisalisNative stage = live->driver;
    CHECK(!krisalis_native_state_get(&stage, good, (uint32_t)c, now), "a payload truncated to %zu bytes is refused", c);
  }
  { KrisalisNative stage = live->driver; memcpy(payload, good, good_length); payload[good_length] = 0u;
    CHECK(!krisalis_native_state_get(&stage, payload, good_length + 1u, now), "a payload with a trailing byte is refused"); }
  /* after all of it, the live driver and an untouched twin continue identically (and the unmutated state is still accepted) */
  {
    size_t base = live->count, tbase = twin->count, i;
    run_to(live, MS(60)); run_to(twin, MS(60));
    CHECK(live->count - base == twin->count - tbase && live->count - base > 0u, "the continuation of the refused-into driver has the twin's length");
    for (i = 0; i < live->count - base && i < twin->count - tbase; ++i)
      CHECK(live->log[base + i].port == twin->log[tbase + i].port && live->log[base + i].value == twin->log[tbase + i].value && live->log[base + i].ticks == twin->log[tbase + i].ticks, "continuation write %zu differs", i);
  }
  free(live); free(twin); free(source);
}

/* a fail-closed guard: a driver whose DAC state was damaged in memory (not through a snapshot) faults instead of dividing by zero */
static void test_zero_rate_guard(void) {
  Fixture *f = fx_new(3u);
  prepare_busy(f);
  f->driver.dac_rate = 0u;
  f->driver.dac_due = f->driver.floor_ticks;
  (void)krisalis_native_run_to(&f->driver, MS(300));
  CHECK(f->driver.fault == KRISALIS_FAULT_INVALID_STATE, "an impossible DAC rate faults the driver (%s)", krisalis_native_fault_name(f->driver.fault));
  free(f);
}

static void test_catalog(void) {
  uint8_t unknown[32] = {1, 2, 3};
  const char *version = NULL;
  uint8_t revision = 0;
  CHECK(!krisalis_native_identity_lookup(unknown, &version, &revision), "an unknown signature is not verified");
  CHECK(revision == 0u, "no revision for an unknown image");
}

int main(void) {
  g_test = "test_start"; test_start();
  g_test = "test_ack_protocol"; test_ack_protocol();
  g_test = "test_note_with_voice"; test_note_with_voice(3u);
  test_note_with_voice(1u);
  g_test = "test_channel_mapping"; test_channel_mapping();
  g_test = "test_volume_attenuation"; test_volume_attenuation();
  g_test = "test_dac_sample"; test_dac_sample();
  g_test = "test_faults_and_hold"; test_faults_and_hold();
  g_test = "test_cell_commands_rev3"; test_cell_commands_rev3();
  g_test = "test_revision_2"; test_revision_2();
  g_test = "test_state"; test_state();
  g_test = "test_corrupt_states"; test_corrupt_states();
  g_test = "test_zero_rate_guard"; test_zero_rate_guard();
  g_test = "test_catalog"; test_catalog();
  if (g_failures != 0) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
  puts("krisalis_native_tests: ok");
  return 0;
}
