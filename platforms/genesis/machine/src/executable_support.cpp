// SEG-024-T001 (experiment, report-only). See executable_support.hpp.

#include "segarecomp/machine/genesis/executable_support.hpp"

#include <array>
#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "segarecomp/cpu/m68k/effects.hpp"

namespace segarecomp {
namespace {

constexpr std::uint64_t bus_span = UINT64_C(1) << 24U;
constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

ExecutableIdentity bus_identity(std::uint32_t pc) { return pc & bus_mask; }

SupportSourceKind support_kind(M68kControlSuccessorKind kind) {
  switch (kind) {
  case M68kControlSuccessorKind::fallthrough: return SupportSourceKind::fallthrough;
  case M68kControlSuccessorKind::branch_target: return SupportSourceKind::direct_branch;
  case M68kControlSuccessorKind::conditional_target:
  case M68kControlSuccessorKind::conditional_fallthrough: return SupportSourceKind::conditional_outcome;
  case M68kControlSuccessorKind::call_target: return SupportSourceKind::direct_call;
  case M68kControlSuccessorKind::call_continuation:
  case M68kControlSuccessorKind::pushed_code_address: return SupportSourceKind::return_continuation;
  case M68kControlSuccessorKind::exception_continuation: return SupportSourceKind::exception_continuation;
  }
  return SupportSourceKind::fallthrough;
}

std::vector<IdentityInterval> bus_intervals(const M68kArchitecturalTargetInterval &interval) {
  const std::int64_t width = interval.end - interval.begin;
  if (width <= 0) return {};
  if (static_cast<std::uint64_t>(width) >= bus_span) return {{0U, bus_span}};
  const std::int64_t span = static_cast<std::int64_t>(bus_span);
  const auto low = static_cast<std::uint64_t>(((interval.begin % span) + span) % span);
  const auto high = low + static_cast<std::uint64_t>(width);
  if (high <= bus_span) return {{low, high}};
  return {{0U, high - bus_span}, {low, bus_span}};
}

// Experiment-local mirror of ADR 0048's push-window scoping rule (libs/codegen/c11/src/frontend.cpp): an
// RTS reachable within 8 stack-neutral admitted identities from an admitted MOVE.L <ea>,-(A7) accepts any
// compiled identity. Productionization must extract one shared owner instead of this mirror.
std::set<std::uint32_t> push_window_rts(const std::map<std::uint32_t, const FrontendAnalysis::ImmutableRomAotEntry *> &entries,
                                        const std::set<std::uint32_t> &block_owned) {
  constexpr unsigned window = 8U;
  std::set<std::uint32_t> result;
  for (const auto &[push_address, push_entry] : entries) {
    const auto &push = push_entry->operation;
    if (push.kind != M68kIrKind::write_move || push.size != M68kMemoryAccessWidth::long_word ||
        push.destination_ea.mode != M68kEaMode::address_predec || push.destination_ea.reg != 7U)
      continue;
    std::deque<std::pair<std::uint32_t, unsigned>> walk;
    std::set<std::uint32_t> visited;
    walk.emplace_back(push_address + push_entry->decoded.provenance.length.value, 1U);
    while (!walk.empty()) {
      const auto [address, depth] = walk.front();
      walk.pop_front();
      if (depth > window || !visited.insert(address).second) continue;
      const auto found = entries.find(address);
      if (found == entries.end() || block_owned.contains(address)) continue;
      const auto &op = found->second->operation;
      if (op.kind == M68kIrKind::return_from_subroutine) {
        result.insert(address);
        continue;
      }
      const auto effect = m68k_operation_effect(op);
      if (!effect.register_write_footprint_complete || (effect.address_register_write_mask & 0x80U) != 0U ||
          effect.stack != M68kStackEffectKind::none)
        continue;
      const auto next = address + found->second->decoded.provenance.length.value;
      if (op.kind == M68kIrKind::general_branch) {
        walk.emplace_back(effect.direct_target, depth + 1U);
        if (op.condition != M68kCondition::always) walk.emplace_back(next, depth + 1U);
      } else if (effect.pc == M68kPcEffectKind::advance) {
        walk.emplace_back(next, depth + 1U);
      }
    }
  }
  return result;
}

bool rom_owned(const FrontendProgram &program, std::uint32_t address) {
  unsigned owners = 0U;
  bool rom = false;
  for (const auto &claim : program.mapping_claims) {
    if (claim.target_begin.space != TargetAddressSpace::m68k_program) continue;
    if (address >= claim.target_begin.value && address < claim.target_end.value) {
      ++owners;
      rom = claim.name == "raw_cartridge_rom";
    }
  }
  return owners == 1U && rom;
}

}  // namespace

std::string genesis_support_family_name(std::uint32_t family) {
  if (family < m68k_dynamic_control_family_count)
    return m68k_dynamic_control_family_name(static_cast<M68kDynamicControlFamily>(family));
  if (family == genesis_support_family_push_window_rts) return "rts_push_window";
  if (family == genesis_support_family_tier1_exact) return "tier1_exact";
  return "unknown";
}

GenesisExecutableSupportRoots genesis_executable_support_roots(const FrontendProgram &program) {
  GenesisExecutableSupportRoots roots{};
  std::set<ExecutableIdentity> identities;
  if (program.startup_ingress) {
    identities.insert(bus_identity(program.startup_ingress->entry.value));
    roots.reset = 1U;
  }
  // MC68000 vectors 2..63 (0x08..0xFC). Vectors 0/1 are the reset SSP/PC; Genesis interrupt sources are
  // autovectored, so 64..255 are never selected (and overlap the cartridge header).
  roots.vector_table_present = program.image.bytes.size() >= 0x100U && program.startup_ingress.has_value() &&
                               genesis_vector_handler_bus_address(program, 0x4U).has_value();
  if (roots.vector_table_present) {
    for (std::size_t offset = 0x08U; offset <= 0xFCU; offset += 4U) {
      const auto handler = genesis_vector_handler_bus_address(program, offset);
      if (!handler) { ++roots.vectors_uninstalled; continue; }
      ++roots.vectors_installed;
      if (!rom_owned(program, *handler)) ++roots.vectors_outside_rom;
      identities.insert(bus_identity(*handler));
    }
  }
  roots.identities.assign(identities.begin(), identities.end());
  return roots;
}

GenesisExecutableSupportModel build_genesis_executable_support_model(const FrontendProgram &program,
                                                                     const FrontendAnalysis &analysis,
                                                                     const GenesisExecutableSupportConfig &config) {
  GenesisExecutableSupportModel model{};
  model.roots = genesis_executable_support_roots(program);
  auto &input = model.input;
  input.family_count = genesis_support_family_count;
  input.roots = model.roots.identities;

  std::map<std::uint32_t, const FrontendAnalysis::ImmutableRomAotEntry *> entries;
  for (const auto &entry : analysis.immutable_rom_aot_entries)
    entries.emplace(entry.decoded.provenance.source.address.value, &entry);
  std::set<std::uint32_t> block_owned;
  for (const auto &block : analysis.static_blocks)
    for (const auto &instruction : block.instructions) block_owned.insert(instruction.source.address.value);
  const auto compiled = [&](std::uint32_t address) {
    return entries.contains(address) || block_owned.contains(address);
  };

  // Reconstructed Gen-2 whole-program return-target set (ADR 0011 + SEG-007-T246 + ADR 0049 PEA rule).
  std::set<ExecutableIdentity> gen2_returns;
  for (const auto &frame : analysis.static_frames) gen2_returns.insert(bus_identity(frame.call.continuation.value));
  for (const auto &[address, entry] : entries) {
    const auto &op = entry->operation;
    if (op.kind == M68kIrKind::call_general || op.kind == M68kIrKind::bsr_call) {
      const auto continuation = address + entry->decoded.provenance.length.value;
      if (compiled(continuation)) gen2_returns.insert(bus_identity(continuation));
    } else if (op.kind == M68kIrKind::push_effective_address &&
               (op.source_ea.mode == M68kEaMode::absolute_word || op.source_ea.mode == M68kEaMode::absolute_long ||
                op.source_ea.mode == M68kEaMode::pc_disp16) &&
               compiled(op.source_ea.absolute_address)) {
      gen2_returns.insert(bus_identity(op.source_ea.absolute_address));
    }
  }
  model.gen2_return_targets = gen2_returns.size();
  const auto push_window = push_window_rts(entries, block_owned);
  model.push_window_rts = push_window.size();

  std::map<std::uint32_t, std::vector<ExecutableIdentity>> tier1;
  for (const auto &set : analysis.indirect_target_ea_sets) {
    auto &members = tier1[set.source_instruction.source.address.value];
    for (const auto &candidate : set.candidates) members.push_back(bus_identity(candidate.value));
  }
  for (auto &[address, members] : tier1) {
    (void)address;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
  }

  // Shared domains: one AnyImmutableRom domain per family (so the first live site of each family is
  // attributable), one reconstructed Gen-2 return domain, one empty continuation domain.
  std::array<std::uint32_t, genesis_support_family_count> any_domain{};
  any_domain.fill(UINT32_MAX);
  const auto any_for = [&](std::uint32_t family) {
    if (any_domain[family] == UINT32_MAX) {
      any_domain[family] = static_cast<std::uint32_t>(input.domains.size());
      input.domains.push_back({TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}});
    }
    return any_domain[family];
  };
  const auto gen2_return_domain = static_cast<std::uint32_t>(input.domains.size());
  input.domains.push_back({TargetDomainKind::return_continuation, DomainEvidence::existing_runtime_authority,
                           std::vector<ExecutableIdentity>(gen2_returns.begin(), gen2_returns.end()), {}});
  const auto live_return_domain = static_cast<std::uint32_t>(input.domains.size());
  input.domains.push_back({TargetDomainKind::return_continuation, DomainEvidence::existing_runtime_authority, {}, {}});
  const auto resumed_domain = static_cast<std::uint32_t>(input.domains.size());
  input.domains.push_back({TargetDomainKind::exception_continuation, DomainEvidence::cpu_semantics, {}, {}});

  std::vector<ExecutableIdentity> universe_ids;
  universe_ids.reserve(entries.size());
  for (const auto &[address, entry] : entries) {
    (void)entry;
    universe_ids.push_back(bus_identity(address));
  }
  input.universe.reserve(entries.size());
  for (const auto &[address, entry] : entries) {
    SupportCandidate candidate{bus_identity(address), {}};
    const auto support = m68k_control_support(entry->operation);
    for (const auto &successor : support.successors)
      candidate.successors.push_back({support_kind(successor.kind), bus_identity(successor.target)});
    input.universe.push_back(std::move(candidate));
    if (support.dynamic == M68kDynamicControlFamily::none) continue;
    auto family = static_cast<std::uint32_t>(support.dynamic);
    std::uint32_t domain = UINT32_MAX;
    switch (support.dynamic) {
    case M68kDynamicControlFamily::return_from_subroutine:
      if (push_window.contains(address)) {
        family = genesis_support_family_push_window_rts;
        domain = any_for(family);
      } else {
        domain = config.returns == GenesisReturnDomainModel::gen2_runtime_authority ? gen2_return_domain
                                                                                    : live_return_domain;
      }
      break;
    case M68kDynamicControlFamily::return_from_exception:
    case M68kDynamicControlFamily::return_restore_condition_codes:
      domain = config.exception_returns == GenesisExceptionReturnModel::any_candidate ? any_for(family)
                                                                                      : resumed_domain;
      break;
    case M68kDynamicControlFamily::unclassified:
      domain = any_for(family);
      break;
    default: {
      const auto proven = tier1.find(address);
      if (config.honor_tier1_exact_sets && proven != tier1.end()) {
        family = genesis_support_family_tier1_exact;
        domain = static_cast<std::uint32_t>(input.domains.size());
        input.domains.push_back({TargetDomainKind::exact_set, DomainEvidence::existing_runtime_authority,
                                 proven->second, {}});
      } else if (config.indirects == GenesisIndirectDomainModel::architectural_operand_width &&
                 support.architectural_target_interval) {
        const TargetDomain broad{TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}};
        const TargetDomain region{TargetDomainKind::bounded_region, DomainEvidence::architectural_operand_width, {},
                                  bus_intervals(*support.architectural_target_interval)};
        const auto accepted = narrow_target_domain(broad, region, universe_ids);
        if (accepted.kind == TargetDomainKind::any_candidate) {
          domain = any_for(family);
        } else {
          domain = static_cast<std::uint32_t>(input.domains.size());
          input.domains.push_back(accepted);
        }
      } else {
        domain = any_for(family);
      }
      break;
    }
    }
    input.sites.push_back({bus_identity(address), family, domain});
  }
  return model;
}

namespace {

struct NamedRun {
  std::string label;
  bool conditional{};  // relies on a stated but unproven premise
  ExecutableSupportResult result;
};

void write_counts(std::ostringstream &out, const char *name, const std::vector<std::uint64_t> &counts) {
  out << "\"" << name << "\":{";
  bool first = true;
  for (std::uint32_t family = 0; family < counts.size(); ++family) {
    if (counts[family] == 0U) continue;
    out << (first ? "" : ",") << "\"" << genesis_support_family_name(family) << "\":" << counts[family];
    first = false;
  }
  out << "}";
}

std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string percent(std::uint64_t part, std::uint64_t whole) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << (whole == 0U ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole));
  return out.str();
}

void write_run(std::ostringstream &out, const NamedRun &run, std::uint64_t universe) {
  const auto &r = run.result;
  out << "{\"label\":\"" << run.label << "\",\"sound\":" << (r.sound ? "true" : "false")
      << ",\"conditional\":" << (run.conditional ? "true" : "false") << ",\"live\":" << r.live.size()
      << ",\"live_digest\":\"" << hex64(executable_identity_digest(r.live)) << "\""
      << ",\"removable\":" << (universe - r.live.size()) << ",\"reduction_percent\":" << percent(universe - r.live.size(), universe)
      << ",\"rounds\":" << r.rounds << ",\"first_reason\":{";
  for (std::size_t k = 0; k < support_source_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << support_source_kind_name(static_cast<SupportSourceKind>(k)) << "\":"
        << r.first_reason_counts[k];
  out << "},\"live_sites_by_domain_kind\":{";
  for (std::size_t k = 0; k < target_domain_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << target_domain_kind_name(static_cast<TargetDomainKind>(k)) << "\":"
        << r.live_sites_by_domain_kind[k];
  out << "},\"first_live_by_domain_kind\":{";
  for (std::size_t k = 0; k < target_domain_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << target_domain_kind_name(static_cast<TargetDomainKind>(k)) << "\":"
        << r.first_live_by_domain_kind[k];
  out << "},";
  write_counts(out, "live_sites_by_family", r.live_sites_by_family);
  out << ",";
  write_counts(out, "first_live_by_family", r.first_live_by_family);
  out << ",\"roots_outside_universe\":" << r.roots_outside_universe << ",\"successors_outside_universe\":{";
  for (std::size_t k = 0; k < support_source_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << support_source_kind_name(static_cast<SupportSourceKind>(k)) << "\":"
        << r.successors_outside_universe[k];
  out << "},\"domain_members_outside_universe\":" << r.domain_members_outside_universe << "}";
}

ExecutableSupportOptions ignore_any() {
  ExecutableSupportOptions options{};
  options.ignore_domain_kind[static_cast<std::size_t>(TargetDomainKind::any_candidate)] = true;
  return options;
}

// |L| when every AnyImmutableRom family except `keep` is ablated, minus the all-ablated floor: the
// candidates that exist in L solely because of that one family's unresolved sites.
std::vector<std::uint64_t> sole_contributions(const ExecutableSupportInput &input, std::uint64_t floor_live) {
  std::vector<std::uint64_t> sole(input.family_count, 0U);
  std::vector<bool> any_family(input.family_count, false);
  for (const auto &site : input.sites)
    if (input.domains[site.domain].kind == TargetDomainKind::any_candidate) any_family[site.family] = true;
  for (std::uint32_t keep = 0; keep < input.family_count; ++keep) {
    if (!any_family[keep]) continue;
    ExecutableSupportOptions options{};
    options.ignore_family.assign(input.family_count, false);
    for (std::uint32_t family = 0; family < input.family_count; ++family)
      options.ignore_family[family] = any_family[family] && family != keep;
    sole[keep] = compute_executable_support(input, options).live.size() - floor_live;
  }
  return sole;
}

}  // namespace

std::string format_genesis_executable_support_report(const FrontendProgram &program, const FrontendAnalysis &analysis) {
  const GenesisExecutableSupportConfig current{};
  GenesisExecutableSupportConfig width = current;
  width.indirects = GenesisIndirectDomainModel::architectural_operand_width;
  GenesisExecutableSupportConfig conditional = current;
  conditional.returns = GenesisReturnDomainModel::live_call_continuations;
  conditional.exception_returns = GenesisExceptionReturnModel::resumed_live_boundary;
  GenesisExecutableSupportConfig conditional_width = conditional;
  conditional_width.indirects = GenesisIndirectDomainModel::architectural_operand_width;

  const auto current_model = build_genesis_executable_support_model(program, analysis, current);
  const auto width_model = build_genesis_executable_support_model(program, analysis, width);
  const auto conditional_model = build_genesis_executable_support_model(program, analysis, conditional);
  const auto conditional_width_model = build_genesis_executable_support_model(program, analysis, conditional_width);
  const auto universe = static_cast<std::uint64_t>(current_model.input.universe.size());
  ExecutableSupportOptions fixed_only{};
  fixed_only.ignore_domain_kind.fill(true);

  // Index 0: the sound answer with current facts. `*_without_any` runs are UNSOUND floors (every
  // AnyImmutableRom site ablated): the best any refinement of the remaining unresolved sites could reach.
  std::vector<NamedRun> runs;
  runs.push_back({"current_facts", false, compute_executable_support(current_model.input)});
  runs.push_back({"current_facts_without_any", false, compute_executable_support(current_model.input, ignore_any())});
  runs.push_back({"fixed_flow_only", false, compute_executable_support(current_model.input, fixed_only)});
  runs.push_back({"operand_width_regions", false, compute_executable_support(width_model.input)});
  runs.push_back({"operand_width_regions_without_any", false, compute_executable_support(width_model.input, ignore_any())});
  runs.push_back({"conditional_returns", true, compute_executable_support(conditional_model.input)});
  runs.push_back({"conditional_returns_without_any", true, compute_executable_support(conditional_model.input, ignore_any())});
  runs.push_back({"conditional_returns_width", true, compute_executable_support(conditional_width_model.input)});
  runs.push_back({"conditional_returns_width_without_any", true,
                  compute_executable_support(conditional_width_model.input, ignore_any())});
  const auto sole_current = sole_contributions(current_model.input, runs[1].result.live.size());
  const auto sole_conditional = sole_contributions(conditional_model.input, runs[6].result.live.size());

  std::vector<ExecutableIdentity> universe_ids;
  for (const auto &candidate : current_model.input.universe) universe_ids.push_back(candidate.identity);
  std::uint64_t aligned = 0U;
  for (const auto &range : program.immutable_rom_aot_ranges)
    aligned += (static_cast<std::uint64_t>(range.end_address) - range.begin_address) / 2U;
  std::uint64_t aliases = 0U;
  for (const auto &entry : analysis.immutable_rom_aot_entries)
    if (entry.execution_alias) ++aliases;

  std::ostringstream out;
  const auto &roots = current_model.roots;
  out << "{\"schema\":\"segarecomp.executable-support-experiment.v1\",\"report_only\":true,"
      << "\"universe\":{\"aligned_starts\":" << aligned << ",\"candidates\":" << universe
      << ",\"execution_alias_candidates\":" << aliases << ",\"digest\":\"" << hex64(executable_identity_digest(universe_ids))
      << "\",\"block_owned_instructions\":" << [&] {
           std::uint64_t count = 0U;
           for (const auto &block : analysis.static_blocks) count += block.instructions.size();
           return count;
         }()
      << "},\"roots\":{\"count\":" << roots.identities.size() << ",\"reset\":" << roots.reset
      << ",\"vector_table_present\":" << (roots.vector_table_present ? "true" : "false")
      << ",\"vectors_installed\":" << roots.vectors_installed << ",\"vectors_uninstalled\":" << roots.vectors_uninstalled
      << ",\"vectors_outside_rom\":" << roots.vectors_outside_rom << "},"
      << "\"gen2_return_targets\":" << current_model.gen2_return_targets
      << ",\"push_window_rts\":" << current_model.push_window_rts << ",\"dynamic_sites\":" << current_model.input.sites.size() << ",";
  write_counts(out, "sites_by_family", runs[0].result.sites_by_family);
  out << ",\"sites_by_domain_kind\":{\"current\":{";
  for (std::size_t k = 0; k < target_domain_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << target_domain_kind_name(static_cast<TargetDomainKind>(k)) << "\":"
        << runs[0].result.sites_by_domain_kind[k];
  out << "},\"operand_width\":{";
  for (std::size_t k = 0; k < target_domain_kind_count; ++k)
    out << (k == 0 ? "" : ",") << "\"" << target_domain_kind_name(static_cast<TargetDomainKind>(k)) << "\":"
        << runs[3].result.sites_by_domain_kind[k];
  out << "}},\"runs\":[";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (i != 0U) out << ",";
    write_run(out, runs[i], universe);
  }
  out << "],";
  write_counts(out, "sole_any_contribution_current", sole_current);
  out << ",";
  write_counts(out, "sole_any_contribution_conditional", sole_conditional);
  out << "}\n";
  return out.str();
}

}  // namespace segarecomp
