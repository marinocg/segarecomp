#pragma once

// Z80 lowering rows (SEG-008-T003). One row lowers one legal form (space, mnemonic, dst, src) to the C statements
// of its architectural effect over the runtime ABI (`runtime/z80_runtime.h`). The image emitter (z80.hpp) owns the
// owner prologue, R/T-state accounting, Q update, fall-through and direct binding; a row owns only the effect.
//
// Extension protocol (docs/testing/z80-conformance-harness.md, "Adding a family"): a family adds rows in its own
// file `src/z80_lower_<family>.cpp` (`data_alu`, `control_stack`, `cb_bit_prefix`, `ed_io_interrupt`), each exposing
// one `<family>_lowering_rows()` accessor that is already listed in `z80_lowering.cpp`. Nothing else is shared.

#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "segarecomp/cpu/z80/decode.hpp"

namespace segarecomp::codegen::z80 {

// How control continues after the instruction.
enum class Flow : std::uint8_t {
  fallthrough,           // next instruction; the emitter may bind directly to an invariant-window successor
  dispatch_fallthrough,  // PC := fall-through, then return to the dispatcher (HALT)
  dispatch,              // the statements assigned `s->pc`; return to the dispatcher (jumps, calls, returns)
};

struct Lowered {
  std::string statements;      // C statements over `s` (Z80State*) and `rt` (Z80Runtime*); may use nested blocks
  bool writes_flags = false;   // true: Q := F after the statements, false: Q := 0
  Flow flow = Flow::fallthrough;
  // Empty: the emitter accounts the form's fixed T-states (primary + 4 per ignored/superseded prefix). Conditional and
  // repeat forms provide a C expression of the T-states of the path actually taken (prefix cost included).
  std::string cycles_expression;
};

// Fixed-timing, fall-through effect (the common case).
inline Lowered effect(std::string statements, bool writes_flags = false) {
  Lowered lowered;
  lowered.statements = std::move(statements);
  lowered.writes_flags = writes_flags;
  return lowered;
}

// What a row sees. PC-dependent values must come from these expressions, never from literals: window-relative
// owners are shared by every window of an image and derive them from the window base at run time.
struct LowerContext {
  const cpu::z80::DecodedInstruction& instruction;
  const cpu::z80::FormDescriptor& form;
  std::string start_pc;  // C expression (uint16_t) of the address of the instruction's first byte (first prefix)
  std::string next_pc;   // C expression (uint16_t) of the address after the instruction (wrapping)
};

using LowerFn = Lowered (*)(const LowerContext&);

struct LoweringRow {
  cpu::z80::Space space;
  cpu::z80::Mnemonic mnemonic;
  cpu::z80::Operand dst;
  cpu::z80::Operand src;
  LowerFn lower;
};

// Per-family accessors, one file each (src/z80_lower_<family>.cpp).
std::span<const LoweringRow> data_alu_lowering_rows();
std::span<const LoweringRow> control_stack_lowering_rows();
std::span<const LoweringRow> cb_bit_prefix_lowering_rows();
std::span<const LoweringRow> ed_io_interrupt_lowering_rows();

// The merged registry. A form claimed by two rows is a programming error (throws std::logic_error on first use).
const LoweringRow* find_lowering(const cpu::z80::FormDescriptor& form);
bool has_lowering(cpu::z80::FormId form);

// Register-field helpers shared by rows: C lvalue of the 8-bit register with 3-bit index (0=B..5=L, 7=A); index 6
// ((HL)) is not a register and returns an empty string.
std::string reg8(unsigned index);
std::string hex_literal(unsigned value, unsigned digits);

}  // namespace segarecomp::codegen::z80
