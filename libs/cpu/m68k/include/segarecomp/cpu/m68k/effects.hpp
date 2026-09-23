#pragma once

// SEG-014-T002: MC68000 instruction effects / semantic-operation helpers
// (docs/architecture/post-seg007-architecture-refactor-contract.md, section
// 8.2 "cpu/m68k/"). Relocated from include/segarecomp/m68k_pipeline.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `cpu/m68k/`
// section ("Semantic-operation helpers"). No behavior changes.
//
// The `*Specification` structs below are the one shared implementation each
// public function/`emit_m68k_operation_c` (codegen, still owned by
// m68k_pipeline.cpp -- out of this task's scope) delegates to for both host
// semantics and generated-C11 lowering, so the two can never independently
// drift. They are declared here (rather than kept file-local, as they were
// before this physical module split) because codegen -- a separate
// translation unit after this split -- must keep calling their
// `emit_c_update` members exactly as it already did; this is a mechanical
// consequence of drawing a real build boundary, not new architecture or a
// widened public contract.

#include "segarecomp/cpu/m68k/ir.hpp"

#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp {

// The instruction-level rule for X after subtraction-style N/Z/V/C arithmetic.
// `from_carry` is retained for a future selected subtraction instruction; it
// does not select or implement such an instruction by itself.
enum class M68kExtendFlagPolicy { preserve, from_carry };

// The shared, sized subtraction arithmetic fact. It deliberately contains no
// instruction-level X policy: callers apply X separately while using these
// result/N/Z/V/C facts for CMP or a future selected subtraction form.
struct M68kSubtractionResult {
  std::uint32_t result{};
  bool negative{};
  bool zero{};
  bool overflow{};
  bool carry{};  // Borrow for destination - source.
};
// Sized addition result and N/Z/V/C facts. `carry` is the unsigned carry out
// of destination + source; ordinary ADD-family operations also copy it to X.
struct M68kAdditionResult {
  std::uint32_t result{};
  bool negative{};
  bool zero{};
  bool overflow{};
  bool carry{};
};
// Sized logical result. Logical operations preserve X and all upper SR bits,
// set N/Z from the operand width, and clear V/C.
struct M68kLogicalResult { std::uint32_t result{}; bool negative{}; bool zero{}; };

// SEG-007-T025 (Batch C, C3): the one shared semantic owner for
// BTST/BCHG/BCLR/BSET. Given the selected mnemonic, the destination's
// ORIGINAL (pre-modification) value, the raw (not yet masked) bit number,
// and the destination's operation width (long_word for a Dn destination,
// byte for a memory destination -- bit operations have no word-destination
// form), this derives the effective bit index (`bit_number & 31` for
// long_word, `bit_number & 7` for byte -- modulo behavior is truthful base
// MC68000 semantics, never a rejection) and the post-operation result.
// `test`'s result always equals `original`: BTST is read-only and the
// caller must never perform a destination write for it. The condition-code
// consequence (Z only; see m68k_bit_test_ccr below) is always derived from
// `original_bit_set`, i.e. the ORIGINAL tested bit, before any modification
// -- identical for all four selected mnemonics, which differ only in their
// destination result.
enum class M68kBitOperationKind { test, change, clear, set };
struct M68kBitOperationResult { std::uint32_t result{}; bool original_bit_set{}; };

// SEG-007-T025 (Batch C, C4c): DBcc's shared low-word decrement fact,
// preserving the upper 16 bits of Dn -- used identically by host semantics
// (unit testing) and generated-C lowering, so the boundary (0x0001 -> 0x0000
// branches; 0x0000 -> 0xFFFF falls through) is expressed exactly once.
// `expired` is true iff the POST-decrement low word equals 0xFFFF.
struct M68kDbccDecrementResult { std::uint32_t updated_register{}; bool expired{}; };

// SEG-007-T025 (Batch C, C5a/C5b): MOVEM's transfer-order shape selector.
// `ascending` is the ordinary (C5a) and postincrement (C5c) shape: register
// index == mask bit index, D0-D7 then A0-A7. `predecrement` is C5b's
// reversed shape: raw bit i selects architectural register (15 - i).
enum class M68kMovemTransferOrder { ascending, predecrement };

// `count` is the ALREADY-RESOLVED effective count: for an immediate source,
// the decoded 1-8 value; for a register source, the CALLER'S already-
// materialized `Dn & 0x3F` value (0-63). `original` is the current
// (pre-operation) destination value; only its low `width`-sized bits are
// meaningful. `original_extend` is the CURRENT X flag bit, consulted by
// every family. `extend_written` distinguishes families whose X flag
// participates (LSL/LSR/ASL/ASR/ROXL/ROXR) from ROL/ROR, which NEVER
// modifies X.
struct M68kShiftRotateResult {
  std::uint32_t result{};
  bool negative{};
  bool zero{};
  bool overflow{};
  bool carry{};
  bool extend_written{};
  bool extend_value{};
};

namespace m68k_effects_detail {
// Local formatting helper for the Specification structs' emit_c_update
// members below. `inline` so this header can be included by more than one
// translation unit without violating ODR.
[[nodiscard]] inline std::string m68k_hex_literal(std::uint32_t value, unsigned width) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
  return out.str();
}
} // namespace m68k_effects_detail
using m68k_effects_detail::m68k_hex_literal;

// The one code-level definition of the Z/N-only CCR update shared by host
// execution and generated-C lowering.  The emitted form deliberately remains
// C11-only; it cannot call the host helper at generated-program runtime.
struct M68kMoveResultCcrSpecification {
  static constexpr std::uint16_t preserved_mask = UINT16_C(0xFFF0);
  static constexpr std::uint16_t negative_mask = UINT16_C(0x0008);
  static constexpr std::uint16_t zero_mask = UINT16_C(0x0004);
  static constexpr std::uint32_t result_negative_bit = UINT32_C(0x80000000);

  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, std::uint32_t result) noexcept {
    return static_cast<std::uint16_t>((status_register & preserved_mask) |
                                      (result == 0U ? zero_mask : 0U) |
                                      ((result & result_negative_bit) != 0U ? negative_mask : 0U));
  }

  static void emit_c_update(std::ostringstream &output, std::string_view status_register,
                            std::string_view result) {
    output << status_register << " = (uint16_t)((" << status_register
           << " & UINT16_C(0x" << m68k_hex_literal(preserved_mask, 4) << ")) | (" << result
           << " == 0U ? " << zero_mask << "U : ((" << result
           << " & UINT32_C(0x" << m68k_hex_literal(result_negative_bit, 8) << ")) != 0U ? " << negative_mask << "U : 0U)));";
  }
};

struct M68kSubtractionResultSpecification {
  [[nodiscard]] static M68kSubtractionResult evaluate(std::uint32_t source,
                                                       std::uint32_t destination,
                                                       M68kMemoryAccessWidth width) noexcept {
    const auto bits = width == M68kMemoryAccessWidth::byte ? 8U :
                      width == M68kMemoryAccessWidth::word ? 16U : 32U;
    const auto mask = bits == 32U ? UINT32_MAX : (UINT32_C(1) << bits) - 1U;
    source &= mask;
    destination &= mask;
    const auto result = (destination - source) & mask;
    const auto sign = UINT32_C(1) << (bits - 1U);
    return {result, (result & sign) != 0U, result == 0U,
            ((destination ^ source) & (destination ^ result) & sign) != 0U,
            source > destination};
  }
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, std::uint32_t source,
                                            std::uint32_t destination, M68kMemoryAccessWidth width,
                                            M68kExtendFlagPolicy extend_flag_policy) noexcept {
    const auto subtraction = evaluate(source, destination, width);
    const auto extend = extend_flag_policy == M68kExtendFlagPolicy::preserve
                            ? (status_register & UINT16_C(0x0010))
                            : (subtraction.carry ? UINT16_C(0x0010) : UINT16_C(0));
    const auto updated = static_cast<std::uint32_t>(status_register & UINT16_C(0xFFE0)) |
        static_cast<std::uint32_t>(extend) |
        (subtraction.negative ? UINT32_C(0x0008) : 0U) |
        (subtraction.zero ? UINT32_C(0x0004) : 0U) |
        (subtraction.overflow ? UINT32_C(0x0002) : 0U) |
        (subtraction.carry ? UINT32_C(0x0001) : 0U);
    return static_cast<std::uint16_t>(updated);
  }
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            const std::string &source, const std::string &destination,
                            M68kMemoryAccessWidth size, M68kExtendFlagPolicy extend_flag_policy) {
    const auto bits = size == M68kMemoryAccessWidth::byte ? 8U : size == M68kMemoryAccessWidth::word ? 16U : 32U;
    const auto mask = bits == 32U ? "UINT32_C(0xFFFFFFFF)" : bits == 16U ? "UINT32_C(0x0000FFFF)" : "UINT32_C(0x000000FF)";
    const auto sign = bits == 32U ? "UINT32_C(0x80000000)" : bits == 16U ? "UINT32_C(0x00008000)" : "UINT32_C(0x00000080)";
    out << "{ const uint32_t compare_source = (" << source << ") & " << mask
        << "; const uint32_t compare_destination = (" << destination << ") & " << mask
         << "; const uint32_t compare_result = (compare_destination - compare_source) & " << mask << "; "
         << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFE0)) | "
         << (extend_flag_policy == M68kExtendFlagPolicy::from_carry
                 ? "(compare_source > compare_destination ? UINT16_C(0x0010) : UINT16_C(0))"
                 : "(" + std::string(status_register) + " & UINT16_C(0x0010))") << " | "
        << "((compare_result & " << sign << ") != 0U ? UINT16_C(8) : 0U) | "
        << "(compare_result == 0U ? UINT16_C(4) : 0U) | "
        << "(((compare_destination ^ compare_source) & (compare_destination ^ compare_result) & " << sign
        << ") != 0U ? UINT16_C(2) : 0U) | (compare_source > compare_destination ? UINT16_C(1) : 0U)); }\n";
  }
};

struct M68kAdditionResultSpecification {
  [[nodiscard]] static constexpr std::uint32_t mask(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x000000FF) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x0000FFFF) : UINT32_MAX;
  }
  [[nodiscard]] static constexpr std::uint32_t sign(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x00000080) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x00008000) : UINT32_C(0x80000000);
  }
  [[nodiscard]] static M68kAdditionResult evaluate(std::uint32_t source, std::uint32_t destination,
                                                    M68kMemoryAccessWidth width) noexcept {
    const auto m = mask(width);
    source &= m;
    destination &= m;
    const auto result = (destination + source) & m;
    const auto s = sign(width);
    const auto sum = static_cast<std::uint64_t>(destination) + source;
    return {result, (result & s) != 0U, result == 0U,
            ((~(destination ^ source) & (destination ^ result) & s) != 0U),
            sum > m};
  }
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, std::uint32_t source,
                                            std::uint32_t destination, M68kMemoryAccessWidth width) noexcept {
    const auto addition = evaluate(source, destination, width);
    return static_cast<std::uint16_t>((status_register & UINT16_C(0xFFE0)) |
        (addition.carry ? UINT16_C(0x0011) : UINT16_C(0)) |
        (addition.negative ? UINT16_C(0x0008) : UINT16_C(0)) |
        (addition.zero ? UINT16_C(0x0004) : UINT16_C(0)) |
        (addition.overflow ? UINT16_C(0x0002) : UINT16_C(0)));
  }
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            const std::string &source, const std::string &destination,
                            M68kMemoryAccessWidth size) {
    const auto mask_literal = "UINT32_C(0x" + m68k_hex_literal(mask(size), 8) + ")";
    const auto sign_literal = "UINT32_C(0x" + m68k_hex_literal(sign(size), 8) + ")";
    // Names are deliberately distinct from the enclosing operation's
    // materialized add_source/add_destination locals.
    out << "{ const uint32_t add_ccr_source = (" << source << ") & " << mask_literal
        << "; const uint32_t add_ccr_destination = (" << destination << ") & " << mask_literal
        << "; const uint32_t add_ccr_result = (add_ccr_destination + add_ccr_source) & " << mask_literal
        << "; const uint64_t add_ccr_sum = (uint64_t)add_ccr_destination + add_ccr_source; "
        << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFE0)) | "
        << "(add_ccr_sum > " << mask_literal << " ? UINT16_C(0x0011) : UINT16_C(0)) | "
        << "((add_ccr_result & " << sign_literal << ") != 0U ? UINT16_C(8) : 0U) | "
        << "(add_ccr_result == 0U ? UINT16_C(4) : 0U) | "
        << "((~(add_ccr_destination ^ add_ccr_source) & (add_ccr_destination ^ add_ccr_result) & " << sign_literal
        << ") != 0U ? UINT16_C(2) : 0U)); }\n";
  }
};

// SEG-021-T014: the one shared owner of ADDX/SUBX/NEGX arithmetic and condition codes, for host
// semantics and generated C. Operand roles: ADDX result = destination + source + X; SUBX result =
// destination - source - X; NEGX is SUBX with destination 0 and source = the operand. From the Motorola
// M68000 Family Programmer's Reference Manual: X and C are set from the carry/borrow out of the
// operation, N from the result's most significant bit, V on signed overflow, and Z is CLEARED when the
// result is non-zero but otherwise UNCHANGED (sticky), so a multi-precision chain keeps the Z of the
// whole value. The pre-operation X is an input operand.
enum class M68kExtendedArithmeticKind { add, subtract };
struct M68kExtendedArithmeticResult {
  std::uint32_t result{};
  bool negative{};
  bool overflow{};
  bool carry{};
};
struct M68kExtendedArithmeticSpecification {
  [[nodiscard]] static constexpr std::uint32_t mask(M68kMemoryAccessWidth width) noexcept {
    return M68kAdditionResultSpecification::mask(width);
  }
  [[nodiscard]] static constexpr std::uint32_t sign(M68kMemoryAccessWidth width) noexcept {
    return M68kAdditionResultSpecification::sign(width);
  }
  [[nodiscard]] static M68kExtendedArithmeticResult evaluate(M68kExtendedArithmeticKind kind, std::uint32_t source,
                                                              std::uint32_t destination, bool extend,
                                                              M68kMemoryAccessWidth width) noexcept {
    const auto m = mask(width);
    const auto s = sign(width);
    source &= m;
    destination &= m;
    const std::uint32_t x = extend ? 1U : 0U;
    const auto result = (kind == M68kExtendedArithmeticKind::add ? destination + source + x
                                                                  : destination - source - x) & m;
    const bool carry = kind == M68kExtendedArithmeticKind::add
        ? (((source & destination) | (~result & destination) | (source & ~result)) & s) != 0U
        : (((source & ~destination) | (result & ~destination) | (source & result)) & s) != 0U;
    const bool overflow = kind == M68kExtendedArithmeticKind::add
        ? (((source & destination & ~result) | (~source & ~destination & result)) & s) != 0U
        : (((~source & destination & ~result) | (source & ~destination & result)) & s) != 0U;
    return {result, (result & s) != 0U, overflow, carry};
  }
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, M68kExtendedArithmeticKind kind,
                                            std::uint32_t source, std::uint32_t destination,
                                            M68kMemoryAccessWidth width) noexcept {
    const auto r = evaluate(kind, source, destination, (status_register & UINT16_C(0x0010)) != 0U, width);
    const bool zero = r.result == 0U && (status_register & UINT16_C(0x0004)) != 0U;
    return static_cast<std::uint16_t>((status_register & UINT16_C(0xFFE0)) | (r.carry ? UINT16_C(0x0011) : 0U) |
                                      (r.negative ? UINT16_C(0x0008) : 0U) | (zero ? UINT16_C(0x0004) : 0U) |
                                      (r.overflow ? UINT16_C(0x0002) : 0U));
  }
  // Emits locals `xa_source`, `xa_destination`, `xa_extend` (pre-operation X) and `xa_result`.
  static void emit_c_compute(std::ostringstream &out, std::string_view status_register,
                             M68kExtendedArithmeticKind kind, const std::string &source,
                             const std::string &destination, M68kMemoryAccessWidth size) {
    const auto m = "UINT32_C(0x" + m68k_hex_literal(mask(size), 8) + ")";
    out << "const uint32_t xa_source = (" << source << ") & " << m << "; const uint32_t xa_destination = ("
        << destination << ") & " << m << "; const uint32_t xa_extend = ((uint32_t)" << status_register
        << " >> 4U) & 1U; const uint32_t xa_result = (xa_destination "
        << (kind == M68kExtendedArithmeticKind::add ? "+ xa_source + xa_extend" : "- xa_source - xa_extend")
        << ") & " << m << "; ";
  }
  // Requires the locals from emit_c_compute in scope.
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            M68kExtendedArithmeticKind kind, M68kMemoryAccessWidth size) {
    const auto sg = "UINT32_C(0x" + m68k_hex_literal(sign(size), 8) + ")";
    const bool add = kind == M68kExtendedArithmeticKind::add;
    out << "{ const uint32_t xa_carry = " << (add ? "((xa_source & xa_destination) | (~xa_result & xa_destination) | "
                                                    "(xa_source & ~xa_result))"
                                                  : "((xa_source & ~xa_destination) | (xa_result & ~xa_destination) | "
                                                    "(xa_source & xa_result))")
        << " & " << sg << "; const uint32_t xa_overflow = "
        << (add ? "((xa_source & xa_destination & ~xa_result) | (~xa_source & ~xa_destination & xa_result))"
                : "((~xa_source & xa_destination & ~xa_result) | (xa_source & ~xa_destination & xa_result))")
        << " & " << sg << "; " << status_register << " = (uint16_t)((" << status_register
        << " & UINT16_C(0xFFE0)) | (xa_carry != 0U ? UINT16_C(0x0011) : UINT16_C(0)) | ((xa_result & " << sg
        << ") != 0U ? UINT16_C(8) : UINT16_C(0)) | ((xa_result == 0U && (" << status_register
        << " & UINT16_C(4)) != 0U) ? UINT16_C(4) : UINT16_C(0)) | (xa_overflow != 0U ? UINT16_C(2) : UINT16_C(0))); }\n";
  }
};

// One logical-operation specification supplies the sized result, SR update,
// and C11 lowering.  Generated C cannot call the host implementation, so the
// emitter deliberately projects these same constants and operations instead
// of maintaining a second hand-written flag rule at each instruction family.
struct M68kLogicalResultSpecification {
  [[nodiscard]] static constexpr std::uint32_t mask(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x000000FF) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x0000FFFF) : UINT32_MAX;
  }
  [[nodiscard]] static constexpr std::uint32_t sign(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x00000080) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x00008000) : UINT32_C(0x80000000);
  }
  [[nodiscard]] static M68kLogicalResult evaluate(std::uint32_t result,
                                                   M68kMemoryAccessWidth width) noexcept {
    const auto sized = result & mask(width);
    return {sized, (sized & sign(width)) != 0U, sized == 0U};
  }
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, std::uint32_t result,
                                            M68kMemoryAccessWidth width) noexcept {
    const auto logical = evaluate(result, width);
    return static_cast<std::uint16_t>((status_register & UINT16_C(0xFFF0)) |
        (logical.negative ? UINT16_C(8) : 0U) | (logical.zero ? UINT16_C(4) : 0U));
  }
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            std::string_view result, M68kMemoryAccessWidth width) {
    out << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFF0)) | "
        << "(((" << result << " & UINT32_C(0x" << m68k_hex_literal(mask(width), 8) << ")) & UINT32_C(0x"
        << m68k_hex_literal(sign(width), 8) << ")) != 0U ? UINT16_C(8) : UINT16_C(0)) | (("
        << result << " & UINT32_C(0x" << m68k_hex_literal(mask(width), 8)
        << ")) == 0U ? UINT16_C(4) : UINT16_C(0)));";
  }
};

// SEG-007-T222 / ADR-0037: the one shared semantic owner for DIVS.W/DIVU.W's
// word-divide result -- signedness is an explicit `bool is_signed` parameter,
// never a duplicated implementation. `divisor` is the already-materialized
// 16-bit source operand; `dividend` is Dn's full 32-bit value. Divide-by-zero
// is NOT handled here -- the caller (generated C11 emission) checks
// `divisor == 0` and raises the vector-5 synchronous exception (ADR-0037)
// BEFORE ever calling this specification; `evaluate`/`emit_c_update` assume a
// non-zero divisor. On quotient overflow (does not fit the signed/unsigned
// 16-bit destination range) `result` equals the ORIGINAL `dividend`
// unchanged (no write), matching the architectural "Dn completely unchanged"
// rule; `negative`/`zero` are a documented deterministic PROJECT CHOICE for
// this architecturally-undefined case (both false), never asserted as
// MC68000 architectural truth.
struct M68kDivisionResult {
  std::uint32_t result{};    // packed {remainder:16, quotient:16} or unchanged dividend on overflow
  bool overflow{};
  bool negative{};
  bool zero{};
};
struct M68kDivisionResultSpecification {
  [[nodiscard]] static M68kDivisionResult evaluate(std::uint32_t dividend, std::uint16_t divisor,
                                                    bool is_signed) noexcept {
    if (is_signed) {
      const auto dividend_s = static_cast<std::int32_t>(dividend);
      const auto divisor_s = static_cast<std::int32_t>(static_cast<std::int16_t>(divisor));
      const std::int64_t quotient = static_cast<std::int64_t>(dividend_s) / divisor_s;
      const std::int64_t remainder = static_cast<std::int64_t>(dividend_s) % divisor_s;
      if (quotient < -32768 || quotient > 32767) return {dividend, true, false, false};
      const auto q16 = static_cast<std::uint16_t>(static_cast<std::int16_t>(quotient));
      const auto r16 = static_cast<std::uint16_t>(static_cast<std::int16_t>(remainder));
      const auto packed = static_cast<std::uint32_t>((static_cast<std::uint32_t>(r16) << 16U) | q16);
      return {packed, false, quotient < 0, quotient == 0};
    }
    const std::uint32_t divisor_u = divisor;
    const std::uint32_t quotient = dividend / divisor_u;
    const std::uint32_t remainder = dividend % divisor_u;
    if (quotient > 0xFFFFU) return {dividend, true, false, false};
    const auto packed = (remainder << 16U) | (quotient & 0xFFFFU);
    return {packed, false, (quotient & 0x8000U) != 0U, quotient == 0U};
  }
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, std::uint32_t dividend,
                                            std::uint16_t divisor, bool is_signed) noexcept {
    const auto division = evaluate(dividend, divisor, is_signed);
    const auto updated = static_cast<std::uint32_t>(status_register & UINT16_C(0xFFF0)) |
        (division.overflow ? UINT32_C(0x0002)
                            : ((division.negative ? UINT32_C(0x0008) : 0U) | (division.zero ? UINT32_C(0x0004) : 0U)));
    return static_cast<std::uint16_t>(updated);
  }
  // Emits a self-contained block assuming `dividend`/`divisor` (already
  // materialized C expressions of the stated width) and a non-zero divisor
  // (the caller must have already emitted the `if (divisor == 0)`
  // vector-5-raise branch). Declares/sets `result_var` (initialized to the
  // unchanged `dividend` so the overflow branch's "Dn completely unchanged"
  // rule holds with no separate flag needed by the caller) and updates
  // `status_register`.
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            const std::string &dividend, const std::string &divisor, bool is_signed,
                            const std::string &result_var) {
    // Deliberately NOT wrapped in its own `{ }` block: `result_var` must stay
    // in scope for the caller's subsequent EA write statement (the caller
    // owns the enclosing block/scope).
    out << "const uint32_t " << result_var << "_dividend = (uint32_t)(" << dividend << "); ";
    if (is_signed) {
      out << "const int32_t " << result_var << "_divisor = (int32_t)(int16_t)(" << divisor << "); "
          << "const int64_t " << result_var << "_q = (int64_t)(int32_t)" << result_var << "_dividend / "
          << result_var << "_divisor; "
          << "const int64_t " << result_var << "_r = (int64_t)(int32_t)" << result_var << "_dividend % "
          << result_var << "_divisor; "
          << "const int " << result_var << "_overflow = (" << result_var << "_q < -32768 || " << result_var
          << "_q > 32767); "
          << "uint32_t " << result_var << " = " << result_var << "_dividend; "
          << "if (!" << result_var << "_overflow) " << result_var << " = (uint32_t)(((uint32_t)(uint16_t)(int16_t)"
          << result_var << "_r << 16U) | (uint32_t)(uint16_t)(int16_t)" << result_var << "_q); "
          << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFF0)) | (" << result_var
          << "_overflow ? UINT16_C(0x0002) : ((" << result_var << "_q < 0 ? UINT16_C(0x0008) : UINT16_C(0)) | ("
          << result_var << "_q == 0 ? UINT16_C(0x0004) : UINT16_C(0)))));";
    } else {
      out << "const uint32_t " << result_var << "_divisor = (uint32_t)(uint16_t)(" << divisor << "); "
          << "const uint32_t " << result_var << "_q = " << result_var << "_dividend / " << result_var
          << "_divisor; "
          << "const uint32_t " << result_var << "_r = " << result_var << "_dividend % " << result_var
          << "_divisor; "
          << "const int " << result_var << "_overflow = (" << result_var << "_q > UINT32_C(0xFFFF)); "
          << "uint32_t " << result_var << " = " << result_var << "_dividend; "
          << "if (!" << result_var << "_overflow) " << result_var << " = (uint32_t)((" << result_var
          << "_r << 16U) | (" << result_var << "_q & UINT32_C(0xFFFF))); "
          << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFF0)) | (" << result_var
          << "_overflow ? UINT16_C(0x0002) : (((" << result_var << "_q & UINT32_C(0x8000)) != 0U ? UINT16_C(0x0008) : UINT16_C(0)) | ("
          << result_var << "_q == 0U ? UINT16_C(0x0004) : UINT16_C(0)))));";
    }
  }
};

// SEG-007-T025 (Batch C, C3): the one shared semantic owner for
// BTST/BCHG/BCLR/BSET (contract: "one shared bit-operation semantic
// owner"). `destination_width` is always exactly byte or long_word (never
// word: bit operations have no word-destination form); the effective bit
// index is `bit_number & 31` for a 32-bit (Dn) destination or
// `bit_number & 7` for an 8-bit (memory) destination -- deliberately modulo,
// not a bounds rejection (contract: "immediate bit-number forms... do not
// reject them merely because the immediate bit number is larger than the
// destination width"). The Z condition-code consequence is a SEPARATE,
// deliberately different rule (m68k_bit_test_ccr below); this struct owns
// only the destination-result/original-bit-value fact both host semantics
// and generated-C lowering read from.
struct M68kBitOperationSpecification {
  [[nodiscard]] static constexpr std::uint32_t width_mask_bits(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::long_word ? 31U : 7U;
  }
  [[nodiscard]] static M68kBitOperationResult evaluate(M68kBitOperationKind kind, std::uint32_t original,
                                                        std::uint32_t bit_number,
                                                        M68kMemoryAccessWidth destination_width) noexcept {
    const auto bit_index = bit_number & width_mask_bits(destination_width);
    const auto bit_mask = UINT32_C(1) << bit_index;
    const bool original_bit_set = (original & bit_mask) != 0U;
    switch (kind) {
    case M68kBitOperationKind::test: return {original, original_bit_set};
    case M68kBitOperationKind::change: return {original ^ bit_mask, original_bit_set};
    case M68kBitOperationKind::clear: return {original & ~bit_mask, original_bit_set};
    case M68kBitOperationKind::set: return {original | bit_mask, original_bit_set};
    }
    return {original, original_bit_set};
  }
  // Emits, into `out`, statements computing `result_local` (the
  // post-operation destination value; equal to `original_expr` for `test`)
  // and `bit_set_local` (the ORIGINAL tested bit, as a C `bool`-shaped
  // `int`) from already-available C expressions. Uses explicit unsigned
  // masks throughout: no signed-`1` shift, no unmasked shift count.
  static void emit_c_update(std::ostringstream &out, M68kBitOperationKind kind,
                            const std::string &original_expr, const std::string &bit_number_expr,
                            M68kMemoryAccessWidth destination_width, const std::string &bit_index_local,
                            const std::string &bit_mask_local, const std::string &bit_set_local,
                            const std::string &result_local) {
    out << "const uint32_t " << bit_index_local << " = (" << bit_number_expr << ") & " << width_mask_bits(destination_width)
        << "U; const uint32_t " << bit_mask_local << " = UINT32_C(1) << " << bit_index_local << "; const int "
        << bit_set_local << " = ((" << original_expr << ") & " << bit_mask_local << ") != 0U; ";
    switch (kind) {
    case M68kBitOperationKind::test:
      out << "const uint32_t " << result_local << " = (" << original_expr << ");";
      break;
    case M68kBitOperationKind::change:
      out << "const uint32_t " << result_local << " = (" << original_expr << ") ^ " << bit_mask_local << ";";
      break;
    case M68kBitOperationKind::clear:
      out << "const uint32_t " << result_local << " = (" << original_expr << ") & ~" << bit_mask_local << ";";
      break;
    case M68kBitOperationKind::set:
      out << "const uint32_t " << result_local << " = (" << original_expr << ") | " << bit_mask_local << ";";
      break;
    }
  }
};

// Shared bit-test Z-only condition-code update (contract: "CCR semantics"):
// deliberately distinct from every logical/move-result CCR owner, since
// this is the ONLY selected operation family whose CCR rule leaves N/V/C
// (and every upper SR bit) completely untouched and updates Z alone, from
// the ORIGINAL tested bit rather than the destination result.
struct M68kBitTestCcrSpecification {
  static constexpr std::uint16_t preserved_mask = UINT16_C(0xFFFB);  // every bit except Z (bit 2)
  static constexpr std::uint16_t zero_mask = UINT16_C(0x0004);
  [[nodiscard]] static std::uint16_t apply(std::uint16_t status_register, bool original_bit_set) noexcept {
    return static_cast<std::uint16_t>((status_register & preserved_mask) | (original_bit_set ? 0U : zero_mask));
  }
  static void emit_c_update(std::ostringstream &out, std::string_view status_register,
                            const std::string &bit_set_expr) {
    out << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0x"
        << m68k_hex_literal(preserved_mask, 4) << ")) | ((" << bit_set_expr << ") ? UINT16_C(0) : " << zero_mask << "U));";
  }
};

// SEG-007-T025 (Batch C, C4): the one shared condition-code owner for Bcc/
// DBcc (contract: "one shared condition-code owner"), verified against
// M68000PM/AD Rev. 1 and pinned Musashi's COND_* macros. No condition reads
// X. `evaluate` (host) and `c_expr` (generated C) express the identical
// truth table from the same four extracted N/Z/V/C booleans; neither Bcc
// nor DBcc's emission hand-writes any of these 16 formulas a second time.
struct M68kConditionSpecification {
  static constexpr std::uint16_t negative_mask = UINT16_C(0x0008);
  static constexpr std::uint16_t zero_mask = UINT16_C(0x0004);
  static constexpr std::uint16_t overflow_mask = UINT16_C(0x0002);
  static constexpr std::uint16_t carry_mask = UINT16_C(0x0001);
  [[nodiscard]] static bool evaluate(M68kCondition condition, std::uint16_t status_register) noexcept {
    const bool n = (status_register & negative_mask) != 0U;
    const bool z = (status_register & zero_mask) != 0U;
    const bool v = (status_register & overflow_mask) != 0U;
    const bool c = (status_register & carry_mask) != 0U;
    switch (condition) {
    case M68kCondition::always: return true;
    case M68kCondition::never: return false;
    case M68kCondition::hi: return !c && !z;
    case M68kCondition::ls: return c || z;
    case M68kCondition::cc: return !c;
    case M68kCondition::cs: return c;
    case M68kCondition::ne: return !z;
    case M68kCondition::eq: return z;
    case M68kCondition::vc: return !v;
    case M68kCondition::vs: return v;
    case M68kCondition::pl: return !n;
    case M68kCondition::mi: return n;
    case M68kCondition::ge: return n == v;
    case M68kCondition::lt: return n != v;
    case M68kCondition::gt: return !z && (n == v);
    case M68kCondition::le: return z || (n != v);
    }
    return false;
  }
  [[nodiscard]] static std::string c_expr(M68kCondition condition, std::string_view status_register) {
    const auto sr = std::string(status_register);
    const auto n = "((" + sr + " & UINT16_C(0x0008)) != 0U)";
    const auto z = "((" + sr + " & UINT16_C(0x0004)) != 0U)";
    const auto v = "((" + sr + " & UINT16_C(0x0002)) != 0U)";
    const auto c = "((" + sr + " & UINT16_C(0x0001)) != 0U)";
    switch (condition) {
    case M68kCondition::always: return "1";
    case M68kCondition::never: return "0";
    case M68kCondition::hi: return "(!" + c + " && !" + z + ")";
    case M68kCondition::ls: return "(" + c + " || " + z + ")";
    case M68kCondition::cc: return "(!" + c + ")";
    case M68kCondition::cs: return c;
    case M68kCondition::ne: return "(!" + z + ")";
    case M68kCondition::eq: return z;
    case M68kCondition::vc: return "(!" + v + ")";
    case M68kCondition::vs: return v;
    case M68kCondition::pl: return "(!" + n + ")";
    case M68kCondition::mi: return n;
    case M68kCondition::ge: return "(" + n + " == " + v + ")";
    case M68kCondition::lt: return "(" + n + " != " + v + ")";
    case M68kCondition::gt: return "(!" + z + " && (" + n + " == " + v + "))";
    case M68kCondition::le: return "(" + z + " || (" + n + " != " + v + "))";
    }
    return "0";
  }
};

// SEG-007-T025 (Batch C, C6): the one shared register shift/rotate semantic
// owner (contract: "one shared register shift/rotate semantic owner").
// `evaluate()` (host) and `emit_c_update()` (generated C) express the
// identical per-family formula from the same extracted count/width facts;
// neither host semantics nor generated-C lowering hand-writes a family's
// formula a second time. C6a supplies only `lsl`/`lsr`; C6b/C6c/C6d widen
// this same struct's switches (never a parallel struct) as
// M68kShiftRotateKind itself widens.
//
// Every branch below is proven safe under strict C11: `count` (whether the
// decode-resolved 1-8 immediate or the caller-materialized `Dn & 0x3F`
// register value, 0-63) is compared against `bits` (8/16/32) BEFORE any
// native shift is performed, so no shift ever executes with a count that
// reaches or exceeds the 32-bit `uint32_t` intermediate's own width
// (contract: "no undefined C shifts") -- the `count == bits` boundary is
// handled by direct bit extraction (`src >> (bits - 1)` or `src & 1`,
// both always safe since `bits - 1 < 32`), never by a shift of exactly
// `bits`.
struct M68kShiftRotateSpecification {
  [[nodiscard]] static constexpr unsigned width_bits(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? 8U : width == M68kMemoryAccessWidth::word ? 16U : 32U;
  }
  [[nodiscard]] static constexpr std::uint32_t mask(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x000000FF) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x0000FFFF) : UINT32_MAX;
  }
  [[nodiscard]] static constexpr std::uint32_t sign(M68kMemoryAccessWidth width) noexcept {
    return width == M68kMemoryAccessWidth::byte ? UINT32_C(0x00000080) :
           width == M68kMemoryAccessWidth::word ? UINT32_C(0x00008000) : UINT32_C(0x80000000);
  }
  // SEG-007-T025 (Batch C, C6a): LSL/LSR's shared shape -- zero-fill
  // shift, four count regions (0; 1..bits-1; exactly bits; beyond bits),
  // verified against pinned Musashi's base-MC68000 "r"/"s" LSL/LSR
  // handlers (m68k_in.c). At count 0: result/N/Z reflect the UNCHANGED
  // sized original, C is cleared, and X is PRESERVED (never cleared) --
  // expressed here as `extend_value = original_extend` (contract:
  // "register count zero... C cleared, X preserved"), never as an
  // unwritten/no-op X. At 1<=count<bits: C=X= the last bit shifted out
  // (LSL: original bit `bits-count`; LSR: original bit `count-1`). At
  // count==bits: result is exactly 0; C=X= the single remaining edge bit
  // (LSL: original bit 0; LSR: original bit `bits-1`) -- this is a
  // genuine, oracle-verified boundary, not folded into "beyond bits". At
  // count>bits: result 0, C and X both CLEARED (not preserved). V is
  // always clear for LSL/LSR.
  [[nodiscard]] static M68kShiftRotateResult evaluate(M68kShiftRotateKind kind, std::uint32_t original,
                                                       std::uint32_t count, M68kMemoryAccessWidth width,
                                                       bool original_extend) noexcept {
    const auto bits = width_bits(width);
    const auto m = mask(width);
    const auto src = original & m;
    M68kShiftRotateResult out{};
    out.extend_written = true;
    std::uint32_t result = src;
    bool carry = false;
    switch (kind) {
    case M68kShiftRotateKind::lsl:
      if (count == 0U) {
        result = src;
        carry = false;
        out.extend_value = original_extend;
      } else if (count < bits) {
        result = (src << count) & m;
        carry = ((src >> (bits - count)) & 1U) != 0U;
        out.extend_value = carry;
      } else if (count == bits) {
        result = 0U;
        carry = (src & 1U) != 0U;
        out.extend_value = carry;
      } else {
        result = 0U;
        carry = false;
        out.extend_value = false;
      }
      break;
    case M68kShiftRotateKind::lsr:
      if (count == 0U) {
        result = src;
        carry = false;
        out.extend_value = original_extend;
      } else if (count < bits) {
        result = src >> count;
        carry = ((src >> (count - 1U)) & 1U) != 0U;
        out.extend_value = carry;
      } else if (count == bits) {
        result = 0U;
        carry = ((src >> (bits - 1U)) & 1U) != 0U;
        out.extend_value = carry;
      } else {
        result = 0U;
        carry = false;
        out.extend_value = false;
      }
      break;
    // SEG-007-T025 (Batch C, C6b): ASR/ASL's shared shape, verified against
    // pinned Musashi's base-MC68000 "r" ASR/ASL handlers (m68k_in.c). Unlike
    // LSL/LSR, the count==bits boundary is folded into the SAME branch as
    // count>bits for ASR (both saturate identically, verified: Musashi's
    // own `shift<8`/`shift<16`/`shift<32` gate, not `<=`) -- this
    // "count>=bits" branch below therefore intentionally covers both, never
    // a fourth C6a-shaped region. ASL keeps count==bits and count>bits
    // distinct only for C/X (never for V, which uses the same
    // `original!=0` rule at both).
    case M68kShiftRotateKind::asr: {
      const auto negative_src = (src & sign(width)) != 0U;
      if (count == 0U) {
        result = src;
        carry = false;
        out.extend_value = original_extend;
      } else if (count < bits) {
        // Sign-fill: replicate the original sign into the vacated top
        // `count` bits, computed without any implementation-defined signed
        // right shift -- `~(m >> count) & m` is the top-`count`-bits mask
        // (safe: `count < bits <= 32`), applied only when the original was
        // negative.
        result = src >> count;
        if (negative_src) result |= (m & ~(m >> count));
        carry = ((src >> (count - 1U)) & 1U) != 0U;
        out.extend_value = carry;
      } else {
        // count >= bits (verified against pinned Musashi: identical at
        // exactly `bits` and beyond -- base-MC68000 saturation).
        if (negative_src) {
          result = m;
          carry = true;
        } else {
          result = 0U;
          carry = false;
        }
        out.extend_value = carry;
      }
      break;
    }
    case M68kShiftRotateKind::asl: {
      if (count == 0U) {
        result = src;
        carry = false;
        out.extend_value = original_extend;
        out.overflow = false;
      } else if (count < bits) {
        result = (src << count) & m;
        carry = ((src >> (bits - count)) & 1U) != 0U;
        out.extend_value = carry;
        // SEG-007-T025 (Batch C, C6b): ASL's load-bearing overflow rule
        // (contract: "ASL overflow is load-bearing") -- V is set unless the
        // top `count + 1` original bits are ALL the same sign (all zero or
        // all one), never merely "old sign != final sign" (insufficient for
        // a multi-bit shift whose sign flips and flips back). `region_mask`
        // is the top-`(count + 1)`-bits mask of the `bits`-wide value,
        // computed the same safe way as the sign-fill mask above; the
        // `region_bits >= bits` guard (reached only when `count == bits -
        // 1`, i.e. `region_bits == bits`) avoids ever shifting `m` by
        // `bits` itself (unsafe at LONG's `count == 31` boundary) by using
        // the whole mask directly instead -- "the top `bits` bits of a
        // `bits`-wide value" is trivially the whole value.
        const auto region_bits = count + 1U;
        const auto region_mask = region_bits >= bits ? m : (m & ~(m >> region_bits));
        const auto region = src & region_mask;
        out.overflow = !(region == 0U || region == region_mask);
      } else if (count == bits) {
        result = 0U;
        carry = (src & 1U) != 0U;
        out.extend_value = carry;
        out.overflow = src != 0U;
      } else {
        result = 0U;
        carry = false;
        out.extend_value = false;
        out.overflow = src != 0U;
      }
      break;
    }
    case M68kShiftRotateKind::rol:
    case M68kShiftRotateKind::ror: {
      // SEG-007-T025 (Batch C, C6c): ROL/ROR contract ("X is never modified
      // by ROL/ROR" -- load-bearing) -- unlike every other shift/rotate kind
      // above, X is architecturally never referenced by Musashi's ROL/ROR
      // handlers at all, so this is the one place `out.extend_written`
      // (defaulted to `true` above for the shift kinds) is overridden back
      // to `false`; `ccr()` below already knows to preserve the caller's
      // existing X bit whenever `extend_written == false`, unchanged since
      // it was designed for exactly this case back in C6a.
      out.extend_written = false;
      // Contract "keep original count and effective count separate": `count`
      // here is the ORIGINAL count (0-63 for register-sourced, 1-8 for
      // immediate-sourced, resolved by the caller) -- it is DISTINCT from
      // the effective (modulo-width) rotation amount computed below. An
      // `original` count of exactly zero is a genuinely different case from
      // a nonzero original count that happens to reduce to an effective
      // count of zero (e.g. BYTE count 8): both leave the data bits
      // unchanged, but only the latter passes a real bit through carry.
      if (count == 0U) {
        result = src;
        carry = false;
      } else {
        // `bits` is always a power of two (8/16/32), so `bits - 1U` is a
        // safe mask for the modulo-width reduction without a `%` operator.
        const auto effective = count & (bits - 1U);
        if (effective == 0U) {
          // Contract "nonzero multiple-of-width rotation": the data is
          // unchanged (a full rotation, or several, returns every bit to
          // its start), but this is NOT the same as the true-zero-count
          // case above -- the carry below still reflects a genuine
          // rotated-out bit, computed uniformly from `result` (== `src`
          // here) exactly like the nonzero-effective-count branch does.
          result = src;
        } else {
          // Contract "safe rotation implementation": `effective` is always
          // in `[1, bits - 1]` here, so neither shift amount below
          // (`effective` or `bits - effective`) ever reaches `bits` --
          // never an undefined/native-width C shift.
          result = kind == M68kShiftRotateKind::rol
                       ? (((src << effective) | (src >> (bits - effective))) & m)
                       : (((src >> effective) | (src << (bits - effective))) & m);
        }
        // Contract carry rule, verified against pinned Musashi's ROL/ROR
        // opcode handlers bit-for-bit (both the `shift == 0`/full-cycle and
        // `shift != 0`/partial-rotation internal branches collapse into
        // this single formula once expressed in terms of the final
        // `result`, for ANY nonzero original count): ROL carries out the
        // final bit that rotated into bit 0; ROR carries out the final bit
        // that rotated into the sign/MSB position.
        carry = kind == M68kShiftRotateKind::rol ? (result & 1U) != 0U : (result & sign(width)) != 0U;
      }
      break;
    }
    case M68kShiftRotateKind::roxl:
    case M68kShiftRotateKind::roxr: {
      // SEG-007-T025 (Batch C, C6d): ROXL/ROXR are a genuine width+1
      // rotation RING (contract: "ROX is a width+1 rotation ring"), never
      // ordinary ROL/ROR plus a bolted-on X assignment. `original_extend`
      // (the caller's PRE-OPERATION X, already an existing parameter to
      // this function -- see contract "original X must be snapshotted")
      // becomes the ring's extra high bit at position `bits`; a `uint64_t`
      // intermediate represents all three ring widths (9/17/33 bits)
      // uniformly and safely (contract: "use uint64_t for the 33-bit
      // ring") -- the widest shift used below is by 32, well inside
      // `uint64_t`'s 64-bit width.
      const std::uint64_t ring =
          (static_cast<std::uint64_t>(original_extend ? 1U : 0U) << bits) | static_cast<std::uint64_t>(src);
      const auto ring_bits = bits + 1U;
      const std::uint64_t ring_mask = (UINT64_C(1) << ring_bits) - 1U;
      // Contract "register count rule": ROX's effective count is the
      // ORIGINAL count modulo the RING width (9/17/33), never modulo the
      // ordinary data width (8/16/32) that ROL/ROR use -- a distinct
      // reduction domain from every other C6 family. `count` here is
      // already the same caller-resolved original count ROL/ROR receive
      // (0-63 register-sourced, 1-8 immediate-sourced); it is reduced with
      // `%`, not `&`, since `ring_bits` (9/17/33) is never a power of two.
      // `count == 0` (true register-count zero) and a nonzero `count` that
      // happens to be an exact multiple of `ring_bits` (e.g. BYTE count 9,
      // 18, 63) BOTH reduce to `effective == 0` here and are handled by
      // the SAME identity branch below -- proven against pinned Musashi to
      // be observationally identical (an unrotated ring), even though
      // `count` itself is never discarded or conflated (contract: "nonzero
      // multiple of ring width" -- retain the truthful raw/effective-count
      // facts).
      const auto effective = count % ring_bits;
      std::uint64_t rotated_ring;
      if (effective == 0U) {
        rotated_ring = ring;
      } else {
        // Contract "safe ring rotation": `effective` is always in
        // `[1, ring_bits - 1]` here (`ring_bits <= 33`), so neither shift
        // amount below ever reaches or exceeds 64 -- never an undefined
        // `uint64_t` shift, and never a native shift by exactly `width` or
        // `width + 1`.
        rotated_ring = kind == M68kShiftRotateKind::roxl
                            ? (((ring << effective) | (ring >> (ring_bits - effective))) & ring_mask)
                            : (((ring >> effective) | (ring << (ring_bits - effective))) & ring_mask);
      }
      // Contract "one ROX result/X/C owner": the sized destination is the
      // ring's low `bits` bits; the NEW X is the ring's one extra bit at
      // position `bits`; C always equals the new X (never independently
      // computed) -- true for both the true-zero-count and
      // ring-width-multiple identity cases (where the "new" X is trivially
      // the unchanged original X) and for a genuine partial rotation
      // alike, with no separate case needed.
      result = static_cast<std::uint32_t>(rotated_ring & static_cast<std::uint64_t>(m));
      const bool new_extend = ((rotated_ring >> bits) & UINT64_C(1)) != 0U;
      carry = new_extend;
      out.extend_value = new_extend;
      // `out.extend_written` is left at its shared default of `true` here
      // (set once, unconditionally, before this switch) -- ROX, unlike
      // ROL/ROR, always writes X.
      break;
    }
    }
    out.result = result;
    out.carry = carry;
    out.negative = (result & sign(width)) != 0U;
    out.zero = result == 0U;
    // `out.overflow` defaults to `false` (M68kShiftRotateResult's own
    // default member initializer) and is left untouched by
    // lsl/lsr/asr/rol/ror, all of which are always V-clear; only the `asl`
    // branches above ever set it explicitly.
    return out;
  }
  [[nodiscard]] static std::uint16_t ccr(std::uint16_t status_register,
                                          const M68kShiftRotateResult &result) noexcept {
    auto updated = static_cast<std::uint16_t>((status_register & UINT16_C(0xFFE0)) |
        (result.negative ? UINT16_C(0x08) : UINT16_C(0)) | (result.zero ? UINT16_C(0x04) : UINT16_C(0)) |
        (result.overflow ? UINT16_C(0x02) : UINT16_C(0)) | (result.carry ? UINT16_C(0x01) : UINT16_C(0)));
    if (result.extend_written)
      updated = static_cast<std::uint16_t>((updated & ~UINT16_C(0x10)) |
                                            (result.extend_value ? UINT16_C(0x10) : UINT16_C(0)));
    else
      updated = static_cast<std::uint16_t>((updated & ~UINT16_C(0x10)) | (status_register & UINT16_C(0x10)));
    return updated;
  }
  // Emits the exact same four-region formula as evaluate() above (never a
  // second, independently hand-written copy), reading `count_expr` and
  // `original_expr` exactly once each into their own locals so the value
  // used for the count and the value used for the (possibly identical, for
  // an alias case like `LSL D0,D0`) original destination are each
  // materialized before any write -- the emitter's caller supplies
  // `count_expr` already fully resolved (the decoded 1-8 immediate, or the
  // caller's own `d[n] & 63U` local captured before this call), so this
  // function itself never re-reads a live register. Returns the C
  // expression (a local variable name) holding the post-operation
  // width-sized result; the caller writes it to the destination through
  // the existing shared sized-register-write primitive and never
  // recomputes it. Also emits the complete SR update as part of the same
  // statement sequence.
  [[nodiscard]] static std::string emit_c_update(std::ostringstream &out, M68kShiftRotateKind kind,
                                                  const std::string &original_expr, const std::string &count_expr,
                                                  M68kMemoryAccessWidth width, std::string_view status_register,
                                                  unsigned &temp_ordinal) {
    const auto bits = width_bits(width);
    const auto mask_literal = "UINT32_C(0x" + m68k_hex_literal(mask(width), 8) + ")";
    const auto sign_literal = "UINT32_C(0x" + m68k_hex_literal(sign(width), 8) + ")";
    const auto count_local = "sr_count_" + std::to_string(temp_ordinal++);
    const auto src_local = "sr_src_" + std::to_string(temp_ordinal++);
    const auto result_local = "sr_result_" + std::to_string(temp_ordinal++);
    const auto carry_local = "sr_carry_" + std::to_string(temp_ordinal++);
    const auto extend_local = "sr_extend_" + std::to_string(temp_ordinal++);
    const auto overflow_local = "sr_overflow_" + std::to_string(temp_ordinal++);
    out << "const uint32_t " << count_local << " = (" << count_expr << "); const uint32_t " << src_local << " = ("
        << original_expr << ") & " << mask_literal << "; uint32_t " << result_local << "; int " << carry_local
        << "; int " << extend_local << "; int " << overflow_local << " = 0;\n";
    const auto current_x = "((" + std::string(status_register) + " & UINT16_C(0x10)) != 0U ? 1 : 0)";
    switch (kind) {
    case M68kShiftRotateKind::lsl:
      out << "if (" << count_local << " == 0U) { " << result_local << " = " << src_local << "; " << carry_local
          << " = 0; " << extend_local << " = " << current_x << "; } else if (" << count_local << " < " << bits
          << "U) { " << result_local << " = (" << src_local << " << " << count_local << ") & " << mask_literal
          << "; " << carry_local << " = ((" << src_local << " >> (" << bits << "U - " << count_local
          << ")) & 1U) != 0U; " << extend_local << " = " << carry_local << "; } else if (" << count_local
          << " == " << bits << "U) { " << result_local << " = 0U; " << carry_local << " = (" << src_local
          << " & 1U) != 0U; " << extend_local << " = " << carry_local << "; } else { " << result_local
          << " = 0U; " << carry_local << " = 0; " << extend_local << " = 0; }\n";
      break;
    case M68kShiftRotateKind::lsr:
      out << "if (" << count_local << " == 0U) { " << result_local << " = " << src_local << "; " << carry_local
          << " = 0; " << extend_local << " = " << current_x << "; } else if (" << count_local << " < " << bits
          << "U) { " << result_local << " = " << src_local << " >> " << count_local << "; " << carry_local
          << " = ((" << src_local << " >> (" << count_local << " - 1U)) & 1U) != 0U; " << extend_local << " = "
          << carry_local << "; } else if (" << count_local << " == " << bits << "U) { " << result_local
          << " = 0U; " << carry_local << " = ((" << src_local << " >> (" << bits << "U - 1U)) & 1U) != 0U; "
          << extend_local << " = " << carry_local << "; } else { " << result_local << " = 0U; " << carry_local
          << " = 0; " << extend_local << " = 0; }\n";
      break;
    case M68kShiftRotateKind::asr: {
      // SEG-007-T025 (Batch C, C6b): sign-fill without any
      // implementation-defined signed right shift -- an explicit unsigned
      // "negative" local (`sr_neg_N`) plus the same safe top-bits mask used
      // in evaluate() drives the fill and the count>=bits saturation alike.
      const auto negative_local = "sr_neg_" + std::to_string(temp_ordinal++);
      out << "const int " << negative_local << " = ((" << src_local << " & " << sign_literal << ") != 0U);\n"
          << "if (" << count_local << " == 0U) { " << result_local << " = " << src_local << "; " << carry_local
          << " = 0; " << extend_local << " = " << current_x << "; } else if (" << count_local << " < " << bits
          << "U) { " << result_local << " = " << src_local << " >> " << count_local << "; if (" << negative_local
          << ") { " << result_local << " |= (" << mask_literal << " & ~(" << mask_literal << " >> " << count_local
          << ")); } " << carry_local << " = ((" << src_local << " >> (" << count_local << " - 1U)) & 1U) != 0U; "
          << extend_local << " = " << carry_local << "; } else { if (" << negative_local << ") { " << result_local
          << " = " << mask_literal << "; " << carry_local << " = 1; } else { " << result_local << " = 0U; "
          << carry_local << " = 0; } " << extend_local << " = " << carry_local << "; }\n";
      break;
    }
    case M68kShiftRotateKind::asl:
      // SEG-007-T025 (Batch C, C6b): the load-bearing overflow rule
      // (contract: "ASL overflow is load-bearing") -- `sr_overflow_N` is set
      // from the SAME top-`(count + 1)`-bits-uniform test as evaluate()
      // (never "old sign != final sign" alone), safe at every boundary
      // including LONG's `count == 31` (`region_bits == bits` uses the
      // whole mask directly, never `mask >> bits`).
      out << "if (" << count_local << " == 0U) { " << result_local << " = " << src_local << "; " << carry_local
          << " = 0; " << extend_local << " = " << current_x << "; " << overflow_local << " = 0; } else if ("
          << count_local << " < " << bits << "U) { " << result_local << " = (" << src_local << " << "
          << count_local << ") & " << mask_literal << "; " << carry_local << " = ((" << src_local << " >> ("
          << bits << "U - " << count_local << ")) & 1U) != 0U; " << extend_local << " = " << carry_local
          << "; { const uint32_t sr_region_bits_" << temp_ordinal << " = " << count_local
          << " + 1U; const uint32_t sr_region_mask_" << temp_ordinal << " = (sr_region_bits_" << temp_ordinal
          << " >= " << bits << "U) ? " << mask_literal << " : (" << mask_literal << " & ~(" << mask_literal
          << " >> sr_region_bits_" << temp_ordinal << ")); const uint32_t sr_region_" << temp_ordinal << " = "
          << src_local << " & sr_region_mask_" << temp_ordinal << "; " << overflow_local << " = !(sr_region_"
          << temp_ordinal << " == 0U || sr_region_" << temp_ordinal << " == sr_region_mask_" << temp_ordinal
          << "); }\n} else if (" << count_local << " == " << bits << "U) { " << result_local << " = 0U; "
          << carry_local << " = (" << src_local << " & 1U) != 0U; " << extend_local << " = " << carry_local
          << "; " << overflow_local << " = (" << src_local << " != 0U); } else { " << result_local << " = 0U; "
          << carry_local << " = 0; " << extend_local << " = 0; " << overflow_local << " = (" << src_local
          << " != 0U); }\n";
      ++temp_ordinal;
      break;
    case M68kShiftRotateKind::rol:
    case M68kShiftRotateKind::ror: {
      // SEG-007-T025 (Batch C, C6c): mirrors evaluate()'s rol/ror case
      // above exactly (never a second, independently hand-written copy).
      // `extend_local` is set to `current_x` (the CURRENT/pre-update X bit
      // read from `status_register`, already used by every other kind's own
      // count==0 branch above) in EVERY branch below, never data-dependent
      // -- this is how "X is never modified by ROL/ROR" (load-bearing) is
      // achieved through the SAME unified SR-update statement shared by
      // every shift/rotate kind, without needing to special-case that
      // statement itself.
      const auto effective_local = "sr_effective_" + std::to_string(temp_ordinal++);
      const auto rotate = kind == M68kShiftRotateKind::rol;
      out << extend_local << " = " << current_x << ";\n"
          << "if (" << count_local << " == 0U) { " << result_local << " = " << src_local << "; " << carry_local
          << " = 0; } else { const uint32_t " << effective_local << " = " << count_local << " & (" << bits
          << "U - 1U); if (" << effective_local << " == 0U) { " << result_local << " = " << src_local
          << "; } else { " << result_local << " = " << (rotate ? "((" : "((") << src_local
          << (rotate ? " << " : " >> ") << effective_local << ") | (" << src_local
          << (rotate ? " >> (" : " << (") << bits << "U - " << effective_local << ")))" << " & " << mask_literal
          << "; } " << carry_local << " = "
          << (rotate ? ("(" + result_local + " & 1U) != 0U") : ("(" + result_local + " & " + sign_literal + ") != 0U"))
          << "; }\n";
      break;
    }
    case M68kShiftRotateKind::roxl:
    case M68kShiftRotateKind::roxr: {
      // SEG-007-T025 (Batch C, C6d): mirrors evaluate()'s roxl/roxr case
      // above exactly (never a second, independently hand-written copy).
      // `current_x` (the caller's PRE-OPERATION X, already declared above
      // and used by every other kind's own count==0 branch) is read into
      // the ring's extra high bit BEFORE this statement performs the final
      // combined SR update below (contract: "original X must be
      // snapshotted") -- never derived from `status_register` after any
      // other flag in this same statement sequence has changed it, since
      // that read happens exactly once, right here, ahead of everything
      // else in this case.
      const auto ring_local = "sr_ring_" + std::to_string(temp_ordinal++);
      const auto rotated_local = "sr_rotated_" + std::to_string(temp_ordinal++);
      const auto effective_local = "sr_effective_" + std::to_string(temp_ordinal++);
      const auto ring_bits = bits + 1U;
      const auto ring_mask_literal = "((UINT64_C(1) << " + std::to_string(ring_bits) + "U) - 1U)";
      const auto rotate = kind == M68kShiftRotateKind::roxl;
      out << "const uint64_t " << ring_local << " = (((uint64_t)(" << current_x << ")) << " << bits << "U) | "
          << "(uint64_t)" << src_local << ";\n"
          // Contract "register count rule": modulo the RING width
          // (bits+1), never the ordinary data width ROL/ROR use.
          << "const uint32_t " << effective_local << " = " << count_local << " % " << ring_bits << "U;\n"
          << "uint64_t " << rotated_local << ";\n"
          << "if (" << effective_local << " == 0U) { " << rotated_local << " = " << ring_local << "; } else { "
          << rotated_local << " = " << (rotate ? "((" : "((") << ring_local << (rotate ? " << " : " >> ")
          << effective_local << ") | (" << ring_local << (rotate ? " >> (" : " << (") << ring_bits << "U - "
          << effective_local << ")))" << " & " << ring_mask_literal << "; }\n"
          << result_local << " = (uint32_t)(" << rotated_local << " & (uint64_t)" << mask_literal << ");\n"
          << extend_local << " = ((" << rotated_local << " >> " << bits << "U) & UINT64_C(1)) != 0U;\n"
          << carry_local << " = " << extend_local << ";\n";
      break;
    }
    }
    out << status_register << " = (uint16_t)((" << status_register << " & UINT16_C(0xFFE0)) | (" << extend_local
        << " ? UINT16_C(0x10) : UINT16_C(0)) | ((" << result_local << " & " << sign_literal
        << ") != 0U ? UINT16_C(0x08) : UINT16_C(0)) | (" << result_local << " == 0U ? UINT16_C(0x04) : UINT16_C(0)) | ("
        << overflow_local << " ? UINT16_C(0x02) : UINT16_C(0)) | (" << carry_local
        << " ? UINT16_C(0x01) : UINT16_C(0)));\n";
    return result_local;
  }
};

// SEG-007-T025 (Batch C, C4c review correction): the one shared DBcc
// low-word decrement fact (contract: "DBcc semantic order"), verified
// against pinned Musashi's dbcc/dbf/dbt opcode bodies:
// `MASK_OUT_ABOVE_16(*r_dst - 1)` combined with `MASK_OUT_BELOW_16(*r_dst) |
// res` -- decrement only the low 16 bits, preserve the upper 16 bits
// unconditionally, then compare the new low word against 0xFFFF to decide
// branch-vs-fallthrough. Mirrors M68kConditionSpecification's own
// host-evaluate-plus-generated-C-lowering shape (immediately above
// M68kShiftRotateSpecification): both evaluate() (host) and emit_c_update()
// (generated C) apply this exact formula from this ONE place -- neither
// m68k_evaluate_dbcc_decrement nor emit_m68k_operation_c's dbcc_loop case
// may reimplement it independently.
struct M68kDbccDecrementSpecification {
  static constexpr std::uint32_t upper_word_mask = UINT32_C(0xFFFF0000);
  static constexpr std::uint32_t low_word_mask = UINT32_C(0x0000FFFF);
  static constexpr std::uint16_t expired_value = UINT16_C(0xFFFF);

  [[nodiscard]] static M68kDbccDecrementResult evaluate(std::uint32_t original) noexcept {
    const auto low = static_cast<std::uint16_t>((original & low_word_mask) - 1U);
    return {(original & upper_word_mask) | low, low == expired_value};
  }

  // Emits the exact same low-word-decrement/upper-word-preservation formula
  // as evaluate() above as a single generated-C assignment statement to the
  // live register lvalue `dn_expr`, then returns the C boolean expression
  // testing the SAME post-decrement expiry condition evaluate() reports via
  // M68kDbccDecrementResult::expired. Callers orchestrate (condition-first
  // dispatch, target-vs-fallthrough selection); they must not compute the
  // decrement or expiry formula themselves.
  [[nodiscard]] static std::string emit_c_update(std::ostringstream &out, const std::string &dn_expr) {
    out << dn_expr << " = (" << dn_expr << " & UINT32_C(0xFFFF0000)) | (uint32_t)(uint16_t)((" << dn_expr
        << " & UINT32_C(0xFFFF)) - 1U); ";
    return "((" + dn_expr + " & UINT32_C(0xFFFF)) == UINT32_C(0xFFFF))";
  }
};

// SEG-007-T025 (Batch C, C4): meaningful only for `branch`/`bsr`/`dbcc`
// downstream in M68kOperationEffect below.
struct M68kRegisterWrite { DataRegister reg{DataRegister::d0}; std::uint32_t value{}; };
// `absolute_test` (SEG-007-T008): a memory read whose value feeds only
// condition-code computation, with no destination register -- distinct from
// `absolute_load`, which always writes a register.
enum class M68kMemoryEffectKind { none, absolute_store, absolute_load, absolute_test };
// What stack effect a selected operation requests. The *value* pushed (the
// discovered call continuation) or the authorization used to validate a pop
// is never part of this effect -- those come from the caller's own
// call/return discovery, and from the actual host RAM read, respectively.
// This only says which kind of stack motion the operation itself requests
// and how wide it is.
enum class M68kStackEffectKind { none, push_static_continuation, pop_static_return, push_data_long, pop_data_long,
  // SEG-007-T047 / ADR-0020 §9: RTE pops the basic MC68000 exception stack
  // frame (SR at SP, PC at SP+2, A7 += 6). Compared-only; the runtime routine
  // genesis_exception_return owns the actual frame read/commit.
  pop_exception_frame };
// What PC transition a selected operation requests: sequential advance by
// pc_delta, an unconditional jump to a statically known direct_target (JSR),
// or a jump to whatever value the adapter observes on the stack for a
// pop_static_return stack effect (RTS) -- the observed value itself is never
// part of this effect.
enum class M68kPcEffectKind { none, advance, direct_target, observed_stack_return,
  // SEG-007-T047 / ADR-0020 §9: PC becomes whatever value RTE restores from
  // the exception frame; the observed value itself is never part of this effect.
  observed_exception_return };
// The shared MC68000 execution-semantic owner for every already-selected
// M68kIrOperation kind. Given only the operation, this decides exactly what
// a selected MC68000 instruction means: which register (if any) is written
// and with what value, whether a memory access is a store or a load and
// which register and (unvalidated) address it uses, and the requested stack
// push/pop *intent* and width plus PC-transition *kind* (sequential advance,
// direct jump target, or observed-stack-return). It has no memory,
// RAM-window, or address-policy model to reuse and must not gain one:
// address alignment/range validation, the actual backing storage,
// condition-code application (via the shared condition-code helpers, applied
// by the caller to whichever value this effect identifies as CCR-affecting),
// the call/return stack *values* that depend on discovered call identity,
// the read observed return value (a real RAM read the adapter performs), and
// whether/how a requested effect succeeds against RAM/A7 bounds all remain
// the caller's job.
struct M68kOperationEffect {
  // Complete architectural Dn/An write footprint.  A set bit names a register
  // which this operation writes, including decoded EA auto-updates.  `false`
  // means that this operation is not safe for a consumer which needs a full
  // footprint (rather than meaning that it writes no registers).
  bool register_write_footprint_complete{};
  std::uint8_t data_register_write_mask{};
  std::uint8_t address_register_write_mask{};
  std::optional<M68kRegisterWrite> register_write;   // MOVEQ only
  M68kMemoryEffectKind memory{M68kMemoryEffectKind::none};
  DataRegister memory_register{DataRegister::d0};    // d0 for store, d1 for load
  std::uint32_t memory_address{};                    // operation.extension, NOT yet validated
  bool affects_condition_codes{};                     // true exactly for selected CCR-affecting kinds
  M68kExtendFlagPolicy extend_flag_policy{M68kExtendFlagPolicy::preserve};
  M68kStackEffectKind stack{M68kStackEffectKind::none};
  std::uint32_t stack_width{};                         // 4 for both selected stack forms
  M68kPcEffectKind pc{M68kPcEffectKind::none};
  std::uint32_t pc_delta{};                            // meaningful only when pc == advance
  std::uint32_t direct_target{};                       // meaningful only when pc == direct_target
  // SEG-007-T023 additions. `address_register_write` is set (instead of
  // `register_write`) exactly when a kind's destination is an address
  // register (MOVEA, LEA); the concrete value is EA-dependent (register
  // state or a resolved address) and is deliberately NOT computed here --
  // this effect layer has no memory/register-file model to reuse, per its
  // existing documented contract, so it identifies only *which* register
  // slot is written, never the value. `operand_size`/`resolved_source_ea`/
  // `resolved_destination_ea` are the operation's own already-decoded EA
  // facts, copied through unresolved (no address arithmetic/validation is
  // performed here); the caller resolves/validates/executes them.
  std::optional<AddressRegister> address_register_write;
  M68kMemoryAccessWidth operand_size{M68kMemoryAccessWidth::long_word};
  std::optional<M68kEffectiveAddress> resolved_source_ea;
  std::optional<M68kEffectiveAddress> resolved_destination_ea;
  // MOVE An,USP's source register; runtime storage remains adapter-owned.
  std::optional<AddressRegister> user_stack_pointer_source;
  // SEG-007-T222 / ADR-0037: additive, defaulted-false fields describing
  // that this operation MAY (not always) raise a synchronous CPU exception
  // instead of completing its normal destination write -- set only for
  // DIVS.W/DIVU.W (`exception_vector = 5`). Every existing consumer of this
  // struct ignores these fields safely (they default false/0); the
  // conservative "Dn may be written" shape above (`resolved_destination_ea`/
  // `affects_condition_codes`) is unchanged and remains sound for every
  // inspected consumer (ADR-0037 Decision D). No new effect "kind" is added.
  bool may_raise_synchronous_exception{};
  std::uint8_t exception_vector{};
};

// Shared Z/N-only condition-code update for register-result operations (e.g.
// MOVEQ-style loads). X is preserved; V and C are cleared. Used by every
// profile-scoped executor that performs this exact update so the
// computation is implemented exactly once.
[[nodiscard]] std::uint16_t m68k_move_result_ccr(std::uint16_t status_register,
                                                   std::uint32_t result) noexcept;
[[nodiscard]] M68kSubtractionResult m68k_evaluate_subtraction(
    std::uint32_t source, std::uint32_t destination,
    M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] M68kAdditionResult m68k_evaluate_addition(
    std::uint32_t source, std::uint32_t destination,
    M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] std::uint16_t m68k_addition_ccr(
    std::uint16_t status_register, std::uint32_t source, std::uint32_t destination,
    M68kMemoryAccessWidth width) noexcept;
// SEG-021-T014: ADDX/SUBX/NEGX (NEGX = subtract with destination 0). Reads X from `status_register`.
[[nodiscard]] M68kExtendedArithmeticResult m68k_evaluate_extended_arithmetic(
    M68kExtendedArithmeticKind kind, std::uint32_t source, std::uint32_t destination, bool extend,
    M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] std::uint16_t m68k_extended_arithmetic_ccr(
    std::uint16_t status_register, M68kExtendedArithmeticKind kind, std::uint32_t source,
    std::uint32_t destination, M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] M68kLogicalResult m68k_evaluate_logical(std::uint32_t result,
                                                        M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] std::uint16_t m68k_logical_ccr(std::uint16_t status_register, std::uint32_t result,
                                             M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] M68kBitOperationResult m68k_evaluate_bit_operation(
    M68kBitOperationKind kind, std::uint32_t original, std::uint32_t bit_number,
    M68kMemoryAccessWidth destination_width) noexcept;
// Shared bit-test Z-only condition-code update, used identically by
// BTST/BCHG/BCLR/BSET: Z=1 iff the tested bit was originally clear; every
// other CCR/SR bit (X, N, V, C, and all upper SR bits) is preserved
// unconditionally. This is deliberately NOT the same rule as
// m68k_move_result_ccr or m68k_logical_ccr, both of which also update N
// and/or clear V/C -- reusing either here would be incorrect.
[[nodiscard]] std::uint16_t m68k_bit_test_ccr(std::uint16_t status_register,
                                                bool original_bit_set) noexcept;
// SEG-007-T025 (Batch C, C4): the one shared condition-code owner for Bcc
// and DBcc, evaluated from the CURRENT status register. No condition ever
// consults X.
[[nodiscard]] bool m68k_evaluate_condition(M68kCondition condition, std::uint16_t status_register) noexcept;
// The exact same condition rule lowered to a C11 boolean expression string
// reading `status_register` (a caller-supplied C identifier); the sole
// generated-C consumer of the condition truth table, so it can never drift
// from m68k_evaluate_condition above.
[[nodiscard]] std::string m68k_condition_c_expr(M68kCondition condition, std::string_view status_register);
// The one shared MC68000 branch-displacement target formula, used
// identically by BRA/Bcc/BSR/DBcc and by the pre-existing direct_flow
// BNE.short/BRA.short compatibility route: the displacement is always
// relative to the address immediately after the PRIMARY word
// (source_address + 2), regardless of byte or word displacement width --
// never `source + total_instruction_length + displacement`.
// `signed_displacement` is the already sign-extended 8-bit or 16-bit value.
[[nodiscard]] std::uint32_t m68k_branch_target(std::uint32_t source_address,
                                                 std::int32_t signed_displacement) noexcept;
[[nodiscard]] M68kDbccDecrementResult m68k_evaluate_dbcc_decrement(std::uint32_t original) noexcept;
// SEG-007-T025 (Batch C, C5a/C5b): the one shared MOVEM register-list/
// transfer-order owner. Given a raw 16-bit register mask and the selected
// transfer-order shape, returns the ordered sequence of selected
// architectural register indices (0-7 = D0-D7, 8-15 = A0-A7) in exactly
// that shape's order.
[[nodiscard]] std::vector<std::uint8_t> m68k_movem_transfer_order(std::uint16_t register_mask,
                                                                    M68kMovemTransferOrder order);
[[nodiscard]] M68kShiftRotateResult m68k_evaluate_shift_rotate(
    M68kShiftRotateKind kind, std::uint32_t original, std::uint32_t count,
    M68kMemoryAccessWidth width, bool original_extend) noexcept;
// The shared CCR/SR application for every register shift/rotate result:
// N/Z/V/C always take the result's own facts; X is overwritten with
// `result.extend_value` when `result.extend_written` is true, and otherwise
// preserved unconditionally.
[[nodiscard]] std::uint16_t m68k_shift_rotate_ccr(std::uint16_t status_register,
                                                    const M68kShiftRotateResult &result) noexcept;
// Applies an instruction-selected X policy to shared sized subtraction facts.
// It updates N/Z/V/C and preserves every non-CCR SR bit. `source` and
// `destination` are truncated to `width`; M68kSubtractionResult remains the
// independent arithmetic result without an X-policy decision.
[[nodiscard]] std::uint16_t m68k_subtraction_ccr(
    std::uint16_t status_register, std::uint32_t source, std::uint32_t destination,
    M68kMemoryAccessWidth width, M68kExtendFlagPolicy extend_flag_policy) noexcept;
// M68000PM/AD Rev. 1, §2 and §4, CMP/CMPI/CMPA instruction entries. CMP's
// instruction-level effect explicitly selects X preservation.
[[nodiscard]] std::uint16_t m68k_compare_ccr(std::uint16_t status_register,
                                               std::uint32_t source, std::uint32_t destination,
                                               M68kMemoryAccessWidth width) noexcept;
[[nodiscard]] M68kOperationEffect m68k_operation_effect(const M68kIrOperation &operation) noexcept;

} // namespace segarecomp
