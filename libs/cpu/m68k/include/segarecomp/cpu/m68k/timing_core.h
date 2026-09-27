#ifndef SEGARECOMP_CPU_M68K_TIMING_CORE_H
#define SEGARECOMP_CPU_M68K_TIMING_CORE_H

#include <stdint.h>

/*
 * SEG-021-T022: the data-dependent MC68000 cycle rules shared, verbatim, by the
 * C++ CPU timing owner (`m68k_instruction_timing`, timing.hpp) and by generated
 * C11 code. This is a header-only strict-C11 support unit owned by the M68K CPU
 * module: it includes only <stdint.h>, names no machine and holds no state.
 *
 * Fidelity: published instruction / exception-processing cycle totals consumed
 * by a deterministic instruction-boundary scheduler. Not a bus-cycle model (no
 * wait states, prefetch, bus arbitration or intra-instruction access timing).
 * None of the values below includes the effective-address calculation time;
 * callers add the MC68000 User's Manual Table 8-1 word cell where the row says
 * "+ EA".
 */

/* MC68000 User's Manual §8, Table 8-4 (standard instructions): MULU.W 38 + 2n, n = the number
   of 1 bits in the source word. */
static inline uint32_t segarecomp_m68k_mulu_word_cycles(uint16_t source) {
  uint32_t n = 0U;
  uint32_t bits = source;
  while (bits != 0U) {
    n += bits & 1U;
    bits >>= 1U;
  }
  return UINT32_C(38) + UINT32_C(2) * n;
}

/* Table 8-4: MULS.W 38 + 2n, n = the number of 10 or 01 bit pairs in the 17-bit
   value formed by the source word with a 0 appended as its least significant bit,
   i.e. the 1 bits of ((source << 1) ^ source) restricted to 16 bits. */
static inline uint32_t segarecomp_m68k_muls_word_cycles(uint16_t source) {
  uint32_t n = 0U;
  uint32_t bits = ((((uint32_t)source) << 1U) ^ (uint32_t)source) & UINT32_C(0xFFFF);
  while (bits != 0U) {
    n += bits & 1U;
    bits >>= 1U;
  }
  return UINT32_C(38) + UINT32_C(2) * n;
}

/* DIVU.W <ea>,Dn with a NON-ZERO divisor (a zero divisor takes the vector-5 entry
   instead; see segarecomp_m68k_exception_entry_cycles). Table 8-4 publishes only
   the bound "< 140"; this is the exact count of Jorge Cwik's published analysis of
   the 68000 division microcode (the same algorithm Genesis Plus GX implements as
   UseDivuCycles/UseDivsCycles), which models the non-restoring shift/subtract loop: overflow (Dn.H >= divisor) is detected up front and costs 10; otherwise
   76 plus, for each of the 15 remaining quotient steps, 0 when the shifted-out bit
   is 1, else 4 minus 2 when the partial remainder still covers the divisor. Range
   76..136 (never exceeding the published bound). */
static inline uint32_t segarecomp_m68k_divu_word_cycles(uint32_t dividend, uint16_t divisor) {
  const uint32_t high_divisor = ((uint32_t)divisor) << 16U;
  uint32_t remainder = dividend;
  uint32_t cycles = UINT32_C(76);
  uint32_t step;
  if ((dividend >> 16U) >= (uint32_t)divisor) return UINT32_C(10);
  for (step = 0U; step < 15U; ++step) {
    if ((remainder & UINT32_C(0x80000000)) != 0U) {
      remainder = (remainder << 1U) - high_divisor;
    } else {
      remainder <<= 1U;
      cycles += UINT32_C(4);
      if (remainder >= high_divisor) {
        remainder -= high_divisor;
        cycles -= UINT32_C(2);
      }
    }
  }
  return cycles;
}

/* DIVS.W <ea>,Dn with a NON-ZERO divisor. Table 8-4 publishes only the bound
   "< 158"; the same J. Cwik analysis gives the exact count from the operand signs
   and the absolute quotient: 12, plus 2 for a negative dividend; absolute overflow
   (|Dn| >> 16 >= |divisor|) then costs 4 more; otherwise 110 more, -2 (positive
   dividend) / +2 (negative dividend) when the divisor is positive, plus 2 for each
   0 among bits 15..1 of the absolute 16-bit quotient. Range 120..156. A signed
   overflow that passes the absolute check (quotient outside -32768..32767) takes
   the full path, as the hardware does. */
static inline uint32_t segarecomp_m68k_divs_word_cycles(uint32_t dividend, uint16_t divisor) {
  const int negative_dividend = (dividend & UINT32_C(0x80000000)) != 0U;
  const int negative_divisor = (divisor & UINT16_C(0x8000)) != 0U;
  const uint32_t magnitude_dividend = negative_dividend ? (uint32_t)(UINT32_C(0) - dividend) : dividend;
  const uint32_t magnitude_divisor =
      negative_divisor ? (uint32_t)(UINT32_C(0x10000) - (uint32_t)divisor) : (uint32_t)divisor;
  uint32_t cycles = UINT32_C(12) + (negative_dividend ? UINT32_C(2) : UINT32_C(0));
  uint32_t quotient;
  uint32_t bit;
  if (magnitude_divisor == 0U) return 0U; /* zero divisor: not a retirement */
  if ((magnitude_dividend >> 16U) >= magnitude_divisor) return cycles + UINT32_C(4);
  quotient = magnitude_dividend / magnitude_divisor;
  cycles += UINT32_C(110);
  if (!negative_divisor) cycles = negative_dividend ? cycles + UINT32_C(2) : cycles - UINT32_C(2);
  for (bit = 1U; bit < 16U; ++bit)
    if (((quotient >> bit) & 1U) == 0U) cycles += UINT32_C(2);
  return cycles;
}

/* MC68000 User's Manual §8, Table 8-14 (exception processing execution times)
   (ADR 0043 U5), by vector number: the complete time from the start of exception
   processing (for the instruction-caused rows: from the start of the instruction)
   to the fetch of the handler's first instruction. Divide by zero (vector 5) and
   CHK (vector 6) additionally take the source operand's EA calculation time, which
   the caller adds. Interrupt rows assume the four-clock acknowledge cycle the table
   assumes. Returns 0 for a vector no MC68000 exception is delivered through
   (reserved vectors, 0/1 reset, 12-14, 16-23, 48-63). Bus/address error (50) and
   reset (40) are listed for completeness; the project does not deliver them. */
static inline uint32_t segarecomp_m68k_exception_entry_cycles(uint32_t vector) {
  if (vector == 2U || vector == 3U) return UINT32_C(50); /* bus error, address error (not delivered) */
  if (vector == 4U) return UINT32_C(34);                 /* illegal instruction */
  if (vector == 5U) return UINT32_C(38);                 /* zero divide (+ EA) */
  if (vector == 6U) return UINT32_C(40);                 /* CHK (+ EA) */
  if (vector == 7U) return UINT32_C(34);                 /* TRAPV */
  if (vector == 8U) return UINT32_C(34);                 /* privilege violation */
  if (vector == 9U) return UINT32_C(34);                 /* trace (not delivered) */
  if (vector == 10U || vector == 11U) return UINT32_C(34); /* line 1010 / 1111 (unimplemented instruction) */
  if (vector == 15U || (vector >= 24U && vector <= 31U) || (vector >= 64U && vector <= 255U))
    return UINT32_C(44);                                 /* uninitialized, spurious, autovector, user interrupt */
  if (vector >= 32U && vector <= 47U) return UINT32_C(34); /* TRAP #0-15 */
  return 0U;
}

#endif
