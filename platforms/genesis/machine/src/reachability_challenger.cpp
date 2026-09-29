// SEG-026-T001 (experiment, report-only). See reachability_challenger.hpp.

#include "segarecomp/machine/genesis/reachability_challenger.hpp"

#include <deque>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <variant>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

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

}  // namespace

std::string genesis_challenger_family_name(std::uint32_t family) {
  if (family < m68k_dynamic_control_family_count)
    return m68k_dynamic_control_family_name(static_cast<M68kDynamicControlFamily>(family));
  if (family == genesis_challenger_family_push_window_rts) return "rts_push_window";
  return "unknown";
}

GenesisReachabilityChallengerResult run_genesis_reachability_challenger(const FrontendProgram &program,
                                                                        const GenesisReachabilityChallengerConfig &config) {
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
    }
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
    if (queue.size() == before) break;
    ++result.rounds;
  }
  // Final ADR 0048 classification over the complete discovered set.
  for (const auto site : push_window_rts(instructions)) {
    result.sites[static_cast<std::size_t>(M68kDynamicControlFamily::return_from_subroutine)].erase(site);
    result.sites[genesis_challenger_family_push_window_rts].insert(site);
  }
  // Overlap: a discovered start strictly inside another discovered instruction's span.
  for (auto it = result.discovered.begin(); it != result.discovered.end(); ++it) {
    auto next = std::next(it);
    if (next != result.discovered.end() && next->first < it->first + it->second) ++result.overlapping_starts;
  }
  return result;
}

std::string format_genesis_reachability_challenger_aggregate(const GenesisReachabilityChallengerResult &result,
                                                             const GenesisReachabilityChallengerConfig &config) {
  std::ostringstream out;
  out << "{\"exception_model\":\""
      << (config.exception_model == GenesisReachabilityExceptionModel::strict ? "strict" : "normal_resumption")
      << "\",\"pea_continuations\":" << (config.pea_continuations ? "true" : "false")
      << ",\"root_count\":" << result.roots.size() << ",\"vector_roots\":" << result.vector_roots
      << ",\"discovered\":" << result.discovered.size() << ",\"call_continuations\":" << result.call_continuations.size()
      << ",\"exception_continuations\":" << result.exception_continuations.size()
      << ",\"pushed_code_addresses\":" << result.pushed_code_addresses.size()
      << ",\"continuations_enabled\":" << (result.continuations_enabled ? "true" : "false")
      << ",\"rounds\":" << result.rounds << ",\"overlapping_starts\":" << result.overlapping_starts
      << ",\"rejected_decode_targets\":" << result.rejected_decode_targets.size()
      << ",\"unmapped_targets\":" << result.unmapped_targets.size() << ",\"odd_targets\":" << result.odd_targets.size()
      << ",\"sites\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << "\"" << genesis_challenger_family_name(family) << "\":" << result.sites[family].size();
  out << "}}";
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
  out << "}}\n";
  return out.str();
}

}  // namespace segarecomp
