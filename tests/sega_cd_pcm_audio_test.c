/* SEG-107 (ADR 0251): RF5C164 PCM synthesis and the mixed PCM + CD-DA output timeline of the Sega CD runtime.
 *
 * The machine is driven only through its real access path (sub-side byte writes to the PCM area through scd_write, the real peripheral reset, the real
 * Gate Array CDD command registers and the real device event loop). The expectation comes from an independent reference written here from the
 * published chip behavior (GPGX pcm_run / MAME rf5c68: sign-magnitude bytes with bit 7 = positive, $FF loop marker, (value x ENV x PAN-nibble) >> 5,
 * summed and clamped), with its own register file, its own sample clock (one event per 384 sub cycles, an access sees the events strictly before it)
 * and its own 44.1 kHz timeline (frame n at floor(n x ticks_per_second / 44100), zero-order hold). The reference rounds negative products toward zero
 * (MAME) where the product rounds toward minus infinity (GPGX): the two may differ by one LSB per running negative channel, which is the tolerance of
 * the scripts that contain negative samples; positive-only scripts are exact.
 *
 * Covered: single channel known waveform, eight channels with saturation, ENV and PAN, channel ON/OFF, chip enable, loop markers (and a loop onto a
 * loop), wave RAM changed while playing, reset, accesses on and next to a sample-clock instant, different advance partitions (identical streams),
 * long bounded output, PCM-only, CD-DA-only and mixed output, silence when nothing plays, and that producing audio changes nothing the guest can
 * observe (pointer reads, device log, time, retirement). */
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
static const char *name0 = "pcm_audio_test_0.bin", *name1 = "pcm_audio_test_1.bin";

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
  disc.identity = "pcm-audio-test";
}


/* ---- the machine ------------------------------------------------------------------------------------------------------------------- */
static ScdMachine *machine;
static ScdProgram program;
static ScdAuthentic authentic;
static uint8_t bios[0x200];

#define MAXF 520000U
static int16_t *out_buf;
static size_t out_frames;

typedef struct Arrival {
  uint64_t tick;
  int32_t lba;
} Arrival;
static Arrival arrivals[4096];
static unsigned n_arrivals;
static uint64_t legacy_frames;
static uint64_t stop_tick;      /* the instant the drive stopped reporting the run (UINT64_MAX = still reported) */
static uint8_t was_playing;

static int mix_put(void *context, const int16_t *frames, uint32_t count) {
  (void)context;
  if (out_frames + count > MAXF) return 1;
  memcpy(out_buf + 2U * out_frames, frames, (size_t)count * 4U);
  out_frames += count;
  return 0;
}

static int refuse_put(void *context, const int16_t *frames, uint32_t count) {
  (void)context;
  (void)frames;
  (void)count;
  return 1;
}

static int legacy_put(void *context, const int16_t *frames, uint32_t count) {
  (void)context;
  (void)frames;
  if (n_arrivals < 4096U) {
    arrivals[n_arrivals].tick = machine->now;
    arrivals[n_arrivals].lba = machine->cdda.next_lba - 1;
    ++n_arrivals;
  }
  legacy_frames += count;
  return 0;
}

static void fresh(int cdda, int mix) {
  if (machine != NULL) scd_machine_release(machine);
  memset(machine, 0, sizeof *machine);
  bios[1] = 0xFFU;
  bios[5] = 0x02U;
  out_frames = 0U;
  n_arrivals = 0U;
  legacy_frames = 0U;
  stop_tick = UINT64_MAX;
  was_playing = 0U;
  scd_disc_close(&disc);
  if (scd_disc_open(&disc, ".") != SCD_DISC_OK) {
    printf("FAIL  disc open\n");
    exit(1);
  }
  if (scd_machine_init_bios(machine, &program, &authentic, &disc, bios, sizeof bios, 1000000U) != 0) {
    printf("FAIL  machine init\n");
    exit(1);
  }
  machine->cdda.sink = legacy_put;
  machine->cdda.enabled = (uint8_t)cdda;
  machine->mix.sink = mix_put;
  machine->mix.enabled = (uint8_t)mix;
}

static uint64_t period(void) { return SCD_TIMER_UNIT_SUB_CYCLES * scd_sub_cycle_ticks(machine); }

static void chip_write(uint32_t reg, uint32_t value) {
  if (scd_write(machine, SCD_SUB, 0xFF0000U + (reg << 1) + 1U, 1U, value, 0x100U) != 0) {
    printf("FAIL  chip write %X\n", reg);
    ++failures;
  }
}

static uint32_t pointer_read(unsigned chip_offset) {
  uint32_t v = 0xDEADU;
  if (scd_read(machine, SCD_SUB, 0xFF0000U + (chip_offset << 1) + 1U, 1U, &v, 0x100U) != 0) {
    printf("FAIL  pointer read\n");
    ++failures;
  }
  return v;
}

static void sub_write(uint32_t reg, uint32_t width, uint32_t value) {
  if (scd_write(machine, SCD_SUB, 0xFF8000U + reg, width, value, 0x100U) != 0) {
    printf("FAIL  sub write %X\n", reg);
    ++failures;
  }
}

/* the instant `playing` falls is the interrupt: the run is no longer reported by the drive */
static void watch_playing(void) {
  if (was_playing && !machine->cdda.playing && stop_tick == UINT64_MAX) stop_tick = machine->now;
  was_playing = machine->cdda.playing;
}

static void drive_until(uint64_t until) {
  for (;;) {
    const uint64_t next = scd_devices_next_event(machine);
    if (next == UINT64_MAX || next >= until || machine->stop.stop_class != SCD_STOP_NONE) break;
    machine->now = next;
    scd_devices_advance(machine, next);
    watch_playing();
  }
  if (machine->now < until && machine->stop.stop_class == SCD_STOP_NONE) machine->now = until;
}

static void cdd_command(unsigned command, int32_t lba) {
  const uint32_t msf = (uint32_t)(lba + 150), mm = msf / 4500U, ss = msf / 75U % 60U, ff = msf % 75U;
  const uint8_t tx[8] = {(uint8_t)command, 0, (uint8_t)(mm / 10U), (uint8_t)(mm % 10U), (uint8_t)(ss / 10U), (uint8_t)(ss % 10U),
                         (uint8_t)(ff / 10U), (uint8_t)(ff % 10U)};
  unsigned i;
  for (i = 0; i < 8U; ++i) sub_write(0x42U + i, 1U, tx[i]);
  sub_write(0x4AU, 2U, 0U);
  watch_playing();
}

/* ---- scripts --------------------------------------------------------------------------------------------------------------------- */
typedef struct Op {
  uint64_t t;
  int reset;
  uint32_t reg, value;
} Op;
static Op ops[8192];
static unsigned n_ops;

static void W(uint64_t t, uint32_t reg, uint32_t value) {
  if (n_ops >= 8192U) exit(1);
  ops[n_ops++] = (Op){t, 0, reg, value};
}
static void RESET(uint64_t t) {
  if (n_ops >= 8192U) exit(1);
  ops[n_ops++] = (Op){t, 1, 0, 0};
}

/* a channel: select it, then ENV, PAN, FD, LS, ST (all written at the same instant t, in order) */
static void chan(uint64_t t, unsigned ch, unsigned env, unsigned pan, unsigned fd, unsigned ls, unsigned st) {
  W(t, 7, 0xC0U | ch);
  W(t, 0, env);
  W(t, 1, pan);
  W(t, 2, fd & 0xFFU);
  W(t, 3, fd >> 8);
  W(t, 4, ls & 0xFFU);
  W(t, 5, ls >> 8);
  W(t, 6, st);
}
static void wave(uint64_t t, unsigned bank, unsigned at, const uint8_t *bytes, unsigned n) {
  unsigned i;
  W(t, 7, 0x80U | bank);
  for (i = 0; i < n; ++i) W(t, 0x1000U + ((at + i) & 0xFFFU), bytes[i]);
}
static void onoff(uint64_t t, unsigned on_mask) { W(t, 8, (~on_mask) & 0xFFU); }

/* ---- the independent reference ----------------------------------------------------------------------------------------------------- */
static uint8_t R_ram[65536], R_env[8], R_pan[8], R_st[8], R_idx, R_bank, R_en, R_onoff;
static uint16_t R_fd[8], R_ls[8];
static uint32_t R_addr[8];

static void ref_reset(void) {
  unsigned c;
  memset(R_ram, 0, sizeof R_ram);
  memset(R_env, 0, sizeof R_env);
  memset(R_st, 0, sizeof R_st);
  memset(R_fd, 0, sizeof R_fd);
  memset(R_ls, 0, sizeof R_ls);
  memset(R_addr, 0, sizeof R_addr);
  R_idx = R_bank = R_en = 0;
  R_onoff = 0; /* nothing runs after a reset: the run mask is the complement of the last ON/OFF write, and 0 until one is written */
  for (c = 0; c < 8U; ++c) R_pan[c] = 0xFFU;
}
static int R_running[8];

static void ref_write(uint32_t reg, uint32_t v) {
  unsigned c;
  if (reg >= 0x1000U) {
    R_ram[((uint32_t)R_bank << 12) | (reg & 0xFFFU)] = (uint8_t)v;
    return;
  }
  switch (reg) {
  case 0: R_env[R_idx] = (uint8_t)v; break;
  case 1: R_pan[R_idx] = (uint8_t)v; break;
  case 2: R_fd[R_idx] = (uint16_t)((R_fd[R_idx] & 0xFF00U) | v); break;
  case 3: R_fd[R_idx] = (uint16_t)((R_fd[R_idx] & 0xFFU) | (v << 8)); break;
  case 4: R_ls[R_idx] = (uint16_t)((R_ls[R_idx] & 0xFF00U) | v); break;
  case 5: R_ls[R_idx] = (uint16_t)((R_ls[R_idx] & 0xFFU) | (v << 8)); break;
  case 6:
    R_st[R_idx] = (uint8_t)v;
    if (!R_running[R_idx]) R_addr[R_idx] = (uint32_t)v << 19;
    break;
  case 7:
    if (v & 0x40U) R_idx = (uint8_t)(v & 7U);
    else R_bank = (uint8_t)(v & 15U);
    R_en = (uint8_t)((v >> 7) & 1U);
    break;
  case 8:
    R_onoff = (uint8_t)v;
    for (c = 0; c < 8U; ++c) {
      R_running[c] = !((v >> c) & 1U);
      if (!R_running[c]) R_addr[c] = (uint32_t)R_st[c] << 19;
    }
    break;
  default: break;
  }
}

typedef struct Smp {
  uint64_t t;
  int32_t l, r;
} Smp;
static Smp *smp;
static unsigned n_smp;

static uint64_t frame_time(uint64_t n) { return (n / 44100U) * SCD_TICKS_PER_SECOND + ((n % 44100U) * SCD_TICKS_PER_SECOND) / 44100U; }

static int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* The reference stream for the script up to `end`, mixed with `n_cd` CD-DA sectors (arrival ticks and LBAs) when given. */
typedef struct RefCd {
  const Arrival *arr;          /* the sectors the drive delivered, in order (one contiguous run) */
  unsigned n_arr;
  int32_t cap;                 /* end (exclusive) of the run's track */
  uint64_t stop;               /* the instant the drive stopped reporting the run */
  uint64_t fader_tick;         /* the CD-DA fader register write (target 1024 before it) */
  unsigned fader_target;
} RefCd;

static size_t ref_render(uint64_t end, int16_t *out, const RefCd *cd) {
  uint64_t origin = 0U, k = 0U, e, n;
  unsigned opi = 0U, j = 0U, c;
  const uint64_t per = period();
  ref_reset();
  memset(R_running, 0, sizeof R_running);
  n_smp = 0U;
  for (;;) {
    e = origin + k * per;
    while (opi < n_ops && ops[opi].t <= e) {
      if (ops[opi].reset) {
        ref_reset();
        memset(R_running, 0, sizeof R_running);
        origin = ops[opi].t;
        k = 0U;
        e = origin;
      } else {
        ref_write(ops[opi].reg, ops[opi].value);
      }
      ++opi;
    }
    if (e >= end) break;
    {
      int32_t L = 0, Rr = 0;
      if (R_en)
        for (c = 0; c < 8U; ++c)
          if (R_running[c]) {
            unsigned b = R_ram[(R_addr[c] >> 11) & 0xFFFFU];
            int32_t mag, lv, rv;
            if (b == 0xFFU) {
              R_addr[c] = (uint32_t)R_ls[c] << 11;
              b = R_ram[R_ls[c]];
              if (b == 0xFFU) continue;
            } else {
              R_addr[c] = (R_addr[c] + R_fd[c]) & 0x7FFFFFFU;
            }
            mag = (int32_t)(b & 0x7FU);
            lv = (int32_t)(R_pan[c] & 15U) * (int32_t)R_env[c];
            rv = (int32_t)(R_pan[c] >> 4) * (int32_t)R_env[c];
            if (b & 0x80U) {
              L += (mag * lv) >> 5;
              Rr += (mag * rv) >> 5;
            } else {
              L -= (mag * lv) >> 5;
              Rr -= (mag * rv) >> 5;
            }
          }
      smp[n_smp++] = (Smp){e, clamp16(L), clamp16(Rr)};
    }
    ++k;
  }
  {
    /* the CD-DA run: it starts at the first frame at or after the first delivery and then advances one frame per output frame. Sector j begins at frame
     * n0 + 588 j and is heard if the drive had delivered it by then, or if the drive still reports the run at that instant and the track goes on
     * (then it is read ahead). A fader write moves the volume one step per CD-DA frame toward its target. */
    uint64_t n0 = UINT64_MAX;
    unsigned vol = 1024U, target = 1024U;
    uint64_t cdn = 0U; /* CD-DA frames consumed */
    int ended = 0;
    if (cd != NULL && cd->n_arr != 0U) {
      for (n0 = 0U; frame_time(n0) < cd->arr[0].tick; ++n0) {}
    }
    for (n = 0U; frame_time(n) < end && n < MAXF; ++n) {
      const uint64_t T = frame_time(n);
      int32_t l, r;
      while (j < n_smp && smp[j].t <= T) ++j;
      l = j == 0U ? 0 : smp[j - 1U].l;
      r = j == 0U ? 0 : smp[j - 1U].r;
      if (cd != NULL && n0 != UINT64_MAX && n >= n0 && !ended) {
        const uint64_t pos = n - n0;
        const unsigned sec = (unsigned)(pos / 588U), fr = (unsigned)(pos % 588U);
        if (fr == 0U && sec > 0U) {
          const int delivered = sec < cd->n_arr && cd->arr[sec].tick <= T;
          const int ahead = T < cd->stop && cd->arr[0].lba + (int32_t)sec < cd->cap;
          if (!delivered && !ahead) ended = 1;
        }
        if (!ended) {
          int32_t cl, cr;
          if (T >= cd->fader_tick) target = cd->fader_target;
          cl = sample_of((uint32_t)(cd->arr[0].lba + (int32_t)sec), fr, 0);
          cr = sample_of((uint32_t)(cd->arr[0].lba + (int32_t)sec), fr, 1);
          {
            const unsigned mult = (vol & 0x7FCU) != 0U ? (vol & 0x7FCU) : (vol & 3U);
            cl = (cl * (int32_t)mult) / 1024;
            cr = (cr * (int32_t)mult) / 1024;
          }
          if (vol < target) ++vol;
          else if (vol > target) --vol;
          ++cdn;
          l += cl;
          r += cr;
        }
      }
      out[2U * n] = (int16_t)clamp16(l);
      out[2U * n + 1U] = (int16_t)clamp16(r);
    }
  }
  return (size_t)n;
}

/* ---- the machine side ---------------------------------------------------------------------------------------------------------- */
static uint64_t rng_state;
static uint64_t rng(void) {
  rng_state = rng_state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
  return rng_state >> 24;
}

static void run_script(uint64_t end, int partition) {
  unsigned i;
  uint64_t extra = 0U;
  rng_state = 12345U;
  for (i = 0; i < n_ops; ++i) {
    if (partition)
      while (extra < ops[i].t) { /* extra advances at pseudo-random instants, before the next access */
        extra += 1U + rng() % (3U * period());
        if (extra < ops[i].t && extra >= machine->now) {
          machine->now = extra;
          scd_mix_advance(machine, extra);
        }
      }
    if (ops[i].t > machine->now) machine->now = ops[i].t;
    if (ops[i].reset) scd_pcm_reset(machine);
    else chip_write(ops[i].reg, ops[i].value);
  }
  if (end > machine->now) machine->now = end;
  scd_mix_advance(machine, end);
}

static int16_t *ref_buf;

static uint64_t fnv(const int16_t *p, size_t frames) {
  uint64_t h = UINT64_C(14695981039346656037);
  size_t i;
  for (i = 0; i < 2U * frames; ++i) {
    h = (h ^ (uint16_t)p[i]) * UINT64_C(1099511628211);
  }
  return h;
}

/* Run the script on a fresh PCM-only machine, with and without extra advances, and compare with the reference within `tol`. Returns the maximum
 * sample difference (-1 on a structural failure). */
static int script_case(const char *label, uint64_t end, int tol, int expect_sound) {
  size_t ref_n, i;
  int maxdiff = 0, ok = 1;
  uint64_t digest_plain, digest_part;
  size_t frames_plain, nonzero = 0U;
  n_arrivals = 0U;
  fresh(0, 1);
  run_script(end, 0);
  digest_plain = fnv(out_buf, out_frames);
  frames_plain = out_frames;
  ref_n = ref_render(end, ref_buf, NULL);
  if (ref_n != out_frames) {
    printf("FAIL  %s: frame count %zu vs reference %zu\n", label, out_frames, ref_n);
    ++failures;
    return -1;
  }
  for (i = 0; i < 2U * ref_n; ++i) {
    const int d = abs((int)out_buf[i] - (int)ref_buf[i]);
    if (d > maxdiff) maxdiff = d;
    if (d > tol) ok = 0;
    if (out_buf[i] != 0) ++nonzero;
  }
  check(ok, label);
  if (!ok) printf("      max difference %d (tolerance %d)\n", maxdiff, tol);
  check(expect_sound ? nonzero > 0U : nonzero == 0U, expect_sound ? "the stream is not silent" : "the stream is silent");
  fresh(0, 1);
  run_script(end, 1);
  digest_part = fnv(out_buf, out_frames);
  check(out_frames == frames_plain && digest_part == digest_plain, "extra advances at arbitrary instants leave the stream identical");
  return maxdiff;
}

/* ---- waves ----------------------------------------------------------------------------------------------------------------------- */
static uint8_t wv_ramp[64]; /* positive ramp 0..62 (bit 7 set), negative ramp, loop marker at the end */
static void make_waves(void) {
  unsigned i;
  for (i = 0; i < 32U; ++i) wv_ramp[i] = (uint8_t)(0x80U | (i * 4U)); /* +0 .. +124 */
  for (i = 0; i < 31U; ++i) wv_ramp[32U + i] = (uint8_t)(0x7FU - i * 4U); /* -127 .. -3 */
  wv_ramp[63] = 0xFFU;                                                   /* loop to LS */
}

static void t_single_channel(void) {
  const uint64_t P = period();
  uint8_t positive[40];
  unsigned i;
  for (i = 0; i < 39U; ++i) positive[i] = (uint8_t)(0x80U | (i * 3U));
  positive[39] = 0xFFU;
  /* positive-only waveform, unit step, full ENV/PAN: exact */
  n_ops = 0U;
  wave(1000U, 0, 0, positive, 40U);
  chan(2000U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(3000U, 1U << 0);
  script_case("single channel, positive waveform, loop to start: equals the reference exactly", 200U * P, 0, 1);
  /* the same channel at half speed and a fractional step: the held sample and the 27-bit pointer */
  n_ops = 0U;
  wave(1000U, 0, 0, positive, 40U);
  chan(2000U, 0, 0xC0, 0xFF, 0x0345, 0, 0);
  onoff(3000U, 1U << 0);
  script_case("fractional frequency step", 300U * P, 0, 1);
  /* signed ramp with negative samples */
  n_ops = 0U;
  wave(1000U, 0, 0, wv_ramp, 64U);
  chan(2000U, 3, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(3000U, 1U << 3);
  script_case("single channel, signed ramp with negative samples (selected channel 3)", 300U * P, 1, 1);
  /* known constants: ENV $FF, PAN $FF (nibble 15), sample +126 / -127, unit step */
  {
    const uint8_t two[3] = {0xFEU, 0x7FU, 0xFFU};
    int16_t first_positive = 0, first_negative = 0;
    size_t n;
    n_ops = 0U;
    wave(0U, 0, 0, two, 3U);
    chan(0U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
    onoff(0U, 1U);
    fresh(0, 1);
    run_script(40U * P, 0);
    for (n = 0U; n < out_frames; ++n) {
      if (out_buf[2U * n] > 0 && first_positive == 0) first_positive = out_buf[2U * n];
      if (out_buf[2U * n] < 0 && first_negative == 0) first_negative = out_buf[2U * n];
    }
    check(first_positive == (126 * 255 * 15) / 32, "+126 at ENV 255, PAN 15 is exactly (126 x 255 x 15) >> 5 = 15059");
    check(first_negative == -((127 * 255 * 15 + 31) / 32), "-127 rounds toward minus infinity (GPGX): -15181");
  }
}

static void t_multi_and_saturation(void) {
  const uint64_t P = period();
  uint8_t a[24], b[24];
  unsigned i, ch;
  for (i = 0; i < 23U; ++i) {
    a[i] = (uint8_t)(0x80U | ((i * 5U) & 0x7FU));
    b[i] = (uint8_t)(0x80U | ((127U - i * 3U) & 0x7EU));
  }
  a[23] = b[23] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, a, 24U);
  wave(100U, 1, 0, b, 24U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  chan(200U, 1, 0x80, 0x0F, 0x0600, 0, 0);
  chan(200U, 2, 0x40, 0xF0, 0x0A00, 0x1000 >> 0, 0x10); /* start address 0x10 << 19 = wave byte 0x1000 (bank 1) */
  onoff(300U, 0x07U);
  script_case("three simultaneous channels with different ENV, PAN, step and start", 400U * P, 0, 1);
  /* eight channels at the maximum: the sum is clamped (saturation, not wrapping) in both directions */
  {
    uint8_t hi[2] = {0xFEU, 0xFFU}, lo[2] = {0x7FU, 0xFFU};
    n_ops = 0U;
    wave(100U, 0, 0, hi, 2U);
    wave(100U, 1, 0, lo, 2U);
    for (ch = 0; ch < 8U; ++ch) chan(200U, ch, 0xFF, 0xFF, 0x0000, ch < 4U ? 0U : 0x1000U, ch < 4U ? 0U : 0x10U);
    onoff(300U, 0xFFU);
    /* channels 0-3 hold +126 forever (step 0), channels 4-7 hold -127 at bank 1 */
    script_case("eight channels: positive saturation and negative cancel", 100U * P, 8, 1);
    n_ops = 0U;
    wave(100U, 0, 0, hi, 2U);
    for (ch = 0; ch < 8U; ++ch) chan(200U, ch, 0xFF, 0xFF, 0x0000, 0, 0);
    onoff(300U, 0xFFU);
    fresh(0, 1);
    run_script(100U * P, 0);
    {
      size_t n, top = 0U;
      for (n = 0U; n < out_frames; ++n)
        if (out_buf[2U * n] == 32767 && out_buf[2U * n + 1U] == 32767) ++top;
      check(top > 0U && machine->mix.saturated > 0U, "eight full-scale positive channels saturate at +32767 (no wrap)");
    }
    n_ops = 0U;
    wave(100U, 0, 0, lo, 2U);
    for (ch = 0; ch < 8U; ++ch) chan(200U, ch, 0xFF, 0xFF, 0x0000, 0, 0);
    onoff(300U, 0xFFU);
    fresh(0, 1);
    run_script(100U * P, 0);
    {
      size_t n, bottom = 0U;
      for (n = 0U; n < out_frames; ++n)
        if (out_buf[2U * n] == -32768 && out_buf[2U * n + 1U] == -32768) ++bottom;
      check(bottom > 0U, "eight full-scale negative channels saturate at -32768");
    }
  }
}

static void t_env_pan(void) {
  const uint64_t P = period();
  unsigned i;
  static const uint8_t pans[8] = {0xFF, 0x0F, 0xF0, 0x84, 0x00, 0x11, 0xA5, 0x5A};
  static const uint8_t envs[8] = {255, 255, 255, 129, 255, 1, 0, 77};
  uint8_t pos[8];
  for (i = 0; i < 7U; ++i) pos[i] = (uint8_t)(0x80U | (20U + i * 15U));
  pos[7] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, pos, 8U);
  for (i = 0; i < 8U; ++i) {
    chan(200U + i, i, envs[i], pans[i], 0x0800, 0, 0);
    onoff(1000U * (i + 1U), (1U << (i + 1U)) - 1U); /* one more channel ON each time */
  }
  script_case("ENV and PAN: left nibble, right nibble, both, none, ENV 0/1 (one channel at a time, then together)", 400U * P, 0, 1);
  /* panning is independent per side: PAN $0F is left only, $F0 right only */
  {
    size_t n;
    int left_only = 1, right_only = 1;
    n_ops = 0U;
    wave(100U, 0, 0, pos, 8U);
    chan(200U, 0, 255, 0x0F, 0x0800, 0, 0);
    onoff(300U, 1U);
    fresh(0, 1);
    run_script(60U * P, 0);
    for (n = 0U; n < out_frames; ++n)
      if (out_buf[2U * n + 1U] != 0) left_only = 0;
    n_ops = 0U;
    wave(100U, 0, 0, pos, 8U);
    chan(200U, 0, 255, 0xF0, 0x0800, 0, 0);
    onoff(300U, 1U);
    fresh(0, 1);
    run_script(60U * P, 0);
    for (n = 0U; n < out_frames; ++n)
      if (out_buf[2U * n] != 0) right_only = 0;
    check(left_only && right_only, "PAN $0F plays the left side only, $F0 the right side only");
  }
}

static void t_onoff_enable(void) {
  const uint64_t P = period();
  uint8_t w[20];
  unsigned i;
  for (i = 0; i < 19U; ++i) w[i] = (uint8_t)(0x80U | (i + 1U));
  w[19] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, w, 20U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  chan(200U, 1, 0xFF, 0xF8, 0x0400, 0, 2);
  onoff(300U, 0x03U);
  onoff(60U * P + 17U, 0x02U);   /* channel 0 OFF: its address reloads from ST, channel 1 continues */
  onoff(100U * P + 3U, 0x03U);   /* channel 0 ON again: restarts from the start address */
  W(140U * P, 7, 0x00U);         /* chip disabled (bank select with bit 7 clear) */
  W(180U * P + 5U, 7, 0x80U);    /* chip enabled again: the channels resume where they stopped */
  onoff(220U * P, 0x00U);        /* everything OFF */
  script_case("channel ON/OFF restart from ST, chip disable mutes without moving, chip enable resumes", 300U * P, 0, 1);
  /* chip disabled from the start: silence, whatever the channels hold */
  n_ops = 0U;
  wave(100U, 0, 0, w, 20U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  W(250U, 7, 0x00U);
  onoff(300U, 0x01U);
  script_case("chip disabled: true silence", 100U * P, 0, 0);
  /* chip enabled, no channel ON: silence */
  n_ops = 0U;
  wave(100U, 0, 0, w, 20U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  W(250U, 7, 0x80U);
  script_case("chip enabled, every channel OFF: silence", 100U * P, 0, 0);
}

static void t_loops(void) {
  const uint64_t P = period();
  uint8_t w[40];
  unsigned i;
  for (i = 0; i < 39U; ++i) w[i] = (uint8_t)(0x80U | (2U * i));
  w[39] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, w, 40U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 10U, 0);   /* loops back to byte 10 */
  chan(200U, 1, 0xFF, 0xFF, 0x0A00, 25U, 0);
  onoff(300U, 0x03U);
  W(100U * P, 7, 0xC0U | 1U);
  W(100U * P, 4, 5U);                          /* the loop address changes while the channel plays */
  script_case("loop marker returns to LS (two channels, LS rewritten while playing)", 400U * P, 0, 1);
  /* a loop onto a loop marker outputs nothing and does not run away */
  n_ops = 0U;
  {
    const uint8_t dead[2] = {0xFFU, 0xFFU};
    wave(100U, 0, 0, dead, 2U);
    chan(200U, 0, 0xFF, 0xFF, 0x0800, 1U, 0);
    onoff(300U, 0x01U);
    script_case("a loop onto a loop marker is silent", 100U * P, 0, 0);
  }
  /* the pointer ends where the SEG-102 model says it does: unchanged by the audio */
  {
    uint32_t with_mix[16], without_mix[16];
    unsigned o;
    n_ops = 0U;
    wave(100U, 0, 0, w, 40U);
    chan(200U, 0, 0xFF, 0xFF, 0x0800, 10U, 0);
    onoff(300U, 0x01U);
    fresh(0, 1);
    run_script(137U * P + 11U, 0);
    for (o = 0; o < 16U; ++o) with_mix[o] = pointer_read(0x10U + o);
    fresh(0, 0);
    run_script(137U * P + 11U, 0);
    for (o = 0; o < 16U; ++o) without_mix[o] = pointer_read(0x10U + o);
    check(memcmp(with_mix, without_mix, sizeof with_mix) == 0, "CPU-visible wave pointers are identical with the audio on and off");
  }
}

static void t_wave_rewrite_reset_clock(void) {
  const uint64_t P = period();
  uint8_t w[30];
  unsigned i;
  for (i = 0; i < 29U; ++i) w[i] = (uint8_t)(0x80U | (i + 40U));
  w[29] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  /* the wave RAM under the playing pointer is rewritten at instants on and next to sample-clock events */
  for (i = 0; i < 12U; ++i) {
    const uint64_t t = (20U + 3U * i) * P + (i % 3U == 0U ? 0U : i % 3U == 1U ? 1U : (uint64_t)-1);
    W(t, 0x1000U + 21U + i, 0x80U | (100U - i));
  }
  script_case("wave RAM rewritten while playing, at a clock instant, one tick after and one tick before", 100U * P, 0, 1);
  /* a bank switch changes where later writes land, not what is playing */
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  W(30U * P, 7, 0x80U | 2U);
  W(30U * P, 0x1000U + 3U, 0xFFU);
  script_case("bank select moves the write window only", 80U * P, 0, 1);
  /* reset mid-stream: the chip, its wave RAM and the sample grid restart at the instant */
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  RESET(50U * P + 123U);
  wave(60U * P, 0, 0, w, 30U);
  chan(60U * P + 1U, 0, 0xFF, 0xF1, 0x0400, 0, 0);
  onoff(60U * P + 2U, 0x01U);
  script_case("peripheral reset clears the chip and restarts the sample grid", 150U * P, 0, 1);
  /* sample-clock edges: an ENV change one tick before, on and one tick after an event */
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  W(40U * P - 1U, 7, 0xC0U);
  W(40U * P - 1U, 0, 200U);
  W(41U * P, 7, 0xC0U);
  W(41U * P, 0, 100U);
  W(42U * P + 1U, 7, 0xC0U);
  W(42U * P + 1U, 0, 50U);
  script_case("ENV changed one tick before, on, and one tick after a sample instant", 60U * P, 0, 1);
  /* a sample event that falls exactly on an output frame's instant is heard by that frame (the "latest event at or before" rule): reset the grid so
   * that event 5 coincides with frame 2000 and let the waveform change at every event */
  {
    const uint64_t t0 = frame_time(2000U) - 5U * P;
    n_ops = 0U;
    RESET(t0);
    wave(t0, 0, 0, w, 30U);
    chan(t0, 0, 0xFF, 0xFF, 0x0800, 0, 0);
    onoff(t0, 0x01U);
    script_case("a sample event exactly on an output frame's instant is heard by that frame", frame_time(2000U) + 20U * P, 0, 1);
  }
}

static void t_long(void) {
  const uint64_t P = period();
  uint8_t w[64];
  unsigned i;
  for (i = 0; i < 63U; ++i) w[i] = (uint8_t)(((i * 7U) & 1U ? 0x80U : 0x00U) | ((i * 11U) & 0x7EU));
  w[63] = 0xFFU;
  n_ops = 0U;
  wave(100U, 0, 0, w, 64U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  chan(200U, 1, 0xB0, 0x3C, 0x0733, 8, 0);
  onoff(300U, 0x03U);
  /* ten guest seconds: 440,000 frames through a queue-less, chunked path */
  script_case("ten seconds of guest time: bounded chunked output equals the reference", (uint64_t)(SCD_TICKS_PER_SECOND * 10U), 2, 1);
  check(out_frames == 441000U, "ten seconds produce 441,000 frames (+-)");
  (void)P;
}

/* ---- CD-DA and mixing -------------------------------------------------------------------------------------------------------------- */
static RefCd ref_cd(int32_t cap) {
  RefCd cd;
  cd.arr = arrivals;
  cd.n_arr = n_arrivals;
  cd.cap = cap;
  cd.stop = stop_tick;
  cd.fader_tick = UINT64_MAX;
  cd.fader_target = 1024U;
  return cd;
}

static size_t differing(const int16_t *a, const int16_t *b, size_t frames) {
  size_t n, bad = 0U;
  for (n = 0U; n < 2U * frames; ++n)
    if (a[n] != b[n]) ++bad;
  return bad;
}

static void t_mixing(void) {
  const uint64_t P = period();
  uint8_t w[30];
  unsigned i;
  size_t ref_n, bad;
  RefCd cd;
  for (i = 0; i < 29U; ++i) w[i] = (uint8_t)(0x80U | (i * 4U));
  w[29] = 0xFFU;
  /* CD-DA only, mix on: one run at the DAC rate on the guest timeline (silence before it); the drive tick is slower than 588 frames, so the queue is
   * refilled from the disc ahead of it and nothing is lost or heard twice */
  n_ops = 0U;
  fresh(1, 1);
  cdd_command(3U, T2);
  drive_until(machine->now + 60U * SCD_CDD_TICK_TICKS);
  scd_mix_advance(machine, machine->now);
  cd = ref_cd(T2 + T2_N);
  ref_n = ref_render(machine->now, ref_buf, &cd);
  bad = ref_n == out_frames ? differing(ref_buf, out_buf, ref_n) : 1U;
  check(ref_n == out_frames && bad == 0U && n_arrivals >= 8U, "CD-DA only: the mixed stream is one continuous run on the guest timeline, equal to the reference");
  check(legacy_frames == 588U * n_arrivals, "the legacy sink still sees every drive-delivered sector, unchanged");
  check(machine->mix.cdda_gaps == 0U && machine->mix.cdda_dropped == 0U && machine->mix.cdda_readahead > 0U && machine->mix.cdda_skipped > 0U,
        "no gap and no drop; the slower drive tick is covered by read-ahead and its later deliveries are not queued twice");
  /* a run into the end of its track: nothing past the track is read ahead (the INDEX 00 pregap of the next track is never heard) */
  fresh(1, 1);
  n_ops = 0U;
  cdd_command(3U, T4); /* the last audio track: a data track follows */
  drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
  scd_mix_advance(machine, machine->now);
  cd = ref_cd(T4 + T4_N);
  ref_n = ref_render(machine->now, ref_buf, &cd);
  bad = ref_n == out_frames ? differing(ref_buf, out_buf, ref_n) : 1U;
  check(ref_n == out_frames && bad == 0U && stop_tick != UINT64_MAX, "a run to the end of its track equals the reference and stops at the track end");
  /* PCM only while CD-DA is stopped */
  fresh(1, 1);
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  run_script(300U * P, 0);
  ref_n = ref_render(300U * P, ref_buf, NULL);
  check(ref_n == out_frames && memcmp(ref_buf, out_buf, 4U * ref_n) == 0 && machine->mix.pcm_frames > 0U, "PCM only, CD-DA enabled but stopped: exactly the PCM reference");
  /* both, with the PCM channel stopped and started inside the run; then the drive is stopped and the run ends */
  fresh(1, 1);
  n_ops = 0U;
  wave(100U, 0, 0, w, 30U);
  chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
  onoff(300U, 0x01U);
  W(25U * SCD_CDD_TICK_TICKS + SCD_CDD_TICK_TICKS / 2U, 8, 0xFFU);
  W(30U * SCD_CDD_TICK_TICKS + SCD_CDD_TICK_TICKS / 7U, 8, 0xFEU);
  {
    unsigned oi = 0U, stopped = 0U;
    const uint64_t end = 60U * SCD_CDD_TICK_TICKS;
    cdd_command(3U, T2);
    while (machine->now < end && machine->stop.stop_class == SCD_STOP_NONE) {
      const uint64_t next_op = oi < n_ops ? ops[oi].t : UINT64_MAX;
      const uint64_t next_dev = scd_devices_next_event(machine);
      if (!stopped && machine->now >= 45U * SCD_CDD_TICK_TICKS) {
        cdd_command(1U, 0); /* the drive stops: the run ends within the sector in progress */
        stopped = 1U;
      }
      if (next_op <= next_dev && next_op < end) {
        machine->now = next_op > machine->now ? next_op : machine->now;
        chip_write(ops[oi].reg, ops[oi].value);
        ++oi;
      } else if (next_dev < end) {
        machine->now = next_dev;
        scd_devices_advance(machine, next_dev);
        watch_playing();
      } else {
        break;
      }
    }
    machine->now = end;
    scd_mix_advance(machine, end);
    cd = ref_cd(T2 + T2_N);
    ref_n = ref_render(end, ref_buf, &cd);
    bad = ref_n == out_frames ? differing(ref_buf, out_buf, ref_n) : 1U;
    check(ref_n == out_frames && bad == 0U && stop_tick != UINT64_MAX && machine->mix.pcm_frames > 0U && machine->mix.cdda_mixed > 0U,
          "PCM and CD-DA mixed on one timeline: equal to the reference, PCM stopping and starting during the run, the run ending at the stop");
    if (bad != 0U) printf("      %zu differing samples of %zu frames\n", bad, ref_n);
    check(machine->mix.cdda_gaps == 0U, "no silence gap inside the run");
  }
  /* the CD-DA fader inside the mix: a register write moves the volume one step per CD-DA frame toward the target, from the instant of the write */
  fresh(1, 1);
  n_ops = 0U;
  cdd_command(3U, T2);
  drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
  sub_write(0x34U, 2U, 0x2000U); /* target 0x200 = 512 */
  {
    const uint64_t write_tick = machine->now;
    drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
    scd_mix_advance(machine, machine->now);
    cd = ref_cd(T2 + T2_N);
    cd.fader_tick = write_tick;
    cd.fader_target = 512U;
    ref_n = ref_render(machine->now, ref_buf, &cd);
    bad = ref_n == out_frames ? differing(ref_buf, out_buf, ref_n) : 1U;
    check(ref_n == out_frames && bad == 0U && machine->mix.fader_volume == 512U, "the fader ramps per CD-DA frame inside the mix from the instant of the register write");
  }
}

/* ---- the guest sees nothing of the audio ---------------------------------------------------------------------------------------- */
static void t_guest_independence(void) {
  uint64_t log_on, log_off, now_on, now_off, ret_on, ret_off;
  uint32_t lba_on, lba_off, ptr_on[16], ptr_off[16];
  unsigned o;
  uint8_t w[20];
  unsigned i;
  const uint64_t P = period();
  for (i = 0; i < 19U; ++i) w[i] = (uint8_t)(0x80U | (i + 3U));
  w[19] = 0xFFU;
  for (o = 0; o < 2U; ++o) {
    fresh(1, (int)o);
    n_ops = 0U;
    wave(100U, 0, 0, w, 20U);
    chan(200U, 0, 0xFF, 0xFF, 0x0800, 0, 0);
    onoff(300U, 0x01U);
    for (i = 0; i < n_ops; ++i) chip_write(ops[i].reg, ops[i].value);
    cdd_command(3U, T2 + 8);
    drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS + 5U * P);
    {
      unsigned q;
      uint32_t *dst = o ? ptr_on : ptr_off;
      for (q = 0; q < 16U; ++q) dst[q] = pointer_read(0x10U + q);
    }
    if (o) {
      log_on = machine->device_log;
      now_on = machine->now;
      ret_on = machine->cpu[0].retired + machine->cpu[1].retired;
      lba_on = (uint32_t)machine->cdd.lba;
    } else {
      log_off = machine->device_log;
      now_off = machine->now;
      ret_off = machine->cpu[0].retired + machine->cpu[1].retired;
      lba_off = (uint32_t)machine->cdd.lba;
    }
  }
  check(memcmp(ptr_on, ptr_off, sizeof ptr_on) == 0, "pointer reads equal with the mix on and off");
  check(log_on == log_off && now_on == now_off && ret_on == ret_off && lba_on == lba_off, "device log, time, retirement and drive position equal with the mix on and off");
  /* a refusing sink changes nothing */
  {
    uint64_t log_ok, log_refused, now_ok;
    fresh(1, 1);
    cdd_command(3U, T2 + 8);
    drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
    log_ok = machine->device_log;
    now_ok = machine->now;
    fresh(1, 1);
    machine->mix.sink = refuse_put;
    cdd_command(3U, T2 + 8);
    drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
    log_refused = machine->device_log;
    check(machine->mix.sink_refused > 0U && log_refused == log_ok && machine->now == now_ok, "a refusing mix sink is only counted: the machine is unaffected");
    fresh(1, 1);
    machine->mix.sink = NULL;
    cdd_command(3U, T2 + 8);
    drive_until(machine->now + 30U * SCD_CDD_TICK_TICKS);
    check(machine->device_log == log_ok && machine->mix.frames > 0U, "no mix sink: the timeline still runs and the machine is unaffected");
  }
}

int main(void) {
  build_disc();
  machine = (ScdMachine *)calloc(1U, sizeof *machine);
  out_buf = (int16_t *)calloc(2U * MAXF, sizeof(int16_t));
  ref_buf = (int16_t *)calloc(2U * MAXF, sizeof(int16_t));
  smp = (Smp *)calloc(420000U, sizeof(Smp));
  if (machine == NULL || out_buf == NULL || ref_buf == NULL || smp == NULL) return 1;
  if (scd_disc_open(&disc, ".") != SCD_DISC_OK) {
    printf("FAIL  disc open\n");
    return 1;
  }
  make_waves();
  t_single_channel();
  t_multi_and_saturation();
  t_env_pan();
  t_onoff_enable();
  t_loops();
  t_wave_rewrite_reset_clock();
  t_long();
  t_mixing();
  t_guest_independence();
  scd_machine_release(machine);
  scd_disc_close(&disc);
  free(machine);
  free(out_buf);
  free(ref_buf);
  free(smp);
  remove(name0);
  remove(name1);
  printf(failures == 0 ? "sega_cd_pcm_audio_test: all checks passed\n" : "sega_cd_pcm_audio_test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
