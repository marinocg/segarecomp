// Data-transfer and ALU family rows (SEG-008-T004): 8/16-bit LD, 8-bit ALU, INC/DEC, 16-bit arithmetic, accumulator
// rotates, DAA/CPL/NEG/SCF/CCF, including the DD/FD indexed and undocumented IXH/IXL/IYH/IYL forms and the ED forms
// the T001 matrix assigns to this family. Every row is table-driven: one lowering function serves every form that
// differs only in operand class, and the operand class (register, (HL), (IX+d), IXH/IXL, immediate) is resolved by the
// same access helper. Flag results follow docs/architecture/z80-cpu-contract.md section 5 (X/Y from the result, from the
// operand for CP, from Q/F/A for SCF/CCF); MEMPTR (WZ) updates follow the same contract.
#include <string>

#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

// ---------------------------------------------------------------------------------------------- operand access
// An 8-bit operand as C text: `setup` declares the effective address (and updates MEMPTR for indexed forms), `rd` is a
// side-effect free read expression and the write is `wr_head + value + wr_tail`.
struct Acc {
  std::string setup;
  std::string rd;
  std::string wr_head;
  std::string wr_tail;
  bool writable = true;
};

std::string index_reg(const LowerContext& c) { return c.form.space == Space::fd ? "s->iy" : "s->ix"; }

std::string hl_expr() { return "((uint16_t)(((uint16_t)s->h << 8) | s->l))"; }

Acc reg_acc(const std::string& name) { return {"", name, name + " = ", ";\n", true}; }

// Undocumented IXH/IXL/IYH/IYL: field 4 selects the high byte, 5 the low byte (the H/L slots of the r field).
Acc half_acc(const std::string& reg, unsigned field) {
  if (field == 4u)
    return {"", "((uint8_t)(" + reg + " >> 8))", reg + " = (uint16_t)((" + reg + " & 0x00FFu) | ((uint16_t)(",
            ") << 8));\n", true};
  return {"", "((uint8_t)" + reg + ")", reg + " = (uint16_t)((" + reg + " & 0xFF00u) | (uint16_t)(", "));\n", true};
}

Acc mem_acc(const std::string& setup) {
  return {setup, "z80_read(rt, ea)", "z80_write(rt, ea, ", ");\n", true};
}

Acc access(const LowerContext& c, Operand op, unsigned field) {
  switch (op) {
    case Operand::r: return reg_acc(reg8(field));
    case Operand::a: return reg_acc(reg8(7));
    case Operand::hl_ind: return mem_acc("const uint16_t ea = " + hl_expr() + ";\n");
    case Operand::ix_d:
    case Operand::iy_d: {
      const unsigned d = static_cast<unsigned>(cpu::z80::displacement(c.instruction).value_or(0)) & 0xFFFFu;
      return mem_acc("const uint16_t ea = (uint16_t)(" + index_reg(c) + " + " + hex_literal(d, 4) + ");\ns->wz = ea;\n");
    }
    case Operand::ix_half: return half_acc("s->ix", field);
    case Operand::iy_half: return half_acc("s->iy", field);
    case Operand::n: {
      Acc a;
      a.rd = "((uint8_t)" + hex_literal(cpu::z80::immediate(c.instruction).value_or(0) & 0xFFu, 2) + ")";
      a.writable = false;
      return a;
    }
    default: return {};
  }
}

std::string block(const std::string& body) { return "{\n" + body + "}\n"; }

// ---------------------------------------------------------------------------------------------- 16-bit registers
enum class P16 { bc, de, hl, sp, ix, iy };

std::string get16(P16 p) {
  switch (p) {
    case P16::bc: return "((uint16_t)(((uint16_t)s->b << 8) | s->c))";
    case P16::de: return "((uint16_t)(((uint16_t)s->d << 8) | s->e))";
    case P16::hl: return hl_expr();
    case P16::sp: return "s->sp";
    case P16::ix: return "s->ix";
    case P16::iy: return "s->iy";
  }
  return "";
}

std::string set16(P16 p, const std::string& v) {
  const auto pair = [&](const char* hi, const char* lo) {
    return std::string("s->") + hi + " = (uint8_t)((" + v + ") >> 8);\ns->" + lo + " = (uint8_t)(" + v + ");\n";
  };
  switch (p) {
    case P16::bc: return pair("b", "c");
    case P16::de: return pair("d", "e");
    case P16::hl: return pair("h", "l");
    case P16::sp: return "s->sp = (uint16_t)(" + v + ");\n";
    case P16::ix: return "s->ix = (uint16_t)(" + v + ");\n";
    case P16::iy: return "s->iy = (uint16_t)(" + v + ");\n";
  }
  return "";
}

// The 16-bit register an operand class names at a given rr field (bits 5:4 of the opcode).
P16 pick16(Operand op, unsigned field) {
  static constexpr P16 plain[4] = {P16::bc, P16::de, P16::hl, P16::sp};
  static constexpr P16 ixed[4] = {P16::bc, P16::de, P16::ix, P16::sp};
  static constexpr P16 iyed[4] = {P16::bc, P16::de, P16::iy, P16::sp};
  switch (op) {
    case Operand::hl: return P16::hl;
    case Operand::ix: return P16::ix;
    case Operand::iy: return P16::iy;
    case Operand::sp: return P16::sp;
    case Operand::rr_ix: return ixed[field & 3u];
    case Operand::rr_iy: return iyed[field & 3u];
    default: return plain[field & 3u];  // rr, rr_ed
  }
}

unsigned opcode_of(const LowerContext& c) { return c.instruction.provenance.opcode; }
unsigned rr_field(const LowerContext& c) { return (opcode_of(c) >> 4) & 3u; }

// ---------------------------------------------------------------------------------------------- flag text
// Bits: S 0x80, Z 0x40, Y 0x20, H 0x10, X 0x08, P/V 0x04, N 0x02, C 0x01.
std::string sz_xy(const std::string& v) { return "((" + v + ") & 0xA8u) | ((" + v + ") == 0u ? 0x40u : 0u)"; }
std::string parity(const std::string& v) {  // P/V bit set for even parity
  return "(((0x9669u >> (((" + v + ") ^ ((" + v + ") >> 4)) & 0xFu)) & 1u) << 2)";
}

// ---------------------------------------------------------------------------------------------- LD
Lowered lower_ld8(const LowerContext& c) {
  const unsigned op = opcode_of(c);
  const Acc dst = access(c, c.form.dst, (op >> 3) & 7u);
  const Acc src = access(c, c.form.src, op & 7u);
  return effect(block(dst.setup + src.setup + dst.wr_head + src.rd + dst.wr_tail));
}

Lowered lower_ld_a_rr_ind(const LowerContext& c) {  // LD A,(BC|DE): MEMPTR = rr + 1
  const std::string rr = get16((rr_field(c) & 1u) ? P16::de : P16::bc);
  return effect(block("const uint16_t ea = " + rr + ";\ns->a = z80_read(rt, ea);\ns->wz = (uint16_t)(ea + 1u);\n"));
}

Lowered lower_ld_rr_ind_a(const LowerContext& c) {  // LD (BC|DE),A: MEMPTR = (A << 8) | ((rr + 1) & 0xFF)
  const std::string rr = get16((rr_field(c) & 1u) ? P16::de : P16::bc);
  return effect(block("const uint16_t ea = " + rr + ";\nz80_write(rt, ea, s->a);\n" +
                      "s->wz = (uint16_t)(((uint16_t)s->a << 8) | ((ea + 1u) & 0xFFu));\n"));
}

Lowered lower_ld_a_nn(const LowerContext& c) {
  const unsigned nn = cpu::z80::immediate(c.instruction).value_or(0);
  return effect("s->a = z80_read(rt, " + hex_literal(nn, 4) + ");\ns->wz = " + hex_literal((nn + 1u) & 0xFFFFu, 4) + ";\n");
}

Lowered lower_ld_nn_a(const LowerContext& c) {
  const unsigned nn = cpu::z80::immediate(c.instruction).value_or(0);
  return effect("z80_write(rt, " + hex_literal(nn, 4) + ", s->a);\ns->wz = (uint16_t)(((uint16_t)s->a << 8) | " +
                hex_literal((nn + 1u) & 0xFFu, 2) + ");\n");
}

Lowered lower_ld_rr_nn(const LowerContext& c) {
  const unsigned nn = cpu::z80::immediate(c.instruction).value_or(0);
  return effect(set16(pick16(c.form.dst, rr_field(c)), hex_literal(nn, 4)));
}

// LD (nn),rr and LD rr,(nn) for HL, IX, IY and the ED BC/DE/SP forms: low byte first, MEMPTR = nn + 1.
Lowered lower_ld16_mem(const LowerContext& c) {
  const unsigned nn = cpu::z80::immediate(c.instruction).value_or(0);
  const std::string lo = hex_literal(nn, 4);
  const std::string hi = hex_literal((nn + 1u) & 0xFFFFu, 4);
  const bool store = c.form.dst == Operand::nn_ind;
  const Operand reg_op = store ? c.form.src : c.form.dst;
  const P16 reg = pick16(reg_op, rr_field(c));
  std::string text;
  if (store) {
    text = "{\nconst uint16_t v = " + get16(reg) + ";\nz80_write(rt, " + lo + ", (uint8_t)v);\nz80_write(rt, " + hi +
           ", (uint8_t)(v >> 8));\n}\n";
  } else {
    text = "{\nconst uint8_t low = z80_read(rt, " + lo + ");\nconst uint8_t high = z80_read(rt, " + hi + ");\n" +
           "const uint16_t v = (uint16_t)(low | ((uint16_t)high << 8));\n" + set16(reg, "v") + "}\n";
  }
  return effect(text + "s->wz = " + hi + ";\n");
}

Lowered lower_ld_sp(const LowerContext& c) { return effect(set16(P16::sp, get16(pick16(c.form.src, 0)))); }

// ---------------------------------------------------------------------------------------------- 8-bit ALU
Lowered lower_alu8(const LowerContext& c) {
  const unsigned op = opcode_of(c);
  const Acc src = access(c, c.form.src, op & 7u);
  std::string body = src.setup;
  body += "const unsigned a = s->a;\nconst unsigned v = " + src.rd + ";\n";
  const Mnemonic m = c.form.mnemonic;
  const bool with_carry = m == Mnemonic::adc || m == Mnemonic::sbc;
  const bool subtract = m == Mnemonic::sub || m == Mnemonic::sbc || m == Mnemonic::cp;
  if (m == Mnemonic::and_ || m == Mnemonic::xor_ || m == Mnemonic::or_) {
    const char* opr = m == Mnemonic::and_ ? "&" : m == Mnemonic::xor_ ? "^" : "|";
    body += std::string("const unsigned res = (a ") + opr + " v) & 0xFFu;\n";
    body += "s->f = (uint8_t)(" + sz_xy("res") + " | " + parity("res") + (m == Mnemonic::and_ ? " | 0x10u" : "") + ");\n";
    body += "s->a = (uint8_t)res;\n";
    return effect(block(body), true);
  }
  body += std::string("const unsigned cin = ") + (with_carry ? "(unsigned)(s->f & 1u)" : "0u") + ";\n";
  body += std::string("const unsigned r = ") + (subtract ? "a - v - cin" : "a + v + cin") + ";\n";
  body += "const unsigned res = r & 0xFFu;\n";
  const std::string xy = m == Mnemonic::cp ? "((v & 0x28u) | (res == 0u ? 0x40u : 0u) | (res & 0x80u))" : "(" + sz_xy("res") + ")";
  const std::string overflow = subtract ? "((((a ^ v) & (a ^ res)) & 0x80u) >> 5)" : "(((~(a ^ v) & (a ^ res)) & 0x80u) >> 5)";
  body += "s->f = (uint8_t)(" + xy + " | ((a ^ v ^ res) & 0x10u) | " + overflow + (subtract ? " | 0x02u" : "") +
          " | ((r >> 8) & 1u));\n";
  if (m != Mnemonic::cp) body += "s->a = (uint8_t)res;\n";
  return effect(block(body), true);
}

// INC/DEC r, (HL), (IX+d), IXH/IXL: the carry flag is preserved.
Lowered lower_incdec8(const LowerContext& c) {
  const unsigned op = opcode_of(c);
  const Acc dst = access(c, c.form.dst, (op >> 3) & 7u);
  const bool inc = c.form.mnemonic == Mnemonic::inc;
  std::string body = dst.setup + "const unsigned t = " + dst.rd + ";\n";
  body += std::string("const unsigned res = (t ") + (inc ? "+" : "-") + " 1u) & 0xFFu;\n";
  body += dst.wr_head + "(uint8_t)res" + dst.wr_tail;
  body += "s->f = (uint8_t)((s->f & 0x01u) | (" + sz_xy("res") + ") | " +
          (inc ? "((t & 0x0Fu) == 0x0Fu ? 0x10u : 0u) | (t == 0x7Fu ? 0x04u : 0u)"
               : "((t & 0x0Fu) == 0x00u ? 0x10u : 0u) | (t == 0x80u ? 0x04u : 0u) | 0x02u") +
          ");\n";
  return effect(block(body), true);
}

Lowered lower_neg(const LowerContext&) {
  return effect(block("const unsigned t = s->a;\nconst unsigned res = (0u - t) & 0xFFu;\ns->a = (uint8_t)res;\n"
                      "s->f = (uint8_t)((" + sz_xy("res") + ") | ((t & 0x0Fu) != 0u ? 0x10u : 0u) | (t == 0x80u ? 0x04u : 0u) | "
                      "0x02u | (t != 0u ? 0x01u : 0u));\n"),
                true);
}

Lowered lower_daa(const LowerContext&) {
  return effect(block(
                    "const unsigned a = s->a;\nconst unsigned f = s->f;\nunsigned corr = 0u;\nunsigned cy = f & 0x01u;\n"
                    "if ((f & 0x10u) != 0u || (a & 0x0Fu) > 9u) corr |= 0x06u;\n"
                    "if (cy != 0u || a > 0x99u) {\n corr |= 0x60u;\n cy = 1u;\n}\n"
                    "const unsigned n = f & 0x02u;\n"
                    "const unsigned res = (n != 0u ? a - corr : a + corr) & 0xFFu;\n"
                    "const unsigned h = n != 0u ? (((f & 0x10u) != 0u && (a & 0x0Fu) < 6u) ? 0x10u : 0u)\n"
                    "                           : ((a & 0x0Fu) > 9u ? 0x10u : 0u);\n"
                    "s->a = (uint8_t)res;\n"
                    "s->f = (uint8_t)((" + sz_xy("res") + ") | h | " + parity("res") + " | n | cy);\n"),
                true);
}

Lowered lower_cpl(const LowerContext&) {
  return effect("s->a = (uint8_t)~s->a;\ns->f = (uint8_t)((s->f & 0xC5u) | (s->a & 0x28u) | 0x12u);\n", true);
}

// SCF/CCF: X/Y = ((Q xor F) or A) bits 5/3 (Q is the F written by the previous instruction, else 0). A DD/FD prefix
// ignored before the form is an instruction of its own that leaves Q = 0, so the input Q is then 0.
std::string q_input(const LowerContext& c) { return c.instruction.provenance.prefix_count > 0 ? "0u" : "s->q"; }

Lowered lower_scf(const LowerContext& c) {
  return effect("s->f = (uint8_t)((s->f & 0xC4u) | ((((unsigned)" + q_input(c) + " ^ s->f) | s->a) & 0x28u) | 0x01u);\n", true);
}

Lowered lower_ccf(const LowerContext& c) {
  return effect(block("const unsigned old = s->f & 0x01u;\n"
                      "s->f = (uint8_t)((s->f & 0xC4u) | ((((unsigned)" + q_input(c) + " ^ s->f) | s->a) & 0x28u) | "
                      "(old != 0u ? 0x10u : 0x01u));\n"),
                true);
}

Lowered lower_rot_a(const LowerContext& c) {
  const bool through_carry = c.form.mnemonic == Mnemonic::rla || c.form.mnemonic == Mnemonic::rra;
  std::string body = "const unsigned a = s->a;\n";
  if (through_carry) body += "const unsigned old = s->f & 0x01u;\n";
  body += "unsigned res;\nunsigned cy;\n";
  switch (c.form.mnemonic) {
    case Mnemonic::rlca: body += "cy = a >> 7;\nres = ((a << 1) | cy) & 0xFFu;\n"; break;
    case Mnemonic::rrca: body += "cy = a & 1u;\nres = (a >> 1) | (cy << 7);\n"; break;
    case Mnemonic::rla: body += "cy = a >> 7;\nres = ((a << 1) | old) & 0xFFu;\n"; break;
    default: body += "cy = a & 1u;\nres = (a >> 1) | (old << 7);\n"; break;  // RRA
  }
  body += "s->a = (uint8_t)res;\ns->f = (uint8_t)((s->f & 0xC4u) | (res & 0x28u) | cy);\n";
  return effect(block(body), true);
}

// ---------------------------------------------------------------------------------------------- 16-bit arithmetic
Lowered lower_inc_dec16(const LowerContext& c) {
  const P16 reg = pick16(c.form.dst, rr_field(c));
  const char* step = c.form.mnemonic == Mnemonic::inc ? "+ 1u" : "- 1u";
  return effect(set16(reg, "(uint16_t)(" + get16(reg) + " " + step + ")"));
}

// ADD HL|IX|IY,rr: S/Z/PV kept, H from bit 11, Y/X from the high byte, MEMPTR = destination + 1 before the add.
Lowered lower_add16(const LowerContext& c) {
  const P16 dst = pick16(c.form.dst, 0);
  const P16 src = pick16(c.form.src, rr_field(c));
  std::string body = "const unsigned x = " + get16(dst) + ";\nconst unsigned y = " + get16(src) + ";\n";
  body += "const unsigned r = x + y;\nconst unsigned res = r & 0xFFFFu;\ns->wz = (uint16_t)(x + 1u);\n";
  body += set16(dst, "res");
  body += "s->f = (uint8_t)((s->f & 0xC4u) | ((res >> 8) & 0x28u) | (((x ^ y ^ res) >> 8) & 0x10u) | ((r >> 16) & 1u));\n";
  return effect(block(body), true);
}

Lowered lower_adc_sbc16(const LowerContext& c) {
  const P16 src = pick16(c.form.src, rr_field(c));
  const bool sub = c.form.mnemonic == Mnemonic::sbc;
  std::string body = "const unsigned x = " + get16(P16::hl) + ";\nconst unsigned y = " + get16(src) + ";\n";
  body += "const unsigned cin = s->f & 1u;\n";
  body += std::string("const unsigned r = ") + (sub ? "x - y - cin" : "x + y + cin") + ";\n";
  body += "const unsigned res = r & 0xFFFFu;\ns->wz = (uint16_t)(x + 1u);\n";
  body += set16(P16::hl, "res");
  const std::string overflow = sub ? "((((x ^ y) & (x ^ res)) & 0x8000u) >> 13)" : "(((~(x ^ y) & (x ^ res)) & 0x8000u) >> 13)";
  body += "s->f = (uint8_t)(((res >> 8) & 0xA8u) | (res == 0u ? 0x40u : 0u) | (((x ^ y ^ res) >> 8) & 0x10u) | " + overflow +
          (sub ? " | 0x02u" : "") + " | ((r >> 16) & 1u));\n";
  return effect(block(body), true);
}

// ---------------------------------------------------------------------------------------------- rows
#define ALU_ROWS(mn)                                                                                              \
  {Space::base, Mnemonic::mn, Operand::a, Operand::r, lower_alu8},                                                \
      {Space::base, Mnemonic::mn, Operand::a, Operand::hl_ind, lower_alu8},                                       \
      {Space::base, Mnemonic::mn, Operand::a, Operand::n, lower_alu8},                                            \
      {Space::dd, Mnemonic::mn, Operand::a, Operand::ix_d, lower_alu8},                                           \
      {Space::dd, Mnemonic::mn, Operand::a, Operand::ix_half, lower_alu8},                                        \
      {Space::fd, Mnemonic::mn, Operand::a, Operand::iy_d, lower_alu8},                                           \
      {Space::fd, Mnemonic::mn, Operand::a, Operand::iy_half, lower_alu8}

#define INCDEC_ROWS(mn)                                                                                           \
  {Space::base, Mnemonic::mn, Operand::r, Operand::none, lower_incdec8},                                          \
      {Space::base, Mnemonic::mn, Operand::hl_ind, Operand::none, lower_incdec8},                                 \
      {Space::dd, Mnemonic::mn, Operand::ix_d, Operand::none, lower_incdec8},                                     \
      {Space::dd, Mnemonic::mn, Operand::ix_half, Operand::none, lower_incdec8},                                  \
      {Space::fd, Mnemonic::mn, Operand::iy_d, Operand::none, lower_incdec8},                                     \
      {Space::fd, Mnemonic::mn, Operand::iy_half, Operand::none, lower_incdec8},                                  \
      {Space::base, Mnemonic::mn, Operand::rr, Operand::none, lower_inc_dec16},                                   \
      {Space::dd, Mnemonic::mn, Operand::ix, Operand::none, lower_inc_dec16},                                     \
      {Space::fd, Mnemonic::mn, Operand::iy, Operand::none, lower_inc_dec16}

constexpr LoweringRow kRows[] = {
    // 8-bit LD (LD r,r' and LD r,n are the T003 seed rows, now served by the shared access helper).
    {Space::base, Mnemonic::ld, Operand::r, Operand::r, lower_ld8},
    {Space::base, Mnemonic::ld, Operand::r, Operand::n, lower_ld8},
    {Space::base, Mnemonic::ld, Operand::r, Operand::hl_ind, lower_ld8},
    {Space::base, Mnemonic::ld, Operand::hl_ind, Operand::r, lower_ld8},
    {Space::base, Mnemonic::ld, Operand::hl_ind, Operand::n, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::r, Operand::ix_d, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::ix_d, Operand::r, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::ix_d, Operand::n, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::r, Operand::ix_half, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::ix_half, Operand::r, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::ix_half, Operand::ix_half, lower_ld8},
    {Space::dd, Mnemonic::ld, Operand::ix_half, Operand::n, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::r, Operand::iy_d, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::iy_d, Operand::r, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::iy_d, Operand::n, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::r, Operand::iy_half, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::iy_half, Operand::r, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::iy_half, Operand::iy_half, lower_ld8},
    {Space::fd, Mnemonic::ld, Operand::iy_half, Operand::n, lower_ld8},
    {Space::base, Mnemonic::ld, Operand::a, Operand::rr_ind, lower_ld_a_rr_ind},
    {Space::base, Mnemonic::ld, Operand::rr_ind, Operand::a, lower_ld_rr_ind_a},
    {Space::base, Mnemonic::ld, Operand::a, Operand::nn_ind, lower_ld_a_nn},
    {Space::base, Mnemonic::ld, Operand::nn_ind, Operand::a, lower_ld_nn_a},
    // 16-bit LD.
    {Space::base, Mnemonic::ld, Operand::rr, Operand::nn, lower_ld_rr_nn},
    {Space::dd, Mnemonic::ld, Operand::ix, Operand::nn, lower_ld_rr_nn},
    {Space::fd, Mnemonic::ld, Operand::iy, Operand::nn, lower_ld_rr_nn},
    {Space::base, Mnemonic::ld, Operand::nn_ind, Operand::hl, lower_ld16_mem},
    {Space::base, Mnemonic::ld, Operand::hl, Operand::nn_ind, lower_ld16_mem},
    {Space::ed, Mnemonic::ld, Operand::nn_ind, Operand::hl, lower_ld16_mem},
    {Space::ed, Mnemonic::ld, Operand::hl, Operand::nn_ind, lower_ld16_mem},
    {Space::ed, Mnemonic::ld, Operand::nn_ind, Operand::rr_ed, lower_ld16_mem},
    {Space::ed, Mnemonic::ld, Operand::rr_ed, Operand::nn_ind, lower_ld16_mem},
    {Space::dd, Mnemonic::ld, Operand::nn_ind, Operand::ix, lower_ld16_mem},
    {Space::dd, Mnemonic::ld, Operand::ix, Operand::nn_ind, lower_ld16_mem},
    {Space::fd, Mnemonic::ld, Operand::nn_ind, Operand::iy, lower_ld16_mem},
    {Space::fd, Mnemonic::ld, Operand::iy, Operand::nn_ind, lower_ld16_mem},
    {Space::base, Mnemonic::ld, Operand::sp, Operand::hl, lower_ld_sp},
    {Space::dd, Mnemonic::ld, Operand::sp, Operand::ix, lower_ld_sp},
    {Space::fd, Mnemonic::ld, Operand::sp, Operand::iy, lower_ld_sp},
    // 8-bit arithmetic and logic.
    ALU_ROWS(add),
    ALU_ROWS(adc),
    ALU_ROWS(sub),
    ALU_ROWS(sbc),
    ALU_ROWS(and_),
    ALU_ROWS(xor_),
    ALU_ROWS(or_),
    ALU_ROWS(cp),
    INCDEC_ROWS(inc),
    INCDEC_ROWS(dec),
    {Space::ed, Mnemonic::neg, Operand::a, Operand::none, lower_neg},
    {Space::base, Mnemonic::daa, Operand::a, Operand::none, lower_daa},
    {Space::base, Mnemonic::cpl, Operand::a, Operand::none, lower_cpl},
    {Space::base, Mnemonic::scf, Operand::a, Operand::none, lower_scf},
    {Space::base, Mnemonic::ccf, Operand::a, Operand::none, lower_ccf},
    {Space::base, Mnemonic::rlca, Operand::a, Operand::none, lower_rot_a},
    {Space::base, Mnemonic::rrca, Operand::a, Operand::none, lower_rot_a},
    {Space::base, Mnemonic::rla, Operand::a, Operand::none, lower_rot_a},
    {Space::base, Mnemonic::rra, Operand::a, Operand::none, lower_rot_a},
    // 16-bit arithmetic.
    {Space::base, Mnemonic::add, Operand::hl, Operand::rr, lower_add16},
    {Space::dd, Mnemonic::add, Operand::ix, Operand::rr_ix, lower_add16},
    {Space::fd, Mnemonic::add, Operand::iy, Operand::rr_iy, lower_add16},
    {Space::ed, Mnemonic::adc, Operand::hl, Operand::rr, lower_adc_sbc16},
    {Space::ed, Mnemonic::sbc, Operand::hl, Operand::rr, lower_adc_sbc16},
};

}  // namespace

std::span<const LoweringRow> data_alu_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
