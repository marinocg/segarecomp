#pragma once

// SEG-029-T004 (ADR 0078, abstract-analysis-core-contract.md section 5): a small CPU-owned Z80 effect/successor projection.
//
// `project_effect` describes, for an explicitly enumerated set of forms only, which registers an instruction writes and how
// (constant, register copy, register plus constant, memory load at an address expression, or an opaque value), the memory store
// it performs, and its control successors. Every other form is `supported = false`: the projection never guesses. It is derived
// from the decoded form (FormDescriptor operand classes and the canonical opcode byte register fields), never from C11 lowering
// text, and it is independent of any analysis library (ADR 0059 split: the semantics proper stay in lowering).
//
// Flags are not modelled: a conditional branch produces both edges; ALU forms write an opaque A (or nothing for CP).
//
// Supported forms (base space, including DD/FD-ignored-prefix executions of them, and the DD/FD index spaces):
//   NOP
//   LD r,n | LD r,r' | LD r,(HL) | LD (HL),r | LD (HL),n
//   LD rr,nn (BC/DE/HL/SP) | LD A,(BC)/(DE) | LD (BC)/(DE),A | LD A,(nn) | LD (nn),A | LD HL,(nn) | LD (nn),HL | LD SP,HL
//   INC r | DEC r | INC rr | DEC rr
//   ADD/ADC/SUB/SBC/AND/XOR/OR A,{r,n,(HL)} (opaque A) | CP {r,n,(HL)} (no register write)
//   JP nn | JP cc,nn | JR e | JR cc,e | DJNZ e | CALL nn | RET | RET cc | JP (HL)
//   LD IX/IY,nn | LD IX/IY,(nn) | LD (nn),IX/IY | INC/DEC IX/IY | LD r,(IX/IY+d) | LD (IX/IY+d),r | LD (IX/IY+d),n
//   LD SP,IX/IY | ADD/ADC/SUB/SBC/AND/XOR/OR A,(IX/IY+d) (opaque A) | CP (IX/IY+d) | JP (IX)/(IY)
// Everything else (ED and CB spaces, EX/EXX, PUSH/POP, block transfers, I/O, RST, HALT, CALL cc, undocumented index-half forms,
// 16-bit ADD, memory read-modify-write, ...) is unsupported.

#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/cpu/z80/decode.hpp"

namespace segarecomp::cpu::z80 {

// Architectural register names used by the projection. 8-bit: A B C D E H L. 16-bit: BC DE HL SP IX IY.
enum class Reg : std::uint8_t { a, b, c, d, e, h, l, bc, de, hl, sp, ix, iy };
inline constexpr bool is_wide(Reg reg) noexcept { return reg >= Reg::bc; }
const char* reg_name(Reg reg);

// A 16-bit logical address: an absolute constant, or a 16-bit register plus a signed offset (modulo 65,536).
struct AddressExpr {
  enum class Kind : std::uint8_t { absolute, register_relative };
  Kind kind{Kind::absolute};
  std::uint16_t absolute{};
  Reg base{Reg::hl};
  std::int32_t offset{};
  friend bool operator==(const AddressExpr&, const AddressExpr&) = default;
};

// How a written (or stored) value is formed from the instruction's input state. Every read uses the input state.
struct ValueExpr {
  enum class Kind : std::uint8_t {
    constant,      // `constant`
    copy,          // the value of register `source`
    add_constant,  // `source` + `delta`, modulo the width of the written register
    load,          // `width` little-endian bytes read at `address`
    opaque,        // the value is not modelled (an exact transfer is unsupported)
  };
  Kind kind{Kind::opaque};
  std::uint16_t constant{};
  Reg source{Reg::a};
  std::int32_t delta{};
  AddressExpr address{};
  std::uint8_t width{1};
  friend bool operator==(const ValueExpr&, const ValueExpr&) = default;
};

struct RegisterWrite {
  Reg target{Reg::a};
  ValueExpr value{};
  friend bool operator==(const RegisterWrite&, const RegisterWrite&) = default;
};

struct MemoryStore {
  AddressExpr address{};
  std::uint8_t width{1};  // bytes, little-endian
  ValueExpr value{};
  friend bool operator==(const MemoryStore&, const MemoryStore&) = default;
};

enum class ControlKind : std::uint8_t {
  fallthrough,         // successor: `fallthrough`
  jump,                // successor: `target`
  conditional_jump,    // successors: `target` and `fallthrough` (flags not modelled; DJNZ included)
  call,                // successor: `target`; `fallthrough` is the return continuation
  return_,             // no static successor
  conditional_return,  // successor: `fallthrough` (the taken return has no static successor)
  computed,            // successor: the value of 16-bit register `computed_base`
};
const char* control_kind_name(ControlKind kind);

struct Z80Effect {
  bool supported{};  // false: nothing below is meaningful; the instruction must not be followed
  std::vector<RegisterWrite> writes;  // all reads see the input state; writes are applied in order
  std::optional<MemoryStore> store;   // address and value read the input state
  ControlKind control{ControlKind::fallthrough};
  std::uint16_t fallthrough{};  // start address + logical length (modulo 65,536)
  std::uint16_t target{};
  Reg computed_base{Reg::hl};
  friend bool operator==(const Z80Effect&, const Z80Effect&) = default;
};

Z80Effect project_effect(const DecodedInstruction& instruction);

}  // namespace segarecomp::cpu::z80
