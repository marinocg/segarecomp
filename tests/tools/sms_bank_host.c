/* SEG-009-T002 test host: runs a generated SMS image (emitted by the SMS generation route) against the SMS runtime
 * memory map and prints the observable result. Plain strict C11.
 *
 *   sms_bank_host --cycle-budget <T> [--pc <hex>] [--write <hex-addr>=<hex-val>]...
 *
 * `--cycle-budget` is mandatory (a finite absolute T-state deadline): the host refuses to run unbounded. Writes are
 * applied through the memory map at T = 0 before the run (for example a mapper-register write). One z80_run call is made.
 * Output lines: `outcome <name>`, `pc <hex>`, `cycles <dec>`, `sms_error <NAME>`, `ram <0xC200..0xC2FF hex>`,
 * `regs <FFFC..FFFF hex>`, and `ci <addr> <identity> <window_base>` for every code_image query (first 512). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_memory.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;

static SmsMemory memory;
static unsigned ci_count;

static uint8_t host_io_in(void *context, uint16_t port, uint64_t cycles) {
  (void)context; (void)port; (void)cycles;
  return 0xFFu;
}
static void host_io_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) {
  (void)context; (void)port; (void)value; (void)cycles;
}
static uint8_t host_ack(void *context, uint64_t cycles) {
  (void)context; (void)cycles;
  return 0xFFu;
}
static int host_code_image(void *context, uint16_t address, Z80CodeImage *image) {
  const int found = sms_memory_host_code_image(context, address, image);
  if (ci_count < 512u) {
    printf("ci %04X %u %04X\n", (unsigned)address, found ? (unsigned)image->identity : 0u, found ? (unsigned)image->window_base : 0u);
    ++ci_count;
  }
  return found;
}

int main(int argc, char **argv) {
  static Z80Runtime rt;
  uint64_t budget = 0;
  unsigned long pc = 0;
  int have_budget = 0;
  int i;
  unsigned n;
  SmsError init;
  z80_reset(&rt.state);
  init = sms_memory_init(&memory, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family);
  if (init != SMS_OK) {
    printf("sms_error %s\n", sms_error_name(init));
    return 4;
  }
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--cycle-budget") == 0 && i + 1 < argc) {
      budget = strtoull(argv[++i], NULL, 10);
      have_budget = budget != 0u;
    } else if (strcmp(argv[i], "--pc") == 0 && i + 1 < argc) {
      pc = strtoul(argv[++i], NULL, 16);
    } else if (strcmp(argv[i], "--write") == 0 && i + 1 < argc) {
      char *end = NULL;
      const unsigned long address = strtoul(argv[++i], &end, 16);
      if (end == NULL || *end != '=') return 64;
      sms_memory_write(&memory, (uint16_t)address, (uint8_t)strtoul(end + 1, NULL, 16), 0u);
    } else {
      return 64;
    }
  }
  if (!have_budget) {
    fprintf(stderr, "sms_bank_host: --cycle-budget <T> is required (finite deadline)\n");
    return 64;
  }
  rt.state.pc = (uint16_t)pc;
  rt.host.context = &memory;
  rt.host.read = sms_memory_host_read;
  rt.host.write = sms_memory_host_write;
  rt.host.io_in = host_io_in;
  rt.host.io_out = host_io_out;
  rt.host.interrupt_acknowledge = host_ack;
  rt.host.code_image = host_code_image;
  rt.outcome = z80_run(&rt, budget);
  printf("outcome %s\n", z80_outcome_name(rt.outcome));
  printf("pc %04X\n", (unsigned)rt.state.pc);
  printf("cycles %llu\n", (unsigned long long)rt.state.cycles);
  printf("sms_error %s\n", sms_error_name(memory.error));
  printf("ram ");
  for (n = 0; n < 0x100u; ++n) printf("%02X", (unsigned)memory.ram[0x200u + n]);
  printf("\nregs");
  for (n = 0; n < SMS_MAPPER_REG_COUNT; ++n) printf(" %02X", (unsigned)memory.mapper_regs[n]);
  printf("\n");
  return z80_outcome_is_error(rt.outcome) ? 3 : 0;
}
