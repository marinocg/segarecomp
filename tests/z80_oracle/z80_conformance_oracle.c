/* SEG-008-T003: pinned redcode/Z80 oracle side of the differential harness (test-only, never linked into
 * production; ADR 0057). Reads the same vector text as the generated runner (tests/tools/z80_conformance_common.h) and
 * prints the same result schema, with `out=-` (the oracle has no typed outcomes).
 *
 * Build (checkout root = directory holding the pinned redcode_Z80 and redcode_Zeta clones), see
 * tools/z80_conformance.py for the exact command and defines. Steps are `i` (one instruction, interrupt response or
 * halted cycle; `z80_run(1)` repeated while the core suspends inside a DD/FD chain) or `r` (a raw run of N T-states,
 * used for the endless-prefix `prefix_lock` state, whose in-run PC is normalised to the next fetch address).
 */
#define _POSIX_C_SOURCE 200809L
#include "z80_conformance_common.h"

#include <Z80.h>

static zc_vector vec;
static zc_events events;
static int in_position, ack_position;

static void log_io(char kind, unsigned port, unsigned value) {
  if (events.io_count < ZC_MAX_EVENTS) {
    events.io_kind[events.io_count] = kind;
    events.io_port[events.io_count] = port;
    events.io_value[events.io_count++] = value;
  }
}
static zuint8 cb_fetch(void *c, zuint16 a) { (void)c; return vec.mem[a]; }
static zuint8 cb_read(void *c, zuint16 a) { (void)c; return vec.mem[a]; }
static void cb_write(void *c, zuint16 a, zuint8 v) {
  (void)c;
  vec.mem[a] = v;
  if (events.write_count < ZC_MAX_EVENTS) {
    events.write_addr[events.write_count] = a;
    events.write_value[events.write_count++] = v;
  }
}
static zuint8 cb_in(void *c, zuint16 p) {
  (void)c;
  const zuint8 v = in_position < vec.in_count ? vec.in_script[in_position++] : 0xFF;
  log_io('I', p, v);
  return v;
}
static void cb_out(void *c, zuint16 p, zuint8 v) { (void)c; log_io('O', p, v); }
static zuint8 cb_inta(void *c, zuint16 a) {
  (void)c; (void)a;
  const zuint8 v = ack_position < vec.ack_count ? vec.ack_script[ack_position++] : 0xFF;
  log_io('A', 0, v);
  return v;
}
static zuint8 cb_nmia(void *c, zuint16 a) { (void)c; return vec.mem[a]; }

static void set_state(Z80 *z, const zc_state *s) {
  Z80_AF(*z) = (zuint16)(s->a << 8 | s->f); Z80_BC(*z) = (zuint16)(s->b << 8 | s->c);
  Z80_DE(*z) = (zuint16)(s->d << 8 | s->e); Z80_HL(*z) = (zuint16)(s->h << 8 | s->l);
  Z80_AF_(*z) = (zuint16)(s->a2 << 8 | s->f2); Z80_BC_(*z) = (zuint16)(s->b2 << 8 | s->c2);
  Z80_DE_(*z) = (zuint16)(s->d2 << 8 | s->e2); Z80_HL_(*z) = (zuint16)(s->h2 << 8 | s->l2);
  Z80_IX(*z) = (zuint16)s->ix; Z80_IY(*z) = (zuint16)s->iy; Z80_SP(*z) = (zuint16)s->sp; Z80_PC(*z) = (zuint16)s->pc;
  Z80_MEMPTR(*z) = (zuint16)s->wz;
  z->i = (zuint8)s->i; z->r = (zuint8)s->r; z->im = (zuint8)s->im;
  z->iff1 = (zuint8)s->iff1; z->iff2 = (zuint8)s->iff2; z->q = (zuint8)s->q;
  z->halt_line = (zuint8)(s->halted != 0);
  z->resume = s->halted ? Z80_RESUME_HALT : 0;
  z->data.uint32_value = 0;
  if (s->deferral) z->data.uint8_array[0] = 0xFB;                                   /* previous opcode was EI */
  else if (s->ldair) { z->data.uint8_array[0] = 0xED; z->data.uint8_array[1] = 0x57; } /* previous was LD A,I */
  z->request = s->nmireject ? Z80_REQUEST_REJECT_NMI : 0;
  if (s->prefix_run) { /* the core suspends inside a chain with PC naming the last fetched prefix */
    z->resume = Z80_RESUME_XY;
    Z80_PC(*z) = (zuint16)(s->pc - 1u);
    z->data.uint8_array[0] = vec.mem[(s->pc - 1u) & 0xFFFFu];
  }
}
static void get_state(const Z80 *z, zc_state *s) {
  memset(s, 0, sizeof *s);
  s->a = Z80_AF(*z) >> 8; s->f = Z80_AF(*z) & 0xFF; s->b = Z80_BC(*z) >> 8; s->c = Z80_BC(*z) & 0xFF;
  s->d = Z80_DE(*z) >> 8; s->e = Z80_DE(*z) & 0xFF; s->h = Z80_HL(*z) >> 8; s->l = Z80_HL(*z) & 0xFF;
  s->a2 = Z80_AF_(*z) >> 8; s->f2 = Z80_AF_(*z) & 0xFF; s->b2 = Z80_BC_(*z) >> 8; s->c2 = Z80_BC_(*z) & 0xFF;
  s->d2 = Z80_DE_(*z) >> 8; s->e2 = Z80_DE_(*z) & 0xFF; s->h2 = Z80_HL_(*z) >> 8; s->l2 = Z80_HL_(*z) & 0xFF;
  s->ix = Z80_IX(*z); s->iy = Z80_IY(*z); s->sp = Z80_SP(*z); s->pc = Z80_PC(*z); s->wz = Z80_MEMPTR(*z);
  s->i = z->i; s->r = z->r; s->im = z->im; s->iff1 = z->iff1; s->iff2 = z->iff2; s->q = z->q;
  s->halted = (z->halt_line || z->resume == Z80_RESUME_HALT);
  const zuint8 d0 = z->data.uint8_array[0], d1 = z->data.uint8_array[1], d2 = z->data.uint8_array[2];
  s->deferral = d0 == 0xFB || (d0 == 0xED && (d1 & 0xC7) == 0x45 && (d2 & 1) == 0);
  s->ldair = (d0 == 0xED && (d1 & 0xF7) == 0x57);
  s->nmireject = (z->request & Z80_REQUEST_REJECT_NMI) != 0;
  s->prefix_run = (z->resume == Z80_RESUME_XY);
  if (s->prefix_run) s->pc = (s->pc + 1u) & 0xFFFFu;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: z80_conformance_oracle <vector-file>\n"); return 2; }
  FILE *file = fopen(argv[1], "r");
  if (!file) { fprintf(stderr, "cannot open vectors\n"); return 2; }
  static Z80 cpu;
  int status;
  while ((status = zc_read_vector(file, &vec)) == 1) {
    memset(&cpu, 0, sizeof cpu);
    cpu.context = NULL;
    cpu.fetch_opcode = cb_fetch; cpu.fetch = cb_read; cpu.read = cb_read; cpu.write = cb_write;
    cpu.in = cb_in; cpu.out = cb_out; cpu.nop = cb_fetch; cpu.nmia = cb_nmia; cpu.inta = cb_inta; cpu.int_fetch = cb_inta;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, Z_TRUE);
    set_state(&cpu, &vec.state);
    in_position = ack_position = 0;
    for (int k = 0; k < vec.step_count; ++k) {
      const zc_step *st = &vec.steps[k];
      z80_int(&cpu, st->int_line ? Z_TRUE : Z_FALSE);
      if (st->nmi) z80_nmi(&cpu);
      memset(&events, 0, sizeof events);
      unsigned long long t;
      if (st->mode == 'i') {
        t = (unsigned long long)z80_run(&cpu, 1);
        while (cpu.resume == Z80_RESUME_XY) t += (unsigned long long)z80_run(&cpu, 1);
      } else {
        t = (unsigned long long)z80_run(&cpu, st->budget ? st->budget : 1);
      }
      zc_state now;
      get_state(&cpu, &now);
      zc_print_step(stdout, vec.name, k, t, "-", &now, &events);
    }
  }
  fclose(file);
  return status < 0 ? 2 : 0;
}
