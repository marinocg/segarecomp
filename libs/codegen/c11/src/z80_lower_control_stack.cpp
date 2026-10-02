// Control-flow, stack and exchange family rows (SEG-008-T005 owns the growth of this file).
//
// Every runtime-selected or direct transfer assigns `s->pc` and returns to the dispatcher (Flow::dispatch), which
// resolves the target through the exact compiled-entry lookup of the current code-image identity: no transfer is
// bound statically here, so a mapping-sensitive target is always resolved against the identity the host reports.
// PC-dependent values come only from LowerContext::start_pc/next_pc (window-relative owners derive them from the
// window base at run time). MEMPTR (WZ) follows the public Z80 documentation (Young, "The Undocumented Z80").
#include <cstdint>
#include <string>

#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;
using cpu::z80::TimingOutcome;

Lowered lower_nop(const LowerContext&) { return effect(""); }

// C condition over `s->f` for the 3-bit cc field: NZ Z NC C PO PE P M.
std::string condition(unsigned cc) {
  static const char* const text[8] = {"(s->f & 0x40u) == 0u", "(s->f & 0x40u) != 0u", "(s->f & 0x01u) == 0u",
                                      "(s->f & 0x01u) != 0u", "(s->f & 0x04u) == 0u", "(s->f & 0x04u) != 0u",
                                      "(s->f & 0x80u) == 0u", "(s->f & 0x80u) != 0u"};
  return text[cc & 7u];
}

// Signed 8-bit relative displacement of JR/DJNZ (the byte after the opcode).
std::string rel_target(const LowerContext& c) {
  return "(uint16_t)((int)(" + c.next_pc + ") + (" + operand_rel(c) + "))";
}

// Timing of a conditional form: not-taken vs taken (extra-prefix cost included by t_states).
Lowered conditional(const LowerContext& c, std::string statements) {
  Lowered lowered;
  lowered.statements = std::move(statements);
  lowered.flow = Flow::dispatch;
  lowered.cycles_expression = "z80_taken ? " + std::to_string(cpu::z80::t_states(c.instruction, TimingOutcome::alternate)) +
                              "u : " + std::to_string(cpu::z80::t_states(c.instruction, TimingOutcome::primary)) + "u";
  return lowered;
}

Lowered transfer(std::string statements) {
  Lowered lowered;
  lowered.statements = std::move(statements);
  lowered.flow = Flow::dispatch;
  return lowered;
}

Lowered lower_jp_nn(const LowerContext& c) {
  const std::string nn = operand_imm16(c);
  return transfer("s->wz = " + nn + ";\ns->pc = " + nn + ";\n");
}

Lowered lower_jp_cc_nn(const LowerContext& c) {
  const std::string nn = operand_imm16(c);
  const unsigned cc = (c.instruction.provenance.opcode >> 3) & 7u;
  return transfer("s->wz = " + nn + ";\nif (" + condition(cc) + ") s->pc = " + nn + "; else s->pc = " + c.next_pc + ";\n");
}

Lowered lower_jr(const LowerContext& c) {
  const std::string target = rel_target(c);
  return transfer("s->wz = " + target + ";\ns->pc = s->wz;\n");
}

Lowered lower_jr_cc(const LowerContext& c) {
  const unsigned cc = (c.instruction.provenance.opcode >> 3) & 3u;
  return conditional(c, "const int z80_taken = " + condition(cc) + ";\nif (z80_taken) {\n  s->wz = " + rel_target(c) +
                            ";\n  s->pc = s->wz;\n} else {\n  s->pc = " + c.next_pc + ";\n}\n");
}

Lowered lower_djnz(const LowerContext& c) {
  return conditional(c, "s->b = (uint8_t)(s->b - 1u);\nconst int z80_taken = s->b != 0u;\nif (z80_taken) {\n  s->wz = " +
                            rel_target(c) + ";\n  s->pc = s->wz;\n} else {\n  s->pc = " + c.next_pc + ";\n}\n");
}

Lowered lower_call(const LowerContext& c) {
  const std::string nn = operand_imm16(c);
  return transfer("z80_push16(rt, " + c.next_pc + ");\ns->wz = " + nn + ";\ns->pc = " + nn + ";\n");
}

Lowered lower_call_cc(const LowerContext& c) {
  const std::string nn = operand_imm16(c);
  const unsigned cc = (c.instruction.provenance.opcode >> 3) & 7u;
  return conditional(c, "const int z80_taken = " + condition(cc) + ";\ns->wz = " + nn + ";\nif (z80_taken) {\n  z80_push16(rt, " +
                            c.next_pc + ");\n  s->pc = " + nn + ";\n} else {\n  s->pc = " + c.next_pc + ";\n}\n");
}

Lowered lower_ret(const LowerContext&) {
  return transfer("s->pc = z80_read16(rt, s->sp);\ns->sp = (uint16_t)(s->sp + 2u);\ns->wz = s->pc;\n");
}

Lowered lower_ret_cc(const LowerContext& c) {
  const unsigned cc = (c.instruction.provenance.opcode >> 3) & 7u;
  return conditional(c, "const int z80_taken = " + condition(cc) +
                            ";\nif (z80_taken) {\n  s->pc = z80_read16(rt, s->sp);\n  s->sp = (uint16_t)(s->sp + 2u);\n"
                            "  s->wz = s->pc;\n} else {\n  s->pc = " + c.next_pc + ";\n}\n");
}

Lowered lower_rst(const LowerContext& c) {
  const std::string target = hex_literal(c.instruction.provenance.opcode & 0x38u, 4);
  return transfer("z80_push16(rt, " + c.next_pc + ");\ns->wz = " + target + ";\ns->pc = " + target + ";\n");
}

// JP (HL)/(IX)/(IY): PC := register; MEMPTR is not modified.
Lowered lower_jp_hl(const LowerContext&) { return transfer("s->pc = (uint16_t)(((unsigned)s->h << 8) | s->l);\n"); }
Lowered lower_jp_ix(const LowerContext&) { return transfer("s->pc = s->ix;\n"); }
Lowered lower_jp_iy(const LowerContext&) { return transfer("s->pc = s->iy;\n"); }

// PUSH/POP qq (opcode bits 5..4: BC DE HL AF).
Lowered lower_push_qq(const LowerContext& c) {
  static const char* const hi[4] = {"s->b", "s->d", "s->h", "s->a"};
  static const char* const lo[4] = {"s->c", "s->e", "s->l", "s->f"};
  const unsigned qq = (c.instruction.provenance.opcode >> 4) & 3u;
  return effect(std::string("z80_push16(rt, (uint16_t)(((unsigned)") + hi[qq] + " << 8) | " + lo[qq] + "));\n");
}

Lowered lower_pop_qq(const LowerContext& c) {
  static const char* const hi[4] = {"s->b", "s->d", "s->h", "s->a"};
  static const char* const lo[4] = {"s->c", "s->e", "s->l", "s->f"};
  const unsigned qq = (c.instruction.provenance.opcode >> 4) & 3u;
  return effect("{\n  const uint16_t value = z80_read16(rt, s->sp);\n  s->sp = (uint16_t)(s->sp + 2u);\n  " + std::string(hi[qq]) +
                    " = (uint8_t)(value >> 8);\n  " + lo[qq] + " = (uint8_t)value;\n}\n");  // POP AF leaves Q clear (oracle-verified)
}

Lowered lower_push_ix(const LowerContext&) { return effect("z80_push16(rt, s->ix);\n"); }
Lowered lower_push_iy(const LowerContext&) { return effect("z80_push16(rt, s->iy);\n"); }
Lowered lower_pop_ix(const LowerContext&) {
  return effect("s->ix = z80_read16(rt, s->sp);\ns->sp = (uint16_t)(s->sp + 2u);\n");
}
Lowered lower_pop_iy(const LowerContext&) {
  return effect("s->iy = z80_read16(rt, s->sp);\ns->sp = (uint16_t)(s->sp + 2u);\n");
}

// EX (SP),rr: read low/high, write the register's high at SP+1 then low at SP, register := read value, MEMPTR := value.
std::string ex_sp(const std::string& reg_hi, const std::string& reg_lo, const std::string& reg16_assign) {
  return "{\n  const uint16_t value = z80_read16(rt, s->sp);\n  z80_write(rt, (uint16_t)(s->sp + 1u), " + reg_hi +
         ");\n  z80_write(rt, s->sp, " + reg_lo + ");\n  " + reg16_assign + "\n  s->wz = value;\n}\n";
}
Lowered lower_ex_sp_hl(const LowerContext&) {
  return effect(ex_sp("s->h", "s->l", "s->h = (uint8_t)(value >> 8);\n  s->l = (uint8_t)value;"));
}
Lowered lower_ex_sp_ix(const LowerContext&) {
  return effect(ex_sp("(uint8_t)(s->ix >> 8)", "(uint8_t)s->ix", "s->ix = value;"));
}
Lowered lower_ex_sp_iy(const LowerContext&) {
  return effect(ex_sp("(uint8_t)(s->iy >> 8)", "(uint8_t)s->iy", "s->iy = value;"));
}

Lowered lower_ex_af(const LowerContext&) {
  return effect("{\n  const uint8_t ta = s->a;\n  const uint8_t tf = s->f;\n  s->a = s->a2;\n  s->f = s->f2;\n  s->a2 = ta;\n  s->f2 = tf;\n}\n");
}
Lowered lower_exx(const LowerContext&) {
  return effect(
      "{\n  uint8_t t;\n  t = s->b; s->b = s->b2; s->b2 = t;\n  t = s->c; s->c = s->c2; s->c2 = t;\n"
      "  t = s->d; s->d = s->d2; s->d2 = t;\n  t = s->e; s->e = s->e2; s->e2 = t;\n"
      "  t = s->h; s->h = s->h2; s->h2 = t;\n  t = s->l; s->l = s->l2; s->l2 = t;\n}\n");
}
Lowered lower_ex_de_hl(const LowerContext&) {
  return effect("{\n  uint8_t t;\n  t = s->d; s->d = s->h; s->h = t;\n  t = s->e; s->e = s->l; s->l = t;\n}\n");
}

constexpr LoweringRow kRows[] = {
    {Space::base, Mnemonic::nop, Operand::none, Operand::none, lower_nop},
    {Space::base, Mnemonic::jp, Operand::none, Operand::nn, lower_jp_nn},
    {Space::base, Mnemonic::jp, Operand::cc, Operand::nn, lower_jp_cc_nn},
    {Space::base, Mnemonic::jr, Operand::none, Operand::e, lower_jr},
    {Space::base, Mnemonic::jr, Operand::cc4, Operand::e, lower_jr_cc},
    {Space::base, Mnemonic::djnz, Operand::none, Operand::e, lower_djnz},
    {Space::base, Mnemonic::call, Operand::none, Operand::nn, lower_call},
    {Space::base, Mnemonic::call, Operand::cc, Operand::nn, lower_call_cc},
    {Space::base, Mnemonic::ret, Operand::none, Operand::none, lower_ret},
    {Space::base, Mnemonic::ret, Operand::cc, Operand::none, lower_ret_cc},
    {Space::base, Mnemonic::rst, Operand::none, Operand::p, lower_rst},
    {Space::base, Mnemonic::jp, Operand::none, Operand::hl, lower_jp_hl},
    {Space::dd, Mnemonic::jp, Operand::none, Operand::ix, lower_jp_ix},
    {Space::fd, Mnemonic::jp, Operand::none, Operand::iy, lower_jp_iy},
    {Space::base, Mnemonic::push, Operand::none, Operand::rr2, lower_push_qq},
    {Space::base, Mnemonic::pop, Operand::rr2, Operand::none, lower_pop_qq},
    {Space::dd, Mnemonic::push, Operand::none, Operand::ix, lower_push_ix},
    {Space::fd, Mnemonic::push, Operand::none, Operand::iy, lower_push_iy},
    {Space::dd, Mnemonic::pop, Operand::ix, Operand::none, lower_pop_ix},
    {Space::fd, Mnemonic::pop, Operand::iy, Operand::none, lower_pop_iy},
    {Space::base, Mnemonic::ex, Operand::sp_ind, Operand::hl, lower_ex_sp_hl},
    {Space::dd, Mnemonic::ex, Operand::sp_ind, Operand::ix, lower_ex_sp_ix},
    {Space::fd, Mnemonic::ex, Operand::sp_ind, Operand::iy, lower_ex_sp_iy},
    {Space::base, Mnemonic::ex, Operand::af, Operand::af_alt, lower_ex_af},
    {Space::base, Mnemonic::exx, Operand::none, Operand::none, lower_exx},
    {Space::base, Mnemonic::ex, Operand::de, Operand::hl, lower_ex_de_hl},
};

}  // namespace

std::span<const LoweringRow> control_stack_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
