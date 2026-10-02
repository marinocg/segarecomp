// SEG-030-T002 (ADR 0079, report-only). See report.hpp.

#include "segarecomp/genesis_analysis_report/report.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <variant>

#include "segarecomp/cpu/m68k/control_successors.hpp"

namespace segarecomp {
namespace {

using analysis::UnknownReason;

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

std::string hex6(std::uint32_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(6) << std::setfill('0') << value;
  return out.str();
}

template <typename Range>
std::string hex_list(const Range &values) {
  std::string out = "[";
  bool first = true;
  for (const auto value : values) {
    out += (first ? "\"" : ",\"") + hex6(value) + "\"";
    first = false;
  }
  return out + "]";
}

GenesisAnalysisFamily report_family(M68kDynamicControlFamily family) {
  switch (family) {
  case M68kDynamicControlFamily::jump_address_indirect: return GenesisAnalysisFamily::jmp_an;
  case M68kDynamicControlFamily::call_address_indirect: return GenesisAnalysisFamily::jsr_an;
  case M68kDynamicControlFamily::jump_address_disp16: return GenesisAnalysisFamily::jmp_d16_an;
  case M68kDynamicControlFamily::call_address_disp16: return GenesisAnalysisFamily::jsr_d16_an;
  case M68kDynamicControlFamily::jump_address_index: return GenesisAnalysisFamily::jmp_an_index;
  case M68kDynamicControlFamily::call_address_index: return GenesisAnalysisFamily::jsr_an_index;
  case M68kDynamicControlFamily::jump_pc_index:
  case M68kDynamicControlFamily::call_pc_index: return GenesisAnalysisFamily::pc_index_explicit;
  case M68kDynamicControlFamily::return_from_exception: return GenesisAnalysisFamily::rte;
  case M68kDynamicControlFamily::return_restore_condition_codes: return GenesisAnalysisFamily::rtr;
  case M68kDynamicControlFamily::return_from_subroutine: return GenesisAnalysisFamily::rts_computed;
  case M68kDynamicControlFamily::none:
  case M68kDynamicControlFamily::unclassified: break;
  }
  return GenesisAnalysisFamily::unclassified;
}

// Baseline CPU sub-reason of an unresolved PC-indexed site (the remaining sub-reasons belong to the staged domains).
GenesisAnalysisSubReason pc_index_detail(const M68kPcIndexSiteReport &site) {
  switch (site.outcome) {
  case M68kPcIndexOutcome::width_only_domain: return GenesisAnalysisSubReason::width_only;
  case M68kPcIndexOutcome::target_outside_image: return GenesisAnalysisSubReason::target_outside_image;
  case M68kPcIndexOutcome::invalidated: return GenesisAnalysisSubReason::invalidated;
  case M68kPcIndexOutcome::index_unknown:
    return site.reason == UnknownReason::set_bound ? GenesisAnalysisSubReason::set_bound : GenesisAnalysisSubReason::none;
  case M68kPcIndexOutcome::resolved:
  case M68kPcIndexOutcome::address_register_index:
  case M68kPcIndexOutcome::entry_outside_immutable_image:
  case M68kPcIndexOutcome::empty_domain: break;
  }
  return GenesisAnalysisSubReason::none;
}

std::string site_label(const GenesisAnalysisComputedSite &site) {
  if (site.resolved) return "resolved";
  return std::string(analysis::unknown_reason_name(site.reason)) + "/" + genesis_analysis_sub_reason_name(site.detail);
}

}  // namespace

const char *genesis_analysis_sub_reason_name(GenesisAnalysisSubReason reason) noexcept {
  return m68k_analysis_sub_reason_name(reason);
}

const char *genesis_analysis_family_name(GenesisAnalysisFamily family) noexcept {
  switch (family) {
  case GenesisAnalysisFamily::pc_index_explicit: return "pc_index_explicit";
  case GenesisAnalysisFamily::pc_index_width_only: return "pc_index_width_only";
  case GenesisAnalysisFamily::jsr_an: return "jsr_an";
  case GenesisAnalysisFamily::jmp_an: return "jmp_an";
  case GenesisAnalysisFamily::jsr_d16_an: return "jsr_d16_an";
  case GenesisAnalysisFamily::jmp_d16_an: return "jmp_d16_an";
  case GenesisAnalysisFamily::jsr_an_index: return "jsr_an_index";
  case GenesisAnalysisFamily::jmp_an_index: return "jmp_an_index";
  case GenesisAnalysisFamily::rte: return "rte";
  case GenesisAnalysisFamily::rtr: return "rtr";
  case GenesisAnalysisFamily::rts_computed: return "rts_computed";
  case GenesisAnalysisFamily::unclassified: return "unclassified";
  }
  return "invalid";
}

// ---------------------------------------------------------------------------------------------------------------
// Driver.

GenesisAnalysisReport run_genesis_analysis_report(const FrontendProgram &program, const GenesisAnalysisReportConfig &config) {
  GenesisAnalysisReport report{};
  report.roots = genesis_reachability_roots(program);
  const auto image = GenesisM68kAnalysisImage::create(program);
  if (!image) return report;
  report.images_valid = true;

  // The challenger's strict policy: call continuations are opaque entries; no exception or pushed-code continuation; width-only
  // index domains are not an explicit table-extent proof. The baseline configuration has nothing to grow between rounds
  // (ADR 0079 decision 9): one round, with the solver-owned pin-and-restart inside it.
  M68kAnalysisConfig adapter_config{};
  adapter_config.accept_width_domains = false;
  adapter_config.call_continuations = true;
  adapter_config.exception_continuations = false;
  adapter_config.pushed_code_continuations = false;
  adapter_config.domains.address = config.domains.address;
  report.analysis = analyze_m68k_finite_values(*image, report.roots.roots, adapter_config, config.bounds);
  report.rounds = 1U;
  if (!report.analysis.complete) return report;  // every query Unknown(bound): no partial D

  std::map<std::uint32_t, GenesisReachabilityInstruction> instructions;
  for (const auto &[point, state] : report.analysis.solution.in_states) {
    (void)state;
    const auto pc = static_cast<std::uint32_t>(point) & bus_mask;
    const auto decoded = image->decode(pc);
    if (!decoded) {
      switch (image->status(pc)) {
      case GenesisM68kAnalysisImage::Status::odd: report.odd_targets.insert(pc); break;
      case GenesisM68kAnalysisImage::Status::unmapped: report.unmapped_targets.insert(pc); break;
      case GenesisM68kAnalysisImage::Status::rejected:
      case GenesisM68kAnalysisImage::Status::decoded: report.rejected_decode_targets.insert(pc); break;
      }
      continue;
    }
    report.discovered.emplace(pc, decoded->length);
    instructions.emplace(pc, GenesisReachabilityInstruction{decoded->operation, decoded->length});
    const auto control = m68k_control_successors(decoded->operation);
    if (control.always_raises_exception) ++report.exception_raising_instructions;
    switch (control.stacked) {
    case M68kStackedContinuationKind::call_continuation: report.call_continuations.insert(control.stacked_address & bus_mask); break;
    case M68kStackedContinuationKind::exception_continuation:
      report.exception_continuations.insert(control.stacked_address & bus_mask);
      break;
    case M68kStackedContinuationKind::pushed_code_address:
      report.pushed_code_addresses.insert(control.stacked_address & bus_mask);
      break;
    case M68kStackedContinuationKind::none: break;
    }
    if (control.dynamic != M68kDynamicControlFamily::none) report.sites[static_cast<std::size_t>(control.dynamic)].insert(pc);
  }

  // ADR 0048 push-window classification over D (the challenger's own classifier).
  const auto push_window = genesis_push_window_rts(instructions);
  for (const auto site : push_window) {
    report.sites[static_cast<std::size_t>(M68kDynamicControlFamily::return_from_subroutine)].erase(site);
    report.sites[genesis_challenger_family_push_window_rts].insert(site);
  }

  // Per-site results. An ordinary RTS is modelled through its callers' opaque continuations and is not a computed site.
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family) {
    if (family == static_cast<std::uint32_t>(M68kDynamicControlFamily::return_from_subroutine)) continue;
    for (const auto pc : report.sites[family]) {
      GenesisAnalysisComputedSite site{};
      site.family = family == genesis_challenger_family_push_window_rts
                        ? GenesisAnalysisFamily::rts_computed
                        : report_family(static_cast<M68kDynamicControlFamily>(family));
      if (const auto found = report.analysis.pc_index_sites.find(pc); found != report.analysis.pc_index_sites.end()) {
        const auto &recovery = found->second;
        if (recovery.outcome == M68kPcIndexOutcome::width_only_domain) site.family = GenesisAnalysisFamily::pc_index_width_only;
        site.resolved = recovery.outcome == M68kPcIndexOutcome::resolved;
        site.reason = recovery.reason;
        site.detail = pc_index_detail(recovery);
        if (site.resolved) site.targets = recovery.targets;
      } else if (const auto address = report.analysis.address_sites.find(pc); address != report.analysis.address_sites.end()) {
        site.resolved = address->second.resolved;
        site.reason = address->second.reason;
        site.detail = address->second.sub;
        if (site.resolved) site.targets = address->second.targets;
      } else if (const auto reason = report.analysis.unresolved_computed.find(pc); reason != report.analysis.unresolved_computed.end()) {
        site.reason = reason->second;
      }
      report.computed_sites.emplace(pc, std::move(site));
    }
  }
  // Resolved PC-indexed sites are no longer unresolved dynamic sites.
  for (const auto &[pc, site] : report.analysis.pc_index_sites) {
    if (site.outcome != M68kPcIndexOutcome::resolved) continue;
    const auto family = site.call ? M68kDynamicControlFamily::call_pc_index : M68kDynamicControlFamily::jump_pc_index;
    report.sites[static_cast<std::size_t>(family)].erase(pc);
  }
  for (const auto &[pc, site] : report.analysis.address_sites)
    if (site.resolved) report.sites[static_cast<std::size_t>(site.family)].erase(pc);
  for (auto it = report.discovered.begin(); it != report.discovered.end(); ++it) {
    const auto next = std::next(it);
    if (next != report.discovered.end() && next->first < it->first + it->second) ++report.overlapping_starts;
  }
  return report;
}

std::optional<std::size_t> genesis_analysis_universe(FrontendProgram program) {
  if (!program.immutable_rom_aot_enabled && !apply_genesis_immutable_rom_aot(program)) return std::nullopt;
  const auto analysis = analyze_m68k_frontend(program);
  if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) return partial->accepted_prefix.immutable_rom_aot_entries.size();
  if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) return accepted->immutable_rom_aot_entries.size();
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------
// Output.

std::string format_genesis_analysis_report_aggregate(const GenesisAnalysisReport &report, const GenesisAnalysisReportConfig &config) {
  std::ostringstream out;
  const auto &domains = config.domains;
  out << "{\"schema\":\"segarecomp.m68k_core_report.aggregate.v1\",\"exception_model\":\"strict\",\"pea_continuations\":false"
      << ",\"domains\":{\"address\":" << (domains.address ? "true" : "false") << ",\"memory\":" << (domains.memory ? "true" : "false")
      << ",\"contexts\":" << (domains.contexts ? "true" : "false") << ",\"frames\":" << (domains.frames ? "true" : "false") << '}'
      << ",\"images_valid\":" << (report.images_valid ? "true" : "false") << ",\"root_count\":" << report.roots.roots.size()
      << ",\"vector_roots\":" << report.roots.vector_roots << ",\"discovered\":" << report.discovered.size()
      << ",\"call_continuations\":" << report.call_continuations.size()
      << ",\"exception_continuations\":" << report.exception_continuations.size()
      << ",\"pushed_code_addresses\":" << report.pushed_code_addresses.size() << ",\"overlapping_starts\":" << report.overlapping_starts
      << ",\"rejected_decode_targets\":" << report.rejected_decode_targets.size()
      << ",\"unmapped_targets\":" << report.unmapped_targets.size() << ",\"odd_targets\":" << report.odd_targets.size()
      << ",\"exception_raising_instructions\":" << report.exception_raising_instructions << ",\"sites\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << '"' << genesis_challenger_family_name(family) << "\":" << report.sites[family].size();
  out << '}';

  // PC-indexed recovery summary (adapter outcome vocabulary).
  std::map<std::string, std::size_t> outcomes;
  std::size_t resolved = 0U, resolved_jsr = 0U, max_targets = 0U;
  std::set<std::uint32_t> recovered;
  for (const auto &[pc, site] : report.analysis.pc_index_sites) {
    (void)pc;
    ++outcomes[m68k_pc_index_outcome_name(site.outcome)];
    if (site.outcome != M68kPcIndexOutcome::resolved) continue;
    ++resolved;
    if (site.call) ++resolved_jsr;
    max_targets = std::max(max_targets, site.targets.size());
    recovered.insert(site.targets.begin(), site.targets.end());
  }
  out << ",\"pc_index_recovery\":{\"sites_encountered\":" << report.analysis.pc_index_sites.size() << ",\"resolved\":" << resolved
      << ",\"unresolved\":" << report.analysis.pc_index_sites.size() - resolved << ",\"resolved_jmp\":" << resolved - resolved_jsr
      << ",\"resolved_jsr\":" << resolved_jsr << ",\"outcomes\":{";
  bool first = true;
  for (const auto &[name, count] : outcomes) {
    out << (first ? "" : ",") << '"' << name << "\":" << count;
    first = false;
  }
  out << "},\"recovered_targets\":" << recovered.size() << ",\"max_targets_per_site\":" << max_targets << '}';

  // ADR 0079 decision 10: per-family resolved / Unknown(generic reason x CPU sub-reason).
  struct FamilyCounts {
    std::size_t sites{}, resolved{};
    std::map<std::string, std::map<std::string, std::size_t>> unknown;
  };
  std::array<FamilyCounts, genesis_analysis_family_count> families{};
  for (const auto &[pc, site] : report.computed_sites) {
    (void)pc;
    auto &counts = families[static_cast<std::size_t>(site.family)];
    ++counts.sites;
    if (site.resolved) ++counts.resolved;
    else ++counts.unknown[analysis::unknown_reason_name(site.reason)][genesis_analysis_sub_reason_name(site.detail)];
  }
  out << ",\"families\":{";
  for (std::size_t family = 0; family < genesis_analysis_family_count; ++family) {
    const auto &counts = families[family];
    out << (family == 0U ? "" : ",") << '"' << genesis_analysis_family_name(static_cast<GenesisAnalysisFamily>(family))
        << "\":{\"sites\":" << counts.sites << ",\"resolved\":" << counts.resolved << ",\"unknown\":{";
    bool first_reason = true;
    for (const auto &[reason, details] : counts.unknown) {
      out << (first_reason ? "" : ",") << '"' << reason << "\":{";
      first_reason = false;
      bool first_detail = true;
      for (const auto &[detail, n] : details) {
        out << (first_detail ? "" : ",") << '"' << detail << "\":" << n;
        first_detail = false;
      }
      out << '}';
    }
    out << "}}";
  }
  out << '}';

  const auto &solution = report.analysis.solution;
  out << ",\"solver\":{\"complete\":" << (report.analysis.complete ? "true" : "false");
  if (!report.analysis.complete) out << ",\"reason\":\"" << analysis::unknown_reason_name(report.analysis.reason) << '"';
  out << ",\"iterations\":" << solution.iterations << ",\"points\":" << solution.in_states.size()
      << ",\"solver_restarts\":" << solution.restarts << ",\"solver_pinned\":" << solution.pinned.size()
      << ",\"driver_restarts\":" << report.analysis.restarts << ",\"rounds\":" << report.rounds << '}';
  const analysis::Bounds effective{std::min(config.bounds.max_iterations, analysis::default_max_iterations),
                                   std::min(config.bounds.max_points, analysis::default_max_points)};
  out << ",\"bounds\":{\"max_iterations\":" << effective.max_iterations << ",\"max_points\":" << effective.max_points
      << ",\"set_bound\":" << analysis::default_set_bound;
  if (domains.address)  // SEG-030-T003 constants (absent from the baseline output, which stays byte-identical)
    out << ",\"points_to_bound\":" << m68k_points_to_bound << ",\"exact_offset_bound\":" << m68k_exact_offset_bound
        << ",\"strided_growth_bound\":" << m68k_strided_growth_bound;
  out << '}';
  if (domains.address) {
    std::size_t resolved_sites = 0U, odd = 0U, max_site_targets = 0U;
    std::set<std::uint32_t> targets;
    for (const auto &[pc, site] : report.analysis.address_sites) {
      (void)pc;
      odd += site.odd_targets_excluded;
      if (!site.resolved) continue;
      ++resolved_sites;
      max_site_targets = std::max(max_site_targets, site.targets.size());
      targets.insert(site.targets.begin(), site.targets.end());
    }
    out << ",\"address_recovery\":{\"sites_encountered\":" << report.analysis.address_sites.size() << ",\"resolved\":" << resolved_sites
        << ",\"recovered_targets\":" << targets.size() << ",\"max_targets_per_site\":" << max_site_targets
        << ",\"odd_targets_excluded\":" << odd << '}';
  }
  if (report.universe) out << ",\"universe_immutable_rom_aot\":" << *report.universe;
  out << '}';
  return out.str();
}

std::string format_genesis_analysis_report_private(const GenesisAnalysisReport &report, std::string_view aggregate,
                                                   std::string_view extra_members) {
  std::ostringstream out;
  std::vector<std::uint32_t> discovered;
  for (const auto &[pc, length] : report.discovered) {
    (void)length;
    discovered.push_back(pc);
  }
  out << "{\"schema\":\"segarecomp.m68k_core_report.private.v1\",\"aggregate\":" << aggregate
      << ",\"roots\":" << hex_list(report.roots.roots) << ",\"discovered\":" << hex_list(discovered)
      << ",\"call_continuations\":" << hex_list(report.call_continuations)
      << ",\"exception_continuations\":" << hex_list(report.exception_continuations)
      << ",\"pushed_code_addresses\":" << hex_list(report.pushed_code_addresses)
      << ",\"rejected_decode_targets\":" << hex_list(report.rejected_decode_targets)
      << ",\"unmapped_targets\":" << hex_list(report.unmapped_targets) << ",\"sites\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << '"' << genesis_challenger_family_name(family) << "\":" << hex_list(report.sites[family]);
  out << "},\"pc_index_sites\":{";
  bool first = true;
  for (const auto &[pc, site] : report.analysis.pc_index_sites) {
    out << (first ? "" : ",") << '"' << hex6(pc) << "\":{\"outcome\":\"" << m68k_pc_index_outcome_name(site.outcome)
        << "\",\"unknown_origin\":\"none\",\"reason\":\""
        << (site.outcome == M68kPcIndexOutcome::resolved ? "none" : analysis::unknown_reason_name(site.reason))
        << "\",\"round\":0,\"targets\":" << hex_list(site.targets) << '}';
    first = false;
  }
  out << "},\"computed_sites\":{";
  first = true;
  for (const auto &[pc, site] : report.computed_sites) {
    out << (first ? "" : ",") << '"' << hex6(pc) << "\":{\"family\":\"" << genesis_analysis_family_name(site.family)
        << "\",\"outcome\":\"" << (site.resolved ? "resolved" : "unknown") << "\",\"reason\":\""
        << (site.resolved ? "none" : analysis::unknown_reason_name(site.reason)) << "\",\"detail\":\""
        << genesis_analysis_sub_reason_name(site.resolved ? GenesisAnalysisSubReason::none : site.detail)
        << "\",\"targets\":" << hex_list(site.targets) << '}';
    first = false;
  }
  out << '}' << extra_members << "}\n";
  return out.str();
}

// ---------------------------------------------------------------------------------------------------------------
// Challenger comparison (comparator only).

GenesisAnalysisChallengerComparison compare_genesis_analysis_with_challenger(const GenesisAnalysisReport &report,
                                                                             const GenesisReachabilityChallengerResult &challenger) {
  GenesisAnalysisChallengerComparison out{};
  for (const auto &[pc, length] : report.discovered) {
    (void)length;
    if (challenger.discovered.contains(pc)) ++out.both;
    else out.core_only.insert(pc);
  }
  for (const auto &[pc, length] : challenger.discovered) {
    (void)length;
    if (!report.discovered.contains(pc)) out.challenger_only.insert(pc);
  }
  for (const auto &[pc, site] : report.analysis.pc_index_sites) {
    const auto other = challenger.pc_index_sites.find(pc);
    if (other == challenger.pc_index_sites.end()) {
      ++out.pc_index_core_only_sites;
      continue;
    }
    const bool core_resolved = site.outcome == M68kPcIndexOutcome::resolved;
    const bool challenger_resolved = other->second.outcome == GenesisPcIndexOutcome::resolved;
    const bool targets_differ = site.targets != other->second.targets;
    if (core_resolved == challenger_resolved && !targets_differ) continue;
    GenesisAnalysisChallengerComparison::SiteDifference difference{};
    const auto computed = report.computed_sites.find(pc);
    difference.core = computed != report.computed_sites.end() ? site_label(computed->second) : m68k_pc_index_outcome_name(site.outcome);
    difference.challenger = genesis_pc_index_outcome_name(other->second.outcome);
    if (other->second.outcome == GenesisPcIndexOutcome::index_unknown)
      difference.challenger += std::string(":") + genesis_pc_index_unknown_origin_name(other->second.unknown_origin);
    difference.targets_differ = targets_differ;
    out.pc_index_differences.emplace(pc, std::move(difference));
  }
  for (const auto &[pc, site] : challenger.pc_index_sites) {
    (void)site;
    if (!report.analysis.pc_index_sites.contains(pc)) ++out.pc_index_challenger_only_sites;
  }
  for (std::size_t family = 0; family < genesis_challenger_family_count; ++family) {
    const auto &mine = report.sites[family];
    const auto &theirs = challenger.sites[family];
    std::vector<std::uint32_t> symmetric;
    std::set_symmetric_difference(mine.begin(), mine.end(), theirs.begin(), theirs.end(), std::back_inserter(symmetric));
    out.unresolved_site_differences[family] = symmetric.size();
  }
  return out;
}

std::string format_genesis_analysis_comparison_aggregate(const GenesisAnalysisChallengerComparison &comparison) {
  std::map<std::string, std::size_t> classes;
  std::size_t target_differences = 0U;
  for (const auto &[pc, difference] : comparison.pc_index_differences) {
    (void)pc;
    ++classes[difference.challenger + " -> " + difference.core];
    if (difference.targets_differ) ++target_differences;
  }
  std::ostringstream out;
  out << "{\"D_core\":" << comparison.both + comparison.core_only.size()
      << ",\"D_challenger\":" << comparison.both + comparison.challenger_only.size() << ",\"both\":" << comparison.both
      << ",\"core_only\":" << comparison.core_only.size() << ",\"challenger_only\":" << comparison.challenger_only.size()
      << ",\"pc_index_sites_differing\":" << comparison.pc_index_differences.size()
      << ",\"pc_index_target_sets_differing\":" << target_differences
      << ",\"pc_index_core_only_sites\":" << comparison.pc_index_core_only_sites
      << ",\"pc_index_challenger_only_sites\":" << comparison.pc_index_challenger_only_sites
      << ",\"pc_index_difference_classes\":{";
  bool first = true;
  for (const auto &[label, count] : classes) {
    out << (first ? "" : ",") << '"' << label << "\":" << count;
    first = false;
  }
  out << "},\"unresolved_site_differences\":{";
  for (std::uint32_t family = 1U; family < genesis_challenger_family_count; ++family)
    out << (family == 1U ? "" : ",") << '"' << genesis_challenger_family_name(family)
        << "\":" << comparison.unresolved_site_differences[family];
  out << "}}";
  return out.str();
}

std::string format_genesis_analysis_comparison_private(const GenesisAnalysisChallengerComparison &comparison) {
  std::ostringstream out;
  out << "{\"core_only\":" << hex_list(comparison.core_only) << ",\"challenger_only\":" << hex_list(comparison.challenger_only)
      << ",\"pc_index_differences\":{";
  bool first = true;
  for (const auto &[pc, difference] : comparison.pc_index_differences) {
    out << (first ? "" : ",") << '"' << hex6(pc) << "\":{\"core\":\"" << difference.core << "\",\"challenger\":\""
        << difference.challenger << "\",\"targets_differ\":" << (difference.targets_differ ? "true" : "false") << '}';
    first = false;
  }
  out << "}}";
  return out.str();
}

}  // namespace segarecomp
