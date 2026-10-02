// ED, I/O and interrupt-state family rows (SEG-008-T007): HALT, EI/DI, IN/OUT, IM, LD I/R,A and LD A,I/R, RRD/RLD,
// RETN/RETI, the ED-undefined no-operation and the block transfer/search/I/O groups (repeat forms included).
//
// Repeat forms execute one iteration per owner run. A repeating iteration sets PC back to the ED opcode byte
// (`next_pc - 2`) and returns to the dispatcher, so the boundary logic (deadline, NMI, INT) runs between iterations and
// the interrupted instruction restarts at the same PC. Every iteration is bounded and re-fetches the ED escape (R + 2).
// Flag and MEMPTR rules follow docs/architecture/z80-cpu-contract.md sections 4-6 (including the repeating-iteration
// X/Y and INxR/OTxR H, P/V adjustments).
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

constexpr const char* kBc = "((uint16_t)(((uint16_t)s->b << 8) | s->c))";
constexpr const char* kDe = "((uint16_t)(((uint16_t)s->d << 8) | s->e))";
constexpr const char* kHl = "((uint16_t)(((uint16_t)s->h << 8) | s->l))";

// HALT: PC is already advanced past the opcode; the runtime accounts halted M1 cycles up to the deadline and leaves
// the halted state when an interrupt is accepted at a boundary (resumable `halted` outcome, ADR 0058 section 7).
Lowered lower_halt(const LowerContext&) {
  Lowered lowered;
  lowered.statements = "s->halted = 1u;\n";
  lowered.flow = Flow::dispatch_fallthrough;
  return lowered;
}

Lowered lower_di(const LowerContext&) { return effect("s->iff1 = 0u;\ns->iff2 = 0u;\n"); }

// EI: interrupts are enabled but a maskable INT is not accepted at the next boundary.
Lowered lower_ei(const LowerContext&) { return effect("s->iff1 = 1u;\ns->iff2 = 1u;\ns->int_deferral = 1u;\n"); }

// Undefined ED opcode: two-byte no-operation (8 T-states and two M1 fetches from the form descriptor).
Lowered lower_ed_nop(const LowerContext&) { return effect(""); }

Lowered lower_im(const LowerContext& c) {
  const char* mode = c.form.dst == Operand::im0 ? "0u" : c.form.dst == Operand::im1 ? "1u" : "2u";
  return effect(std::string("s->im = ") + mode + ";\n");
}

Lowered lower_ld_i_a(const LowerContext&) { return effect("s->i = s->a;\n"); }
Lowered lower_ld_r_a(const LowerContext&) { return effect("s->r = s->a;\n"); }

// LD A,I / LD A,R: P/V = IFF2; the NMOS marker lets an INT accepted at the next boundary clear P/V. R has already
// been incremented by this instruction's two M1 fetches.
Lowered lower_ld_a_ir(const LowerContext& c) {
  const std::string source = c.form.src == Operand::i ? "s->i" : "s->r";
  std::string text = "s->a = " + source + ";\n";
  text += "s->f = (uint8_t)((s->a & 0xA8u) | (s->a == 0u ? 0x40u : 0u) | (s->iff2 ? 0x04u : 0u) | (s->f & 0x01u));\n";
  text += "s->ld_a_ir = 1u;\n";
  return effect(text, true);
}

// RRD / RLD: rotate the low nibbles of A and (HL).
Lowered lower_rxd(const LowerContext& c) {
  const bool rld = c.form.mnemonic == Mnemonic::rld;
  std::string text;
  text = "{ uint16_t hl = " + std::string(kHl) + ";\n uint8_t t = z80_read(rt, hl);\n uint8_t pp;\n";
  text += " s->wz = (uint16_t)(hl + 1u);\n";
  text += rld ? " z80_write(rt, hl, (uint8_t)((t << 4) | (s->a & 0x0Fu)));\n s->a = (uint8_t)((s->a & 0xF0u) | (t >> 4));\n"
              : " z80_write(rt, hl, (uint8_t)((t >> 4) | (s->a << 4)));\n s->a = (uint8_t)((s->a & 0xF0u) | (t & 0x0Fu));\n";
  text += " pp = s->a; pp = (uint8_t)(pp ^ (pp >> 4)); pp = (uint8_t)(pp ^ (pp >> 2)); pp = (uint8_t)(pp ^ (pp >> 1));\n";
  text += " s->f = (uint8_t)((s->a & 0xA8u) | (s->a == 0u ? 0x40u : 0u) | ((pp & 1u) ? 0u : 0x04u) | (s->f & 0x01u));\n}\n";
  return effect(text, true);
}

// RETN / RETI (and their undocumented ED aliases): pop PC, IFF1 := IFF2. A maskable INT is deferred one boundary
// when the instruction changed IFF1 (contract section 4.2). The deferral bit is set whenever IFF1 was clear before:
// that is the pinned oracle's internal state and observably identical, because the deferral only matters when IFF1
// is set afterwards (IFF2 set), which with IFF1 clear before is exactly "IFF1 changed".
Lowered lower_retn_reti(const LowerContext&) {
  Lowered lowered;
  lowered.statements =
      "{ uint16_t target = z80_read16(rt, s->sp);\n"
      " s->sp = (uint16_t)(s->sp + 2u);\n"
      " s->pc = s->wz = target;\n"
      " s->int_deferral = (uint8_t)(s->iff1 == 0u);\n"
      " s->iff1 = s->iff2;\n}\n";
  lowered.flow = Flow::dispatch;
  return lowered;
}

// ---- I/O ----

Lowered lower_in_a_n(const LowerContext& c) {
  return effect("{ uint16_t port = (uint16_t)(((uint16_t)s->a << 8) | " + operand_imm8(c) + ");\n s->wz = (uint16_t)(port + 1u);\n"
                " s->a = z80_io_in(rt, port);\n}\n");
}

Lowered lower_out_n_a(const LowerContext& c) {
  return effect("{ uint16_t port = (uint16_t)(((uint16_t)s->a << 8) | " + operand_imm8(c) + ");\n"
                " s->wz = (uint16_t)(((uint16_t)s->a << 8) | " + operand_imm8_plus1(c) + ");\n"
                " z80_io_out(rt, port, s->a);\n}\n");
}

// IN r,(C) and the undocumented IN (C): flags from the input, C unchanged; the value is discarded for IN (C).
Lowered lower_in_c(const LowerContext& c) {
  const unsigned op = c.instruction.provenance.opcode;
  std::string text = "{ uint16_t port = " + std::string(kBc) + ";\n uint8_t v = z80_io_in(rt, port);\n uint8_t pp = v;\n";
  text += " s->wz = (uint16_t)(port + 1u);\n";
  text += " pp = (uint8_t)(pp ^ (pp >> 4)); pp = (uint8_t)(pp ^ (pp >> 2)); pp = (uint8_t)(pp ^ (pp >> 1));\n";
  text += " s->f = (uint8_t)((v & 0xA8u) | (v == 0u ? 0x40u : 0u) | ((pp & 1u) ? 0u : 0x04u) | (s->f & 0x01u));\n";
  if (c.form.src == Operand::c_port && c.form.dst == Operand::r) text += " " + reg8((op >> 3) & 7u) + " = v;\n";
  text += "}\n";
  return effect(text, true);
}

Lowered lower_out_c(const LowerContext& c) {
  const unsigned op = c.instruction.provenance.opcode;
  const std::string value = c.form.src == Operand::zero ? "0u" : reg8((op >> 3) & 7u);
  return effect("{ uint16_t port = " + std::string(kBc) + ";\n s->wz = (uint16_t)(port + 1u);\n z80_io_out(rt, port, (uint8_t)(" +
                value + "));\n}\n");
}

// ---- block groups ----

std::string set_pair(const char* high, const char* low, const std::string& value) {
  return std::string("{ uint16_t pv = (uint16_t)(") + value + "); s->" + high + " = (uint8_t)(pv >> 8); s->" + low +
         " = (uint8_t)pv; }\n";
}

// C expression of the address of the ED opcode byte (the restart PC of a repeating iteration).
std::string restart_pc(const LowerContext& c) { return "(uint16_t)(" + c.next_pc + " - 2u)"; }

Lowered lower_block(const LowerContext& c) {
  const Mnemonic m = c.form.mnemonic;
  const bool increment = m == Mnemonic::ldi || m == Mnemonic::ldir || m == Mnemonic::cpi || m == Mnemonic::cpir ||
                         m == Mnemonic::ini || m == Mnemonic::inir || m == Mnemonic::outi || m == Mnemonic::otir;
  const bool repeat = m == Mnemonic::ldir || m == Mnemonic::lddr || m == Mnemonic::cpir || m == Mnemonic::cpdr ||
                      m == Mnemonic::inir || m == Mnemonic::indr || m == Mnemonic::otir || m == Mnemonic::otdr;
  const bool transfer = m == Mnemonic::ldi || m == Mnemonic::ldd || m == Mnemonic::ldir || m == Mnemonic::lddr;
  const bool search = m == Mnemonic::cpi || m == Mnemonic::cpd || m == Mnemonic::cpir || m == Mnemonic::cpdr;
  const bool input = m == Mnemonic::ini || m == Mnemonic::ind || m == Mnemonic::inir || m == Mnemonic::indr;
  const std::string step = increment ? "+ 1u" : "- 1u";
  const std::string back = restart_pc(c);
  std::string t = "{\n uint16_t restart = " + back + ";\n (void)restart;\n";
  auto advance_hl = [&] { t += " " + set_pair("h", "l", std::string(kHl) + " " + step); };

  if (transfer) {
    t += " uint8_t v = z80_read(rt, " + std::string(kHl) + ");\n";
    t += " z80_write(rt, " + std::string(kDe) + ", v);\n";
    advance_hl();
    t += " " + set_pair("d", "e", std::string(kDe) + " " + step);
    t += " " + set_pair("b", "c", std::string(kBc) + " - 1u");
    t += " uint8_t n = (uint8_t)(v + s->a);\n";
    t += " uint8_t bcnz = (uint8_t)((s->b | s->c) != 0u);\n";
    if (repeat) {
      t += " if (bcnz) {\n"
           "  s->f = (uint8_t)((s->f & 0xC1u) | ((restart >> 8) & 0x28u) | 0x04u);\n"
           "  s->wz = (uint16_t)(restart + 1u);\n  s->pc = restart;\n } else {\n"
           "  s->f = (uint8_t)((s->f & 0xC1u) | ((n & 2u) << 4) | (n & 8u));\n  s->pc = " + c.next_pc + ";\n }\n";
    } else {
      t += " s->f = (uint8_t)((s->f & 0xC1u) | ((n & 2u) << 4) | (n & 8u) | (bcnz ? 0x04u : 0u));\n";
    }
  } else if (search) {
    t += " uint8_t v = z80_read(rt, " + std::string(kHl) + ");\n";
    t += " uint8_t t0 = (uint8_t)(s->a - v);\n";
    t += " uint8_t hf = (uint8_t)((s->a ^ v ^ t0) & 0x10u);\n";
    t += " uint8_t t1 = (uint8_t)(t0 - (hf >> 4));\n";
    advance_hl();
    t += " " + set_pair("b", "c", std::string(kBc) + " - 1u");
    t += " uint8_t bcnz = (uint8_t)((s->b | s->c) != 0u);\n";
    t += " uint8_t f = (uint8_t)((t0 & 0x80u) | (t0 == 0u ? 0x40u : 0u) | hf | (bcnz ? 0x04u : 0u) | 0x02u | (s->f & 0x01u));\n";
    if (repeat) {
      t += " if (t0 != 0u && bcnz) {\n"
           "  s->f = (uint8_t)(f | ((restart >> 8) & 0x28u));\n  s->wz = (uint16_t)(restart + 1u);\n  s->pc = restart;\n } else {\n"
           "  s->f = (uint8_t)(f | ((t1 & 2u) << 4) | (t1 & 8u));\n  s->wz = (uint16_t)(s->wz " + step + ");\n  s->pc = " + c.next_pc + ";\n }\n";
    } else {
      t += " s->f = (uint8_t)(f | ((t1 & 2u) << 4) | (t1 & 8u));\n s->wz = (uint16_t)(s->wz " + step + ");\n";
    }
  } else {
    // INI/IND(R) and OUTI/OUTD(R): B decremented, port = BC; INx drives B before its decrement, OUTx after it.
    t += " uint8_t v;\n uint16_t tt;\n";
    if (input) {
      t += " uint16_t port = " + std::string(kBc) + ";\n";
      t += " v = z80_io_in(rt, port);\n";
      t += " tt = (uint16_t)(v + (uint8_t)(s->c " + step + "));\n";
      t += " z80_write(rt, " + std::string(kHl) + ", v);\n";
      advance_hl();
      t += " s->wz = (uint16_t)(port " + step + ");\n";
      t += " s->b = (uint8_t)(s->b - 1u);\n";
    } else {
      t += " v = z80_read(rt, " + std::string(kHl) + ");\n";
      advance_hl();
      t += " tt = (uint16_t)(v + s->l);\n";
      t += " s->b = (uint8_t)(s->b - 1u);\n";
      t += " { uint16_t port = " + std::string(kBc) + ";\n  z80_io_out(rt, port, v);\n";
      t += "  s->wz = (uint16_t)(port " + step + ");\n";
      t += " }\n";
    }
    t += " uint8_t b = s->b;\n uint8_t nf = (uint8_t)((v >> 6) & 2u);\n uint8_t hcf = (uint8_t)(tt > 255u ? 1u : 0u);\n"
         " uint8_t p = (uint8_t)((tt & 7u) ^ b);\n uint8_t pp;\n";
    auto parity_of = [&](const std::string& expr) {
      return "pp = (uint8_t)(" + expr + "); pp = (uint8_t)(pp ^ (pp >> 4)); pp = (uint8_t)(pp ^ (pp >> 2)); pp = (uint8_t)(pp ^ (pp >> 1));";
    };
    if (!repeat) {
      t += " " + parity_of("p") + "\n";
      t += " s->f = (uint8_t)((b & 0xA8u) | (b == 0u ? 0x40u : 0u) | ((pp & 1u) ? 0u : 0x04u) | (hcf ? 0x11u : 0u) | nf);\n";
    } else {
      t += " if (b != 0u) {\n  uint8_t f = (uint8_t)((b & 0x80u) | ((restart >> 8) & 0x28u) | nf);\n";
      t += "  if (hcf) {\n   f = (uint8_t)(f | 0x01u);\n   if (nf) {\n    f = (uint8_t)(f | ((b & 0x0Fu) == 0u ? 0x10u : 0u));\n    " +
           parity_of("p ^ ((b - 1u) & 7u)") + "\n   } else {\n    f = (uint8_t)(f | ((b & 0x0Fu) == 0x0Fu ? 0x10u : 0u));\n    " +
           parity_of("p ^ ((b + 1u) & 7u)") + "\n   }\n  } else {\n   " + parity_of("p ^ (b & 7u)") + "\n  }\n";
      t += "  f = (uint8_t)(f | ((pp & 1u) ? 0u : 0x04u));\n  s->f = f;\n  s->wz = (uint16_t)(restart + 1u);\n  s->pc = restart;\n } else {\n";
      t += "  " + parity_of("p") + "\n";
      t += "  s->f = (uint8_t)(0x40u | (hcf ? 0x11u : 0u) | ((pp & 1u) ? 0u : 0x04u) | nf);\n  s->pc = " + c.next_pc + ";\n }\n";
    }
  }
  t += "}\n";
  Lowered lowered;
  lowered.statements = t;
  lowered.writes_flags = true;
  if (repeat) {
    lowered.flow = Flow::dispatch;
    const unsigned extra = c.instruction.extra_prefix_count * 4u;
    lowered.cycles_expression =
        std::string("(s->pc == ") + c.next_pc + " ? 16u : 21u) + " + std::to_string(extra) + "u";
  }
  return lowered;
}

constexpr LoweringRow kRows[] = {
    {Space::base, Mnemonic::halt, Operand::none, Operand::none, lower_halt},
    {Space::base, Mnemonic::di, Operand::none, Operand::none, lower_di},
    {Space::base, Mnemonic::ei, Operand::none, Operand::none, lower_ei},
    {Space::base, Mnemonic::in, Operand::a, Operand::n_port, lower_in_a_n},
    {Space::base, Mnemonic::out, Operand::n_port, Operand::a, lower_out_n_a},
    {Space::ed, Mnemonic::nop, Operand::none, Operand::ed_undefined, lower_ed_nop},
    {Space::ed, Mnemonic::in, Operand::r, Operand::c_port, lower_in_c},
    {Space::ed, Mnemonic::in, Operand::flags_only, Operand::c_port, lower_in_c},
    {Space::ed, Mnemonic::out, Operand::c_port, Operand::r, lower_out_c},
    {Space::ed, Mnemonic::out, Operand::c_port, Operand::zero, lower_out_c},
    {Space::ed, Mnemonic::retn, Operand::none, Operand::none, lower_retn_reti},
    {Space::ed, Mnemonic::reti, Operand::none, Operand::none, lower_retn_reti},
    {Space::ed, Mnemonic::im, Operand::im0, Operand::none, lower_im},
    {Space::ed, Mnemonic::im, Operand::im1, Operand::none, lower_im},
    {Space::ed, Mnemonic::im, Operand::im2, Operand::none, lower_im},
    {Space::ed, Mnemonic::ld, Operand::i, Operand::a, lower_ld_i_a},
    {Space::ed, Mnemonic::ld, Operand::r_refresh, Operand::a, lower_ld_r_a},
    {Space::ed, Mnemonic::ld, Operand::a, Operand::i, lower_ld_a_ir},
    {Space::ed, Mnemonic::ld, Operand::a, Operand::r_refresh, lower_ld_a_ir},
    {Space::ed, Mnemonic::rrd, Operand::a, Operand::hl_ind, lower_rxd},
    {Space::ed, Mnemonic::rld, Operand::a, Operand::hl_ind, lower_rxd},
    {Space::ed, Mnemonic::ldi, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::ldd, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::ldir, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::lddr, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::cpi, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::cpd, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::cpir, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::cpdr, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::ini, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::ind, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::inir, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::indr, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::outi, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::outd, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::otir, Operand::none, Operand::none, lower_block},
    {Space::ed, Mnemonic::otdr, Operand::none, Operand::none, lower_block},
};

}  // namespace

std::span<const LoweringRow> ed_io_interrupt_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
