/*
 * SEG-032-T003 (ADR 0073): host runner for RAM-backed (live-bytes) Z80 images and their immutable reference.
 *
 *   z80_live_guard_runner <ram.hex> <mode> [args]
 *     ram.hex  8,192 bytes as hex: the live sound RAM, mirrored at $0000-$3FFF; the generated image was compiled from it
 *     modes:   run   <max-steps> [mutate-after <steps> <offset> <xor>] [int-after <steps>] [no-matcher] [immutable]
 *              Steps one instruction at a time (deadline = cycles + 1) until a halt, an error outcome or max-steps.
 * Prints `STEP <n> <state>` after every executed instruction and `END <outcome> <n> <state>`; a state is
 * `pc=XXXX sp=.. af=.. bc=.. de=.. hl=.. ix=.. iy=.. cy=<cycles> r=..`.
 * `immutable`: the host provides no code_matches (the image is the immutable reference or a live image whose host forgot the
 * matcher). Test tool only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "segarecomp/codegen/c11/runtime/z80_runtime.h"

static uint8_t ram[8192];

static uint8_t mem_read(void *context, uint16_t address, uint64_t cycles) {
  (void)context; (void)cycles;
  return address < 0x4000u ? ram[address & 0x1FFFu] : 0xFFu;
}
static void mem_write(void *context, uint16_t address, uint8_t value, uint64_t cycles) {
  (void)context; (void)cycles;
  if (address < 0x4000u) ram[address & 0x1FFFu] = value;
}
static uint8_t io_in(void *context, uint16_t port, uint64_t cycles) { (void)context; (void)port; (void)cycles; return 0xFFu; }
static void io_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) { (void)context; (void)port; (void)value; (void)cycles; }
static uint8_t int_ack(void *context, uint64_t cycles) { (void)context; (void)cycles; return 0xFFu; }
static int code_image(void *context, uint16_t address, Z80CodeImage *image) {
  (void)context;
  if (address >= 0x4000u) return 0;
  image->identity = 1u;
  image->window_base = 0u;
  return 1;
}
static int code_matches(void *context, uint16_t address, const uint8_t *expected, uint32_t length) {
  uint32_t i;
  (void)context;
  for (i = 0; i < length; ++i)
    if (ram[(uint16_t)(address + i) & 0x1FFFu] != expected[i]) return 0;
  return 1;
}

static void print_state(const Z80State *s) {
  printf("pc=%04X sp=%04X af=%02X%02X bc=%02X%02X de=%02X%02X hl=%02X%02X ix=%04X iy=%04X cy=%llu r=%02X iff=%u", s->pc, s->sp, s->a,
         s->f, s->b, s->c, s->d, s->e, s->h, s->l, s->ix, s->iy, (unsigned long long)s->cycles, s->r, s->iff1);
}

int main(int argc, char **argv) {
  Z80Runtime rt;
  unsigned long max_steps, mutate_after = (unsigned long)-1, mutate_offset = 0, mutate_xor = 0, int_after = (unsigned long)-1;
  int matcher = 1, i;
  unsigned long step;
  if (argc < 4 || strcmp(argv[2], "run") != 0) return 2;
  {
    FILE *file = fopen(argv[1], "r");
    char pair[3] = {0, 0, 0};
    size_t n;
    if (file == NULL) return 2;
    for (n = 0; n < sizeof ram; ++n) {
      if (fread(pair, 1, 2, file) != 2) return 2;
      ram[n] = (uint8_t)strtoul(pair, NULL, 16);
    }
    fclose(file);
  }
  max_steps = strtoul(argv[3], NULL, 10);
  for (i = 4; i < argc; ++i) {
    if (strcmp(argv[i], "mutate-after") == 0 && i + 3 < argc) {
      mutate_after = strtoul(argv[i + 1], NULL, 10); mutate_offset = strtoul(argv[i + 2], NULL, 16);
      mutate_xor = strtoul(argv[i + 3], NULL, 16); i += 3;
    } else if (strcmp(argv[i], "int-after") == 0 && i + 1 < argc) {
      int_after = strtoul(argv[i + 1], NULL, 10); ++i;
    } else if (strcmp(argv[i], "no-matcher") == 0 || strcmp(argv[i], "immutable") == 0) {
      matcher = 0;
    } else {
      return 2;
    }
  }
  memset(&rt, 0, sizeof rt);
  rt.host.read = mem_read;
  rt.host.write = mem_write;
  rt.host.io_in = io_in;
  rt.host.io_out = io_out;
  rt.host.interrupt_acknowledge = int_ack;
  rt.host.code_image = code_image;
  rt.host.code_matches = matcher ? code_matches : NULL;
  z80_reset(&rt.state);
  rt.state.sp = 0x1F00u; /* a stack inside the RAM so CALL/PUSH/RET work */
  for (step = 0; step < max_steps; ++step) {
    Z80Outcome outcome;
    if (step == mutate_after) ram[mutate_offset & 0x1FFFu] ^= (uint8_t)mutate_xor;
    if (step == int_after) { rt.state.int_line = 1u; rt.state.iff1 = 1u; rt.state.im = 1u; }
    outcome = z80_run(&rt, rt.state.cycles + 1u);
    if (z80_outcome_is_error(outcome)) {
      printf("END %s %lu ", z80_outcome_name(outcome), step);
      print_state(&rt.state);
      printf("\n");
      return 0;
    }
    if (outcome == Z80_OUTCOME_HALTED) {
      printf("END halted %lu ", step);
      print_state(&rt.state);
      printf("\n");
      return 0;
    }
    if (step == int_after) rt.state.int_line = 0u;
    printf("STEP %lu ", step);
    print_state(&rt.state);
    printf("\n");
  }
  printf("END max_steps %lu ", max_steps);
  print_state(&rt.state);
  printf("\n");
  return 0;
}
