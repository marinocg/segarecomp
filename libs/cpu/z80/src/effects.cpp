#include "segarecomp/cpu/z80/effects.hpp"

#include <array>

namespace segarecomp::cpu::z80 {
namespace {

// Register fields of the canonical opcode byte (Zilog UM0080 encoding tables): r = y or z field, rr = p field.
constexpr std::array<Reg, 8> kR = {Reg::b, Reg::c, Reg::d, Reg::e, Reg::h, Reg::l, Reg::a /*unused (HL)*/, Reg::a};
constexpr std::array<Reg, 4> kRr = {Reg::bc, Reg::de, Reg::hl, Reg::sp};

ValueExpr constant(std::uint16_t value) {
  ValueExpr v;
  v.kind = ValueExpr::Kind::constant;
  v.constant = value;
  return v;
}
ValueExpr copy(Reg source) {
  ValueExpr v;
  v.kind = ValueExpr::Kind::copy;
  v.source = source;
  return v;
}
ValueExpr add(Reg source, std::int32_t delta) {
  ValueExpr v;
  v.kind = ValueExpr::Kind::add_constant;
  v.source = source;
  v.delta = delta;
  return v;
}
ValueExpr opaque() { return {}; }
AddressExpr absolute(std::uint16_t address) {
  AddressExpr a;
  a.kind = AddressExpr::Kind::absolute;
  a.absolute = address;
  return a;
}
AddressExpr relative(Reg base, std::int32_t offset) {
  AddressExpr a;
  a.kind = AddressExpr::Kind::register_relative;
  a.base = base;
  a.offset = offset;
  return a;
}
ValueExpr load(const AddressExpr& address, std::uint8_t width) {
  ValueExpr v;
  v.kind = ValueExpr::Kind::load;
  v.address = address;
  v.width = width;
  return v;
}

bool is_alu(Mnemonic m) {
  return m == Mnemonic::add || m == Mnemonic::adc || m == Mnemonic::sub || m == Mnemonic::sbc || m == Mnemonic::and_ ||
         m == Mnemonic::xor_ || m == Mnemonic::or_ || m == Mnemonic::cp;
}

class Builder {
 public:
  explicit Builder(const DecodedInstruction& instruction)
      : inst_(instruction), desc_(form_descriptor(instruction.form)) {
    effect_.fallthrough =
        static_cast<std::uint16_t>(instruction.provenance.address + logical_length(instruction));
    const std::uint8_t op = instruction.provenance.opcode;
    y_ = (op >> 3) & 7U;
    z_ = op & 7U;
  }

  Z80Effect build() {
    effect_.supported = desc_.space == Space::base ? base() : (desc_.space == Space::dd || desc_.space == Space::fd) && index();
    if (!effect_.supported) return Z80Effect{};
    return effect_;
  }

 private:
  std::uint16_t imm() const { return immediate(inst_).value_or(0); }
  std::int32_t disp() const { return displacement(inst_).value_or(0); }
  Reg ry() const { return kR[y_]; }
  Reg rz() const { return kR[z_]; }
  Reg rp() const { return kRr[y_ >> 1]; }
  void write(Reg target, const ValueExpr& value) { effect_.writes.push_back({target, value}); }
  void store(const AddressExpr& address, std::uint8_t width, const ValueExpr& value) {
    effect_.store = MemoryStore{address, width, value};
  }
  bool jump(ControlKind kind, std::uint16_t target) {
    effect_.control = kind;
    effect_.target = target;
    return true;
  }
  std::uint16_t relative_target() const {
    return static_cast<std::uint16_t>(effect_.fallthrough + static_cast<std::int8_t>(imm() & 0xFFU));
  }

  bool base() {
    const Mnemonic m = desc_.mnemonic;
    const Operand d = desc_.dst, s = desc_.src;
    using O = Operand;
    switch (m) {
      case Mnemonic::nop: return true;
      case Mnemonic::ld:
        if (d == O::r && s == O::n) return write(ry(), constant(imm())), true;
        if (d == O::r && s == O::r) return write(ry(), copy(rz())), true;
        if (d == O::r && s == O::hl_ind) return write(ry(), load(relative(Reg::hl, 0), 1)), true;
        if (d == O::hl_ind && s == O::r) return store(relative(Reg::hl, 0), 1, copy(rz())), true;
        if (d == O::hl_ind && s == O::n) return store(relative(Reg::hl, 0), 1, constant(imm())), true;
        if (d == O::rr && s == O::nn) return write(rp(), constant(imm())), true;
        if (d == O::a && s == O::rr_ind) return write(Reg::a, load(relative(rp(), 0), 1)), true;
        if (d == O::rr_ind && s == O::a) return store(relative(rp(), 0), 1, copy(Reg::a)), true;
        if (d == O::a && s == O::nn_ind) return write(Reg::a, load(absolute(imm()), 1)), true;
        if (d == O::nn_ind && s == O::a) return store(absolute(imm()), 1, copy(Reg::a)), true;
        if (d == O::hl && s == O::nn_ind) return write(Reg::hl, load(absolute(imm()), 2)), true;
        if (d == O::nn_ind && s == O::hl) return store(absolute(imm()), 2, copy(Reg::hl)), true;
        if (d == O::sp && s == O::hl) return write(Reg::sp, copy(Reg::hl)), true;
        return false;
      case Mnemonic::inc:
      case Mnemonic::dec: {
        const std::int32_t delta = m == Mnemonic::inc ? 1 : -1;
        if (d == O::r) return write(ry(), add(ry(), delta)), true;
        if (d == O::rr) return write(rp(), add(rp(), delta)), true;
        return false;
      }
      case Mnemonic::jp:
        if (d == O::none && s == O::nn) return jump(ControlKind::jump, imm());
        if (d == O::cc && s == O::nn) return jump(ControlKind::conditional_jump, imm());
        if (d == O::none && s == O::hl) {
          effect_.control = ControlKind::computed;
          effect_.computed_base = Reg::hl;
          return true;
        }
        return false;
      case Mnemonic::jr:
        return jump(d == O::none ? ControlKind::jump : ControlKind::conditional_jump, relative_target());
      case Mnemonic::djnz:
        write(Reg::b, add(Reg::b, -1));
        return jump(ControlKind::conditional_jump, relative_target());
      case Mnemonic::call:
        if (d != O::none) return false;  // CALL cc is not in the enumerated set
        write(Reg::sp, add(Reg::sp, -2));
        store(relative(Reg::sp, -2), 2, constant(effect_.fallthrough));
        return jump(ControlKind::call, imm());
      case Mnemonic::ret:
        if (d == O::cc) {
          effect_.control = ControlKind::conditional_return;
          return true;
        }
        write(Reg::sp, add(Reg::sp, 2));
        effect_.control = ControlKind::return_;
        return true;
      default:
        if (is_alu(m) && d == O::a && (s == O::r || s == O::n || s == O::hl_ind)) {
          if (m != Mnemonic::cp) write(Reg::a, opaque());
          return true;
        }
        return false;
    }
  }

  bool index() {
    const Mnemonic m = desc_.mnemonic;
    const Operand d = desc_.dst, s = desc_.src;
    using O = Operand;
    const bool iy = desc_.space == Space::fd;
    const Reg x = iy ? Reg::iy : Reg::ix;
    const O xr = iy ? O::iy : O::ix, xd = iy ? O::iy_d : O::ix_d;
    switch (m) {
      case Mnemonic::ld:
        if (d == xr && s == O::nn) return write(x, constant(imm())), true;
        if (d == xr && s == O::nn_ind) return write(x, load(absolute(imm()), 2)), true;
        if (d == O::nn_ind && s == xr) return store(absolute(imm()), 2, copy(x)), true;
        if (d == O::r && s == xd) return write(ry(), load(relative(x, disp()), 1)), true;
        if (d == xd && s == O::r) return store(relative(x, disp()), 1, copy(rz())), true;
        if (d == xd && s == O::n) return store(relative(x, disp()), 1, constant(imm())), true;
        if (d == O::sp && s == xr) return write(Reg::sp, copy(x)), true;
        return false;
      case Mnemonic::inc:
      case Mnemonic::dec:
        if (d == xr) return write(x, add(x, m == Mnemonic::inc ? 1 : -1)), true;
        return false;
      case Mnemonic::jp:
        if (d == O::none && s == xr) {
          effect_.control = ControlKind::computed;
          effect_.computed_base = x;
          return true;
        }
        return false;
      default:
        if (is_alu(m) && d == O::a && s == xd) {
          if (m != Mnemonic::cp) write(Reg::a, opaque());
          return true;
        }
        return false;
    }
  }

  const DecodedInstruction& inst_;
  const FormDescriptor& desc_;
  Z80Effect effect_{};
  unsigned y_{};
  unsigned z_{};
};

}  // namespace

const char* reg_name(Reg reg) {
  switch (reg) {
    case Reg::a: return "a";
    case Reg::b: return "b";
    case Reg::c: return "c";
    case Reg::d: return "d";
    case Reg::e: return "e";
    case Reg::h: return "h";
    case Reg::l: return "l";
    case Reg::bc: return "bc";
    case Reg::de: return "de";
    case Reg::hl: return "hl";
    case Reg::sp: return "sp";
    case Reg::ix: return "ix";
    case Reg::iy: return "iy";
  }
  return "invalid";
}

const char* control_kind_name(ControlKind kind) {
  switch (kind) {
    case ControlKind::fallthrough: return "fallthrough";
    case ControlKind::jump: return "jump";
    case ControlKind::conditional_jump: return "conditional_jump";
    case ControlKind::call: return "call";
    case ControlKind::return_: return "return";
    case ControlKind::conditional_return: return "conditional_return";
    case ControlKind::computed: return "computed";
  }
  return "invalid";
}

Z80Effect project_effect(const DecodedInstruction& instruction) {
  if (instruction.form == kNoForm) return {};
  return Builder(instruction).build();
}

}  // namespace segarecomp::cpu::z80
