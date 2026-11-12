/*
 * Unit tests of the native GEMS replacement core (ADR 0291) against project-authored synthetic cartridge data: no commercial data and no original
 * driver code is involved. Expected chip accesses are derived by hand from the published behaviour of the verified driver lineage (command FIFO,
 * sequence encoding, digital voice, Timer A paced DAC with a two-half FIFO, idle FM/PSG clipping).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gems_native.h"
#include "runtime.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct Access { char kind; uint32_t port; uint8_t value; uint64_t ticks; } Access;

typedef struct Fixture {
  uint8_t rom[0x10000];
  uint8_t ram[8192];
  Access log[200000];
  size_t count;
  /* a minimal free-running Timer A: the device side of the Timer-A-paced DAC */
  uint8_t latched;
  uint16_t timer_a;
  uint64_t expiry;   /* next overflow, 0 = stopped */
  int flag;
  GemsNative driver;
} Fixture;

static uint64_t fx_period(const Fixture *f) { return (uint64_t)(1024u - f->timer_a) * YM2612_MASTER_TICKS_PER_SAMPLE; }

static void fx_advance(Fixture *f, uint64_t ticks) {   /* overflows up to `ticks` set the flag and re-arm the timer (it free-runs) */
  while (f->expiry != 0u && ticks >= f->expiry) { f->flag = 1; f->expiry += fx_period(f); }
}

static void fx_ym_write(void *context, uint32_t port, uint8_t value, uint64_t ticks) {
  Fixture *f = (Fixture *)context;
  fx_advance(f, ticks);
  f->log[f->count].kind = 'Y'; f->log[f->count].port = port; f->log[f->count].value = value; f->log[f->count++].ticks = ticks;
  if ((port & 1u) == 0u) { if (port == 0u) f->latched = value; return; }
  if (port != 1u) return;
  if (f->latched == 0x24) f->timer_a = (uint16_t)((f->timer_a & 3u) | ((unsigned)value << 2));
  else if (f->latched == 0x25) f->timer_a = (uint16_t)((f->timer_a & ~3u) | (value & 3u));
  else if (f->latched == 0x27) {
    if (value & 0x10u) f->flag = 0;
    if ((value & 1u) == 0u) f->expiry = 0u;
    else if (f->expiry == 0u) f->expiry = ticks + fx_period(f);   /* the load bit going 0 -> 1 starts the count; a write while it runs does not restart it */
  }
}
static uint8_t fx_ym_read(void *context, uint32_t port, uint64_t ticks) {
  Fixture *f = (Fixture *)context;
  (void)port;
  fx_advance(f, ticks);
  return (uint8_t)f->flag;
}
static uint64_t fx_timer_a_expiry(void *context) { const Fixture *f = (const Fixture *)context; return f->expiry == 0u ? UINT64_MAX : f->expiry; }
static void fx_psg_write(void *context, uint8_t value, uint64_t ticks) {
  Fixture *f = (Fixture *)context;
  f->log[f->count].kind = 'P'; f->log[f->count].port = 0; f->log[f->count].value = value; f->log[f->count++].ticks = ticks;
}
static int fx_rom_read(void *context, uint32_t address, uint8_t *destination, uint32_t count) {
  Fixture *f = (Fixture *)context;
  if (address + count > sizeof f->rom) return 0;
  memcpy(destination, f->rom + address, count);
  return 1;
}

enum { PATCH_TABLE = 0x1000, SEQ_TABLE = 0x2000, SAMPLE_TABLE = 0x3000, SAMPLE_DATA = 0x4000, SAMPLE_LENGTH = 3000 };

static void le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Synthetic resources: patches {0: digital, 1: FM}; sequences {0: one digital note, 1: one FM note, 2: an unsupported event, 3: empty};
 * sample 0 (note 48): 3000 bytes, rate 5. */
static void build_cartridge(Fixture *f) {
  uint8_t *p;
  unsigned i;
  memset(f->rom, 0, sizeof f->rom);
  for (i = 0; i < 4; ++i) le16(f->rom + PATCH_TABLE + 2 * i, 8 + 39 * i);
  f->rom[PATCH_TABLE + 8] = 1; f->rom[PATCH_TABLE + 9] = 4;          /* patch 0: digital, no rate override */
  f->rom[PATCH_TABLE + 8 + 39] = 0;                                  /* patch 1: FM (all registers zero) */
  {  /* patch 2: PSG tone: attack 0x10, sustain level 2, attack level 0, decay 0x10, release 0x10 */
    uint8_t *tone = f->rom + PATCH_TABLE + 8 + 78;
    tone[0] = 2; tone[2] = 0x10; tone[3] = 2; tone[4] = 0; tone[5] = 0x10; tone[6] = 0x10;
  }
  /* sequence table: offsets of the sequence headers */
  for (i = 0; i < 6; ++i) le16(f->rom + SEQ_TABLE + 2 * i, 16 + 3 * i);
  p = f->rom + SEQ_TABLE + 16;
  p[0] = 1; le16(p + 1, 0x40);                                       /* seq 0: one track at +0x40 */
  p[3] = 1; le16(p + 4, 0x60);                                       /* seq 1 */
  p[6] = 1; le16(p + 7, 0xDFF0);                                       /* seq 2 */
  p[9] = 0;                                                          /* seq 3: empty */
  p[12] = 1; le16(p + 13, 0xA0);                                     /* seq 4: one PSG note */
  p[15] = 1; le16(p + 16, 0xC0);                                     /* seq 5: a PSG note in a two-pass loop */
  {
    static const uint8_t t0[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x00, 0xC2, 0x30, 0x60};   /* delay 0, prio 100, SFX base, patch 0, delay 2, note 48, eos */
    static const uint8_t t1[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x01, 0xC2, 0x30, 0x60};   /* the same with the FM patch */
    static const uint8_t t2[] = {0xC0, 0x64, 3};                                           /* a loop start, at the very end of the cartridge */
    memcpy(f->rom + SEQ_TABLE + 0x40, t0, sizeof t0);
    memcpy(f->rom + SEQ_TABLE + 0x60, t1, sizeof t1);
    memcpy(f->rom + SEQ_TABLE + 0xDFF0, t2, sizeof t2);
    {
      static const uint8_t t4[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x02, 0xC1, 0x3C, 0x60};                          /* PSG patch, note 60, eos */
      static const uint8_t t5[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x02, 0x64, 0x02, 0xC1, 0x3C, 0x65, 0x60};      /* loop twice: note 60, delay 1 */
      memcpy(f->rom + SEQ_TABLE + 0xA0, t4, sizeof t4);
      memcpy(f->rom + SEQ_TABLE + 0xC0, t5, sizeof t5);
    }
  }
  /* sample table: 12-byte headers; sample 0: rate 5, data at +0x1000 from the table, 3000 bytes */
  f->rom[SAMPLE_TABLE + 0] = 0x05;
  f->rom[SAMPLE_TABLE + 1] = (uint8_t)((SAMPLE_DATA - SAMPLE_TABLE) & 0xFF); f->rom[SAMPLE_TABLE + 2] = (uint8_t)((SAMPLE_DATA - SAMPLE_TABLE) >> 8);
  le16(f->rom + SAMPLE_TABLE + 6, SAMPLE_LENGTH);
  for (i = 0; i < SAMPLE_LENGTH + 256; ++i) f->rom[SAMPLE_DATA + i] = (uint8_t)(i * 7u + 3u);
}

static void fx_start(Fixture *f, uint64_t at) {
  GemsNativeHost host;
  memset(&host, 0, sizeof host);
  host.context = f; host.ram = f->ram; host.rom_read = fx_rom_read; host.ym_write = fx_ym_write; host.ym_read = fx_ym_read; host.ym_timer_a_expiry = fx_timer_a_expiry; host.psg_write = fx_psg_write;
  f->count = 0; f->expiry = 0; f->flag = 0; f->timer_a = 0;
  gems_native_start(&f->driver, &host, at);
}

static void fifo_put(Fixture *f, const uint8_t *bytes, unsigned n) {
  unsigned i;
  uint8_t w = f->ram[0x36];
  for (i = 0; i < n; ++i) { f->ram[0x1B40 + w] = bytes[i]; w = (uint8_t)((w + 1u) & 0x3F); }
  f->ram[0x36] = w;
}

static void send_pointers(Fixture *f) {
  /* start byte, command 11, then four little-endian 24-bit pointers: patches, envelopes (unused), sequences, samples */
  const uint8_t c[14] = {0xFF, 11, PATCH_TABLE & 0xFF, PATCH_TABLE >> 8, 0, 0, 0, 0, SEQ_TABLE & 0xFF, SEQ_TABLE >> 8, 0, SAMPLE_TABLE & 0xFF, SAMPLE_TABLE >> 8, 0};
  fifo_put(f, c, sizeof c);
}

static void send_start(Fixture *f, uint8_t seq) { const uint8_t c[] = {0xFF, 16, seq}; fifo_put(f, c, 3); }
static void send_stop(Fixture *f, uint8_t seq) { const uint8_t c[] = {0xFF, 18, seq}; fifo_put(f, c, 3); }

static size_t count_if(const Fixture *f, char kind, uint32_t port, int has_value, uint8_t value) {
  size_t i, n = 0;
  for (i = 0; i < f->count; ++i)
    if (f->log[i].kind == kind && f->log[i].port == port && (!has_value || f->log[i].value == value)) ++n;
  return n;
}

static const uint64_t FRAME = GENESIS_NTSC_MASTER_TICKS_PER_FRAME;

static void test_identity(void) {
  const char *version = NULL;
  uint8_t sig[32];
  size_t i;
  const char *hex = "f68e547d7dec54d2e00a4f292c1986a07d6bc8de14cc417e5e376b93a9f86cf7";
  for (i = 0; i < 32; ++i) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); sig[i] = (uint8_t)v; }
  CHECK(gems_native_identity_lookup(sig, &version) == 1 && version != NULL && strcmp(version, "2.5") == 0, "the verified image is found with its version");
  sig[5] ^= 1u;
  CHECK(gems_native_identity_lookup(sig, &version) == 0, "a changed signature is not a verified image");
  memset(sig, 0, 32);
  CHECK(gems_native_identity_lookup(sig, NULL) == 0, "the zero signature is not a verified image");
}

static void test_boot_and_digital_sequence(Fixture *f) {
  size_t i, dac = 0, seen_setup = 0;
  uint8_t expected_setup[10] = {0x24, 0xFE, 0x25, 0x03, 0x2B, 0x80, 0x27, 0x15, 0xB6, 0xC0};
  build_cartridge(f);
  memset(f->ram, 0, sizeof f->ram);
  fx_start(f, 1000);
  send_pointers(f);
  send_start(f, 0);
  CHECK(gems_native_run_to(&f->driver, 40 * FRAME) == GEMS_FAULT_NONE, "a verified digital sequence runs without a fault (%s)", gems_native_fault_name(f->driver.fault));
  CHECK(f->log[0].kind == 'P' && f->log[0].value == 0x9F && f->log[3].value == 0xFF && f->log[0].ticks == f->log[3].ticks, "boot silences the four PSG voices at one instant");
  CHECK(f->ram[0x37] == f->ram[0x36], "the driver consumed the whole command FIFO");
  CHECK(f->driver.count_commands == 2 && f->driver.count_sequences == 1, "two commands, one sequence");
  for (i = 0; i < f->count; ++i) {  /* the note-on setup: Timer A for rate 5 (1019), DAC on, timer load, channel-6 output */
    if (f->log[i].kind == 'Y' && f->log[i].port == 0 && f->log[i].value == 0x24) {
      unsigned k;
      uint8_t got[10];
      for (k = 0; k < 10; ++k) got[k] = f->log[i + k].value;
      CHECK(memcmp(got, expected_setup, 10) == 0, "note-on programs Timer A, DAC enable, timer load and channel-6 pan in the verified order");
      CHECK(f->log[i + 8].port == 2 && f->log[i + 9].port == 3, "channel-6 pan goes to part II");
      seen_setup = 1;
      break;
    }
  }
  CHECK(seen_setup, "the note-on setup was written");
  for (i = 0; i + 1 < f->count; ++i)
    if (f->log[i].kind == 'Y' && f->log[i].port == 0 && f->log[i].value == 0x2A && f->log[i + 1].port == 1) {
      CHECK(f->log[i + 1].value == (uint8_t)(dac * 7u + 3u), "DAC byte %zu equals the cartridge sample byte", dac);
      ++dac;
    }
  /* 3000 samples in 128-byte fills: fill k (k >= 1) happens when the reader has consumed 128 x (k - 1) samples; the fill number 23 finds only 56 left,
   * loads them and shuts the DAC down at once, so 128 x 22 = 2816 samples are played - the tail cut of the original driver, reproduced. */
  CHECK(dac == 2816, "2816 samples are played before the original driver's tail cut (got %zu)", dac);
  CHECK(count_if(f, 'Y', 1, 1, 0x00) >= 1 && f->log[f->count - 1].port == 1 && f->log[f->count - 1].value == 0x00, "the DAC is disabled at the end");
  CHECK(count_if(f, 'Y', 0, 1, 0x2B) >= 2, "DAC enable then disable");
}

/* Notes below 48 select samples 48..95 (the original subtracts 48 and adds 96 on a borrow): note 15 is sample 63, not sample 111. */
static void test_low_note_sample_mapping(Fixture *f) {
  uint8_t *header;
  unsigned i;
  size_t dac = 0, k;
  build_cartridge(f);
  f->rom[SEQ_TABLE + 0x40 + 7] = 0x0F;                                 /* the note of sequence 0's track */
  header = f->rom + SAMPLE_TABLE + 12 * 63;
  header[0] = 0x06;                                                    /* rate 6 */
  header[1] = (uint8_t)((SAMPLE_DATA + 0x800 - SAMPLE_TABLE) & 0xFF); header[2] = (uint8_t)((SAMPLE_DATA + 0x800 - SAMPLE_TABLE) >> 8);
  le16(header + 6, 1000);
  for (i = 0; i < 1100; ++i) f->rom[SAMPLE_DATA + 0x800 + i] = (uint8_t)(i * 5u + 1u);
  /* a sample 111 (the wrong mapping) is left as zeros: a driver that used it would fault on an empty or unsupported header */
  memset(f->ram, 0, sizeof f->ram);
  fx_start(f, 1000);
  send_pointers(f); send_start(f, 0);
  CHECK(gems_native_run_to(&f->driver, 40 * FRAME) == GEMS_FAULT_NONE, "a low note plays (%s)", gems_native_fault_name(f->driver.fault));
  for (k = 0; k + 1 < f->count; ++k)
    if (f->log[k].kind == 'Y' && f->log[k].port == 0 && f->log[k].value == 0x2A) {
      CHECK(f->log[k + 1].value == (uint8_t)(dac * 5u + 1u), "DAC byte %zu comes from sample 63", dac);
      ++dac;
    }
  CHECK(dac == 768, "1000 samples in 128-byte fills: fill 7 finds 104 left and cuts the tail after 768 played (got %zu)", dac);
}

static void test_cadence_invariance(Fixture *a, Fixture *b) {
  uint64_t t;
  size_t i;
  build_cartridge(a); build_cartridge(b);
  memset(a->ram, 0, sizeof a->ram); memset(b->ram, 0, sizeof b->ram);
  fx_start(a, 1000); fx_start(b, 1000);
  send_pointers(a); send_pointers(b); send_start(a, 0); send_start(b, 0);
  gems_native_run_to(&a->driver, 40 * FRAME);
  for (t = 0; t < 40 * FRAME; t += 777) gems_native_run_to(&b->driver, t);
  gems_native_run_to(&b->driver, 40 * FRAME);
  CHECK(a->count == b->count, "the number of chip accesses does not depend on the run cadence (%zu vs %zu)", a->count, b->count);
  for (i = 0; i < a->count && i < b->count; ++i)
    if (memcmp(&a->log[i], &b->log[i], sizeof(Access)) != 0) { CHECK(0, "access %zu differs between cadences", i); break; }
}

static void test_stop_cuts_sample(Fixture *f) {
  size_t before;
  build_cartridge(f);
  memset(f->ram, 0, sizeof f->ram);
  fx_start(f, 1000);
  send_pointers(f); send_start(f, 0);
  gems_native_run_to(&f->driver, 8 * FRAME);
  CHECK(f->driver.dac_active == 1, "the sample is playing");
  send_stop(f, 0);
  gems_native_resume(&f->driver, 8 * FRAME + 5000);
  gems_native_run_to(&f->driver, 9 * FRAME);
  CHECK(f->driver.dac_active == 0 && f->driver.fault == GEMS_FAULT_NONE, "stop sequence silences the digital voice");
  before = f->count;
  gems_native_run_to(&f->driver, 12 * FRAME);
  CHECK(count_if(f, 'Y', 1, 0, 0) > 0 && f->count >= before, "no sample is written after the stop");
  {
    size_t i, last_dac = 0, stop_at = 0;
    for (i = 0; i < f->count; ++i) {
      if (f->log[i].kind == 'Y' && f->log[i].port == 0 && f->log[i].value == 0x2A) last_dac = i;
      if (f->log[i].kind == 'Y' && f->log[i].port == 0 && f->log[i].value == 0x28) { stop_at = i; break; }
    }
    CHECK(stop_at != 0 && last_dac < stop_at + 10, "the clipping of every FM voice (key off) accompanies the stop");
  }
}

static void test_faults(Fixture *f) {
  size_t after;
  /* an unknown command id is ignored exactly as the original ignores it: consumed, no fault, nothing written */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t unknown[] = {0xFF, 99}; fifo_put(f, unknown, 2); }
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_NONE && f->ram[0x37] == f->ram[0x36] && f->count == 4, "an unknown command id is ignored and consumed");
  /* a start byte with the command byte still missing waits; non-start bytes are discarded */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t garbage[] = {0x12, 0x34, 0xFF}; fifo_put(f, garbage, 3); }
  gems_native_run_to(&f->driver, 3 * FRAME);
  CHECK(f->ram[0x37] == 2 && f->driver.fault == GEMS_FAULT_NONE, "stray bytes are discarded, an incomplete command waits (read pointer %u)", f->ram[0x37]);
  /* a sequence that runs off the cartridge: explicit fault, nothing further is written */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 2);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_UNMAPPED_RESOURCE, "a sequence that runs off the cartridge faults (%s)", gems_native_fault_name(f->driver.fault));
  after = f->count;
  gems_native_run_to(&f->driver, 20 * FRAME);
  CHECK(f->count == after, "a faulted driver writes nothing further");
  /* unmapped resource: pointers into the void */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t c[] = {0xFF, 11, 0, 0, 0, 0, 0, 0, 0xFE, 0xFF, 0, 0, 0, 0}; fifo_put(f, c, 14); }   /* sequence table at $FFFE: its header is not inside the cartridge */
  send_start(f, 0);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_UNMAPPED_RESOURCE, "a resource outside the cartridge faults");
  /* invalid resource: more than 16 tracks */
  build_cartridge(f); f->rom[SEQ_TABLE + 16 + 9] = 17; memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 3);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_INVALID_RESOURCE, "a track count above 16 is an invalid resource");
  /* invalid FIFO pointer, invalid channel */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  f->ram[0x36] = 0x41;
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_INVALID_COMMAND_STREAM, "a write pointer outside the ring is rejected");
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t c[] = {0xFF, 28, 200}; fifo_put(f, c, 3); }
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_INVALID_COMMAND_STREAM, "a control-block number above 15 is rejected");
  /* an empty sequence is accepted and starts nothing */
  build_cartridge(f); f->rom[SEQ_TABLE + 16 + 9] = 0; memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 3);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_NONE && f->driver.count_sequences == 0, "an empty sequence starts nothing");
}

static int find_write(const Fixture *f, size_t from, char kind, uint32_t port, uint8_t value) {
  size_t i;
  for (i = from; i < f->count; ++i) if (f->log[i].kind == kind && f->log[i].port == port && f->log[i].value == value) return (int)i;
  return -1;
}

/* FM, PSG, loops, tempo, mailboxes: behaviours that need no commercial data */
static void test_fm_psg_and_sequencer_features(Fixture *f) {
  int at;
  /* an FM note: the whole register map is written, the frequency from the table, then key on with the voice number */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 1);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_NONE && f->driver.count_notes_fm == 1, "an FM note plays (%s)", gems_native_fault_name(f->driver.fault));
  /* note 48 = C4: octave 4 (block bits 4 << 3 = 0x20), table entry 0 = 644 = 0x284 -> 0xA4 = 0x22 | ..., 0xA0 = 0x84 */
  at = find_write(f, 0, 'Y', 1, 0x2C);   /* 0x284 | (4 << 11) = 0x2284 -> msb 0x22? computed below */
  (void)at;
  at = find_write(f, 0, 'Y', 0, 0xA4);
  CHECK(at >= 0 && f->log[at + 1].value == (uint8_t)(((644u | (4u << 11)) >> 8) & 0xFF), "FM frequency MSB: block 4 and the table's F-number");
  at = find_write(f, (size_t)at, 'Y', 0, 0xA0);
  CHECK(at >= 0 && f->log[at + 1].value == (uint8_t)(644u & 0xFF), "FM frequency LSB");
  at = find_write(f, 0, 'Y', 0, 0x28);
  CHECK(at >= 0 && f->log[at + 1].value == 0x00, "key on of voice 0 with the patch's operator mask");
  /* a PSG tone note: at the next tick the tone is written (low nibble | 0x80, then the high part) followed by the level */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 4);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_NONE && f->driver.count_notes_psg == 1, "a PSG tone note plays");
  at = find_write(f, 4, 'P', 0, 0x86);
  CHECK(at >= 0 && f->log[at + 1].kind == 'P' && f->log[at + 1].value == 0x0D, "PSG tone: note 60 is wavelength $0D6");
  /* loops: a two-pass loop plays its body twice, the second note on the next free voice */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 5);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_NONE && f->driver.count_notes_psg == 2, "a loop of count 2 plays two notes (got %llu)", (unsigned long long)f->driver.count_notes_psg);
  CHECK(find_write(f, 4, 'P', 0, 0xA6) >= 0, "the second pass uses the next tone voice");
  /* tempo command and mailbox store */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t tempo[] = {0xFF, 5, 90}; const uint8_t store[] = {0xFF, 27, 3, 0x5A}; fifo_put(f, tempo, 3); fifo_put(f, store, 4); }
  gems_native_run_to(&f->driver, 3 * FRAME);
  CHECK(f->driver.sbpt == (90u * 218u) >> 7, "tempo 90 sets the sub-beat increment (%u)", f->driver.sbpt);
  CHECK(f->ram[0x1B20 + 2 + 3] == 0x5A, "a mailbox store reaches the RAM the 68000 reads");
  /* a fast tempo carries more than one sub-beat per tick; every beat is processed, none piles up in the accumulator */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t fast[] = {0xFF, 5, 255}; fifo_put(f, fast, 3); }
  gems_native_run_to(&f->driver, 200 * FRAME);
  CHECK(f->driver.sbpt == (255u * 218u) >> 7 && f->driver.sbpt > 256u && f->driver.sbptacc < 0x100u, "at a tempo above one beat per tick the accumulator is drained (sbpt %u, left %u)", f->driver.sbpt, f->driver.sbptacc);
  /* a looped sample keeps playing past its first part */
  build_cartridge(f); f->rom[SAMPLE_TABLE] = 0x15; le16(f->rom + SAMPLE_TABLE + 8, 100); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 0);
  gems_native_run_to(&f->driver, 60 * FRAME);
  CHECK(f->driver.fault == GEMS_FAULT_NONE && f->driver.dac_active == 1 && f->driver.count_dac_samples > 2 * SAMPLE_LENGTH, "a looped sample plays on past its first part (%llu)", (unsigned long long)f->driver.count_dac_samples);
}

static void test_restart_and_bus_hold(Fixture *f) {
  uint64_t floor_time;
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram);
  fx_start(f, 1000);
  send_pointers(f); send_start(f, 0);
  gems_native_run_to(&f->driver, 6 * FRAME);
  CHECK(f->driver.dac_active == 1, "playing before the restart");
  fx_start(f, 6 * FRAME);   /* /RESET restart: all driver state is gone, the chip is reset by the machine */
  CHECK(f->driver.dac_active == 0 && f->driver.have_pointers == 0 && f->driver.ccb[0].flags == 0, "a restart clears the driver state");
  gems_native_run_to(&f->driver, 7 * FRAME);
  CHECK(f->log[0].kind == 'P' && f->count == 4, "the restarted driver boots again and writes only its PSG silence");
  /* bus hold: the VBlank interrupt edges that fall inside a long hold are lost, work due earlier happens when the bus returns */
  floor_time = 60 * FRAME;
  gems_native_resume(&f->driver, floor_time);
  gems_native_run_to(&f->driver, floor_time + FRAME);
  CHECK(f->driver.count_missed_ticks >= 10, "interrupts during a bus hold are lost like the real Z80's (%llu)", (unsigned long long)f->driver.count_missed_ticks);
}

/* ---- save states (ADR 0291): a driver restored from its canonical payload continues exactly like the uninterrupted one ---- */
typedef struct Snapshot {
  uint8_t payload[GEMS_NATIVE_STATE_MAX_BYTES];
  uint32_t length;
  uint8_t ram[8192];
  uint8_t latched;
  uint16_t timer_a;
  uint64_t expiry;
  int flag;
  uint64_t at;
  size_t count;
} Snapshot;

static void fx_bind(Fixture *f, GemsNativeHost *host) {
  memset(host, 0, sizeof *host);
  host->context = f; host->ram = f->ram; host->rom_read = fx_rom_read; host->ym_write = fx_ym_write; host->ym_read = fx_ym_read; host->ym_timer_a_expiry = fx_timer_a_expiry; host->psg_write = fx_psg_write;
}

static int snapshot_take(const Fixture *f, uint64_t at, Snapshot *s) {
  s->length = gems_native_state_put(&f->driver, s->payload, sizeof s->payload);
  memcpy(s->ram, f->ram, sizeof s->ram);
  s->latched = f->latched; s->timer_a = f->timer_a; s->expiry = f->expiry; s->flag = f->flag; s->at = at; s->count = f->count;
  return s->length != 0u;
}

/* The machine's order: stage privately (validated against the clock and the chip's schedule), then commit. Returns 0 for a refused payload and leaves `f` untouched. */
static int snapshot_restore(Fixture *f, const Snapshot *s, const uint8_t *payload, uint32_t length, uint64_t at) {
  GemsNative *stage = (GemsNative *)malloc(sizeof *stage);
  GemsNativeHost host;
  if (stage == NULL) return 0;
  *stage = f->driver;
  if (!gems_native_state_get(stage, payload, length, at, s->expiry == 0u ? UINT64_MAX : s->expiry)) { free(stage); return 0; }
  f->driver = *stage;
  free(stage);
  fx_bind(f, &host);
  f->driver.host = host;
  memcpy(f->ram, s->ram, sizeof f->ram);
  f->latched = s->latched; f->timer_a = s->timer_a; f->expiry = s->expiry; f->flag = s->flag; f->count = 0;
  return 1;
}

static void run_sliced(Fixture *f, uint64_t from, uint64_t to, uint64_t slice) {
  uint64_t t;
  for (t = from + slice; t < to; t += slice) gems_native_run_to(&f->driver, t);
  gems_native_run_to(&f->driver, to);
}

static int same_accesses(const Fixture *ref, size_t from, const Fixture *got) {
  size_t i;
  if (ref->count - from != got->count) return 0;
  for (i = 0; i < got->count; ++i) {
    const Access *x = &ref->log[from + i], *y = &got->log[i];
    if (x->kind != y->kind || x->port != y->port || x->value != y->value || x->ticks != y->ticks) return 0;
  }
  return 1;
}

static int same_end_state(const Fixture *a, const Fixture *b) {
  uint8_t pa[GEMS_NATIVE_STATE_MAX_BYTES], pb[GEMS_NATIVE_STATE_MAX_BYTES];
  const uint32_t la = gems_native_state_put(&a->driver, pa, sizeof pa), lb = gems_native_state_put(&b->driver, pb, sizeof pb);
  return la == lb && la != 0u && memcmp(pa, pb, la) == 0 && memcmp(a->ram, b->ram, sizeof a->ram) == 0 && a->latched == b->latched && a->timer_a == b->timer_a &&
         a->expiry == b->expiry && a->flag == b->flag;
}

typedef enum Tail { TAIL_PLAIN, TAIL_PENDING_COMMAND, TAIL_BUS_HOLD, TAIL_RESET_REACTIVATION } Tail;

/* One scenario: play to `snap_at`, snapshot, finish uninterrupted (the reference), then restore twice into other fixtures (different slicing; a rewind of one of them) and compare. */
static void continuation(const char *name, uint8_t sequence, uint64_t snap_at, uint64_t end_at, Tail tail, void (*tweak)(Fixture *)) {
  Fixture *ref = (Fixture *)calloc(1, sizeof(Fixture)), *one = (Fixture *)calloc(1, sizeof(Fixture));
  Snapshot *snap = (Snapshot *)calloc(1, sizeof(Snapshot));
  size_t from;
  int pass;
  if (ref == NULL || one == NULL || snap == NULL) { ++g_failures; return; }
  build_cartridge(ref); build_cartridge(one);
  if (tweak != NULL) { tweak(ref); tweak(one); }
  memset(ref->ram, 0, sizeof ref->ram);
  fx_start(ref, 1000);
  send_pointers(ref); send_start(ref, sequence);
  gems_native_run_to(&ref->driver, snap_at);
  CHECK(ref->driver.fault == GEMS_FAULT_NONE, "%s: healthy at the snapshot point", name);
  if (tail == TAIL_PENDING_COMMAND) { const uint8_t tempo[] = {0xFF, 5, 120}; fifo_put(ref, tempo, 3); }   /* unconsumed at the snapshot */
  CHECK(snapshot_take(ref, snap_at, snap), "%s: the state serialises", name);
  from = ref->count;
  { /* the uninterrupted continuation */
    if (tail == TAIL_BUS_HOLD) gems_native_resume(&ref->driver, snap_at + 9 * FRAME);
    if (tail == TAIL_RESET_REACTIVATION) { fx_start(ref, snap_at + FRAME); ref->count = from; send_pointers(ref); send_start(ref, sequence); }
    gems_native_run_to(&ref->driver, end_at);
  }
  CHECK(ref->count - from > 20u && ref->driver.fault == GEMS_FAULT_NONE, "%s: the continuation is not trivial (%zu operations)", name, ref->count - from);
  for (pass = 0; pass < 3; ++pass) {   /* pass 2 rewinds a fixture that already ran past the snapshot */
    Fixture *c = one;
    GemsNative live_before;
    if (pass != 2) { memset(c->ram, 0xA5, sizeof c->ram); fx_start(c, 5000); }   /* a different, running driver: the restore replaces everything */
    live_before = c->driver;
    CHECK(snapshot_restore(c, snap, snap->payload, snap->length, snap_at), "%s pass %d: the state restores", name, pass);
    (void)live_before;
    if (tail == TAIL_BUS_HOLD) gems_native_resume(&c->driver, snap_at + 9 * FRAME);
    if (tail == TAIL_RESET_REACTIVATION) { fx_start(c, snap_at + FRAME); c->count = 0; send_pointers(c); send_start(c, sequence); }
    run_sliced(c, snap_at, end_at, pass == 0 ? 777u : 100003u);
    CHECK(same_accesses(ref, from, c), "%s pass %d: identical chip operations after the restore (%zu vs %zu)", name, pass, ref->count - from, c->count);
    CHECK(same_end_state(ref, c), "%s pass %d: identical driver state, RAM, mailboxes and chip timer at the end", name, pass);
  }
  free(ref); free(one); free(snap);
}

static void tweak_looped_sample(Fixture *f) { f->rom[SAMPLE_TABLE] = 0x15; le16(f->rom + SAMPLE_TABLE + 8, 100); }

static void test_state_continuation(void) {
  continuation("mid-sample", 0, 4 * FRAME + 12345, 40 * FRAME, TAIL_PLAIN, NULL);
  continuation("looped sample", 0, 9 * FRAME + 777, 60 * FRAME, TAIL_PLAIN, tweak_looped_sample);
  continuation("FM note", 1, FRAME / 2, 30 * FRAME, TAIL_PLAIN, NULL);
  continuation("PSG loop", 5, 3 * FRAME + 99, 30 * FRAME, TAIL_PLAIN, NULL);
  continuation("pending command", 0, 5 * FRAME + 321, 40 * FRAME, TAIL_PENDING_COMMAND, NULL);
  continuation("bus hold", 0, 6 * FRAME + 4242, 50 * FRAME, TAIL_BUS_HOLD, NULL);
  continuation("reset and reactivation", 0, 7 * FRAME + 11, 60 * FRAME, TAIL_RESET_REACTIVATION, NULL);
}

/* A payload that this implementation could not have produced is refused, whatever the damage, and refusing never touches the live driver. */
static void test_state_refusals(void) {
  Fixture *f = (Fixture *)calloc(1, sizeof(Fixture));
  Snapshot *s = (Snapshot *)calloc(1, sizeof(Snapshot));
  GemsNative *stage = (GemsNative *)malloc(sizeof(GemsNative));
  uint8_t damaged[GEMS_NATIVE_STATE_MAX_BYTES];
  uint32_t i, accepted = 0u;
  const uint64_t at = 4 * FRAME + 12345;
  if (f == NULL || s == NULL || stage == NULL) { ++g_failures; return; }
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 0);
  gems_native_run_to(&f->driver, at);
  CHECK(snapshot_take(f, at, s) && s->length > 0u && s->length <= GEMS_NATIVE_STATE_MAX_BYTES, "a running driver serialises (%u bytes)", s->length);
  { uint8_t again[GEMS_NATIVE_STATE_MAX_BYTES]; CHECK(gems_native_state_put(&f->driver, again, sizeof again) == s->length && memcmp(again, s->payload, s->length) == 0, "serialisation is canonical (identical bytes twice)"); }
  CHECK(gems_native_state_put(&f->driver, damaged, s->length - 1u) == 0u, "a destination that is too small is refused, not truncated");
  *stage = f->driver;
  CHECK(gems_native_state_get(stage, s->payload, s->length, at, s->expiry == 0u ? UINT64_MAX : s->expiry) == 1, "the genuine payload validates");
  CHECK(gems_native_state_get(stage, s->payload, 0u, at, s->expiry) == 0, "an empty payload is refused");
  for (i = 0u; i < s->length; i += 1u) {   /* every truncation */
    *stage = f->driver;
    if (gems_native_state_get(stage, s->payload, i, at, s->expiry == 0u ? UINT64_MAX : s->expiry)) { ++accepted; break; }
  }
  CHECK(accepted == 0u, "no truncated payload is accepted");
  memcpy(damaged, s->payload, s->length); damaged[s->length] = 0;
  *stage = f->driver;
  CHECK(gems_native_state_get(stage, damaged, s->length + 1u, at, s->expiry == 0u ? UINT64_MAX : s->expiry) == 0, "trailing bytes are refused");
  *stage = f->driver;
  CHECK(gems_native_state_get(stage, s->payload, s->length, at + 40u * FRAME, s->expiry == 0u ? UINT64_MAX : s->expiry) == 0, "a payload from another point of the guest clock is refused");
  *stage = f->driver;
  CHECK(gems_native_state_get(stage, s->payload, s->length, at, UINT64_MAX) == 0, "a chip whose Timer A schedule contradicts a pacing DAC is refused");
  *stage = f->driver;
  CHECK(gems_native_state_get(stage, s->payload, s->length, at, 1u) == 0, "a chip whose next overflow is before the DAC's due time is refused");
  for (i = 0u; i < s->length; ++i) {   /* a corrupted byte: refused, or (when it is a legal value) still a state that continues without faulting the process */
    unsigned bit;
    for (bit = 0u; bit < 8u; bit += 7u) {
      const GemsNative before = f->driver;
      memcpy(damaged, s->payload, s->length);
      damaged[i] = (uint8_t)(damaged[i] ^ (1u << bit));
      *stage = f->driver;
      (void)gems_native_state_get(stage, damaged, s->length, at, s->expiry == 0u ? UINT64_MAX : s->expiry);
      CHECK(memcmp(&before, &f->driver, sizeof before) == 0, "decoding never touches the live driver (byte %u)", i);
    }
  }
  free(f); free(s); free(stage);
}

int main(void) {
  Fixture *a = (Fixture *)calloc(1, sizeof(Fixture)), *b = (Fixture *)calloc(1, sizeof(Fixture));
  if (a == NULL || b == NULL) return 2;
  test_identity();
  test_boot_and_digital_sequence(a);
  test_low_note_sample_mapping(a);
  test_cadence_invariance(a, b);
  test_stop_cuts_sample(a);
  test_faults(a);
  test_fm_psg_and_sequencer_features(a);
  test_restart_and_bus_hold(a);
  test_state_continuation();
  test_state_refusals();
  free(a); free(b);
  if (g_failures != 0) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
  puts("gems_native_tests: ok");
  return 0;
}
