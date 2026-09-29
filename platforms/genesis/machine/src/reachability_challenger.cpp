// SEG-026-T001 (experiment, report-only). See reachability_challenger.hpp.

#include "segarecomp/machine/genesis/reachability_challenger.hpp"

#include <algorithm>
#include <deque>
#include <tuple>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <variant>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/finite_register_values.hpp"

namespace segarecomp {
namespace {

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

struct Decoded {
  M68kIrOperation operation;
  std::uint32_t length{};
};

enum class DecodeFailure { none, odd, unmapped, rejected };

// The machine's generated-native exception model (ADR 0021 / ADR 0043): IRQ6 (vector 30) and the synchronous
// vectors the runtime can raise (5, 8, 4, 6, 7, 10, 11, 32-47). Any other vector is never delivered.
std::vector<std::uint32_t> machine_delivered_vectors() {
  std::vector<std::uint32_t> vectors{30U, 5U, 8U, 4U, 6U, 7U, 10U, 11U};
  for (std::uint32_t trap = 32U; trap <= 47U; ++trap) vectors.push_back(trap);
  return vectors;
}

// Mirrors the discovery vector rule (frontend.cpp `resolve_vector_handler`): only an image whose reset-PC slot
// equals the startup entry carries a vector table; a zero slot is uninstalled; the handler is a 24-bit bus PC.
std::optional<std::uint32_t> vector_handler(const FrontendProgram &program, std::size_t offset) {
  const auto &bytes = program.image.bytes;
  if (bytes.size() < 0x100U || !program.startup_ingress || offset + 4U > 0x100U) return std::nullopt;
  const auto be32 = [&](std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1U]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2U]) << 8) | static_cast<std::uint32_t>(bytes[at + 3U]);
  };
  if (be32(4U) != program.startup_ingress->entry.value) return std::nullopt;
  const auto word = be32(offset);
  if (word == 0U) return std::nullopt;
  return word & bus_mask;
}

class Decoder {
public:
  explicit Decoder(const FrontendProgram &program) : program_(program) {}

  DecodeFailure decode(std::uint32_t pc, Decoded &out) const {
    if ((pc & 1U) != 0U) return DecodeFailure::odd;
    for (const auto &alias : program_.immutable_copy_aliases) {
      if (pc < alias.execution_base || static_cast<std::uint64_t>(pc) >= static_cast<std::uint64_t>(alias.execution_base) + alias.length)
        continue;
      const auto offset = pc - alias.execution_base;
      const auto *claim = unique_rom_claim(alias.source_base + offset);
      if (claim == nullptr) return DecodeFailure::unmapped;
      return decode_in(*claim, pc, alias.source_base + offset, alias.length - offset, out);
    }
    const auto *claim = unique_rom_claim(pc);
    if (claim == nullptr) return DecodeFailure::unmapped;
    return decode_in(*claim, pc, pc, claim->target_end.value - pc, out);
  }

  // SEG-026-T002: true when `pc` is an even PC with a unique ROM mapping or ADR 0049 alias (decodability aside).
  bool mapped(std::uint32_t pc) const {
    if ((pc & 1U) != 0U) return false;
    for (const auto &alias : program_.immutable_copy_aliases)
      if (pc >= alias.execution_base && static_cast<std::uint64_t>(pc) < static_cast<std::uint64_t>(alias.execution_base) + alias.length)
        return unique_rom_claim(alias.source_base + (pc - alias.execution_base)) != nullptr;
    return unique_rom_claim(pc) != nullptr;
  }

  // SEG-026-T002: big-endian read of `count` bytes at bus `address`, only from bytes uniquely owned by the raw
  // cartridge ROM claim (immutable). An alias execution address is work RAM and is never read as immutable.
  std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned count) const {
    std::uint32_t value = 0U;
    for (unsigned i = 0; i < count; ++i) {
      const auto at = (address + i) & bus_mask;
      if (at < address) return std::nullopt;  // wrapped past the bus
      const auto *claim = unique_rom_claim(at);
      if (claim == nullptr) return std::nullopt;
      const auto offset = static_cast<std::uint64_t>(claim->image_begin.value) + (at - claim->target_begin.value);
      if (offset >= program_.image.bytes.size()) return std::nullopt;
      value = (value << 8U) | program_.image.bytes[static_cast<std::size_t>(offset)];
    }
    return value;
  }

private:
  const MappingClaim *unique_rom_claim(std::uint32_t address) const {
    const MappingClaim *found = nullptr;
    unsigned owners = 0U;
    for (const auto &claim : program_.mapping_claims) {
      if (claim.target_begin.space != TargetAddressSpace::m68k_program) continue;
      if (address >= claim.target_begin.value && address < claim.target_end.value) {
        ++owners;
        found = &claim;
      }
    }
    if (owners != 1U || found->name != "raw_cartridge_rom" || found->image_end.value < found->image_begin.value ||
        found->image_end.value > program_.image.bytes.size() ||
        found->target_end.value - found->target_begin.value != found->image_end.value - found->image_begin.value)
      return nullptr;
    return found;
  }

  // Decodes the instruction whose bytes start at mapped address `source` (inside `claim`), with execution
  // address `pc` (equal to `source` except for an ADR 0049 alias), requiring the whole span to stay within
  // `span_limit` bytes and within the claim, and no other claim to own any byte of it.
  DecodeFailure decode_in(const MappingClaim &claim, std::uint32_t pc, std::uint32_t source, std::uint64_t span_limit,
                          Decoded &out) const {
    const auto bytes = std::span<const std::uint8_t>(program_.image.bytes)
                           .subspan(static_cast<std::size_t>(claim.image_begin.value),
                                    static_cast<std::size_t>(claim.image_end.value - claim.image_begin.value));
    const std::uint64_t local = static_cast<std::uint64_t>(source) - claim.target_begin.value;
    DecodeSource decode_source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{local}};
    auto result = decode_m68k_instruction(bytes, decode_source, M68kDecodeProfile::general_startup);
    auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return DecodeFailure::rejected;
    const std::uint64_t length = decoded->provenance.length.value;
    if (length < 2U || length > span_limit || local + length > bytes.size()) return DecodeFailure::rejected;
    for (const auto &other : program_.mapping_claims) {
      if (&other == &claim || other.target_begin.space != TargetAddressSpace::m68k_program) continue;
      if (source < other.target_end.value && other.target_begin.value < static_cast<std::uint64_t>(source) + length)
        return DecodeFailure::unmapped;
    }
    decoded->provenance.source.image_offset = MoveqImageOffset{claim.image_begin.value + local};
    out.operation = lift_m68k_instruction(*decoded);
    out.length = static_cast<std::uint32_t>(length);
    return DecodeFailure::none;
  }

  const FrontendProgram &program_;
};

bool is_long_push_to_a7(const M68kIrOperation &op) {
  return op.kind == M68kIrKind::write_move && op.size == M68kMemoryAccessWidth::long_word &&
         op.destination_ea.mode == M68kEaMode::address_predec && op.destination_ea.reg == 7U;
}

// Experiment-local mirror of ADR 0048's push-window scoping rule (libs/codegen/c11/src/frontend.cpp): an RTS
// reached within 8 stack-neutral instructions from a MOVE.L <ea>,-(A7) is a computed jump, not an ordinary
// return. Evaluated over the challenger's own discovered instructions only.
std::set<std::uint32_t> push_window_rts(const std::map<std::uint32_t, Decoded> &instructions) {
  constexpr unsigned window = 8U;
  std::set<std::uint32_t> result;
  for (const auto &[push_address, push] : instructions) {
    if (!is_long_push_to_a7(push.operation)) continue;
    std::deque<std::pair<std::uint32_t, unsigned>> walk;
    std::set<std::uint32_t> visited;
    walk.emplace_back(push_address + push.length, 1U);
    while (!walk.empty()) {
      const auto [address, depth] = walk.front();
      walk.pop_front();
      if (depth > window || !visited.insert(address).second) continue;
      const auto found = instructions.find(address);
      if (found == instructions.end()) continue;
      const auto &op = found->second.operation;
      if (op.kind == M68kIrKind::return_from_subroutine) {
        result.insert(address);
        continue;
      }
      const auto effect = m68k_operation_effect(op);
      if (!effect.register_write_footprint_complete || (effect.address_register_write_mask & 0x80U) != 0U ||
          effect.stack != M68kStackEffectKind::none)
        continue;
      const auto next = address + found->second.length;
      if (op.kind == M68kIrKind::general_branch) {
        walk.emplace_back(effect.direct_target & bus_mask, depth + 1U);
        if (op.condition != M68kCondition::always) walk.emplace_back(next, depth + 1U);
      } else if (effect.pc == M68kPcEffectKind::advance) {
        walk.emplace_back(next, depth + 1U);
      }
    }
  }
  return result;
}

std::string hex_list(const std::vector<std::uint32_t> &values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i)
    out << (i == 0U ? "" : ",") << "\"" << std::hex << std::setw(6) << std::setfill('0') << values[i] << "\"";
  out << ']';
  return out.str();
}

template <typename Set>
std::vector<std::uint32_t> sorted(const Set &set) {
  return std::vector<std::uint32_t>(set.begin(), set.end());
}

// ---------------------------------------------------------------------------------------------------------
// SEG-026-T002: demand-driven proof of a PC-indexed site's exact index domain.

enum class EdgeKind : std::uint8_t { fallthrough, taken, not_taken, call, dynamic };
struct Edge {
  std::uint32_t from{};
  EdgeKind kind{EdgeKind::fallthrough};
};
using PredecessorMap = std::map<std::uint32_t, std::vector<Edge>>;

constexpr std::uint32_t evaluation_budget = 20000U;  // backward steps per site (resource bound -> unresolved)

EdgeKind edge_kind(M68kControlSuccessorKind kind) {
  switch (kind) {
  case M68kControlSuccessorKind::fallthrough: return EdgeKind::fallthrough;
  case M68kControlSuccessorKind::branch_target: return EdgeKind::fallthrough;  // unconditional: no filter
  case M68kControlSuccessorKind::conditional_target: return EdgeKind::taken;
  case M68kControlSuccessorKind::conditional_fallthrough: return EdgeKind::not_taken;
  case M68kControlSuccessorKind::call_target: return EdgeKind::call;
  }
  return EdgeKind::fallthrough;
}

// SEG-026-T003: what one proof learned about the mutable byte sources it read.
struct SourceLog {
  std::uint32_t kinds{};
  std::uint32_t bases_exact{};    // register-relative sources whose base An has an exact local value
  std::uint32_t bases_unknown{};  // ... whose base An does not
  std::optional<GenesisStateSourceOutcome> outcome;  // first non-resolved outcome
  void note(GenesisStateSourceOutcome value) {
    if (!outcome && value != GenesisStateSourceOutcome::resolved) outcome = value;
  }
};

GenesisStateSourceKind state_source_kind(M68kEaMode mode) {
  switch (mode) {
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long: return GenesisStateSourceKind::absolute_ram;
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_disp16: return GenesisStateSourceKind::register_relative;
  case M68kEaMode::address_index8: return GenesisStateSourceKind::indexed_relative;
  case M68kEaMode::address_postinc:
  case M68kEaMode::address_predec: return GenesisStateSourceKind::auto_update_relative;
  default: return GenesisStateSourceKind::other;
  }
}

std::string source_kinds_label(std::uint32_t kinds) {
  if (kinds == 0U) return "none";
  std::string out;
  for (std::uint32_t kind = 0; kind < genesis_state_source_kind_count; ++kind)
    if (((kinds >> kind) & 1U) != 0U) {
      if (!out.empty()) out += '+';
      out += genesis_state_source_kind_name(static_cast<GenesisStateSourceKind>(kind));
    }
  return out;
}

class StoreDomain;

// Registers 0..7 are D0..D7 (query width 16 or 32); 8..15 are A0..A7 (width 32, SEG-026-T003).
constexpr unsigned address_register_base = 8U;

class IndexEvaluator final : public M68kFiniteValueInputs {
public:
  IndexEvaluator(const std::map<std::uint32_t, Decoded> &instructions, const PredecessorMap &predecessors,
                 const std::set<std::uint32_t> &opaque, const Decoder &decoder, StoreDomain *stores = nullptr,
                 SourceLog *log = nullptr)
      : instructions_(instructions), predecessors_(predecessors), opaque_(opaque), decoder_(decoder), stores_(stores),
        log_(log) {}

  // Value of the register (mod 2^width) immediately before the instruction at `pc` on every known path.
  M68kFiniteValues before(unsigned reg, unsigned width, std::uint32_t pc) {
    const auto key = std::make_tuple(reg, width, pc);
    if (const auto found = memo_.find(key); found != memo_.end()) return found->second;
    if (active_.contains(key)) {  // a cycle: no loop reasoning
      note(GenesisPcIndexUnknownOrigin::cycle);
      return M68kFiniteValues::unknown();
    }
    if (steps_++ >= evaluation_budget) {
      exhausted_ = true;
      return M68kFiniteValues::unknown();
    }
    const auto preds = predecessors_.find(pc);
    if (opaque_.contains(pc) || preds == predecessors_.end() || preds->second.empty()) {
      note(roots_.contains(pc)                                          ? GenesisPcIndexUnknownOrigin::machine_root
           : opaque_.contains(pc)                                       ? GenesisPcIndexUnknownOrigin::return_continuation
                                                                        : GenesisPcIndexUnknownOrigin::no_known_predecessor);
      memo_[key] = M68kFiniteValues::unknown();
      return memo_[key];
    }
    active_.insert(key);
    M68kFiniteValues acc = M68kFiniteValues::of({}, width);
    for (const auto &edge : preds->second) {
      auto value = after(reg, width, edge.from);
      if (value.known && reg < address_register_base && (edge.kind == EdgeKind::taken || edge.kind == EdgeKind::not_taken)) {
        // Flags at the branch are exact only when its sole predecessor is the physically preceding flag setter.
        const auto branch_preds = predecessors_.find(edge.from);
        if (!opaque_.contains(edge.from) && branch_preds != predecessors_.end() && branch_preds->second.size() == 1U &&
            branch_preds->second.front().kind == EdgeKind::fallthrough) {
          const auto setter = instructions_.find(branch_preds->second.front().from);
          if (setter != instructions_.end() && setter->first + setter->second.length == edge.from &&
              m68k_finite_branch_filter(setter->second.operation, instructions_.at(edge.from).operation,
                                        edge.kind == EdgeKind::taken, reg, width, value))
            proof_ |= m68k_finite_proof::guard;
        }
      }
      if (edge.kind == EdgeKind::call) proof_ |= m68k_finite_proof::call_edge;
      if (edge.kind == EdgeKind::dynamic) proof_ |= m68k_finite_proof::dynamic_edge;
      acc = m68k_finite_union(acc, value, width);
      if (!acc.known) break;
    }
    active_.erase(key);
    memo_[key] = acc;
    return acc;
  }

  M68kFiniteValues data_register_before(unsigned reg, unsigned width) override { return before(reg, width, current_); }
  M68kFiniteValues address_register_before(unsigned reg) override {
    ++quiet_;
    auto value = before(address_register_base + reg, 32U, current_);
    --quiet_;
    return value;
  }

  std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) override {
    const auto value = decoder_.immutable_read(address, bytes);
    if (value) entries_.insert(address);
    return value;
  }

  // SEG-026-T003: a byte index source read from mutable memory.
  std::optional<M68kFiniteValues> mutable_byte(const M68kEffectiveAddress &ea) override;

  // Evaluate operands "at" `pc` (used for a store operation's own operands).
  void at(std::uint32_t pc) { current_ = pc; }
  void set_roots(const std::vector<std::uint32_t> &roots) { roots_.insert(roots.begin(), roots.end()); }

  bool exhausted() const { return exhausted_; }
  std::optional<GenesisPcIndexUnknownOrigin> origin() const { return origin_; }
  bool read_failed() const { return read_failed_; }
  std::uint32_t proof() const { return proof_; }
  std::uint32_t table_reads() const { return table_reads_; }
  std::uint32_t misaligned() const { return misaligned_; }
  const std::set<std::uint32_t> &entries() const { return entries_; }

private:
  M68kFiniteValues after(unsigned reg, unsigned width, std::uint32_t pc) {
    const auto found = instructions_.find(pc);
    if (found == instructions_.end()) return M68kFiniteValues::unknown();
    const auto saved = current_;
    current_ = pc;
    const auto transfer = reg >= address_register_base
                              ? m68k_finite_address_register_after(found->second.operation, reg - address_register_base, *this)
                              : m68k_finite_register_after(found->second.operation, reg, width, *this);
    current_ = saved;
    proof_ |= transfer.proof;
    table_reads_ += transfer.table_reads;
    misaligned_ += transfer.misaligned_reads_excluded;
    if (transfer.immutable_read_failed) {
      read_failed_ = true;
      return M68kFiniteValues::unknown();
    }
    if (!transfer.writes) return before(reg, width, pc);
    if (!transfer.values.known) {
      const auto kind = found->second.operation.kind;
      note(kind == M68kIrKind::write_move ? GenesisPcIndexUnknownOrigin::untracked_load_or_source
           : kind == M68kIrKind::add || kind == M68kIrKind::subtract || kind == M68kIrKind::logical_and ||
                   kind == M68kIrKind::logical_or || kind == M68kIrKind::add_immediate ||
                   kind == M68kIrKind::subtract_immediate || kind == M68kIrKind::logical_and_immediate
               ? GenesisPcIndexUnknownOrigin::arithmetic_on_unknown
               : GenesisPcIndexUnknownOrigin::unsupported_writer);
    }
    return transfer.values;
  }

  void note(GenesisPcIndexUnknownOrigin origin) {
    if (!origin_ && quiet_ == 0U) origin_ = origin;
  }

  const std::map<std::uint32_t, Decoded> &instructions_;
  const PredecessorMap &predecessors_;
  const std::set<std::uint32_t> &opaque_;
  const Decoder &decoder_;
  StoreDomain *stores_{};
  SourceLog *log_{};
  std::map<std::tuple<unsigned, unsigned, std::uint32_t>, M68kFiniteValues> memo_;
  std::set<std::tuple<unsigned, unsigned, std::uint32_t>> active_;
  std::uint32_t current_{};
  std::uint32_t steps_{};
  unsigned quiet_{};
  bool exhausted_{};
  bool read_failed_{};
  std::uint32_t proof_{};
  std::uint32_t table_reads_{};
  std::uint32_t misaligned_{};
  std::set<std::uint32_t> entries_;
  std::set<std::uint32_t> roots_;
  std::optional<GenesisPcIndexUnknownOrigin> origin_;
};

// ---------------------------------------------------------------------------------------------------------
// SEG-026-T003: exact store domains of mutable work-RAM bytes, relative to the challenger's discovered set D.

constexpr std::uint32_t work_ram_begin = UINT32_C(0x00E00000);  // 64 KiB work RAM, mirrored up to 0xFFFFFF
constexpr std::uint32_t max_state_locations = 16U;               // exact base sets naming more locations: unresolved
constexpr std::uint32_t max_store_addresses = 64U;               // exact store address sets beyond this: unresolved
constexpr std::uint32_t max_update_iterations = 16U;             // self-update fixed-point bound

std::uint32_t normalize_ram(std::uint32_t address) {
  address &= bus_mask;
  return address >= work_ram_begin ? (UINT32_C(0x00FF0000) | (address & 0xFFFFU)) : address;
}

class StoreDomain {
public:
  StoreDomain(const std::map<std::uint32_t, Decoded> &instructions, const PredecessorMap &predecessors,
              const std::set<std::uint32_t> &opaque, const Decoder &decoder, GenesisStoreAliasPolicy policy,
              bool interrupt_frames)
      : instructions_(instructions), predecessors_(predecessors), opaque_(opaque), decoder_(decoder), policy_(policy),
        interrupt_frames_(interrupt_frames) {}

  // The exact domain of the work-RAM byte `location`. `resolved` with the current iterate when the location is
  // the source being iterated (a self-update); `source_cycle` for any other active source.
  std::pair<GenesisStateSourceOutcome, M68kFiniteValues> domain(std::uint32_t location) {
    location = normalize_ram(location);
    if (location < work_ram_begin) {
      records_.try_emplace(location).first->second.outcome = GenesisStateSourceOutcome::not_work_ram;
      return {GenesisStateSourceOutcome::not_work_ram, M68kFiniteValues::unknown()};
    }
    if (!active_.empty() && active_.back() == location) return {GenesisStateSourceOutcome::resolved, iterate_.at(location)};
    if (std::find(active_.begin(), active_.end(), location) != active_.end()) {
      tainted_ = true;
      return {GenesisStateSourceOutcome::source_cycle, M68kFiniteValues::unknown()};
    }
    if (const auto found = records_.find(location); found != records_.end())
      return {found->second.outcome, found->second.outcome == GenesisStateSourceOutcome::resolved
                                         ? M68kFiniteValues::of(found->second.values, 32U)
                                         : M68kFiniteValues::unknown()};
    build();
    const bool outer_taint = tainted_;
    tainted_ = false;
    active_.push_back(location);
    auto record = compute(location);
    active_.pop_back();
    const bool tainted = tainted_;
    tainted_ = outer_taint || tainted;
    auto values = record.outcome == GenesisStateSourceOutcome::resolved ? M68kFiniteValues::of(record.values, 32U)
                                                                        : M68kFiniteValues::unknown();
    const auto outcome = record.outcome;
    // A record that met an outer active source depends on that source's iterate: never memoized.
    if (!tainted) records_.emplace(location, std::move(record));
    return {outcome, values};
  }

  const std::map<std::uint32_t, GenesisStateSourceRecord> &records() const { return records_; }
  void counts(std::array<std::uint64_t, genesis_store_class_count> &all,
              std::array<std::uint64_t, genesis_store_class_count> &unknown) {
    build();
    all = {};
    unknown = {};
    for (const auto &entry : entries_) {
      ++all[static_cast<std::size_t>(entry.store_class)];
      if (!entry.plain_value.known || entry.plain_value.width_derived) ++unknown[static_cast<std::size_t>(entry.store_class)];
    }
  }

private:
  struct Entry {
    std::uint32_t pc{};
    M68kMemoryStore store;
    GenesisStoreClass store_class{GenesisStoreClass::unmodeled};
    std::vector<std::uint32_t> addresses;  // exact start addresses (exact_address only)
    M68kFiniteValues plain_value;          // value with no state-source recursion (every mutable byte is width-only)
  };

  IndexEvaluator evaluator(SourceLog *log, bool with_sources) {
    return IndexEvaluator{instructions_, predecessors_, opaque_, decoder_, with_sources ? this : nullptr, log};
  }

  void build() {
    if (built_) return;
    built_ = true;
    for (const auto &[pc, decoded] : instructions_) {
      for (const auto &store : m68k_memory_stores(decoded.operation)) {
        Entry entry{pc, store, GenesisStoreClass::unmodeled, {}, M68kFiniteValues::unknown()};
        switch (store.target) {
        case M68kStoreTarget::stack_push: entry.store_class = GenesisStoreClass::stack; break;
        case M68kStoreTarget::exception_frame: entry.store_class = GenesisStoreClass::exception_frame; break;
        case M68kStoreTarget::unmodeled: entry.store_class = GenesisStoreClass::unmodeled; break;
        case M68kStoreTarget::effective_address: classify(entry, pc); break;
        }
        if (store.target != M68kStoreTarget::exception_frame && store.target != M68kStoreTarget::unmodeled) {
          auto plain = evaluator(nullptr, false);
          plain.at(pc);
          entry.plain_value = m68k_finite_store_value(decoded.operation, store, plain).values;
        }
        entries_.push_back(std::move(entry));
      }
    }
    // An interrupt entry stacks a frame at the interrupted code's A7 (value: its PC and SR).
    if (interrupt_frames_) entries_.push_back({0U, {M68kStoreTarget::exception_frame, {}, 6U}, GenesisStoreClass::exception_frame, {}, M68kFiniteValues::unknown()});
  }

  void classify(Entry &entry, std::uint32_t pc) {
    const auto &ea = entry.store.ea;
    const auto disp = static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement));
    const auto base = [&](GenesisStoreClass unknown_class) -> std::optional<M68kFiniteValues> {
      if (ea.reg == 7U) {
        entry.store_class = GenesisStoreClass::stack;
        return std::nullopt;
      }
      auto eval = evaluator(nullptr, false);
      eval.at(pc);
      auto value = eval.address_register_before(ea.reg);
      if (!value.known || value.values.size() > max_store_addresses) {
        entry.store_class = unknown_class;
        return std::nullopt;
      }
      return value;
    };
    std::vector<std::uint32_t> addresses;
    switch (ea.mode) {
    case M68kEaMode::absolute_word:
    case M68kEaMode::absolute_long: addresses.push_back(ea.absolute_address & bus_mask); break;
    case M68kEaMode::address_indirect:
    case M68kEaMode::address_disp16: {
      const auto value = base(GenesisStoreClass::register_relative);
      if (!value) return;
      for (const auto a : value->values) addresses.push_back((a + (ea.mode == M68kEaMode::address_disp16 ? disp : 0U)) & bus_mask);
      break;
    }
    case M68kEaMode::address_postinc:
    case M68kEaMode::address_predec: {
      const auto value = base(GenesisStoreClass::auto_update_relative);
      if (!value) return;
      for (const auto a : value->values)
        addresses.push_back((ea.mode == M68kEaMode::address_predec ? a - entry.store.bytes : a) & bus_mask);
      break;
    }
    case M68kEaMode::address_index8: {
      const auto value = base(GenesisStoreClass::indexed_relative);
      if (!value) return;
      auto eval = evaluator(nullptr, false);
      eval.at(pc);
      const unsigned width = ea.index_is_long ? 32U : 16U;
      const auto index = ea.index_is_address ? eval.address_register_before(ea.index_reg)
                                             : eval.data_register_before(ea.index_reg, width);
      if (!index.known || index.values.size() * value->values.size() > max_store_addresses) {
        entry.store_class = GenesisStoreClass::indexed_relative;
        return;
      }
      for (const auto a : value->values)
        for (const auto x : index.values) {
          const auto offset = ea.index_is_long ? x : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(x)));
          addresses.push_back((a + offset + disp) & bus_mask);
        }
      break;
    }
    default: entry.store_class = GenesisStoreClass::unmodeled; return;
    }
    entry.store_class = GenesisStoreClass::exact_address;
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    entry.addresses = std::move(addresses);
  }

  bool excluded(GenesisStoreClass store_class) const {
    switch (policy_) {
    case GenesisStoreAliasPolicy::strict: return false;
    case GenesisStoreAliasPolicy::exclude_stack:
      return store_class == GenesisStoreClass::stack || store_class == GenesisStoreClass::exception_frame;
    case GenesisStoreAliasPolicy::exclude_unresolved: return store_class != GenesisStoreClass::exact_address;
    }
    return false;
  }

  static void add_bytes(std::vector<std::uint32_t> &out, const M68kFiniteValues &value, unsigned bytes,
                        std::optional<unsigned> offset) {
    for (const auto v : value.values)
      for (unsigned i = 0; i < bytes; ++i)
        if (!offset || *offset == i) out.push_back((v >> (8U * (bytes - 1U - i))) & 0xFFU);
  }

  GenesisStateSourceRecord compute(std::uint32_t location) {
    GenesisStateSourceRecord record{};
    // Stores that may cover the byte: exact writers (with the covered byte offset), and every non-excluded store
    // with an unresolved destination (any offset).
    std::vector<std::pair<const Entry *, unsigned>> exact;
    std::vector<std::uint32_t> fixed;  // bytes contributed by unresolved-address stores
    for (const auto &entry : entries_) {
      if (entry.store_class == GenesisStoreClass::exact_address) {
        for (const auto a : entry.addresses)
          for (unsigned i = 0; i < entry.store.bytes; ++i)
            if (normalize_ram(a + i) == location) exact.emplace_back(&entry, i);
        continue;
      }
      if (excluded(entry.store_class)) continue;
      if (!entry.plain_value.known || entry.plain_value.width_derived) {
        record.poison_classes |= UINT32_C(1) << static_cast<unsigned>(entry.store_class);
        continue;
      }
      add_bytes(fixed, entry.plain_value, entry.store.bytes, std::nullopt);
    }
    if (record.poison_classes != 0U) {
      record.outcome = GenesisStateSourceOutcome::alias_poison;
      return record;
    }
    std::set<const Entry *> writers;
    for (const auto &[entry, offset] : exact) writers.insert(entry);
    record.exact_writers = static_cast<std::uint32_t>(writers.size());
    // Least fixed point from the machine model's reset work RAM (zero).
    auto current = M68kFiniteValues::of({0U}, 32U);
    for (std::uint32_t iteration = 1;; ++iteration) {
      if (iteration > max_update_iterations) {
        record.outcome = GenesisStateSourceOutcome::unbounded_update;
        return record;
      }
      iterate_[location] = current;
      std::vector<std::uint32_t> next = fixed;
      next.push_back(0U);
      bool zero_written = std::find(fixed.begin(), fixed.end(), 0U) != fixed.end();
      for (const auto &[entry, offset] : exact) {
        SourceLog log;
        auto eval = evaluator(&log, true);
        eval.at(entry->pc);
        const auto value = m68k_finite_store_value(instructions_.at(entry->pc).operation, entry->store, eval).values;
        if (!value.known || value.width_derived) {
          record.outcome = log.outcome && *log.outcome != GenesisStateSourceOutcome::resolved ? *log.outcome
                                                                                              : GenesisStateSourceOutcome::writer_value_unknown;
          if (record.outcome == GenesisStateSourceOutcome::not_attempted ||
              record.outcome == GenesisStateSourceOutcome::base_unknown)
            record.outcome = GenesisStateSourceOutcome::writer_value_unknown;
          iterate_.erase(location);
          return record;
        }
        std::vector<std::uint32_t> bytes;
        add_bytes(bytes, value, entry->store.bytes, offset);
        if (std::find(bytes.begin(), bytes.end(), 0U) != bytes.end()) zero_written = true;
        next.insert(next.end(), bytes.begin(), bytes.end());
      }
      auto merged = M68kFiniteValues::of(std::move(next), 32U);
      record.update_iterations = iteration;
      if (merged.values == current.values) {
        record.outcome = GenesisStateSourceOutcome::resolved;
        record.values = current.values;
        record.zero_only_from_reset = !zero_written;
        iterate_.erase(location);
        return record;
      }
      current = std::move(merged);
    }
  }

  const std::map<std::uint32_t, Decoded> &instructions_;
  const PredecessorMap &predecessors_;
  const std::set<std::uint32_t> &opaque_;
  const Decoder &decoder_;
  GenesisStoreAliasPolicy policy_;
  bool interrupt_frames_{};
  bool built_{};
  bool tainted_{};
  std::vector<Entry> entries_;
  std::vector<std::uint32_t> active_;
  std::map<std::uint32_t, M68kFiniteValues> iterate_;
  std::map<std::uint32_t, GenesisStateSourceRecord> records_;
};

std::optional<M68kFiniteValues> IndexEvaluator::mutable_byte(const M68kEffectiveAddress &ea) {
  const auto kind = state_source_kind(ea.mode);
  if (log_ != nullptr) log_->kinds |= UINT32_C(1) << static_cast<unsigned>(kind);
  const auto fail = [&](GenesisStateSourceOutcome outcome) -> std::optional<M68kFiniteValues> {
    if (log_ != nullptr) log_->note(outcome);
    return std::nullopt;
  };
  if (stores_ == nullptr) {
    // Classification: record whether a register-relative source's base is locally exact (never a location).
    if (log_ != nullptr && kind == GenesisStateSourceKind::register_relative) {
      const auto base = address_register_before(ea.reg);
      ++(base.known && ea.reg != 7U ? log_->bases_exact : log_->bases_unknown);
    }
    return fail(GenesisStateSourceOutcome::not_attempted);
  }
  std::vector<std::uint32_t> locations;
  if (kind == GenesisStateSourceKind::absolute_ram) {
    locations.push_back(ea.absolute_address & bus_mask);
  } else if (kind == GenesisStateSourceKind::register_relative) {
    const auto base = address_register_before(ea.reg);
    if (log_ != nullptr) ++(base.known && ea.reg != 7U ? log_->bases_exact : log_->bases_unknown);
    if (!base.known || ea.reg == 7U) return fail(GenesisStateSourceOutcome::base_unknown);
    if (base.values.size() > max_state_locations) return fail(GenesisStateSourceOutcome::too_many_locations);
    const auto disp = ea.mode == M68kEaMode::address_disp16 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)) : 0U;
    for (const auto a : base.values) locations.push_back((a + disp) & bus_mask);
  } else {
    return fail(GenesisStateSourceOutcome::not_attempted);
  }
  auto acc = M68kFiniteValues::of({}, 32U);
  for (const auto location : locations) {
    const auto [outcome, values] = stores_->domain(location);
    if (outcome != GenesisStateSourceOutcome::resolved) return fail(outcome);
    acc = m68k_finite_union(acc, values, 32U);
  }
  if (log_ != nullptr) log_->note(GenesisStateSourceOutcome::resolved);
  return acc;
}

GenesisPcIndexSiteRecovery resolve_pc_index_site(const Decoded &site, std::uint32_t pc,
                                                 const std::map<std::uint32_t, Decoded> &instructions,
                                                 const PredecessorMap &predecessors, const std::set<std::uint32_t> &opaque,
                                                 const std::vector<std::uint32_t> &roots,
                                                 const Decoder &decoder, bool accept_width_domains,
                                                 std::set<std::uint32_t> &entries, StoreDomain *stores = nullptr,
                                                 SourceLog *log = nullptr) {
  GenesisPcIndexSiteRecovery out{};
  out.call = site.operation.kind == M68kIrKind::call_general;
  const auto &ea = site.operation.source_ea;
  if (ea.mode != M68kEaMode::pc_index8) return out;
  if (ea.index_is_address) {
    out.outcome = GenesisPcIndexOutcome::address_register_index;
    return out;
  }
  IndexEvaluator evaluator{instructions, predecessors, opaque, decoder, stores, log};
  evaluator.set_roots(roots);
  const unsigned width = ea.index_is_long ? 32U : 16U;
  const auto index = evaluator.before(ea.index_reg, width, pc);
  if (log != nullptr) {
    out.source_kinds = log->kinds;
    out.source_outcome = log->outcome;
  }
  out.proof = evaluator.proof();
  out.table_reads = evaluator.table_reads();
  out.misaligned_reads_excluded = evaluator.misaligned();
  if (evaluator.read_failed()) {
    out.outcome = GenesisPcIndexOutcome::entry_outside_immutable_image;
    return out;
  }
  if (evaluator.exhausted()) {
    out.outcome = GenesisPcIndexOutcome::resource_limit;
    return out;
  }
  if (!index.known) {  // index_unknown
    out.unknown_origin = evaluator.origin().value_or(GenesisPcIndexUnknownOrigin::limit_exceeded);
    return out;
  }
  if (index.width_derived && !accept_width_domains) {
    out.outcome = GenesisPcIndexOutcome::width_only_domain;
    return out;
  }
  std::set<std::uint32_t> targets;
  for (const auto value : index.values) {
    const auto target = m68k_pc_index_address(ea, value);
    if ((target & 1U) != 0U) {
      ++out.odd_targets_excluded;  // an odd JMP/JSR target raises an address error: never a normal target
      continue;
    }
    if (!decoder.mapped(target)) {
      out.outcome = GenesisPcIndexOutcome::target_outside_image;
      return out;
    }
    targets.insert(target);
  }
  if (targets.empty()) {
    out.outcome = GenesisPcIndexOutcome::empty_domain;
    return out;
  }
  out.outcome = GenesisPcIndexOutcome::resolved;
  out.targets.assign(targets.begin(), targets.end());
  entries.insert(evaluator.entries().begin(), evaluator.entries().end());
  return out;
}

bool is_pc_index_site(const Decoded &decoded) {
  return (decoded.operation.kind == M68kIrKind::jump_general || decoded.operation.kind == M68kIrKind::call_general) &&
         decoded.operation.source_ea.mode == M68kEaMode::pc_index8;
}

}  // namespace

std::string genesis_challenger_family_name(std::uint32_t family) {
  if (family < m68k_dynamic_control_family_count)
    return m68k_dynamic_control_family_name(static_cast<M68kDynamicControlFamily>(family));
  if (family == genesis_challenger_family_push_window_rts) return "rts_push_window";
  return "unknown";
}

namespace {

GenesisReachabilityChallengerResult run_once(const FrontendProgram &program, const GenesisReachabilityChallengerConfig &config,
                                             const std::set<std::uint32_t> &pinned, std::set<std::uint32_t> &invalidated,
                                             std::uint32_t &store_invalidations) {
  GenesisReachabilityChallengerResult result{};
  const Decoder decoder{program};
  const bool hypothesis = config.exception_model == GenesisReachabilityExceptionModel::normal_resumption;
  std::set<std::uint32_t> roots;
  if (program.startup_ingress) roots.insert(program.startup_ingress->entry.value & bus_mask);
  for (const auto vector : machine_delivered_vectors()) {
    if (const auto handler = vector_handler(program, static_cast<std::size_t>(vector) * 4U)) {
      roots.insert(*handler);
      ++result.vector_roots;
    }
  }
  result.roots.assign(roots.begin(), roots.end());

  std::map<std::uint32_t, Decoded> instructions;
  std::set<std::uint32_t> visited;
  std::deque<std::uint32_t> queue(result.roots.begin(), result.roots.end());
  std::set<std::uint32_t> rts_sites;
  std::set<std::uint32_t> rtr_sites;
  std::set<std::uint32_t> rte_sites;
  std::uint32_t recovery_round = 0U;
  bool recovery_started = false;
  const auto drain = [&]() {
    while (!queue.empty()) {
      const std::uint32_t pc = queue.front() & bus_mask;
      queue.pop_front();
      if (!visited.insert(pc).second) continue;
      Decoded decoded{};
      switch (decoder.decode(pc, decoded)) {
      case DecodeFailure::odd: result.odd_targets.insert(pc); continue;
      case DecodeFailure::unmapped: result.unmapped_targets.insert(pc); continue;
      case DecodeFailure::rejected: result.rejected_decode_targets.insert(pc); continue;
      case DecodeFailure::none: break;
      }
      const auto control = m68k_control_successors(decoded.operation);
      instructions.emplace(pc, decoded);
      result.discovered.emplace(pc, decoded.length);
      if (control.always_raises_exception) ++result.exception_raising_instructions;
      for (const auto &successor : control.successors) queue.push_back(successor.target & bus_mask);
      switch (control.stacked) {
      case M68kStackedContinuationKind::call_continuation: result.call_continuations.insert(control.stacked_address & bus_mask); break;
      case M68kStackedContinuationKind::exception_continuation:
        result.exception_continuations.insert(control.stacked_address & bus_mask);
        break;
      case M68kStackedContinuationKind::pushed_code_address:
        result.pushed_code_addresses.insert(control.stacked_address & bus_mask);
        break;
      case M68kStackedContinuationKind::none: break;
      }
      if (control.dynamic == M68kDynamicControlFamily::return_from_subroutine) rts_sites.insert(pc);
      else if (control.dynamic == M68kDynamicControlFamily::return_restore_condition_codes) rtr_sites.insert(pc);
      else if (control.dynamic == M68kDynamicControlFamily::return_from_exception) rte_sites.insert(pc);
      if (control.dynamic != M68kDynamicControlFamily::none)
        result.sites[static_cast<std::size_t>(control.dynamic)].insert(pc);
      if (config.pc_index_recovery && is_pc_index_site(decoded)) {
        GenesisPcIndexSiteRecovery site{};
        site.call = decoded.operation.kind == M68kIrKind::call_general;
        site.first_round = recovery_round;
        result.pc_index_sites.emplace(pc, site);
      }
    }
  };
  // SEG-026-T002: one recovery step over every encountered PC-indexed site against the current predecessor
  // graph. Returns true when any site's exact target set changed (new targets are enqueued).
  const auto recover = [&]() {
    if (!recovery_started) {
      recovery_started = true;
      result.discovered_before_recovery = result.discovered.size();
      for (std::size_t family = 0; family < genesis_challenger_family_count; ++family)
        result.sites_before_recovery[family] = result.sites[family].size();
    }
    PredecessorMap predecessors;
    for (const auto &[pc, decoded] : instructions)
      for (const auto &successor : m68k_control_successors(decoded.operation).successors)
        predecessors[successor.target & bus_mask].push_back({pc, edge_kind(successor.kind)});
    for (const auto &[pc, site] : result.pc_index_sites)
      if (site.outcome == GenesisPcIndexOutcome::resolved)
        for (const auto target : site.targets) predecessors[target].push_back({pc, EdgeKind::dynamic});
    // Opaque entries: machine roots and every stacked continuation (entered through a return whose register
    // state this analysis never models).
    std::set<std::uint32_t> opaque(result.roots.begin(), result.roots.end());
    opaque.insert(result.call_continuations.begin(), result.call_continuations.end());
    opaque.insert(result.exception_continuations.begin(), result.exception_continuations.end());
    opaque.insert(result.pushed_code_addresses.begin(), result.pushed_code_addresses.end());
    bool changed = false;
    result.table_entry_addresses.clear();
    // SEG-026-T003: one store-domain view per recovery step, over exactly the current D.
    const bool classify_sources = config.store_provenance != GenesisReachabilityChallengerConfig::StoreProvenance::off;
    std::optional<StoreDomain> stores;
    if (config.store_provenance == GenesisReachabilityChallengerConfig::StoreProvenance::prove)
      stores.emplace(instructions, predecessors, opaque, decoder, config.store_alias_policy, result.vector_roots > 0U);
    for (auto &[pc, site] : result.pc_index_sites) {
      if (pinned.contains(pc)) {
        site.outcome = GenesisPcIndexOutcome::invalidated;
        continue;
      }
      const bool was_resolved = site.outcome == GenesisPcIndexOutcome::resolved;
      SourceLog log;
      auto next = resolve_pc_index_site(instructions.at(pc), pc, instructions, predecessors, opaque, result.roots, decoder,
                                        config.pc_index_width_domains, result.table_entry_addresses,
                                        stores ? &*stores : nullptr, classify_sources ? &log : nullptr);
      next.first_round = site.first_round;
      // Monotone growth only: a resolved site that loses its proof, or any target, is invalidated (restart).
      if (was_resolved && (next.outcome != GenesisPcIndexOutcome::resolved ||
                           !std::includes(next.targets.begin(), next.targets.end(), site.targets.begin(), site.targets.end()))) {
        invalidated.insert(pc);
        if ((site.proof & m68k_finite_proof::store_domain) != 0U) ++store_invalidations;
      }
      if (next.targets != site.targets) changed = true;
      for (const auto target : next.targets)
        if (!visited.contains(target)) queue.push_back(target);
      site = std::move(next);
    }
    if (stores) {
      result.state_sources = stores->records();
      stores->counts(result.stores_by_class, result.unknown_value_stores_by_class);
    }
    return changed;
  };
  for (;;) {
    drain();
    // Resumption step (deterministic, monotone once enabled).
    if (!result.continuations_enabled) {
      const auto computed = push_window_rts(instructions);
      for (const auto site : rts_sites)
        if (!computed.contains(site)) { result.continuations_enabled = true; break; }
      if (hypothesis && !rtr_sites.empty()) result.continuations_enabled = true;
    }
    std::size_t before = queue.size();
    if (result.continuations_enabled) {
      for (const auto target : result.call_continuations) if (!visited.contains(target)) queue.push_back(target);
      if (config.pea_continuations)
        for (const auto target : result.pushed_code_addresses) if (!visited.contains(target)) queue.push_back(target);
    }
    if (hypothesis && !rte_sites.empty())
      for (const auto target : result.exception_continuations) if (!visited.contains(target)) queue.push_back(target);
    if (queue.size() != before) {
      ++result.rounds;
      continue;
    }
    if (!config.pc_index_recovery) break;
    const bool changed = recover();
    if (!invalidated.empty()) return result;  // the caller restarts with those sites pinned
    if (!changed && queue.empty()) break;
    ++recovery_round;
    ++result.recovery_rounds;
  }
  // Final ADR 0048 classification over the complete discovered set.
  for (const auto site : push_window_rts(instructions)) {
    result.sites[static_cast<std::size_t>(M68kDynamicControlFamily::return_from_subroutine)].erase(site);
    result.sites[genesis_challenger_family_push_window_rts].insert(site);
  }
  // Resolved PC-indexed sites are no longer unresolved dynamic sites.
  for (const auto &[pc, site] : result.pc_index_sites) {
    if (site.outcome != GenesisPcIndexOutcome::resolved) continue;
    const auto family = site.call ? M68kDynamicControlFamily::call_pc_index : M68kDynamicControlFamily::jump_pc_index;
    result.sites[static_cast<std::size_t>(family)].erase(pc);
    result.recovered_targets.insert(site.targets.begin(), site.targets.end());
  }
  // Overlap: a discovered start strictly inside another discovered instruction's span.
  for (auto it = result.discovered.begin(); it != result.discovered.end(); ++it) {
    auto next = std::next(it);
    if (next != result.discovered.end() && next->first < it->first + it->second) ++result.overlapping_starts;
  }
  // Table entries whose bytes are also bytes of a discovered instruction (a table read overlapping code).
  for (const auto entry : result.table_entry_addresses) {
    auto it = result.discovered.upper_bound(entry);
    if (it != result.discovered.begin()) {
      --it;
      if (entry < it->first + it->second) ++result.table_entries_overlapping_code;
    }
  }
  return result;
}

}  // namespace

const char *genesis_pc_index_outcome_name(GenesisPcIndexOutcome outcome) noexcept {
  switch (outcome) {
  case GenesisPcIndexOutcome::resolved: return "resolved";
  case GenesisPcIndexOutcome::index_unknown: return "index_unknown";
  case GenesisPcIndexOutcome::resource_limit: return "resource_limit";
  case GenesisPcIndexOutcome::address_register_index: return "address_register_index";
  case GenesisPcIndexOutcome::entry_outside_immutable_image: return "entry_outside_immutable_image";
  case GenesisPcIndexOutcome::target_outside_image: return "target_outside_image";
  case GenesisPcIndexOutcome::empty_domain: return "empty_domain";
  case GenesisPcIndexOutcome::invalidated: return "invalidated";
  case GenesisPcIndexOutcome::width_only_domain: return "width_only_domain";
  }
  return "unknown";
}

const char *genesis_store_alias_policy_name(GenesisStoreAliasPolicy policy) noexcept {
  switch (policy) {
  case GenesisStoreAliasPolicy::strict: return "strict";
  case GenesisStoreAliasPolicy::exclude_stack: return "exclude_stack";
  case GenesisStoreAliasPolicy::exclude_unresolved: return "exclude_unresolved";
  }
  return "unknown";
}

const char *genesis_state_source_kind_name(GenesisStateSourceKind kind) noexcept {
  switch (kind) {
  case GenesisStateSourceKind::absolute_ram: return "absolute_ram";
  case GenesisStateSourceKind::register_relative: return "register_relative";
  case GenesisStateSourceKind::indexed_relative: return "indexed_relative";
  case GenesisStateSourceKind::auto_update_relative: return "auto_update_relative";
  case GenesisStateSourceKind::other: return "other";
  }
  return "unknown";
}

const char *genesis_state_source_outcome_name(GenesisStateSourceOutcome outcome) noexcept {
  switch (outcome) {
  case GenesisStateSourceOutcome::resolved: return "resolved";
  case GenesisStateSourceOutcome::not_attempted: return "not_attempted";
  case GenesisStateSourceOutcome::base_unknown: return "base_unknown";
  case GenesisStateSourceOutcome::not_work_ram: return "not_work_ram";
  case GenesisStateSourceOutcome::alias_poison: return "alias_poison";
  case GenesisStateSourceOutcome::writer_value_unknown: return "writer_value_unknown";
  case GenesisStateSourceOutcome::unbounded_update: return "unbounded_update";
  case GenesisStateSourceOutcome::source_cycle: return "source_cycle";
  case GenesisStateSourceOutcome::too_many_locations: return "too_many_locations";
  }
  return "unknown";
}

const char *genesis_store_class_name(GenesisStoreClass store_class) noexcept {
  switch (store_class) {
  case GenesisStoreClass::exact_address: return "exact_address";
  case GenesisStoreClass::stack: return "stack";
  case GenesisStoreClass::exception_frame: return "exception_frame";
  case GenesisStoreClass::register_relative: return "register_relative";
  case GenesisStoreClass::indexed_relative: return "indexed_relative";
  case GenesisStoreClass::auto_update_relative: return "auto_update_relative";
  case GenesisStoreClass::unmodeled: return "unmodeled";
  }
  return "unknown";
}

const char *genesis_pc_index_unknown_origin_name(GenesisPcIndexUnknownOrigin origin) noexcept {
  switch (origin) {
  case GenesisPcIndexUnknownOrigin::none: return "none";
  case GenesisPcIndexUnknownOrigin::machine_root: return "machine_root";
  case GenesisPcIndexUnknownOrigin::return_continuation: return "return_continuation";
  case GenesisPcIndexUnknownOrigin::no_known_predecessor: return "no_known_predecessor";
  case GenesisPcIndexUnknownOrigin::cycle: return "cycle";
  case GenesisPcIndexUnknownOrigin::untracked_load_or_source: return "untracked_load_or_source";
  case GenesisPcIndexUnknownOrigin::arithmetic_on_unknown: return "arithmetic_on_unknown";
  case GenesisPcIndexUnknownOrigin::unsupported_writer: return "unsupported_writer";
  case GenesisPcIndexUnknownOrigin::limit_exceeded: return "limit_exceeded";
  }
  return "unknown";
}

GenesisReachabilityChallengerResult run_genesis_reachability_challenger(const FrontendProgram &program,
                                                                        const GenesisReachabilityChallengerConfig &config) {
  std::set<std::uint32_t> pinned;
  std::uint32_t store_invalidations = 0U;
  for (std::uint32_t restarts = 0;; ++restarts) {
    std::set<std::uint32_t> invalidated;
    auto result = run_once(program, config, pinned, invalidated, store_invalidations);
    if (invalidated.empty()) {
      result.recovery_restarts = restarts;
      result.store_domain_invalidations = store_invalidations;
      return result;
    }
    pinned.insert(invalidated.begin(), invalidated.end());  // monotone: terminates
  }
}

std::map<std::uint32_t, GenesisReachabilityPcClassification> classify_genesis_reachability_pcs(
    const FrontendProgram &program, const std::vector<std::uint32_t> &pcs) {
  const Decoder decoder{program};
  std::map<std::uint32_t, GenesisReachabilityPcClassification> out;
  std::map<std::uint32_t, Decoded> instructions;
  for (const auto raw : pcs) {
    const auto pc = raw & bus_mask;
    if (out.contains(pc)) continue;
    GenesisReachabilityPcClassification entry{};
    Decoded decoded{};
    if (decoder.decode(pc, decoded) == DecodeFailure::none) {
      entry.decoded = true;
      entry.length = decoded.length;
      entry.control = m68k_control_successors(decoded.operation);
      instructions.emplace(pc, decoded);
    }
    out.emplace(pc, entry);
  }
  for (const auto site : push_window_rts(instructions)) out[site].push_window_rts = true;
  // SEG-026-T002: strict local domain labels of classified PC-indexed sites (measurement only).
  PredecessorMap predecessors;
  std::set<std::uint32_t> opaque;
  for (const auto &[pc, decoded] : instructions) {
    const auto control = m68k_control_successors(decoded.operation);
    for (const auto &successor : control.successors)
      predecessors[successor.target & bus_mask].push_back({pc, edge_kind(successor.kind)});
    if (control.stacked != M68kStackedContinuationKind::none) opaque.insert(control.stacked_address & bus_mask);
  }
  // Machine roots (reset entry, delivered vectors) are opaque exactly as in discovery.
  std::vector<std::uint32_t> roots;
  if (program.startup_ingress) roots.push_back(program.startup_ingress->entry.value & bus_mask);
  for (const auto vector : machine_delivered_vectors())
    if (const auto handler = vector_handler(program, static_cast<std::size_t>(vector) * 4U)) roots.push_back(*handler);
  opaque.insert(roots.begin(), roots.end());
  std::set<std::uint32_t> entries;
  for (const auto &[pc, decoded] : instructions) {
    if (!is_pc_index_site(decoded)) continue;
    SourceLog log;
    const auto site = resolve_pc_index_site(decoded, pc, instructions, predecessors, opaque, roots, decoder, false, entries,
                                            nullptr, &log);
    out[pc].pc_index_domain = site.outcome;
    out[pc].pc_index_unknown_origin = site.unknown_origin;
    out[pc].pc_index_source_kinds = site.source_kinds;
    out[pc].pc_index_source_base = log.bases_exact == 0U && log.bases_unknown == 0U ? "none"
                                   : log.bases_unknown == 0U                      ? "exact"
                                   : log.bases_exact == 0U                        ? "unknown"
                                                                                  : "mixed";
  }
  return out;
}

std::string format_genesis_reachability_classification_private(
    const std::map<std::uint32_t, GenesisReachabilityPcClassification> &classification) {
  std::ostringstream out;
  out << "{\"schema\":\"segarecomp.reachability_classification.private.v1\",\"pcs\":{";
  bool first = true;
  for (const auto &[pc, entry] : classification) {
    out << (first ? "" : ",") << "\"" << std::hex << std::setw(6) << std::setfill('0') << pc << std::dec << "\":{";
    first = false;
    if (!entry.decoded) {
      out << "\"decoded\":false}";
      continue;
    }
    const auto family = entry.push_window_rts ? genesis_challenger_family_push_window_rts
                                              : static_cast<std::uint32_t>(entry.control.dynamic);
    std::vector<std::uint32_t> successors;
    for (const auto &successor : entry.control.successors) successors.push_back(successor.target & bus_mask);
    out << "\"decoded\":true,\"length\":" << entry.length << ",\"family\":\"" << genesis_challenger_family_name(family)
        << "\",\"successors\":" << hex_list(successors) << ",\"stacked\":\""
        << (entry.control.stacked == M68kStackedContinuationKind::call_continuation        ? "call"
            : entry.control.stacked == M68kStackedContinuationKind::exception_continuation ? "exception"
            : entry.control.stacked == M68kStackedContinuationKind::pushed_code_address    ? "pea"
                                                                                           : "none")
        << "\",\"stacked_address\":\"" << std::hex << std::setw(6) << std::setfill('0')
        << (entry.control.stacked_address & bus_mask) << std::dec << "\",\"exception_entry\":"
        << (entry.control.always_raises_exception ? "true" : "false");
    if (entry.pc_index_domain)
      out << ",\"pc_index_domain\":\"" << genesis_pc_index_outcome_name(*entry.pc_index_domain)
          << "\",\"pc_index_unknown_origin\":\"" << genesis_pc_index_unknown_origin_name(entry.pc_index_unknown_origin)
          << "\",\"pc_index_source_kind\":\"" << source_kinds_label(entry.pc_index_source_kinds)
          << "\",\"pc_index_source_base\":\"" << entry.pc_index_source_base << '"';
    out << '}';
  }
  out << "}}\n";
  return out.str();
}

namespace {

// SEG-026-T002: aggregate-only recovery report (counts, generic families, proof mechanism names; no address).
std::string format_pc_index_recovery_aggregate(const GenesisReachabilityChallengerResult &result,
                                              const GenesisReachabilityChallengerConfig &config) {
  const bool sources = config.store_provenance != GenesisReachabilityChallengerConfig::StoreProvenance::off;
  // The SEG-026-T003 proof bit is reported only when store provenance is enabled (T002 output is unchanged).
  const std::uint32_t mechanism_count = sources ? m68k_finite_proof::count : m68k_finite_proof::count - 1U;
  std::array<std::size_t, genesis_pc_index_outcome_count> outcomes{};
  std::array<std::size_t, genesis_pc_index_unknown_origin_count> origins{};
  std::array<std::size_t, m68k_finite_proof::count> mechanisms{};
  std::map<std::uint32_t, std::size_t> by_round;
  std::size_t resolved_jmp = 0U, resolved_jsr = 0U, max_targets = 0U;
  std::uint64_t table_reads = 0U, misaligned = 0U, odd = 0U, target_sum = 0U;
  struct Row {
    std::uint32_t round;
    bool call;
    std::uint32_t outcome;
    std::size_t targets;
    std::uint32_t proof;
    std::uint32_t reads;
    std::uint32_t source_kinds;
    std::uint32_t source_outcome;  // genesis_state_source_outcome_count: none
    auto key() const { return std::make_tuple(round, call, outcome, targets, proof, reads, source_kinds, source_outcome); }
  };
  // SEG-026-T003: sites by (source kind label, outcome) and by (source kind label, source outcome).
  std::map<std::string, std::map<std::string, std::size_t>> by_source_kind;
  std::map<std::string, std::map<std::string, std::size_t>> by_source_outcome;
  std::vector<Row> rows;
  for (const auto &[pc, site] : result.pc_index_sites) {
    (void)pc;
    ++outcomes[static_cast<std::size_t>(site.outcome)];
    if (site.outcome == GenesisPcIndexOutcome::index_unknown) ++origins[static_cast<std::size_t>(site.unknown_origin)];
    ++by_round[site.first_round];
    table_reads += site.table_reads;
    misaligned += site.misaligned_reads_excluded;
    odd += site.odd_targets_excluded;
    if (site.outcome == GenesisPcIndexOutcome::resolved) {
      (site.call ? resolved_jsr : resolved_jmp) += 1U;
      max_targets = std::max(max_targets, site.targets.size());
      target_sum += site.targets.size();
      for (std::uint32_t bit = 0; bit < mechanism_count; ++bit)
        if (((site.proof >> bit) & 1U) != 0U) ++mechanisms[bit];
    }
    rows.push_back({site.first_round, site.call, static_cast<std::uint32_t>(site.outcome), site.targets.size(),
                    site.outcome == GenesisPcIndexOutcome::resolved ? site.proof : 0U, site.table_reads,
                    sources ? site.source_kinds : 0U,
                    sources && site.source_outcome ? static_cast<std::uint32_t>(*site.source_outcome)
                                                   : genesis_state_source_outcome_count});
    if (sources) {
      const auto label = source_kinds_label(site.source_kinds);
      ++by_source_kind[label][genesis_pc_index_outcome_name(site.outcome)];
      ++by_source_outcome[label][site.source_outcome ? genesis_state_source_outcome_name(*site.source_outcome) : "none"];
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.key() < b.key(); });
  std::size_t targets_discovered = 0U, targets_rejected = 0U, targets_unmapped = 0U, targets_odd = 0U;
  for (const auto target : result.recovered_targets) {
    if (result.discovered.contains(target)) ++targets_discovered;
    else if (result.rejected_decode_targets.contains(target)) ++targets_rejected;
    else if (result.unmapped_targets.contains(target)) ++targets_unmapped;
    else if (result.odd_targets.contains(target)) ++targets_odd;
  }
  const auto proof_names = [](std::uint32_t proof) {
    std::ostringstream names;
    names << '[';
    bool first = true;
    for (std::uint32_t bit = 0; bit < m68k_finite_proof::count; ++bit)
      if (((proof >> bit) & 1U) != 0U) {
        names << (first ? "" : ",") << '"' << m68k_finite_proof::name(bit) << '"';
        first = false;
      }
    names << ']';
    return names.str();
  };
  const auto resolved = outcomes[static_cast<std::size_t>(GenesisPcIndexOutcome::resolved)];
  std::ostringstream out;
  out << "{\"sites_encountered\":" << result.pc_index_sites.size() << ",\"resolved\":" << resolved
      << ",\"unresolved\":" << result.pc_index_sites.size() - resolved << ",\"resolved_jmp\":" << resolved_jmp
      << ",\"resolved_jsr\":" << resolved_jsr << ",\"outcomes\":{";
  for (std::uint32_t o = 0; o < genesis_pc_index_outcome_count; ++o)
    out << (o == 0U ? "" : ",") << '"' << genesis_pc_index_outcome_name(static_cast<GenesisPcIndexOutcome>(o))
        << "\":" << outcomes[o];
  out << "},\"index_unknown_origins\":{";
  for (std::uint32_t o = 1; o < genesis_pc_index_unknown_origin_count; ++o)
    out << (o == 1U ? "" : ",") << '"' << genesis_pc_index_unknown_origin_name(static_cast<GenesisPcIndexUnknownOrigin>(o))
        << "\":" << origins[o];
  out << "},\"proof_mechanisms_of_resolved_sites\":{";
  for (std::uint32_t bit = 0; bit < mechanism_count; ++bit)
    out << (bit == 0U ? "" : ",") << '"' << m68k_finite_proof::name(bit) << "\":" << mechanisms[bit];
  out << "},\"recovered_targets\":" << result.recovered_targets.size() << ",\"target_identities_sum\":" << target_sum
      << ",\"max_targets_per_site\":" << max_targets << ",\"recovered_targets_discovered\":" << targets_discovered
      << ",\"recovered_targets_rejected\":" << targets_rejected << ",\"recovered_targets_unmapped\":" << targets_unmapped
      << ",\"recovered_targets_odd\":" << targets_odd << ",\"table_reads\":" << table_reads
      << ",\"table_entries_read\":" << result.table_entry_addresses.size()
      << ",\"table_entries_overlapping_code\":" << result.table_entries_overlapping_code
      << ",\"misaligned_reads_excluded\":" << misaligned << ",\"odd_targets_excluded\":" << odd
      << ",\"recovery_rounds\":" << result.recovery_rounds << ",\"recovery_restarts\":" << result.recovery_restarts
      << ",\"discovered_before_recovery\":" << result.discovered_before_recovery
      << ",\"newly_discovered\":" << result.discovered.size() - result.discovered_before_recovery
      << ",\"sites_by_first_round\":{";
  bool first = true;
  for (const auto &[round, n] : by_round) {
    out << (first ? "" : ",") << '"' << round << "\":" << n;
    first = false;
  }
  out << "},\"unresolved_sites_before_recovery\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << '"' << genesis_challenger_family_name(family)
        << "\":" << result.sites_before_recovery[family];
  out << "},\"sites\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &row = rows[i];
    out << (i == 0U ? "" : ",") << "{\"round\":" << row.round << ",\"family\":\"" << (row.call ? "jsr" : "jmp")
        << "\",\"outcome\":\"" << genesis_pc_index_outcome_name(static_cast<GenesisPcIndexOutcome>(row.outcome))
        << "\",\"targets\":" << row.targets << ",\"table_reads\":" << row.reads << ",\"proof\":" << proof_names(row.proof);
    if (sources)
      out << ",\"source_kind\":\"" << source_kinds_label(row.source_kinds) << "\",\"source_outcome\":\""
          << (row.source_outcome < genesis_state_source_outcome_count
                  ? genesis_state_source_outcome_name(static_cast<GenesisStateSourceOutcome>(row.source_outcome))
                  : "none")
          << '"';
    out << '}';
  }
  out << "]";
  if (sources) {
    const auto nested = [&](const std::map<std::string, std::map<std::string, std::size_t>> &table) {
      std::ostringstream text;
      text << '{';
      bool first_outer = true;
      for (const auto &[label, counts] : table) {
        text << (first_outer ? "" : ",") << '"' << label << "\":{";
        first_outer = false;
        bool first_inner = true;
        for (const auto &[name, n] : counts) {
          text << (first_inner ? "" : ",") << '"' << name << "\":" << n;
          first_inner = false;
        }
        text << '}';
      }
      text << '}';
      return text.str();
    };
    out << ",\"store_provenance\":{\"mode\":\""
        << (config.store_provenance == GenesisReachabilityChallengerConfig::StoreProvenance::prove ? "prove" : "classify")
        << "\",\"alias_policy\":\"" << genesis_store_alias_policy_name(config.store_alias_policy)
        << "\",\"sites_by_source_kind_and_outcome\":" << nested(by_source_kind)
        << ",\"sites_by_source_kind_and_source_outcome\":" << nested(by_source_outcome);
    if (config.store_provenance == GenesisReachabilityChallengerConfig::StoreProvenance::prove) {
      std::array<std::size_t, genesis_state_source_outcome_count> source_outcomes{};
      std::array<std::size_t, genesis_store_class_count> poison{};
      std::size_t in_ram = 0U, domain_sum = 0U, domain_max = 0U, zero_from_reset = 0U, writers = 0U, self_updates = 0U;
      std::uint32_t max_iterations = 0U;
      for (const auto &[location, record] : result.state_sources) {
        (void)location;
        ++source_outcomes[static_cast<std::size_t>(record.outcome)];
        if (record.outcome != GenesisStateSourceOutcome::not_work_ram) ++in_ram;
        for (std::uint32_t c = 0; c < genesis_store_class_count; ++c)
          if (((record.poison_classes >> c) & 1U) != 0U) ++poison[c];
        writers += record.exact_writers;
        max_iterations = std::max(max_iterations, record.update_iterations);
        if (record.update_iterations > 2U) ++self_updates;
        if (record.outcome == GenesisStateSourceOutcome::resolved) {
          domain_sum += record.values.size();
          domain_max = std::max(domain_max, record.values.size());
          if (record.zero_only_from_reset) ++zero_from_reset;
        }
      }
      out << ",\"state_locations\":" << result.state_sources.size() << ",\"state_locations_in_work_ram\":" << in_ram
          << ",\"state_location_outcomes\":{";
      for (std::uint32_t o = 0; o < genesis_state_source_outcome_count; ++o)
        out << (o == 0U ? "" : ",") << '"' << genesis_state_source_outcome_name(static_cast<GenesisStateSourceOutcome>(o))
            << "\":" << source_outcomes[o];
      out << "},\"alias_poison_by_store_class\":{";
      for (std::uint32_t c = 0; c < genesis_store_class_count; ++c)
        out << (c == 0U ? "" : ",") << '"' << genesis_store_class_name(static_cast<GenesisStoreClass>(c)) << "\":" << poison[c];
      out << "},\"resolved_domain_values_sum\":" << domain_sum << ",\"resolved_domain_values_max\":" << domain_max
          << ",\"resolved_zero_only_from_reset_ram\":" << zero_from_reset << ",\"exact_writers_sum\":" << writers
          << ",\"locations_needing_more_than_two_iterations\":" << self_updates
          << ",\"max_update_iterations\":" << max_iterations << ",\"stores_in_d_by_class\":{";
      for (std::uint32_t c = 0; c < genesis_store_class_count; ++c)
        out << (c == 0U ? "" : ",") << '"' << genesis_store_class_name(static_cast<GenesisStoreClass>(c))
            << "\":" << result.stores_by_class[c];
      out << "},\"non_exact_value_stores_in_d_by_class\":{";
      for (std::uint32_t c = 0; c < genesis_store_class_count; ++c)
        out << (c == 0U ? "" : ",") << '"' << genesis_store_class_name(static_cast<GenesisStoreClass>(c))
            << "\":" << result.unknown_value_stores_by_class[c];
      out << "},\"store_domain_invalidations\":" << result.store_domain_invalidations;
    }
    out << '}';
  }
  out << '}';
  return out.str();
}

}  // namespace

std::string format_genesis_reachability_challenger_aggregate(const GenesisReachabilityChallengerResult &result,
                                                             const GenesisReachabilityChallengerConfig &config) {
  std::ostringstream out;
  out << "{\"exception_model\":\""
      << (config.exception_model == GenesisReachabilityExceptionModel::strict ? "strict" : "normal_resumption")
      << "\",\"pea_continuations\":" << (config.pea_continuations ? "true" : "false")
      << (config.pc_index_width_domains ? ",\"pc_index_width_domains\":true" : "")
      << ",\"root_count\":" << result.roots.size() << ",\"vector_roots\":" << result.vector_roots
      << ",\"discovered\":" << result.discovered.size() << ",\"call_continuations\":" << result.call_continuations.size()
      << ",\"exception_continuations\":" << result.exception_continuations.size()
      << ",\"pushed_code_addresses\":" << result.pushed_code_addresses.size()
      << ",\"continuations_enabled\":" << (result.continuations_enabled ? "true" : "false")
      << ",\"rounds\":" << result.rounds << ",\"overlapping_starts\":" << result.overlapping_starts
      << ",\"rejected_decode_targets\":" << result.rejected_decode_targets.size()
      << ",\"unmapped_targets\":" << result.unmapped_targets.size() << ",\"odd_targets\":" << result.odd_targets.size()
      << ",\"exception_raising_instructions\":" << result.exception_raising_instructions << ",\"sites\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << "\"" << genesis_challenger_family_name(family) << "\":" << result.sites[family].size();
  out << "}";
  if (config.pc_index_recovery) out << ",\"pc_index_recovery\":" << format_pc_index_recovery_aggregate(result, config);
  out << "}";
  return out.str();
}

std::string format_genesis_reachability_challenger_private(const GenesisReachabilityChallengerResult &result,
                                                           const GenesisReachabilityChallengerConfig &config) {
  std::ostringstream out;
  std::vector<std::uint32_t> discovered;
  for (const auto &[pc, length] : result.discovered) {
    (void)length;
    discovered.push_back(pc);
  }
  out << "{\"schema\":\"segarecomp.reachability_challenger.private.v1\",\"aggregate\":"
      << format_genesis_reachability_challenger_aggregate(result, config) << ",\"roots\":" << hex_list(result.roots)
      << ",\"discovered\":" << hex_list(discovered) << ",\"call_continuations\":" << hex_list(sorted(result.call_continuations))
      << ",\"exception_continuations\":" << hex_list(sorted(result.exception_continuations))
      << ",\"pushed_code_addresses\":" << hex_list(sorted(result.pushed_code_addresses))
      << ",\"rejected_decode_targets\":" << hex_list(sorted(result.rejected_decode_targets))
      << ",\"unmapped_targets\":" << hex_list(sorted(result.unmapped_targets)) << ",\"sites\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << "\"" << genesis_challenger_family_name(family)
        << "\":" << hex_list(sorted(result.sites[family]));
  out << "}";
  if (config.pc_index_recovery) {
    out << ",\"pc_index_sites\":{";
    bool first = true;
    for (const auto &[pc, site] : result.pc_index_sites) {
      out << (first ? "" : ",") << "\"" << std::hex << std::setw(6) << std::setfill('0') << pc << std::dec
          << "\":{\"outcome\":\"" << genesis_pc_index_outcome_name(site.outcome) << "\",\"unknown_origin\":\""
          << genesis_pc_index_unknown_origin_name(site.unknown_origin) << "\",\"round\":" << site.first_round
          << ",\"targets\":" << hex_list(site.targets);
      if (config.store_provenance != GenesisReachabilityChallengerConfig::StoreProvenance::off)
        out << ",\"source_kind\":\"" << source_kinds_label(site.source_kinds) << "\",\"source_outcome\":\""
            << (site.source_outcome ? genesis_state_source_outcome_name(*site.source_outcome) : "none") << '"';
      out << '}';
      first = false;
    }
    out << '}';
  }
  out << "}\n";
  return out.str();
}

}  // namespace segarecomp
