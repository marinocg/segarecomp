/* SEG-070-T002 (ADR 0149): table-driven checks of the timed standard-pad machine input, driven through the runtime's own bus ABI
 * (scd_read / scd_write, exactly as generated code issues the accesses) and its device-event interface. Every expected value is
 * derived by hand from the documented 3-button pad protocol (Sega Genesis hardware notes; GTO1 I/O section) and the script grammar of
 * ADR 0149, never copied from the implementation:
 *   DATA read, bit 7 = the data latch; bit 6 = TH (the latch when CTRL bit 6 is an output, 1 - pulled up - when it is an input);
 *   bits 5-0 = the pad lines, 0 = pressed, when their CTRL bit is an input (the latch otherwise);
 *   TH = 1: bits 5-0 = C B R L D U        TH = 0: bits 5-0 = Start A 0 0 D U.
 * Released: TH 1 -> $7F (bit 6 plus 111111), TH 0 -> $33 (110011). One tick is 1/500,000 master clock; the machine's time is the
 * `now` the table sets, the device events are processed exactly as scd_machine_run does (next event, then advance). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"
#include "runtime.h"

static int failures;
static ScdMachine *m;
static uint8_t bios[0x20000];

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
  m->vdp_onset = UINT64_C(3420) * 224U * 500000U * 6U; /* the first vertical-blank onset is far beyond every tick below */
}

static int install(const char *text) {
  const char *error = NULL;
  unsigned line = 0U;
  return scd_input_install(m, text, strlen(text), 1, &error, &line);
}

/* Process every device event up to and including `t`, then stand at `t` (the run loop's order: a device event at or before a
 * processor's ready time runs first). */
static void run_to(uint64_t t) {
  for (;;) {
    const uint64_t e = scd_devices_next_event(m);
    if (e > t) break;
    m->now = e;
    scd_devices_advance(m, e);
  }
  m->now = t;
}

static uint32_t io_read(uint32_t address, uint32_t width) {
  uint32_t v = 0xDEADU;
  if (scd_read(m, SCD_MAIN, address, width, &v, 0xFF0100U) != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
    return 0xFFFFFFFFU;
  }
  return v;
}
static int io_write(uint32_t address, uint32_t width, uint32_t value) {
  const int s = scd_write(m, SCD_MAIN, address, width, value, 0xFF0100U);
  if (s != 0) m->stop.stop_class = SCD_STOP_NONE;
  return s;
}

#define DATA1 0xA10003U
#define DATA2 0xA10005U
#define CTRL1 0xA10009U
#define CTRL2 0xA1000BU

/* TH-driving read of a port: the standard sequence a game uses */
static uint32_t th_read(uint32_t data, int th) {
  io_write(data, 1U, th ? 0x40U : 0x00U);
  return io_read(data, 1U);
}

static void expect(uint32_t got, uint32_t want, const char *label) {
  char text[240];
  snprintf(text, sizeof text, "%s: $%02X (want $%02X)", label, got, want);
  row(got == want, text);
}

/* parse a script and compare its refusal class and line */
static void refuse(const char *text, size_t length, const char *want_class, unsigned want_line, const char *label) {
  static ScdInputEvent events[SCD_INPUT_MAX_EVENTS];
  size_t count = 0U;
  const char *error = NULL;
  unsigned line = 0U;
  const int rc = scd_input_parse(text, length, events, &count, &error, &line);
  char out[300];
  snprintf(out, sizeof out, "refuse %s -> %s line %u (want %s line %u)", label, error != NULL ? error : "ok", line, want_class, want_line);
  row(rc != 0 && error != NULL && strcmp(error, want_class) == 0 && line == want_line && count == 0U, out);
}
static void refuse_s(const char *text, const char *want_class, unsigned want_line, const char *label) {
  refuse(text, strlen(text), want_class, want_line, label);
}

int main(void) {
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* ---- no script: all released, nothing scheduled, nothing observed ---- */
  fresh();
  io_write(CTRL1, 1U, 0x40U);
  io_write(CTRL2, 1U, 0x40U);
  expect(th_read(DATA1, 1), 0x7FU, "no script: port 1 TH high, everything released");
  expect(th_read(DATA1, 0), 0x33U, "no script: port 1 TH low, everything released");
  expect(th_read(DATA2, 1), 0x7FU, "no script: port 2 TH high");
  expect(th_read(DATA2, 0), 0x33U, "no script: port 2 TH low");
  row(m->input == NULL && scd_input_next_event(m) == UINT64_MAX, "no script: no input state, no input event");
  row(scd_devices_next_event(m) >= UINT64_C(0x1000000000), "no script: the first device event is the vertical-blank onset, far from tick 1000");

  /* ---- every button, both TH states (hand-derived), alone ---- */
  {
    static const struct { const char *name; uint32_t high, low; } b[] = {
        {"UP", 0x7EU, 0x32U}, {"DOWN", 0x7DU, 0x31U}, {"LEFT", 0x7BU, 0x33U}, {"RIGHT", 0x77U, 0x33U},
        {"B", 0x6FU, 0x33U},  {"C", 0x5FU, 0x33U},    {"A", 0x7FU, 0x23U},    {"START", 0x7FU, 0x13U}};
    unsigned i;
    for (i = 0; i < 8U; ++i) {
      char script[120], label[120];
      fresh();
      snprintf(script, sizeof script, "segarecomp-input-script 1\n100 1 press %s\n", b[i].name);
      row(install(script) == 0, "install single press");
      io_write(CTRL1, 1U, 0x40U);
      run_to(99U);
      expect(th_read(DATA1, 1), 0x7FU, "before the press, TH high");
      run_to(100U);
      snprintf(label, sizeof label, "%s pressed, TH high", b[i].name);
      expect(th_read(DATA1, 1), b[i].high, label);
      snprintf(label, sizeof label, "%s pressed, TH low", b[i].name);
      expect(th_read(DATA1, 0), b[i].low, label);
      snprintf(label, sizeof label, "%s pressed on port 1 leaves port 2 released (TH high)", b[i].name);
      io_write(CTRL2, 1U, 0x40U);
      expect(th_read(DATA2, 1), 0x7FU, label);
    }
  }

  /* ---- the timing of a press/release pair, at and around the exact transition ticks ---- */
  fresh();
  row(install("segarecomp-input-script 1\n1000 1 press START\n3000 1 release START\n") == 0, "install START press 1000 / release 3000");
  io_write(CTRL1, 1U, 0x40U);
  row(scd_input_next_event(m) == 1000U, "the first event is due at 1000");
  expect(th_read(DATA1, 0), 0x33U, "tick 0: released");
  run_to(999U);
  expect(th_read(DATA1, 0), 0x33U, "tick 999 (one tick before the press): released");
  run_to(1000U);
  expect(th_read(DATA1, 0), 0x13U, "tick 1000 (exactly at the press): pressed - the event precedes the access at its own tick");
  row(scd_input_next_event(m) == 3000U, "after the press the next event is the release at 3000");
  run_to(2999U);
  expect(th_read(DATA1, 0), 0x13U, "tick 2999: still pressed");
  expect(th_read(DATA1, 1), 0x7FU, "tick 2999: TH high does not show START");
  run_to(3000U);
  expect(th_read(DATA1, 0), 0x33U, "tick 3000 (exactly at the release): released");
  row(scd_input_next_event(m) == UINT64_MAX && m->input->next == 2U && m->input->pad[0] == 0U, "all events applied, pad state released");

  /* ---- an event at tick 0, and a late first read ---- */
  fresh();
  row(install("segarecomp-input-script 1\n0 1 press START\n") == 0, "install press at tick 0");
  io_write(CTRL1, 1U, 0x40U);
  run_to(0U);
  expect(th_read(DATA1, 0), 0x13U, "an event at tick 0 is applied before the first access");

  /* ---- two events at the same tick: both applied, in file order, before any access at that tick ---- */
  fresh();
  row(install("segarecomp-input-script 1\n5000 1 press START\n5000 1 press A\n5000 2 press UP\n") == 0, "install three events at tick 5000");
  io_write(CTRL1, 1U, 0x40U);
  io_write(CTRL2, 1U, 0x40U);
  run_to(4999U);
  expect(th_read(DATA1, 0), 0x33U, "tick 4999: nothing yet (port 1 TH low)");
  run_to(5000U);
  expect(th_read(DATA1, 0), 0x03U, "tick 5000: START and A together (TH low: 110011 minus bits 5 and 4)");
  expect(th_read(DATA2, 1), 0x7EU, "tick 5000: port 2 UP in the same instant (TH high)");
  row(m->input->next == 3U, "all three same-tick events applied by one device step");

  /* ---- buttons changing between reads; the same instant releases one button and presses another ---- */
  fresh();
  row(install("# a comment\n\nsegarecomp-input-script 1\n7000 1 press UP\n8000 1 release UP\n8000 1 press DOWN\r\n9000 1 release DOWN\n") == 0,
      "install with comments, a blank line and a CRLF ending");
  io_write(CTRL1, 1U, 0x40U);
  run_to(6999U);
  expect(th_read(DATA1, 1), 0x7FU, "6999: released");
  run_to(7500U);
  expect(th_read(DATA1, 1), 0x7EU, "7500: UP");
  run_to(8000U);
  expect(th_read(DATA1, 1), 0x7DU, "8000: UP released and DOWN pressed at the same tick: DOWN only");
  run_to(9000U);
  expect(th_read(DATA1, 1), 0x7FU, "9000: released");

  /* ---- data direction and control register semantics ---- */
  fresh();
  row(install("segarecomp-input-script 1\n10 1 press START\n10 1 press UP\n") == 0, "install START+UP");
  run_to(10U);
  io_write(CTRL1, 1U, 0x00U); /* every line an input: TH floats high */
  io_write(DATA1, 1U, 0x00U);
  expect(io_read(DATA1, 1U), 0x7EU, "CTRL 0 (all inputs), DATA 0: TH reads high regardless of the latch, UP visible, START not");
  io_write(CTRL1, 1U, 0x7FU); /* every line an output: the latch comes back */
  io_write(DATA1, 1U, 0x55U);
  expect(io_read(DATA1, 1U), 0x55U, "CTRL $7F (all outputs), DATA $55: the latch, buttons invisible");
  io_write(DATA1, 1U, 0xD5U);
  expect(io_read(DATA1, 1U), 0xD5U, "CTRL $7F, DATA $D5: bit 7 is the latch too");
  io_write(CTRL1, 1U, 0x40U);
  io_write(DATA1, 1U, 0xC0U);
  expect(io_read(DATA1, 1U), 0xFEU, "CTRL $40, DATA $C0: bit 7 latch 1, TH 1, pad bits 111110 (UP)");
  io_write(DATA1, 1U, 0x80U);
  expect(io_read(DATA1, 1U), 0x92U, "CTRL $40, DATA $80: bit 7 latch 1, TH 0, START and UP: 010010");
  io_write(CTRL1, 1U, 0x70U);   /* TH, C and B lines driven */
  io_write(DATA1, 1U, 0x40U);   /* TH 1; C and B latches 0 */
  expect(io_read(DATA1, 1U), 0x4EU, "CTRL $70, DATA $40: bits 6-4 echo the latch (100), bits 3-0 from the pad (1110, UP)");
  expect(io_read(CTRL1, 1U), 0x70U, "the control register reads back what was written");

  /* ---- a WORD read reaches the data port as its byte (both lanes) and is observed once ---- */
  fresh();
  row(install("segarecomp-input-script 1\n10 1 press START\n") == 0, "install");
  run_to(10U);
  io_write(CTRL1, 1U, 0x40U);
  io_write(DATA1, 1U, 0x00U);
  expect(io_read(0xA10002U, 2U), 0x1313U, "word read of data port 1, TH low, START: the byte on both lanes");
  row(m->input->reads[0] == 1U && m->input->log_count == 1U && m->input->log[0].value == 0x13U && m->input->log[0].tick == 10U &&
          m->input->log[0].port == 1U,
      "the word read is observed exactly once, with its value, port and tick");
  expect(io_read(DATA2, 1U), 0x7FU, "a port 2 read (CTRL 0: TH floats high), nothing pressed on port 2");
  row(m->input->reads[1] == 1U && m->input->log_count == 2U && m->input->log[1].port == 2U, "port 2 read counted separately");

  /* ---- unsupported hardware access stays a typed stop, with a button pressed and observation on: the even-address BYTE lane of a
   * data register, a byte write to the version register and the unmodelled register block above the pad ports are not pad reads ---- */
  fresh();
  row(install("segarecomp-input-script 1\n10 1 press START\n") == 0, "install");
  run_to(10U);
  {
    static const struct { uint32_t address, width; int write; } bad[] = {
        {0xA10002U, 1U, 0}, {0xA10004U, 1U, 0}, {0xA10002U, 1U, 1}, {0xA10001U, 1U, 1}, {0xA10010U, 1U, 0}, {0xA10011U, 1U, 0}};
    unsigned i;
    for (i = 0U; i < sizeof bad / sizeof bad[0]; ++i) {
      uint32_t v = 0U;
      char label[140];
      const int st = bad[i].write ? scd_write(m, SCD_MAIN, bad[i].address, bad[i].width, 0x40U, 0xFF0100U)
                                  : scd_read(m, SCD_MAIN, bad[i].address, bad[i].width, &v, 0xFF0100U);
      snprintf(label, sizeof label, "%s.%u $%06X is the typed stop main_device_unsupported at that address, not an observation",
               bad[i].write ? "write" : "read", bad[i].width, bad[i].address);
      row(st != 0 && m->stop.stop_class == SCD_STOP_MAIN_DEVICE_UNSUPPORTED && m->stop.address == bad[i].address &&
              m->stop.width == bad[i].width && m->stop.is_write == (uint32_t)bad[i].write && m->input->reads[0] == 0U &&
              m->input->reads[1] == 0U,
          label);
      m->stop.stop_class = SCD_STOP_NONE;
    }
  }

  /* ---- the transition timeline is independent of how the run is partitioned ---- */
  {
    static const uint64_t stride[] = {1U, 7U, 333U, 999U, 1000U, 4096U, 100000U, 0U};
    unsigned s;
    uint8_t reference[11];
    for (s = 0; s < sizeof stride / sizeof stride[0]; ++s) {
      uint64_t t;
      uint8_t seen[11];
      unsigned k = 0U;
      fresh();
      row(install("segarecomp-input-script 1\n1000 1 press START\n3000 1 release START\n3000 1 press A\n5000 1 release A\n") == 0, "install");
      io_write(CTRL1, 1U, 0x40U);
      for (t = 0U; t <= 6000U; t += 600U) { /* the sample instants */
        uint64_t at = 0U;
        if (stride[s] == 0U) run_to(t); /* the exact-event stepping */
        else { /* arbitrary advance instants: scd_devices_advance at every stride-th tick, then the sample */
          for (at = 0U; at <= t; at += stride[s]) { /* spurious advance instants are harmless: only due events act */
            m->now = at;
            scd_devices_advance(m, at);
          }
          run_to(t);
        }
        seen[k++] = (uint8_t)th_read(DATA1, 0);
      }
      if (s == 0U) memcpy(reference, seen, sizeof reference);
      {
        char label[100];
        snprintf(label, sizeof label, "partition stride %llu: the sampled pad timeline equals stride 1", (unsigned long long)stride[s]);
        row(memcmp(reference, seen, sizeof seen) == 0, label);
      }
    }
    /* the timeline itself, derived by hand: 0,600 released; 1200..2400 START; 3000 A (START released at the same tick); 3600,4200,4800 A;
     * 5400,6000 released. Samples at 0,600,...,6000 (11). TH low: released $33, START $13, A $23. */
    {
      static const uint8_t want[11] = {0x33, 0x33, 0x13, 0x13, 0x13, 0x23, 0x23, 0x23, 0x23, 0x33, 0x33};
      row(memcmp(reference, want, sizeof want) == 0, "the hand-derived timeline: released, START 1000-2999, A 3000-4999, released");
    }
  }

  /* ---- script validation: every refusal class, with its line; nothing is applied ---- */
  refuse_s("", "missing_header", 0U, "an empty script");
  refuse_s("# only a comment\n", "missing_header", 0U, "no header at all");
  refuse_s("100 1 press START\n", "missing_header", 1U, "an event before the header");
  refuse_s("segarecomp-input-script 2\n", "bad_header", 1U, "an unknown version");
  refuse_s("segarecomp-input-script  1\n", "bad_header", 1U, "a double space in the header");
  refuse_s("segarecomp-input-script 1\n100 3 press START\n", "bad_port", 2U, "port 3");
  refuse_s("segarecomp-input-script 1\n100 0 press START\n", "bad_port", 2U, "port 0");
  refuse_s("segarecomp-input-script 1\n100 12 press START\n", "bad_port", 2U, "a two-digit port");
  refuse_s("segarecomp-input-script 1\n100 1 push START\n", "bad_action", 2U, "an unknown action");
  refuse_s("segarecomp-input-script 1\n100 1 PRESS START\n", "bad_action", 2U, "an upper-case action");
  refuse_s("segarecomp-input-script 1\n100 1 press SELECT\n", "bad_button", 2U, "an unknown button");
  refuse_s("segarecomp-input-script 1\n100 1 press start\n", "bad_button", 2U, "a lower-case button");
  refuse_s("segarecomp-input-script 1\n100 1 press MODE\n", "unsupported_button", 2U, "MODE");
  refuse_s("segarecomp-input-script 1\n100 1 press X\n", "unsupported_button", 2U, "X");
  refuse_s("segarecomp-input-script 1\n100 1 press Y,UP\n", "unsupported_button", 2U, "Y in a list");
  refuse_s("segarecomp-input-script 1\n100 1 press Z\n", "unsupported_button", 2U, "Z");
  refuse_s("segarecomp-input-script 1\n100 1 press UP,UP\n", "duplicate_button", 2U, "a repeated button");
  refuse_s("segarecomp-input-script 1\n100 1 press UP,\n", "bad_button", 2U, "a trailing comma");
  refuse_s("segarecomp-input-script 1\n100 1 press ,UP\n", "bad_button", 2U, "a leading comma");
  refuse_s("segarecomp-input-script 1\nx 1 press UP\n", "bad_tick", 2U, "a non-numeric tick");
  refuse_s("segarecomp-input-script 1\n-1 1 press UP\n", "bad_tick", 2U, "a negative tick");
  refuse_s("segarecomp-input-script 1\n0x10 1 press UP\n", "bad_tick", 2U, "a hexadecimal tick");
  refuse_s("segarecomp-input-script 1\n9223372036854775808 1 press UP\n", "tick_range", 2U, "a tick above 2^63-1");
  refuse_s("segarecomp-input-script 1\n99999999999999999999999 1 press UP\n", "tick_range", 2U, "a tick above 2^64");
  refuse_s("segarecomp-input-script 1\n100 1 press\n", "bad_event", 2U, "a missing button field");
  refuse_s("segarecomp-input-script 1\n100  1 press UP\n", "bad_event", 2U, "a double space between fields");
  refuse_s("segarecomp-input-script 1\n100 1 press UP \n", "bad_event", 2U, "a trailing space");
  refuse_s("segarecomp-input-script 1\n100 1 press UP extra\n", "bad_event", 2U, "a fifth field");
  refuse_s("segarecomp-input-script 1\n100\t1 press UP\n", "bad_event", 2U, "a tab separator");
  refuse_s("segarecomp-input-script 1\n200 1 press UP\n100 1 release UP\n", "tick_order", 3U, "a decreasing tick");
  refuse_s("segarecomp-input-script 1\n100 1 press UP\n200 1 press UP\n", "redundant_transition", 3U, "pressing a held button");
  refuse_s("segarecomp-input-script 1\n100 1 release UP\n", "redundant_transition", 2U, "releasing a released button");
  refuse_s("segarecomp-input-script 1\n100 1 press UP\n200 1 release UP,DOWN\n", "redundant_transition", 3U, "releasing a list with a released button");
  refuse_s("segarecomp-input-script 1\n100 1 press UP\n100 1 release UP\n", "same_tick_conflict", 3U, "one button touched twice in one tick");
  refuse_s("segarecomp-input-script 1\n100 1 press UP,DOWN\n100 1 press DOWN\n", "same_tick_conflict", 3U, "a listed button touched again in one tick");
  {
    static const char nul[] = "segarecomp-input-script 1\n100 1 press UP\n\0\n";
    refuse(nul, sizeof nul - 1U, "binary", 0U, "a NUL byte");
  }
  {
    static char big[SCD_INPUT_MAX_SCRIPT_BYTES + 2U];
    memset(big, '#', sizeof big);
    big[0] = '\n';
    refuse(big, sizeof big - 1U, "too_large", 0U, "a script above 64 KiB");
  }
  {
    static char longline[SCD_INPUT_MAX_LINE + 40U];
    memset(longline, 'a', sizeof longline);
    memcpy(longline, "segarecomp-input-script 1\n# ", 28U);
    longline[sizeof longline - 2U] = '\n';
    refuse(longline, sizeof longline - 1U, "line_too_long", 2U, "a line above 256 bytes");
  }
  {
    static char text[SCD_INPUT_MAX_SCRIPT_BYTES];
    static ScdInputEvent events[SCD_INPUT_MAX_EVENTS];
    size_t n = 0U, count = 0U, i;
    const char *error = NULL;
    unsigned line = 0U;
    char label[100];
    int rc;
    n += (size_t)snprintf(text + n, sizeof text - n, "segarecomp-input-script 1\n");
    for (i = 0U; i < SCD_INPUT_MAX_EVENTS; ++i) n += (size_t)snprintf(text + n, sizeof text - n, "%zu 1 %s A\n", i, (i & 1U) ? "release" : "press");
    rc = scd_input_parse(text, n, events, &count, &error, &line);
    snprintf(label, sizeof label, "exactly %u events are accepted (got %zu)", SCD_INPUT_MAX_EVENTS, count);
    row(rc == 0 && count == SCD_INPUT_MAX_EVENTS && events[1].tick == 1U && events[1].press == 0U && events[0].port == 1U &&
            events[0].mask == GENESIS_PAD_A,
        label);
    n += (size_t)snprintf(text + n, sizeof text - n, "%u 1 press A\n", SCD_INPUT_MAX_EVENTS);
    refuse(text, n, "too_many_events", SCD_INPUT_MAX_EVENTS + 2U, "one event more than the limit");
  }
  {
    /* a refused script leaves the machine without input state; a later valid one replaces an earlier one */
    const char *error = NULL;
    unsigned line = 0U;
    const char bad[] = "segarecomp-input-script 1\n1 7 press UP\n";
    fresh();
    row(install("segarecomp-input-script 1\n1 1 press UP\n") == 0 && m->input != NULL && m->input->count == 1U, "install a script");
    row(scd_input_install(m, bad, sizeof bad - 1U, 0, &error, &line) != 0 && m->input != NULL && m->input->count == 1U && line == 2U,
        "a refused script does not disturb the installed one");
    row(install("segarecomp-input-script 1\n") == 0 && m->input->count == 0U && scd_input_next_event(m) == UINT64_MAX,
        "a header-only script is valid: no events, observation only");
  }
  scd_input_release(m);
  printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
