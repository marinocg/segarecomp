/* Test-only pinned Musashi primary-word sweep (SEG-021-T001). Configured as a 68000.
 * Executes each of the 65,536 opcode words once in supervisor mode and once in user mode from
 * a fixed, recorded state and prints the exception vector taken (or "-" when none):
 *   WWWW <supervisor-outcome> <user-outcome>
 * Overlap/hole cross-check on PRIMARY words only: it never infers EA/extension legality.
 * Fixed state: D0-D7 = 0, A0-A6 = 0x00002000, ISP = 0x00004000, USP = 0x00006000,
 * extension bytes following the opcode word = all 0x00, every other readable byte = 0x00,
 * vectors 2..255 point at 0x00008000 + 8 * vector, PC = 0x00001000, writes discarded. */
#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
#define CODE_PC UINT32_C(0x00001000)
#define HANDLER_BASE UINT32_C(0x00008000)
#define ISP_INIT UINT32_C(0x00004000)
#define USP_INIT UINT32_C(0x00006000)
static uint16_t opcode_word;
static unsigned int read8(unsigned int a) {
  a &= UINT32_C(0x00FFFFFF);
  if (a < 8U) { /* reset vectors: SSP = ISP_INIT, PC = CODE_PC */
    static const uint8_t reset[8] = {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x10, 0x00};
    return reset[a];
  }
  if (a < UINT32_C(0x400)) { /* vector table: vector v at 4*v -> HANDLER_BASE + 8*v */
    uint32_t v = a >> 2U, t = HANDLER_BASE + 8U * v;
    return (t >> (8U * (3U - (a & 3U)))) & 0xFFU;
  }
  if (a == CODE_PC) return opcode_word >> 8U;
  if (a == CODE_PC + 1U) return opcode_word & 0xFFU;
  return 0U;
}
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a) << 8U) | read8(a + 1U); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16U) | m68k_read_memory_16(a + 2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) { (void)a; (void)v; }

static int run_one(uint16_t word, unsigned int sr) {
  int i;
  unsigned int pc;
  opcode_word = word;
  m68k_pulse_reset(); /* clears STOPPED state left by the previous word */
  (void)m68k_execute(0); /* consume only the pending reset cycles (returns before fetching) */
  m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700)); /* supervisor first so ISP/USP can be set explicitly */
  m68k_set_reg(M68K_REG_ISP, ISP_INIT);
  m68k_set_reg(M68K_REG_USP, USP_INIT);
  for (i = 0; i < 8; ++i) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), 0U);
  for (i = 0; i < 7; ++i) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), UINT32_C(0x00002000));
  m68k_set_reg(M68K_REG_SR, sr);
  m68k_set_reg(M68K_REG_PC, CODE_PC);
  (void)m68k_execute(1);
  pc = m68k_get_reg(NULL, M68K_REG_PC);
  if (pc >= HANDLER_BASE && pc < HANDLER_BASE + 8U * 256U && ((pc - HANDLER_BASE) % 8U) == 0U)
    return (int)((pc - HANDLER_BASE) / 8U);
  return -1;
}
static void print_outcome(int v) { if (v < 0) fputs("-", stdout); else printf("%d", v); }
int main(void) {
  unsigned int w;
  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_pulse_reset();
  for (w = 0; w < 0x10000U; ++w) {
    int s = run_one((uint16_t)w, UINT32_C(0x2700)), u = run_one((uint16_t)w, UINT32_C(0x0700));
    printf("%04X ", w); print_outcome(s); fputs(" ", stdout); print_outcome(u); fputs("\n", stdout);
  }
  return 0;
}
