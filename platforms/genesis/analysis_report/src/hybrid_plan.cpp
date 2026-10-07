// SEG-031 (ADR 0080, report-only): the Genesis M68K hybrid admission planner (see hybrid_plan.hpp).

#include "segarecomp/genesis_analysis_report/hybrid_plan.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>
#include <variant>

#include "segarecomp/cpu/m68k/control_successors.hpp"

namespace segarecomp {

namespace {

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

std::string hex6(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%06x", static_cast<unsigned>(value & bus_mask));
  return text;
}

// SEG-041-T008 parsing helpers, mirroring the existing `GenesisHybridAdmissionPlan` text-format conventions
// (hybrid_admission.cpp's own `parse_hex8`/`valid_sha256`) rather than inventing a second parsing style.
std::optional<std::uint32_t> fact_parse_hex8(std::string_view text) {
  if (text.size() != 8U) return std::nullopt;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

bool fact_valid_sha256(std::string_view text) {
  return text.size() == 64U && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A bounded, non-empty ASCII token (no whitespace, no control characters): the producer identity field must stay generic and
// non-reconstructable (never a path, never a commercial identifier) -- this only enforces shape, not content policy.
bool fact_valid_token(std::string_view text) {
  if (text.empty() || text.size() > 128U) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

// Strictly ascending, unique hex8 values (the same "sorted, disjoint" convention `GenesisHybridAdmissionPlan`'s own text format
// already requires elsewhere in this product) -- an out-of-order or duplicate entry is rejected rather than silently normalized.
std::optional<std::vector<std::uint32_t>> fact_parse_entry_list(std::string_view text) {
  std::vector<std::uint32_t> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto token = text.substr(0, comma);
    const auto value = fact_parse_hex8(token);
    if (!value) return std::nullopt;
    if (!out.empty() && *value <= out.back()) return std::nullopt;
    out.push_back(*value);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1U);
    if (text.empty()) return std::nullopt;  // no trailing comma
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// U: the sorted execution addresses of the broad immutable-ROM AOT identities of an independent copy of `program`.
std::optional<std::vector<std::uint32_t>> broad_universe(FrontendProgram program) {
  if (!program.immutable_rom_aot_enabled && !apply_genesis_immutable_rom_aot(program)) return std::nullopt;
  const auto analysis = analyze_m68k_frontend(program);
  const std::vector<FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
  if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) entries = &partial->accepted_prefix.immutable_rom_aot_entries;
  else if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) entries = &accepted->immutable_rom_aot_entries;
  if (entries == nullptr) return std::nullopt;
  std::vector<std::uint32_t> out;
  out.reserve(entries->size());
  for (const auto &entry : *entries) out.push_back(static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value) & bus_mask);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool contains(const std::vector<std::uint32_t> &sorted, std::uint32_t value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

// Every uncovered dynamic control site of a round: unresolved computed sites, unresolved return sites, and every computed site the
// report does not credit as resolved (a superset: an extra site only widens).
std::set<std::uint32_t> uncovered_sites(const GenesisAnalysisReport &report) {
  std::set<std::uint32_t> out;
  for (const auto &[pc, reason] : report.analysis.unresolved_computed) {
    (void)reason;
    out.insert(pc);
  }
  for (const auto &[pc, site] : report.analysis.return_sites)
    if (!site.resolved) out.insert(pc);
  for (const auto &[pc, site] : report.computed_sites)
    if (!site.resolved) out.insert(pc);
  return out;
}

// The exact targets a resolved site's analysis followed (pc-indexed, address-register-relative or return site), when resolved.
std::optional<std::vector<std::uint32_t>> resolved_targets(const GenesisAnalysisReport &report, std::uint32_t pc) {
  if (const auto found = report.analysis.pc_index_sites.find(pc); found != report.analysis.pc_index_sites.end())
    return found->second.outcome == M68kPcIndexOutcome::resolved ? std::optional(found->second.targets) : std::nullopt;
  if (const auto found = report.analysis.address_sites.find(pc); found != report.analysis.address_sites.end())
    return found->second.resolved ? std::optional(found->second.targets) : std::nullopt;
  if (const auto found = report.analysis.return_sites.find(pc); found != report.analysis.return_sites.end())
    return found->second.resolved ? std::optional(found->second.targets) : std::nullopt;
  return std::nullopt;
}

bool alias_pc(const GenesisM68kAnalysisImage &image, std::uint32_t pc) {
  for (const auto &candidate : image.images().set.images) {
    if (candidate.provenance.authority != ImageAuthority::static_proof) continue;
    for (const auto &mapping : candidate.mappings)
      if (pc >= mapping.execution_base && static_cast<std::uint64_t>(pc) < static_cast<std::uint64_t>(mapping.execution_base) + mapping.length)
        return true;
  }
  return false;
}

// Every mapped even PC of every `static_proof` image (the mandatory materialized roots).
std::vector<std::uint32_t> materialized_entries(const GenesisM68kAnalysisImage &image) {
  std::vector<std::uint32_t> out;
  for (const auto &candidate : image.images().set.images) {
    if (candidate.provenance.authority != ImageAuthority::static_proof) continue;
    for (const auto &mapping : candidate.mappings)
      for (std::uint64_t pc = mapping.execution_base; pc < static_cast<std::uint64_t>(mapping.execution_base) + mapping.length; pc += 2U)
        if (image.mapped(static_cast<std::uint32_t>(pc) & bus_mask)) out.push_back(static_cast<std::uint32_t>(pc) & bus_mask);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void merge_into(std::vector<std::uint32_t> &into, const std::vector<std::uint32_t> &more) {
  std::vector<std::uint32_t> out;
  out.reserve(into.size() + more.size());
  std::set_union(into.begin(), into.end(), more.begin(), more.end(), std::back_inserter(out));
  into = std::move(out);
}

std::size_t total_entries(const std::map<std::uint32_t, std::vector<std::uint32_t>> &islands) {
  std::set<std::uint32_t> distinct;
  for (const auto &[pc, entries] : islands) distinct.insert(entries.begin(), entries.end());
  return distinct.size();
}

std::string ratio(std::size_t numerator, std::size_t denominator) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << (denominator == 0U ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator));
  return out.str();
}

}  // namespace

const char *genesis_hybrid_outcome_name(GenesisHybridOutcome outcome) noexcept {
  switch (outcome) {
  case GenesisHybridOutcome::hybrid: return "hybrid";
  case GenesisHybridOutcome::broad_images_invalid: return "broad_images_invalid";
  case GenesisHybridOutcome::broad_universe_rejected: return "broad_universe_rejected";
  case GenesisHybridOutcome::broad_analysis_incomplete: return "broad_analysis_incomplete";
  case GenesisHybridOutcome::broad_historical_model: return "broad_historical_model";
  case GenesisHybridOutcome::broad_whole_image: return "broad_whole_image";
  case GenesisHybridOutcome::broad_island_bound: return "broad_island_bound";
  case GenesisHybridOutcome::broad_closure_bound: return "broad_closure_bound";
  case GenesisHybridOutcome::broad_validation_failed: return "broad_validation_failed";
  }
  return "invalid";
}

const char *genesis_hybrid_container_name(GenesisHybridContainer container) noexcept {
  switch (container) {
  case GenesisHybridContainer::exact: return "exact";
  case GenesisHybridContainer::points_to_region: return "points_to_region";
  case GenesisHybridContainer::executable_image: return "executable_image";
  case GenesisHybridContainer::materialized_image: return "materialized_image";
  case GenesisHybridContainer::whole_image: return "whole_image";
  }
  return "invalid";
}

std::optional<GenesisExternalM68kFacts> parse_genesis_external_m68k_facts(const std::string &text, const std::string &rom_sha256) {
  if (text.size() > genesis_external_m68k_facts_max_bytes) return std::nullopt;
  if (!fact_valid_sha256(rom_sha256)) return std::nullopt;
  std::vector<std::string_view> lines;
  std::string_view remaining = text;
  while (!remaining.empty()) {
    const auto newline = remaining.find('\n');
    if (newline == std::string_view::npos) return std::nullopt;  // unterminated trailing line: malformed, reject the whole file
    lines.push_back(remaining.substr(0, newline));
    remaining.remove_prefix(newline + 1U);
  }
  std::size_t index = 0;
  const auto next = [&]() -> std::optional<std::string_view> {
    if (index >= lines.size()) return std::nullopt;
    return lines[index++];
  };
  if (next() != genesis_external_m68k_facts_schema) return std::nullopt;
  const auto sha_line = next();
  if (!sha_line || sha_line->substr(0, 11U) != "rom_sha256 " || sha_line->substr(11U) != rom_sha256) return std::nullopt;
  // The ROM hash must equal the caller's own digest of the exact image being analysed -- a file produced for a different ROM
  // (or for a stale build of the same title) is rejected outright, never partially trusted.
  const auto producer_line = next();
  if (!producer_line || producer_line->substr(0, 9U) != "producer " || !fact_valid_token(producer_line->substr(9U))) return std::nullopt;
  GenesisExternalM68kFacts facts;
  facts.rom_sha256 = rom_sha256;
  facts.producer = std::string(producer_line->substr(9U));
  auto line = next();
  while (line && line->substr(0, 5U) == "fact ") {
    const auto value = line->substr(5U);
    if (value.size() < 8U + 1U + 7U + 1U) return std::nullopt;  // "pc"+' '+"exact "/"contained "+at least one hex8
    const auto pc = fact_parse_hex8(value.substr(0, 8U));
    if (!pc || value[8] != ' ') return std::nullopt;
    bool exact{};
    std::string_view rest;
    if (value.substr(9U, 6U) == "exact ") {
      exact = true;
      rest = value.substr(15U);
    } else if (value.substr(9U, 10U) == "contained ") {
      exact = false;
      rest = value.substr(19U);
    } else {
      return std::nullopt;
    }
    const auto entries = fact_parse_entry_list(rest);
    if (!entries) return std::nullopt;
    if (!facts.facts.empty() && *pc <= facts.facts.back().pc) return std::nullopt;  // strictly ascending, unique PCs (determinism)
    if (facts.facts.size() >= genesis_external_m68k_facts_max_facts) return std::nullopt;
    facts.facts.push_back(GenesisExternalM68kFact{*pc, *entries, exact});
    line = next();
  }
  if (line != std::string_view{"end"} || index != lines.size()) return std::nullopt;
  return facts;
}

namespace {

// SEG-041-T008: structurally re-verifies one externally-supplied fact against `image` -- segarecomp's own CPU decoder/legality
// and mapping authority, unchanged and unweakened. A fact that fails any check here is discarded entirely (never partially
// applied); the caller must then fall back to its own existing (angr-free) classification. This is the only place external
// facts touch the hybrid planner, and it never trusts `image.decode`/`image.mapped`'s callers less than the planner already
// does for every other admission authority in this file.
std::optional<std::vector<std::uint32_t>> reverify_external_entries(const GenesisM68kAnalysisImage &image,
                                                                     const std::vector<std::uint32_t> &entries, std::size_t max_entries) {
  if (entries.empty() || entries.size() > max_entries) return std::nullopt;
  std::set<std::uint32_t> verified;
  for (const auto raw : entries) {
    const std::uint32_t target = raw & bus_mask;
    if ((target & 1U) != 0U) return std::nullopt;         // an odd target raises an address error: discard the whole fact
    if (!image.mapped(target)) return std::nullopt;        // not a legal image address per segarecomp's own mapping authority
    if (!image.decode(target)) return std::nullopt;        // not an MC68000-legal instruction per segarecomp's own decoder
    verified.insert(target);
  }
  return std::vector<std::uint32_t>(verified.begin(), verified.end());
}

// Looks up and re-verifies `pc`'s externally-supplied fact, if any. Returns `site` unchanged (still `whole_image`) when there is
// no fact for `pc`, or when the fact fails re-verification.
GenesisHybridSite apply_external_fact(GenesisHybridSite site, const GenesisM68kAnalysisImage &image, std::uint32_t pc,
                                      std::size_t max_entries, const std::optional<GenesisExternalM68kFacts> &external) {
  if (!external) return site;
  const auto found = std::find_if(external->facts.begin(), external->facts.end(), [&](const auto &fact) { return fact.pc == pc; });
  if (found == external->facts.end()) return site;
  auto verified = reverify_external_entries(image, found->entries, max_entries);
  if (!verified) return site;
  site.entries = std::move(*verified);
  site.external = true;
  site.container = found->exact ? GenesisHybridContainer::exact : GenesisHybridContainer::points_to_region;
  return site;
}

}  // namespace

GenesisHybridSite genesis_hybrid_container(const GenesisAnalysisReport &report, const GenesisM68kAnalysisImage &image, std::uint32_t pc,
                                           std::size_t max_entries, const std::optional<GenesisExternalM68kFacts> &external) {
  GenesisHybridSite site{};
  if (const auto found = report.computed_sites.find(pc); found != report.computed_sites.end()) {
    site.family = found->second.family;
    site.reason = found->second.reason;
    site.sub = found->second.detail;
  } else if (const auto reason = report.analysis.unresolved_computed.find(pc); reason != report.analysis.unresolved_computed.end()) {
    site.reason = reason->second;
  }
  const auto decoded = image.decode(pc);
  if (!decoded) return site;  // whole_image (defensive: a site's own PC that cannot even decode is not a case external facts can help)
  const auto &operation = decoded->operation;
  const auto family = m68k_control_successors(operation).dynamic;
  const bool address_form = family == M68kDynamicControlFamily::jump_address_indirect || family == M68kDynamicControlFamily::call_address_indirect ||
                            family == M68kDynamicControlFamily::jump_address_disp16 || family == M68kDynamicControlFamily::call_address_disp16;
  if (!address_form) return apply_external_fact(site, image, pc, max_entries, external);  // indexed/PC-indexed/returns/unclassified
  // The address register before the instruction, joined over every context and partition of the site.
  const auto value = m68k_query_address_register(report.analysis, pc, operation.source_ea.reg & 7U);
  if (!value.is_known() || value.width_derived) return apply_external_fact(site, image, pc, max_entries, external);
  const auto displacement = operation.source_ea.mode == M68kEaMode::address_disp16
                                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(operation.source_ea.displacement))
                                : 0U;
  std::uint64_t count = 0;
  for (const auto &[region, offsets] : value.pairs) count += offsets.count();
  if (count > max_entries) return apply_external_fact(site, image, pc, max_entries, external);  // over the island bound: whole
  std::set<std::uint32_t> entries;
  bool whole_image_stride = false;
  for (const auto &[region, offsets] : value.pairs) {
    const auto add = [&](std::uint32_t offset) {
      // 32-bit address-register arithmetic, then the 24-bit bus (the exact semantics the decoder and the runtime dispatch use).
      const std::uint32_t target = (region.base + offset + displacement) & bus_mask;
      if ((target & 1U) == 0U && image.mapped(target)) entries.insert(target);  // an odd target raises an address error
    };
    if (offsets.is_strided()) {
      for (std::uint64_t offset = offsets.lo(); offset <= offsets.hi(); offset += offsets.stride()) add(static_cast<std::uint32_t>(offset));
      if (region.kind == M68kRegionKind::image && offsets.stride() <= 2U && offsets.lo() == 0U && offsets.hi() + 2U >= region.size)
        whole_image_stride = true;
    } else {
      for (const auto offset : offsets.exact()) add(offset);
    }
  }
  site.entries.assign(entries.begin(), entries.end());
  if (std::any_of(site.entries.begin(), site.entries.end(), [&](std::uint32_t target) { return alias_pc(image, target); }))
    site.container = GenesisHybridContainer::materialized_image;
  else site.container = whole_image_stride ? GenesisHybridContainer::executable_image : GenesisHybridContainer::points_to_region;
  return site;
}

std::optional<std::string> validate_genesis_hybrid_round(const GenesisAnalysisReport &report, const GenesisM68kAnalysisImage &image,
                                                         const std::map<std::uint32_t, std::vector<std::uint32_t>> &island_entries,
                                                         const std::vector<std::uint32_t> &universe, std::size_t max_island_entries,
                                                         const std::optional<GenesisExternalM68kFacts> &external) {
  if (!report.images_valid || !report.analysis.complete) return "analysis_incomplete";
  const auto &reached = report.discovered;
  const auto covered = [&](std::uint32_t target) -> bool {
    target &= bus_mask;
    if (reached.contains(target)) return true;
    // A target the analysis image cannot decode cannot execute as generated code unless it is a broad identity.
    return !image.decode(target) && !contains(universe, target);
  };
  for (const auto root : report.roots.roots)
    if (!covered(root)) return "root_not_admitted";
  for (const auto pc : report.analysis.undecodable)
    if (contains(universe, pc & bus_mask)) return "undecodable_broad_identity";
  const auto uncovered = uncovered_sites(report);
  for (const auto &[pc, length] : reached) {
    (void)length;
    const auto decoded = image.decode(pc);
    if (!decoded) return "reached_not_decodable";
    const auto control = m68k_control_successors(decoded->operation);
    for (const auto &successor : control.successors)
      if (!covered(successor.target)) return "fixed_successor_not_admitted";
    if (control.stacked == M68kStackedContinuationKind::call_continuation && !covered(control.stacked_address))
      return "call_continuation_not_admitted";
    if (control.dynamic == M68kDynamicControlFamily::none) continue;
    if (uncovered.contains(pc)) {
      const auto site = genesis_hybrid_container(report, image, pc, max_island_entries, external);
      if (site.container == GenesisHybridContainer::whole_image) return "unbounded_site";
      const auto configured = island_entries.find(pc);
      if (configured == island_entries.end()) return "site_not_configured";
      if (!std::includes(configured->second.begin(), configured->second.end(), site.entries.begin(), site.entries.end()))
        return "site_container_not_configured";
      continue;
    }
    if (const auto targets = resolved_targets(report, pc)) {
      for (const auto target : *targets)
        if (!covered(target)) return "resolved_target_not_admitted";
      continue;
    }
    // What remains must be an ordinary RTS covered by the continuation model (normal or the inherited premise); any other dynamic
    // site that is neither uncovered nor resolved is unexpected and rejected rather than passed silently.
    if (control.dynamic != M68kDynamicControlFamily::return_from_subroutine) return "unclassified_dynamic_site";
  }
  for (const auto &[pc, entries] : island_entries)
    for (const auto entry : entries)
      if (!covered(entry)) return "island_entry_not_admitted";
  return std::nullopt;
}

GenesisHybridPlan plan_genesis_hybrid_admission(const FrontendProgram &program, const GenesisHybridPlanConfig &config) {
  GenesisHybridPlan plan{};
  const auto image = GenesisM68kAnalysisImage::create(program);
  if (!image) return plan;  // broad_images_invalid
  const auto universe = broad_universe(program);
  if (!universe) {
    plan.outcome = GenesisHybridOutcome::broad_universe_rejected;
    return plan;
  }
  plan.universe = *universe;
  plan.admitted = plan.universe;  // broad until a validated fixed point says otherwise
  auto analysis_config = config.analysis;
  plan.diagnostic_transparent_handlers = config.diagnostic_transparent_handlers;
  analysis_config.diagnostic_transparent_handlers = config.diagnostic_transparent_handlers;
  analysis_config.domains = GenesisAnalysisDomains{true, true, true, true};
  const auto materialized = materialized_entries(*image);
  plan.materialized_entries = materialized.size();
  const auto entry = program.startup_ingress ? std::optional<std::uint32_t>(program.startup_ingress->entry.value & bus_mask) : std::nullopt;
  std::map<std::uint32_t, std::vector<std::uint32_t>> islands, opaque;
  for (std::uint32_t round = 1; round <= config.max_rounds; ++round) {
    analysis_config.island_entries = islands;
    analysis_config.opaque_entries = opaque;
    const auto report = run_genesis_analysis_report(program, analysis_config);
    plan.rounds = round;
    if (!report.images_valid) {
      plan.outcome = GenesisHybridOutcome::broad_images_invalid;
      return plan;
    }
    if (!report.analysis.complete) {
      plan.outcome = GenesisHybridOutcome::broad_analysis_incomplete;
      return plan;
    }
    if (round == 1U) {
      plan.precise_d = report.discovered.size();
      for (const auto &[pc, length] : report.discovered) {
        (void)length;
        plan.precise_d_in_universe += contains(plan.universe, pc) ? 1U : 0U;
      }
    }
    // ADR 0079 decision 8: only a validated frames round carries the proven interrupt-register model; every other result is a
    // historical reference model and is never credited.
    if (!report.analysis.frames.enabled || !report.analysis.frames.validated) {
      plan.outcome = GenesisHybridOutcome::broad_historical_model;
      return plan;
    }
    plan.sites.clear();
    plan.external_facts_applied = 0U;
    plan.resolved_sites = 0U;
    for (const auto &[pc, site] : report.computed_sites) plan.resolved_sites += site.resolved ? 1U : 0U;
    plan.premise_returns = report.analysis.return_slots.premise_sites;
    auto grown = islands;
    auto grown_opaque = opaque;
    bool whole = false;
    for (const auto pc : uncovered_sites(report)) {
      auto site = genesis_hybrid_container(report, *image, pc, config.max_island_entries, config.external_m68k_facts);
      if (site.container == GenesisHybridContainer::whole_image) whole = true;
      else merge_into(grown[pc], site.entries);
      if (site.external) ++plan.external_facts_applied;
      plan.sites.emplace(pc, std::move(site));
    }
    if (whole) {
      plan.outcome = GenesisHybridOutcome::broad_whole_image;
      plan.island_entries = islands;
      return plan;
    }
    // The mandatory materialized roots: every `static_proof` image PC, entered (opaque) from the startup entry once round 1 has
    // measured the unmodified SEG-030 D. Opaque: no proven source transfer gives their entry state.
    if (!materialized.empty()) {
      if (!entry) {
        plan.outcome = GenesisHybridOutcome::broad_validation_failed;
        plan.validation_failure = "materialized_image_without_entry";
        return plan;
      }
      merge_into(grown_opaque[*entry], materialized);
    }
    if (total_entries(grown) + total_entries(grown_opaque) > config.max_island_entries) {
      plan.outcome = GenesisHybridOutcome::broad_island_bound;
      plan.island_entries = islands;
      return plan;
    }
    if (grown == islands && grown_opaque == opaque) {
      plan.island_entries = islands;
      if (const auto failure = validate_genesis_hybrid_round(report, *image, islands, plan.universe, config.max_island_entries,
                                                              config.external_m68k_facts)) {
        plan.outcome = GenesisHybridOutcome::broad_validation_failed;
        plan.validation_failure = *failure;
        return plan;
      }
      for (const auto pc : materialized)
        if (contains(plan.universe, pc) && !report.discovered.contains(pc)) {
          plan.outcome = GenesisHybridOutcome::broad_validation_failed;
          plan.validation_failure = "materialized_image_not_admitted";
          return plan;
        }
      plan.outcome = GenesisHybridOutcome::hybrid;
      for (const auto &[pc, length] : report.discovered) {
        (void)length;
        plan.hybrid.push_back(pc);
      }
      plan.admitted.clear();
      for (const auto pc : plan.hybrid)
        if (contains(plan.universe, pc)) plan.admitted.push_back(pc);
      return plan;
    }
    islands = std::move(grown);
    opaque = std::move(grown_opaque);
  }
  plan.outcome = GenesisHybridOutcome::broad_closure_bound;
  plan.island_entries = islands;
  return plan;
}

GenesisHybridAdmissionPlan genesis_hybrid_admission_plan(const GenesisHybridPlan &plan, const FrontendProgram &program,
                                                         const std::string &rom_sha256) {
  GenesisHybridAdmissionPlan out;
  out.rom_sha256 = rom_sha256;
  out.universe_sha256 = genesis_hybrid_admission_universe_digest(plan.universe);
  out.aliases = program.immutable_copy_aliases;
  if (plan.outcome != GenesisHybridOutcome::hybrid || plan.admitted.empty()) return out;  // broad
  out.strategy = GenesisAdmissionStrategy::hybrid;
  out.ranges = genesis_hybrid_admission_ranges(plan.universe, plan.admitted);
  return out;
}

std::string format_genesis_hybrid_plan_aggregate(const GenesisHybridPlan &plan) {
  const bool hybrid = plan.outcome == GenesisHybridOutcome::hybrid;
  const std::size_t broad_u = plan.universe.size();
  const std::size_t total = hybrid ? plan.admitted.size() : broad_u;
  const std::size_t fallback = total >= plan.precise_d_in_universe ? total - plan.precise_d_in_universe : 0U;
  std::array<std::size_t, genesis_hybrid_container_count> containers{};
  containers[static_cast<std::size_t>(GenesisHybridContainer::exact)] = plan.resolved_sites;
  std::size_t islands = 0, whole = 0;
  // Fallback attribution by generic reason (family / generic reason / CPU sub-reason / container): sites and island entries.
  std::map<std::string, std::pair<std::size_t, std::size_t>> by_reason;
  for (const auto &[pc, site] : plan.sites) {
    (void)pc;
    ++containers[static_cast<std::size_t>(site.container)];
    if (site.container == GenesisHybridContainer::whole_image) ++whole;
    else if (!site.entries.empty()) ++islands;
    const std::string key = std::string(genesis_analysis_family_name(site.family)) + "/" + analysis::unknown_reason_name(site.reason) + "/" +
                            genesis_analysis_sub_reason_name(site.sub) + "/" + genesis_hybrid_container_name(site.container);
    auto &cost = by_reason[key];
    ++cost.first;
    cost.second += site.entries.size();
  }
  if (plan.materialized_entries != 0U) ++islands;
  std::ostringstream out;
  out << "{\"schema\":\"segarecomp.m68k_hybrid_plan.aggregate.v1\",\"outcome\":\"" << genesis_hybrid_outcome_name(plan.outcome)
      << "\",\"strategy\":\"" << (hybrid ? "hybrid" : "broad") << "\",\"closure_rounds\":" << plan.rounds << ",\"broad_u\":" << broad_u
      << ",\"precise_d\":" << plan.precise_d << ",\"precise_d_in_u\":" << plan.precise_d_in_universe
      << ",\"fallback_island_count\":" << islands << ",\"fallback_admitted\":" << fallback << ",\"hybrid_total\":" << total
      << ",\"hybrid_vs_broad_ratio\":" << ratio(total, broad_u) << ",\"reduction_vs_broad\":" << (broad_u - std::min(broad_u, total))
      << ",\"whole_image_fallback_count\":" << whole << ",\"materialized_entries\":" << plan.materialized_entries
      << ",\"premise_returns\":" << plan.premise_returns << ",\"sites\":{";
  for (std::size_t i = 0; i < genesis_hybrid_container_count; ++i)
    out << (i == 0U ? "" : ",") << '"' << genesis_hybrid_container_name(static_cast<GenesisHybridContainer>(i)) << "\":" << containers[i];
  out << "},\"fallback_cost_by_reason\":{";
  bool first = true;
  for (const auto &[key, cost] : by_reason) {
    out << (first ? "" : ",") << '"' << key << "\":{\"sites\":" << cost.first << ",\"island_entries\":" << cost.second << '}';
    first = false;
  }
  out << '}';
  if (!plan.validation_failure.empty()) out << ",\"validation_failure\":\"" << plan.validation_failure << '"';
  if (plan.diagnostic_transparent_handlers) out << ",\"diagnostic_ablation\":\"transparent_handlers\",\"credited\":false";
  out << ",\"external_facts_applied\":" << plan.external_facts_applied;
  out << '}';
  return out.str();
}

std::string format_genesis_hybrid_plan_private(const GenesisHybridPlan &plan) {
  std::ostringstream out;
  out << "{\"schema\":\"segarecomp.m68k_hybrid_plan.private.v1\",\"aggregate\":" << format_genesis_hybrid_plan_aggregate(plan)
      << ",\"sites\":[";
  bool first = true;
  for (const auto &[pc, site] : plan.sites) {
    out << (first ? "" : ",") << "{\"pc\":\"" << hex6(pc) << "\",\"family\":\"" << genesis_analysis_family_name(site.family)
        << "\",\"reason\":\"" << analysis::unknown_reason_name(site.reason) << "\",\"sub\":\"" << genesis_analysis_sub_reason_name(site.sub)
        << "\",\"container\":\"" << genesis_hybrid_container_name(site.container) << "\",\"entries\":" << site.entries.size() << '}';
    first = false;
  }
  out << "],\"islands\":[";
  first = true;
  for (const auto &[pc, entries] : plan.island_entries) {
    out << (first ? "" : ",") << "{\"site\":\"" << hex6(pc) << "\",\"entries\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) out << (i == 0U ? "" : ",") << '"' << hex6(entries[i]) << '"';
    out << "]}";
    first = false;
  }
  out << "]}";
  return out.str();
}

}  // namespace segarecomp
