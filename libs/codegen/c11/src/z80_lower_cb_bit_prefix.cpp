// CB / DDCB / FDCB rotate, shift and bit family (SEG-008-T006 owns this file).
//
// One table-driven lowering serves every row: the row key (space, mnemonic, operand classes) selects the operation
// and the operand location, the opcode byte supplies the bit number and (for the register copy-back forms) the
// copied register. There is no per-opcode special case. Fixed T-states, the M1/R count (DDCB/FDCB: two M1 cycles,
// the displacement and opcode bytes are ordinary reads) and Q for flag writers come from the emitter and the
// form descriptor. DD/FD prefix chains and ignored prefixes are handled by the decoder (`extra_prefix_count`).
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

bool is_bit_op(Mnemonic m) { return m == Mnemonic::bit || m == Mnemonic::res || m == Mnemonic::set; }

// Statements computing `r` (result) and `cy` (carry-out) of a rotate/shift of `v` (F still holds the old carry).
std::string rotate_shift(Mnemonic m) {
  switch (m) {
    case Mnemonic::rlc: return "cy = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | cy);\n";
    case Mnemonic::rrc: return "cy = (uint8_t)(v & 1u); r = (uint8_t)((v >> 1) | (cy << 7));\n";
    case Mnemonic::rl: return "cy = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | (s->f & 1u));\n";
    case Mnemonic::rr: return "cy = (uint8_t)(v & 1u); r = (uint8_t)((v >> 1) | ((s->f & 1u) << 7));\n";
    case Mnemonic::sla: return "cy = (uint8_t)(v >> 7); r = (uint8_t)(v << 1);\n";
    case Mnemonic::sra: return "cy = (uint8_t)(v & 1u); r = (uint8_t)((v >> 1) | (v & 0x80u));\n";
    case Mnemonic::sll: return "cy = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | 1u);\n";
    default: return "cy = (uint8_t)(v & 1u); r = (uint8_t)(v >> 1);\n";  // srl
  }
}

Lowered lower_cb_bit_prefix(const LowerContext& c) {
  const Mnemonic m = c.form.mnemonic;
  const unsigned op = c.instruction.provenance.opcode;
  const bool bit_op = is_bit_op(m);
  const Operand loc = bit_op ? c.form.src : c.form.dst;
  const bool indexed = loc == Operand::ix_d || loc == Operand::iy_d || loc == Operand::ix_d_copy_r ||
                       loc == Operand::iy_d_copy_r;
  const bool copy_back = loc == Operand::ix_d_copy_r || loc == Operand::iy_d_copy_r;
  const bool memory = indexed || loc == Operand::hl_ind;

  std::string out = "{\n";
  // Operand fetch: `v` is the operand value; `a` (memory forms) the effective address.
  if (loc == Operand::hl_ind) {
    out += "uint16_t a = (uint16_t)(((uint16_t)s->h << 8) | s->l);\nuint8_t v = z80_read(rt, a);\n";
  } else if (indexed) {
    const bool ix = loc == Operand::ix_d || loc == Operand::ix_d_copy_r;
    out += std::string("uint16_t a = (uint16_t)(") + (ix ? "s->ix" : "s->iy") + " + " + operand_disp16(c) +
           ");\ns->wz = a;\nuint8_t v = z80_read(rt, a);\n";
  } else {
    out += "uint8_t v = " + reg8(op & 7u) + ";\n";
  }

  if (m == Mnemonic::bit) {
    const unsigned mask = 1u << ((op >> 3) & 7u);
    // X/Y come from the operand register, or from the high byte of MEMPTR (the effective address for (IX+d)/(IY+d)).
    const std::string xy = !memory ? "v" : loc == Operand::hl_ind ? "s->wz >> 8" : "a >> 8";
    out += "{\nuint8_t t = (uint8_t)(v & " + hex_literal(mask, 2) + ");\n";
    out += "s->f = (uint8_t)((s->f & 0x01u) | 0x10u | (t ? " + std::string(mask == 0x80u ? "0x80u" : "0x00u") +
           " : 0x44u) | ((" + xy + ") & 0x28u));\n}\n";
  } else if (m == Mnemonic::res || m == Mnemonic::set) {
    const unsigned mask = 1u << ((op >> 3) & 7u);
    out += "uint8_t r = ";
    out += m == Mnemonic::set ? "(uint8_t)(v | " + hex_literal(mask, 2) + ");\n"
                              : "(uint8_t)(v & " + hex_literal(~mask & 0xFFu, 2) + ");\n";
    if (memory) out += "z80_write(rt, a, r);\n";
    if (!memory || copy_back) out += reg8(op & 7u) + " = r;\n";
  } else {
    out += "uint8_t cy; uint8_t r;\n" + rotate_shift(m);
    out += "{\nuint8_t p = r;\np = (uint8_t)(p ^ (p >> 4)); p = (uint8_t)(p ^ (p >> 2)); p = (uint8_t)(p ^ (p >> 1));\n"
           "s->f = (uint8_t)((r & 0xA8u) | (r == 0u ? 0x40u : 0u) | ((p & 1u) ? 0u : 0x04u) | cy);\n}\n";
    if (memory) out += "z80_write(rt, a, r);\n";
    if (!memory || copy_back) out += reg8(op & 7u) + " = r;\n";
  }
  out += "}\n";
  return effect(std::move(out), m != Mnemonic::res && m != Mnemonic::set);
}

struct Table {
  LoweringRow rows[64] = {};
  std::size_t count = 0;
  void add(Space space, Mnemonic m, Operand dst, Operand src) {
    rows[count++] = LoweringRow{space, m, dst, src, lower_cb_bit_prefix};
  }
};

// Rows come from the cross-product (space x operation x operand class), not from a per-opcode list.
Table build() {
  Table t;
  const Mnemonic rotates[] = {Mnemonic::rlc, Mnemonic::rrc, Mnemonic::rl, Mnemonic::rr,
                              Mnemonic::sla, Mnemonic::sra, Mnemonic::sll, Mnemonic::srl};
  const Mnemonic bits[] = {Mnemonic::bit, Mnemonic::res, Mnemonic::set};
  for (const Mnemonic m : rotates) {
    t.add(Space::cb, m, Operand::r, Operand::none);
    t.add(Space::cb, m, Operand::hl_ind, Operand::none);
    t.add(Space::ddcb, m, Operand::ix_d, Operand::none);
    t.add(Space::ddcb, m, Operand::ix_d_copy_r, Operand::none);
    t.add(Space::fdcb, m, Operand::iy_d, Operand::none);
    t.add(Space::fdcb, m, Operand::iy_d_copy_r, Operand::none);
  }
  for (const Mnemonic m : bits) {
    t.add(Space::cb, m, Operand::bit, Operand::r);
    t.add(Space::cb, m, Operand::bit, Operand::hl_ind);
    t.add(Space::ddcb, m, Operand::bit, Operand::ix_d);
    t.add(Space::fdcb, m, Operand::bit, Operand::iy_d);
    if (m != Mnemonic::bit) {  // BIT has no copy-back forms; its register-field encodings alias the (IX+d) form
      t.add(Space::ddcb, m, Operand::bit, Operand::ix_d_copy_r);
      t.add(Space::fdcb, m, Operand::bit, Operand::iy_d_copy_r);
    }
  }
  return t;
}

}  // namespace

std::span<const LoweringRow> cb_bit_prefix_lowering_rows() {
  static const Table table = build();
  return std::span<const LoweringRow>(table.rows, table.count);
}

}  // namespace segarecomp::codegen::z80
