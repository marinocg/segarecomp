#pragma once

// SEG-030-T004 (ADR 0079 decisions 4, 5, 7, 11; report-only): the CPU-owned M68K abstract memory.
//
// A cell is one physical location of a mutable machine region (mutable RAM; the I/O/device window and the immutable images are never
// cells): region kind and identity, the physical offset (the region offset modulo its mirror size) and an access width of 1, 2 or 4
// bytes. A cell holds a precise finite value of that width or a points-to value (a long cell written from an address register). An
// absent cell is Unknown (top); `absent` records why (initial memory, a poisoning store, the cell bound). The join keeps only the
// cells present on both sides and joins them pointwise, so a cell is known only when every path wrote it.
//
// Stores (`m68k_memory_store`): a target that is one physical cell is a strong update; any other known target is a weak update (an
// exactly matching cell is joined, every other overlapping cell is removed, an absent cell stays absent); a strided target updates
// every congruent cell weakly (the summary over the stride, never enumerated); an Unknown target removes every cell (`store_poison`).
// A target that crosses a mirror boundary or a region end removes the whole region. More than `m68k_memory_cell_bound` cells drop
// every cell (`set_bound`, generic `state_bound`). Alias exclusion is structural: a store only touches the cells its target set
// overlaps, so a push at a known A7 offset or a store to a distinct exact field leaves every disjoint cell intact.
//
// The policy (`M68kMemoryPolicy`, ADR 0079 decision 7) removes cells the analysed flow cannot own: with an external writer every
// work-RAM read is Unknown(`external_writer`); a cell overlapping an asynchronous-writer range (or every cell, `async_all`) is never
// stored and reads Unknown(`async_writer`).
//
// The writer description (`m68k_memory_writes`) states, from the M68000PRM instruction entries, what each lifted kind writes to
// memory on its normal successors. A kind it does not describe is an undescribed writer: the caller poisons every cell.
//
// The generic core is unchanged: sub-reasons are CPU-owned and each Unknown also carries one of the six generic reasons.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"
#include "segarecomp/cpu/m68k/analysis/address_value.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

// ADR 0079 decision 11.
inline constexpr std::size_t m68k_memory_cell_bound = 512U;

// The generic reason of a memory sub-reason.
[[nodiscard]] analysis::UnknownReason m68k_memory_generic_reason(M68kAnalysisSubReason sub) noexcept;

// True for a region kind whose locations are abstract-memory cells (mutable RAM only).
[[nodiscard]] constexpr bool m68k_memory_tracked(M68kRegionKind kind) noexcept { return kind == M68kRegionKind::mutable_ram; }

struct M68kCell {
  M68kRegionKind kind{M68kRegionKind::mutable_ram};
  std::uint32_t id{};
  std::uint32_t offset{};  // physical offset (region offset modulo the mirror)
  std::uint32_t width{};   // 1, 2 or 4 bytes
  friend auto operator<=>(const M68kCell &, const M68kCell &) = default;
};

struct M68kCellValue {
  analysis::FiniteValue data;  // precise (values < 2^(8 * width)) unless `pointer` is known
  bool width_derived{};        // the data set is only an operand-width bound (M68kFiniteValues::width_derived)
  M68kPointsTo pointer;        // known: the cell holds this address-register value (long cells only)

  [[nodiscard]] bool is_pointer() const noexcept { return pointer.is_known(); }
  friend bool operator==(const M68kCellValue &, const M68kCellValue &) = default;
};

// Asynchronous-writer range of one tracked region: physical offsets [lo, hi).
struct M68kAsyncRange {
  M68kRegionKind kind{M68kRegionKind::mutable_ram};
  std::uint32_t id{};
  std::uint32_t lo{};
  std::uint32_t hi{};
  friend auto operator<=>(const M68kAsyncRange &, const M68kAsyncRange &) = default;
};

// ADR 0079 decision 7: what the analysed flow cannot own. Monotone configuration of the driver rounds (decision 9).
struct M68kMemoryPolicy {
  bool external_writer{};               // another bus master may write all of mutable RAM at any point
  bool async_all{};                     // every tracked cell has an asynchronous writer
  std::vector<M68kAsyncRange> async;    // sorted, merged

  [[nodiscard]] bool asynchronous(const M68kCell &cell) const noexcept;
  // Adds a range (keeps `async` sorted and merged).
  void add_async(const M68kAsyncRange &range);
  // Least upper bound / order of configurations.
  friend M68kMemoryPolicy join(const M68kMemoryPolicy &left, const M68kMemoryPolicy &right);
  friend bool leq(const M68kMemoryPolicy &left, const M68kMemoryPolicy &right);
  friend bool operator==(const M68kMemoryPolicy &, const M68kMemoryPolicy &) = default;
};

// SEG-030-T008 (ADR 0079 decision 8, the return-slot integrity premise): a return slot is the long cell a call (JSR/BSR) pushed its
// return address into. It is recorded whether or not the policy lets the cell hold the value, so that every later store can be
// related to it: a store with a known target that may touch a byte of it marks it `rewritten` (precise or weak knowledge of a
// contradicting write), a store with an Unknown target (or an undescribed writer) marks it `unknown_store` (the premise).
struct M68kReturnSlot {
  analysis::FiniteValue pushed;  // the return addresses pushed into it (precise)
  bool rewritten{};
  bool unknown_store{};
  friend bool operator==(const M68kReturnSlot &, const M68kReturnSlot &) = default;
};

class M68kAbstractMemory {
public:
  std::map<M68kCell, M68kCellValue> cells;
  // Why an absent cell is Unknown (none while the memory domain is off; join keeps the larger).
  M68kAnalysisSubReason absent{M68kAnalysisSubReason::none};
  // SEG-030-T008: the return slots recorded on every path (the join keeps the slots of both sides, pointwise). At most
  // `m68k_memory_cell_bound`; beyond, every slot is forgotten (an unrecorded slot is never proven intact).
  std::map<M68kCell, M68kReturnSlot> slots;

  friend M68kAbstractMemory join(const M68kAbstractMemory &left, const M68kAbstractMemory &right);
  friend bool leq(const M68kAbstractMemory &left, const M68kAbstractMemory &right);
  friend bool operator==(const M68kAbstractMemory &, const M68kAbstractMemory &) = default;

  // Removes every cell (an Unknown-target or undescribed store).
  void poison_all(M68kAnalysisSubReason sub = M68kAnalysisSubReason::store_poison);
  [[nodiscard]] std::string describe() const;
};

// The value read from memory, or Unknown with a generic reason and a CPU sub-reason.
struct M68kMemoryRead {
  bool known{};
  M68kCellValue value;
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};
};

// Stores `value` (nullopt: an Unknown value) of `span` bytes at every address of `targets`. `value` is kept only when the target is
// precise enough and `span` is 1, 2 or 4; every other overlapped cell is removed.
void m68k_memory_store(M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t span,
                       const std::optional<M68kCellValue> &value, const M68kMemoryPolicy &policy);

// Reads `width` bytes at every address of the tracked regions of `targets` (`targets` must name tracked regions only; the caller
// reads immutable images itself). Unknown when any address is absent, asynchronous, externally written or a strided summary
// member was never written.
[[nodiscard]] M68kMemoryRead m68k_memory_read(const M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t width,
                                              const M68kMemoryPolicy &policy);

// The physical byte ranges [lo, hi) a store of `span` bytes at `targets` may touch in tracked regions, or nullopt when `targets` is
// Unknown (the store may touch any byte). Used for asynchronous-writer ranges.
[[nodiscard]] std::optional<std::vector<M68kAsyncRange>> m68k_memory_touched(const M68kPointsTo &targets, std::uint32_t span);

// SEG-030-T008: relates one store of `span` bytes at `targets` to the recorded return slots. `return_address` is set for the push of a
// call (JSR/BSR): a push at one exact tracked cell records (or replaces) that slot. Every other store with a known target marks the
// slots it may touch `rewritten` (a target that may spill past its region marks every slot); a store with an Unknown target marks
// every slot `unknown_store`.
void m68k_return_slots_store(M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t span,
                             std::optional<std::uint32_t> return_address);
// SEG-030-T008: an undescribed writer (it may write any byte): every slot `unknown_store`.
void m68k_return_slots_unknown_store(M68kAbstractMemory &memory);

// Joins two read values of the same width (nullopt: the join is Unknown).
[[nodiscard]] std::optional<M68kCellValue> m68k_cell_join(const M68kCellValue &left, const M68kCellValue &right, std::uint32_t width);

// ---------------------------------------------------------------------------------------------------------------
// Writer description (M68000PRM; normal successors only).

struct M68kMemoryWrite {
  enum class Target : std::uint8_t {
    destination,  // the decoded destination operand (only when it is a memory mode); MOVEM -(An) spans below An
    push,         // the `span` bytes below A7 (JSR/BSR/PEA/LINK); A7 itself is updated by the address domain
    unknown,      // an address the analysis cannot name (the supervisor stack of an exception frame)
  };
  enum class Value : std::uint8_t { unknown, source, zero, return_address, effective_address };
  Target target{Target::destination};
  std::uint32_t span{};  // bytes written
  Value value{Value::unknown};
};

struct M68kMemoryWrites {
  bool described{};  // false: an undescribed writer (poisons every cell)
  std::vector<M68kMemoryWrite> writes;
};

[[nodiscard]] bool m68k_memory_mode(M68kEaMode mode) noexcept;
[[nodiscard]] M68kMemoryWrites m68k_memory_writes(const M68kIrOperation &operation);

}  // namespace segarecomp
