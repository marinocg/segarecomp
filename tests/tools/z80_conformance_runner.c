/* SEG-008-T003: generated-native side of the Z80 differential harness. Linked with the generated image
 * (`z80_run`, produced by the production emitter) and the ABI header; reads a vector file (z80_conformance_common.h)
 * and prints one result line per step. Hosts a flat 64 KiB memory and per-vector code-image map sets.
 *
 * usage: z80_conformance_runner <vector-file>
 * Every run is bounded: each step passes a finite cycle deadline to `z80_run`, and a vector has at most ZC_MAX_STEPS
 * steps, so a non-terminating guest cannot hang the harness.
 */
#define _POSIX_C_SOURCE 200809L
#include "segarecomp/codegen/c11/runtime/z80_runtime.h"
#include "z80_conformance_common.h"

static zc_vector vec;
static zc_events events;
static int in_position, ack_position, current_map;

static void log_io(char kind, unsigned port, unsigned value) {
  if (events.io_count < ZC_MAX_EVENTS) {
    events.io_kind[events.io_count] = kind;
    events.io_port[events.io_count] = port;
    events.io_value[events.io_count++] = value;
  }
}
static uint8_t host_read(void *context, uint16_t address, uint64_t cycles) {
  (void)context; (void)cycles;
  return vec.mem[address];
}
static void host_write(void *context, uint16_t address, uint8_t value, uint64_t cycles) {
  (void)context; (void)cycles;
  vec.mem[address] = value;
  if (events.write_count < ZC_MAX_EVENTS) {
    events.write_addr[events.write_count] = address;
    events.write_value[events.write_count++] = value;
  }
}
static uint8_t host_in(void *context, uint16_t port, uint64_t cycles) {
  (void)context; (void)cycles;
  const uint8_t value = in_position < vec.in_count ? vec.in_script[in_position++] : 0xFF;
  log_io('I', port, value);
  return value;
}
static void host_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) {
  (void)context; (void)cycles;
  log_io('O', port, value);
}
static uint8_t host_ack(void *context, uint64_t cycles) {
  (void)context; (void)cycles;
  const uint8_t value = ack_position < vec.ack_count ? vec.ack_script[ack_position++] : 0xFF;
  log_io('A', 0, value);
  return value;
}
static int host_code_image(void *context, uint16_t address, Z80CodeImage *image) {
  (void)context;
  const zc_map *map = &vec.maps[current_map];
  if (map->count == 0) { /* default: one invariant image 1 over the whole logical space */
    image->identity = 1;
    image->window_base = 0;
    return 1;
  }
  for (int i = 0; i < map->count; ++i) {
    if (address >= map->ranges[i].lo && address < map->ranges[i].hi) {
      image->identity = map->ranges[i].identity;
      image->window_base = (uint16_t)map->ranges[i].base;
      return 1;
    }
  }
  return 0;
}

static void load_state(Z80State *s, const zc_state *z) {
  memset(s, 0, sizeof *s);
  s->a = (uint8_t)z->a; s->f = (uint8_t)z->f; s->b = (uint8_t)z->b; s->c = (uint8_t)z->c;
  s->d = (uint8_t)z->d; s->e = (uint8_t)z->e; s->h = (uint8_t)z->h; s->l = (uint8_t)z->l;
  s->a2 = (uint8_t)z->a2; s->f2 = (uint8_t)z->f2; s->b2 = (uint8_t)z->b2; s->c2 = (uint8_t)z->c2;
  s->d2 = (uint8_t)z->d2; s->e2 = (uint8_t)z->e2; s->h2 = (uint8_t)z->h2; s->l2 = (uint8_t)z->l2;
  s->ix = (uint16_t)z->ix; s->iy = (uint16_t)z->iy; s->sp = (uint16_t)z->sp; s->pc = (uint16_t)z->pc;
  s->wz = (uint16_t)z->wz; s->i = (uint8_t)z->i; s->r = (uint8_t)z->r; s->im = (uint8_t)z->im;
  s->iff1 = (uint8_t)z->iff1; s->iff2 = (uint8_t)z->iff2; s->q = (uint8_t)z->q; s->halted = (uint8_t)z->halted;
  s->int_deferral = (uint8_t)z->deferral; s->ld_a_ir = (uint8_t)z->ldair; s->in_prefix_run = (uint8_t)z->prefix_run;
  s->nmi_reject = (uint8_t)z->nmireject;
}
static void store_state(zc_state *z, const Z80State *s) {
  z->a = s->a; z->f = s->f; z->b = s->b; z->c = s->c; z->d = s->d; z->e = s->e; z->h = s->h; z->l = s->l;
  z->a2 = s->a2; z->f2 = s->f2; z->b2 = s->b2; z->c2 = s->c2; z->d2 = s->d2; z->e2 = s->e2; z->h2 = s->h2; z->l2 = s->l2;
  z->ix = s->ix; z->iy = s->iy; z->sp = s->sp; z->pc = s->pc; z->wz = s->wz; z->i = s->i; z->r = s->r; z->im = s->im;
  z->iff1 = s->iff1; z->iff2 = s->iff2; z->q = s->q; z->halted = s->halted; z->deferral = s->int_deferral;
  z->ldair = s->ld_a_ir; z->prefix_run = s->in_prefix_run; z->nmireject = s->nmi_reject;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: z80_conformance_runner <vector-file>\n"); return 2; }
  FILE *file = fopen(argv[1], "r");
  if (!file) { fprintf(stderr, "cannot open vectors\n"); return 2; }
  static Z80Runtime rt;
  int status;
  while ((status = zc_read_vector(file, &vec)) == 1) {
    memset(&rt, 0, sizeof rt);
    load_state(&rt.state, &vec.state);
    rt.host = (Z80Host){NULL, host_read, host_write, host_in, host_out, host_ack, host_code_image, NULL};
    in_position = ack_position = 0;
    for (int k = 0; k < vec.step_count; ++k) {
      const zc_step *st = &vec.steps[k];
      current_map = st->map >= 0 && st->map < ZC_MAX_MAPS ? st->map : 0;
      rt.state.int_line = (uint8_t)st->int_line;
      if (st->nmi) rt.state.nmi_pending = 1;
      memset(&events, 0, sizeof events);
      const uint64_t before = rt.state.cycles;
      /* 'i': one instruction (deadline = now + 1); 'r': a relative run of N T-states; 'a' (SEG-008-T008): an ABSOLUTE
       * cycle deadline, the platform-owned scheduling form of the contract (a deadline already reached does no work). */
      const uint64_t budget = st->mode == 'i' ? 1u : st->budget;
      const uint64_t deadline = st->mode == 'a' ? (uint64_t)st->budget : before + (budget ? budget : 1u);
      const Z80Outcome outcome = z80_run(&rt, deadline);
      zc_state now;
      memset(&now, 0, sizeof now);
      store_state(&now, &rt.state);
      zc_print_step(stdout, vec.name, k, (unsigned long long)(rt.state.cycles - before), z80_outcome_name(outcome), &now, &events);
      if (z80_outcome_is_error(outcome)) break; /* fail-closed: later steps are meaningless */
    }
  }
  fclose(file);
  return status < 0 ? 2 : 0;
}
