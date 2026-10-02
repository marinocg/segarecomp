/*
 * SEG-032-T003 (ADR 0073): host runner for RAM-backed (live-bytes) Z80 images and their immutable reference.
 *
 *   z80_live_guard_runner <ram.hex> <mode> [args]
 *     ram.hex  8,192 bytes as hex: the live sound RAM, mirrored at $0000-$3FFF; the generated image was compiled from it
 *     modes:   run   <max-steps> [mutate-after <steps> <offset> <xor>] [int-after <steps>] [no-matcher] [immutable]
 *              Steps one instruction at a time (deadline = cycles + 1) until a halt, an error outcome or max-steps.
 *              More options (SEG-032-T012): `repair` (on a code_mismatch after the mutation print `MISMATCH`, undo the mutation and retry the
 *              instruction), `int-on-repair` (raise INT, IM1, IFF1 for that retry), init keys `iff` and `im`, `start <hex pc>`, `init <reg>=<hex> ...` (a f b c d e h l ix iy sp i r), `dump <hex addr> <len>`
 *              (prints `DUMP <hex bytes>` before the END line).
 * Prints `STEP <n> <state>` after every executed instruction and `END <outcome> <n> <state>`; a state is
 * `pc=XXXX sp=.. af=.. bc=.. de=.. hl=.. ix=.. iy=.. cy=<cycles> r=.. iff=.. wz=.. df=<deferral> ai=<ld a,i/r marker> mem=<8 KiB digest>
 * io=<port-access digest>`.
 * `immutable`: the host provides no code_fetch (the image is the immutable reference or a live image whose host forgot the
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
static uint32_t io_digest = 2166136261u;
static void io_mix(uint32_t value) { io_digest = (io_digest ^ value) * 16777619u; }
static uint8_t io_in(void *context, uint16_t port, uint64_t cycles) {
  (void)context;
  io_mix(0x10000u | port); io_mix((uint32_t)cycles);
  return (uint8_t)(port * 7u + (port >> 8) + 0x21u);
}
static void io_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) {
  (void)context;
  io_mix(0x20000u | port); io_mix(value); io_mix((uint32_t)cycles);
}
static uint8_t int_ack(void *context, uint64_t cycles) { (void)context; (void)cycles; return 0xFFu; }
static int code_image(void *context, uint16_t address, Z80CodeImage *image) {
  (void)context;
  if (address >= 0x4000u) return 0;
  image->identity = 1u;
  image->window_base = 0u;
  return 1;
}
static int code_fetch(void *context, uint16_t address, uint8_t *bytes, uint32_t length) {
  uint32_t i;
  (void)context;
  for (i = 0; i < length; ++i) bytes[i] = ram[(uint16_t)(address + i) & 0x1FFFu];
  return 1;
}

static void print_state(const Z80State *s) {
  uint32_t mem = 2166136261u;
  size_t n;
  for (n = 0; n < sizeof ram; ++n) mem = (mem ^ ram[n]) * 16777619u;
  printf("pc=%04X sp=%04X af=%02X%02X bc=%02X%02X de=%02X%02X hl=%02X%02X ix=%04X iy=%04X cy=%llu r=%02X iff=%u wz=%04X df=%u ai=%u mem=%08X io=%08X",
         s->pc, s->sp, s->a, s->f, s->b, s->c, s->d, s->e, s->h, s->l, s->ix, s->iy, (unsigned long long)s->cycles, s->r, s->iff1, s->wz,
         s->int_deferral, s->ld_a_ir, mem, io_digest);
}

static void print_dump(unsigned long address, unsigned long length) {
  unsigned long n;
  if (length == 0) return;
  printf("DUMP ");
  for (n = 0; n < length; ++n) printf("%02X", ram[(address + n) & 0x1FFFu]);
  printf("\n");
}

static void apply_init(Z80State *s, const char *arg) {
  const char *eq = strchr(arg, '=');
  char name[8];
  unsigned long value;
  if (eq == NULL || (size_t)(eq - arg) >= sizeof name) exit(2);
  memcpy(name, arg, (size_t)(eq - arg));
  name[eq - arg] = 0;
  value = strtoul(eq + 1, NULL, 16);
  if (strcmp(name, "a") == 0) s->a = (uint8_t)value;
  else if (strcmp(name, "f") == 0) s->f = (uint8_t)value;
  else if (strcmp(name, "b") == 0) s->b = (uint8_t)value;
  else if (strcmp(name, "c") == 0) s->c = (uint8_t)value;
  else if (strcmp(name, "d") == 0) s->d = (uint8_t)value;
  else if (strcmp(name, "e") == 0) s->e = (uint8_t)value;
  else if (strcmp(name, "h") == 0) s->h = (uint8_t)value;
  else if (strcmp(name, "l") == 0) s->l = (uint8_t)value;
  else if (strcmp(name, "ix") == 0) s->ix = (uint16_t)value;
  else if (strcmp(name, "iy") == 0) s->iy = (uint16_t)value;
  else if (strcmp(name, "sp") == 0) s->sp = (uint16_t)value;
  else if (strcmp(name, "i") == 0) s->i = (uint8_t)value;
  else if (strcmp(name, "r") == 0) s->r = (uint8_t)value;
  else if (strcmp(name, "iff") == 0) s->iff1 = s->iff2 = (uint8_t)value;
  else if (strcmp(name, "im") == 0) s->im = (uint8_t)value;
  else exit(2);
}

int main(int argc, char **argv) {
  Z80Runtime rt;
  unsigned long max_steps, mutate_after = (unsigned long)-1, mutate_offset = 0, mutate_xor = 0, int_after = (unsigned long)-1;
  int matcher = 1, i;
  unsigned long step;
  unsigned long start_pc = 0, dump_address = 0, dump_length = 0;
  int repair = 0, int_on_repair = 0, mutate_done = 0, int_active = 0;
  const char *inits[16];
  int init_count = 0;
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
    } else if (strcmp(argv[i], "start") == 0 && i + 1 < argc) {
      start_pc = strtoul(argv[i + 1], NULL, 16); ++i;
    } else if (strcmp(argv[i], "dump") == 0 && i + 2 < argc) {
      dump_address = strtoul(argv[i + 1], NULL, 16); dump_length = strtoul(argv[i + 2], NULL, 10); i += 2;
    } else if (strcmp(argv[i], "init") == 0 && i + 1 < argc && init_count < 16) {
      inits[init_count++] = argv[i + 1]; ++i;
    } else if (strcmp(argv[i], "repair") == 0) {
      repair = 1;
    } else if (strcmp(argv[i], "int-on-repair") == 0) {
      int_on_repair = 1;
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
  rt.host.code_fetch = matcher ? code_fetch : NULL;
  z80_reset(&rt.state);
  rt.state.sp = 0x1F00u; /* a stack inside the RAM so CALL/PUSH/RET work */
  rt.state.pc = (uint16_t)start_pc;
  for (i = 0; i < init_count; ++i) apply_init(&rt.state, inits[i]);
  for (step = 0; step < max_steps; ++step) {
    Z80Outcome outcome;
    if (step == mutate_after && !mutate_done) {
      ram[mutate_offset & 0x1FFFu] ^= (uint8_t)mutate_xor;
      mutate_done = 1;
    }
    if (step == int_after) { rt.state.int_line = 1u; rt.state.iff1 = 1u; rt.state.im = 1u; int_active = 1; }
    outcome = z80_run(&rt, rt.state.cycles + 1u);
    if (repair && mutate_done && outcome == Z80_ERROR_CODE_MISMATCH) {
      /* The rejected instruction left the state untouched: undo the mutation and retry the very same instruction. */
      printf("MISMATCH %lu ", step);
      print_state(&rt.state);
      printf("\n");
      ram[mutate_offset & 0x1FFFu] ^= (uint8_t)mutate_xor;
      repair = 0;
      if (int_on_repair) int_after = step;
      --step; /* the loop increment retries this step */
      continue;
    }
    if (z80_outcome_is_error(outcome)) {
      print_dump(dump_address, dump_length);
      printf("END %s %lu ", z80_outcome_name(outcome), step);
      print_state(&rt.state);
      printf("\n");
      return 0;
    }
    if (outcome == Z80_OUTCOME_HALTED) {
      print_dump(dump_address, dump_length);
      printf("END halted %lu ", step);
      print_state(&rt.state);
      printf("\n");
      return 0;
    }
    if (int_active && rt.state.iff1 == 0u) { rt.state.int_line = 0u; int_active = 0; } /* level held until the interrupt is accepted */
    printf("STEP %lu ", step);
    print_state(&rt.state);
    printf("\n");
  }
  print_dump(dump_address, dump_length);
  printf("END max_steps %lu ", max_steps);
  print_state(&rt.state);
  printf("\n");
  return 0;
}
