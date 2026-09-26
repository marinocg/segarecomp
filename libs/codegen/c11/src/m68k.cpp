#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <algorithm>

namespace segarecomp {



namespace {
// Local formatting helper for the codegen (C11 lowering) section below only;
// the effects/CCR layer this project also uses has its own copy in
// cpu/m68k/effects.hpp (segarecomp::m68k_hex_literal), since the two live in
// separate translation units after SEG-014-T002's module split.
std::string hex(std::uint32_t value, unsigned width) { std::ostringstream out; out << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value; return out.str(); }
} // namespace

namespace {
// SEG-007-T023: the one shared general EA C-lowering layer, reused by every
// whitelisted TST/MOVE/MOVEA/CLR kind's emission case below (decision #3:
// static-vs-runtime EA split). Dn/An/absolute/pc_disp16/immediate are
// resolved without any guard (either trivial register access or an
// already-validated compile-time-constant address, exactly like the
// shared Batch-A static-EA cases do); (An)/(An)+/-(An)/d16(An) emit the
// same runtime bounds-check-guard convention JSR/RTS already use
// (`if (<out of range>) { return 1; }`), generalized to a runtime-computed
// address, immediately before the access, with any postincrement/
// predecrement register mutation ordered exactly per the contract (postinc:
// read at the pre-mutation address, then increment; predec: decrement
// first, then access at the new address).

// Sign-extends a `size`-wide operand expression to a canonical 32-bit value.
// Its original consumer feeds this into m68k_move_result_ccr's existing
// N(bit31)/Z(whole-value) contract, so a byte/word TST/MOVE's N/Z reflect
// bit7/bit15 of the correctly-sized operand rather than always bit31 of an
// unmasked 32-bit register/memory read -- never used there for the data
// actually written/tested itself (which retains its raw, non-sign-extended
// bits). SEG-007-T025 (Batch C, C5c1/C5c2 audit): MOVEM.W memory->register
// is a second, legitimate consumer that DOES use this for the actual
// stored register value (every loaded WORD sign-extends to 32 bits for
// both Dn and An) -- the identical word/long-word formula applies
// regardless of which fact it ultimately feeds, so this remains the one
// shared sign-extension owner rather than a second hand-written copy.
[[nodiscard]] std::string m68k_sign_extend_expr(const std::string &expr, M68kMemoryAccessWidth size) {
  if (size == M68kMemoryAccessWidth::byte) return "(uint32_t)(int32_t)(int8_t)(" + expr + ")";
  if (size == M68kMemoryAccessWidth::word) return "(uint32_t)(int32_t)(int16_t)(" + expr + ")";
  return "(" + expr + ")";
}

[[nodiscard]] std::string m68k_mask_hex(M68kMemoryAccessWidth size) {
  switch (size) {
  case M68kMemoryAccessWidth::byte: return "FF";
  case M68kMemoryAccessWidth::word: return "FFFF";
  case M68kMemoryAccessWidth::long_word: return "FFFFFFFF";
  }
  return "FFFFFFFF";
}

[[nodiscard]] std::string m68k_emit_ram_read_expr(std::string_view ram_array, const std::string &offset_expr,
                                                  M68kMemoryAccessWidth size) {
  std::ostringstream out;
  if (size == M68kMemoryAccessWidth::byte) {
    out << "(uint32_t)" << ram_array << "[" << offset_expr << "]";
  } else if (size == M68kMemoryAccessWidth::word) {
    out << "(((uint32_t)" << ram_array << "[" << offset_expr << "] << 8U) | " << ram_array << "[" << offset_expr
        << "+1U])";
  } else {
    out << "(((uint32_t)" << ram_array << "[" << offset_expr << "] << 24U) | ((uint32_t)" << ram_array << "["
        << offset_expr << "+1U] << 16U) | ((uint32_t)" << ram_array << "[" << offset_expr << "+2U] << 8U) | "
        << ram_array << "[" << offset_expr << "+3U])";
  }
  return out.str();
}

void m68k_emit_ram_write_stmts(std::ostringstream &out, std::string_view ram_array, const std::string &offset_expr,
                               const std::string &value_expr, M68kMemoryAccessWidth size) {
  if (size == M68kMemoryAccessWidth::byte) {
    out << ram_array << "[" << offset_expr << "] = (uint8_t)(" << value_expr << "); ";
  } else if (size == M68kMemoryAccessWidth::word) {
    out << ram_array << "[" << offset_expr << "] = (uint8_t)((" << value_expr << ") >> 8U); " << ram_array << "["
        << offset_expr << "+1U] = (uint8_t)(" << value_expr << "); ";
  } else {
    out << ram_array << "[" << offset_expr << "] = (uint8_t)((" << value_expr << ") >> 24U); " << ram_array << "["
        << offset_expr << "+1U] = (uint8_t)((" << value_expr << ") >> 16U); " << ram_array << "[" << offset_expr
        << "+2U] = (uint8_t)((" << value_expr << ") >> 8U); " << ram_array << "[" << offset_expr
        << "+3U] = (uint8_t)(" << value_expr << "); ";
  }
}

struct M68kRuntimeEaAddress { std::string prelude; std::string address_expr; std::string postlude; };

// Computes the runtime byte-address expression for a register-relative EA
// mode, plus any register-mutation statements the mode itself requires.
// `reg == 7` (A7) with byte size steps by 2 instead of 1, per the contract's
// stated stack-pointer-alignment exception.
[[nodiscard]] M68kRuntimeEaAddress m68k_emit_runtime_ea_address(const M68kEffectiveAddress &ea,
                                                                 M68kMemoryAccessWidth size,
                                                                 std::string_view address_registers,
                                                                 std::string_view data_registers = {}) {
  const auto width = static_cast<std::uint32_t>(size);
  const auto reg = static_cast<unsigned>(ea.reg);
  const auto step = (reg == 7U && size == M68kMemoryAccessWidth::byte) ? 2U : width;
  M68kRuntimeEaAddress out{};
  std::ostringstream addr;
  addr << address_registers << '[' << reg << ']';
  switch (ea.mode) {
  case M68kEaMode::address_indirect:
    out.address_expr = addr.str();
    break;
  case M68kEaMode::address_postinc: {
    out.address_expr = addr.str();
    std::ostringstream post;
    post << address_registers << '[' << reg << "] += UINT32_C(" << step << ");\n";
    out.postlude = post.str();
    break;
  }
  case M68kEaMode::address_predec: {
    std::ostringstream pre;
    pre << address_registers << '[' << reg << "] -= UINT32_C(" << step << ");\n";
    out.prelude = pre.str();
    out.address_expr = addr.str();
    break;
  }
  case M68kEaMode::address_disp16: {
    std::ostringstream expr;
    expr << "(uint32_t)(" << address_registers << '[' << reg << "] + (int32_t)(int16_t)"
         << static_cast<int>(ea.displacement) << ")";
    out.address_expr = expr.str();
    break;
  }
  case M68kEaMode::address_index8: {
    // SEG-007-T120: An + sign_extend(Xn by size) + sign_extend(d8), computed
    // in 32 bits exactly as the MC68000 forms the effective address before
    // the 24-bit external bus truncation the routing seam applies. Word-size
    // index is sign-extended; long-size index is used whole. No auto-update:
    // this mode mutates no register.
    const auto index = ea.index_is_address ? std::string(address_registers) : std::string(data_registers);
    std::ostringstream expr;
    expr << "(uint32_t)(" << address_registers << '[' << reg << "] + ";
    if (ea.index_is_long)
      expr << "(int32_t)" << index << '[' << static_cast<unsigned>(ea.index_reg) << ']';
    else
      expr << "(int32_t)(int16_t)(uint16_t)" << index << '[' << static_cast<unsigned>(ea.index_reg) << ']';
    expr << " + (int32_t)(int8_t)" << static_cast<int>(ea.displacement) << ")";
    out.address_expr = expr.str();
    break;
  }
  case M68kEaMode::pc_index8: {
    // SEG-007-T136: pc_base_address + sign_extend(Xn by size) + sign_extend(d8),
    // computed in 32 bits exactly like `address_index8` above, except the base
    // is the decoded `pc_base_address` field (the address of this operand's own
    // first extension word, per ADR-0009 / decode.cpp's `pc_index8` provenance
    // rule) rather than a register file entry -- never an emitter-local
    // instruction length. No auto-update: this mode mutates no register.
    const auto index = ea.index_is_address ? std::string(address_registers) : std::string(data_registers);
    std::ostringstream expr;
    expr << "(uint32_t)(UINT32_C(0x" << hex(ea.pc_base_address, 8) << ") + ";
    if (ea.index_is_long)
      expr << "(int32_t)" << index << '[' << static_cast<unsigned>(ea.index_reg) << ']';
    else
      expr << "(int32_t)(int16_t)(uint16_t)" << index << '[' << static_cast<unsigned>(ea.index_reg) << ']';
    expr << " + (int32_t)(int8_t)" << static_cast<int>(ea.displacement) << ")";
    out.address_expr = expr.str();
    break;
  }
  default: break;
  }
  return out;
}

// Emits the fail-closed runtime bounds-check guard shared by every runtime
// EA access, generalizing JSR/RTS's existing `if (<out of range>) { return
// 1; }` convention (see call_general/return_from_subroutine below) to
// a runtime-computed address. Binds the address to `local_name` exactly once
// (so a later postincrement mutation cannot change the value this access
// itself uses).
void m68k_emit_runtime_ea_guard(std::ostringstream &out, std::string_view local_name,
                                const std::string &address_expr, M68kMemoryAccessWidth size,
                                const M68kMemoryEmissionContext &memory) {
  const auto width = static_cast<std::uint32_t>(size);
  out << "const uint32_t " << local_name << " = " << address_expr << "; if (" << local_name << " < UINT32_C(0x"
      << hex(memory.linear_memory_begin, 8) << ") || " << local_name << " > UINT32_C(0x"
      << hex(memory.linear_memory_end, 8) << ") - UINT32_C(" << width << ")) { return 1; }\n";
}

[[nodiscard]] const M68kRuntimeCEmitter &emit_runtime(const M68kMemoryEmissionContext &memory) { return *memory.runtime_emitter; }

struct M68kEaCode { std::string expression; std::string postlude; bool ok{true}; };


// SEG-007-T105: the single MC68000 24-bit external-address-bus truncation seam
// for runtime-routed data accesses. The MC68000 drives only 24 external address
// lines (A1-A23, plus the internal A0 used for byte selection via UDS/LDS); the
// upper eight bits A24-A31 of a 32-bit effective address are not pinned, so the
// hardware-visible bus address is `ea & 0x00FFFFFF` and the physical address
// space is 16 megabytes (2^24). See the MC68000 8-/16-/32-Bit Microprocessors
// User's Manual (Motorola/Freescale, order no. MC68000UM), "Signal Description"
// address-bus section and the 16-megabyte address-space statement. This is the
// same generic rule `m68k_canonical_ea_address` already applies to
// `absolute_word` EAs at compile time, now applied consistently at the runtime
// routing seam for the address-register-relative EA families whose address is
// only known at run time. It is bound once to a freshly named local so that the
// value routed to the routed-access owner and the value recorded in
// `provenance.access_address` are the exact same truncated 24-bit value.
void m68k_emit_routed_read(std::ostringstream &out, std::string_view address, M68kMemoryAccessWidth size,
                           const M68kMemoryEmissionContext &memory, std::string &expression,
                           unsigned &temp_ordinal) {
  const auto routed_addr = "m68k_routed_addr_" + std::to_string(temp_ordinal++);
  const auto value = "m68k_routed_value_" + std::to_string(temp_ordinal++);
  const auto stop = "m68k_route_stop_" + std::to_string(temp_ordinal++);
  out << "const uint32_t " << routed_addr << " = (" << address << ") & UINT32_C(0x00FFFFFF); ";
  out << emit_runtime(memory).routed_read(memory, routed_addr, value, stop, size);
  expression = value;
}

void m68k_emit_routed_write(std::ostringstream &out, std::string_view address, M68kMemoryAccessWidth size,
                            const M68kMemoryEmissionContext &memory, std::string_view value,
                            unsigned &temp_ordinal) {
  // SEG-007-T105: same single MC68000 24-bit external-address-bus truncation
  // seam as m68k_emit_routed_read above (see that comment and MC68000UM
  // "Signal Description" / 16-megabyte address-space statement). Bound once to a
  // freshly named local shared by the route call and provenance.access_address.
  const auto routed_addr = "m68k_routed_addr_" + std::to_string(temp_ordinal++);
  const auto stop = "m68k_route_stop_" + std::to_string(temp_ordinal++);
  out << "{ const uint32_t " << routed_addr << " = (" << address << ") & UINT32_C(0x00FFFFFF); ";
  out << emit_runtime(memory).routed_write(memory, routed_addr, stop, size, value) << " }\n";
}

// Emits, into `out`, any statements needed before the operand's value is
// available (runtime guard, predecrement mutation), and returns the C
// expression yielding that (already appropriately sized, but NOT masked)
// value, plus any postlude statements (postincrement mutation) the caller
// must emit strictly after the operation's own access statement. `memory`
// supplies the already-resolved static-operand fact for an absolute/
// pc-relative SOURCE read (M68kMemoryEmissionContext::test_operand_access/
// test_operand_value); this function is never called for a destination-only
// position (CLR has no source).
[[nodiscard]] M68kEaCode m68k_emit_ea_read(const M68kEffectiveAddress &ea, M68kMemoryAccessWidth size,
                                           std::string_view data_registers, const M68kMemoryEmissionContext &memory,
                                           std::ostringstream &out, unsigned &temp_ordinal) {
  M68kEaCode result{};
  switch (ea.mode) {
  case M68kEaMode::data_register:
    result.expression = std::string(data_registers) + "[" + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
    break;
  case M68kEaMode::address_register:
    result.expression =
        std::string(memory.address_registers) + "[" + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
    break;
  case M68kEaMode::immediate:
    result.expression = "UINT32_C(0x" + hex(ea.immediate_value, 8) + ")";
    break;
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long:
  case M68kEaMode::pc_disp16:
    if (!memory.test_operand_access) { result.ok = false; break; }
    if (*memory.test_operand_access != M68kOperandAccess::resolved_constant && memory.runtime_routing) {
      // Every non-constant absolute operand (linear memory or a device window -- the
      // wrapper's classification, opaque here) reaches the platform's runtime-routed access
      // owner; no compile-time literal, no static device model.
      m68k_emit_routed_read(out, "UINT32_C(0x" + hex(m68k_canonical_ea_address(ea), 8) + ")", size,
                            memory, result.expression, temp_ordinal);
    } else if (*memory.test_operand_access == M68kOperandAccess::linear_memory) {
      const auto offset = (m68k_canonical_ea_address(ea) - memory.linear_memory_begin);
      // Static addresses retain one literal RAM index per byte.  Besides
      // deterministic C, this keeps the fixed-profile TST presentation
      // artifact's established observable source shape without reviving a
      // separate TST emitter.
      const auto slot = [&](std::uint32_t index) {
        return std::string(memory.ram_array) + "[UINT32_C(" + std::to_string(offset + index) + ")]";
      };
      if (size == M68kMemoryAccessWidth::byte) result.expression = "(uint32_t)" + slot(0U);
      else if (size == M68kMemoryAccessWidth::word)
        result.expression = "(((uint32_t)" + slot(0U) + " << 8U) | " + slot(1U) + ")";
      else
        result.expression = "(((uint32_t)" + slot(0U) + " << 24U) | ((uint32_t)" + slot(1U) +
                            " << 16U) | ((uint32_t)" + slot(2U) + " << 8U) | " + slot(3U) + ")";
    } else {
      // The already-verified constant folded from the resolver's ROM-region
      // read (resolved_constant). A device fact is never folded
      // here: SEG-007-T040 routes every runtime_routing device fact
      // through the branch above instead, so it always reaches the runtime
      // device gate rather than a compile-time literal.
      result.expression = "UINT32_C(0x" + hex(memory.test_operand_value, 8) + ") /* resolved static read */";
    }
    break;
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc:
  case M68kEaMode::address_predec:
  case M68kEaMode::address_index8:  // SEG-007-T120: brief-format (d8,An,Xn) source read
  case M68kEaMode::pc_index8:  // SEG-007-T136: brief-format (d8,PC,Xn) MOVE-family source read
  case M68kEaMode::address_disp16: {
    const auto runtime = m68k_emit_runtime_ea_address(ea, size, memory.address_registers, data_registers);
    out << runtime.prelude;
    const auto local = "m68k_ea_addr_" + std::to_string(temp_ordinal++);
    if (memory.runtime_routing) {
      out << "const uint32_t " << local << " = " << runtime.address_expr << ";\n";
      m68k_emit_routed_read(out, local, size, memory, result.expression, temp_ordinal);
    } else {
      m68k_emit_runtime_ea_guard(out, local, runtime.address_expr, size, memory);
      const auto offset_expr = local + " - UINT32_C(0x" + hex(memory.linear_memory_begin, 8) + ")";
      result.expression = m68k_emit_ram_read_expr(memory.ram_array, offset_expr, size);
    }
    result.postlude = runtime.postlude;
    break;
  }
  default: result.ok = false; break;
  }
  return result;
}

// Materialize a source before a second operand is even evaluated.  This is
// required when source and destination name the same An: the source's
// postincrement is part of completing its read, not a deferred instruction
// epilogue.  Destination RMW postincrement remains owned by its write side.
[[nodiscard]] M68kEaCode m68k_emit_materialized_ea_read(const M68kEffectiveAddress &ea,
                                                         M68kMemoryAccessWidth size,
                                                         std::string_view data_registers,
                                                         const M68kMemoryEmissionContext &memory,
                                                         std::ostringstream &out,
                                                         unsigned &temp_ordinal) {
  auto result = m68k_emit_ea_read(ea, size, data_registers, memory, out, temp_ordinal);
  if (!result.ok) return result;
  const auto local = "m68k_ea_value_" + std::to_string(temp_ordinal++);
  out << "const uint32_t " << local << " = " << result.expression << ";\n" << result.postlude;
  result.expression = local;
  result.postlude.clear();
  return result;
}

// The write-side counterpart, used by MOVE/CLR destinations. Only ever
// called with a mode from `m68k_ea_data_alterable` (Dn plus the six memory
// modes) or, since SEG-007-T176, plain MOVE's own widened
// `m68k_ea_move_family_destination` (that same set plus the brief-format
// indexed `address_index8` mode); address_register/immediate/pc_disp16 are
// never legal destinations and are not handled here. A statically-resolved absolute destination is
// linear memory, a device register window (SEG-007-T113), or a
// generalized routed device window (SEG-007-T115); ROM writes are rejected before emission is ever
// reached. Whenever `memory.runtime_routing` is set (every C4 general-startup
// destination) the absolute store lowers straight to the routed-access owner, so
// no memory-context region lookup is needed here for any of those cases.
[[nodiscard]] M68kEaCode m68k_emit_ea_write(const M68kEffectiveAddress &ea, M68kMemoryAccessWidth size,
                                            std::string_view data_registers, const M68kMemoryEmissionContext &memory,
                                            const std::string &value_expr, std::ostringstream &out,
                                            unsigned &temp_ordinal) {
  M68kEaCode result{};
  switch (ea.mode) {
  case M68kEaMode::data_register: {
    const auto reg = std::string(data_registers) + "[" + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
    std::ostringstream statement;
    if (size == M68kMemoryAccessWidth::long_word) {
      statement << reg << " = (" << value_expr << ");";
    } else {
      const auto preserve_mask = size == M68kMemoryAccessWidth::byte ? "FFFFFF00" : "FFFF0000";
      statement << reg << " = (" << reg << " & UINT32_C(0x" << preserve_mask << ")) | ((" << value_expr
                << ") & UINT32_C(0x" << m68k_mask_hex(size) << "));";
    }
    result.expression = statement.str();
    break;
  }
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long: {
    if (memory.runtime_routing) {
      m68k_emit_routed_write(out, "UINT32_C(0x" + hex(m68k_canonical_ea_address(ea), 8) + ")", size,
                             memory, value_expr, temp_ordinal);
      result.expression.clear();
      break;
    }
    const auto offset = (m68k_canonical_ea_address(ea) - memory.linear_memory_begin);
    std::ostringstream statement;
    m68k_emit_ram_write_stmts(statement, memory.ram_array, "UINT32_C(" + std::to_string(offset) + ")", value_expr,
                              size);
    result.expression = statement.str();
    break;
  }
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc:
  case M68kEaMode::address_predec:
  case M68kEaMode::address_disp16:
  case M68kEaMode::address_index8: {  // SEG-007-T176: brief-format (d8,An,Xn) MOVE destination write
    // `address_index8` mutates no register (no postinc/predec of its own),
    // exactly like `address_disp16`, so this shared runtime-address/routed-
    // write path applies unchanged; the index-register lookup needs
    // `data_registers` (unlike the other three modes here, which only ever
    // reference `memory.address_registers`), matching the identical
    // `m68k_emit_ea_read` call for this same mode's source-read use.
    const auto runtime = m68k_emit_runtime_ea_address(ea, size, memory.address_registers, data_registers);
    out << runtime.prelude;
    const auto local = "m68k_ea_waddr_" + std::to_string(temp_ordinal++);
    if (memory.runtime_routing) {
      out << "const uint32_t " << local << " = " << runtime.address_expr << ";\n";
      m68k_emit_routed_write(out, local, size, memory, value_expr, temp_ordinal);
      result.expression.clear();
    } else {
      m68k_emit_runtime_ea_guard(out, local, runtime.address_expr, size, memory);
      const auto offset_expr = local + " - UINT32_C(0x" + hex(memory.linear_memory_begin, 8) + ")";
      std::ostringstream statement;
      m68k_emit_ram_write_stmts(statement, memory.ram_array, offset_expr, value_expr, size);
      result.expression = statement.str();
    }
    result.postlude = runtime.postlude;
    break;
  }
  default: result.ok = false; break;
  }
  return result;
}

// SEG-021-T006: runtime-routed lowering of an auto-updating ((An)+ / -(An)) operand of the
// SUB/SUBA/SUBQ/SUBI and CMP/CMPA/CMPI families through the operation-local deferred-address
// commit technique of docs/architecture/c4-add-family-auto-update-commit-contract.md: the
// touched An is snapshotted into one local, predecrement/postincrement act on that local only,
// every routed access uses it, and the live register is committed in one statement strictly after
// every routed access (a runtime stop returns from inside the access, before any architectural
// write, leaving the guest at the pre-instruction boundary). Legal forms carry at most one auto-
// updating operand. Returns false when the operation has no auto-updating operand or is not
// routed (the caller's ordinary path then applies unchanged).
[[nodiscard]] bool m68k_emit_routed_arith_auto_update(std::ostringstream &output, const M68kIrOperation &operation,
                                                     std::string_view data_registers,
                                                     std::string_view status_register,
                                                     const M68kMemoryEmissionContext &memory) {
  if (!memory.runtime_routing) return false;
  const auto is_auto = [](const M68kEffectiveAddress &ea) {
    return ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_postinc;
  };
  const bool source_auto = is_auto(operation.source_ea);
  const bool destination_auto = is_auto(operation.destination_ea);
  if (!source_auto && !destination_auto) return false;
  const bool compare = operation.kind == M68kIrKind::compare || operation.kind == M68kIrKind::compare_immediate ||
                       operation.kind == M68kIrKind::compare_address;
  const bool address_operation =
      operation.kind == M68kIrKind::subtract_address || operation.kind == M68kIrKind::compare_address;
  const char *prefix = compare ? "cmp" : "sub";
  const auto &auto_ea = source_auto ? operation.source_ea : operation.destination_ea;
  const auto width = static_cast<std::uint32_t>(operation.size);
  const std::uint32_t step =
      (static_cast<unsigned>(auto_ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
  const auto an_expr = [&](unsigned reg) { return std::string(memory.address_registers) + "[" + std::to_string(reg) + "]"; };
  const auto dn_expr = [&](unsigned reg) { return std::string(data_registers) + "[" + std::to_string(reg) + "]"; };
  const std::string local = std::string("m68k_") + prefix + "_auto_ea";
  // Decode fixes the other operand to Dn/An or an instruction-embedded immediate; anything else is declined
  // (no output), never guessed.
  const auto &other = source_auto ? operation.destination_ea : operation.source_ea;
  if (other.mode != M68kEaMode::data_register && other.mode != M68kEaMode::address_register &&
      other.mode != M68kEaMode::immediate)
    return true;
  const bool alias = source_auto && address_operation &&
                     other.mode == M68kEaMode::address_register && other.reg == auto_ea.reg;
  unsigned temp_ordinal = 0U;
  std::ostringstream body;
  body << "uint32_t " << local << " = " << an_expr(auto_ea.reg) << ";\n";
  if (auto_ea.mode == M68kEaMode::address_predec) body << local << " -= UINT32_C(" << step << ");\n";
  const auto other_expr = [&](const M68kEffectiveAddress &ea) -> std::string {
    if (ea.mode == M68kEaMode::data_register) return dn_expr(ea.reg);
    if (ea.mode == M68kEaMode::address_register) return an_expr(ea.reg);
    return "UINT32_C(0x" + hex(ea.immediate_value, 8) + ")";
  };
  std::string source_expr;
  std::string destination_expr;
  if (source_auto) {
    m68k_emit_routed_read(body, local, operation.size, memory, source_expr, temp_ordinal);
    if (auto_ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
    // Motorola/Musashi order: the source auto-update is applied first, so an aliased An destination operand
    // is the already-updated register (pinned by the direct SUBA/CMPA alias rows).
    destination_expr = alias ? local : other_expr(other);
  } else {
    source_expr = other_expr(other);
    m68k_emit_routed_read(body, local, operation.size, memory, destination_expr, temp_ordinal);
  }
  const bool word_address = address_operation && operation.size == M68kMemoryAccessWidth::word;
  const std::string source_value = word_address ? m68k_sign_extend_expr(source_expr, operation.size) : source_expr;
  const auto arithmetic_size = address_operation ? M68kMemoryAccessWidth::long_word : operation.size;
  body << "{ const uint32_t " << prefix << "_source = " << source_value << "; const uint32_t " << prefix
       << "_destination = " << destination_expr << "; ";
  if (compare) {
    M68kSubtractionResultSpecification::emit_c_update(body, status_register, std::string(prefix) + "_source",
                                                      std::string(prefix) + "_destination", arithmetic_size,
                                                      m68k_operation_effect(operation).extend_flag_policy);
    if (destination_auto && auto_ea.mode == M68kEaMode::address_postinc)
      body << local << " += UINT32_C(" << step << ");\n";
    body << " }\n";
  } else {
    body << "const uint32_t sub_result = sub_destination - sub_source; ";
    if (operation.kind == M68kIrKind::subtract_address) {
      body << an_expr(operation.destination_ea.reg) << " = sub_result; }\n";
    } else if (destination_auto) {
      m68k_emit_routed_write(body, local, operation.size, memory, "sub_result", temp_ordinal);
      if (auto_ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
      M68kSubtractionResultSpecification::emit_c_update(body, status_register, "sub_source", "sub_destination",
                                                        operation.size, M68kExtendFlagPolicy::from_carry);
      body << " }\n";
    } else {
      std::ostringstream write_prelude;
      const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, memory,
                                            "sub_result", write_prelude, temp_ordinal);
      if (!write.ok) return true;
      body << write_prelude.str() << write.expression << ' ';
      M68kSubtractionResultSpecification::emit_c_update(body, status_register, "sub_source", "sub_destination",
                                                        operation.size, M68kExtendFlagPolicy::from_carry);
      body << " }\n";
    }
  }
  // The single deferred live-register commit, strictly after every routed access. It is skipped only when the
  // arithmetic result itself is written to that same An (SUBA aliasing): the sum write is the last write.
  if (!(alias && operation.kind == M68kIrKind::subtract_address))
    body << an_expr(auto_ea.reg) << " = " << local << ";\n";
  output << "{\n" << body.str();
  if (compare || !memory.pc_macro_bridge_active)
    output << (compare ? std::string("pc") : std::string(memory.program_counter));
  else
    output << "pc";
  output << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
  return true;
}

// SEG-021-T014: lowering of ADDX, SUBX and CMPM (the register-pair, -(Ay),-(Ax) and (Ay)+,(Ax)+ shapes).
// Register pairs read both Dn, compute through M68kExtendedArithmeticSpecification and write the sized
// destination. Memory pairs use the operation-local deferred address-register commit technique of
// docs/architecture/c4-add-family-auto-update-commit-contract.md: each An is snapshotted into one local
// (the destination local starts from the source local when both name the same register, so an aliased
// pair sees the source's update first, as the MC68000 does), pre/post-adjusted on the locals only, every
// access (routed or linear-window guarded) uses them, and both live registers are committed in two
// statements strictly after every access and the CCR computation; a routed stop or window-guard exit
// happens before any architectural write. The A7 byte step is 2, per operand register.
// Returns false when the operation shape is not one of the three legal forms (nothing is emitted).
[[nodiscard]] bool m68k_emit_extended_pair(std::ostringstream &output, const M68kIrOperation &operation,
                                           std::string_view data_registers, std::string_view status_register,
                                           const M68kMemoryEmissionContext &memory) {
  const bool compare = operation.kind == M68kIrKind::compare_memory;
  // SEG-021-T015: ABCD/SBCD share this exact pair lowering; only the compute/update emitters differ.
  const bool decimal = operation.kind == M68kIrKind::add_decimal || operation.kind == M68kIrKind::subtract_decimal;
  const auto decimal_kind = operation.kind == M68kIrKind::add_decimal ? M68kDecimalArithmeticKind::add
                                                                      : M68kDecimalArithmeticKind::subtract;
  const auto kind = operation.kind == M68kIrKind::add_extended ? M68kExtendedArithmeticKind::add
                                                                : M68kExtendedArithmeticKind::subtract;
  const auto emit_compute = [&](std::ostringstream &o, const std::string &source, const std::string &destination) {
    if (decimal) M68kDecimalArithmeticSpecification::emit_c_compute(o, status_register, decimal_kind, source, destination);
    else M68kExtendedArithmeticSpecification::emit_c_compute(o, status_register, kind, source, destination, operation.size);
  };
  const auto emit_update = [&](std::ostringstream &o) {
    if (decimal) M68kDecimalArithmeticSpecification::emit_c_update(o, status_register);
    else M68kExtendedArithmeticSpecification::emit_c_update(o, status_register, kind, operation.size);
  };
  const char *result_name = decimal ? "bcd_result" : "xa_result";
  const bool registers = operation.source_ea.mode == M68kEaMode::data_register &&
                         operation.destination_ea.mode == M68kEaMode::data_register && !compare;
  const auto memory_mode = compare ? M68kEaMode::address_postinc : M68kEaMode::address_predec;
  const bool memory_pair = operation.source_ea.mode == memory_mode && operation.destination_ea.mode == memory_mode;
  if (!registers && !memory_pair) return false;
  const auto dn = [&](unsigned reg) { return std::string(data_registers) + "[" + std::to_string(reg) + "]"; };
  const auto an = [&](unsigned reg) { return std::string(memory.address_registers) + "[" + std::to_string(reg) + "]"; };
  const auto size = operation.size;
  std::ostringstream body;
  unsigned temp_ordinal = 0U;
  if (registers) {
    body << "{ ";
    emit_compute(body, dn(operation.source_ea.reg), dn(operation.destination_ea.reg));
    std::ostringstream write_prelude;
    const auto write = m68k_emit_ea_write(operation.destination_ea, size, data_registers, memory, result_name,
                                          write_prelude, temp_ordinal);
    if (!write.ok) return false;
    body << write_prelude.str() << write.expression << ' ';
    emit_update(body);
    body << "}\n";
  } else {
    const auto width = static_cast<std::uint32_t>(size);
    const auto step = [&](unsigned reg) {
      return (reg == 7U && size == M68kMemoryAccessWidth::byte) ? 2U : width;
    };
    const auto src_reg = static_cast<unsigned>(operation.source_ea.reg);
    const auto dst_reg = static_cast<unsigned>(operation.destination_ea.reg);
    const bool predecrement = memory_mode == M68kEaMode::address_predec;
    const auto read_at = [&](const char *local, const char *guard_local, std::string &expression) {
      if (memory.runtime_routing) {
        m68k_emit_routed_read(body, local, size, memory, expression, temp_ordinal);
      } else {
        m68k_emit_runtime_ea_guard(body, guard_local, local, size, memory);
        expression = m68k_emit_ram_read_expr(
            memory.ram_array, std::string(guard_local) + " - UINT32_C(0x" + hex(memory.linear_memory_begin, 8) + ")",
            size);
      }
    };
    body << "{\nuint32_t m68k_xa_src_ea = " << an(src_reg) << ";\n";
    if (predecrement) body << "m68k_xa_src_ea -= UINT32_C(" << step(src_reg) << ");\n";
    std::string src_expr;
    read_at("m68k_xa_src_ea", "m68k_xa_src_addr", src_expr);
    body << "const uint32_t xa_src_value = " << src_expr << ";\n";
    if (!predecrement) body << "m68k_xa_src_ea += UINT32_C(" << step(src_reg) << ");\n";
    body << "uint32_t m68k_xa_dst_ea = " << (src_reg == dst_reg ? std::string("m68k_xa_src_ea") : an(dst_reg))
         << ";\n";
    if (predecrement) body << "m68k_xa_dst_ea -= UINT32_C(" << step(dst_reg) << ");\n";
    std::string dst_expr;
    read_at("m68k_xa_dst_ea", "m68k_xa_dst_addr", dst_expr);
    body << "const uint32_t xa_dst_value = " << dst_expr << ";\n";
    if (compare) {
      M68kSubtractionResultSpecification::emit_c_update(body, status_register, "xa_src_value", "xa_dst_value", size,
                                                        M68kExtendFlagPolicy::preserve);
    } else {
      body << "{ ";
      emit_compute(body, "xa_src_value", "xa_dst_value");
      if (memory.runtime_routing) {
        m68k_emit_routed_write(body, "m68k_xa_dst_ea", size, memory, result_name, temp_ordinal);
      } else {
        m68k_emit_ram_write_stmts(body, memory.ram_array,
                                  std::string("m68k_xa_dst_addr - UINT32_C(0x") + hex(memory.linear_memory_begin, 8) + ")",
                                  result_name, size);
      }
      emit_update(body);
      body << "}\n";
    }
    if (!predecrement) body << "m68k_xa_dst_ea += UINT32_C(" << step(dst_reg) << ");\n";
    body << an(src_reg) << " = m68k_xa_src_ea;\n" << an(dst_reg) << " = m68k_xa_dst_ea;\n}\n";
  }
  output << "{\n" << body.str() << memory.program_counter << " += UINT32_C(" << operation.provenance.length.value
         << ");\n}\n";
  return true;
}

// SEG-021-T007: runtime-routed lowering of an auto-updating ((An)+ / -(An)) operand of AND/OR/EOR and
// ANDI/ORI/EORI, by the same operation-local deferred-address commit technique as
// m68k_emit_routed_arith_auto_update: An is snapshotted into one local, pre/post-adjusted on the local
// only, every routed access uses it, and the live register is committed in one statement strictly after
// every routed access and the destination write (a routed stop returns before any architectural write).
// Legal forms carry at most one auto-updating operand and the other operand is Dn or an immediate.
// Returns false when the operation has no auto-updating operand or is not routed.
[[nodiscard]] bool m68k_emit_routed_logical_auto_update(std::ostringstream &output, const M68kIrOperation &operation,
                                                       std::string_view data_registers,
                                                       std::string_view status_register,
                                                       const M68kMemoryEmissionContext &memory) {
  if (!memory.runtime_routing) return false;
  const auto is_auto = [](const M68kEffectiveAddress &ea) {
    return ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_postinc;
  };
  const bool source_auto = is_auto(operation.source_ea);
  const bool destination_auto = is_auto(operation.destination_ea);
  if (!source_auto && !destination_auto) return false;
  const bool is_and = operation.kind == M68kIrKind::logical_and || operation.kind == M68kIrKind::logical_and_immediate;
  const bool is_or = operation.kind == M68kIrKind::logical_or || operation.kind == M68kIrKind::logical_or_immediate;
  const char *op = is_and ? "&" : (is_or ? "|" : "^");
  const auto &auto_ea = source_auto ? operation.source_ea : operation.destination_ea;
  const auto width = static_cast<std::uint32_t>(operation.size);
  const std::uint32_t step =
      (static_cast<unsigned>(auto_ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
  const auto an_expr = [&](unsigned reg) { return std::string(memory.address_registers) + "[" + std::to_string(reg) + "]"; };
  const std::string local = "m68k_logical_auto_ea";
  const auto &other = source_auto ? operation.destination_ea : operation.source_ea;
  if (other.mode != M68kEaMode::data_register && other.mode != M68kEaMode::immediate) return true;
  unsigned temp_ordinal = 0U;
  std::ostringstream body;
  body << "uint32_t " << local << " = " << an_expr(auto_ea.reg) << ";\n";
  if (auto_ea.mode == M68kEaMode::address_predec) body << local << " -= UINT32_C(" << step << ");\n";
  const auto other_expr = [&](const M68kEffectiveAddress &ea) -> std::string {
    if (ea.mode == M68kEaMode::data_register) return std::string(data_registers) + "[" + std::to_string(ea.reg) + "]";
    return "UINT32_C(0x" + hex(ea.immediate_value, 8) + ")";
  };
  std::string source_expr;
  std::string destination_expr;
  if (source_auto) {
    m68k_emit_routed_read(body, local, operation.size, memory, source_expr, temp_ordinal);
    if (auto_ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
    destination_expr = other_expr(other);
  } else {
    source_expr = other_expr(other);
    m68k_emit_routed_read(body, local, operation.size, memory, destination_expr, temp_ordinal);
  }
  body << "{ const uint32_t logical_source = " << source_expr << "; const uint32_t logical_destination = "
       << destination_expr << "; const uint32_t logical_result = logical_destination " << op << " logical_source; ";
  if (destination_auto) {
    m68k_emit_routed_write(body, local, operation.size, memory, "logical_result", temp_ordinal);
    if (auto_ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
  } else {
    std::ostringstream write_prelude;
    const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, memory,
                                          "logical_result", write_prelude, temp_ordinal);
    if (!write.ok) return true;
    body << write_prelude.str() << write.expression << ' ';
  }
  M68kLogicalResultSpecification::emit_c_update(body, status_register, "logical_result", operation.size);
  body << " }\n" << an_expr(auto_ea.reg) << " = " << local << ";\n";
  // Same PC statement as the ordinary logical body below (`pc` is a macro in the routed bridge).
  output << "{\n" << body.str() << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
  return true;
}

// SEG-021-T008: runtime-routed lowering of an auto-updating ((An)+ / -(An)) destination of
// BTST/BCHG/BCLR/BSET by the same operation-local deferred-address commit technique as the
// arithmetic and logical families: An is snapshotted into one local, pre/post-adjusted on the local
// only, the routed read and (for BCHG/BCLR/BSET) the routed write use it, and the live register is
// committed in one statement strictly after every routed access (a routed stop returns before any
// architectural write). The bit number is materialized first; it is Dn or an immediate, never memory.
// BTST never writes back but still commits the auto-update. Returns false when the destination is not
// auto-updating or the context is not routed (the caller's ordinary path then applies unchanged).
[[nodiscard]] bool m68k_emit_routed_bit_auto_update(std::ostringstream &output, const M68kIrOperation &operation,
                                                    std::string_view data_registers,
                                                    std::string_view status_register,
                                                    const M68kMemoryEmissionContext &memory) {
  if (!memory.runtime_routing) return false;
  const auto &ea = operation.destination_ea;
  if (ea.mode != M68kEaMode::address_predec && ea.mode != M68kEaMode::address_postinc) return false;
  if (operation.source_ea.mode != M68kEaMode::data_register && operation.source_ea.mode != M68kEaMode::immediate)
    return true;
  const auto width = static_cast<std::uint32_t>(operation.size);
  const std::uint32_t step =
      (static_cast<unsigned>(ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
  const std::string an = std::string(memory.address_registers) + "[" + std::to_string(ea.reg) + "]";
  const std::string local = "m68k_bit_auto_ea";
  unsigned temp_ordinal = 0U;
  std::ostringstream prelude;
  const auto bit_number = m68k_emit_materialized_ea_read(operation.source_ea, M68kMemoryAccessWidth::long_word,
                                                          data_registers, memory, prelude, temp_ordinal);
  if (!bit_number.ok) return true;
  std::ostringstream body;
  body << prelude.str() << "uint32_t " << local << " = " << an << ";\n";
  if (ea.mode == M68kEaMode::address_predec) body << local << " -= UINT32_C(" << step << ");\n";
  std::string destination_expr;
  m68k_emit_routed_read(body, local, operation.size, memory, destination_expr, temp_ordinal);
  const auto bit_kind = operation.kind == M68kIrKind::bit_test ? M68kBitOperationKind::test
                        : operation.kind == M68kIrKind::bit_change ? M68kBitOperationKind::change
                        : operation.kind == M68kIrKind::bit_clear ? M68kBitOperationKind::clear
                                                                   : M68kBitOperationKind::set;
  body << "{ ";
  M68kBitOperationSpecification::emit_c_update(body, bit_kind, destination_expr, bit_number.expression,
                                                operation.size, "bit_index", "bit_mask", "bit_set", "bit_result");
  body << ' ';
  if (operation.kind != M68kIrKind::bit_test)
    m68k_emit_routed_write(body, local, operation.size, memory, "bit_result", temp_ordinal);
  else
    body << "(void)bit_result; ";
  M68kBitTestCcrSpecification::emit_c_update(body, status_register, "bit_set");
  body << " }\n";
  if (ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
  body << an << " = " << local << ";\n";
  output << "{\n" << body.str() << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
  return true;
}

// SEG-021-T010: runtime-routed lowering of an auto-updating ((An)+ / -(An)) SOURCE of
// MULS.W/MULU.W/DIVS.W/DIVU.W, the source-side counterpart of `m68k_emit_routed_bit_auto_update`
// above (same operation-local deferred-address commit technique). The destination is always Dn
// (decode.cpp fixes it): it never auto-updates and is read/written directly, never routed. The
// auto-update commits immediately after the routed source read/postincrement, exactly matching the
// real MC68000's own fetch-then-operate ordering -- the source operand (including its own
// auto-update) is fully consumed before MULS/MULU compute their product or DIVS/DIVU's divisor==0
// check below may raise the synchronous vector-5 exception (ADR-0037); the auto-update is
// unconditional even when that exception is later raised. Returns false when the source is not
// auto-updating or the context is not routed (the caller's ordinary path then applies unchanged).
[[nodiscard]] bool m68k_emit_routed_muldiv_auto_update(std::ostringstream &output, const M68kIrOperation &operation,
                                                        std::string_view data_registers,
                                                        std::string_view status_register,
                                                        const M68kMemoryEmissionContext &memory) {
  if (!memory.runtime_routing) return false;
  const auto &ea = operation.source_ea;
  if (ea.mode != M68kEaMode::address_predec && ea.mode != M68kEaMode::address_postinc) return false;
  const auto width = static_cast<std::uint32_t>(operation.size);
  const std::uint32_t step =
      (static_cast<unsigned>(ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
  const std::string an = std::string(memory.address_registers) + "[" + std::to_string(ea.reg) + "]";
  const std::string local = "m68k_muldiv_auto_ea";
  const std::string dn = std::string(data_registers) + "[" + std::to_string(operation.destination_ea.reg) + "]";
  unsigned temp_ordinal = 0U;
  std::ostringstream body;
  body << "uint32_t " << local << " = " << an << ";\n";
  if (ea.mode == M68kEaMode::address_predec) body << local << " -= UINT32_C(" << step << ");\n";
  std::string source_expr;
  m68k_emit_routed_read(body, local, operation.size, memory, source_expr, temp_ordinal);
  // A failed routed access returns from inside m68k_emit_routed_read above, before this point is
  // ever reached -- so source_expr is only used here once the read has genuinely succeeded, and no
  // architectural An mutation (predecrement already applied above notwithstanding -- it mutates only
  // the LOCAL snapshot, never the live register, until the unconditional commit below) has happened
  // yet either. Capture the same dynamic MUL timing source the ordinary (non-auto-update) MULS.W/
  // MULU.W body captures from its own materialized source read, so the caller's dynamic MUL
  // retirement-cycle helper (selected by `m68k_retirement_cycle_expression` in frontend.cpp)
  // retires against the actual fetched word, never the AOT/C4 block's zero-initialized default.
  // MUL-only: DIVS/DIVU carry no such dynamic timing hook.
  if (!memory.timing_mul_source.empty()) body << memory.timing_mul_source << " = (uint16_t)(" << source_expr << "); ";
  if (ea.mode == M68kEaMode::address_postinc) body << local << " += UINT32_C(" << step << ");\n";
  body << an << " = " << local << ";\n";
  if (operation.kind == M68kIrKind::multiply_signed_word || operation.kind == M68kIrKind::multiply_unsigned_word) {
    const bool is_signed = operation.kind == M68kIrKind::multiply_signed_word;
    const std::string prefix = is_signed ? "muls" : "mulu";
    const std::string signed_type = is_signed ? "int32_t" : "uint32_t";
    const std::string narrow_cast = is_signed ? "(int32_t)(int16_t)(" : "(uint32_t)(uint16_t)(";
    const std::string result_var = prefix + "_auto_result";
    body << "{ const " << signed_type << ' ' << prefix << "_auto_source = " << narrow_cast << source_expr
         << "); const " << signed_type << ' ' << prefix << "_auto_destination = " << narrow_cast << dn
         << "); const uint32_t " << result_var << " = (uint32_t)(" << prefix << "_auto_source * " << prefix
         << "_auto_destination); " << dn << " = " << result_var << "; ";
    M68kLogicalResultSpecification::emit_c_update(body, status_register, result_var, M68kMemoryAccessWidth::long_word);
    body << " }\n";
  } else {
    const bool is_signed = operation.kind == M68kIrKind::divide_signed_word;
    body << "{ const uint16_t divide_auto_divisor = (uint16_t)(" << source_expr << "); "
         << "if (divide_auto_divisor == 0U) { "
         << emit_runtime(memory).divide_by_zero(memory,
                operation.provenance.source.address.value + operation.provenance.length.value)
         << "else { ";
    std::ostringstream divide_body;
    M68kDivisionResultSpecification::emit_c_update(divide_body, status_register, dn, std::string("divide_auto_divisor"),
                                                    is_signed, std::string("divide_auto_result"));
    body << divide_body.str() << " if (!divide_auto_result_overflow) " << dn << " = divide_auto_result; } }\n";
  }
  output << "{\n" << body.str() << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
  return true;
}

// SEG-021-T018 / ADR 0043 §3, §5, §6: the supervisor/user lowering helpers shared by every privileged and
// status-register form. `memory.user_stack_pointer` names the INACTIVE stack-pointer slot and
// `address_registers[7]` the active one.

// SEG-021-T019 / ADR 0043 §2, §3, §5: one synchronous exception entry -- `vector` with the build-time `stacked_pc`.
// Routed: the platform's raise through the M68K-owned entry core (a complete statement that always returns: the
// build-time-resolved handler or a fail-closed stop). Direct linear-memory route: the same rule inline against the
// window -- the frame goes on the SSP (the active A7 when S = 1, the inactive slot when S = 0), the complete
// six-byte extent and the vector slot are validated before the first write, then SR word at SSP-6 / PC long at
// SSP-4, SR <- (saved & 0x271F) | S (T cleared, mask and CCR kept), the USP moves to the inactive slot when the
// mode changes, and PC <- the handler read from the window's vector slot (the direct route's flat machine keeps its
// vector table in that window; no program byte is decoded). The direct text falls through with the handler PC set.
[[nodiscard]] std::string m68k_exception_entry(std::uint32_t vector, std::uint32_t stacked_pc,
                                               std::string_view status_register,
                                               const M68kMemoryEmissionContext &memory) {
  if (memory.runtime_routing) {
    if (vector == 8U) return emit_runtime(memory).privilege_violation(memory, stacked_pc);
    return emit_runtime(memory).software_exception(memory, vector, stacked_pc);
  }
  const std::string_view program_counter = memory.program_counter.empty() ? "pc" : memory.program_counter;
  const auto begin = memory.linear_memory_begin;
  const auto end = memory.linear_memory_end;
  const auto a7 = std::string(memory.address_registers) + "[7]";
  const std::uint32_t slot_address = vector * 4U;
  std::ostringstream out;
  out << "{\n";
  if (slot_address < begin || end < slot_address + 4U || end - begin < 6U) {
    out << "return 1;\n}\n";
    return out.str();
  }
  const auto slot = [&](std::uint32_t address) {
    return std::string(memory.ram_array) + "[UINT32_C(" + std::to_string(address - begin) + ")]";
  };
  const auto frame = [&](unsigned index) {
    return std::string(memory.ram_array) + "[m68k_exception_frame - UINT32_C(0x" + hex(begin, 8) + ") + " +
           std::to_string(index) + "U]";
  };
  out << "const uint16_t m68k_exception_saved_sr = " << status_register << ";\n"
      << "const uint32_t m68k_exception_ssp = (m68k_exception_saved_sr & UINT16_C(0x2000)) != 0U ? " << a7 << " : "
      << memory.user_stack_pointer << ";\n"
      << "if ((m68k_exception_ssp & 1U) != 0U || m68k_exception_ssp < UINT32_C(0x" << hex(begin + 6U, 8)
      << ") || m68k_exception_ssp > UINT32_C(0x" << hex(end, 8) << ")) { return 1; }\n"
      << "{ const uint32_t m68k_exception_handler = ((uint32_t)" << slot(slot_address) << " << 24U) | ((uint32_t)"
      << slot(slot_address + 1U) << " << 16U) | ((uint32_t)" << slot(slot_address + 2U) << " << 8U) | (uint32_t)"
      << slot(slot_address + 3U) << ";\n"
      << "const uint32_t m68k_exception_frame = m68k_exception_ssp - UINT32_C(6);\n"
      << frame(0U) << " = (uint8_t)(m68k_exception_saved_sr >> 8U); " << frame(1U)
      << " = (uint8_t)m68k_exception_saved_sr;\n"
      << frame(2U) << " = UINT8_C(0x" << hex((stacked_pc >> 24U) & 0xFFU, 2) << "); " << frame(3U) << " = UINT8_C(0x"
      << hex((stacked_pc >> 16U) & 0xFFU, 2) << "); " << frame(4U) << " = UINT8_C(0x"
      << hex((stacked_pc >> 8U) & 0xFFU, 2) << "); " << frame(5U) << " = UINT8_C(0x" << hex(stacked_pc & 0xFFU, 2)
      << ");\n"
      << "if ((m68k_exception_saved_sr & UINT16_C(0x2000)) == 0U) " << memory.user_stack_pointer << " = " << a7 << ";\n"
      << a7 << " = m68k_exception_frame;\n"
      << status_register << " = (uint16_t)((m68k_exception_saved_sr & UINT16_C(0x271F)) | UINT16_C(0x2000));\n"
      << program_counter << " = m68k_exception_handler; }\n}\n";
  return out.str();
}

// The privilege guard. Returns the text that must PRECEDE the instruction's own body. Routed: raise vector 8
// through the platform (the text always returns) and let the body follow. Direct linear-memory route: the shared
// inline exception entry above (vector 8, the privileged instruction's own address stacked), then `else` -- the
// caller wraps its body in braces. `closing` receives the text to append after the body.
[[nodiscard]] std::string m68k_privilege_guard(const M68kIrOperation &operation, std::string_view status_register,
                                               const M68kMemoryEmissionContext &memory, std::string &closing) {
  const auto fault_pc = operation.provenance.source.address.value;
  std::ostringstream out;
  out << "if ((" << status_register << " & UINT16_C(0x2000)) == 0U) "
      << m68k_exception_entry(8U, fault_pc, status_register, memory);
  if (memory.runtime_routing) {
    out << "\n";
    closing.clear();
    return out.str();
  }
  out << "else {\n";
  closing = "}\n";
  return out.str();
}

// The SR write shared by MOVE to SR and ANDI/ORI/EORI to SR (and, with the same rules, nothing else): mask to
// the implemented bits, stop fail-closed when T would be set (trace is deferred; nothing committed yet), run
// `commit` (the operand's deferred address-register commit, which must precede the swap so an auto-updated A7
// is the OLD active stack pointer), swap the active and inactive stack pointers when S changes, write SR.
void m68k_emit_status_register_write(std::ostringstream &out, const std::string &value_expr,
                                     std::string_view status_register, const M68kMemoryEmissionContext &memory,
                                     std::string_view commit) {
  const auto a7 = std::string(memory.address_registers) + "[7]";
  out << "{ const uint16_t m68k_sr_new = (uint16_t)((" << value_expr << ") & UINT32_C(0xA71F));\n"
      << "if ((m68k_sr_new & UINT16_C(0x8000)) != 0U) ";
  if (memory.runtime_routing) out << emit_runtime(memory).trace_deferred_stop(memory) << "\n";
  else out << "{ return 1; }\n";
  out << commit
      << "if (((m68k_sr_new ^ " << status_register << ") & UINT16_C(0x2000)) != 0U) { const uint32_t m68k_sp_other = "
      << memory.user_stack_pointer << "; " << memory.user_stack_pointer << " = " << a7 << "; " << a7
      << " = m68k_sp_other; }\n"
      << status_register << " = m68k_sr_new; }\n";
}

// A word source operand of MOVE to SR / MOVE to CCR. Emits the read into `out` and returns the value
// expression plus the address-register commit the caller must emit after its own fail-closed checks (empty when
// the mode has no auto-update). Routed auto-updating operands use the operation-local deferred commit
// (c4-add-family-auto-update-commit-contract.md): a routed stop returns before An changes.
struct M68kStatusSource { std::string value; std::string commit; bool ok{false}; };
[[nodiscard]] M68kStatusSource m68k_emit_status_source(const M68kIrOperation &operation,
                                                      std::string_view data_registers,
                                                      const M68kMemoryEmissionContext &memory,
                                                      std::ostringstream &out) {
  M68kStatusSource result{};
  unsigned temp_ordinal = 0U;
  const auto &ea = operation.source_ea;
  const bool auto_update = ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_postinc;
  if (memory.runtime_routing && auto_update) {
    const auto an = std::string(memory.address_registers) + "[" + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
    out << "uint32_t m68k_status_auto_ea = " << an << ";\n";
    if (ea.mode == M68kEaMode::address_predec) out << "m68k_status_auto_ea -= UINT32_C(2);\n";
    std::string expression;
    m68k_emit_routed_read(out, "m68k_status_auto_ea", M68kMemoryAccessWidth::word, memory, expression, temp_ordinal);
    out << "const uint32_t m68k_status_source = " << expression << ";\n";
    result.value = "m68k_status_source";
    result.commit = an + " = m68k_status_auto_ea" +
                    (ea.mode == M68kEaMode::address_postinc ? std::string(" + UINT32_C(2)") : std::string()) + ";\n";
    result.ok = true;
    return result;
  }
  std::ostringstream prelude;
  const auto read = m68k_emit_ea_read(ea, M68kMemoryAccessWidth::word, data_registers, memory, prelude, temp_ordinal);
  if (!read.ok) return result;
  if (ea.mode == M68kEaMode::data_register || ea.mode == M68kEaMode::immediate) {
    result.value = read.expression;  // register / literal: no memory access, no local needed
    result.ok = true;
    return result;
  }
  out << prelude.str() << "const uint32_t m68k_status_source = " << read.expression << ";\n";
  result.value = "m68k_status_source";
  result.commit = read.postlude;
  result.ok = true;
  return result;
}
} // namespace

// This is the sole per-operation C-lowering definition for every accepted
// M68kIrKind, including the profile-specific absolute-memory and static
// call/return forms that have no SEG-003 direct-flow equivalent: no caller
// re-emits per-operation semantics itself. When `memory` is null (every
// SEG-003/direct-flow caller, which never selects the four memory-affecting
// kinds), those kinds retain exactly their prior CCR-only/no-op behavior.
std::string emit_m68k_operation_c(const M68kIrOperation &operation, std::string_view data_registers,
                                  std::string_view status_register, std::string_view indent,
                                   const M68kMemoryEmissionContext *memory) {
  std::ostringstream output;
  // Runtime-routed lowering requires the platform's emitter; without one nothing is emitted.
  if (memory != nullptr && memory->runtime_routing && memory->runtime_emitter == nullptr) return {};
  M68kMemoryEmissionContext routed_memory{};
  std::string routed_source;
  if (memory != nullptr && memory->runtime_routing) {
    routed_memory = *memory;
    routed_source = memory->runtime_source_symbol.empty() ? emit_runtime(*memory).instruction_source(operation)
                                                          : std::string(memory->runtime_source_symbol);
    routed_memory.runtime_source = routed_source;
    if (routed_memory.runtime_provenance_helper.empty()) routed_memory.runtime_provenance_helper = emit_runtime(*memory).default_provenance_helper();
    memory = &routed_memory;
  }
  output << indent;
  switch (operation.kind) {
  case M68kIrKind::write_moveq: {
    const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(operation.operand));
    output << data_registers << '[' << static_cast<unsigned>(operation.destination) << "] = UINT32_C(0x"
           << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value << "); "
           << status_register << " = (uint16_t)((" << status_register
           << " & UINT16_C(0xFFF0)) | (" << data_registers << '['
           << static_cast<unsigned>(operation.destination)
           << "] == 0U ? 4U : ((" << data_registers << '[' << static_cast<unsigned>(operation.destination)
           << "] & UINT32_C(0x80000000)) != 0U ? 8U : 0U)));\n";
    if (memory != nullptr) output << memory->program_counter << " += 2U;\n";
    break;
  }
  case M68kIrKind::write_user_stack_pointer:
  case M68kIrKind::read_user_stack_pointer: {
    // SEG-021-T018 / ADR 0043: MOVE An,USP / MOVE USP,An (privileged; vector 8 in user mode). In supervisor
    // mode the user stack pointer is the inactive stack-pointer slot, so both directions are one register
    // transfer through that slot; no condition code changes and no memory access. MOVE A7,USP copies the
    // SSP and MOVE USP,A7 overwrites the SSP (Musashi-identical, `A7` is the active SSP here).
    const auto effect = m68k_operation_effect(operation);
    const bool to_usp = operation.kind == M68kIrKind::write_user_stack_pointer;
    const auto &an_ea = to_usp ? operation.source_ea : operation.destination_ea;
    if (memory != nullptr && !memory->user_stack_pointer.empty() && an_ea.mode == M68kEaMode::address_register &&
        an_ea.reg < 8U && effect.pc == M68kPcEffectKind::advance &&
        effect.pc_delta == operation.provenance.length.value) {
      const auto an = std::string(memory->address_registers) + "[" + std::to_string(static_cast<unsigned>(an_ea.reg)) + "]";
      std::string closing;
      output << m68k_privilege_guard(operation, status_register, *memory, closing);
      if (to_usp) output << memory->user_stack_pointer << " = " << an << ";\n";
      else output << an << " = " << memory->user_stack_pointer << ";\n";
      output << memory->program_counter << " += UINT32_C(" << effect.pc_delta << ");\n" << closing;
    }
    break;
  }
  case M68kIrKind::write_status_register: {
    // SEG-021-T018 / ADR 0043 (supersedes the SEG-007-T088 no-privilege-check policy): MOVE <ea>,SR from every
    // data addressing mode. Privileged: in user mode vector 8 is raised before the operand is even read. The
    // word read is masked to the implemented SR bits; T = 1 stops fail-closed (trace deferred) before any
    // commit; the operand's address-register update commits before the S-change stack-pointer swap (so an
    // auto-updated A7 is the old active stack pointer, as on the MC68000); PC advances last.
    if (memory != nullptr && !memory->user_stack_pointer.empty()) {
      std::ostringstream body;
      const auto source = m68k_emit_status_source(operation, data_registers, *memory, body);
      if (source.ok) {
        std::string closing;
        output << m68k_privilege_guard(operation, status_register, *memory, closing) << "{\n" << body.str();
        m68k_emit_status_register_write(output, source.value, status_register, *memory, source.commit);
        output << (memory->program_counter.empty() ? std::string_view("pc") : memory->program_counter)
               << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n" << closing;
      }
    }
    break;
  }
  case M68kIrKind::logical_immediate_to_sr: {
    // SEG-021-T018: ANDI/ORI/EORI #imm,SR (privileged). The logical result goes through the same SR write as
    // MOVE to SR (masking, trace stop, S-change swap).
    if (memory != nullptr && !memory->user_stack_pointer.empty() && operation.source_ea.mode == M68kEaMode::immediate) {
      const char *op = operation.status_operation == M68kStatusLogicalOperation::and_op ? " & "
                       : operation.status_operation == M68kStatusLogicalOperation::or_op ? " | " : " ^ ";
      const auto value = "(uint32_t)" + std::string(status_register) + op + "UINT32_C(0x" +
                         hex(operation.source_ea.immediate_value & 0xFFFFU, 4) + ")";
      std::string closing;
      output << m68k_privilege_guard(operation, status_register, *memory, closing) << "{\n";
      m68k_emit_status_register_write(output, value, status_register, *memory, {});
      output << (memory->program_counter.empty() ? std::string_view("pc") : memory->program_counter)
             << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n" << closing;
    }
    break;
  }
  case M68kIrKind::logical_immediate_to_ccr: {
    // SEG-021-T018: ANDI/ORI/EORI #imm,CCR (unprivileged): only X/N/Z/V/C change (bits 7..5 of the CCR are
    // unimplemented and read as zero); the system byte is unchanged.
    if (memory != nullptr && operation.source_ea.mode == M68kEaMode::immediate) {
      const char *op = operation.status_operation == M68kStatusLogicalOperation::and_op ? " & "
                       : operation.status_operation == M68kStatusLogicalOperation::or_op ? " | " : " ^ ";
      output << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFF00)) | (((uint32_t)"
             << status_register << op << "UINT32_C(0x" << hex(operation.source_ea.immediate_value & 0xFFU, 2)
             << ")) & UINT32_C(0x1F)));\n"
             << (memory->program_counter.empty() ? std::string_view("pc") : memory->program_counter)
             << " += UINT32_C(" << operation.provenance.length.value << ");\n";
    }
    break;
  }
  case M68kIrKind::no_operation:
    // NOP (Motorola M68000 Family PRM NOP entry): "No operation occurs ...
    // The processor state, other than the program counter, is unaffected."
    // No register/memory/SR write, no CCR effect -- the sole lowered effect
    // is the program-counter advance past the single instruction word. The
    // PC-advance shape mirrors M68kIrKind::write_status_register /
    // test_operand above.
    if (memory != nullptr) {
      const std::string_view program_counter =
          memory->program_counter.empty() ? "pc" : memory->program_counter;
      output << "/* NOP */\n"
             << program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n";
    }
    break;
  case M68kIrKind::read_status_register: {
    // SEG-007-T116 / SEG-021-T018: MOVE from SR to every data-alterable destination. Unprivileged on the MC68000
    // and affects no condition codes. A Dn destination is a word register write (upper word preserved). A
    // MEMORY destination is read before it is written on the MC68000 (the value is discarded), lowered exactly
    // like the SEG-021-T016 memory-Scc precedent: one EA computation, one routed word read, one routed word
    // write to the same EA, the single deferred address-register commit strictly after the write, PC last.
    // (The pinned Musashi core performs no dummy read; the difference is not observable in its flat memory.)
    if (memory != nullptr) {
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      const std::string value = "(uint32_t)(uint16_t)" + std::string(status_register);
      const auto length = operation.provenance.length.value;
      const auto mode = operation.destination_ea.mode;
      unsigned temp_ordinal = 0U;
      if (mode == M68kEaMode::data_register) {
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                              value, write_prelude, temp_ordinal);
        if (write.ok)
          output << write_prelude.str() << write.expression << "\n" << write.postlude << program_counter
                 << " += UINT32_C(" << length << ");\n";
        break;
      }
      if (memory->runtime_routing && (mode == M68kEaMode::address_predec || mode == M68kEaMode::address_postinc)) {
        const auto an_expr = std::string(memory->address_registers) + "[" +
                             std::to_string(static_cast<unsigned>(operation.destination_ea.reg)) + "]";
        std::ostringstream body;
        body << "const uint32_t m68k_frs_value = " << value << ";\nuint32_t m68k_frs_auto_ea = " << an_expr << ";\n";
        if (mode == M68kEaMode::address_predec) body << "m68k_frs_auto_ea -= UINT32_C(2);\n";
        std::string discarded;
        m68k_emit_routed_read(body, "m68k_frs_auto_ea", operation.size, *memory, discarded, temp_ordinal);
        body << "(void)" << discarded << ";\n";
        m68k_emit_routed_write(body, "m68k_frs_auto_ea", operation.size, *memory, "m68k_frs_value", temp_ordinal);
        if (mode == M68kEaMode::address_postinc) body << "m68k_frs_auto_ea += UINT32_C(2);\n";
        body << an_expr << " = m68k_frs_auto_ea;\n";
        output << "{\n" << body.str() << program_counter << " += UINT32_C(" << length << ");\n}\n";
        break;
      }
      std::ostringstream prelude;
      const auto discarded = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers, *memory,
                                               prelude, temp_ordinal);
      if (discarded.ok) {
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;  // the read already applied the single pointer mutation
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, "m68k_frs_value",
                                              write_prelude, temp_ordinal);
        if (write.ok)
          output << "{\nconst uint32_t m68k_frs_value = " << value << ";\n" << prelude.str() << "(void)("
                 << discarded.expression << ");\n" << write_prelude.str() << write.expression << "\n"
                 << discarded.postlude << program_counter << " += UINT32_C(" << length << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::write_condition_codes: {
    // SEG-007-T118 / SEG-021-T018: MOVE <ea>,CCR from every data addressing mode. The source word is read but
    // only bits 4..0 of its low byte are copied into the CCR (X/N/Z/V/C); CCR bits 7..5 are unimplemented and
    // read as 0 (Musashi's m68ki_set_ccr consults only BIT_4..BIT_0). The system byte of SR is untouched and
    // the instruction is unprivileged. Auto-updating sources commit their address register after the read
    // (routed: the operation-local deferred commit).
    if (memory != nullptr) {
      std::ostringstream body;
      const auto source = m68k_emit_status_source(operation, data_registers, *memory, body);
      if (source.ok) {
        output << "{\n" << body.str() << status_register << " = (uint16_t)((" << status_register
               << " & UINT16_C(0xFF00)) | ((" << source.value << ") & UINT16_C(0x001F)));\n"
               << source.commit
               << (memory->program_counter.empty() ? std::string_view("pc") : memory->program_counter)
               << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::subtract_quick_long_d0:
    output << "{ const uint32_t old = " << data_registers << "[0]; " << data_registers
           << "[0] = old - 1U; " << status_register << " = (uint16_t)((" << status_register
           << " & UINT16_C(0xFFE0)) | (" << data_registers
           << "[0] == 0U ? 4U : 0U) | ((" << data_registers
           << "[0] & UINT32_C(0x80000000)) != 0U ? 8U : 0U) | (old == UINT32_C(0x80000000) ? 2U : 0U) | (old == 0U ? 17U : 0U)); }\n";
    break;
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
    break;
  case M68kIrKind::return_from_subroutine:
    if (memory != nullptr) {
      if (memory->runtime_routing) {
        const auto a7 = std::string(memory->address_registers) + "[7]";
        output << emit_runtime(*memory).return_pop_guard(*memory, a7) << a7 << " += UINT32_C(4); " << memory->program_counter << " = m68k_observed_return; }\n";
        break;
      }
      const auto a7 = std::string(memory->address_registers) + "[7]";
      const auto ram_begin_hex = hex(memory->linear_memory_begin, 8);
      const auto ram_last_aligned_hex = hex(memory->linear_memory_end - 4U, 8);
      output << "{ uint32_t observed_return; if ((" << a7 << " & 1U) != 0U || "
             << a7 << " < UINT32_C(0x" << ram_begin_hex << ") || " << a7
             << " > UINT32_C(0x" << ram_last_aligned_hex << ")) return 1; observed_return = ((uint32_t)"
             << memory->ram_array << "[" << a7 << "-UINT32_C(0x" << ram_begin_hex
             << ")] << 24U) | ((uint32_t)" << memory->ram_array << "[" << a7
             << "-UINT32_C(0x" << ram_begin_hex << ")+1U] << 16U) | ((uint32_t)" << memory->ram_array << "["
             << a7 << "-UINT32_C(0x" << ram_begin_hex << ")+2U] << 8U) | " << memory->ram_array
             << "[" << a7 << "-UINT32_C(0x" << ram_begin_hex << ")+3U]; if ("
             << memory->frame_depth << " == 0U || " << memory->frame_ids_array << "[" << memory->frame_depth
             << "-1U] != 0U) return 1; if (observed_return != " << memory->frame_continuations_array << "["
             << memory->frame_depth << "-1U]) return 1; --" << memory->frame_depth << "; "
             << a7 << " += 4U; pc = observed_return; }\n";
    }
    break;
  // SEG-007-T023: the general whitelist kinds. Each is gated on
  // `memory != nullptr` exactly like every existing memory-affecting kind
  // above; a null-context caller (every SEG-003/direct-flow caller) never
  // selects these kinds.
  case M68kIrKind::test_operand: {
    if (memory != nullptr) {
      unsigned temp_ordinal = 0U;
      // SEG-021-T005: an auto-updating TST operand is lowered by the operation-local deferred address-register
      // commit (docs/architecture/c4-add-family-auto-update-commit-contract.md Q2/Q5): one snapshot local, one
      // routed read, one live-register commit after the read (a stop returns before it).
      if (memory->runtime_routing && (operation.source_ea.mode == M68kEaMode::address_predec ||
                                      operation.source_ea.mode == M68kEaMode::address_postinc)) {
        const auto reg = static_cast<unsigned>(operation.source_ea.reg);
        const auto width = static_cast<std::uint32_t>(operation.size);
        const auto step = (reg == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        std::ostringstream body;
        body << "uint32_t m68k_test_auto_ea = " << an_expr << ";\n";
        if (operation.source_ea.mode == M68kEaMode::address_predec)
          body << "m68k_test_auto_ea -= UINT32_C(" << step << ");\n";
        std::string test_source;
        m68k_emit_routed_read(body, "m68k_test_auto_ea", operation.size, *memory, test_source, temp_ordinal);
        if (operation.source_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_test_auto_ea += UINT32_C(" << step << ");\n";
        body << "{ const uint32_t test_value = " << m68k_sign_extend_expr(test_source, operation.size) << "; ";
        M68kMoveResultCcrSpecification::emit_c_update(body, status_register, "test_value");
        body << " }\n" << an_expr << " = m68k_test_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
        break;
      }
      std::ostringstream prelude;
      const auto read = m68k_emit_ea_read(operation.source_ea, operation.size, data_registers, *memory, prelude,
                                          temp_ordinal);
      if (read.ok) {
        // The tested value is sign-extended to a canonical 32-bit
        // representation before m68k_move_result_ccr's existing bit31/
        // whole-value contract is applied, so N/Z are always taken from the
        // correctly-sized operand (bit7/bit15 for byte/word, not always
        // bit31) without any new CCR computation (decision #6): sign
        // extension of a masked value preserves both its sign bit at bit31
        // and its zero-ness.
        output << prelude.str() << "{ const uint32_t test_value = "
               << m68k_sign_extend_expr(read.expression, operation.size) << "; ";
        M68kMoveResultCcrSpecification::emit_c_update(output, status_register, "test_value");
        output << " }\n" << read.postlude;
        if (memory->program_counter == "pc")
          output << "pc += UINT32_C(" << operation.provenance.length.value << ");\n";
        else
          output << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n";
      }
    }
    break;
  }
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address: {
    if (memory != nullptr) {
      if (m68k_emit_routed_arith_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers, *memory,
                                                          prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea,
          operation.kind == M68kIrKind::compare_address ? M68kMemoryAccessWidth::long_word : operation.size,
          data_registers, *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        const bool word_address_compare = operation.kind == M68kIrKind::compare_address && operation.size == M68kMemoryAccessWidth::word;
        // Keep materialized source temporaries local to this operation.  A
        // frontend may concatenate multiple emitted operations, each with a
        // fresh deterministic local ordinal.
        output << "{\n" << prelude.str();
        const auto effect = m68k_operation_effect(operation);
        M68kSubtractionResultSpecification::emit_c_update(output, status_register,
            word_address_compare ? m68k_sign_extend_expr(source.expression, operation.size) : source.expression,
            destination.expression, word_address_compare ? M68kMemoryAccessWidth::long_word : operation.size,
            effect.extend_flag_policy);
        output << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::subtract_quick:
  case M68kIrKind::subtract_address: {
    if (memory != nullptr) {
      // SEG-007-T202 / SEG-021-T006: every routed auto-updating SUB-family operand (SUB, SUBA, SUBQ, SUBI) is
      // lowered by the deferred-address-commit helper above; other shapes fall through unchanged.
      if (m68k_emit_routed_arith_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers, *memory,
                                                          prelude, temp_ordinal);
      const bool address_destination = operation.kind == M68kIrKind::subtract_address ||
          (operation.kind == M68kIrKind::subtract_quick && operation.destination_ea.mode == M68kEaMode::address_register);
      const auto destination_size = address_destination ? M68kMemoryAccessWidth::long_word : operation.size;
      const auto destination = m68k_emit_ea_read(operation.destination_ea, destination_size, data_registers, *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t sub_source = "
               << (operation.kind == M68kIrKind::subtract_address && operation.size == M68kMemoryAccessWidth::word
                    ? m68k_sign_extend_expr(source.expression, operation.size) : source.expression)
               << "; const uint32_t sub_destination = " << destination.expression
               << "; const uint32_t sub_result = sub_destination - sub_source; ";
        if (address_destination) {
          output << memory->address_registers << '[' << static_cast<unsigned>(operation.destination_ea.reg)
                 << "] = sub_result; }\n";
        } else {
          // The write uses the same already-mutated EA shape as the read:
          // predecrement happens once before both accesses and postincrement
          // happens once after both accesses.
          auto write_ea = operation.destination_ea;
          if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
            write_ea.mode = M68kEaMode::address_indirect;
          std::ostringstream write_prelude;
          const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory,
                                                "sub_result", write_prelude, temp_ordinal);
          if (write.ok) {
            output << write_prelude.str() << write.expression << ' ';
            M68kSubtractionResultSpecification::emit_c_update(output, status_register, "sub_source", "sub_destination",
                                                               operation.size, M68kExtendFlagPolicy::from_carry);
            output << " }\n";
          }
        }
        // SEG-007-T174: this plain (non-auto-update) destination-write path
        // is shared by two frontend.cpp callers with different textual
        // needs -- the subtract/add/logical C4 case (which wraps this call
        // with a `#define pc runtime->pc` / `#undef pc` bridge, needed for
        // the sibling ADD/logical bodies, which emit the bare `pc` spelling
        // unconditionally) and SUBQ/SUBI's own sibling case (no bridge,
        // needs the fully-qualified text). Both set `program_counter` to the
        // same "runtime->pc" text, so `pc_macro_bridge_active` is the only
        // signal that distinguishes them: emitting the fully-qualified text
        // while the bridge is active would re-expose its own trailing `pc`
        // token to that active macro a second time, corrupting it into
        // `runtime->runtime->pc` -- a real defect this instruction shape's
        // routed linear-memory destination exposed only once a wider
        // set of Ghidra-assisted validated candidate blocks made it
        // reachable.
        output << destination.postlude;
        if (memory->pc_macro_bridge_active)
          output << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        else
          output << memory->program_counter << " += UINT32_C("
                 << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::add:
  case M68kIrKind::add_immediate:
  case M68kIrKind::add_quick:
  case M68kIrKind::add_address: {
    if (memory != nullptr) {
      // SEG-007-T145: an ADD / ADDA operand whose effective address uses an
      // auto-updating mode (predecrement `-(An)` or postincrement `(An)+`) is
      // lowered here through the add-family deferred-address-commit technique
      // (docs/architecture/c4-add-family-auto-update-commit-contract.md),
      // directly applying the MOVE contract's Q2/Q4/Q5 discipline
      // (docs/architecture/c4-move-predecrement-postincrement-commit-contract.md)
      // to the add family's single-memory-operand shape: the touched address
      // register is snapshotted into one local, every auto-update arithmetic
      // is applied to that local only, every access is routed through the
      // existing m68k_emit_routed_read / m68k_emit_routed_write boundary, and
      // the local is written back to the live register file in exactly one
      // statement placed strictly after every routed access this instruction
      // performs. A runtime stop reached by any routed access returns from
      // inside that access's own generated statement, textually before the
      // single commit, so the address register always remains at its
      // pre-instruction value (no partial decrement/increment observable).
      const bool add_auto_source = operation.source_ea.mode == M68kEaMode::address_predec ||
                                   operation.source_ea.mode == M68kEaMode::address_postinc;
      const bool add_auto_destination = operation.destination_ea.mode == M68kEaMode::address_predec ||
                                        operation.destination_ea.mode == M68kEaMode::address_postinc;
      // SEG-007-T153: ADDQ (`add_quick`) joins the add-family deferred-address
      // -commit path for an auto-updating memory destination (`(An)+` / `-(An)`).
      // ADDQ's source is always the instruction-embedded quick immediate, never
      // an EA read, so only its destination can be auto-updating; the ADDA
      // same-register aliasing sub-case (which needs a data-register-free
      // composed value) can never arise here.
      const bool add_auto_kind =
          operation.kind == M68kIrKind::add || operation.kind == M68kIrKind::add_address ||
          operation.kind == M68kIrKind::add_quick || operation.kind == M68kIrKind::add_immediate;
      if (memory->runtime_routing && add_auto_kind && (add_auto_source || add_auto_destination)) {
        const bool address_destination = operation.kind == M68kIrKind::add_address;
        const M68kEffectiveAddress &auto_ea = add_auto_source ? operation.source_ea : operation.destination_ea;
        const auto width = static_cast<std::uint32_t>(operation.size);
        const std::uint32_t step =
            (static_cast<unsigned>(auto_ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        const auto an_expr = [&](unsigned reg) {
          return std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        };
        const auto dn_expr = [&](unsigned reg) {
          return std::string(data_registers) + "[" + std::to_string(reg) + "]";
        };
        // SEG-021-T006: `ADDA <ea>,An` whose auto-updating source register is the
        // destination register itself. Motorola PRM/Musashi order: the source EA is
        // evaluated (auto-update applied) first, then the destination is read, so the
        // destination operand is the already-updated register and the sum write wins
        // over the auto-update. The deferred-commit local models this exactly: the
        // destination operand is the local after its adjustment and the final live
        // register commit is skipped (the sum write is the last architectural write).
        const bool alias_same_register =
            address_destination && add_auto_source &&
            static_cast<unsigned>(operation.source_ea.reg) == static_cast<unsigned>(operation.destination_ea.reg);
        {
          unsigned temp_ordinal = 0U;
          std::ostringstream body;
          bool ok = true;
          body << "uint32_t m68k_add_auto_ea = " << an_expr(static_cast<unsigned>(auto_ea.reg)) << ";\n";
          if (auto_ea.mode == M68kEaMode::address_predec)
            body << "m68k_add_auto_ea -= UINT32_C(" << step << ");\n";
          std::string add_source_expr;
          std::string add_destination_expr;
          if (add_auto_source) {
            m68k_emit_routed_read(body, "m68k_add_auto_ea", operation.size, *memory, add_source_expr, temp_ordinal);
            if (auto_ea.mode == M68kEaMode::address_postinc)
              body << "m68k_add_auto_ea += UINT32_C(" << step << ");\n";
            add_destination_expr = alias_same_register
                                       ? std::string("m68k_add_auto_ea")
                                   : address_destination
                                       ? an_expr(static_cast<unsigned>(operation.destination_ea.reg))
                                       : dn_expr(static_cast<unsigned>(operation.destination_ea.reg));
          } else {
            add_source_expr =
                operation.kind == M68kIrKind::add_quick || operation.kind == M68kIrKind::add_immediate
                    ? "UINT32_C(0x" + hex(operation.source_ea.immediate_value, 8) + ")"
                    : dn_expr(static_cast<unsigned>(operation.source_ea.reg));
            m68k_emit_routed_read(body, "m68k_add_auto_ea", operation.size, *memory, add_destination_expr,
                                   temp_ordinal);
          }
          body << "{ const uint32_t add_source = "
               << (address_destination && operation.size == M68kMemoryAccessWidth::word
                       ? m68k_sign_extend_expr(add_source_expr, operation.size)
                       : add_source_expr)
               << "; const uint32_t add_destination = " << add_destination_expr
               << "; const uint32_t add_result = add_destination + add_source; ";
          if (address_destination) {
            body << an_expr(static_cast<unsigned>(operation.destination_ea.reg)) << " = add_result; }\n";
          } else if (add_auto_source) {
            std::ostringstream write_prelude;
            const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                                  "add_result", write_prelude, temp_ordinal);
            if (!write.ok) {
              ok = false;
            } else {
              body << write_prelude.str() << write.expression << ' ';
              M68kAdditionResultSpecification::emit_c_update(body, status_register, "add_source", "add_destination",
                                                              operation.size);
              body << " }\n";
            }
          } else {
            m68k_emit_routed_write(body, "m68k_add_auto_ea", operation.size, *memory, "add_result", temp_ordinal);
            if (auto_ea.mode == M68kEaMode::address_postinc)
              body << "m68k_add_auto_ea += UINT32_C(" << step << ");\n";
            M68kAdditionResultSpecification::emit_c_update(body, status_register, "add_source", "add_destination",
                                                            operation.size);
            body << " }\n";
          }
          if (ok) {
            // The single deferred live-register-file commit, strictly after
            // every routed access above.
            if (!alias_same_register) body << an_expr(static_cast<unsigned>(auto_ea.reg)) << " = m68k_add_auto_ea;\n";
            output << "{\n" << body.str() << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
          }
        }
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers, *memory, prelude, temp_ordinal);
      const bool address_destination = operation.kind == M68kIrKind::add_address ||
          (operation.kind == M68kIrKind::add_quick && operation.destination_ea.mode == M68kEaMode::address_register);
      const auto destination_size = address_destination ? M68kMemoryAccessWidth::long_word : operation.size;
      const auto destination = m68k_emit_ea_read(operation.destination_ea, destination_size, data_registers, *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t add_source = "
               << (operation.kind == M68kIrKind::add_address && operation.size == M68kMemoryAccessWidth::word ? m68k_sign_extend_expr(source.expression, operation.size) : source.expression)
               << "; const uint32_t add_destination = " << destination.expression << "; const uint32_t add_result = add_destination + add_source; ";
        if (address_destination) {
          output << memory->address_registers << '[' << static_cast<unsigned>(operation.destination_ea.reg) << "] = add_result; }\n";
        } else {
          auto write_ea = operation.destination_ea;
          if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc) write_ea.mode = M68kEaMode::address_indirect;
          std::ostringstream write_prelude;
          const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, "add_result", write_prelude, temp_ordinal);
          if (write.ok) {
            output << write_prelude.str() << write.expression << ' ';
            M68kAdditionResultSpecification::emit_c_update(output, status_register, "add_source", "add_destination", operation.size);
            output << " }\n";
          }
        }
        output << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate: {
    if (memory != nullptr) {
      // SEG-021-T007: routed auto-updating operands lower through the operation-local deferred
      // address-commit helper above (like the add/sub/compare families); other shapes fall through.
      if (m68k_emit_routed_logical_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers,
                                                          *memory, prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers,
                                                  *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        const char *op = (operation.kind == M68kIrKind::logical_and || operation.kind == M68kIrKind::logical_and_immediate) ? "&" :
                         (operation.kind == M68kIrKind::logical_or || operation.kind == M68kIrKind::logical_or_immediate) ? "|" : "^";
        output << "{\n" << prelude.str() << "{ const uint32_t logical_source = " << source.expression
               << "; const uint32_t logical_destination = " << destination.expression
               << "; const uint32_t logical_result = logical_destination " << op << " logical_source; ";
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory,
                                              "logical_result", write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          M68kLogicalResultSpecification::emit_c_update(output, status_register, "logical_result", operation.size);
          output << " }\n";
        }
        output << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::multiply_signed_word: {
    if (memory != nullptr) {
      // SEG-021-T010: routed auto-updating sources lower through the operation-local
      // deferred address-commit helper above (like the logical/bit families).
      if (m68k_emit_routed_muldiv_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      // The source is read at word size and sign-extended (the manual's
      // MULS entry: "the operand is sign-extended to a long word"); the
      // destination is always Dn (decode.cpp fixes it), read at word size
      // (only Dn's low word participates) and likewise sign-extended --
      // never a full 32-bit read of a destination that might already hold
      // an unrelated upper word.
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers,
                                                          *memory, prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers,
                                                  *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        output << "{\n" << prelude.str();
        if (!memory->timing_mul_source.empty()) output << memory->timing_mul_source << " = (uint16_t)(" << source.expression << "); ";
        output << "{ const int32_t muls_source = (int32_t)(int16_t)("
               << source.expression << "); const int32_t muls_destination = (int32_t)(int16_t)("
               << destination.expression << "); const uint32_t muls_result = (uint32_t)(muls_source * muls_destination); ";
        const auto write = m68k_emit_ea_write(operation.destination_ea, M68kMemoryAccessWidth::long_word,
                                              data_registers, *memory, "muls_result", output, temp_ordinal);
        if (write.ok) {
          output << write.expression << ' ';
          M68kLogicalResultSpecification::emit_c_update(output, status_register, "muls_result",
                                                        M68kMemoryAccessWidth::long_word);
          output << " }\n" << destination.postlude
                 << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        }
      }
    }
    break;
  }
  case M68kIrKind::multiply_unsigned_word: {
    // SEG-007-T222: MULU.W <ea>,Dn -- unsigned sibling of multiply_signed_word
    // immediately above; shares every structural treatment (word-size
    // source/destination read, full 32-bit Dn write,
    // M68kLogicalResultSpecification CCR update), evaluated as an unsigned
    // product rather than a signed one.
    if (memory != nullptr) {
      // SEG-021-T010: see multiply_signed_word above.
      if (m68k_emit_routed_muldiv_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers,
                                                          *memory, prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers,
                                                  *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        output << "{\n" << prelude.str();
        if (!memory->timing_mul_source.empty()) output << memory->timing_mul_source << " = (uint16_t)(" << source.expression << "); ";
        output << "{ const uint32_t mulu_source = (uint32_t)(uint16_t)("
               << source.expression << "); const uint32_t mulu_destination = (uint32_t)(uint16_t)("
               << destination.expression << "); const uint32_t mulu_result = (uint32_t)(mulu_source * mulu_destination); ";
        const auto write = m68k_emit_ea_write(operation.destination_ea, M68kMemoryAccessWidth::long_word,
                                              data_registers, *memory, "mulu_result", output, temp_ordinal);
        if (write.ok) {
          output << write.expression << ' ';
          M68kLogicalResultSpecification::emit_c_update(output, status_register, "mulu_result",
                                                        M68kMemoryAccessWidth::long_word);
          output << " }\n" << destination.postlude
                 << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        }
      }
    }
    break;
  }
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word: {
    // SEG-007-T222 / ADR-0037: DIVS.W/DIVU.W <ea>,Dn. Only meaningful in the
    // runtime-routed C4 context (the vector-5 raise helper needs a live
    // `the platform runtime object`).
    if (memory != nullptr && memory->runtime_routing) {
      // SEG-021-T010: routed auto-updating sources lower through the operation-local deferred
      // address-commit helper above (see multiply_signed_word).
      if (m68k_emit_routed_muldiv_auto_update(output, operation, data_registers, status_register, *memory)) break;
      const bool is_signed = operation.kind == M68kIrKind::divide_signed_word;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      // Source: the word-size divisor. Destination: the FULL 32-bit Dn
      // dividend -- `operation.destination_ea` is always data_register (fixed
      // by decode.cpp), so requesting a `long_word`-size read yields the
      // complete register value unmasked (m68k_emit_ea_read's data_register
      // arm ignores `size` entirely), never just Dn's low word.
      const auto source = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers,
                                                          *memory, prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea, M68kMemoryAccessWidth::long_word,
                                                  data_registers, *memory, prelude, temp_ordinal);
      if (source.ok && destination.ok) {
        output << "{\n" << prelude.str()
               << "{ const uint16_t divide_divisor = (uint16_t)(" << source.expression << "); "
               << "if (divide_divisor == 0U) { "
               << emit_runtime(*memory).divide_by_zero(*memory, operation.provenance.source.address.value + operation.provenance.length.value)
               << "else { ";
        const auto write = m68k_emit_ea_write(operation.destination_ea, M68kMemoryAccessWidth::long_word,
                                              data_registers, *memory, "divide_result", output, temp_ordinal);
        // Emit the division computation before the write so `divide_result`
        // is in scope; M68kDivisionResultSpecification::emit_c_update
        // declares it.
        if (write.ok) {
          std::ostringstream divide_body;
          M68kDivisionResultSpecification::emit_c_update(divide_body, status_register, destination.expression,
                                                          "divide_divisor", is_signed, "divide_result");
          // Overflow is not merely an unchanged value: DIVS/DIVU leave Dn
          // unwritten. The shared specification initializes divide_result to
          // the original dividend, but this guard preserves the architectural
          // no-write effect rather than redundantly storing that same value.
          output << divide_body.str() << " if (!divide_result_overflow) " << write.expression << " } }\n" << destination.postlude
                 << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        }
      }
    }
    break;
  }
  case M68kIrKind::write_move: {
    if (memory != nullptr) {
      const bool source_mutating = operation.source_ea.mode == M68kEaMode::address_predec ||
                                    operation.source_ea.mode == M68kEaMode::address_postinc;
      const bool destination_mutating = operation.destination_ea.mode == M68kEaMode::address_predec ||
                                        operation.destination_ea.mode == M68kEaMode::address_postinc;
      // SEG-007-T070: implements
      // docs/architecture/c4-move-predecrement-postincrement-commit-contract.md
      // (Q1-Q5). A predecrement/postincrement source and/or destination is
      // lowered through this MOVE-specific deferred-address-commit
      // technique, confined entirely to this one case, generalizing
      // movem_transfer's own already-validated single-register snapshot/
      // mutate-local/route/writeback-last technique (its address_predec/
      // address_postinc branches above) to MOVE's independent source-and-
      // destination shape. It never calls m68k_emit_runtime_ea_address/
      // m68k_emit_ea_read/m68k_emit_ea_write for the mutating operand(s);
      // every non-mutating EA mode still routes through those primitives
      // exactly as today. Gated on memory->runtime_routing, exactly like
      // movem_transfer's own routed branch above (the only caller that can
      // ever produce a predecrement/postincrement write_move operand today,
      // the C4 general-startup emitter, always sets runtime_routing).
      if (memory->runtime_routing && (source_mutating || destination_mutating)) {
        // Q3 (SEG-021-T005, superseding the former outright decline): a MOVE
        // whose source is the mutating operand and whose destination's EA
        // computation reads that same address register -- (An), d16(An),
        // (An)+ or -(An) -- observes the register AFTER the source's
        // auto-update (the MC68000 completes the source EA calculation before
        // it forms the destination EA; pinned Musashi agrees). The routed
        // lowering keeps every architectural write deferred: the destination
        // address is derived from the source's updated LOCAL
        // (`m68k_move_src_ea`), never from the live register, and the live
        // commits stay after both routed accesses (destination commit last,
        // so `(An)+,(An)+` nets two steps, exactly as the hardware does).
        const bool destination_reads_same_register_address =
            operation.destination_ea.mode == M68kEaMode::address_predec ||
            operation.destination_ea.mode == M68kEaMode::address_postinc ||
            operation.destination_ea.mode == M68kEaMode::address_indirect ||
            operation.destination_ea.mode == M68kEaMode::address_disp16;
        // (d8,An,Xn): both the base An and an address-register index Xn read
        // the source's post-update value when they name the source register.
        const auto &dea = operation.destination_ea;
        const bool destination_index_uses_source =
            source_mutating && dea.mode == M68kEaMode::address_index8 &&
            (dea.reg == operation.source_ea.reg ||
             (dea.index_is_address && dea.index_reg == operation.source_ea.reg));
        const bool destination_aliases_source_local =
            source_mutating &&
            ((destination_reads_same_register_address && operation.source_ea.reg == dea.reg) ||
             destination_index_uses_source);
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        bool ok = true;
        const auto width = static_cast<std::uint32_t>(operation.size);
        // Matches m68k_emit_runtime_ea_address's own step formula exactly
        // (the A7-with-byte-size steps-by-2 stack-pointer-alignment
        // exception), since MOVE, unlike MOVEM, has a legal BYTE
        // predecrement/postincrement form.
        const auto step_for = [&](const M68kEffectiveAddress &ea) {
          return (static_cast<unsigned>(ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U
                                                                                                          : width;
        };
        const auto an_expr = [&](const M68kEffectiveAddress &ea) {
          return std::string(memory->address_registers) + "[" + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
        };
        // SEG-007-T252 / ADR-0040: the former SEG-007-T155/ADR-0017 and
        // SEG-007-T157/ADR-0019 guarded watchdog-progress notes for an
        // auto-updating write_move destination/source have been removed --
        // termination/progress policy now belongs to the runner
        // (the runner's dispatch allowance), never to a
        // CPU/codegen-proven data-transform cursor fact. The routed
        // read/write and `An` commit below are unaffected guest semantics.
        // Q2 step 1-2: source, if mutating, snapshots its own address
        // register into a dedicated local (never the live array), applies
        // predecrement to that local before the access, and routes the
        // read through m68k_emit_routed_read; postincrement is applied to
        // the local strictly after the access. A non-mutating source
        // still routes through the untouched, shared m68k_emit_ea_read.
        std::string source_expr;
        if (source_mutating) {
          body << "uint32_t m68k_move_src_ea = " << an_expr(operation.source_ea) << ";\n";
          if (operation.source_ea.mode == M68kEaMode::address_predec)
            body << "m68k_move_src_ea -= UINT32_C(" << step_for(operation.source_ea) << ");\n";
          m68k_emit_routed_read(body, "m68k_move_src_ea", operation.size, *memory, source_expr, temp_ordinal);
          if (operation.source_ea.mode == M68kEaMode::address_postinc)
            body << "m68k_move_src_ea += UINT32_C(" << step_for(operation.source_ea) << ");\n";
        } else {
          const auto read =
              m68k_emit_ea_read(operation.source_ea, operation.size, data_registers, *memory, body, temp_ordinal);
          if (!read.ok) {
            ok = false;
          } else {
            source_expr = read.expression;
            body << read.postlude;
          }
        }
        // Q2 step 3: destination, if mutating, snapshots its own address
        // register into a second, distinct local and applies predecrement
        // before the access, mirroring the source above. Same-register
        // source/destination aliasing is accepted (Q3): the destination EA
        // reads the source's updated local, and the live register commits stay
        // deferred until both routed accesses have succeeded.
        if (ok && destination_aliases_source_local && !destination_mutating && !destination_index_uses_source) {
          // Non-mutating (An)/d16(An) destination reading the source's updated register.
          body << "const uint32_t m68k_move_dst_ea = m68k_move_src_ea";
          if (operation.destination_ea.mode == M68kEaMode::address_disp16)
            body << " + (uint32_t)(int32_t)(int16_t)" << static_cast<int>(operation.destination_ea.displacement);
          body << ";\n";
        }
        if (ok && destination_index_uses_source) {
          const auto reg_expr = [&](bool is_address, unsigned n) {
            if (is_address && n == static_cast<unsigned>(operation.source_ea.reg)) return std::string("m68k_move_src_ea");
            return std::string(is_address ? memory->address_registers : data_registers) + "[" + std::to_string(n) + "]";
          };
          const auto index_value = reg_expr(dea.index_is_address, static_cast<unsigned>(dea.index_reg));
          body << "const uint32_t m68k_move_dst_ea = (uint32_t)("
               << reg_expr(true, static_cast<unsigned>(dea.reg)) << " + "
               << (dea.index_is_long ? "(int32_t)" + index_value
                                     : "(int32_t)(int16_t)(uint16_t)" + index_value)
               << " + (int32_t)(int8_t)" << static_cast<int>(dea.displacement) << ");\n";
        }
        if (ok && destination_mutating) {
          body << "uint32_t m68k_move_dst_ea = "
               << (destination_aliases_source_local ? std::string("m68k_move_src_ea")
                                                    : an_expr(operation.destination_ea))
               << ";\n";
          if (operation.destination_ea.mode == M68kEaMode::address_predec)
            body << "m68k_move_dst_ea -= UINT32_C(" << step_for(operation.destination_ea) << ");\n";
        }
        if (ok) {
          // `move_value` retains the operand's raw (not sign-extended)
          // bits for the actual data write, exactly as real MOVE never
          // sign-extends; only the separate CCR feed below is sign-
          // extended.
          body << "{ const uint32_t move_value = " << source_expr << "; ";
          // Q2 step 4: destination write, routed through
          // m68k_emit_routed_write for a mutating destination (postinc
          // applied to the local strictly after the access), or the
          // untouched, shared m68k_emit_ea_write for a non-mutating one.
          if (destination_mutating || destination_aliases_source_local) {
            m68k_emit_routed_write(body, "m68k_move_dst_ea", operation.size, *memory, "move_value", temp_ordinal);
            if (operation.destination_ea.mode == M68kEaMode::address_postinc)
              body << "m68k_move_dst_ea += UINT32_C(" << step_for(operation.destination_ea) << ");\n";
          } else {
            std::ostringstream write_prelude;
            const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers,
                                                  *memory, "move_value", write_prelude, temp_ordinal);
            if (!write.ok) {
              ok = false;
            } else {
              body << write_prelude.str() << write.expression << " ";
            }
          }
          if (ok) {
            M68kMoveResultCcrSpecification::emit_c_update(body, status_register,
                                                          m68k_sign_extend_expr("move_value", operation.size));
          }
          body << " }\n";
        }
        if (ok) {
          // Q2 step 5 / Q5: both live-register-array writeback statements
          // are placed here, strictly after both the source access's and
          // the destination access's routed statements have already been
          // emitted above -- never one immediately after its own
          // operand's access. A runtime stop reached by either routed
          // access above returns from inside that access's own generated
          // statement, so neither writeback below is ever reached unless
          // both accesses have already succeeded.
          if (source_mutating) body << an_expr(operation.source_ea) << " = m68k_move_src_ea;\n";
          if (destination_mutating) body << an_expr(operation.destination_ea) << " = m68k_move_dst_ea;\n";
          // SEG-007-T252 / ADR-0040: the former SEG-007-T155/ADR-0017 WRITE
          // and SEG-007-T157/ADR-0019 READ guarded watchdog-progress notes,
          // previously emitted here after the routed write and the `An`
          // commit above, have been removed -- termination/progress policy
          // now belongs to the runner, never to this per-instruction cursor
          // fact. The routed accesses and register commits above are
          // unaffected guest semantics.
          output << "{\n" << body.str();
          output << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        }
      } else {
        unsigned temp_ordinal = 0U;
        std::ostringstream prelude;
        const auto read = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers, *memory,
                                                          prelude, temp_ordinal);
        if (read.ok) {
          std::ostringstream write_prelude;
          const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                                "move_value", write_prelude, temp_ordinal);
          if (write.ok) {
            // `move_value` retains the operand's raw (not sign-extended) bits
            // for the actual data write, exactly as real MOVE never sign-
            // extends; only the separate CCR feed below is sign-extended.
            output << "{\n" << prelude.str() << "{ const uint32_t move_value = " << read.expression << "; " << write_prelude.str()
                   << write.expression << " ";
            M68kMoveResultCcrSpecification::emit_c_update(output, status_register,
                                                          m68k_sign_extend_expr("move_value", operation.size));
            output << " }\n" << write.postlude;
            output << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
          }
        }
      }
    }
    break;
  }
  case M68kIrKind::write_movea: {
    if (memory != nullptr) {
      const bool source_mutating = operation.source_ea.mode == M68kEaMode::address_predec ||
                                    operation.source_ea.mode == M68kEaMode::address_postinc;
      // SEG-007-T192: an auto-updating MOVEA source (`-(An)`/`(An)+`) is
      // lowered through the same deferred-address-commit technique already
      // established for write_move (Q2/Q4/Q5,
      // docs/architecture/c4-move-predecrement-postincrement-commit-contract.md)
      // and the add family (docs/architecture/c4-add-family-auto-update-
      // commit-contract.md): the touched address register is snapshotted
      // into one local, the auto-update arithmetic is applied only to that
      // local, the read is routed through the existing
      // m68k_emit_routed_read boundary, and the local is written back to
      // the live register file in exactly one statement placed strictly
      // after the routed access. A runtime stop reached inside the routed
      // read returns from inside that access's own generated statement,
      // textually before the commit, so the source address register always
      // remains at its pre-instruction value.
      //
      // Unlike ADDA, MOVEA's destination is a plain overwrite of the
      // destination An with the loaded value -- it never reads the
      // destination register's own prior value to compute the result -- so
      // the same-register shape (`MOVEA.L (A0)+,A0`) has no undefined
      // composed-value aliasing hazard the way `ADDA <auto>(An),An` does. It
      // does, however, require a specific commit order for that case: real
      // MC68000 hardware completes the source's own auto-update (part of
      // source EA calculation) strictly before writing the loaded value into
      // the destination, so when source and destination name the same
      // register, the destination's loaded-value write must be the LAST
      // thing observable in that register, overwriting the auto-updated
      // address rather than being overwritten by it. The routed read is
      // still the only operation that can fail, so it is safe -- and,
      // per this ordering requirement, necessary -- to commit the source's
      // local back to the live register file immediately after the routed
      // read succeeds, strictly before the (failure-free) destination
      // register write. A runtime stop reached inside the routed read
      // returns from inside that access's own generated statement,
      // textually before the commit, so the source address register always
      // remains at its pre-instruction value on failure.
      if (memory->runtime_routing && source_mutating) {
        const auto width = static_cast<std::uint32_t>(operation.size);
        const auto step_for = [&](const M68kEffectiveAddress &ea) {
          return (static_cast<unsigned>(ea.reg) == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        };
        const auto an_expr = [&](unsigned reg) {
          return std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        };
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_movea_src_ea = " << an_expr(static_cast<unsigned>(operation.source_ea.reg)) << ";\n";
        if (operation.source_ea.mode == M68kEaMode::address_predec)
          body << "m68k_movea_src_ea -= UINT32_C(" << step_for(operation.source_ea) << ");\n";
        std::string source_expr;
        m68k_emit_routed_read(body, "m68k_movea_src_ea", operation.size, *memory, source_expr, temp_ordinal);
        if (operation.source_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_movea_src_ea += UINT32_C(" << step_for(operation.source_ea) << ");\n";
        // Commit, strictly after the routed read has already succeeded and
        // strictly before the destination register write below (see the
        // ordering rationale above -- this is the one MOVEA-specific
        // deviation from Q2 step 5 / Q5's "defer every writeback past every
        // access" rule, required only because MOVEA's sole remaining step,
        // the destination write, can never itself fail).
        body << an_expr(static_cast<unsigned>(operation.source_ea.reg)) << " = m68k_movea_src_ea;\n";
        const auto dest_reg = an_expr(static_cast<unsigned>(operation.destination_ea.reg));
        if (operation.size == M68kMemoryAccessWidth::word) {
          // "Word-size source operands are sign-extended to 32-bit
          // quantities" (contract, MOVEA "Description"): MOVEA.W is the one
          // sign-extending form in this whole batch.
          body << dest_reg << " = (uint32_t)(int32_t)(int16_t)(" << source_expr << ");\n";
        } else {
          body << dest_reg << " = (" << source_expr << ");\n";
        }
        // MOVEA's entire CCR/SR is unaffected: no status_register update is
        // emitted at all (contract, MOVEA "Condition Codes": "Not
        // affected.").
        output << "{\n" << body.str();
        output << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto read = m68k_emit_materialized_ea_read(operation.source_ea, operation.size, data_registers, *memory,
                                                        prelude, temp_ordinal);
      if (read.ok) {
        output << "{\n" << prelude.str();
        const auto dest_reg = std::string(memory->address_registers) + "[" +
                              std::to_string(static_cast<unsigned>(operation.destination_ea.reg)) + "]";
        if (operation.size == M68kMemoryAccessWidth::word) {
          // "Word-size source operands are sign-extended to 32-bit
          // quantities" (contract, MOVEA "Description"): MOVEA.W is the one
          // sign-extending form in this whole batch.
          output << dest_reg << " = (uint32_t)(int32_t)(int16_t)(" << read.expression << ");\n";
        } else {
          output << dest_reg << " = (" << read.expression << ");\n";
        }
        // MOVEA's entire CCR/SR is unaffected: no status_register update is
        // emitted at all (contract, MOVEA "Condition Codes": "Not
        // affected.").
        output << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::write_clr: {
    if (memory != nullptr) {
      // SEG-007-T157 / ADR-0019 Stage B: an auto-updating `(An)+` / `-(An)`
      // CLR destination is lowered through the same deferred-address-
      // register-commit technique already established for the add family
      // above (docs/architecture/c4-add-family-auto-update-commit-contract.md)
      // -- snapshot the EA into one local, apply the predecrement/
      // postincrement adjustment to that local, route the WRITE through the
      // existing m68k_emit_routed_write boundary, and commit the
      // architectural An exactly once, strictly after that routed write, in
      // one statement. A runtime stop reached by the routed write returns
      // from inside its own generated statement (m68k_emit_routed_write's
      // own `return transfer;`), textually before the commit and before the
      // CCR update below, so a failed access never exposes a partial
      // auto-update or a partial CCR update. `write_clr`'s own CCR
      // semantics are reused completely unchanged: no new M68kIrKind, no new
      // CCR formula.
      const bool auto_destination = operation.destination_ea.mode == M68kEaMode::address_predec ||
                                    operation.destination_ea.mode == M68kEaMode::address_postinc;
      if (memory->runtime_routing && auto_destination) {
        const auto width = static_cast<std::uint32_t>(operation.size);
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        // The MC68000 A7-byte step-by-2 rule, identical to every other
        // auto-update lowering in this file.
        const auto step = (reg == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_clr_auto_ea = " << an_expr << ";\n";
        if (operation.destination_ea.mode == M68kEaMode::address_predec)
          body << "m68k_clr_auto_ea -= UINT32_C(" << step << ");\n";
        m68k_emit_routed_write(body, "m68k_clr_auto_ea", operation.size, *memory, "UINT32_C(0)", temp_ordinal);
        if (operation.destination_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_clr_auto_ea += UINT32_C(" << step << ");\n";
        body << an_expr << " = m68k_clr_auto_ea;\n";
        // Always N=0,Z=1,V=0,C=0, X unaffected -- identical fixed pattern to
        // the non-auto-update branch below, only reached after the commit.
        body << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFF0)) | UINT16_C(4));\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream write_prelude;
      const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                            "UINT32_C(0)", write_prelude, temp_ordinal);
      if (write.ok) {
        output << write_prelude.str() << write.expression << "\n";
        // Always N=0,Z=1,V=0,C=0, X unaffected -- regardless of the cleared
        // value (contract, CLR "Condition Codes"): a fixed pattern, not
        // m68k_move_result_ccr applied to a value.
        output << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFF0)) | UINT16_C(4));\n";
        output << write.postlude;
        output << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n";
      }
    }
    break;
  }
  case M68kIrKind::logical_not: {
    if (memory != nullptr) {
      // SEG-007-T168 / SEG-021-T005: NOT is a one-address RMW. In the routed
      // context an auto-updating destination uses the deferred address-register
      // commit below; the non-routed path lowers it through the shared EA
      // primitives (the read applies the single pointer mutation; the write is
      // retargeted to `address_indirect` further down so it is never re-applied).
      // SEG-021-T005: an auto-updating NOT destination is lowered by the same
      // operation-local deferred address-register commit NEG and the ADD family use
      // (docs/architecture/c4-add-family-auto-update-commit-contract.md Q2/Q5): one
      // snapshot local, one routed read and one routed write at the same
      // address, the single live-register commit strictly after both accesses.
      if (memory->runtime_routing &&
          (operation.destination_ea.mode == M68kEaMode::address_predec ||
           operation.destination_ea.mode == M68kEaMode::address_postinc)) {
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        const auto width = static_cast<std::uint32_t>(operation.size);
        const auto step = (reg == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_not_auto_ea = " << an_expr << ";\n";
        if (operation.destination_ea.mode == M68kEaMode::address_predec)
          body << "m68k_not_auto_ea -= UINT32_C(" << step << ");\n";
        std::string not_destination;
        m68k_emit_routed_read(body, "m68k_not_auto_ea", operation.size, *memory, not_destination, temp_ordinal);
        body << "{ const uint32_t not_result = ~(" << not_destination << "); ";
        m68k_emit_routed_write(body, "m68k_not_auto_ea", operation.size, *memory, "not_result", temp_ordinal);
        if (operation.destination_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_not_auto_ea += UINT32_C(" << step << ");\n";
        M68kLogicalResultSpecification::emit_c_update(body, status_register, "not_result", operation.size);
        body << " }\n" << an_expr << " = m68k_not_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers,
                                                  *memory, prelude, temp_ordinal);
      if (destination.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t not_result = ~(" << destination.expression << "); ";
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory,
                                              "not_result", write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          M68kLogicalResultSpecification::emit_c_update(output, status_register, "not_result", operation.size);
          output << " }\n";
        }
        output << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::negate_decimal:
  case M68kIrKind::negate_extended:
  case M68kIrKind::negate_word: {
    if (memory != nullptr) {
      // SEG-021-T014: NEG and NEGX share this one-address RMW lowering. NEGX computes 0 - operand - X
      // through M68kExtendedArithmeticSpecification (X/C from borrow, sticky Z); NEG's CCR is the plain
      // subtraction rule with X copied from C.
      const bool extended = operation.kind == M68kIrKind::negate_extended;
      const bool decimal = operation.kind == M68kIrKind::negate_decimal;  // SEG-021-T015: NBCD shares this RMW path
      const auto emit_neg_compute = [&](std::ostringstream &o) {
        if (decimal) {
          M68kDecimalArithmeticSpecification::emit_c_compute(o, status_register, M68kDecimalArithmeticKind::negate,
                                                             "UINT32_C(0)", "neg_destination");
          o << "const uint32_t neg_result = bcd_result; ";
        } else if (extended) {
          M68kExtendedArithmeticSpecification::emit_c_compute(o, status_register, M68kExtendedArithmeticKind::subtract,
                                                              "neg_destination", "UINT32_C(0)", operation.size);
          o << "const uint32_t neg_result = xa_result; ";
        } else {
          o << "const uint32_t neg_result = UINT32_C(0) - neg_destination; ";
        }
      };
      const auto emit_neg_update = [&](std::ostringstream &o) {
        if (decimal)
          M68kDecimalArithmeticSpecification::emit_c_update(o, status_register);
        else if (extended)
          M68kExtendedArithmeticSpecification::emit_c_update(o, status_register, M68kExtendedArithmeticKind::subtract,
                                                             operation.size);
        else
          M68kSubtractionResultSpecification::emit_c_update(o, status_register, "neg_destination", "UINT32_C(0)",
                                                            operation.size, M68kExtendFlagPolicy::from_carry);
      };
      const bool auto_destination = operation.destination_ea.mode == M68kEaMode::address_predec ||
                                    operation.destination_ea.mode == M68kEaMode::address_postinc;
      // NEG is a one-address RMW operation. Its two accesses must not
      // independently evaluate an auto-updating EA: that would update An
      // twice and make the write use a different address.
      if (auto_destination) {
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        const auto width = static_cast<std::uint32_t>(operation.size);
        const auto step = (reg == 7U && operation.size == M68kMemoryAccessWidth::byte) ? 2U : width;
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_neg_auto_ea = " << an_expr << ";\n";
        if (operation.destination_ea.mode == M68kEaMode::address_predec)
          body << "m68k_neg_auto_ea -= UINT32_C(" << step << ");\n";
        std::string neg_destination;
        if (memory->runtime_routing) {
          m68k_emit_routed_read(body, "m68k_neg_auto_ea", operation.size, *memory, neg_destination, temp_ordinal);
        } else {
          const auto local = "m68k_neg_auto_addr";
          m68k_emit_runtime_ea_guard(body, local, "m68k_neg_auto_ea", operation.size, *memory);
          neg_destination = m68k_emit_ram_read_expr(
              memory->ram_array, std::string(local) + " - UINT32_C(0x" + hex(memory->linear_memory_begin, 8) + ")",
              operation.size);
        }
        body << "{ const uint32_t neg_destination = " << neg_destination << "; ";
        emit_neg_compute(body);
        if (memory->runtime_routing) {
          m68k_emit_routed_write(body, "m68k_neg_auto_ea", operation.size, *memory, "neg_result", temp_ordinal);
        } else {
          m68k_emit_ram_write_stmts(
              body, memory->ram_array,
              std::string("m68k_neg_auto_addr - UINT32_C(0x") + hex(memory->linear_memory_begin, 8) + ")",
              "neg_result", operation.size);
        }
        if (operation.destination_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_neg_auto_ea += UINT32_C(" << step << ");\n";
        emit_neg_update(body);
        body << " }\n" << an_expr << " = m68k_neg_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers,
                                                  *memory, prelude, temp_ordinal);
      if (destination.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t neg_destination = " << destination.expression << "; ";
        emit_neg_compute(output);
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                              "neg_result", write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          emit_neg_update(output);
          output << " }\n";
        }
        output << destination.postlude << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::exchange_registers: {
    // SEG-021-T016: EXG swaps the two full 32-bit registers through one temporary; no condition code changes.
    // The same register named twice (EXG Dn,Dn) is the identity.
    if (memory != nullptr) {
      const auto slot = [&](const M68kEffectiveAddress &ea) {
        return std::string(ea.mode == M68kEaMode::address_register ? memory->address_registers : data_registers) + "[" +
               std::to_string(static_cast<unsigned>(ea.reg)) + "]";
      };
      const auto first = slot(operation.source_ea);
      const auto second = slot(operation.destination_ea);
      output << "{ const uint32_t exg_saved = " << first << "; " << first << " = " << second << "; " << second
             << " = exg_saved; }\n"
             << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n";
    }
    break;
  }
  case M68kIrKind::movep_transfer: {
    // SEG-021-T016: MOVEP transfers `size` bytes between Dn and every second byte of d16(An), most significant byte
    // first (Motorola M68000 Family Programmer's Reference Manual, MOVEP). Every byte access is an individual routed /
    // guarded byte access; a memory-to-register transfer performs all reads before writing Dn, and PC advances last,
    // so a runtime stop on any access leaves the register state unchanged. An is never updated.
    if (memory != nullptr) {
      const bool to_register = operation.destination_ea.mode == M68kEaMode::data_register;
      const auto &memory_ea = to_register ? operation.source_ea : operation.destination_ea;
      const auto &dn_ea = to_register ? operation.destination_ea : operation.source_ea;
      const unsigned count = operation.size == M68kMemoryAccessWidth::long_word ? 4U : 2U;
      const auto runtime = m68k_emit_runtime_ea_address(memory_ea, M68kMemoryAccessWidth::byte,
                                                         memory->address_registers, data_registers);
      const auto dn = std::string(data_registers) + "[" + std::to_string(static_cast<unsigned>(dn_ea.reg)) + "]";
      const auto byte_at = [&](unsigned index) { return "m68k_movep_base + UINT32_C(" + std::to_string(2U * index) + ")"; };
      unsigned temp_ordinal = 0U;
      std::ostringstream body;
      if (memory->runtime_routing) {
        body << "const uint32_t m68k_movep_base = " << runtime.address_expr << ";\n";
      } else {
        // Direct linear window: the whole 2*count-1 byte span must lie inside [begin, end).
        body << "const uint32_t m68k_movep_base = " << runtime.address_expr << "; if (m68k_movep_base < UINT32_C(0x"
             << hex(memory->linear_memory_begin, 8) << ") || m68k_movep_base > UINT32_C(0x"
             << hex(memory->linear_memory_end, 8) << ") - UINT32_C(" << (2U * count - 1U) << ")) { return 1; }\n";
      }
      if (to_register) {
        std::vector<std::string> values;
        for (unsigned index = 0; index < count; ++index) {
          if (memory->runtime_routing) {
            std::string value;
            m68k_emit_routed_read(body, byte_at(index), M68kMemoryAccessWidth::byte, *memory, value, temp_ordinal);
            values.push_back(value);
          } else {
            values.push_back("(uint32_t)" + std::string(memory->ram_array) + "[m68k_movep_base - UINT32_C(0x" +
                             hex(memory->linear_memory_begin, 8) + ") + UINT32_C(" + std::to_string(2U * index) + ")]");
          }
        }
        std::string combined;
        for (unsigned index = 0; index < count; ++index) {
          const auto shift = 8U * (count - 1U - index);
          combined += (index == 0U ? "" : " | ") + std::string("((uint32_t)") + values[index] + " << " +
                      std::to_string(shift) + "U)";
        }
        body << "{ const uint32_t m68k_movep_value = " << combined << "; ";
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(dn_ea, operation.size, data_registers, *memory, "m68k_movep_value",
                                              write_prelude, temp_ordinal);
        if (!write.ok) break;
        body << write_prelude.str() << write.expression << " }\n";
      } else {
        body << "{ const uint32_t m68k_movep_value = " << dn << ";\n";
        for (unsigned index = 0; index < count; ++index) {
          const auto shift = 8U * (count - 1U - index);
          const auto byte_value = "(uint8_t)(m68k_movep_value >> " + std::to_string(shift) + "U)";
          if (memory->runtime_routing) {
            m68k_emit_routed_write(body, byte_at(index), M68kMemoryAccessWidth::byte, *memory, byte_value,
                                   temp_ordinal);
          } else {
            m68k_emit_ram_write_stmts(body,
                                      memory->ram_array,
                                      "m68k_movep_base - UINT32_C(0x" + hex(memory->linear_memory_begin, 8) +
                                          ") + UINT32_C(" + std::to_string(2U * index) + ")",
                                      byte_value, M68kMemoryAccessWidth::byte);
          }
        }
        body << " }\n";
      }
      output << "{\n" << body.str() << memory->program_counter << " += UINT32_C(" << operation.provenance.length.value
             << ");\n}\n";
    }
    break;
  }
  case M68kIrKind::set_conditional: {
    // SEG-021-T016: Scc writes 0xFF (condition true) or 0x00 to its byte destination and changes no condition code.
    // The condition comes from the one shared m68k_condition_c_expr owner (Bcc/DBcc use the same). A Dn destination is
    // register-only (no memory access). A MEMORY destination is read before it is written (MC68000 family: "a memory
    // destination is read before it is written"; the value is discarded and the condition does not depend on it), so
    // it is lowered NOT/TAS-shaped: one EA computation, one routed byte read, one routed byte write to the same EA,
    // the single deferred address-register commit strictly after the write, PC last. A stop on the read or write
    // returns from inside that access, before any commit, CCR change or PC advance.
    if (memory != nullptr) {
      std::ostringstream condition_text;
      condition_text << "const uint8_t m68k_scc_condition_true = (uint8_t)("
                     << m68k_condition_c_expr(operation.condition, status_register) << "); ";
      if (!memory->timing_scc_true.empty())
        condition_text << memory->timing_scc_true << " = m68k_scc_condition_true; ";
      const std::string value = "(m68k_scc_condition_true != 0U ? UINT32_C(0xFF) : UINT32_C(0))";
      const auto length = operation.provenance.length.value;
      const bool auto_destination = operation.destination_ea.mode == M68kEaMode::address_predec ||
                                    operation.destination_ea.mode == M68kEaMode::address_postinc;
      if (operation.destination_ea.mode == M68kEaMode::data_register) {
        unsigned temp_ordinal = 0U;
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory, value,
                                              write_prelude, temp_ordinal);
        if (write.ok) {
          output << "{\n" << condition_text.str() << "\n" << write_prelude.str() << write.expression << "\n"
                 << write.postlude << memory->program_counter << " += UINT32_C(" << length << ");\n}\n";
        }
        break;
      }
      if (memory->runtime_routing && auto_destination) {
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        const auto step = reg == 7U ? 2U : 1U;  // byte operand; A7 steps by two
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << condition_text.str() << "\nuint32_t m68k_scc_auto_ea = " << an_expr << ";\n";
        if (operation.destination_ea.mode == M68kEaMode::address_predec)
          body << "m68k_scc_auto_ea -= UINT32_C(" << step << ");\n";
        std::string discarded;
        m68k_emit_routed_read(body, "m68k_scc_auto_ea", operation.size, *memory, discarded, temp_ordinal);
        body << "(void)" << discarded << ";\n";
        m68k_emit_routed_write(body, "m68k_scc_auto_ea", operation.size, *memory, value, temp_ordinal);
        if (operation.destination_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_scc_auto_ea += UINT32_C(" << step << ");\n";
        body << an_expr << " = m68k_scc_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C(" << length << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto discarded = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers, *memory,
                                               prelude, temp_ordinal);
      if (discarded.ok) {
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;  // the read already applied the single pointer mutation
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, value, write_prelude,
                                              temp_ordinal);
        if (write.ok) {
          output << "{\n" << condition_text.str() << "\n" << prelude.str() << "(void)(" << discarded.expression
                 << ");\n" << write_prelude.str() << write.expression << "\n" << discarded.postlude
                 << memory->program_counter << " += UINT32_C(" << length << ");\n}\n";
        }
      }
    }
    break;
  }
  case M68kIrKind::test_and_set: {
    // SEG-021-T016: TAS sets N/Z from the operand byte (V/C cleared, X preserved: the logical-result rule), then
    // writes the byte with bit 7 set. Only the CPU-visible read-modify-write is lowered here; the indivisible bus
    // cycle stays platform-owned. Lowering mirrors NOT's one-address RMW, including the deferred commit.
    if (memory != nullptr) {
      const auto length = operation.provenance.length.value;
      const auto auto_destination_mode = operation.destination_ea.mode;
      if (memory->runtime_routing && (auto_destination_mode == M68kEaMode::address_predec ||
                                      auto_destination_mode == M68kEaMode::address_postinc)) {
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        const auto step = reg == 7U ? 2U : 1U;
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_tas_auto_ea = " << an_expr << ";\n";
        if (auto_destination_mode == M68kEaMode::address_predec) body << "m68k_tas_auto_ea -= UINT32_C(" << step << ");\n";
        std::string operand;
        m68k_emit_routed_read(body, "m68k_tas_auto_ea", operation.size, *memory, operand, temp_ordinal);
        body << "{ const uint32_t tas_operand = (" << operand << ") & UINT32_C(0xFF); const uint32_t tas_result = tas_operand | UINT32_C(0x80); ";
        m68k_emit_routed_write(body, "m68k_tas_auto_ea", operation.size, *memory, "tas_result", temp_ordinal);
        if (auto_destination_mode == M68kEaMode::address_postinc) body << "m68k_tas_auto_ea += UINT32_C(" << step << ");\n";
        M68kLogicalResultSpecification::emit_c_update(body, status_register, "tas_operand", operation.size);
        body << " }\n" << an_expr << " = m68k_tas_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C(" << length << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto operand = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers, *memory,
                                             prelude, temp_ordinal);
      if (operand.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t tas_operand = (" << operand.expression
               << ") & UINT32_C(0xFF); const uint32_t tas_result = tas_operand | UINT32_C(0x80); ";
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;  // the read already applied the single pointer mutation
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, "tas_result",
                                              write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          M68kLogicalResultSpecification::emit_c_update(output, status_register, "tas_operand", operation.size);
          output << " }\n";
        }
        output << operand.postlude << memory->program_counter << " += UINT32_C(" << length << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::add_decimal:
  case M68kIrKind::subtract_decimal:
  case M68kIrKind::add_extended:
  case M68kIrKind::subtract_extended:
  case M68kIrKind::compare_memory: {
    // SEG-021-T014/T015: see m68k_emit_extended_pair.
    if (memory != nullptr) (void)m68k_emit_extended_pair(output, operation, data_registers, status_register, *memory);
    break;
  }
  // SEG-007-T025 (Batch C, C1): SWAP/EXT.W/EXT.L. Each reads and writes the
  // same Dn slot (destination_ea; there is no separate source_ea), so the
  // materialized-read-then-write shape below mirrors the existing
  // add/subtract/logical register-destination cases, just without a second
  // (source) operand. CCR reuses M68kMoveResultCcrSpecification::emit_c_update
  // unchanged (contract: "no new CCR formula"), fed the already correctly
  // sized/sign-extended written value so N/Z always reflect the real result
  // width (whole register for SWAP/EXT.L, low word only for EXT.W -- matching
  // the pinned Musashi ext16 opcode body's FLAG_N/FLAG_Z, which read only the
  // post-extension low word, not the full 32-bit register).
  case M68kIrKind::write_swap: {
    if (memory != nullptr) {
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto read = m68k_emit_materialized_ea_read(operation.destination_ea, operation.size, data_registers,
                                                        *memory, prelude, temp_ordinal);
      if (read.ok) {
        output << "{\n" << prelude.str() << "{ const uint32_t swap_result = ((" << read.expression
               << " << 16U) | (" << read.expression << " >> 16U)); ";
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                              "swap_result", write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          M68kMoveResultCcrSpecification::emit_c_update(output, status_register, "swap_result");
          output << " }\n";
        }
        output << read.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long: {
    if (memory != nullptr) {
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto read = m68k_emit_materialized_ea_read(operation.destination_ea, operation.size, data_registers,
                                                        *memory, prelude, temp_ordinal);
      if (read.ok) {
        // EXT.W sign-extends the low BYTE into the low word (narrow ==
        // byte); EXT.L sign-extends the low WORD into the full long (narrow
        // == word). `operation.size` is the WRITE width (word/long_word
        // respectively), never the narrow source width.
        const auto narrow_width = operation.kind == M68kIrKind::sign_extend_word ? M68kMemoryAccessWidth::byte
                                                                                  : M68kMemoryAccessWidth::word;
        output << "{\n" << prelude.str() << "{ const uint32_t ext_result = "
               << m68k_sign_extend_expr(read.expression, narrow_width) << "; ";
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                              "ext_result", write_prelude, temp_ordinal);
        if (write.ok) {
          output << write_prelude.str() << write.expression << ' ';
          M68kMoveResultCcrSpecification::emit_c_update(output, status_register, "ext_result");
          output << " }\n";
        }
        output << read.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::load_effective_address: {
    // LEA never accesses memory at all: it only computes an address value
    // (Dn/An/(An)+/-(An)/immediate/pc_disp16 with runtime-only content are
    // excluded from its legal source set entirely; every legal source is one
    // of the seven control-addressing forms, none of which involves a bounds
    // guard for LEA itself -- computing "any" address, mapped or not, is
    // real MC68000 LEA behavior).
    if (memory != nullptr) {
      // Most legacy structured-C callers use the conventional local `pc`,
      // while persistent-runtime callers supply their context member.  LEA
      // shares that one program-counter projection with every other lowering.
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      const auto dest_reg = std::string(memory->address_registers) + "[" +
                            std::to_string(static_cast<unsigned>(operation.destination_ea.reg)) + "]";
      const auto &ea = operation.source_ea;
      if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long ||
          ea.mode == M68kEaMode::pc_disp16) {
        output << dest_reg << " = UINT32_C(0x" << hex(ea.absolute_address, 8) << ");\n";
      } else {
        // SEG-007-T135: pass the data-register bank so the shared helper can
        // lower a Dn-indexed brief `(d8,An,Xn)` LEA source; An-indexed and
        // every previously-supported control form are unaffected.
        const auto runtime = m68k_emit_runtime_ea_address(ea, M68kMemoryAccessWidth::long_word,
                                                           memory->address_registers, data_registers);
        output << runtime.prelude << dest_reg << " = " << runtime.address_expr << ";\n" << runtime.postlude;
      }
      output << program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n";
    }
    break;
  }
  // SEG-007-T025 (Batch C, C2): PEA/LINK/UNLK. Each reuses the existing
  // address_predec/address_postinc EA-write/read machinery on register 7 to
  // model a real A7 data-stack long push/pop -- never the static CFG
  // continuation stack (M68kStackEffectKind::push_static_continuation is a
  // distinct fact, unused here). No new memory router, RAM array, or
  // instruction-specific device routing is introduced.
  case M68kIrKind::push_effective_address: {
    // PEA computes source_ea's ADDRESS only (never its contents), exactly
    // like load_effective_address above, then pushes that 32-bit address.
    // The address is captured into `pea_address` BEFORE the -(A7) write's
    // own predecrement runs, so a legal `PEA (A7)` source observes the
    // PRE-decrement A7 -- matching Musashi's pea handler, which computes its
    // EA into a snapshot local before calling m68ki_push_32 (contract: "PEA
    // is address calculation, not operand read"; see the C2 PR notes for the
    // alias-ordering rationale, which is the opposite of LINK's below).
    if (memory != nullptr) {
      const auto &ea = operation.source_ea;
      std::ostringstream prelude;
      std::string address_expr;
      if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long ||
          ea.mode == M68kEaMode::pc_disp16) {
        address_expr = "UINT32_C(0x" + hex(ea.absolute_address, 8) + ")";
      } else {
        // SEG-021-T011: pass the data-register bank too, exactly like
        // `load_effective_address` above -- PEA's control-EA ceiling now
        // also admits the brief-format `(d8,An,Xn)`/`(d8,PC,Xn)` Dn-indexed
        // forms (`m68k_ea_pea_control_modes`), and the shared helper needs
        // `data_registers` to lower a Dn index for either.
        const auto runtime = m68k_emit_runtime_ea_address(ea, M68kMemoryAccessWidth::long_word,
                                                           memory->address_registers, data_registers);
        prelude << runtime.prelude;
        address_expr = runtime.address_expr;
        prelude << runtime.postlude;
      }
      output << "{\n" << prelude.str() << "const uint32_t pea_address = " << address_expr << ";\n";
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      if (memory->runtime_routing) {
        // SEG-021-T011: PEA's -(A7) push is now admitted to the C4
        // block-dispatch/AOT routed path, so it can no longer go through the
        // shared m68k_emit_ea_write's address_predec branch, which mutates the
        // LIVE A7 register in its own prelude, unconditionally, before the
        // routed write's own fail-closed guard has run (see
        // docs/architecture/c4-move-predecrement-postincrement-commit-contract.md,
        // whose Q1-Q5 deferred-commit technique this generalizes to PEA's
        // single always-`-(A7)` push target). A7 is instead snapshotted into a
        // dedicated local, decremented on that local only, and the routed
        // write's own generated statement contains its own fail-closed early
        // return -- so the live A7 writeback below is reached only after the
        // write has already succeeded.
        unsigned temp_ordinal = 0U;
        output << "uint32_t m68k_pea_a7 = " << memory->address_registers << "[7];\n";
        output << "m68k_pea_a7 -= UINT32_C(4);\n";
        m68k_emit_routed_write(output, "m68k_pea_a7", M68kMemoryAccessWidth::long_word, *memory, "pea_address",
                               temp_ordinal);
        output << memory->address_registers << "[7] = m68k_pea_a7;\n";
        output << program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      } else {
        unsigned temp_ordinal = 0U;
        std::ostringstream write_prelude;
        const M68kEffectiveAddress push_target{M68kEaMode::address_predec, 7, 0, 0, 0, 0};
        const auto write = m68k_emit_ea_write(push_target, M68kMemoryAccessWidth::long_word, data_registers, *memory,
                                              "pea_address", write_prelude, temp_ordinal);
        if (write.ok) {
          // SEG-021-T011: use the same conventional-local-vs-persistent-context
          // program-counter projection `load_effective_address` above already
          // uses, instead of always assuming a bare local `pc`. This
          // non-routed branch keeps the original, previously-only, direct-
          // emission shape (no runtime_emitter is guaranteed available here),
          // matching write_move's own runtime_routing gate above.
          output << write_prelude.str() << write.expression << '\n'
                 << program_counter << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        }
      }
    }
    break;
  }
  case M68kIrKind::link_frame: {
    // LINK pushes the OLD An value, then An = (new) A7, then A7 += disp
    // (contract: "LINK.W semantics"). The pushed value is embedded as the
    // live `a[reg]` expression, textually sequenced by the write helper
    // AFTER the -(A7) write's own predecrement statement -- deliberately the
    // OPPOSITE materialization order from PEA above. This reproduces
    // Musashi's real `LINK A7,#disp` hardware-alias behavior (its a7-special
    // handler pushes the ALREADY-DECREMENTED A7, per m68k_in.c's `link 16 .
    // a7` entry) for reg==7, while remaining the plain untouched old An
    // value for every reg!=7 -- with no runtime branch on reg==7, since
    // array-based register aliasing (a[7] IS A7) does the work.
    if (memory != nullptr && memory->runtime_routing) {
      // SEG-021-T016: routed LINK (AOT/C4 admission). The live A7 is never
      // touched before the routed push has succeeded (a routed stop returns
      // first); A7 is decremented on a local. The pushed value keeps the
      // Musashi reg==7 alias (the already-decremented A7) and is the old An
      // otherwise. Commit order: An = A7', A7 = A7' + disp, then PC.
      const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
      const auto an = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
      const auto a7 = std::string(memory->address_registers) + "[7]";
      unsigned temp_ordinal = 0U;
      output << "{\nuint32_t m68k_link_a7 = " << a7 << ";\nm68k_link_a7 -= UINT32_C(4);\n";
      m68k_emit_routed_write(output, "m68k_link_a7", M68kMemoryAccessWidth::long_word, *memory,
                             reg == 7U ? std::string("m68k_link_a7") : an, temp_ordinal);
      output << an << " = m68k_link_a7;\n"
             << a7 << " = m68k_link_a7 + "
             << m68k_sign_extend_expr("UINT32_C(0x" + hex(operation.source_ea.immediate_value & 0xFFFFU, 4) + ")",
                                      M68kMemoryAccessWidth::word)
             << ";\n"
             << (memory->program_counter.empty() ? std::string_view("pc") : std::string_view(memory->program_counter))
             << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
    } else if (memory != nullptr) {
      const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
      const auto register_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
      unsigned temp_ordinal = 0U;
      std::ostringstream write_prelude;
      const M68kEffectiveAddress push_target{M68kEaMode::address_predec, 7, 0, 0, 0, 0};
      const auto write = m68k_emit_ea_write(push_target, M68kMemoryAccessWidth::long_word, data_registers, *memory,
                                            register_expr, write_prelude, temp_ordinal);
      if (write.ok) {
        output << "{\n" << write_prelude.str() << write.expression << ' '
               << memory->address_registers << "[" << reg << "] = " << memory->address_registers << "[7]; "
               << memory->address_registers << "[7] = " << memory->address_registers << "[7] + "
               << m68k_sign_extend_expr(
                      "UINT32_C(0x" + hex(operation.source_ea.immediate_value & 0xFFFFU, 4) + ")",
                      M68kMemoryAccessWidth::word)
               << ";\n" << write.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::unlink_frame: {
    // UNLK sets A7 = An, then An = the popped saved long (contract: "UNLK
    // semantics"). `a[7] = a[reg];` is emitted first (a no-op when reg==7),
    // then the postincrement pop reads whatever A7 already is at that
    // point -- reproducing Musashi's `UNLK A7` hardware-alias behavior
    // (its a7-special handler is a plain `A7 = read32(A7)`, no increment
    // survives the final `*r_dst = pulled` assignment in the generic
    // handler either) with no runtime branch on reg==7.
    if (memory != nullptr && memory->runtime_routing) {
      // SEG-021-T016: routed UNLK. Read the saved long at An through the
      // routed gate before any register write; then A7 = An + 4, An = value
      // (An assigned last so UNLK A7 yields the popped value, like Musashi).
      const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
      const auto an = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
      unsigned temp_ordinal = 0U;
      output << "{\nconst uint32_t m68k_unlk_ea = " << an << ";\n";
      std::string expr;
      m68k_emit_routed_read(output, "m68k_unlk_ea", M68kMemoryAccessWidth::long_word, *memory, expr, temp_ordinal);
      output << "const uint32_t m68k_unlk_value = " << expr << ";\n"
             << memory->address_registers << "[7] = m68k_unlk_ea + UINT32_C(4);\n"
             << an << " = m68k_unlk_value;\n"
             << (memory->program_counter.empty() ? std::string_view("pc") : std::string_view(memory->program_counter))
             << " += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
    } else if (memory != nullptr) {
      const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
      output << "{\n" << memory->address_registers << "[7] = " << memory->address_registers << "[" << reg
             << "];\n";
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const M68kEffectiveAddress pop_source{M68kEaMode::address_postinc, 7, 0, 0, 0, 0};
      const auto read = m68k_emit_materialized_ea_read(pop_source, M68kMemoryAccessWidth::long_word, data_registers,
                                                        *memory, prelude, temp_ordinal);
      if (read.ok) {
        output << prelude.str() << memory->address_registers << "[" << reg << "] = " << read.expression << ";\n"
               << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::bit_test:
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set: {
    // SEG-007-T025 (Batch C, C3): BTST/BCHG/BCLR/BSET. The bit number is
    // materialized FIRST (contract: "bit number materialization"), before
    // any destination read/write, so an alias case such as `BCHG D0,D0`
    // reads the ORIGINAL source-register value rather than a value the
    // destination write may have already changed. The destination itself
    // reuses the exact one-ARCHITECTURAL-address RMW shape already
    // established by add/subtract/logical above: `destination` is a plain
    // (unmaterialized) read of the EA (a runtime-relative form's own
    // predecrement mutation happens once, in the read's prelude, before
    // either access -- the read and the later write each derive their own
    // equivalent generated-C address local from the same EA formula, safe
    // because nothing mutates the address register in between); the write's
    // own predecrement/postincrement mutation is retargeted to
    // `address_indirect` so it is never re-applied, and any postincrement
    // postlude is appended exactly once, after the write, so BTST's
    // read-only EA side effects ((An)+/-(An) register auto-update) still
    // occur even though it never writes back
    // (contract: "BTST is read-only" -- "addressing-mode effects, not
    // destination mutation").
    if (memory != nullptr) {
      if (m68k_emit_routed_bit_auto_update(output, operation, data_registers, status_register, *memory)) break;
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto bit_number = m68k_emit_materialized_ea_read(operation.source_ea, M68kMemoryAccessWidth::long_word,
                                                              data_registers, *memory, prelude, temp_ordinal);
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers, *memory,
                                                  prelude, temp_ordinal);
      if (bit_number.ok && destination.ok) {
        const auto bit_kind = operation.kind == M68kIrKind::bit_test ? M68kBitOperationKind::test
                             : operation.kind == M68kIrKind::bit_change ? M68kBitOperationKind::change
                             : operation.kind == M68kIrKind::bit_clear ? M68kBitOperationKind::clear
                                                                        : M68kBitOperationKind::set;
        output << "{\n" << prelude.str() << "{ ";
        M68kBitOperationSpecification::emit_c_update(output, bit_kind, destination.expression, bit_number.expression,
                                                      operation.size, "bit_index", "bit_mask", "bit_set",
                                                      "bit_result");
        output << ' ';
        if (operation.kind != M68kIrKind::bit_test) {
          auto write_ea = operation.destination_ea;
          if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
            write_ea.mode = M68kEaMode::address_indirect;
          std::ostringstream write_prelude;
          const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, "bit_result",
                                                write_prelude, temp_ordinal);
          if (write.ok) output << write_prelude.str() << write.expression << ' ';
        } else {
          // BTST never writes destination_ea (read-only); `bit_result`
          // (always equal to `original` for `test`, per
          // M68kBitOperationSpecification's shared contract) is otherwise
          // unused in strict C11, which -Wunused-variable/-Werror reject.
          output << "(void)bit_result; ";
        }
        M68kBitTestCcrSpecification::emit_c_update(output, status_register, "bit_set");
        output << " }\n" << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value
               << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::general_branch: {
    // SEG-007-T025 (Batch C, C4a): BRA (operation.condition == always) and
    // every selected Bcc condition. Bcc reads SR but never modifies it;
    // both the taken target and the fallthrough are deterministic C11
    // expressions computed through the one shared m68k_branch_target
    // formula and m68k_condition_c_expr -- never a second target/condition
    // formula.
    if (memory != nullptr) {
      const auto raw = operation.source_ea.immediate_value;
      const auto signed_disp = operation.size == M68kMemoryAccessWidth::byte
                                    ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                    : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
      const auto target = m68k_branch_target(operation.provenance.source.address.value, signed_disp);
      const auto fallthrough = operation.provenance.source.address.value + operation.provenance.length.value;
      if (operation.condition == M68kCondition::always) {
        output << memory->program_counter << " = UINT32_C(0x" << hex(target, 8) << ");\n";
      } else {
        output << "const uint8_t m68k_branch_taken = (uint8_t)(" << m68k_condition_c_expr(operation.condition, status_register) << ");\n";
        output << "if (m68k_branch_taken) { "
               << memory->program_counter << " = UINT32_C(0x" << hex(target, 8) << ");";
        output << " } else { "
               << memory->program_counter << " = UINT32_C(0x" << hex(fallthrough, 8) << "); }\n";
      }
    }
    break;
  }
  case M68kIrKind::dbcc_loop: {
    // SEG-007-T025 (Batch C, C4c review correction): condition-first
    // ordering is load-bearing (contract: "DBcc semantic order") -- if the
    // shared condition is TRUE, Dn is NEVER decremented and control simply
    // falls through; only when it is FALSE does the ONE shared
    // M68kDbccDecrementSpecification::emit_c_update lower the low-word
    // decrement/upper-word-preservation formula into generated C (the exact
    // same formula m68k_evaluate_dbcc_decrement applies host-side), and only
    // THEN is its returned expiry expression used to choose the branch
    // target (also computed via the one shared m68k_branch_target formula)
    // or the fallthrough. This orchestration performs no decrement/expiry
    // arithmetic of its own. Entire SR is unaffected throughout (no CCR
    // update is ever emitted).
    if (memory != nullptr) {
      const auto raw = operation.source_ea.immediate_value;
      const auto signed_disp = static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
      const auto target = m68k_branch_target(operation.provenance.source.address.value, signed_disp);
      const auto fallthrough = operation.provenance.source.address.value + operation.provenance.length.value;
      const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
      const auto dn = std::string(data_registers) + "[" + std::to_string(reg) + "]";
      output << "const uint8_t m68k_dbcc_condition_true = (uint8_t)(" << m68k_condition_c_expr(operation.condition, status_register)
             << ");\n";
      output << "if (m68k_dbcc_condition_true) { pc = UINT32_C(0x"
             << hex(fallthrough, 8) << "); } else { ";
      const auto expired = M68kDbccDecrementSpecification::emit_c_update(output, dn);
      // SEG-007-T252 / ADR-0040: the decrement/expiry orchestration itself is
      // unchanged guest CPU semantics. The former SEG-007-T107 / SEG-007-T211
      // watchdog-progress and loop-completion note emission on this branch's
      // taken/expired arms has been removed; termination/progress policy now
      // belongs to the runner (the runner's dispatch allowance),
      // not to any CPU/codegen-proven finite-loop fact.
      output << "if (" << expired << ") { pc = UINT32_C(0x" << hex(fallthrough, 8) << ");";
      output << " } else { ";
      if (!memory->timing_dbcc_taken.empty()) output << memory->timing_dbcc_taken << " = 1U; ";
      output << "pc = UINT32_C(0x" << hex(target, 8) << ");";
      output << " } }\n";
    }
    break;
  }
  case M68kIrKind::movem_transfer: {
    // SEG-007-T025 (Batch C, C5a/C5b): the emitter performs ONLY
    // orchestration (which EA-address family, how many transfers, which
    // register order) -- the one shared m68k_movem_transfer_order owner
    // supplies the transfer sequence (never re-derived here), and each
    // individual
    // transfer reuses the existing shared RAM read/write primitives
    // (m68k_emit_ram_read_expr/m68k_emit_ram_write_stmts) plus the existing
    // shared runtime-EA-address/guard primitives
    // (m68k_emit_runtime_ea_address/m68k_emit_runtime_ea_guard) exactly like
    // every other selected memory-affecting kind -- never a MOVEM-specific
    // memory routing policy. WORD memory->register transfers sign-extend
    // every loaded word to 32 bits for BOTH Dn and An (contract: "WORD
    // memory->register: EVERY loaded WORD is sign-extended"; verified
    // against pinned Musashi's "er ." handlers, which apply MAKE_INT_16 to
    // REG_DA[i] unconditionally). MOVEM never affects SR (no CCR update is
    // ever emitted). Statically-foldable RAM-resident EA forms (absolute.w/
    // absolute.l/d16(PC)) are lowered as direct literal RAM indices with no
    // runtime guard, exactly matching the existing established convention
    // for an absolute MOVE/CLR destination (contract: "a statically-
    // resolved absolute destination is always linear memory at this
    // point"); a statically-foldable memory_to_registers source the caller
    // has already resolved as ROM-resident (`memory->movem_transfer_access`/
    // `movem_transfer_values`, populated once per transfer by the exact same
    // shared m68k_resolve_absolute_test_operand resolver TST.L's own single-
    // value ROM fold already uses -- contract: "ROM: memory->register MOVEM
    // reads may succeed") is instead lowered as that already-verified
    // literal constant, never a runtime source-image re-read. (An)/d16(An)
    // reuse the ordinary runtime-guarded convention, one guard per transfer
    // (never a single whole-range guard, so this stays a plain reuse of the
    // existing per-access primitive rather than a new range concept), but
    // the EA itself is snapshotted into one local ONCE before the transfer
    // loop (see the `address_indirect`/`address_disp16` branch below) so a
    // memory->register transfer that loads the EA's own base register can
    // never corrupt a later transfer's address (contract: "snapshot the
    // ordinary MOVEM working EA once").
    if (memory != nullptr) {
      const bool store = operation.movem_direction == M68kMovemDirection::registers_to_memory;
      const auto &ea = store ? operation.destination_ea : operation.source_ea;
      // SEG-007-T025 (Batch C, C5b): predecrement (register->memory only;
      // decode's own legal-EA set never selects it for memory->register)
      // uses the shared owner's reversed shape; every other selected EA
      // form keeps C5a's ascending shape.
      const auto order = m68k_movem_transfer_order(operation.movem_register_mask,
          ea.mode == M68kEaMode::address_predec ? M68kMovemTransferOrder::predecrement
                                                 : M68kMovemTransferOrder::ascending);
      const auto width = static_cast<std::uint32_t>(operation.size);
      const bool have_resolved_reads = !store && memory->movem_transfer_access.size() == order.size() &&
                                       memory->movem_transfer_values.size() == order.size();
      const auto register_expr = [&](std::uint8_t reg) {
        return reg < 8U ? std::string(data_registers) + "[" + std::to_string(static_cast<unsigned>(reg)) + "]"
                         : std::string(memory->address_registers) + "[" + std::to_string(static_cast<unsigned>(reg) - 8U) + "]";
      };
      // SEG-007-T025 (Batch C, C5c2 audit): reuses the one shared
      // m68k_sign_extend_expr formula (contract: "search for duplicated
      // MOVEM sign-extension formulas") rather than a second, independently
      // hand-written `(uint32_t)(int32_t)(int16_t)(...)`/passthrough
      // ternary. m68k_sign_extend_expr's word/long-word branches are
      // byte-for-byte identical to what this case previously emitted
      // inline, so this is a pure ownership consolidation with no
      // generated-C text change (verified by the unchanged static-slice/
      // Musashi differential evidence).
      const auto emit_register_read = [&](std::ostringstream &body, const std::string &read_expr, std::uint8_t reg) {
        body << register_expr(reg) << " = " << m68k_sign_extend_expr(read_expr, operation.size) << ";\n";
      };
      const auto emit_transfer = [&](std::ostringstream &body, const std::string &offset_expr, std::uint8_t reg) {
        const auto reg_expr = register_expr(reg);
        if (store) {
          m68k_emit_ram_write_stmts(body, memory->ram_array, offset_expr, reg_expr, operation.size);
          body << "\n";
        } else {
          emit_register_read(body, m68k_emit_ram_read_expr(memory->ram_array, offset_expr, operation.size), reg);
        }
      };
      std::ostringstream body;
      bool ok = true;
      body << "{\n";
      // SEG-007-T067: C4's routed lowering. Every representable MOVEM EA
      // family's per-slot address is routed through the same fail-closed
      // the routed-access owner boundary used by the other memory operations.
      // SEG-007-T075's one exception is the independently validated adjacent
      // LEA fact for (An)/(An)+ reads. Its immutable values remain usable in
      // this same body only while the live instruction-boundary An snapshot
      // equals movem_transfer_fold_base; otherwise each slot takes the
      // existing generic routed path. This keeps interior entry independent
      // of predecessor execution without a second instruction body.
      if (memory->runtime_routing) {
        unsigned temp_ordinal = 0U;
        const auto emit_slot = [&](const std::string &address_expr, std::uint8_t reg) {
          if (store) {
            m68k_emit_routed_write(body, address_expr, operation.size, *memory, register_expr(reg), temp_ordinal);
          } else {
            std::string expr;
            m68k_emit_routed_read(body, address_expr, operation.size, *memory, expr, temp_ordinal);
            emit_register_read(body, expr, reg);
          }
        };
        if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long ||
            ea.mode == M68kEaMode::pc_disp16) {
          const auto base = m68k_canonical_ea_address(ea);
          for (std::size_t slot = 0; slot < order.size(); ++slot) {
            const auto address_expr =
                "UINT32_C(0x" + hex(base + static_cast<std::uint32_t>(slot) * width, 8) + ")";
            emit_slot(address_expr, order[slot]);
          }
        } else if (ea.mode == M68kEaMode::address_indirect || ea.mode == M68kEaMode::address_disp16 ||
            ea.mode == M68kEaMode::address_index8 || ea.mode == M68kEaMode::pc_index8) {
          // Same one-time working-EA snapshot discipline as the direct_flow
          // reference branch below (contract: "snapshot the ordinary MOVEM
          // working EA once") -- (An)/d16(An)/(d8,An,Xn)/(d8,PC,Xn) never
          // auto-update their own EA register (no prelude/postlude from
          // m68k_emit_runtime_ea_address for any of these four modes), so
          // every slot's address derives from this ONE snapshot, never
          // re-read from the live address/data-register arrays. SEG-021-T012:
          // (d8,An,Xn)/(d8,PC,Xn) need the index register value, so unlike
          // the plain (An)/d16(An) forms this call also passes
          // `data_registers` (the index register may be either an An or a
          // Dn; m68k_emit_runtime_ea_address selects which array to read
          // from `ea.index_is_address`).
          const auto runtime =
              m68k_emit_runtime_ea_address(ea, operation.size, memory->address_registers, data_registers);
          body << runtime.prelude;
          const auto base_local = "m68k_movem_base_" + std::to_string(temp_ordinal++);
          body << "const uint32_t " << base_local << " = " << runtime.address_expr << "; (void)" << base_local
               << ";\n";
          const bool guarded_fold = have_resolved_reads && ea.mode == M68kEaMode::address_indirect &&
                                    memory->movem_transfer_fold_base.has_value();
          std::string fold_local;
          if (guarded_fold) {
            fold_local = "m68k_movem_use_fold_" + std::to_string(temp_ordinal++);
            body << "const uint8_t " << fold_local << " = (uint8_t)(" << base_local
                 << " == UINT32_C(0x" << hex(*memory->movem_transfer_fold_base, 8) << "));\n";
          }
          for (std::size_t slot = 0; slot < order.size(); ++slot) {
            // SEG-007-T075: the bounded adjacent LEA->MOVEM fold
            // (docs/architecture/c4-movem-adjacent-lea-constant-propagation-
            // contract.md). Only ever true for plain address_indirect --
            // never address_disp16, which this fold does not cover -- and
            // only when the C4 caller has independently reverified the
            // producer/consumer fact and populated movem_transfer_access/
            // movem_transfer_values for every slot of this transfer.
            const bool rom_folded = have_resolved_reads && ea.mode == M68kEaMode::address_indirect &&
                memory->movem_transfer_access[slot] == M68kOperandAccess::resolved_constant;
            const auto address_expr = slot == 0U ? base_local
                                                   : "(uint32_t)(" + base_local + " + UINT32_C(" +
                                                         std::to_string(static_cast<std::uint32_t>(slot) * width) +
                                                         "))";
            if (rom_folded && guarded_fold) {
              body << "if (" << fold_local << ") {\n";
              emit_register_read(body, "UINT32_C(0x" + hex(memory->movem_transfer_values[slot], 8) + ")", order[slot]);
              body << "} else {\n";
              emit_slot(address_expr, order[slot]);
              body << "}\n";
            } else {
              emit_slot(address_expr, order[slot]);
            }
          }
        } else if (ea.mode == M68kEaMode::address_predec) {
          // Same one final architectural An writeback discipline as the
          // direct_flow reference branch below: the live address-register
          // array is never assigned until AFTER the whole transfer loop
          // completes, so a routed access that fails mid-transfer (which
          // returns a runtime stop control transfer immediately, strictly
          // before that one writeback statement) leaves An completely
          // unmodified -- never a partial decrement -- the same
          // unrecoverable-partial-effect fail-closed discipline
          // SEG-007-T065 already established for a single-EA
          // predecrement/postincrement destination.
          const auto reg_index = static_cast<unsigned>(ea.reg);
          const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg_index) + "]";
          const auto ea_local = "m68k_movem_ea_" + std::to_string(temp_ordinal++);
          body << "uint32_t " << ea_local << " = " << an_expr << ";\n";
          for (std::size_t slot = 0; slot < order.size(); ++slot) {
            body << ea_local << " -= UINT32_C(" << width << ");\n";
            emit_slot(ea_local, order[slot]);
          }
          body << an_expr << " = " << ea_local << ";\n";
        } else if (ea.mode == M68kEaMode::address_postinc) {
          // The read-side mirror of the predecrement branch immediately
          // above: the same one final architectural An writeback, deferred
          // until after the whole loop, gives the identical fail-closed
          // guarantee for a routed access that fails mid-transfer.
          const auto reg_index = static_cast<unsigned>(ea.reg);
          const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg_index) + "]";
          const auto ea_local = "m68k_movem_ea_" + std::to_string(temp_ordinal++);
          body << "uint32_t " << ea_local << " = " << an_expr << ";\n";
          const bool guarded_fold = have_resolved_reads && memory->movem_transfer_fold_base.has_value();
          std::string fold_local;
          if (guarded_fold) {
            fold_local = "m68k_movem_use_fold_" + std::to_string(temp_ordinal++);
            body << "const uint8_t " << fold_local << " = (uint8_t)(" << ea_local
                 << " == UINT32_C(0x" << hex(*memory->movem_transfer_fold_base, 8) << "));\n";
          }
          for (std::size_t slot = 0; slot < order.size(); ++slot) {
            // SEG-007-T075: the bounded adjacent LEA->MOVEM fold, the
            // postincrement mirror of the address_indirect branch above.
            // The architectural An postincrement writeback below is
            // unconditional either way (condition 8: a folded slot still
            // advances the working local by `width`, exactly like a routed
            // slot).
            const bool rom_folded = have_resolved_reads &&
                memory->movem_transfer_access[slot] == M68kOperandAccess::resolved_constant;
            if (rom_folded && guarded_fold) {
              body << "if (" << fold_local << ") {\n";
              emit_register_read(body, "UINT32_C(0x" + hex(memory->movem_transfer_values[slot], 8) + ")", order[slot]);
              body << "} else {\n";
              emit_slot(ea_local, order[slot]);
              body << "}\n";
            } else {
              emit_slot(ea_local, order[slot]);
            }
            body << ea_local << " += UINT32_C(" << width << ");\n";
          }
          body << an_expr << " = " << ea_local << ";\n";
        } else {
          ok = false;
        }
      } else if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long ||
          ea.mode == M68kEaMode::pc_disp16) {
        const auto base = (m68k_canonical_ea_address(ea) - memory->linear_memory_begin);
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
          const bool rom_folded = have_resolved_reads &&
              memory->movem_transfer_access[slot] == M68kOperandAccess::resolved_constant;
          if (rom_folded) {
            emit_register_read(body, "UINT32_C(0x" + hex(memory->movem_transfer_values[slot], 8) + ")", order[slot]);
            continue;
          }
          const auto offset_expr =
              "UINT32_C(" + std::to_string(base + static_cast<std::uint32_t>(slot) * width) + ")";
          emit_transfer(body, offset_expr, order[slot]);
        }
      } else if (ea.mode == M68kEaMode::address_indirect || ea.mode == M68kEaMode::address_disp16 ||
          ea.mode == M68kEaMode::address_index8 || ea.mode == M68kEaMode::pc_index8) {
        // SEG-021-T012: (d8,An,Xn)/(d8,PC,Xn) join this same branch as
        // (An)/d16(An) -- none of the four modes auto-update any register,
        // so all four share the identical one-time working-EA-snapshot
        // discipline below and the identical guarded per-slot runtime read/
        // write path. `pc_index8`'s base is a fixed (PC-relative-constant)
        // address plus a genuinely runtime index register, so a
        // memory->register MOVEM through it can never be an all-constant
        // ROM fold in general; it is intentionally routed through this
        // ordinary guarded path with no ROM-fold branch, exactly like plain
        // `address_indirect`/`address_disp16` whenever the T075 fold
        // precondition does not hold.
        //
        // SEG-007-T025 (Batch C, C5a review correction): ordinary (An)/
        // d16(An) MOVEM never auto-updates its EA register (contract:
        // "ordinary MOVEM forms do NOT auto-update their EA register"), so
        // the working transfer address is a snapshot of the ORIGINAL An,
        // computed exactly once before any transfer -- never re-derived
        // from the live `a[n]` array for a later slot. Without this
        // snapshot, a memory->register transfer that loads the EA's own
        // base register (e.g. `MOVEM.W (A0),A0-A1`) would corrupt every
        // subsequent transfer's address, since each occurrence of the
        // shared EA owner's textual `a[n]` expression re-reads whatever
        // that C variable holds at that point in the generated statement
        // sequence, not the architectural value at instruction start
        // (verified against pinned Musashi's own single `uint ea =
        // GET_EA(...)` local, computed once before its transfer loop and
        // never re-derived from REG_DA[]). `movem_base` is this one
        // snapshot; every transfer (register->memory and memory->register
        // alike, for consistency) derives its address from `movem_base +
        // slot * width`, never from the EA owner's expression a second
        // time. This is still the exact same shared
        // m68k_emit_runtime_ea_address owner every other selected
        // (An)/d16(An) form uses -- only its ONE result is now bound to a
        // local once, rather than re-embedded verbatim per transfer.
        const auto runtime =
            m68k_emit_runtime_ea_address(ea, operation.size, memory->address_registers, data_registers);
        body << runtime.prelude;
        unsigned temp_ordinal = 0U;
        const auto base_local = "m68k_movem_base_" + std::to_string(temp_ordinal++);
        // An empty register mask (section: "empty mask") performs zero
        // transfers, so `base_local` would otherwise be unused under
        // strict -Wunused-variable/-Werror; the explicit (void) cast is
        // harmless whether or not any transfer actually consumes it.
        body << "const uint32_t " << base_local << " = " << runtime.address_expr << "; (void)" << base_local
             << ";\n";
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
          const auto address_expr = slot == 0U ? base_local
                                                 : "(uint32_t)(" + base_local + " + UINT32_C(" +
                                                       std::to_string(static_cast<std::uint32_t>(slot) * width) + "))";
          const auto local = "m68k_movem_addr_" + std::to_string(temp_ordinal++);
          m68k_emit_runtime_ea_guard(body, local, address_expr, operation.size, *memory);
          const auto offset_expr = local + " - UINT32_C(0x" + hex(memory->linear_memory_begin, 8) + ")";
          emit_transfer(body, offset_expr, order[slot]);
        }
      } else if (ea.mode == M68kEaMode::address_predec) {
        // SEG-007-T025 (Batch C, C5b): predecrement's one working-EA
        // snapshot (contract: "predecrement working EA"). The
        // architectural base An is NEVER decremented in the live register
        // file inside the transfer loop -- doing so incrementally could
        // corrupt a later transfer's address and, if the base itself is a
        // selected register, the very value that transfer stores, exactly
        // the class of defect the C5a ordinary-form review correction
        // already fixed for the non-auto-update forms. `movem_ea` is one
        // internal working-EA local, snapshotted from the ORIGINAL
        // architectural An once; each transfer decrements it by `width`
        // BEFORE binding/guarding/writing to it (matching pinned Musashi's
        // base-MC68000 "re pd" handler: `ea -= width;
        // m68ki_write_16/32(ea, ...)`), and the architectural An itself
        // receives exactly one final writeback -- the fully-decremented
        // `movem_ea` -- after every transfer, never per-transfer (contract:
        // "one final architectural An writeback"). Because the live `a[n]`
        // array slot is therefore never assigned until that one final
        // writeback, a selected register that happens to equal the base An
        // is read for its STORE VALUE at its still-original architectural
        // value throughout the entire loop with no special case needed
        // (contract: "base-register alias semantics"; verified against a
        // dedicated pinned-Musashi vector). `order` above already carries
        // the shared M68kMovemTransferOrder::predecrement shape -- never a
        // second, independently reimplemented `15 - i` reversal here. Base
        // MC68000 MOVEM has no BYTE form, so there is no A7-step-by-2
        // exception here (unlike BCHG/BCLR/BSET's own -(A7)/(A7)+ forms):
        // width is always exactly 2 or 4, even when the base is A7.
        const auto reg = static_cast<unsigned>(ea.reg);
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        const auto ea_local = "m68k_movem_ea_" + std::to_string(temp_ordinal++);
        body << "uint32_t " << ea_local << " = " << an_expr << ";\n";
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
          body << ea_local << " -= UINT32_C(" << width << ");\n";
          const auto local = "m68k_movem_addr_" + std::to_string(temp_ordinal++);
          m68k_emit_runtime_ea_guard(body, local, ea_local, operation.size, *memory);
          const auto offset_expr = local + " - UINT32_C(0x" + hex(memory->linear_memory_begin, 8) + ")";
          emit_transfer(body, offset_expr, order[slot]);
        }
        // Unconditional, matching pinned Musashi's own unconditional
        // `AY = ea;` after the transfer loop: for an empty mask, `ea_local`
        // still equals the original An (never decremented), so this is a
        // harmless self-assignment -- exactly the oracle-observed
        // "final An unchanged" empty-mask fact, with no special case.
        body << an_expr << " = " << ea_local << ";\n";
      } else if (ea.mode == M68kEaMode::address_postinc) {
        // SEG-007-T025 (Batch C, C5c1): postincrement's one working-EA
        // snapshot, the read-side mirror of C5b's predecrement snapshot
        // (contract: "one working EA snapshot"). `movem_ea` is one internal
        // working-EA local, snapshotted from the ORIGINAL architectural An
        // once; each transfer reads at the CURRENT `movem_ea` and only THEN
        // increments it by `width` (matching pinned Musashi's base-MC68000
        // "er pi" handler: `REG_DA[i] = read(ea); ea += width;`), and the
        // architectural An itself receives exactly one final writeback --
        // the fully-incremented `movem_ea` -- after every transfer, never
        // per-transfer (contract: "base-register alias semantics"). `order`
        // above already carries the shared M68kMovemTransferOrder::ascending
        // shape -- postincrement never introduces a third register-order
        // interpretation. The actual invariant is that `movem_ea` is
        // snapshotted ONCE and every transfer address derives only from
        // that independent working-EA local, never again from the live
        // `a[n]` array -- NOT that `a[n]` itself is left unassigned during
        // the loop. The architectural base An MAY legitimately be assigned
        // by an ordinary selected-register load during the loop (if the
        // base itself is one of the loaded registers, it transiently
        // receives its OWN loaded, sign-extended-for-WORD, memory value,
        // exactly like any other selected register); that transient value
        // simply cannot affect any later transfer's address, because those
        // addresses are computed exclusively from `movem_ea`. The one final
        // postincrement writeback then unconditionally overwrites whatever
        // `a[n]` currently holds, matching pinned Musashi's own AY = ea
        // ordering (never special-cased by register number; verified
        // against a dedicated pinned-Musashi vector). Base MC68000 MOVEM
        // has no BYTE form, so there is no A7-step-by-2 exception here even
        // when the base is A7.
        const auto reg = static_cast<unsigned>(ea.reg);
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        const auto ea_local = "m68k_movem_ea_" + std::to_string(temp_ordinal++);
        body << "uint32_t " << ea_local << " = " << an_expr << ";\n";
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
          const auto local = "m68k_movem_addr_" + std::to_string(temp_ordinal++);
          m68k_emit_runtime_ea_guard(body, local, ea_local, operation.size, *memory);
          const auto offset_expr = local + " - UINT32_C(0x" + hex(memory->linear_memory_begin, 8) + ")";
          emit_transfer(body, offset_expr, order[slot]);
          body << ea_local << " += UINT32_C(" << width << ");\n";
        }
        // Unconditional, matching pinned Musashi's own unconditional
        // `AY = ea;` after the transfer loop: for an empty mask,
        // `ea_local` still equals the original An (never incremented), so
        // this is a harmless self-assignment -- exactly the
        // oracle-observed "final An unchanged" empty-mask fact, with no
        // special case.
        body << an_expr << " = " << ea_local << ";\n";
      } else {
        ok = false;
      }
      if (ok) {
        body << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
        output << body.str();
      }
    }
    break;
  }
  case M68kIrKind::shift_rotate_register: {
    // SEG-007-T025 (Batch C, C6): the emitter performs ONLY orchestration
    // (which count-source expression, which destination) -- the one shared
    // M68kShiftRotateSpecification::emit_c_update owner supplies the
    // complete count/width-region formula and SR update (never re-derived
    // here), and the destination write reuses the existing shared
    // sized-register-write primitive (m68k_emit_ea_write's data_register
    // branch) exactly like every other selected partial-width Dn write --
    // never a shift-specific partial-write formula. For a register-count
    // source, `count_expr` reads the count register directly as the FIRST
    // statement `emit_c_update` emits (contract: "source count must be
    // materialized first"), strictly before the destination write below,
    // so a `LSL D0,D0`-shaped alias (count source == destination) still
    // captures the pre-operation value correctly regardless of whether the
    // two registers happen to be the same one.
    if (memory != nullptr) {
      const auto dest_reg = static_cast<unsigned>(operation.destination_ea.reg);
      const auto dest_expr = std::string(data_registers) + "[" + std::to_string(dest_reg) + "]";
      std::string count_expr;
      if (operation.source_ea.mode == M68kEaMode::immediate) {
        count_expr = "UINT32_C(" + std::to_string(operation.source_ea.immediate_value) + ")";
      } else {
        const auto count_reg = static_cast<unsigned>(operation.source_ea.reg);
        count_expr =
            "(" + std::string(data_registers) + "[" + std::to_string(count_reg) + "] & UINT32_C(63))";
      }
      // Kept outside the semantic block so the instruction-boundary timing
      // seam reuses this exact materialized count after the write.
      output << "const uint32_t m68k_shift_effective_count = " << count_expr << ";\n";
      count_expr = "m68k_shift_effective_count";
      unsigned temp_ordinal = 0U;
      output << "{\n";
      const auto result_local = M68kShiftRotateSpecification::emit_c_update(
          output, operation.shift_rotate_kind, dest_expr, count_expr, operation.size, status_register,
          temp_ordinal);
      std::ostringstream write_prelude;
      const auto write = m68k_emit_ea_write(operation.destination_ea, operation.size, data_registers, *memory,
                                            result_local, write_prelude, temp_ordinal);
      if (write.ok) output << write_prelude.str() << write.expression << "\n";
      output << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
    }
    break;
  }
  case M68kIrKind::shift_rotate_memory: {
    // SEG-007-T025 (Batch C, C7a/C7b): reuses the SAME
    // M68kShiftRotateSpecification::emit_c_update owner every one of the 8
    // families (register or memory alike) uses (contract: "reuse C6 semantic
    // owner with count=1"; "no new semantics engine") -- count is
    // architecturally fixed to `UINT32_C(1)`, never re-derived, never a
    // memory-specific shift/rotate formula. This is the exact same
    // one-ARCHITECTURAL-address RMW template BTST/BCHG/BCLR/BSET already
    // established above: the destination's EA mutation side effect happens
    // exactly once (predecrement, in the read's own prelude, strictly before
    // either access; postincrement, deferred to the read's postlude, strictly
    // after the write), and the write is retargeted to `address_indirect` so
    // it never re-applies that mutation itself. For a runtime-relative EA
    // ((An)/(An)+/-(An)/d16(An)), the shared read/write helpers each derive
    // their own equivalent generated-C address local
    // (`m68k_ea_addr_N`/`m68k_ea_waddr_N`) from the identical EA formula --
    // two syntactically distinct locals evaluating to the SAME numeric
    // architectural address, since nothing mutates the address register
    // between them. This is not literally "one C local bound once"; it is
    // architecturally one address because no side effect can occur between
    // the two derivations. No new memory-access primitive is introduced
    // (contract: "shared memory/device routing" -- no
    // `shift_read16`/`shift_write16`).
    if (memory != nullptr) {
      // SEG-021-T009: in the routed context an auto-updating destination uses the operation-local deferred
      // address-register commit shared by NOT/NEG and the ADD family: one snapshot local, one routed read and
      // one routed write at that address, the single live-register commit strictly after both accesses (a
      // routed stop returns before any architectural write).
      if (memory->runtime_routing && (operation.destination_ea.mode == M68kEaMode::address_predec ||
                                      operation.destination_ea.mode == M68kEaMode::address_postinc)) {
        const auto reg = static_cast<unsigned>(operation.destination_ea.reg);
        const std::uint32_t step = reg == 7U && operation.size == M68kMemoryAccessWidth::byte
                                       ? 2U : static_cast<std::uint32_t>(operation.size);
        const auto an_expr = std::string(memory->address_registers) + "[" + std::to_string(reg) + "]";
        unsigned temp_ordinal = 0U;
        std::ostringstream body;
        body << "uint32_t m68k_shift_auto_ea = " << an_expr << ";\n";
        if (operation.destination_ea.mode == M68kEaMode::address_predec)
          body << "m68k_shift_auto_ea -= UINT32_C(" << step << ");\n";
        std::string shift_destination;
        m68k_emit_routed_read(body, "m68k_shift_auto_ea", operation.size, *memory, shift_destination, temp_ordinal);
        body << "{\n";
        const auto result_local = M68kShiftRotateSpecification::emit_c_update(
            body, operation.shift_rotate_kind, shift_destination, "UINT32_C(1)", operation.size, status_register,
            temp_ordinal);
        m68k_emit_routed_write(body, "m68k_shift_auto_ea", operation.size, *memory, result_local, temp_ordinal);
        body << "}\n";
        if (operation.destination_ea.mode == M68kEaMode::address_postinc)
          body << "m68k_shift_auto_ea += UINT32_C(" << step << ");\n";
        body << an_expr << " = m68k_shift_auto_ea;\n";
        output << "{\n" << body.str() << memory->program_counter << " += UINT32_C("
               << operation.provenance.length.value << ");\n}\n";
        break;
      }
      unsigned temp_ordinal = 0U;
      std::ostringstream prelude;
      const auto destination = m68k_emit_ea_read(operation.destination_ea, operation.size, data_registers, *memory,
                                                  prelude, temp_ordinal);
      if (destination.ok) {
        output << "{\n" << prelude.str();
        const auto result_local = M68kShiftRotateSpecification::emit_c_update(
            output, operation.shift_rotate_kind, destination.expression, "UINT32_C(1)", operation.size,
            status_register, temp_ordinal);
        auto write_ea = operation.destination_ea;
        if (write_ea.mode == M68kEaMode::address_predec || write_ea.mode == M68kEaMode::address_postinc)
          write_ea.mode = M68kEaMode::address_indirect;
        std::ostringstream write_prelude;
        const auto write = m68k_emit_ea_write(write_ea, operation.size, data_registers, *memory, result_local,
                                              write_prelude, temp_ordinal);
        if (write.ok) output << write_prelude.str() << write.expression << "\n";
        output << destination.postlude << "pc += UINT32_C(" << operation.provenance.length.value << ");\n}\n";
      }
    }
    break;
  }
  case M68kIrKind::bsr_call:
  case M68kIrKind::jump_general:
  case M68kIrKind::call_general: {
    // SEG-007-T025 (Batch C, C4b): BSR shares this exact push-continuation-
    // then-jump C-lowering with JSR (contract: "reuse the existing JSR
    // static call identity"), merged into the same case rather than a
    // second BSR-specific stack-write implementation. Only its target
    // computation differs upstream, in m68k_operation_effect (PC-relative
    // via m68k_branch_target, not an EA) -- this emitter only ever needs
    // the already-folded constant target.
    //
    // For JMP/JSR, only the statically-foldable target modes (absolute.w/
    // absolute.l/d16(PC)) ever reach emission with a usable direct_target;
    // discover_m68k_general_startup rejects a register-relative
    // ((An)/d16(An)) target before any static graph containing it is ever
    // built (decision: JSR scope). BSR's target is always foldable (a
    // relative displacement, never a runtime-only EA), so it always reaches
    // here with a usable direct_target.
    //
    // SEG-007-T124 / ADR-0009: the one exception -- a JSR whose decoded
    // source EA is the brief PC-relative indexed form. `m68k_operation_effect`
    // never folds this EA (it is genuinely not statically foldable), so this
    // branch computes the runtime EA from architectural registers itself,
    // checks it against the proven `memory->indirect_candidate_targets` set
    // C4 already validated for this exact source instruction, and only on
    // membership performs the existing call-stack write and PC assignment.
    // It fetches or decodes no target instruction and creates no second
    // dispatch mechanism: a membership success still only ever assigns
    // `pc`, letting the existing, unchanged dispatcher select the
    // already emitted candidate block on the next drive iteration.
    // SEG-007-T178 / ADR-0009 producer extension: `address_indirect` (mode 2,
    // no displacement/index/extension word) joins `pc_index8` as a lowered
    // computed control EA -- the runtime EA is the architectural `An` value
    // itself, with no base/index arithmetic.
    //
    // SEG-021-T034: the remaining register-relative control EAs -- `d16(An)`,
    // `(d8,An,Xn)` (every index bank/size) and the address-register/long
    // index variants of `(d8,PC,Xn)` -- join the same branch. Their runtime
    // EA comes from the existing shared `m68k_emit_runtime_ea_address`
    // helper (none of these modes has an auto-update prelude/postlude), so
    // no EA formula is duplicated; membership, JSR push and the fail-closed
    // stop are the unchanged code below. The two earlier shapes keep their
    // original, byte-identical EA text.
    const bool is_an_indirect_control = operation.source_ea.mode == M68kEaMode::address_indirect &&
                                        operation.source_ea.displacement == 0 &&
                                        operation.source_ea.extension_words == 0U;
    const bool is_word_dn_pc_index = operation.source_ea.mode == M68kEaMode::pc_index8 &&
                                     !operation.source_ea.index_is_address && !operation.source_ea.index_is_long;
    const bool is_helper_ea_control = operation.source_ea.mode == M68kEaMode::address_disp16 ||
                                      operation.source_ea.mode == M68kEaMode::address_index8 ||
                                      (operation.source_ea.mode == M68kEaMode::pc_index8 && !is_word_dn_pc_index);
    if ((operation.kind == M68kIrKind::call_general || operation.kind == M68kIrKind::jump_general) &&
        (is_word_dn_pc_index || is_an_indirect_control || is_helper_ea_control)) {
      const auto &ea = operation.source_ea;
      if (memory == nullptr || !memory->runtime_routing ||
          (memory->compiled_entry_lookup_symbol.empty() && memory->indirect_candidate_targets.empty())) {
        break;
      }
      const bool shared_lookup = !memory->compiled_entry_lookup_symbol.empty();
      const auto array_name = "m68k_indirect_targets_" + hex(operation.provenance.source.address.value, 8);
      if (!shared_lookup) {
        output << "static const uint32_t " << array_name << "[] = {";
        for (std::size_t index = 0; index < memory->indirect_candidate_targets.size(); ++index) {
          if (index != 0U) output << ", ";
          output << "UINT32_C(0x" << hex(memory->indirect_candidate_targets[index], 8) << ")";
        }
        output << "};\n";
      }
      const auto base = static_cast<std::uint32_t>(static_cast<std::int64_t>(ea.pc_base_address) +
                                                     static_cast<std::int64_t>(ea.displacement));
      const auto index_expr = std::string(data_registers) + "[" + std::to_string(static_cast<unsigned>(ea.index_reg)) + "]";
      if (is_an_indirect_control)
        output << "{ const uint32_t m68k_indirect_ea = " << memory->address_registers << "["
               << static_cast<unsigned>(ea.reg) << "];\n";
      else if (is_helper_ea_control) {
        const auto runtime = m68k_emit_runtime_ea_address(ea, M68kMemoryAccessWidth::long_word,
                                                          memory->address_registers, data_registers);
        if (runtime.address_expr.empty() || !runtime.prelude.empty() || !runtime.postlude.empty()) break;
        output << "{ const uint32_t m68k_indirect_ea = " << runtime.address_expr << ";\n";
      } else
        output << "{ const uint32_t m68k_indirect_ea = UINT32_C(0x" << hex(base, 8)
             << ") + (uint32_t)(int32_t)(int16_t)(uint16_t)(" << index_expr << ");\n";
      if (shared_lookup)
        output << "  if (" << memory->compiled_entry_lookup_symbol << "(m68k_indirect_ea) == NULL) "
               << emit_runtime(*memory).unresolved_indirect_stop(*memory);
      else
        output << "  if (!m68k_indirect_target_member(" << array_name << ", (uint32_t)(sizeof(" << array_name
               << ") / sizeof(" << array_name << "[0])), m68k_indirect_ea)) " << emit_runtime(*memory).unresolved_indirect_stop(*memory);
       if (operation.kind == M68kIrKind::jump_general) {
         output << "  " << memory->program_counter << " = m68k_indirect_ea;\n}\n";
         break;
       }
       const auto a7 = std::string(memory->address_registers) + "[7]";
      output << emit_runtime(*memory).call_push_guard(*memory, a7, true)
             << "      " << a7 << " = m68k_new_a7;\n"
             << "      " << memory->program_counter << " = m68k_indirect_ea;\n"
             << "    }\n"
             << "  }\n"
             << "}\n";
      break;
    }
    if (memory != nullptr) {
      const auto effect = m68k_operation_effect(operation);
      if (effect.pc == M68kPcEffectKind::direct_target) {
        if (operation.kind == M68kIrKind::call_general || operation.kind == M68kIrKind::bsr_call) {
          if (memory->runtime_routing) {
            const auto a7 = std::string(memory->address_registers) + "[7]";
            // Validate the architectural pre-decrement before forming it.
            // In particular, an invalid A7 must not wrap in a C initializer
            // before the generated code has selected its fail-closed stop.
            output << emit_runtime(*memory).call_push_guard(*memory, a7, false)
                   << a7 << " = m68k_new_a7; " << memory->program_counter << " = UINT32_C(0x" << hex(effect.direct_target, 8) << "); } }\n";
            break;
          }
          const auto a7 = std::string(memory->address_registers) + "[7]";
          const auto stack_low = hex(memory->linear_memory_begin + 4U, 8);
          const auto stack_high = hex(memory->linear_memory_end, 8);
          const auto ram_begin_hex = hex(memory->linear_memory_begin, 8);
          output << "if ((" << a7 << " & 1U) != 0U || " << a7 << " < UINT32_C(0x" << stack_low << ") || " << a7
                 << " > UINT32_C(0x" << stack_high << ") || " << memory->frame_depth << " != 0U) { return 1; }\n"
                 << a7 << " -= 4U; " << memory->ram_array << "[" << a7 << "-UINT32_C(0x" << ram_begin_hex
                 << ")] = (uint8_t)(UINT32_C(0x" << hex(memory->continuation, 8) << ") >> 24U); " << memory->ram_array
                 << "[" << a7 << "-UINT32_C(0x" << ram_begin_hex << ")+1U] = (uint8_t)(UINT32_C(0x"
                 << hex(memory->continuation, 8) << ") >> 16U); " << memory->ram_array << "[" << a7
                 << "-UINT32_C(0x" << ram_begin_hex << ")+2U] = (uint8_t)(UINT32_C(0x" << hex(memory->continuation, 8)
                 << ") >> 8U); " << memory->ram_array << "[" << a7 << "-UINT32_C(0x" << ram_begin_hex
                 << ")+3U] = (uint8_t)UINT32_C(0x" << hex(memory->continuation, 8) << "); " << memory->frame_ids_array
                 << "[" << memory->frame_depth << "] = 0U; " << memory->frame_continuations_array << "["
                 << memory->frame_depth << "++] = UINT32_C(0x" << hex(memory->continuation, 8) << "); ";
        }
        output << "pc = UINT32_C(0x" << hex(effect.direct_target, 8) << ");\n";
      }
    }
    break;
  }
  case M68kIrKind::trap_exception:
  case M68kIrKind::trap_on_overflow:
  case M68kIrKind::instruction_exception: {
    // SEG-021-T019 / ADR 0043 §3: TRAP #n always raises vector 32 + n and TRAPV raises vector 7 when V = 1, both with
    // the NEXT instruction stacked; ILLEGAL, line 1010/1111 and every other illegal word raise vector 4/10/11 with
    // THIS instruction stacked. Nothing else of the instruction executes (TRAPV with V = 0 only advances the PC).
    // The vector is a build-time fact of the decoded word; no opcode is inspected at runtime.
    if (memory != nullptr && !memory->user_stack_pointer.empty() && operation.exception_vector != 0U) {
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      const auto address = operation.provenance.source.address.value;
      const auto next_pc = address + operation.provenance.length.value;
      const bool current = operation.kind == M68kIrKind::instruction_exception;
      const auto entry = m68k_exception_entry(operation.exception_vector, current ? address : next_pc,
                                              status_register, *memory);
      if (operation.kind == M68kIrKind::trap_on_overflow)
        output << "if ((" << status_register << " & UINT16_C(0x0002)) != 0U) " << entry << "else { " << program_counter
               << " += UINT32_C(" << operation.provenance.length.value << "); }\n";
      else
        output << entry;
    }
    break;
  }
  case M68kIrKind::check_bounds: {
    // SEG-021-T019 / ADR 0043 §3: CHK.W <ea>,Dn. The word bound is read first (an (An)+/-(An) bound commits its
    // address-register update before any exception, as on the MC68000; the routed read uses the operation-local
    // deferred commit, so a failed read changes nothing). Condition codes follow the pinned Musashi core, which is
    // consistent with the Motorola manual where it is defined: Z <- (Dn.W == 0), V <- 0, C <- 0 (all three are
    // "undefined" in the manual), X unchanged; N is changed only when the exception is taken -- set when
    // Dn.W < 0, cleared when Dn.W > bound (the manual's defined cases). Out of range raises vector 6 with the NEXT
    // instruction stacked (the saved SR carries the new flags); in range only advances the PC.
    if (memory != nullptr && !memory->user_stack_pointer.empty() &&
        operation.destination_ea.mode == M68kEaMode::data_register && operation.destination_ea.reg < 8U) {
      std::ostringstream body;
      const auto source = m68k_emit_status_source(operation, data_registers, *memory, body);
      if (source.ok) {
        const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
        const auto next_pc = operation.provenance.source.address.value + operation.provenance.length.value;
        const auto dn = std::string(data_registers) + "[" +
                        std::to_string(static_cast<unsigned>(operation.destination_ea.reg)) + "]";
        output << "{\n" << body.str()
               << "const int32_t m68k_chk_bound = (int32_t)(int16_t)(uint16_t)(" << source.value << ");\n"
               << source.commit
               << "{ const int32_t m68k_chk_value = (int32_t)(int16_t)(uint16_t)(" << dn << ");\n"
               << status_register << " = (uint16_t)((" << status_register
               << " & UINT16_C(0xFFF8)) | (m68k_chk_value == 0 ? UINT16_C(0x0004) : UINT16_C(0)));\n"
               << "if (m68k_chk_value < 0 || m68k_chk_value > m68k_chk_bound) { " << status_register << " = (uint16_t)(("
               << status_register << " & UINT16_C(0xFFF7)) | (m68k_chk_value < 0 ? UINT16_C(0x0008) : UINT16_C(0)));\n"
               << m68k_exception_entry(operation.exception_vector, next_pc, status_register, *memory)
               << "} else { " << program_counter << " += UINT32_C(" << operation.provenance.length.value
               << "); } } }\n";
      }
    }
    break;
  }
  case M68kIrKind::return_restore_condition_codes: {
    // SEG-021-T019 / ADR 0043 §5: RTR (unprivileged). The CCR word at SP and the PC long at SP+2 are read, then
    // committed together: CCR <- read & 0x1F (the system byte is unchanged), PC <- read PC, SP += 6. Routed: the
    // M68K-owned frame-return core bound by the platform (validated routed reads; a failed read commits nothing);
    // the restored PC continues through the ordinary dispatcher exactly like RTE's. Direct: the same rule inline.
    if (memory != nullptr && memory->runtime_routing) {
      output << emit_runtime(*memory).condition_code_return(*memory) << memory->program_counter
             << " = m68k_rtr_pc; }\n";
    } else if (memory != nullptr && !memory->user_stack_pointer.empty()) {
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      const auto a7 = std::string(memory->address_registers) + "[7]";
      const auto at = [&](unsigned index) {
        return "(uint32_t)" + std::string(memory->ram_array) + "[m68k_rtr_sp - UINT32_C(0x" +
               hex(memory->linear_memory_begin, 8) + ") + " + std::to_string(index) + "U]";
      };
      output << "{ const uint32_t m68k_rtr_sp = " << a7 << ";\n"
             << "if ((m68k_rtr_sp & 1U) != 0U || m68k_rtr_sp < UINT32_C(0x" << hex(memory->linear_memory_begin, 8)
             << ") || m68k_rtr_sp > UINT32_C(0x" << hex(memory->linear_memory_end, 8) << ") - UINT32_C(6)) { return 1; }\n"
             << "{ const uint32_t m68k_rtr_ccr = " << at(1U) << " & UINT32_C(0x1F);\n"
             << "const uint32_t m68k_rtr_pc = (" << at(2U) << " << 24U) | (" << at(3U) << " << 16U) | (" << at(4U)
             << " << 8U) | " << at(5U) << ";\n"
             << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFF00)) | m68k_rtr_ccr);\n"
             << a7 << " = m68k_rtr_sp + UINT32_C(6);\n"
             << program_counter << " = m68k_rtr_pc; } }\n";
    }
    break;
  }
  case M68kIrKind::return_from_exception: {
    // SEG-007-T047 / ADR-0020 §9, SEG-021-T018 / ADR 0043 §5: RTE. Privileged: in user mode vector 8 is raised
    // with this instruction's own address before the frame is touched. Routed: the validated atomic {SR, PC,
    // SSP, inactive SP} restore is the M68K-owned exception core bound by the platform's exception-return
    // routine (no return-target membership check -- the restored PC is a generated-runtime fact popped from the
    // exception frame -- continuing through the block tail's continue-at-PC on runtime->pc). Direct linear
    // route: the same rule inline against the window. A failed frame read or a restored T = 1 fails closed
    // with nothing restored.
    if (memory != nullptr && memory->runtime_routing) {
      std::string closing;
      output << m68k_privilege_guard(operation, status_register, *memory, closing)
             << emit_runtime(*memory).exception_return(*memory) << memory->program_counter << " = m68k_rte_pc; }\n"
             << closing;
    } else if (memory != nullptr && !memory->user_stack_pointer.empty()) {
      const std::string_view program_counter = memory->program_counter.empty() ? "pc" : memory->program_counter;
      const auto a7 = std::string(memory->address_registers) + "[7]";
      const auto at = [&](unsigned index) {
        return "(uint32_t)" + std::string(memory->ram_array) + "[m68k_rte_sp - UINT32_C(0x" +
               hex(memory->linear_memory_begin, 8) + ") + " + std::to_string(index) + "U]";
      };
      std::string closing;
      output << m68k_privilege_guard(operation, status_register, *memory, closing)
             << "{ const uint32_t m68k_rte_sp = " << a7 << ";\n"
             << "if ((m68k_rte_sp & 1U) != 0U || m68k_rte_sp < UINT32_C(0x" << hex(memory->linear_memory_begin, 8)
             << ") || m68k_rte_sp > UINT32_C(0x" << hex(memory->linear_memory_end, 8) << ") - UINT32_C(6)) { return 1; }\n"
             << "{ const uint16_t m68k_rte_sr = (uint16_t)(((" << at(0U) << " << 8U) | " << at(1U)
             << ") & UINT32_C(0xA71F));\n"
             << "const uint32_t m68k_rte_pc = (" << at(2U) << " << 24U) | (" << at(3U) << " << 16U) | (" << at(4U)
             << " << 8U) | " << at(5U) << ";\n"
             << "if ((m68k_rte_sr & UINT16_C(0x8000)) != 0U) { return 1; }\n"
             << "if ((m68k_rte_sr & UINT16_C(0x2000)) != 0U) { " << a7 << " = m68k_rte_sp + UINT32_C(6); } else { "
             << a7 << " = " << memory->user_stack_pointer << "; " << memory->user_stack_pointer
             << " = m68k_rte_sp + UINT32_C(6); }\n"
             << status_register << " = m68k_rte_sr;\n"
             << program_counter << " = m68k_rte_pc; } }\n" << closing;
    }
    break;
  }
  }
  return output.str();
}

bool m68k_operation_has_complete_c_emission(const M68kIrOperation &operation) {
  M68kMemoryEmissionContext probe_memory{
      "ram", "a", "frame_ids", "frame_continuations", "frame_depth", 0U,
      M68kOperandAccess::resolved_constant, 0U, {}, {}};
  probe_memory.user_stack_pointer = "usp";
  return m68k_operation_effect(operation).pc != M68kPcEffectKind::none &&
         !emit_m68k_operation_c(operation, "d", "sr", {}, &probe_memory).empty();
}

} // namespace segarecomp
