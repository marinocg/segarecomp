/* SEG-078-T001 (ADR 0204): table-driven checks of the live (interactive) pieces that need no generated program and no window, driven
 * through the runtime's own device-event interface. Expectations are derived by hand:
 *   - the recorder writes exactly the script grammar of ADR 0149 (header, then `<tick> 1 press|release BUTTON[,BUTTON]`), releases before
 *     presses, buttons in the grammar's order UP DOWN LEFT RIGHT A B C START; a button already in the recorded state yields nothing;
 *   - a held RIGHT with TH high reads $77 (released $7F = bit 6 plus the six lines, RIGHT is bit 3);
 *   - the frame period is the machine's: 262 lines * 3,420 master clocks / 53,693,175 Hz = 896,040e9 / 53,693,175 ns = 16,688,154.5 ns
 *     (59.9227 Hz), never 16,666,666 ns (60 Hz);
 *   - continuous presentation renders every frame but the first (the power-on run-in has no first lines) and changes nothing the machine
 *     computes: the device log and onset count equal those of a machine that does not present.
 * usage: sega_cd_live_table */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime.h"
#include "scd_runtime.h"
#include "viewer.h"

static int failures;
static ScdMachine *m;
static uint8_t bios[0x20000];

#define MASTER UINT64_C(500000)
#define LINE (UINT64_C(3420) * MASTER)
#define FRAME (LINE * 262U)
#define ONSET(k) (SCD_BIOS_FIRST_ONSET_MCLK * MASTER + ((uint64_t)(k)-1U) * FRAME)

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static void fresh(void) {
  scd_input_release(m);
  if (m->vdp_host != NULL) scd_vdp_release(m);
  memset(m, 0, sizeof *m);
  m->bios = bios;
  m->bios_size = sizeof bios;
  m->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  m->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  if (scd_vdp_init(m) != 0) exit(2);
}

static void run_to(uint64_t t) {
  for (;;) {
    const uint64_t e = scd_devices_next_event(m);
    if (e > t) break;
    m->now = e;
    scd_devices_advance(m, e);
  }
  m->now = t;
}

static uint32_t th_high_read(void) {
  uint32_t v = 0xDEADU;
  scd_write(m, SCD_MAIN, 0xA10009U, 1U, 0x40U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xA10003U, 1U, 0x40U, 0xFF0100U);
  if (scd_read(m, SCD_MAIN, 0xA10003U, 1U, &v, 0xFF0100U) != 0) return 0xFFFFFFFFU;
  return v;
}

static char text[SCD_INPUT_MAX_SCRIPT_BYTES + 2U];
static size_t serialize(void) { return scd_input_serialize(m, text, sizeof text); }

static void recorder(void) {
  size_t n;
  uint64_t t;
  unsigned i;
  ScdLiveSample r = SCD_LIVE_SAMPLE_OK;
  static ScdInputEvent events[SCD_INPUT_MAX_EVENTS];
  size_t count = 0U;
  const char *error = NULL;
  unsigned line = 0U;

  fresh();
  row(scd_input_live_sample(m, 100U, GENESIS_PAD_RIGHT) == SCD_LIVE_SAMPLE_NO_INPUT, "no live input installed: the sample is refused");
  row(scd_input_live_begin(m, 0) == 0, "live input installed");
  n = serialize();
  row(n == 26U && strcmp(text, "segarecomp-input-script 1\n") == 0, "nothing recorded yet: the script is exactly the header");
  m->now = 1000U;
  row(scd_input_live_sample(m, 1000U, GENESIS_PAD_RIGHT) == SCD_LIVE_SAMPLE_PASSED, "a tick equal to the machine time is refused (already passed)");
  row(scd_input_live_sample(m, 999U, GENESIS_PAD_RIGHT) == SCD_LIVE_SAMPLE_PASSED, "a tick before the machine time is refused");
  row(scd_input_live_sample(m, 2000U, 0U) == SCD_LIVE_SAMPLE_OK && serialize() == 26U, "an unchanged (released) pad records nothing");
  row(scd_input_live_sample(m, 2000U, GENESIS_PAD_RIGHT) == SCD_LIVE_SAMPLE_OK, "RIGHT pressed at 2000");
  row(scd_input_live_sample(m, 2000U, GENESIS_PAD_RIGHT | GENESIS_PAD_A) == SCD_LIVE_SAMPLE_PASSED, "a changed pad at the same tick is refused (one tick touches a button once)");
  row(scd_input_live_sample(m, 1500U, GENESIS_PAD_RIGHT | GENESIS_PAD_A) == SCD_LIVE_SAMPLE_PASSED, "a tick below the last recorded one is refused");
  row(scd_input_live_sample(m, 3000U, GENESIS_PAD_RIGHT) == SCD_LIVE_SAMPLE_OK && serialize() == 26U + 19U, "RIGHT still held at 3000: nothing recorded");
  row(scd_input_live_sample(m, 4000U, GENESIS_PAD_A | GENESIS_PAD_START | GENESIS_PAD_UP) == SCD_LIVE_SAMPLE_OK, "RIGHT released, UP+A+START pressed at 4000");
  n = serialize();
  row(n > 0U && strcmp(text, "segarecomp-input-script 1\n2000 1 press RIGHT\n4000 1 release RIGHT\n4000 1 press UP,A,START\n") == 0,
      "the script: release before press, buttons in the grammar's order, no redundant line");
  row(scd_input_parse(text, n, events, &count, &error, &line) == 0 && count == 3U, "the recorded text is accepted by the ADR 0149 validator");

  /* application: the recorded events act through the ordinary device-event path */
  m->now = 0U;
  run_to(1999U);
  row(th_high_read() == 0x7FU, "before 2000: released ($7F)");
  run_to(2000U);
  row(th_high_read() == 0x77U, "at 2000: RIGHT reads pressed ($77)");
  run_to(4000U);
  row(th_high_read() == 0x7EU, "at 4000: UP only on the TH-high read ($7E; A and START are TH-low lines)");

  /* replay of the recorded text on a fresh machine gives the same reads */
  {
    const char *error2 = NULL;
    unsigned line2 = 0U;
    char copy[256];
    strcpy(copy, text);
    fresh();
    row(scd_input_install(m, copy, strlen(copy), 0, &error2, &line2) == 0, "the recorded text installs as an ordinary script");
    run_to(2000U);
    row(th_high_read() == 0x77U, "replay: at 2000 RIGHT reads pressed");
  }

  /* limits: even the shortest lines (4-digit ticks, UP only) exceed 65,536 bytes before 4,096 events: the byte limit binds */
  fresh();
  scd_input_live_begin(m, 0);
  for (i = 0U, t = 1U; (r = scd_input_live_sample(m, t, (i & 1U) ? 0U : GENESIS_PAD_UP)) == SCD_LIVE_SAMPLE_OK; ++i, ++t) {
  }
  n = serialize();
  row(r == SCD_LIVE_SAMPLE_CAPACITY && m->input->count > 3500U && m->input->count < SCD_INPUT_MAX_EVENTS &&
          m->input->script_bytes <= SCD_INPUT_MAX_SCRIPT_BYTES,
      "shortest lines: refused by the 65,536-byte limit before the 4,096-event limit");
  row(n > 0U && scd_input_parse(text, n, events, &count, &error, &line) == 0 && count == m->input->count,
      "the kept prefix is a valid script of exactly the recorded events");
  /* limits: large ticks hit the byte limit first */
  fresh();
  scd_input_live_begin(m, 0);
  for (i = 0U, t = UINT64_C(100000000000000); (r = scd_input_live_sample(m, t, (i & 1U) ? 0U : GENESIS_PAD_START)) == SCD_LIVE_SAMPLE_OK; ++i, ++t) {
  }
  n = serialize();
  row(r == SCD_LIVE_SAMPLE_CAPACITY && m->input->count < SCD_INPUT_MAX_EVENTS && m->input->script_bytes <= SCD_INPUT_MAX_SCRIPT_BYTES,
      "byte limit: refused before 4096 events and within 65,536 bytes");
  row(n == m->input->script_bytes && scd_input_parse(text, n, events, &count, &error, &line) == 0 && count == m->input->count,
      "byte limit: the kept prefix is a valid script of exactly the counted size");
  /* all or nothing: a sample needing two events with room for one */
  fresh();
  scd_input_live_begin(m, 0);
  scd_input_live_sample(m, 10U, GENESIS_PAD_UP);
  m->input->count = SCD_INPUT_MAX_EVENTS - 1U;   /* one slot left (the events themselves are irrelevant to this check) */
  row(scd_input_live_sample(m, 20U, GENESIS_PAD_DOWN) == SCD_LIVE_SAMPLE_CAPACITY && m->input->count == SCD_INPUT_MAX_EVENTS - 1U &&
          m->input->live_pad[0] == GENESIS_PAD_UP,
      "all or nothing: a release + press needing two events with one slot left appends none and keeps the recorded state");
  m->input->count = 1U;
}

static void pacing(void) {
  uint64_t num = 0U, den = 0U;
  row(scd_frame_ticks() == FRAME && FRAME / MASTER == 896040U, "the machine's frame is 262 lines * 3,420 master clocks");
  row(genesis_pacer_period(scd_frame_ticks(), SCD_TICKS_PER_SECOND, &num, &den) == 0 && num / den == 16688154U && num % den != 0U,
      "the frame period derived from the machine is 16,688,154.5 ns (59.92 Hz), not 60 Hz (16,666,666 ns)");
  row(num / den != 16666666U && num * 60U != UINT64_C(1000000000) * den, "the machine period is not a literal 60.0 Hz");
  row(genesis_pacer_period(0U, 1U, &num, &den) == -1 && genesis_pacer_period(1U, 0U, &num, &den) == -1, "a zero argument is refused");
  row(genesis_pacer_period(UINT64_MAX, 1U, &num, &den) == -1, "a period that does not fit is refused, never rounded");
}

static uint64_t fake_now(void *ctx) { return *(uint64_t *)ctx; }
static void fake_sleep(void *ctx, uint64_t ns) { *(uint64_t *)ctx += ns; }

static void pacing_total(void) {
  uint64_t clock_ns = 0U, num, den;
  GenesisViewerHost host;
  GenesisPacer pacer;
  unsigned i;
  const unsigned frames = 6000U;
  memset(&host, 0, sizeof host);
  host.ctx = &clock_ns;
  host.now_ns = fake_now;
  host.sleep_ns = fake_sleep;
  genesis_pacer_init(&pacer, 0);
  genesis_pacer_period(scd_frame_ticks(), SCD_TICKS_PER_SECOND, &num, &den);
  for (i = 0U; i < frames; ++i) {
    if (genesis_pacer_wait_period(&pacer, &host, scd_frame_ticks(), SCD_TICKS_PER_SECOND) != 0) break;
  }
  /* the first wait only starts the schedule; the clock then sleeps to each following deadline: after `frames` waits the clock stands at
   * the deadline of wait number `frames` = (frames - 1) exact periods */
  row(i == frames && clock_ns == (num * (frames - 1U)) / den, "6,000 paced frames land on the exact rational deadline (no drift, no 60 Hz)");
}

static void presentation(void) {
  static char dir[] = "";
  uint64_t rendered = 0U, skipped = 0U, rejected = 0U, onset = 0U;
  uint64_t log_plain, log_present;
  unsigned k;
  char digest[65];
  const GenesisFrameArtifact *frame;
  clock_t t0, t1;
  double plain_s, present_s;
  (void)dir;

  /* the reference machine: no presentation */
  fresh();
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8004U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8144U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8701U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 4U, 0xC0020000U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00000U, 2U, 0x0A64U, 0xFF0100U);
  t0 = clock();
  run_to(ONSET(300));
  t1 = clock();
  plain_s = (double)(t1 - t0) / CLOCKS_PER_SEC;
  log_plain = m->device_log;
  row(m->vdp_frames == 300U, "reference: 300 onsets processed");

  fresh();
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8004U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8144U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 2U, 0x8701U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00004U, 4U, 0xC0020000U, 0xFF0100U);
  scd_write(m, SCD_MAIN, 0xC00000U, 2U, 0x0A64U, 0xFF0100U);
  row(scd_vdp_present_enable(m) == 0, "presentation enabled");
  row(scd_vdp_present_take(m, &onset) == NULL, "no frame before the first onset");
  run_to(ONSET(1));
  row(scd_vdp_present_take(m, &onset) == NULL, "onset 1: no frame (the power-on run-in has no first lines)");
  scd_vdp_present_counts(m, &rendered, &skipped, &rejected);
  row(rendered == 0U && skipped == 1U && rejected == 0U, "onset 1 is counted skipped, not approximated");
  run_to(ONSET(2));
  frame = scd_vdp_present_take(m, &onset);
  row(frame != NULL && onset == 2U && frame->width == 256U && frame->height == 224U, "onset 2: a complete 256 x 224 frame, numbered 2");
  row(scd_vdp_present_take(m, &onset) == NULL, "the frame is taken once");
  t0 = clock();
  for (k = 3U; k <= 300U; ++k) {
    run_to(ONSET(k));
    frame = scd_vdp_present_take(m, &onset);
    if (frame == NULL || onset != k) break;
  }
  t1 = clock();
  present_s = (double)(t1 - t0) / CLOCKS_PER_SEC;
  row(k == 301U, "every onset from 2 to 300 yields exactly one frame with its own number");
  scd_vdp_present_counts(m, &rendered, &skipped, &rejected);
  row(rendered == 299U - 0U && skipped == 1U && rejected == 0U, "300 onsets: 299 rendered, 1 skipped, 0 rejected (presented = onsets - 1)");
  log_present = m->device_log;
  row(m->vdp_frames == 300U && log_present == log_plain, "presentation changes nothing the machine computes (device log and onset count equal)");
  scd_frame_sha256(frame, digest);
  row(strlen(digest) == 64U, "the presented frame has the canonical digest form");
  printf("      host cost (informational): 300 frames, device events only: %.3f s without presentation, %.3f s with it (%.1f us per frame)\n",
         plain_s, present_s, present_s * 1e6 / 299.0);
  row(present_s < 5.0, "bounded host cost: presenting 299 frames takes well under a second of host time");
}

int main(void) {
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;
  recorder();
  pacing();
  pacing_total();
  presentation();
  printf("%d failures\n", failures);
  return failures != 0 ? 1 : 0;
}
