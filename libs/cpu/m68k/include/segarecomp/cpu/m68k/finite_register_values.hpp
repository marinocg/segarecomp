#pragma once

// SEG-026-T002 (experiment, report-only): a deliberately tiny finite value domain for one MC68000 data register,
// used only to prove the index domain of a `JMP/JSR (d8,PC,Xn)` site in the reachability challenger.
//
// A value is either Unknown or an exact finite set of register values modulo 2^width (width 16 for a `.W` index,
// 32 for a `.L` index). Every supported transfer is exact on that low-width slice: an operation whose low `width`
// result bits depend on higher input bits (a right shift/sign extension wider than the width, SWAP, rotates, an
// address-register source, ...) yields Unknown. Unknown is always sound. There is no interval widening, no loop
// reasoning, no pointer/memory model: the only memory ever read is immutable image bytes supplied by the caller,
// and a byte loaded from any other memory contributes its full 0..255 domain. A set larger than
// `m68k_finite_values_limit` becomes Unknown; the limit is a resource bound (the site stays unresolved), never
// a proof of a table extent.
//
// SEG-026-T003 (experiment, report-only) adds three caller-driven pieces and no memory model of its own: a
// `mutable_byte` hook through which a caller may supply a proven exact store domain for a mutable byte source, an
// address-register transfer for locally exact bases, and a conservative description of every memory write an
// operation performs (`m68k_memory_stores`) with the exact value it stores when that value is in this tiny domain.
// A memory operand is always read at the operation's own size and then restricted to the queried low slice.
//
// The transfer reads only the lifted operation's decoded fields and the existing semantic owners
// (`m68k_operation_effect` for the register write footprint, `m68k_evaluate_subtraction` and
// `M68kConditionSpecification` for guard filtering). It adds no decoding or execution semantics.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

inline constexpr std::size_t m68k_finite_values_limit = 4096U;

struct M68kFiniteValues {
  bool known{};                       // false: Unknown (top)
  std::vector<std::uint32_t> values;  // sorted, distinct, each < 2^width
  // True when the set's upper extent is only the width of a byte loaded from mutable memory (0..255 carried
  // through copies, loads, additions and left shifts): no explicit mask, right shift or compare guard has cut
  // its maximum. Such a domain is sound but is an operand-width bound, not an explicit index bound.
  bool width_derived{};

  [[nodiscard]] static M68kFiniteValues unknown() { return {}; }
  // Normalizes (mask, sort, dedupe); more than the limit becomes Unknown.
  [[nodiscard]] static M68kFiniteValues of(std::vector<std::uint32_t> values, unsigned width);
};

// Union; Unknown absorbs; `width_derived` if either side is.
[[nodiscard]] M68kFiniteValues m68k_finite_union(const M68kFiniteValues &left, const M68kFiniteValues &right,
                                                 unsigned width);

// Proof ingredients a transfer used (report aggregation only).
namespace m68k_finite_proof {
inline constexpr std::uint32_t constant = 1U << 0;          // MOVEQ / CLR / immediate MOVE
inline constexpr std::uint32_t mask = 1U << 1;              // AND with an immediate
inline constexpr std::uint32_t byte_load = 1U << 2;         // byte from non-immutable memory: 0..255
inline constexpr std::uint32_t immutable_load = 1U << 3;    // entry read from immutable image bytes
inline constexpr std::uint32_t shift = 1U << 4;             // LSL/ASL/LSR/ASR by an immediate count
inline constexpr std::uint32_t add_sub = 1U << 5;           // ADD/SUB (immediate, quick, register)
inline constexpr std::uint32_t sign_extend = 1U << 6;       // EXT
inline constexpr std::uint32_t guard = 1U << 7;             // CMP/CMPI/TST + Bcc edge filter
inline constexpr std::uint32_t register_copy = 1U << 8;     // MOVE Dm,Dn
inline constexpr std::uint32_t logical = 1U << 9;           // OR/EOR
inline constexpr std::uint32_t call_edge = 1U << 10;        // value carried across a direct call into a callee
inline constexpr std::uint32_t dynamic_edge = 1U << 11;     // value carried across a recovered PC-indexed edge
// SEG-026-T003: a byte read from a mutable state location whose exact store domain the caller proved
// (`M68kFiniteValueInputs::mutable_byte`).
inline constexpr std::uint32_t store_domain = 1U << 12;
inline constexpr std::uint32_t count = 13U;
[[nodiscard]] const char *name(std::uint32_t bit_index) noexcept;
}  // namespace m68k_finite_proof

class M68kFiniteValueInputs {
public:
  virtual ~M68kFiniteValueInputs() = default;
  // Value of Dn immediately before the operation, modulo 2^width.
  [[nodiscard]] virtual M68kFiniteValues data_register_before(unsigned reg, unsigned width) = 0;
  // Big-endian read of `bytes` (1, 2 or 4) at the 24-bit bus `address` from provably immutable image bytes.
  // nullopt: the bytes are not uniquely owned immutable image bytes (outside the image, a RAM alias, ...).
  [[nodiscard]] virtual std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) = 0;
  // SEG-026-T003: the exact finite domain (values 0..255) of a BYTE read from mutable memory through `ea` by the
  // current operation, when the caller proves one; nullopt keeps the width-only 0..255 rule. The domain must not
  // be `width_derived`. Default: nullopt (the SEG-026-T002 behaviour).
  [[nodiscard]] virtual std::optional<M68kFiniteValues> mutable_byte(const M68kEffectiveAddress &ea) {
    (void)ea;
    return std::nullopt;
  }
  // SEG-026-T003: value of An (32 bits) immediately before the operation. Default: Unknown.
  [[nodiscard]] virtual M68kFiniteValues address_register_before(unsigned reg) {
    (void)reg;
    return M68kFiniteValues::unknown();
  }
};

struct M68kFiniteTransfer {
  bool writes{};              // false: the operation does not write the register (value unchanged)
  M68kFiniteValues values;    // value after the operation when `writes`
  std::uint32_t proof{};      // m68k_finite_proof bits used
  bool immutable_read_failed{};  // an immutable table read left the immutable image: fail closed
  std::uint32_t table_reads{};
  std::uint32_t misaligned_reads_excluded{};  // odd word/long reads raise an address error: not a value
};

// Value of Dreg after `operation`, modulo 2^width (16 or 32). An operation with an incomplete write footprint is
// reported as writing (Unknown).
[[nodiscard]] M68kFiniteTransfer m68k_finite_register_after(const M68kIrOperation &operation, unsigned reg,
                                                            unsigned width, M68kFiniteValueInputs &inputs);

// Restricts `values` (Dreg modulo 2^width at `branch`, whose only predecessor is `flag_setter`) to the values for
// which `branch` (a conditional general_branch) takes (`taken`) or does not take its target. Returns false and
// leaves `values` unchanged when the flag setter / branch / register / width shape gives no exact filter.
[[nodiscard]] bool m68k_finite_branch_filter(const M68kIrOperation &flag_setter, const M68kIrOperation &branch,
                                             bool taken, unsigned reg, unsigned width, M68kFiniteValues &values);

// SEG-026-T003: value of An (all 32 bits) after `operation`. Exact only for LEA/MOVEA of an absolute, PC-relative
// or immediate address, LEA d16(Am) of a known Am, and ADDA/SUBA/ADDQ/SUBQ of an immediate to a known An. An
// operation stated (M68000PRM) or by a complete effect footprint not to write An leaves it unchanged; every other
// writer (auto-increment, MOVEM, EXG, loads from memory, ...) yields Unknown.
[[nodiscard]] M68kFiniteTransfer m68k_finite_address_register_after(const M68kIrOperation &operation, unsigned reg,
                                                                    M68kFiniteValueInputs &inputs);

// SEG-026-T003: every memory write an operation performs, described conservatively for store-completeness proofs.
enum class M68kStoreTarget : std::uint8_t {
  effective_address,  // `ea` (a memory-mode operand); `bytes` bytes starting at its address
  stack_push,         // an implicit push through A7 (JSR/BSR return address, PEA, LINK)
  exception_frame,    // the stacked frame of a raised exception (TRAP/TRAPV/CHK/illegal-style/divide)
  unmodeled,          // a writer whose destination this description does not model (poison)
};
struct M68kMemoryStore {
  M68kStoreTarget target{M68kStoreTarget::unmodeled};
  M68kEffectiveAddress ea{};
  unsigned bytes{};
};
[[nodiscard]] std::vector<M68kMemoryStore> m68k_memory_stores(const M68kIrOperation &operation);

// SEG-026-T003: the exact finite set of values (low `8 * store.bytes` bits, width 32) that `store` writes, or
// Unknown. Modeled: MOVE (the tiny source domain; a memory byte source goes through `mutable_byte`), CLR, the
// ADD/SUB/AND/OR/EOR immediate/quick/Dn forms on a BYTE destination (old value through `mutable_byte`), and the
// JSR/BSR/PEA pushes of a constant address. Everything else is Unknown.
[[nodiscard]] M68kFiniteTransfer m68k_finite_store_value(const M68kIrOperation &operation, const M68kMemoryStore &store,
                                                         M68kFiniteValueInputs &inputs);

// The effective address of a `(d8,PC,Xn)` operand for one exact index value (the index register modulo 2^16 for
// `.W`, sign extended; modulo 2^32 for `.L`), as a 24-bit bus address.
[[nodiscard]] std::uint32_t m68k_pc_index_address(const M68kEffectiveAddress &ea, std::uint32_t index) noexcept;

}  // namespace segarecomp
