// SEG-021-T019: generation-time MC68000 primary-word legality partition.
//
// Written from the Motorola M68000 Family Programmer's Reference Manual (M68000PM/AD Rev. 1, sections 4-6 and
// the operation-code map of Table 8-1 / Appendix A "Processor Instruction Summary") and the MC68000 User's Manual
// exception chapter (vectors 4, 10 and 11). It classifies one 16-bit operation word of the ORIGINAL MC68000 only:
// every encoding the manual assigns to a base-MC68000 instruction with a legal effective-address field is `legal`;
// the 1010 and 1111 lines are the line-A / line-F emulator traps; everything else -- unassigned operation words,
// reserved effective-address fields and the MC68010-and-later encodings (MOVE from CCR, MOVEC, MOVES, RTD, BKPT,
// CHK.L, LINK.L, EXTB.L, MULx.L/DIVx.L, CMP2/CHK2, CAS, bit-field, PACK/UNPK, TRAPcc, ...) -- is `illegal` and
// raises vector 4 on the MC68000.
//
// It is a pure function of the primary word: extension words never change the classification on the MC68000
// (a brief-format index word's reserved bits are ignored by the part, and immediate operand widths follow from
// the primary word). No dataset, table file or oracle is consulted; production never reads the test-side legal
// form baseline, which is compared against this function only by tests.
#include "segarecomp/cpu/m68k/decode.hpp"

namespace segarecomp {

namespace {

// Effective-address field categories (M68000PM/AD Rev. 1, Table 2-4). `mode`/`reg` are bits 5-3 / 2-0.
constexpr bool ea_valid(unsigned mode, unsigned reg) noexcept { return mode < 7U || reg <= 4U; }
constexpr bool ea_data(unsigned mode, unsigned reg) noexcept { return mode != 1U && ea_valid(mode, reg); }
constexpr bool ea_alterable(unsigned mode, unsigned reg) noexcept { return mode < 7U || reg <= 1U; }
constexpr bool ea_data_alterable(unsigned mode, unsigned reg) noexcept {
  return mode != 1U && ea_alterable(mode, reg);
}
constexpr bool ea_memory_alterable(unsigned mode, unsigned reg) noexcept {
  return mode >= 2U && ea_alterable(mode, reg);
}
constexpr bool ea_control(unsigned mode, unsigned reg) noexcept {
  return mode == 2U || mode == 5U || mode == 6U || (mode == 7U && reg <= 3U);
}
constexpr bool ea_control_alterable(unsigned mode, unsigned reg) noexcept {
  return mode == 2U || mode == 5U || mode == 6U || (mode == 7U && reg <= 1U);
}

// Line 0000: bit manipulation, MOVEP and immediate.
bool line0_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const unsigned size = (word >> 6U) & 3U;
  if ((word & 0x0100U) != 0U) {
    if (mode == 1U) return true;  // MOVEP (all four opmodes)
    // Dynamic BTST/BCHG/BCLR/BSET Dn,<ea>: BTST takes every data mode (including #imm); the others data alterable.
    return size == 0U ? ea_data(mode, reg) : ea_data_alterable(mode, reg);
  }
  switch ((word >> 9U) & 7U) {
  case 0U:  // ORI
  case 1U:  // ANDI
  case 5U:  // EORI
    if (mode == 7U && reg == 4U) return size == 0U || size == 1U;  // to CCR (byte) / to SR (word)
    return size != 3U && ea_data_alterable(mode, reg);
  case 2U:  // SUBI
  case 3U:  // ADDI
  case 6U:  // CMPI (the MC68000 has no PC-relative CMPI destination)
    return size != 3U && ea_data_alterable(mode, reg);
  case 4U:  // static BTST/BCHG/BCLR/BSET #<data>,<ea>
    if (size == 0U) return ea_data(mode, reg) && !(mode == 7U && reg == 4U);
    return ea_data_alterable(mode, reg);
  default:  // 111: MOVES (MC68010+)
    return false;
  }
}

// Lines 0001/0010/0011: MOVE.B / MOVE.L / MOVE.W and MOVEA.
bool move_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const bool byte = (word >> 12U) == 1U;
  const unsigned dst_mode = (word >> 6U) & 7U;
  const unsigned dst_reg = (word >> 9U) & 7U;
  if (!ea_valid(mode, reg) || (byte && mode == 1U)) return false;
  if (dst_mode == 1U) return !byte;  // MOVEA.W / MOVEA.L
  return ea_data_alterable(dst_mode, dst_reg);
}

// Line 0100: miscellaneous.
bool line4_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const unsigned size = (word >> 6U) & 3U;
  if ((word & 0x0100U) != 0U) {
    if (size == 3U) return ea_control(mode, reg);  // LEA
    if (size == 2U) return ea_data(mode, reg);     // CHK.W (CHK.L 1 00 is MC68020)
    return false;
  }
  switch ((word >> 9U) & 7U) {
  case 0U:  // NEGX / MOVE from SR
  case 1U:  // CLR (size 11 is MC68010 MOVE from CCR)
  case 2U:  // NEG / MOVE to CCR
  case 3U:  // NOT / MOVE to SR
    if (size != 3U) return ea_data_alterable(mode, reg);
    if (((word >> 9U) & 7U) == 0U) return ea_data_alterable(mode, reg);
    if (((word >> 9U) & 7U) == 1U) return false;
    return ea_data(mode, reg);
  case 4U:
    switch (size) {
    case 0U: return ea_data_alterable(mode, reg);  // NBCD (mode 001 is MC68020 LINK.L)
    case 1U:
      if (mode == 0U) return true;  // SWAP
      return ea_control(mode, reg);  // PEA (mode 001 is MC68010 BKPT)
    default:
      if (mode == 0U) return true;  // EXT.W / EXT.L
      return mode == 4U || ea_control_alterable(mode, reg);  // MOVEM registers to memory
    }
  case 5U:
    if (size != 3U) return ea_data_alterable(mode, reg);  // TST (the MC68000 has no An/PC/#imm TST)
    if (word == 0x4AFCU) return true;                      // ILLEGAL
    return ea_data_alterable(mode, reg);                   // TAS
  case 6U:
    if (size < 2U) return false;  // MULx.L / DIVx.L (MC68020)
    return mode == 3U || ea_control(mode, reg);  // MOVEM memory to registers
  default:  // 111
    switch (size) {
    case 0U: return false;
    case 1U:
      if (word <= 0x4E73U) return true;  // TRAP, LINK, UNLK, MOVE USP, RESET, NOP, STOP, RTE
      return word == 0x4E75U || word == 0x4E76U || word == 0x4E77U;  // RTS, TRAPV, RTR (RTD/MOVEC excluded)
    default: return ea_control(mode, reg);  // JSR / JMP
    }
  }
}

// Line 0101: ADDQ / SUBQ / Scc / DBcc.
bool line5_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const unsigned size = (word >> 6U) & 3U;
  if (size == 3U) return mode == 1U || ea_data_alterable(mode, reg);  // DBcc / Scc (TRAPcc is MC68020)
  if (mode == 1U) return size != 0U;                                 // ADDQ/SUBQ to An: word/long only
  return ea_alterable(mode, reg);
}

// Lines 1000 / 1100: OR, DIVU/DIVS, SBCD / AND, MULU/MULS, ABCD, EXG.
bool or_and_legal(unsigned word, unsigned mode, unsigned reg, bool and_line) noexcept {
  const unsigned opmode = (word >> 6U) & 7U;
  if (opmode == 3U || opmode == 7U) return ea_data(mode, reg);  // DIVU/DIVS.W, MULU/MULS.W
  if (opmode < 3U) return ea_data(mode, reg);                   // OR/AND <ea>,Dn
  if (mode <= 1U) {
    if (opmode == 4U) return true;  // SBCD / ABCD
    if (!and_line) return false;    // PACK / UNPK (MC68020)
    if (opmode == 5U) return true;  // EXG Dx,Dy / EXG Ax,Ay
    return mode == 1U;              // EXG Dx,Ay (01000 110 000 is unassigned)
  }
  return ea_memory_alterable(mode, reg);  // OR/AND Dn,<ea>
}

// Lines 1001 / 1101: SUB, SUBA, SUBX / ADD, ADDA, ADDX.
bool add_sub_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const unsigned opmode = (word >> 6U) & 7U;
  if (opmode == 3U || opmode == 7U) return ea_valid(mode, reg);  // SUBA/ADDA
  if (opmode < 3U) return ea_valid(mode, reg) && !(opmode == 0U && mode == 1U);  // <ea>,Dn (no byte An)
  if (mode <= 1U) return true;             // SUBX / ADDX
  return ea_memory_alterable(mode, reg);   // Dn,<ea>
}

// Line 1011: CMP, CMPA, EOR, CMPM.
bool line_b_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  const unsigned opmode = (word >> 6U) & 7U;
  if (opmode == 3U || opmode == 7U) return ea_valid(mode, reg);  // CMPA
  if (opmode < 3U) return ea_valid(mode, reg) && !(opmode == 0U && mode == 1U);  // CMP
  if (mode == 1U) return true;             // CMPM
  return ea_data_alterable(mode, reg);     // EOR
}

// Line 1110: shift/rotate. Register forms are all assigned; the memory-word forms need bit 11 clear (bit 11 set is
// the MC68020 bit-field group) and a memory alterable operand.
bool line_e_legal(unsigned word, unsigned mode, unsigned reg) noexcept {
  if (((word >> 6U) & 3U) != 3U) return true;
  if ((word & 0x0800U) != 0U) return false;
  return ea_memory_alterable(mode, reg);
}

} // namespace

M68kPrimaryWordClass m68k_classify_primary_word(std::uint16_t word) noexcept {
  const unsigned w = word;
  const unsigned mode = (w >> 3U) & 7U;
  const unsigned reg = w & 7U;
  bool legal = false;
  switch (w >> 12U) {
  case 0x0U: legal = line0_legal(w, mode, reg); break;
  case 0x1U:
  case 0x2U:
  case 0x3U: legal = move_legal(w, mode, reg); break;
  case 0x4U: legal = line4_legal(w, mode, reg); break;
  case 0x5U: legal = line5_legal(w, mode, reg); break;
  case 0x6U: legal = true; break;  // BRA/BSR/Bcc: every displacement byte is an MC68000 form
  case 0x7U: legal = (w & 0x0100U) == 0U; break;  // MOVEQ
  case 0x8U: legal = or_and_legal(w, mode, reg, false); break;
  case 0x9U:
  case 0xDU: legal = add_sub_legal(w, mode, reg); break;
  case 0xAU: return M68kPrimaryWordClass::line_a_emulator;
  case 0xBU: legal = line_b_legal(w, mode, reg); break;
  case 0xCU: legal = or_and_legal(w, mode, reg, true); break;
  case 0xEU: legal = line_e_legal(w, mode, reg); break;
  default: return M68kPrimaryWordClass::line_f_emulator;
  }
  return legal ? M68kPrimaryWordClass::legal : M68kPrimaryWordClass::illegal;
}

std::uint8_t m68k_primary_word_exception_vector(M68kPrimaryWordClass word_class) noexcept {
  switch (word_class) {
  case M68kPrimaryWordClass::line_a_emulator: return 10U;
  case M68kPrimaryWordClass::line_f_emulator: return 11U;
  case M68kPrimaryWordClass::illegal: return 4U;
  case M68kPrimaryWordClass::legal: break;
  }
  return 0U;
}

} // namespace segarecomp
