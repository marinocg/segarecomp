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
    if (site.reason == UnknownReason::set_bound) return GenesisAnalysisSubReason::set_bound;
    // SEG-030-T005: only the opaque continuation of a merged or recursive activation makes a data register Unknown(state_bound).
    if (site.reason == UnknownReason::state_bound) return GenesisAnalysisSubReason::context_bound;
    return GenesisAnalysisSubReason::none;
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
  // SEG-030-T005/T006: the frames domain implies the contexts domain, which implies the memory domain, which implies the address
  // domain.
  const bool frames = config.domains.frames;
  const bool contexts = config.domains.contexts || frames;
  const bool memory = config.domains.memory || contexts;
  adapter_config.domains.address = config.domains.address || memory;
  adapter_config.domains.memory = memory;
  adapter_config.domains.contexts = contexts;
  adapter_config.domains.frames = frames;
  if (memory) {
    // ADR 0079 decision 7. Without the frames domain the SR interrupt mask is not tracked, so an interrupt may be taken at any
    // boundary (handler code included): every work-RAM cell has an asynchronous writer. The handler roots are the machine-delivered
    // vector roots (every root other than the startup entry; the entry too when it is also a vector handler). With the frames
    // domain (SEG-030-T006) the asynchronous writers are derived per partition from the delivered vectors instead.
    adapter_config.memory.interrupts = !frames;
    const auto entry = program.startup_ingress ? std::optional<std::uint32_t>(program.startup_ingress->entry.value & bus_mask) : std::nullopt;
    for (const auto root : report.roots.roots)
      if (!entry || root != *entry || report.roots.vector_roots == report.roots.roots.size()) adapter_config.memory.handler_roots.push_back(root);
    adapter_config.memory.release_ranges.emplace_back(genesis_z80_control_first, genesis_z80_control_last);
    adapter_config.memory.assume_no_external_writer = config.assume_no_z80_ram_writes;
    if (frames) {
      // SEG-030-T006: the delivered vectors (ADR 0021 / ADR 0043) and, for a reset-entry program, the 68000 reset state.
      for (const auto &[vector, handler] : report.roots.vectors) adapter_config.frames.vectors.push_back({vector, handler & bus_mask});
      if (entry) adapter_config.frames.main_entries.insert(*entry);
      if (config.reset_entry && entry) {
        adapter_config.frames.reset_entry = *entry;
        adapter_config.frames.reset_ssp = image->immutable_read(0U, 4U);
      }
    }
  }
  report.analysis = analyze_m68k_finite_values(*image, report.roots.roots, adapter_config, config.bounds);
  report.rounds = memory ? report.analysis.memory.rounds : 1U;
  if (!report.analysis.complete) return report;  // every query Unknown(bound): no partial D
  if (memory) {
    auto comparator_config = adapter_config;
    comparator_config.domains.memory = false;
    comparator_config.domains.contexts = false;
    comparator_config.domains.frames = false;
    const auto comparator = analyze_m68k_finite_values(*image, report.roots.roots, comparator_config, config.bounds);
    GenesisAnalysisReport::MemoryComparison comparison{};
    if (comparator.complete) {
      comparison.discovered_without_memory = comparator.reached.size();
      const auto resolved = [](const M68kFiniteAnalysisResult &result) {
        std::set<std::uint32_t> out;
        for (const auto &[pc, site] : result.pc_index_sites)
          if (site.outcome == M68kPcIndexOutcome::resolved) out.insert(pc);
        for (const auto &[pc, site] : result.address_sites)
          if (site.resolved) out.insert(pc);
        return out;
      };
      const auto with = resolved(report.analysis);
      const auto without = resolved(comparator);
      for (const auto pc : with) comparison.sites_resolved_only_with_memory += without.contains(pc) ? 0U : 1U;
      for (const auto pc : without) comparison.sites_resolved_only_without_memory += with.contains(pc) ? 0U : 1U;
    }
    comparison.unresolved_sites = report.analysis.unresolved_computed.size();
    report.memory_comparison = comparison;
  }

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
      } else if (const auto ret = report.analysis.return_sites.find(pc); ret != report.analysis.return_sites.end()) {
        site.resolved = ret->second.resolved;
        site.reason = ret->second.reason;
        site.detail = ret->second.sub;
        if (site.resolved) site.targets = ret->second.targets;
      } else if (const auto reason = report.analysis.unresolved_computed.find(pc); reason != report.analysis.unresolved_computed.end()) {
        site.reason = reason->second;
      }
      report.computed_sites.emplace(pc, std::move(site));
    }
  }
  // SEG-030-T006: an RTS away from its activation's entry stack delta is a computed RTS (frames domain), push-window or not.
  for (const auto &[pc, ret] : report.analysis.return_sites) {
    if (ret.family != M68kDynamicControlFamily::return_from_subroutine || report.computed_sites.contains(pc)) continue;
    GenesisAnalysisComputedSite site{};
    site.family = GenesisAnalysisFamily::rts_computed;
    site.resolved = ret.resolved;
    site.reason = ret.reason;
    site.detail = ret.sub;
    if (site.resolved) site.targets = ret.targets;
    report.computed_sites.emplace(pc, std::move(site));
  }
  for (const auto &[pc, ret] : report.analysis.return_sites) {
    if (!ret.resolved) continue;
    report.sites[static_cast<std::size_t>(ret.family)].erase(pc);
    report.sites[genesis_challenger_family_push_window_rts].erase(pc);
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
  auto domains = config.domains;
  domains.contexts = domains.contexts || domains.frames;  // the frames domain implies the contexts domain
  domains.memory = domains.memory || domains.contexts;   // the contexts domain implies the memory domain
  domains.address = domains.address || domains.memory;  // the memory domain implies the address domain
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
  if (domains.memory) out << ",\"memory_cell_bound\":" << m68k_memory_cell_bound << ",\"round_bound\":" << m68k_memory_round_bound;
  if (domains.contexts) out << ",\"context_depth\":" << m68k_context_depth << ",\"context_bound\":" << m68k_context_bound;
  if (domains.frames)
    out << ",\"instance_depth_bound\":" << m68k_instance_depth_bound << ",\"instance_tag_bound\":" << m68k_max_instance_tag
        << ",\"instance_growth_bound\":" << m68k_instance_growth_bound;
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
  if (domains.memory) {
    const auto &memory = report.analysis.memory;
    out << ",\"memory\":{\"converged\":" << (memory.converged ? "true" : "false") << ",\"rounds\":" << memory.rounds
        << ",\"external_writer\":" << (memory.policy.external_writer ? "true" : "false")
        << ",\"z80_release_store\":" << (memory.release_store ? "true" : "false")
        << ",\"async_all\":" << (memory.policy.async_all ? "true" : "false") << ",\"async_ranges\":" << memory.policy.async.size()
        << ",\"release_stores\":" << memory.release_stores << ",\"unknown_target_stores\":" << memory.unknown_target_stores
        << ",\"undescribed_writers\":" << memory.undescribed_writers << ",\"handler_points\":" << memory.handler_points
        << ",\"handler_store_sites\":" << memory.handler_store_sites << ",\"max_cells\":" << memory.max_cells
        << ",\"reads\":{\"precise\":" << memory.precise_reads << ",\"unknown\":{";
    std::map<std::string, std::map<std::string, std::size_t>> reads;
    for (const auto &[key, count] : memory.unknown_reads)
      reads[analysis::unknown_reason_name(key.first)][genesis_analysis_sub_reason_name(key.second)] += count;
    bool first_reason = true;
    for (const auto &[reason, details] : reads) {
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
    if (report.memory_comparison) {
      const auto &comparison = *report.memory_comparison;
      out << ",\"comparator_address_only\":{\"discovered\":" << comparison.discovered_without_memory
          << ",\"sites_resolved_only_with_memory\":" << comparison.sites_resolved_only_with_memory
          << ",\"sites_resolved_only_without_memory\":" << comparison.sites_resolved_only_without_memory
          << ",\"unresolved_sites\":" << comparison.unresolved_sites << '}';
    }
    out << '}';
    if (domains.contexts) {
      const auto &contexts = report.analysis.contexts;
      const auto subs = [&](const std::map<GenesisAnalysisSubReason, std::size_t> &counts) {
        std::string text = "{";
        for (const auto &[sub, n] : counts)
          text += (text.size() == 1U ? "\"" : ",\"") + std::string(genesis_analysis_sub_reason_name(sub)) + "\":" + std::to_string(n);
        return text + "}";
      };
      out << ",\"contexts\":{\"validated\":" << (contexts.validated ? "true" : "false")
          << ",\"converged\":" << (contexts.converged ? "true" : "false");
      if (!contexts.validated) out << ",\"reason\":\"" << analysis::unknown_reason_name(contexts.reason) << '"';
      out << ",\"rounds\":" << contexts.rounds << ",\"returned_round\":" << contexts.returned_round
          << ",\"total_iterations\":" << contexts.total_iterations << ",\"call_site_contexts\":" << contexts.contexts
          << ",\"max_contexts_per_callee\":" << contexts.max_contexts_per_callee << ",\"merged_callees\":" << contexts.merged_callees
          << ",\"activations\":" << contexts.activations << ",\"balanced_activations\":" << contexts.balanced_activations
          << ",\"recursive_activations\":" << contexts.recursive_activations << ",\"summaries\":" << contexts.summaries
          << ",\"unproven\":" << subs(contexts.unproven) << ",\"summary_continuations\":" << contexts.summary_continuations
          << ",\"opaque_continuations\":" << subs(contexts.opaque_continuations)
          << ",\"comparator_memory_only\":{\"discovered\":" << contexts.discovered_without_contexts
          << ",\"sites_resolved_only_with_contexts\":" << contexts.sites_resolved_only_with_contexts
          << ",\"sites_resolved_only_without_contexts\":" << contexts.sites_resolved_only_without_contexts
          << ",\"unresolved_sites\":" << contexts.unresolved_sites << "}}";
    }
    if (domains.frames) {
      const auto &frames = report.analysis.frames;
      out << ",\"frames\":{\"validated\":" << (frames.validated ? "true" : "false");
      if (!frames.validated)
        out << ",\"reason\":\"" << analysis::unknown_reason_name(frames.reason) << "\",\"failure\":\"" << frames.failure << '"';
      std::map<std::string, std::map<std::string, std::size_t>> returns;
      std::map<std::string, std::size_t> resolved_returns;
      for (const auto &[pc, ret] : report.analysis.return_sites) {
        (void)pc;
        const std::string family = m68k_dynamic_control_family_name(ret.family);
        if (ret.resolved) ++resolved_returns[family];
        else ++returns[family][std::string(analysis::unknown_reason_name(ret.reason)) + "/" + genesis_analysis_sub_reason_name(ret.sub)];
      }
      const auto counts = [](const std::map<std::string, std::size_t> &map) {
        std::string text = "{";
        for (const auto &[name, n] : map) text += (text.size() == 1U ? "\"" : ",\"") + name + "\":" + std::to_string(n);
        return text + "}";
      };
      out << ",\"warm_rounds\":" << frames.warm_rounds << ",\"frame_rounds\":" << frames.frame_rounds
          << ",\"reset_state\":" << (frames.reset_state ? "true" : "false") << ",\"handler_vectors\":" << frames.handler_vectors
          << ",\"instances\":{\"analysed\":" << frames.instances << ",\"interrupt\":" << frames.interrupt_instances
          << ",\"synchronous_resuming\":" << frames.resuming_instances << ",\"synchronous\":" << frames.synchronous_instances
          << ",\"dead_handlers\":" << frames.dead_handlers << ",\"unanalysed\":" << counts(frames.unanalysed)
          << ",\"frame_integrity_failures\":" << frames.frame_integrity_failures
          << ",\"clobbered_partitions\":" << frames.clobbered_partitions << '}'
          << ",\"points\":{\"live\":" << frames.points << ",\"status_unknown\":" << frames.status_unknown
          << ",\"supervisor_proven\":" << frames.supervisor_proven << ",\"interrupt_eligible\":" << frames.interrupt_eligible
          << ",\"interrupt_masked\":" << frames.interrupt_masked << ",\"raising\":" << frames.raising_points
          << ",\"frame_unknown_a7\":" << frames.frame_unknown_a7
          << ",\"frame_supervisor_unproven\":" << frames.frame_unproven_supervisor
          << ",\"frame_a7_unknown_by_reason\":" << counts(frames.frame_a7_unknown_by_reason) << '}'
          << ",\"first_round_points\":{\"live\":" << frames.first_round_points
          << ",\"status_unknown\":" << frames.first_round_status_unknown
          << ",\"interrupt_eligible\":" << frames.first_round_interrupt_eligible
          << ",\"interrupt_masked\":" << frames.first_round_interrupt_masked
          << ",\"frame_unknown_a7\":" << frames.first_round_frame_unknown_a7
          << ",\"frame_supervisor_unproven\":" << frames.first_round_frame_unproven_supervisor
          << ",\"frame_a7_unknown_by_reason\":" << counts(frames.first_round_a7_unknown_by_reason) << '}'
          << ",\"main_async\":{\"all\":" << (frames.main_async_all ? "true" : "false") << ",\"ranges\":" << frames.main_async_ranges
          << ",\"bytes\":" << frames.main_async_bytes << ",\"unknown_target_writer_stores\":" << frames.unknown_target_writer_stores
          << '}' << ",\"returns\":{";
      bool first_family = true;
      for (const auto *family : {"rte", "rtr", "rts"}) {
        out << (first_family ? "" : ",") << '"' << family << "\":{\"resolved\":" << resolved_returns[family] << ",\"unknown\":{";
        first_family = false;
        bool first_class = true;
        for (const auto &[label, n] : returns[family]) {
          out << (first_class ? "" : ",") << '"' << label << "\":" << n;
          first_class = false;
        }
        out << "}}";
      }
      out << "}}";
    }
    if (config.assume_no_z80_ram_writes)
      out << ",\"diagnostic_premise_ablation\":\"assume_no_z80_ram_writes (NOT credited: D, recall and resolved counts of this run "
             "are diagnostic sensitivity only)\"";
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
