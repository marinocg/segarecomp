/* SEG-009-T003 test-only device wiring: stub VDP/PSG/pad objects behind the typed device seams, linked instead of
 * headless/sms_devices_none.c. They implement just enough to exercise the scheduler: the VDP stub raises a frame interrupt
 * at line 192 of every frame and deasserts it on a status read; the V counter returns the last scanline event's line.
 * Every device access and every scanline event is appended to a log written at exit to $SMS_TEST_DEVICE_LOG:
 *   E <absolute line> <T>                     scanline event
 *   R|W <port hex> <T> <last event T> <value> device access at its instruction-start T-state
 * Not a VDP/PSG/pad model: T004/T006/T007 own those. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_machine.h"

#define LOG_CAPACITY (1u << 19)

typedef struct LogEntry {
  char kind;
  uint16_t port;
  uint64_t cycles, event_cycles;
  uint8_t value;
} LogEntry;

static LogEntry log_entries[LOG_CAPACITY];
static uint32_t log_count;
static SmsMachine *machine;
static uint8_t pending;      /* frame flag */
static uint8_t line_pending; /* line flag */
static uint8_t regs[16];     /* R0/R1/R10 only are interpreted */
static uint8_t latch_value, latch_set;
static uint8_t line_counter;
static uint32_t last_line;
static uint64_t last_event_cycles;

static void add(char kind, uint16_t port, uint64_t cycles, uint8_t value) {
  if (log_count < LOG_CAPACITY) {
    LogEntry *e = &log_entries[log_count++];
    e->kind = kind;
    e->port = port;
    e->cycles = cycles;
    e->event_cycles = last_event_cycles;
    e->value = value;
  }
}

static void dump_log(void) {
  const char *path = getenv("SMS_TEST_DEVICE_LOG");
  FILE *f;
  uint32_t i;
  if (path == NULL) return;
  f = fopen(path, "w");
  if (f == NULL) return;
  for (i = 0; i < log_count; ++i) {
    const LogEntry *e = &log_entries[i];
    if (e->kind == 'E') fprintf(f, "E %llu %llu\n", (unsigned long long)e->port, (unsigned long long)e->cycles);
    else fprintf(f, "%c %02X %llu %llu %02X\n", e->kind, (unsigned)e->port, (unsigned long long)e->cycles,
                 (unsigned long long)e->event_cycles, (unsigned)e->value);
  }
  fclose(f);
}

static void vdp_reset(void *context) {
  (void)context;
  pending = 0;
  line_pending = 0;
  memset(regs, 0, sizeof regs);
  regs[1] = 0x20u; /* stub convention: frame interrupt enabled until a program writes R1 */
  regs[10] = 0xFFu;
  latch_set = 0;
  line_counter = 0xFFu;
  last_line = 0;
  last_event_cycles = 0;
  log_count = 0;
}

static void vdp_scanline(void *context, uint64_t frame, uint32_t line, uint64_t cycles) {
  (void)context;
  (void)frame;
  last_line = line;
  last_event_cycles = cycles;
  if (line == 192u) pending = 1;
  /* line counter: reloaded from R10 outside lines 0-192, decremented on 0-192, underflow sets the line flag */
  if (line <= 192u) {
    if (line_counter == 0u) {
      line_counter = regs[10];
      line_pending = 1;
    } else {
      --line_counter;
    }
  } else {
    line_counter = regs[10];
  }
  if (log_count < LOG_CAPACITY) {
    LogEntry *e = &log_entries[log_count++];
    e->kind = 'E';
    e->port = (uint16_t)(frame * SMS_LINES_PER_FRAME + line);
    e->cycles = cycles;
    e->event_cycles = cycles;
    e->value = 0;
  }
}

static uint8_t vdp_irq_sources(void *context) {
  (void)context;
  return (uint8_t)(((pending && (regs[1] & 0x20u)) ? SMS_IRQ_FRAME : 0u) | ((line_pending && (regs[0] & 0x10u)) ? SMS_IRQ_LINE : 0u));
}

static uint8_t vdp_read(void *context, SmsPortClass cls, uint64_t cycles) {
  uint8_t value = 0xFFu;
  (void)context;
  if (cls == SMS_PORT_VDP_STATUS) {
    value = pending ? 0x80u : 0x00u;
    pending = 0;
    line_pending = 0;
    latch_set = 0;
  } else if (cls == SMS_PORT_V_COUNTER) {
    value = (uint8_t)last_line;
  } else if (cls == SMS_PORT_H_COUNTER) {
    value = 0;
  }
  add('R', (uint16_t)(cls == SMS_PORT_VDP_STATUS ? 0xBF : cls == SMS_PORT_V_COUNTER ? 0x7E : 0x7F), cycles, value);
  return value;
}

static void vdp_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  (void)context;
  if (cls == SMS_PORT_VDP_CONTROL) {
    if (!latch_set) {
      latch_value = value;
      latch_set = 1;
    } else {
      latch_set = 0;
      if ((value & 0xF0u) == 0x80u) regs[value & 15u] = latch_value; /* code 2: register write */
    }
  } else {
    latch_set = 0;
  }
  add('W', (uint16_t)(cls == SMS_PORT_VDP_CONTROL ? 0xBF : 0xBE), cycles, value);
}

static void vdp_digest(void *context, SmsSha256 *sha) {
  (void)context;
  sms_sha256_update_u8(sha, pending);
  sms_sha256_update_u8(sha, line_pending);
  sms_sha256_update(sha, regs, sizeof regs);
  sms_sha256_update_u8(sha, line_counter);
  sms_sha256_update_u32(sha, last_line);
}

static void psg_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  (void)context;
  (void)cls;
  add('W', 0x7F, cycles, value);
}

static uint8_t pad_read(void *context, SmsPortClass cls, uint64_t cycles) {
  uint8_t value;
  (void)context;
  if (cls == SMS_PORT_PAD_DC) value = (uint8_t)~((machine->input.p1 & 0x3Fu) | ((machine->input.p2 & 0x03u) << 6));
  else value = (uint8_t)~((machine->input.p2 >> 2) & 0x0Fu) | 0x30u;
  add('R', cls == SMS_PORT_PAD_DC ? 0xDC : 0xDD, cycles, value);
  return value;
}

static void pad_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  (void)context;
  (void)cls;
  add('W', 0x3F, cycles, value);
}

void sms_install_devices(SmsMachine *m) {
  static int registered;
  machine = m;
  m->vdp.port.reset = vdp_reset;
  m->vdp.port.read = vdp_read;
  m->vdp.port.write = vdp_write;
  m->vdp.port.digest = vdp_digest;
  m->vdp.scanline = vdp_scanline;
  m->vdp.irq_sources = vdp_irq_sources;
  m->psg.write = psg_write;
  m->pad.read = pad_read;
  m->pad.write = pad_write;
  if (!registered) {
    registered = 1;
    atexit(dump_log);
  }
}
