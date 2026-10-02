#pragma once

// SEG-030-T002 (ADR 0079, report-only): the Genesis M68K instantiation driver of the SEG-029 abstract-analysis core.
//
// Report-only. This library and its executable (`segarecomp-genesis-analysis-report`) are the only non-test targets allowed to link
// the analysis (`analysis_core_boundary_test`); the `segarecomp` CLI and every production target never link it. Nothing here alters
// admission, generated output or generated execution authority, and nothing observed at run time is ever an input.
//
// 1. The executable-image view (`GenesisM68kAnalysisImage`, ADR 0079 decision 3) is backed by `genesis_m68k_executable_images`:
//    the `immutable_input` cartridge images plus the `static_proof` ADR 0049 alias images at their work-RAM execution base. Only
//    those bytes decode or execute, under the challenger's ownership rules (exactly one program-space mapping claim owns every
//    byte of an instruction, the span stays inside its mapping or alias, `general_startup` decode profile). Only `immutable_input`
//    bytes are immutable: an alias execution address is mutable work RAM and is never read as immutable.
// 2. The driver seeds the machine roots (`genesis_reachability_roots`, the challenger's own owner) with the all-Unknown state and
//    runs the CPU adapter's forward fixed point under the challenger's strict exception policy (call continuations opaque, no
//    exception or pushed-code continuation, width-only index domains rejected). Lost computed targets pin and restart.
// 3. The private output (`segarecomp.m68k_core_report.private.v1`) is a superset of the challenger's private v1 format and is
//    consumed unchanged by `tools/reachability_coverage_compare.py`. It carries exact PCs: for a commercial input keep it in an
//    ignored location and never persist it. The aggregate output carries counts and generic classes only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "segarecomp/analysis/solver.hpp"
#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/reachability_challenger.hpp"

namespace segarecomp {

// ADR 0079 decision 3: the M68K analysis image over the Genesis executable-image artifact.
class GenesisM68kAnalysisImage final : public M68kAnalysisImage {
public:
  enum class Status : std::uint8_t { decoded, odd, unmapped, rejected };

  // nullopt when the program's executable-image set does not validate (fail closed). `program` must outlive the view.
  [[nodiscard]] static std::optional<GenesisM68kAnalysisImage> create(const FrontendProgram &program);

  [[nodiscard]] std::optional<Instruction> decode(std::uint32_t pc) const override;
  [[nodiscard]] bool mapped(std::uint32_t pc) const override;
  [[nodiscard]] std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const override;

  // Why `pc` does (not) decode, in the challenger's vocabulary (report use).
  [[nodiscard]] Status status(std::uint32_t pc) const;
  [[nodiscard]] const GenesisM68kExecutableImages &images() const noexcept { return images_; }

private:
  GenesisM68kAnalysisImage(const FrontendProgram &program, GenesisM68kExecutableImages images)
      : program_(&program), images_(std::move(images)) {}
  // Index (in `images_`) of the `immutable_input` cartridge image whose mapping claim is the unique program-space claim owning
  // `address`, else nullopt.
  [[nodiscard]] std::optional<std::size_t> unique_cartridge(std::uint32_t address) const;
  // Index of the `static_proof` alias image whose execution mapping contains `pc`, else nullopt.
  [[nodiscard]] std::optional<std::size_t> alias_at(std::uint32_t pc) const;
  [[nodiscard]] Status decode_into(std::uint32_t pc, std::optional<Instruction> &out) const;

  const FrontendProgram *program_;
  GenesisM68kExecutableImages images_;
};

// ADR 0079 decision 10: CPU-owned sub-reasons; each Unknown carries one generic reason plus one of these.
enum class GenesisAnalysisSubReason : std::uint8_t {
  none,
  base_unknown,
  region_exit,
  set_bound,
  target_outside_image,
  width_only,
  store_poison,
  async_writer,
  initial_memory,
  external_writer,
  context_bound,
  stack_unbalanced,
  frame_unproven,
  interrupt_resumption,
  invalidated,
};
inline constexpr std::size_t genesis_analysis_sub_reason_count = 15U;
[[nodiscard]] const char *genesis_analysis_sub_reason_name(GenesisAnalysisSubReason reason) noexcept;

// ADR 0079 decision 10: report families of computed control sites.
enum class GenesisAnalysisFamily : std::uint8_t {
  pc_index_explicit,
  pc_index_width_only,
  jsr_an,
  jmp_an,
  jsr_d16_an,
  jmp_d16_an,
  jsr_an_index,
  jmp_an_index,
  rte,
  rtr,
  rts_computed,
  unclassified,
};
inline constexpr std::size_t genesis_analysis_family_count = 12U;
[[nodiscard]] const char *genesis_analysis_family_name(GenesisAnalysisFamily family) noexcept;

// ADR 0079 decision 5: the staged domains (Phase A: only the baseline is implemented; every flag must be false).
struct GenesisAnalysisDomains {
  bool address{};
  bool memory{};
  bool contexts{};
  bool frames{};
  [[nodiscard]] bool baseline() const noexcept { return !address && !memory && !contexts && !frames; }
};

struct GenesisAnalysisReportConfig {
  GenesisAnalysisDomains domains{};
  analysis::Bounds bounds{};
};

struct GenesisAnalysisComputedSite {
  GenesisAnalysisFamily family{GenesisAnalysisFamily::unclassified};
  bool resolved{};
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};  // when unresolved
  GenesisAnalysisSubReason detail{GenesisAnalysisSubReason::none};                // when unresolved
  std::vector<std::uint32_t> targets;  // sorted exact targets when resolved
};

struct GenesisAnalysisReport {
  bool images_valid{};
  GenesisReachabilityRoots roots;
  M68kFiniteAnalysisResult analysis;
  std::map<std::uint32_t, std::uint32_t> discovered;  // D: decoded reached instruction start -> length
  std::set<std::uint32_t> call_continuations;
  std::set<std::uint32_t> exception_continuations;
  std::set<std::uint32_t> pushed_code_addresses;
  std::set<std::uint32_t> rejected_decode_targets;
  std::set<std::uint32_t> unmapped_targets;
  std::set<std::uint32_t> odd_targets;
  std::array<std::set<std::uint32_t>, genesis_challenger_family_count> sites{};  // unresolved dynamic sites (challenger families)
  std::map<std::uint32_t, GenesisAnalysisComputedSite> computed_sites;            // every computed control site in D
  std::uint64_t overlapping_starts{};
  std::uint64_t exception_raising_instructions{};
  std::uint32_t rounds{};  // driver rounds (ADR 0079 decision 9; the baseline configuration converges in one)
  std::optional<std::size_t> universe;  // U, when requested
};

[[nodiscard]] GenesisAnalysisReport run_genesis_analysis_report(const FrontendProgram &program,
                                                                const GenesisAnalysisReportConfig &config);

// U: the unchanged broad Gen-2 immutable-ROM AOT identity count on an independent copy of the program (never an input to D).
// nullopt when the broad analysis rejects the program.
[[nodiscard]] std::optional<std::size_t> genesis_analysis_universe(FrontendProgram program);

// Aggregate-only JSON (counts and generic classes; never an address).
[[nodiscard]] std::string format_genesis_analysis_report_aggregate(const GenesisAnalysisReport &report,
                                                                   const GenesisAnalysisReportConfig &config);
// PRIVATE JSON with exact PCs (challenger private v1 superset). `aggregate` is the embedded aggregate object (normally
// `format_genesis_analysis_report_aggregate`, possibly extended by the caller); `extra_members` is appended verbatim as further
// top-level members (empty, or starting with ',').
[[nodiscard]] std::string format_genesis_analysis_report_private(const GenesisAnalysisReport &report, std::string_view aggregate,
                                                                 std::string_view extra_members = {});

// --compare-challenger: the challenger (strict, PC-index recovery) is a comparator only; its result never enters the report.
struct GenesisAnalysisChallengerComparison {
  std::set<std::uint32_t> core_only;        // in D(core) \ D(challenger)
  std::set<std::uint32_t> challenger_only;  // in D(challenger) \ D(core)
  std::size_t both{};
  // PC-indexed sites present in both whose resolution class or exact target set differs, with both outcome labels.
  struct SiteDifference {
    std::string core;        // "resolved" or "<generic reason>/<sub-reason>"
    std::string challenger;  // challenger outcome[:unknown_origin]
    bool targets_differ{};
  };
  std::map<std::uint32_t, SiteDifference> pc_index_differences;
  std::size_t pc_index_core_only_sites{};
  std::size_t pc_index_challenger_only_sites{};
  std::array<std::size_t, genesis_challenger_family_count> unresolved_site_differences{};  // symmetric difference per family
};
[[nodiscard]] GenesisAnalysisChallengerComparison compare_genesis_analysis_with_challenger(
    const GenesisAnalysisReport &report, const GenesisReachabilityChallengerResult &challenger);
[[nodiscard]] std::string format_genesis_analysis_comparison_aggregate(const GenesisAnalysisChallengerComparison &comparison);
[[nodiscard]] std::string format_genesis_analysis_comparison_private(const GenesisAnalysisChallengerComparison &comparison);

}  // namespace segarecomp
