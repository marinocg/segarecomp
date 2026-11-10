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
  /* a minimal Timer A model: the device side of the Timer-A-paced DAC */
  uint8_t latched;
  uint16_t timer_a;
  uint64_t expiry;
  int flag;
  GemsNative driver;
} Fixture;

static void fx_ym_write(void *context, uint32_t port, uint8_t value, uint64_t ticks) {
  Fixture *f = (Fixture *)context;
  f->log[f->count].kind = 'Y'; f->log[f->count].port = port; f->log[f->count].value = value; f->log[f->count++].ticks = ticks;
  if ((port & 1u) == 0u) { if (port == 0u) f->latched = value; return; }
  if (port != 1u) return;
  if (f->latched == 0x24) f->timer_a = (uint16_t)((f->timer_a & 3u) | ((unsigned)value << 2));
  else if (f->latched == 0x25) f->timer_a = (uint16_t)((f->timer_a & ~3u) | (value & 3u));
  else if (f->latched == 0x27) {
    if (value & 0x10u) f->flag = 0;
    f->expiry = (value & 1u) ? ticks + (uint64_t)(1024u - f->timer_a) * YM2612_MASTER_TICKS_PER_SAMPLE : 0u;
  }
}
static uint8_t fx_ym_read(void *context, uint32_t port, uint64_t ticks) {
  Fixture *f = (Fixture *)context;
  (void)port;
  if (f->expiry != 0u && ticks >= f->expiry) f->flag = 1;
  return (uint8_t)f->flag;
}
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
  le16(f->rom + PATCH_TABLE, 4);
  le16(f->rom + PATCH_TABLE + 2, 4 + 39);
  f->rom[PATCH_TABLE + 4] = 1; f->rom[PATCH_TABLE + 5] = 4;          /* patch 0: digital, no rate override */
  f->rom[PATCH_TABLE + 4 + 39] = 0;                                  /* patch 1: FM */
  /* sequence table: offsets of the sequence headers */
  le16(f->rom + SEQ_TABLE + 0, 8); le16(f->rom + SEQ_TABLE + 2, 8 + 3); le16(f->rom + SEQ_TABLE + 4, 8 + 6); le16(f->rom + SEQ_TABLE + 6, 8 + 9);
  p = f->rom + SEQ_TABLE + 8;
  p[0] = 1; le16(p + 1, 0x40);                                       /* seq 0: one track at +0x40 */
  p[3] = 1; le16(p + 4, 0x60);                                       /* seq 1 */
  p[6] = 1; le16(p + 7, 0x80);                                       /* seq 2 */
  p[9] = 0;                                                          /* seq 3: empty */
  {
    static const uint8_t t0[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x00, 0xC2, 0x30, 0x60};   /* delay 0, prio 100, SFX base, patch 0, delay 2, note 48, eos */
    static const uint8_t t1[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x01, 0xC2, 0x30, 0x60};   /* the same with the FM patch */
    static const uint8_t t2[] = {0xC0, 0x64, 3};                                           /* a loop start: outside the verified subset */
    memcpy(f->rom + SEQ_TABLE + 0x40, t0, sizeof t0);
    memcpy(f->rom + SEQ_TABLE + 0x60, t1, sizeof t1);
    memcpy(f->rom + SEQ_TABLE + 0x80, t2, sizeof t2);
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
  host.context = f; host.ram = f->ram; host.rom_read = fx_rom_read; host.ym_write = fx_ym_write; host.ym_read = fx_ym_read; host.psg_write = fx_psg_write;
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
  const uint8_t bad_command[] = {0xFF, 5, 1};
  size_t after;
  /* unsupported command */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  fifo_put(f, bad_command, 3);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_UNSUPPORTED_COMMAND, "an unsupported command faults explicitly");
  after = f->count;
  gems_native_run_to(&f->driver, 10 * FRAME);
  CHECK(f->count == after, "a faulted driver writes nothing further");
  CHECK(f->ram[0x37] == 0, "no acknowledgement (read pointer advance) is fabricated for an unsupported command");
  /* FM patch */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 1);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_UNSUPPORTED_PATCH, "a note on an FM patch faults");
  /* unsupported sequence event */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 2);
  CHECK(gems_native_run_to(&f->driver, 10 * FRAME) == GEMS_FAULT_UNSUPPORTED_SEQUENCE_EVENT, "an event outside the subset faults");
  /* unmapped resource: pointers into the void */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  { const uint8_t c[] = {0xFF, 11, 0, 0, 0, 0, 0, 0, 0xFE, 0xFF, 0, 0, 0, 0}; fifo_put(f, c, 14); }   /* sequence table at $FFFE: its header is not inside the cartridge */
  send_start(f, 0);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_UNMAPPED_RESOURCE, "a resource outside the cartridge faults");
  /* invalid resource: more than 16 tracks */
  build_cartridge(f); f->rom[SEQ_TABLE + 8 + 9] = 17; memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 3);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_INVALID_RESOURCE, "a track count above 16 is an invalid resource");
  /* invalid FIFO pointer */
  build_cartridge(f); memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  f->ram[0x36] = 0x41;
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_INVALID_COMMAND_STREAM, "a write pointer outside the ring is rejected");
  /* an empty sequence is accepted and starts nothing */
  build_cartridge(f); f->rom[SEQ_TABLE + 8 + 9] = 0; memset(f->ram, 0, sizeof f->ram); fx_start(f, 1000);
  send_pointers(f); send_start(f, 3);
  CHECK(gems_native_run_to(&f->driver, 3 * FRAME) == GEMS_FAULT_NONE && f->driver.count_sequences == 0, "an empty sequence starts nothing");
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

int main(void) {
  Fixture *a = (Fixture *)calloc(1, sizeof(Fixture)), *b = (Fixture *)calloc(1, sizeof(Fixture));
  if (a == NULL || b == NULL) return 2;
  test_identity();
  test_boot_and_digital_sequence(a);
  test_cadence_invariance(a, b);
  test_stop_cuts_sample(a);
  test_faults(a);
  test_restart_and_bus_hold(a);
  free(a); free(b);
  if (g_failures != 0) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
  puts("gems_native_tests: ok");
  return 0;
}
