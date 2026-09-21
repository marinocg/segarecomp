/* SEG-021-T003: pinned-Musashi oracle runner. Executes exactly one instruction per vector from the same
 * bit-identical memory image as the generated-native runner and prints the same boundary-schema JSON.
 * Supervisor and user initial state are both represented (explicit USP and SSP); test/development oracle, never
 * linked into generated code. */
#include "m68k_conformance_common.h"
#include "m68k.h"
#ifndef MUSASHI_GIT_REVISION
#error "MUSASHI_GIT_REVISION must be supplied by the harness"
#endif
static uint8_t ram[CF_MEM_SIZE], before[CF_MEM_SIZE];
static unsigned rd8(unsigned a) { return a < CF_MEM_SIZE ? ram[a] : 0U; }
static void wr8(unsigned a, unsigned v) { if (a < CF_MEM_SIZE) ram[a] = (uint8_t)v; }
static unsigned rd16(unsigned a) { return (rd8(a) << 8U) | rd8(a + 1U); }
static unsigned rd32(unsigned a) { return (rd16(a) << 16U) | rd16(a + 2U); }
unsigned int m68k_read_memory_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return rd32(a); }
unsigned int m68k_read_immediate_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return rd32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return rd32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { wr8(a, v >> 8U); wr8(a + 1U, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16U); m68k_write_memory_16(a + 2U, v); }
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) { m68k_write_memory_32(a, v); }
int main(int argc, char **argv) {
  FILE *f; char line[4096]; cf_vector v; unsigned i;
  if (argc != 2 || !(f = fopen(argv[1], "r"))) return 2;
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  while (fgets(line, sizeof line, f)) {
    unsigned d[8], a[8];
    if (!cf_parse(line, &v)) { fprintf(stderr, "bad vector line\n"); return 2; }
    cf_init_memory(ram, &v); memcpy(before, ram, sizeof before);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, v.sr); /* first: selects (and swaps to) the requested stack, then seed both stacks explicitly */
    m68k_set_reg(M68K_REG_USP, v.usp); m68k_set_reg(M68K_REG_ISP, v.ssp);
    for (i = 0; i < 8U; ++i) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), v.d[i]);
    for (i = 0; i < 7U; ++i) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), v.a[i]);
    m68k_set_reg(M68K_REG_PC, CF_CODE_BASE);
    (void)m68k_execute(1); /* drain the pending reset cycles; executes no instruction */
    (void)m68k_execute(1); /* exactly one instruction (or one exception entry) */
    for (i = 0; i < 8U; ++i) { d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i)); a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i)); }
    cf_print(&v, before, ram, m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_USP), m68k_get_reg(NULL, M68K_REG_ISP), d, a);
  }
  fclose(f); return 0;
}
