/* SEG-104 (ADR 0244): CD-DA sector reading, sample format and drive-timed playback of the Sega CD runtime.
 *
 * A project-authored disc (one BIN of data + audio tracks with an in-file INDEX 00 pregap, plus a second file) is written to a scratch file and
 * opened through the real scd_disc_open (size and SHA-256 verified). Every sample is derived from a closed-form formula that is independent of
 * the reader (left and right differ everywhere; extremes -32768 / 32767 and byte-swap-sensitive values occur), so swapped channels or reversed
 * byte order fail. The machine is driven through the real Gate Array CDD registers (a sub-side word write of the command) and the real device
 * event loop; nothing is stubbed in the runtime. Layers: reader exactness and refusals; one sector = 588 frames and 75 sectors = 44,100;
 * play / pause / resume / seek / stop / data / pregap / track boundary / end-of-disc; fader; reset; sink independence; enabled=0 equality;
 * bounded memory over a long jump. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "scd_runtime.h"

static int failures;

static void check(int ok, const char *label) {
  if (!ok) {
    printf("FAIL  %s\n", label);
    ++failures;
  }
}

/* ---- the disc ------------------------------------------------------------------------------------------------------------------------ */
/* File 0: LBA 0-19 data (2352-byte raw), 20-21 INDEX 00 pregap of track 2, 22-101 track 2 audio (80 sectors: more than one second), 102 pregap of
 * track 3, 103-108 track 3 audio. File 1: LBA 109-112 track 4 audio, 113-116 track 5 data. */
enum { T2_PRE = 20, T2 = 22, T2_N = 80, T3_PRE = 102, T3 = 103, T3_N = 6, T4 = 109, T4_N = 4, T5 = 113, T5_N = 4 };

static int16_t sample_of(uint32_t lba, uint32_t frame, int channel) {
  /* two different mixes of the same inputs; the special values appear in fixed positions */
  uint32_t x = lba * 2654435761U + frame * 40503U + (channel ? 0x9E37U : 0x1357U);
  if (frame == 0U) return channel ? (int16_t)32767 : (int16_t)-32768;
  if (frame == 1U) return channel ? (int16_t)0x0100 : (int16_t)0x0001; /* byte-order sensitive */
  if (frame == 2U) return channel ? (int16_t)-1 : (int16_t)0;
  return (int16_t)(uint16_t)((x >> 7) ^ (x << 3) ^ (uint32_t)channel * 0x5A5AU);
}

static void put_audio_sector(uint8_t *out, uint32_t lba) {
  uint32_t f;
  for (f = 0; f < 588U; ++f) {
    const uint16_t l = (uint16_t)sample_of(lba, f, 0), r = (uint16_t)sample_of(lba, f, 1);
    out[4U * f + 0U] = (uint8_t)(l & 0xFFU); /* Red Book: left first, each sample least-significant byte first */
    out[4U * f + 1U] = (uint8_t)(l >> 8);
    out[4U * f + 2U] = (uint8_t)(r & 0xFFU);
    out[4U * f + 3U] = (uint8_t)(r >> 8);
  }
}

static uint8_t disc0[109U * 2352U], disc1[8U * 2352U];
static char hex0[65], hex1[65];
static ScdDiscFile files[2];
static ScdDiscTrack tracks[5];
static ScdDisc disc;
static const char *name0 = "cdda_test_0.bin", *name1 = "cdda_test_1.bin";

static void sha_hex(const uint8_t *data, size_t n, char out[65]) {
  GenesisSha256 sha;
  uint8_t digest[32];
  unsigned i;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, data, (uint32_t)n);
  genesis_sha256_final(&sha, digest);
  for (i = 0; i < 32U; ++i) snprintf(out + 2U * i, 3U, "%02x", digest[i]);
}

static void build_disc(void) {
  uint32_t lba;
  FILE *f;
  memset(disc0, 0x11, sizeof disc0); /* data-track filler: must never be read as audio */
  memset(disc1, 0x22, sizeof disc1);
  for (lba = T2_PRE; lba < T2_PRE + 2U; ++lba) memset(disc0 + lba * 2352U, 0x77, 2352U); /* INDEX 00 content (non-silent on purpose) */
  memset(disc0 + T3_PRE * 2352U, 0x77, 2352U);
  for (lba = T2; lba < T2 + T2_N; ++lba) put_audio_sector(disc0 + lba * 2352U, lba);
  for (lba = T3; lba < T3 + T3_N; ++lba) put_audio_sector(disc0 + lba * 2352U, lba);
  for (lba = T4; lba < T4 + T4_N; ++lba) put_audio_sector(disc1 + (lba - T4) * 2352U, lba);
  sha_hex(disc0, sizeof disc0, hex0);
  sha_hex(disc1, sizeof disc1, hex1);
  f = fopen(name0, "wb");
  if (f == NULL || fwrite(disc0, 1U, sizeof disc0, f) != sizeof disc0 || fclose(f) != 0) exit(1);
  f = fopen(name1, "wb");
  if (f == NULL || fwrite(disc1, 1U, sizeof disc1, f) != sizeof disc1 || fclose(f) != 0) exit(1);
  files[0].name = name0;
  files[0].size = sizeof disc0;
  files[0].sha256 = hex0;
  files[1].name = name1;
  files[1].size = sizeof disc1;
  files[1].sha256 = hex1;
  /* number, type (1 = raw mode 1, 2 = audio), file, sector size, file offset of INDEX 01, first LBA, sectors */
  tracks[0] = (ScdDiscTrack){1U, 1U, 0U, 2352U, 0U, 0U, 20U};
  tracks[1] = (ScdDiscTrack){2U, 2U, 0U, 2352U, (uint64_t)T2 * 2352U, T2, T2_N};
  tracks[2] = (ScdDiscTrack){3U, 2U, 0U, 2352U, (uint64_t)T3 * 2352U, T3, T3_N};
  tracks[3] = (ScdDiscTrack){4U, 2U, 1U, 2352U, 0U, T4, T4_N};
  tracks[4] = (ScdDiscTrack){5U, 1U, 1U, 2352U, (uint64_t)T4_N * 2352U, T5, T5_N};
  disc.files = files;
  disc.file_count = 2U;
  disc.tracks = tracks;
  disc.track_count = 5U;
  disc.identity = "cdda-test";
}

/* ---- the machine ------------------------------------------------------------------------------------------------------------------- */
static ScdMachine *machine;
static ScdProgram program;
static ScdAuthentic authentic;
static uint8_t bios[0x200];

typedef struct Sink {
  uint64_t frames, sectors;
  int refuse;
  int32_t lbas[4096];
  uint64_t first_tick;
  int16_t last[1176];
  int mismatches;     /* sectors whose samples differ from the formula */
} Sink;

static Sink sink;

static int sink_put(void *context, const int16_t *frames, uint32_t count) {
  Sink *const s = (Sink *)context;
  const int32_t lba = machine->cdda.next_lba - 1;
  uint32_t i;
  if (count != 588U) s->mismatches += 1;
  for (i = 0; i < count; ++i)
    if (frames[2U * i] != sample_of((uint32_t)lba, i, 0) || frames[2U * i + 1U] != sample_of((uint32_t)lba, i, 1)) {
      /* the fader may be scaling: only counted as a mismatch when the test expects unity */
      s->mismatches += 1;
      break;
    }
  if (s->sectors < 4096U) s->lbas[s->sectors] = lba;
  if (s->sectors == 0U) s->first_tick = machine->now;
  memcpy(s->last, frames, sizeof s->last);
  s->sectors += 1U;
  s->frames += count;
  return s->refuse;
}

static void fresh(int enabled) {
  if (machine != NULL) scd_machine_release(machine);
  memset(machine, 0, sizeof *machine);
  bios[1] = 0xFFU;
  bios[5] = 0x02U;
  memset(&sink, 0, sizeof sink);
  scd_disc_close(&disc);
  if (scd_disc_open(&disc, ".") != SCD_DISC_OK) {
    printf("FAIL  disc open\n");
    exit(1);
  }
  if (scd_machine_init_bios(machine, &program, &authentic, &disc, bios, sizeof bios, 1000000U) != 0) {
    printf("FAIL  machine init\n");
    exit(1);
  }
  machine->cdda.sink = sink_put;
  machine->cdda.sink_context = &sink;
  machine->cdda.enabled = (uint8_t)enabled;
}

/* run the real device event loop for `ticks` drive periods of machine time (events of every device interleave as they do in a run) */
static void drive_ticks(unsigned ticks) {
  const uint64_t until = machine->now + (uint64_t)ticks * SCD_CDD_TICK_TICKS;
  for (;;) {
    const uint64_t next = scd_devices_next_event(machine);
    if (next == UINT64_MAX || next >= until || machine->stop.stop_class != SCD_STOP_NONE) break;
    machine->now = next;
    scd_devices_advance(machine, next);
  }
  if (machine->now < until && machine->stop.stop_class == SCD_STOP_NONE) machine->now = until;
}

static void sub_write(uint32_t reg, uint32_t width, uint32_t value) {
  if (scd_write(machine, SCD_SUB, 0xFF8000U + reg, width, value, 0x100U) != 0) {
    printf("FAIL  sub write %X\n", reg);
    ++failures;
  }
}

static void cdd_command(unsigned command, int32_t lba) {
  const uint32_t msf = (uint32_t)(lba + 150), mm = msf / 4500U, ss = msf / 75U % 60U, ff = msf % 75U;
  const uint8_t tx[8] = {(uint8_t)command, 0, (uint8_t)(mm / 10U), (uint8_t)(mm % 10U), (uint8_t)(ss / 10U), (uint8_t)(ss % 10U),
                         (uint8_t)(ff / 10U), (uint8_t)(ff % 10U)};
  unsigned i;
  for (i = 0; i < 8U; ++i) sub_write(0x42U + i, 1U, tx[i]);
  sub_write(0x4AU, 2U, 0U);
}

/* drive to a state where the audio sink has seen `want` sectors (bounded) */
static void run_until_sectors(uint64_t want) {
  unsigned guard = 0;
  while (sink.sectors < want && guard++ < 2000U && machine->stop.stop_class == SCD_STOP_NONE) drive_ticks(1U);
}

/* ---- tests ---------------------------------------------------------------------------------------------------------------------- */
static void reader(void) {
  int16_t out[1176];
  uint32_t lba, f;
  int exact = 1, distinct = 1;
  for (lba = T2; lba < T2 + T2_N; ++lba) {
    check(scd_disc_read_cdda(&disc, lba, out) == SCD_DISC_OK, "audio sector reads");
    for (f = 0; f < 588U; ++f) {
      if (out[2U * f] != sample_of(lba, f, 0) || out[2U * f + 1U] != sample_of(lba, f, 1)) exact = 0;
      if (sample_of(lba, f, 0) == sample_of(lba, f, 1) && f > 2U) distinct = 0;
    }
  }
  check(exact, "track 2: every sample equals the closed form (L,R order; little-endian samples)");
  check(distinct, "the fixture's channels differ at every ordinary frame (a swap is detectable)");
  scd_disc_read_cdda(&disc, T2, out);
  check(out[0] == -32768 && out[1] == 32767 && out[2] == 1 && out[3] == 0x0100 && out[4] == 0 && out[5] == -1, "extremes and byte-order-sensitive values");
  for (lba = T3; lba < T3 + T3_N; ++lba)
    if (scd_disc_read_cdda(&disc, lba, out) != SCD_DISC_OK || out[0] != -32768 || out[2 * 587] != sample_of(lba, 587U, 0)) exact = 0;
  for (lba = T4; lba < T4 + T4_N; ++lba)
    if (scd_disc_read_cdda(&disc, lba, out) != SCD_DISC_OK || out[1] != 32767 || out[2 * 100 + 1] != sample_of(lba, 100U, 1)) exact = 0;
  check(exact, "tracks 3 and 4 (second file) are exact");
  check(scd_disc_read_cdda(&disc, 0U, out) == SCD_DISC_CDDA_FORMAT, "a data-track sector is never audio");
  check(scd_disc_read_cdda(&disc, T5, out) == SCD_DISC_CDDA_FORMAT, "a second-file data sector is never audio");
  check(scd_disc_read_cdda(&disc, T2_PRE, out) == SCD_DISC_LBA_OUT_OF_RANGE, "an INDEX 00 pregap sector is outside every track's data");
  check(scd_disc_read_cdda(&disc, T3_PRE, out) == SCD_DISC_LBA_OUT_OF_RANGE, "a second pregap sector is outside every track's data");
  check(scd_disc_read_cdda(&disc, T5 + T5_N, out) == SCD_DISC_LBA_OUT_OF_RANGE, "beyond the last track");
  check(scd_disc_read_cdda(&disc, 0xFFFFFFFFU, out) == SCD_DISC_LBA_OUT_OF_RANGE, "a huge LBA");
  {
    /* a table that over-claims: the last declared sector lies beyond the verified file (never read) */
    ScdDiscTrack over[1] = {{2U, 2U, 1U, 2352U, 7U * 2352U + 1U, 100U, 3U}};
    ScdDisc d2 = disc;
    d2.tracks = over;
    d2.track_count = 1U;
    check(scd_disc_read_cdda(&d2, 100U, out) == SCD_DISC_IO, "a range beyond the verified file is refused before any read");
    over[0].sector_size = 2048U;
    check(scd_disc_read_cdda(&d2, 100U, out) == SCD_DISC_CDDA_FORMAT, "a non-2352 audio track is refused");
  }
}

static int sequence_is(const int32_t *expected, unsigned n) {
  unsigned i;
  if (sink.sectors != n) return 0;
  for (i = 0; i < n; ++i)
    if (sink.lbas[i] != expected[i]) return 0;
  return 1;
}

static void play_and_counts(void) {
  fresh(1);
  check(sink.sectors == 0U && machine->cdda.frames == 0U, "power-on silence");
  cdd_command(3U, T2);
  run_until_sectors(75U);
  check(sink.sectors == 75U && sink.frames == 75U * 588U && sink.frames == 44100U, "75 sectors are exactly 44,100 frames");
  check(machine->cdda.frames == 44100U && machine->cdda.sectors == 75U, "machine counters equal the sink");
  check(sink.lbas[0] == T2, "playback starts at the commanded LBA");
  check(sink.mismatches == 0, "all 75 sectors exact at unity");
  check(machine->cdda.segments == 1U && machine->cdda.fader_volume == 1024U, "one segment, fader unity");
  /* one sector produces 588 frames */
  fresh(1);
  cdd_command(3U, T2);
  run_until_sectors(1U);
  check(sink.sectors == 1U && sink.frames == 588U && sink.mismatches == 0, "one sector = 588 stereo frames, exact");
}

static void audio_sequence(void) {
  static const int32_t expected_t2_to_t4[] = {T2 + T2_N - 1, T3, T3 + 1, T3 + 2, T3 + 3, T3 + 4, T3 + 5, T4, T4 + 1, T4 + 2, T4 + 3};
  fresh(1);
  /* track 2 last sector, the track-3 pregap (silent: the drive reports no audio playing), track 3, then track 4 in the second file */
  cdd_command(3U, T2 + T2_N - 1);
  run_until_sectors(11U);
  check(sequence_is(expected_t2_to_t4, 11U), "track boundary: pregap silent, then contiguous track 3, then track 4 (file 1)");
  check(machine->cdda.pregap_ticks == 1U && machine->cdda.interruptions == 1U && machine->cdda.segments == 2U, "pregap tick counted; one run ended, one began");
  check(sink.mismatches == 0, "all boundary sectors exact");
  /* into the data track: no more audio, then past the last track */
  drive_ticks(12U);
  check(sink.sectors == 11U, "data track and end of disc produce no audio");
  check(machine->cdd.status == SCD_CDD_END, "the drive reports end-of-disc (unchanged behaviour)");

  /* pause / resume */
  fresh(1);
  cdd_command(3U, T2);
  run_until_sectors(3U);
  cdd_command(6U, 0);
  drive_ticks(10U);
  check(sink.sectors == 3U, "pause: no new audio");
  cdd_command(7U, 0);
  run_until_sectors(6U);
  check(sink.lbas[3] == sink.lbas[2] + 1 || sink.lbas[3] == sink.lbas[2] + 2, "resume continues at the drive position (no replay, no skip beyond one sector)");
  check(machine->cdda.segments == 2U && machine->cdda.interruptions == 1U, "pause ended a run; resume began one");
  check(machine->cdda.commands[6] == 1U && machine->cdda.commands[7] == 1U && machine->cdda.commands[3] == 1U, "command counters");

  /* seek: pauses at the target; then play from it - nothing from the old track after the new state takes effect */
  fresh(1);
  cdd_command(3U, T2);
  run_until_sectors(4U);
  cdd_command(4U, T3 + 2);
  drive_ticks(12U);
  check(sink.sectors <= 5U, "seek: at most the sector in flight before the command, none after");
  {
    const uint64_t before = sink.sectors;
    cdd_command(3U, T3 + 2);
    run_until_sectors(before + 2U);
    check(sink.lbas[before] == T3 + 2 && sink.lbas[before + 1] == T3 + 3, "play after seek starts exactly at the target");
  }
  check(machine->cdda.suppressed_pending <= 2U, "ticks with a command pending are silent");

  /* stop */
  fresh(1);
  cdd_command(3U, T3);
  run_until_sectors(2U);
  cdd_command(1U, 0);
  drive_ticks(10U);
  check(sink.sectors == 2U && machine->cdd.status == SCD_CDD_TOC_STATUS, "stop: silent");

  /* a data track: PLAY status, no audio */
  fresh(1);
  cdd_command(3U, 3);
  drive_ticks(15U);
  check(sink.sectors == 0U && machine->cdd.status == SCD_CDD_PLAY && machine->cdd.delivered > 0U, "a data track plays no audio");

  /* peripheral reset */
  fresh(1);
  cdd_command(3U, T2);
  run_until_sectors(3U);
  sub_write(0x34U, 2U, 0x1000U); /* fader target 256 */
  scd_peripheral_reset(machine);
  drive_ticks(8U);
  check(sink.sectors == 3U && machine->cdda.fader_volume == 1024U && machine->cdda.fader_target == 1024U, "peripheral reset restores silence and the fader");

  /* repeated start / stop sequences are deterministic */
  {
    uint64_t sectors[2], frames[2];
    unsigned round;
    for (round = 0; round < 2U; ++round) {
      unsigned k;
      fresh(1);
      for (k = 0; k < 3U; ++k) {
        cdd_command(3U, T2 + (int32_t)k);
        run_until_sectors(sink.sectors + 2U);
        cdd_command(1U, 0);
        drive_ticks(3U);
      }
      sectors[round] = sink.sectors;
      frames[round] = machine->cdda.frames;
    }
    check(sectors[0] == sectors[1] && sectors[0] == 6U && frames[0] == frames[1], "repeated start/stop sequences are deterministic");
  }
}

static void fader(void) {
  uint32_t i;
  int ramp = 1;
  fresh(1);
  sub_write(0x34U, 2U, 0x2000U); /* 12-bit 0x200 = 512: half scale, reached one step per frame */
  cdd_command(3U, T2);
  run_until_sectors(1U);
  check(machine->cdda.fader_target == 512U && machine->cdda.fader_volume == 512U, "fader reached its target within the first sector");
  for (i = 0; i < 588U; ++i) {
    const uint32_t volume = i < 512U ? 1024U - i : 512U;
    const uint32_t mul = (volume & 0x7FCU) ? (volume & 0x7FCU) : (volume & 3U);
    const int32_t el = ((int32_t)sample_of(T2, i, 0) * (int32_t)mul) / 1024, er = ((int32_t)sample_of(T2, i, 1) * (int32_t)mul) / 1024;
    if (sink.last[2U * i] != el || sink.last[2U * i + 1U] != er) ramp = 0;
  }
  check(ramp, "fader ramp: one step per frame, LC7883 multiplier, truncation toward zero");
  fresh(1);
  sub_write(0x34U, 2U, 0xFFF0U); /* above unity: clamped, no amplification */
  check(machine->cdda.fader_target == 1024U, "targets above unity are clamped");
  sub_write(0x34U, 1U, 0x00U);
  sub_write(0x35U, 1U, 0x00U);
  check(machine->cdda.fader_register == 0U && machine->cdda.fader_target == 0U, "byte writes compose the register");
  machine->cdda.fader_volume = 0U;
  cdd_command(3U, T2);
  run_until_sectors(2U);
  {
    int silent = 1;
    for (i = 0; i < 1176U; ++i)
      if (sink.last[i] != 0) silent = 0;
    check(silent && sink.sectors == 2U && machine->cdda.frames == 1176U, "volume 0 is true silence while the position still advances");
  }
}

static void sink_independence(void) {
  uint64_t log_on, log_refused, log_off;
  uint32_t lba_on, lba_off;
  uint64_t retired_on, retired_off, now_on, now_off;
  uint8_t rx_on[10], rx_off[10];
  fresh(1);
  cdd_command(3U, T2 + 8);
  drive_ticks(30U);
  log_on = machine->device_log;
  lba_on = (uint32_t)machine->cdd.lba;
  now_on = machine->now;
  retired_on = machine->cpu[0].retired + machine->cpu[1].retired;
  memcpy(rx_on, machine->cdd.rx, 10U);
  fresh(1);
  sink.refuse = 1; /* a refusing host */
  cdd_command(3U, T2 + 8);
  drive_ticks(30U);
  log_refused = machine->device_log;
  check(machine->cdda.sink_refused > 0U && machine->cdd.lba == (int32_t)lba_on && log_refused == log_on, "a refusing sink changes nothing in the machine");
  fresh(0); /* output disabled: nothing is read, nothing is produced */
  cdd_command(3U, T2 + 8);
  drive_ticks(30U);
  log_off = machine->device_log;
  lba_off = (uint32_t)machine->cdd.lba;
  now_off = machine->now;
  retired_off = machine->cpu[0].retired + machine->cpu[1].retired;
  memcpy(rx_off, machine->cdd.rx, 10U);
  check(sink.sectors == 0U && machine->cdda.sectors == 0U, "disabled output produces nothing");
  check(log_on == log_off && lba_on == lba_off && now_on == now_off && retired_on == retired_off && memcmp(rx_on, rx_off, 10U) == 0,
        "output enabled or disabled: identical device log, drive position, time, retirement and CDD status");
}

static void long_jump(void) {
  /* two minutes of guest time over a looping seek-free play of the last audio sectors is not possible (the disc is short); instead play
   * track 2 repeatedly through many commands: the producer holds one sector, so memory is bounded by construction. */
  unsigned round;
  fresh(1);
  for (round = 0; round < 400U; ++round) {
    cdd_command(3U, T2 + (int32_t)(round % 8U));
    drive_ticks(30U);
  }
  check(machine->cdda.sectors > 400U && machine->stop.stop_class == SCD_STOP_NONE, "many play commands over a long jump keep producing with bounded state");
}

int main(void) {
  build_disc();
  machine = (ScdMachine *)calloc(1U, sizeof *machine);
  if (machine == NULL) return 1;
  if (scd_disc_open(&disc, ".") != SCD_DISC_OK) {
    printf("FAIL  disc open\n");
    return 1;
  }
  reader();
  play_and_counts();
  audio_sequence();
  fader();
  sink_independence();
  long_jump();
  scd_machine_release(machine);
  scd_disc_close(&disc);
  free(machine);
  remove(name0);
  remove(name1);
  printf(failures == 0 ? "sega_cd_cdda_test: all checks passed\n" : "sega_cd_cdda_test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
