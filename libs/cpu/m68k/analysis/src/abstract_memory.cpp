// SEG-030-T004 (ADR 0079, report-only). See abstract_memory.hpp.

#include "segarecomp/cpu/m68k/analysis/abstract_memory.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <utility>

namespace segarecomp {
namespace {

using analysis::FiniteValue;
using analysis::UnknownReason;
using Sub = M68kAnalysisSubReason;

std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

// The order of the `absent` annotation (a chain, so the join is the larger): none < initial_memory < store_poison < set_bound.
int absent_rank(Sub sub) {
  switch (sub) {
  case Sub::none: return 0;
  case Sub::initial_memory: return 1;
  case Sub::store_poison: return 2;
  default: return 3;
  }
}
Sub larger(Sub left, Sub right) { return absent_rank(left) >= absent_rank(right) ? left : right; }

std::uint64_t width_mask(std::uint32_t width) { return width >= 4U ? UINT64_C(0xFFFFFFFF) : (UINT64_C(1) << (8U * width)) - 1U; }

// A cell value as plain data (an exact pointer's 32-bit register values), or nullopt.
std::optional<M68kCellValue> as_data(const M68kCellValue &value, std::uint32_t width) {
  if (!value.is_pointer()) return value;
  const auto values = value.pointer.values();
  if (!values) return std::nullopt;
  std::vector<std::uint64_t> data;
  for (const auto v : *values) data.push_back(v & width_mask(width));
  M68kCellValue out;
  out.data = FiniteValue::of(std::move(data));
  out.width_derived = value.pointer.width_derived;
  if (!out.data.is_precise()) return std::nullopt;
  return out;
}

std::uint32_t physical(const M68kRegion &region, std::uint32_t offset) {
  return region.mirror != 0U ? offset % region.mirror : offset;
}

// The physical extent of a tracked region (the mirror period, or the region size).
std::uint32_t physical_extent(const M68kRegion &region) { return region.mirror != 0U ? region.mirror : region.size; }

// Iterates the cells of (kind, id) whose bytes overlap [lo, hi).
template <typename F>
void for_overlapping(std::map<M68kCell, M68kCellValue> &cells, M68kRegionKind kind, std::uint32_t id, std::uint64_t lo,
                     std::uint64_t hi, F &&visit) {
  const auto start = static_cast<std::uint32_t>(lo >= 3U ? lo - 3U : 0U);  // a cell is at most 4 bytes wide
  for (auto it = cells.lower_bound(M68kCell{kind, id, start, 0U}); it != cells.end();) {
    const auto &cell = it->first;
    if (cell.kind != kind || cell.id != id || cell.offset >= hi) break;
    if (static_cast<std::uint64_t>(cell.offset) + cell.width <= lo) {
      ++it;
      continue;
    }
    it = visit(it);
  }
}

// Physical store targets of one region.
struct ExactTarget {
  M68kRegionKind kind{};
  std::uint32_t id{};
  std::uint32_t offset{};
  friend auto operator<=>(const ExactTarget &, const ExactTarget &) = default;
};
struct StridedTarget {
  M68kRegionKind kind{};
  std::uint32_t id{};
  std::uint32_t lo{}, stride{}, hi{};
};

void remove_region(std::map<M68kCell, M68kCellValue> &cells, M68kRegionKind kind, std::uint32_t id) {
  for (auto it = cells.lower_bound(M68kCell{kind, id, 0U, 0U}); it != cells.end() && it->first.kind == kind && it->first.id == id;)
    it = cells.erase(it);
}

}  // namespace

UnknownReason m68k_memory_generic_reason(M68kAnalysisSubReason sub) noexcept {
  switch (sub) {
  case Sub::initial_memory:
  case Sub::async_writer:
  case Sub::external_writer: return UnknownReason::unknown_input;
  case Sub::set_bound:
  case Sub::context_bound: return UnknownReason::state_bound;  // SEG-030-T005: a merged or recursive activation's continuation
  default: return UnknownReason::unsupported_transfer;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Policy.

bool M68kMemoryPolicy::asynchronous(const M68kCell &cell) const noexcept {
  if (async_all) return true;
  const std::uint64_t lo = cell.offset, hi = static_cast<std::uint64_t>(cell.offset) + cell.width;
  return std::any_of(async.begin(), async.end(), [&](const M68kAsyncRange &range) {
    return range.kind == cell.kind && range.id == cell.id && range.lo < hi && lo < range.hi;
  });
}

void M68kMemoryPolicy::add_async(const M68kAsyncRange &range) {
  if (range.lo >= range.hi) return;
  async.push_back(range);
  std::sort(async.begin(), async.end());
  std::vector<M68kAsyncRange> merged;
  for (const auto &next : async) {
    if (!merged.empty() && merged.back().kind == next.kind && merged.back().id == next.id && next.lo <= merged.back().hi)
      merged.back().hi = std::max(merged.back().hi, next.hi);
    else merged.push_back(next);
  }
  async = std::move(merged);
}

M68kMemoryPolicy join(const M68kMemoryPolicy &left, const M68kMemoryPolicy &right) {
  M68kMemoryPolicy out = left;
  out.external_writer = left.external_writer || right.external_writer;
  out.async_all = left.async_all || right.async_all;
  for (const auto &range : right.async) out.add_async(range);
  return out;
}

bool leq(const M68kMemoryPolicy &left, const M68kMemoryPolicy &right) {
  if (left.external_writer && !right.external_writer) return false;
  if (left.async_all && !right.async_all) return false;
  if (right.async_all) return true;
  return std::all_of(left.async.begin(), left.async.end(), [&](const M68kAsyncRange &range) {
    return std::any_of(right.async.begin(), right.async.end(), [&](const M68kAsyncRange &cover) {
      return cover.kind == range.kind && cover.id == range.id && cover.lo <= range.lo && range.hi <= cover.hi;
    });
  });
}

// ---------------------------------------------------------------------------------------------------------------
// Lattice.

std::optional<M68kCellValue> m68k_cell_join(const M68kCellValue &left, const M68kCellValue &right, std::uint32_t width) {
  if (left.is_pointer() && right.is_pointer()) {
    M68kCellValue out;
    out.pointer = join(left.pointer, right.pointer);
    if (!out.pointer.is_known()) return std::nullopt;
    return out;
  }
  const auto a = as_data(left, width);
  const auto b = as_data(right, width);
  if (!a || !b || !a->data.is_precise() || !b->data.is_precise()) return std::nullopt;
  M68kCellValue out;
  out.data = join(a->data, b->data);
  if (!out.data.is_precise()) return std::nullopt;
  out.width_derived = a->width_derived || b->width_derived;
  return out;
}

namespace {
// SEG-030-T008: slots are kept only when both sides recorded them (a path that never pushed proves nothing about the cell).
std::map<M68kCell, M68kReturnSlot> join_slots(const std::map<M68kCell, M68kReturnSlot> &left,
                                              const std::map<M68kCell, M68kReturnSlot> &right) {
  std::map<M68kCell, M68kReturnSlot> out;
  auto a = left.begin();
  auto b = right.begin();
  while (a != left.end() && b != right.end()) {
    if (a->first < b->first) ++a;
    else if (b->first < a->first) ++b;
    else {
      M68kReturnSlot slot;
      slot.pushed = join(a->second.pushed, b->second.pushed);
      slot.rewritten = a->second.rewritten || b->second.rewritten;
      slot.unknown_store = a->second.unknown_store || b->second.unknown_store;
      if (slot.pushed.is_precise()) out.emplace_hint(out.end(), a->first, std::move(slot));
      ++a;
      ++b;
    }
  }
  return out;
}

bool leq_slots(const std::map<M68kCell, M68kReturnSlot> &left, const std::map<M68kCell, M68kReturnSlot> &right) {
  for (const auto &[cell, slot] : right) {
    const auto found = left.find(cell);
    if (found == left.end()) return false;
    if (!leq(found->second.pushed, slot.pushed)) return false;
    if (found->second.rewritten && !slot.rewritten) return false;
    if (found->second.unknown_store && !slot.unknown_store) return false;
  }
  return true;
}
}  // namespace

M68kAbstractMemory join(const M68kAbstractMemory &left, const M68kAbstractMemory &right) {
  M68kAbstractMemory out;
  out.absent = larger(left.absent, right.absent);
  out.slots = join_slots(left.slots, right.slots);
  auto a = left.cells.begin();
  auto b = right.cells.begin();
  while (a != left.cells.end() && b != right.cells.end()) {
    if (a->first < b->first) ++a;
    else if (b->first < a->first) ++b;
    else {
      if (a->second == b->second) out.cells.emplace_hint(out.cells.end(), a->first, a->second);
      else if (auto joined = m68k_cell_join(a->second, b->second, a->first.width)) out.cells.emplace_hint(out.cells.end(), a->first, *joined);
      ++a;
      ++b;
    }
  }
  return out;
}

bool leq(const M68kAbstractMemory &left, const M68kAbstractMemory &right) {
  if (larger(left.absent, right.absent) != right.absent) return false;
  if (!leq_slots(left.slots, right.slots)) return false;
  for (const auto &[cell, value] : right.cells) {
    const auto found = left.cells.find(cell);
    if (found == left.cells.end()) return false;  // absent (Unknown) is not below a known cell
    if (found->second == value) continue;
    if (found->second.is_pointer() && value.is_pointer()) {
      if (!leq(found->second.pointer, value.pointer)) return false;
      continue;
    }
    const auto joined = m68k_cell_join(found->second, value, cell.width);
    if (!joined || !(*joined == value)) return false;
  }
  return true;
}

void M68kAbstractMemory::poison_all(M68kAnalysisSubReason sub) {
  cells.clear();
  absent = larger(absent, sub);
}

std::string M68kAbstractMemory::describe() const {
  std::string out = "mem[";
  out += m68k_analysis_sub_reason_name(absent);
  for (const auto &[cell, value] : cells) {
    out += ' ' + std::string(m68k_region_kind_name(cell.kind)) + '#' + std::to_string(cell.id) + '+' + hex(cell.offset) + '/' +
           std::to_string(cell.width) + '=';
    out += value.is_pointer() ? value.pointer.describe() : value.data.describe() + (value.width_derived ? "~" : "");
  }
  for (const auto &[cell, slot] : slots)
    out += " slot#" + std::to_string(cell.id) + '+' + hex(cell.offset) + '=' + slot.pushed.describe() + (slot.rewritten ? "!" : "") +
           (slot.unknown_store ? "?" : "");
  return out + ']';
}

// ---------------------------------------------------------------------------------------------------------------
// Store / read.

void m68k_memory_store(M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t span,
                       const std::optional<M68kCellValue> &value, const M68kMemoryPolicy &policy) {
  if (targets.is_bottom() || span == 0U) return;
  if (!targets.is_known()) {
    memory.poison_all();
    return;
  }
  std::vector<ExactTarget> exact;
  std::vector<StridedTarget> strided;
  std::vector<std::pair<M68kRegionKind, std::uint32_t>> whole;
  std::size_t untracked = 0U;
  for (const auto &[region, offsets] : targets.pairs) {
    const bool tracked = m68k_memory_tracked(region.kind);
    // A store spilling past the region end may reach the next bus region: fail closed.
    if (static_cast<std::uint64_t>(offsets.hi()) + span > region.size) {
      memory.poison_all();
      return;
    }
    if (!tracked) {
      untracked += offsets.is_strided() ? 2U : offsets.exact().size();
      continue;
    }
    const auto extent = physical_extent(region);
    if (offsets.is_strided()) {
      const auto lo = physical(region, offsets.lo());
      if (static_cast<std::uint64_t>(offsets.hi()) - offsets.lo() + span > extent || lo + (offsets.hi() - offsets.lo()) + span > extent)
        whole.emplace_back(region.kind, region.id);  // wraps the mirror: every physical byte may be hit
      else strided.push_back({region.kind, region.id, lo, offsets.stride(), lo + (offsets.hi() - offsets.lo())});
      continue;
    }
    for (const auto offset : offsets.exact()) {
      const auto at = physical(region, offset);
      if (static_cast<std::uint64_t>(at) + span > extent) whole.emplace_back(region.kind, region.id);
      else exact.push_back({region.kind, region.id, at});
    }
  }
  std::sort(exact.begin(), exact.end());
  exact.erase(std::unique(exact.begin(), exact.end()), exact.end());
  const bool strong = exact.size() + untracked == 1U && strided.empty() && whole.empty();
  const bool storable = value && (span == 1U || span == 2U || span == 4U) && (!value->is_pointer() || span == 4U);
  for (const auto &[kind, id] : whole) remove_region(memory.cells, kind, id);
  if (!whole.empty()) memory.absent = larger(memory.absent, Sub::store_poison);
  for (const auto &target : exact) {
    const M68kCell cell{target.kind, target.id, target.offset, span};
    for_overlapping(memory.cells, target.kind, target.id, target.offset, static_cast<std::uint64_t>(target.offset) + span,
                    [&](auto it) {
                      if (!strong && storable && it->first == cell) {
                        auto joined = m68k_cell_join(it->second, *value, span);
                        if (joined) {
                          it->second = std::move(*joined);
                          return std::next(it);
                        }
                      }
                      return memory.cells.erase(it);
                    });
    if (strong && storable && !policy.external_writer && !policy.asynchronous(cell)) memory.cells[cell] = *value;
  }
  for (const auto &target : strided) {
    for_overlapping(memory.cells, target.kind, target.id, target.lo, static_cast<std::uint64_t>(target.hi) + span, [&](auto it) {
      const auto &cell = it->first;
      const bool member = cell.width == span && cell.offset >= target.lo && cell.offset <= target.hi &&
                          (cell.offset - target.lo) % target.stride == 0U;
      if (member && storable) {
        auto joined = m68k_cell_join(it->second, *value, span);
        if (joined) {
          it->second = std::move(*joined);
          return std::next(it);
        }
      }
      return memory.cells.erase(it);
    });
  }
  if (memory.cells.size() > m68k_memory_cell_bound) memory.poison_all(Sub::set_bound);
}

M68kMemoryRead m68k_memory_read(const M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t width,
                                const M68kMemoryPolicy &policy) {
  M68kMemoryRead out;
  const auto fail = [&](Sub sub, std::optional<UnknownReason> reason = std::nullopt) {
    M68kMemoryRead failed;
    failed.sub = sub;
    failed.reason = reason ? *reason : m68k_memory_generic_reason(sub);
    return failed;
  };
  if (targets.is_unknown()) return fail(targets.sub == Sub::none ? Sub::base_unknown : targets.sub, targets.reason);
  if (!targets.is_known()) return fail(Sub::none, UnknownReason::unsupported_transfer);
  const auto absent = memory.absent == Sub::none ? Sub::initial_memory : memory.absent;
  std::optional<M68kCellValue> result;
  const auto take = [&](const M68kCell &cell) -> std::optional<M68kMemoryRead> {
    if (policy.external_writer) return fail(Sub::external_writer);
    if (policy.asynchronous(cell)) return fail(Sub::async_writer);
    const auto found = memory.cells.find(cell);
    if (found == memory.cells.end()) return fail(absent);
    if (!result) result = found->second;
    else {
      result = m68k_cell_join(*result, found->second, width);
      if (!result) return fail(Sub::set_bound, UnknownReason::set_bound);
    }
    return std::nullopt;
  };
  for (const auto &[region, offsets] : targets.pairs) {
    if (!m68k_memory_tracked(region.kind)) return fail(Sub::none, UnknownReason::unsupported_transfer);
    if (static_cast<std::uint64_t>(offsets.hi()) + width > region.size) return fail(Sub::region_exit);
    const auto extent = physical_extent(region);
    std::vector<std::uint32_t> members;
    if (offsets.is_strided()) {
      // A summary over the stride: Unknown unless every member was written (more members than cells cannot all be).
      if (offsets.count() > m68k_memory_cell_bound) {
        if (policy.external_writer) return fail(Sub::external_writer);
        return fail(absent);
      }
      for (std::uint64_t at = offsets.lo(); at <= offsets.hi(); at += offsets.stride()) members.push_back(static_cast<std::uint32_t>(at));
    } else {
      members = offsets.exact();
    }
    for (const auto offset : members) {
      const auto at = physical(region, offset);
      if (static_cast<std::uint64_t>(at) + width > extent) return fail(Sub::region_exit);
      if (auto failed = take(M68kCell{region.kind, region.id, at, width})) return *failed;
    }
  }
  if (!result) return fail(Sub::none, UnknownReason::unsupported_transfer);
  out.known = true;
  out.value = std::move(*result);
  return out;
}

void m68k_return_slots_unknown_store(M68kAbstractMemory &memory) {
  for (auto &[cell, slot] : memory.slots) slot.unknown_store = true;
}

void m68k_return_slots_store(M68kAbstractMemory &memory, const M68kPointsTo &targets, std::uint32_t span,
                             std::optional<std::uint32_t> return_address) {
  if (targets.is_bottom() || span == 0U) return;
  if (!targets.is_known()) {
    m68k_return_slots_unknown_store(memory);
    return;
  }
  // The physical byte ranges the store may touch (nullopt: it may spill past a region end into any byte).
  const auto touched = m68k_memory_touched(targets, span);
  if (!touched) {
    for (auto &[cell, slot] : memory.slots) slot.rewritten = true;
    return;
  }
  std::optional<M68kCell> exact;
  if (return_address && span == 4U && targets.is_exact() && targets.pairs.size() == 1U && targets.pairs.front().second.exact().size() == 1U &&
      m68k_memory_tracked(targets.pairs.front().first.kind)) {
    const auto &region = targets.pairs.front().first;
    const auto at = physical(region, targets.pairs.front().second.exact().front());
    if (static_cast<std::uint64_t>(at) + span <= physical_extent(region)) exact = M68kCell{region.kind, region.id, at, 4U};
  }
  for (auto &[cell, slot] : memory.slots) {
    if (exact && cell == *exact) continue;
    const std::uint64_t lo = cell.offset, hi = static_cast<std::uint64_t>(cell.offset) + cell.width;
    for (const auto &range : *touched)
      if (range.kind == cell.kind && range.id == cell.id && range.lo < hi && lo < range.hi) slot.rewritten = true;
  }
  if (!exact) return;
  memory.slots[*exact] = M68kReturnSlot{FiniteValue::of({*return_address}), false, false};
  if (memory.slots.size() > m68k_memory_cell_bound) memory.slots.clear();
}

std::optional<std::vector<M68kAsyncRange>> m68k_memory_touched(const M68kPointsTo &targets, std::uint32_t span) {
  if (targets.is_bottom()) return std::vector<M68kAsyncRange>{};
  if (!targets.is_known()) return std::nullopt;
  std::vector<M68kAsyncRange> out;
  for (const auto &[region, offsets] : targets.pairs) {
    if (static_cast<std::uint64_t>(offsets.hi()) + span > region.size) return std::nullopt;  // may spill into the next region
    if (!m68k_memory_tracked(region.kind)) continue;
    const auto extent = physical_extent(region);
    const auto lo = physical(region, offsets.lo());
    const std::uint64_t length = static_cast<std::uint64_t>(offsets.hi()) - offsets.lo() + span;
    if (offsets.is_strided() || offsets.exact().size() > 1U) {
      if (lo + length > extent) out.push_back({region.kind, region.id, 0U, extent});
      else out.push_back({region.kind, region.id, lo, static_cast<std::uint32_t>(lo + length)});
      continue;
    }
    if (lo + span > extent) out.push_back({region.kind, region.id, 0U, extent});
    else out.push_back({region.kind, region.id, lo, lo + span});
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Writer description.

bool m68k_memory_mode(M68kEaMode mode) noexcept {
  switch (mode) {
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc:
  case M68kEaMode::address_predec:
  case M68kEaMode::address_disp16:
  case M68kEaMode::address_index8:
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long:
  case M68kEaMode::pc_disp16:
  case M68kEaMode::pc_index8: return true;
  default: return false;
  }
}

M68kMemoryWrites m68k_memory_writes(const M68kIrOperation &operation) {
  using Target = M68kMemoryWrite::Target;
  using Value = M68kMemoryWrite::Value;
  M68kMemoryWrites out;
  out.described = true;
  const auto size = static_cast<std::uint32_t>(operation.size);
  const bool memory_destination = m68k_memory_mode(operation.destination_ea.mode);
  const auto destination = [&](std::uint32_t span, Value value) {
    if (memory_destination) out.writes.push_back({Target::destination, span, value});
  };
  switch (operation.kind) {
  // No memory write on a normal successor (register, flag or control effects only; reads only).
  case M68kIrKind::write_moveq:
  case M68kIrKind::subtract_quick_long_d0:
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
  case M68kIrKind::general_branch:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::return_from_subroutine:
  case M68kIrKind::return_from_exception:
  case M68kIrKind::return_restore_condition_codes:
  case M68kIrKind::test_operand:
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
  case M68kIrKind::compare_memory:
  case M68kIrKind::add_address:
  case M68kIrKind::subtract_address:
  case M68kIrKind::write_movea:
  case M68kIrKind::load_effective_address:
  case M68kIrKind::jump_general:
  case M68kIrKind::write_swap:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
  case M68kIrKind::unlink_frame:
  case M68kIrKind::bit_test:
  case M68kIrKind::shift_rotate_register:
  case M68kIrKind::write_user_stack_pointer:
  case M68kIrKind::read_user_stack_pointer:
  case M68kIrKind::logical_immediate_to_ccr:
  case M68kIrKind::logical_immediate_to_sr:
  case M68kIrKind::write_status_register:
  case M68kIrKind::write_condition_codes:
  case M68kIrKind::no_operation:
  case M68kIrKind::exchange_registers:
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word: return out;
  // MOVE / CLR: an exact value of the operation size.
  case M68kIrKind::write_move: destination(size, Value::source); return out;
  case M68kIrKind::write_clr: destination(size, Value::zero); return out;
  // Sized read-modify-write forms with a memory destination: an Unknown value of the operation size.
  case M68kIrKind::add:
  case M68kIrKind::add_immediate:
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::subtract_quick:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
  case M68kIrKind::logical_not:
  case M68kIrKind::negate_word:
  case M68kIrKind::negate_extended:
  case M68kIrKind::add_extended:
  case M68kIrKind::subtract_extended: destination(size, Value::unknown); return out;
  // Byte-only memory forms (ABCD/SBCD/NBCD, Scc, TAS, BCHG/BCLR/BSET to memory).
  case M68kIrKind::add_decimal:
  case M68kIrKind::subtract_decimal:
  case M68kIrKind::negate_decimal:
  case M68kIrKind::set_conditional:
  case M68kIrKind::test_and_set:
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set: destination(1U, Value::unknown); return out;
  // Word-only memory forms (memory shifts/rotates, MOVE from SR).
  case M68kIrKind::shift_rotate_memory:
  case M68kIrKind::read_status_register: destination(2U, Value::unknown); return out;
  // MOVEP Dn,d16(An): every second byte of `2 * size` bytes.
  case M68kIrKind::movep_transfer: destination(2U * size, Value::unknown); return out;
  // MOVEM registers to memory: `count * size` bytes (below An for -(An)).
  case M68kIrKind::movem_transfer:
    if (operation.movem_direction == M68kMovemDirection::registers_to_memory) {
      const auto count = static_cast<std::uint32_t>(__builtin_popcount(operation.movem_register_mask));
      destination(count * size, Value::unknown);
    }
    return out;
  // Implicit pushes below A7.
  case M68kIrKind::call_general:
  case M68kIrKind::bsr_call: out.writes.push_back({Target::push, 4U, Value::return_address}); return out;
  case M68kIrKind::push_effective_address: out.writes.push_back({Target::push, 4U, Value::effective_address}); return out;
  case M68kIrKind::link_frame: out.writes.push_back({Target::push, 4U, Value::unknown}); return out;
  // An exception frame (SR + PC) on the supervisor stack, whose address the analysis does not track (the supervisor mode is not
  // proven); DIVx/CHK/TRAPV and STOP resume at their fallthrough after the handler returns.
  case M68kIrKind::trap_exception:
  case M68kIrKind::trap_on_overflow:
  case M68kIrKind::check_bounds:
  case M68kIrKind::instruction_exception:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word:
  case M68kIrKind::stop_until_interrupt: out.writes.push_back({Target::unknown, 6U, Value::unknown}); return out;
  }
  out.described = false;
  return out;
}

}  // namespace segarecomp
