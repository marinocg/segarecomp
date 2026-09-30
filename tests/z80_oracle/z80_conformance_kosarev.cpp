// SEG-008-T009: secondary independent oracle side of the Z80 differential harness (kosarev/z80, MIT, pinned in
// ADR 0057; test-only, never linked into production). Reads the same vector text as the generated runner and the
// primary oracle and prints the same result schema. kosarev has no Q, no LD A,I marker, no in-prefix-run state and no
// typed outcomes, so those fields print as 0 and tools/z80_conformance.py compares this side under the committed
// deviation mask (ADR 0057 decision 2). Step `i` = NMI (ungated, if requested), else an accepted INT, else one
// instruction (kosarev completes a DD/FD chain in one step); step `r` is not modelled (skipped vectors use `i` only).
#define _POSIX_C_SOURCE 200809L
#include <cassert>
#include <cstdint>

extern "C" {
#include "z80_conformance_common.h"
}

#include "z80.h"

using z80::fast_u16;
using z80::fast_u8;

static zc_vector vec;
static zc_events events;
static int in_position, ack_position;
static unsigned long ticks;

static void log_io(char kind, unsigned port, unsigned value) {
  if (events.io_count < ZC_MAX_EVENTS) {
    events.io_kind[events.io_count] = kind;
    events.io_port[events.io_count] = port;
    events.io_value[events.io_count++] = value;
  }
}

class cpu : public z80::z80_cpu<cpu> {
 public:
  typedef z80::z80_cpu<cpu> base;
  fast_u8 on_read(fast_u16 addr) { return vec.mem[addr & 0xFFFFu]; }
  void on_write(fast_u16 addr, fast_u8 n) {
    vec.mem[addr & 0xFFFFu] = static_cast<unsigned char>(n);
    if (events.write_count < ZC_MAX_EVENTS) {
      events.write_addr[events.write_count] = addr & 0xFFFFu;
      events.write_value[events.write_count++] = n & 0xFFu;
    }
  }
  fast_u8 on_input(fast_u16 port) {
    const unsigned v = in_position < vec.in_count ? vec.in_script[in_position++] : 0xFFu;
    log_io('I', port & 0xFFFFu, v);
    return v;
  }
  void on_output(fast_u16 port, fast_u8 n) { log_io('O', port & 0xFFFFu, n & 0xFFu); }
  fast_u8 on_get_int_vector() {
    const unsigned v = ack_position < vec.ack_count ? vec.ack_script[ack_position++] : 0xFFu;
    log_io('A', 0, v);
    return v;
  }
  void on_tick(unsigned t) {
    ticks += t;
    base::on_tick(t);
  }
};

static void set_state(cpu &z, const zc_state *s) {
  z.set_af(static_cast<fast_u16>(s->a << 8 | s->f)); z.set_bc(static_cast<fast_u16>(s->b << 8 | s->c));
  z.set_de(static_cast<fast_u16>(s->d << 8 | s->e)); z.set_hl(static_cast<fast_u16>(s->h << 8 | s->l));
  z.set_alt_af(static_cast<fast_u16>(s->a2 << 8 | s->f2)); z.set_alt_bc(static_cast<fast_u16>(s->b2 << 8 | s->c2));
  z.set_alt_de(static_cast<fast_u16>(s->d2 << 8 | s->e2)); z.set_alt_hl(static_cast<fast_u16>(s->h2 << 8 | s->l2));
  z.set_ix(static_cast<fast_u16>(s->ix)); z.set_iy(static_cast<fast_u16>(s->iy));
  z.set_sp(static_cast<fast_u16>(s->sp)); z.set_pc(static_cast<fast_u16>(s->pc)); z.set_wz(static_cast<fast_u16>(s->wz));
  z.set_i(s->i); z.set_r(s->r); z.set_int_mode(s->im);
  z.set_iff1(s->iff1 != 0); z.set_iff2(s->iff2 != 0);
  z.set_is_halted(s->halted != 0); z.set_is_int_disabled(s->deferral != 0);
}
static void get_state(cpu &z, zc_state *s) {
  memset(s, 0, sizeof *s);
  s->a = z.get_a(); s->f = z.get_f(); s->b = z.get_b(); s->c = z.get_c(); s->d = z.get_d(); s->e = z.get_e();
  s->h = z.get_h(); s->l = z.get_l();
  s->a2 = z.get_alt_af() >> 8; s->f2 = z.get_alt_af() & 0xFF; s->b2 = z.get_alt_bc() >> 8; s->c2 = z.get_alt_bc() & 0xFF;
  s->d2 = z.get_alt_de() >> 8; s->e2 = z.get_alt_de() & 0xFF; s->h2 = z.get_alt_hl() >> 8; s->l2 = z.get_alt_hl() & 0xFF;
  s->ix = z.get_ix(); s->iy = z.get_iy(); s->sp = z.get_sp(); s->pc = z.get_pc(); s->wz = z.get_wz();
  s->i = z.get_i(); s->r = z.get_r(); s->im = z.get_int_mode(); s->iff1 = z.get_iff1(); s->iff2 = z.get_iff2();
  s->halted = z.is_halted(); s->deferral = z.is_int_disabled();
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: z80_conformance_kosarev <vector-file>\n"); return 2; }
  FILE *file = fopen(argv[1], "r");
  if (!file) { fprintf(stderr, "cannot open vectors\n"); return 2; }
  static cpu z;
  int status;
  while ((status = zc_read_vector(file, &vec)) == 1) {
    z.on_reset();
    set_state(z, &vec.state);
    in_position = ack_position = 0;
    for (int k = 0; k < vec.step_count; ++k) {
      const zc_step *st = &vec.steps[k];
      memset(&events, 0, sizeof events);
      ticks = 0;
      if (st->nmi) z.initiate_nmi();
      else if (!(st->int_line && z.on_handle_active_int())) {
        do { z.on_step(); } while (z.get_iregp_kind() != z80::iregp::hl);  /* complete a DD/FD chain */
      }
      zc_state now;
      get_state(z, &now);
      zc_print_step(stdout, vec.name, k, ticks, "-", &now, &events);
    }
  }
  fclose(file);
  return status < 0 ? 2 : 0;
}
