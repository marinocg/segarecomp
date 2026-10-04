// SEG-031 (ADR 0080): validation and application of an explicit Genesis M68K hybrid admission plan (see hybrid_admission.hpp).

#include "segarecomp/machine/genesis/hybrid_admission.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/machine/genesis/reachability_challenger.hpp"

namespace segarecomp {

namespace {

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

std::string hex8(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(value));
  return text;
}

std::optional<std::uint32_t> parse_hex8(std::string_view text) {
  if (text.size() != 8U) return std::nullopt;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

bool valid_sha256(std::string_view text) {
  return text.size() == 64U && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::uint32_t entry_address(const FrontendAnalysis::ImmutableRomAotEntry &entry) {
  return static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value) & bus_mask;
}

}  // namespace

const char *genesis_admission_strategy_name(GenesisAdmissionStrategy strategy) noexcept {
  switch (strategy) {
  case GenesisAdmissionStrategy::broad: return "broad";
  case GenesisAdmissionStrategy::hybrid: return "hybrid";
  }
  return "invalid";
}

std::string format_genesis_hybrid_admission_plan(const GenesisHybridAdmissionPlan &plan) {
  std::ostringstream out;
  out << genesis_hybrid_admission_plan_schema << '\n' << "rom_sha256 " << plan.rom_sha256 << '\n';
  for (const auto &alias : plan.aliases)
    out << "alias " << hex8(alias.execution_base) << ':' << hex8(alias.source_base) << ':' << hex8(alias.length) << '\n';
  out << "strategy " << genesis_admission_strategy_name(plan.strategy) << '\n';
  if (plan.strategy == GenesisAdmissionStrategy::hybrid)
    for (const auto &range : plan.ranges) out << "range " << hex8(range.begin_address) << ' ' << hex8(range.end_address) << '\n';
  out << "end\n";
  return out.str();
}

std::optional<GenesisHybridAdmissionPlan> parse_genesis_hybrid_admission_plan(std::string_view text, std::string *error) {
  const auto fail = [&](const char *reason) -> std::optional<GenesisHybridAdmissionPlan> {
    if (error != nullptr) *error = reason;
    return std::nullopt;
  };
  if (text.size() > genesis_hybrid_admission_plan_max_bytes) return fail("plan_too_large");
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos) return fail("plan_unterminated_line");
    lines.push_back(text.substr(0, newline));
    text.remove_prefix(newline + 1U);
  }
  std::size_t index = 0;
  const auto next = [&]() -> std::optional<std::string_view> {
    if (index >= lines.size()) return std::nullopt;
    return lines[index++];
  };
  GenesisHybridAdmissionPlan plan;
  if (next() != genesis_hybrid_admission_plan_schema) return fail("plan_schema");
  const auto sha = next();
  if (!sha || sha->substr(0, 11U) != "rom_sha256 " || !valid_sha256(sha->substr(11U))) return fail("plan_rom_sha256");
  plan.rom_sha256 = std::string(sha->substr(11U));
  std::optional<std::string_view> line = next();
  while (line && line->substr(0, 6U) == "alias ") {
    const auto value = line->substr(6U);
    if (value.size() != 26U || value[8] != ':' || value[17] != ':') return fail("plan_alias");
    const auto execution = parse_hex8(value.substr(0, 8U));
    const auto source = parse_hex8(value.substr(9U, 8U));
    const auto length = parse_hex8(value.substr(18U, 8U));
    if (!execution || !source || !length) return fail("plan_alias");
    if (!plan.aliases.empty() && *execution <= plan.aliases.back().execution_base) return fail("plan_alias_order");
    plan.aliases.push_back({*execution, *source, *length});
    line = next();
  }
  if (line == std::string_view{"strategy broad"}) plan.strategy = GenesisAdmissionStrategy::broad;
  else if (line == std::string_view{"strategy hybrid"}) plan.strategy = GenesisAdmissionStrategy::hybrid;
  else return fail("plan_strategy");
  line = next();
  while (line && line->substr(0, 6U) == "range ") {
    if (plan.strategy != GenesisAdmissionStrategy::hybrid) return fail("plan_range_in_broad");
    const auto value = line->substr(6U);
    if (value.size() != 17U || value[8] != ' ') return fail("plan_range");
    const auto begin = parse_hex8(value.substr(0, 8U));
    const auto end = parse_hex8(value.substr(9U, 8U));
    if (!begin || !end || *begin >= *end || (*begin & 1U) != 0U || (*end & 1U) != 0U || *end > bus_mask + 1U) return fail("plan_range");
    if (!plan.ranges.empty() && *begin <= plan.ranges.back().end_address) return fail("plan_range_order");
    if (plan.ranges.size() >= genesis_hybrid_admission_plan_max_ranges) return fail("plan_too_many_ranges");
    plan.ranges.push_back({*begin, *end});
    line = next();
  }
  if (line != std::string_view{"end"} || index != lines.size()) return fail("plan_trailer");
  if (plan.strategy == GenesisAdmissionStrategy::hybrid && plan.ranges.empty()) return fail("plan_empty");
  return plan;
}

std::vector<FrontendProgram::ImmutableRomAotRange> genesis_hybrid_admission_ranges(const std::vector<std::uint32_t> &universe,
                                                                                   const std::vector<std::uint32_t> &admitted) {
  const std::set<std::uint32_t> keep(admitted.begin(), admitted.end());
  std::vector<FrontendProgram::ImmutableRomAotRange> out;
  bool open = false;
  for (std::size_t i = 0; i < universe.size(); ++i) {
    const auto address = universe[i];
    if (!keep.contains(address)) {
      open = false;
      continue;
    }
    // The run ends just past the last admitted identity's address (the next identity, admitted or not, starts at or after it).
    const std::uint32_t end = address + 2U;
    if (open) out.back().end_address = end;
    else out.push_back({address, end});
    open = true;
  }
  return out;
}

bool genesis_hybrid_admission_contains(const std::vector<FrontendProgram::ImmutableRomAotRange> &ranges, std::uint32_t address) {
  auto it = std::upper_bound(ranges.begin(), ranges.end(), address,
                             [](std::uint32_t value, const FrontendProgram::ImmutableRomAotRange &range) { return value < range.begin_address; });
  if (it == ranges.begin()) return false;
  --it;
  return address >= it->begin_address && address < it->end_address;
}

std::optional<std::string> apply_genesis_hybrid_admission(const FrontendProgram &program, std::string_view rom_sha256,
                                                         const GenesisHybridAdmissionPlan &plan,
                                                         std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries) {
  if (!program.immutable_rom_aot_enabled) return "broad_aot_not_enabled";
  if (plan.rom_sha256 != rom_sha256) return "rom_sha256_mismatch";
  if (plan.aliases.size() != program.immutable_copy_aliases.size() ||
      !std::equal(plan.aliases.begin(), plan.aliases.end(), program.immutable_copy_aliases.begin(), [](const auto &a, const auto &b) {
        return std::tie(a.execution_base, a.source_base, a.length) == std::tie(b.execution_base, b.source_base, b.length);
      }))
    return "alias_set_mismatch";
  if (plan.strategy == GenesisAdmissionStrategy::broad) return std::nullopt;
  if (plan.ranges.empty()) return "empty_admission";
  for (std::size_t i = 0; i < plan.ranges.size(); ++i) {
    const auto &range = plan.ranges[i];
    if (range.begin_address >= range.end_address || (range.begin_address & 1U) != 0U || (range.end_address & 1U) != 0U ||
        (i > 0U && range.begin_address <= plan.ranges[i - 1U].end_address))
      return "malformed_range";
  }
  // The broad identities by execution address (the universe U of this emission).
  std::map<std::uint32_t, const FrontendAnalysis::ImmutableRomAotEntry *> broad;
  for (const auto &entry : entries) broad.emplace(entry_address(entry), &entry);
  const auto admitted = [&](std::uint32_t address) { return genesis_hybrid_admission_contains(plan.ranges, address & bus_mask); };
  std::size_t admitted_count = 0;
  for (const auto &[address, entry] : broad) {
    if (!admitted(address)) {
      // ADR 0049 identities are statically materialized executable images: always covered (mandatory roots).
      if (entry->execution_alias) return "materialized_image_not_admitted";
      continue;
    }
    ++admitted_count;
    const auto control = m68k_control_successors(entry->operation);
    for (const auto &successor : control.successors) {
      const auto target = successor.target & bus_mask;
      if (broad.contains(target) && !admitted(target)) return "fixed_successor_not_admitted";
    }
    if (control.stacked == M68kStackedContinuationKind::call_continuation) {
      const auto target = control.stacked_address & bus_mask;
      if (broad.contains(target) && !admitted(target)) return "call_continuation_not_admitted";
    }
  }
  if (admitted_count == 0U) return "empty_admission";
  for (const auto root : genesis_reachability_roots(program).roots)
    if (broad.contains(root & bus_mask) && !admitted(root)) return "machine_root_not_admitted";
  std::erase_if(entries, [&](const FrontendAnalysis::ImmutableRomAotEntry &entry) { return !admitted(entry_address(entry)); });
  return std::nullopt;
}

}  // namespace segarecomp
