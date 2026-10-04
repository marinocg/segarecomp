#pragma once

// SEG-031 (ADR 0080): the production side of the explicit hybrid admission candidate for Genesis M68K immutable-ROM AOT.
//
// A hybrid admission plan is a build-time artifact produced by the report-only planner (`segarecomp-genesis-analysis-report
// --hybrid-plan`, which links the analysis; production never does). It names a subset of the broad immutable-ROM AOT identities
// (`FrontendAnalysis::immutable_rom_aot_entries`, the universe `U`). The emitter consumes it only through an explicit opt-in, and only
// after this owner has validated it fail-closed:
//
// - the ROM digest, the ADR 0049 alias set and the broad-universe fingerprint (the SHA-256 of the sorted broad identity execution
//   addresses, one `%08x\n` line each) equal the emission's, so a plan computed for another frontend version, entry mode or image
//   set never applies;
// - the admitted ranges are well formed (sorted, disjoint, even, non-empty) and admit at least one identity;
// - CPU-owned structural closure over the broad identities: every machine root that is a broad identity is admitted; every
//   `static_proof` alias identity is admitted; every fixed successor and stacked call continuation (`m68k_control_successors`) of an
//   admitted identity that is itself a broad identity is admitted.
//
// Dynamic-transfer containment (the island proof) is established by the planner, not here: this owner cannot see analysis facts. A
// plan that fails any check is rejected and nothing is filtered (never a partial program). A `broad` plan filters nothing. The filtered
// program is an ordinary generated-native program: no identity knows whether it came from precise discovery, an island or broad
// admission, and the runtime is unchanged.
//
// The text format (one record per line, ASCII, bounded):
//
//   segarecomp.m68k_hybrid_admission_plan.v1
//   rom_sha256 <64 lowercase hex>
//   universe_sha256 <64 lowercase hex>
//   alias <execution hex8>:<source hex8>:<length hex8>      (zero or more, ascending execution base)
//   strategy hybrid|broad
//   range <begin hex8> <end hex8>                           (hybrid only; half-open, ascending, disjoint; every broad identity whose
//                                                            execution address lies in a range is admitted)
//   end
//
// A plan holds exact addresses: for a commercial input it is a private artifact (ignored location only, never persisted).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/machine/genesis/frontend.hpp"

namespace segarecomp {

inline constexpr std::string_view genesis_hybrid_admission_plan_schema = "segarecomp.m68k_hybrid_admission_plan.v1";
// Bounds on an untrusted plan (defence in depth): the text size and the range count.
inline constexpr std::size_t genesis_hybrid_admission_plan_max_bytes = std::size_t{16} << 20U;
inline constexpr std::size_t genesis_hybrid_admission_plan_max_ranges = std::size_t{1} << 20U;

enum class GenesisAdmissionStrategy : std::uint8_t { broad, hybrid };
[[nodiscard]] const char *genesis_admission_strategy_name(GenesisAdmissionStrategy strategy) noexcept;

struct GenesisHybridAdmissionPlan {
  std::string rom_sha256;
  std::string universe_sha256;
  std::vector<FrontendProgram::ImmutableCopyAlias> aliases;  // ascending execution base
  GenesisAdmissionStrategy strategy{GenesisAdmissionStrategy::broad};
  std::vector<FrontendProgram::ImmutableRomAotRange> ranges;  // hybrid only
  friend bool operator==(const GenesisHybridAdmissionPlan &left, const GenesisHybridAdmissionPlan &right) noexcept {
    const auto same_alias = [](const FrontendProgram::ImmutableCopyAlias &a, const FrontendProgram::ImmutableCopyAlias &b) {
      return a.execution_base == b.execution_base && a.source_base == b.source_base && a.length == b.length;
    };
    const auto same_range = [](const FrontendProgram::ImmutableRomAotRange &a, const FrontendProgram::ImmutableRomAotRange &b) {
      return a.begin_address == b.begin_address && a.end_address == b.end_address;
    };
    return left.rom_sha256 == right.rom_sha256 && left.universe_sha256 == right.universe_sha256 && left.strategy == right.strategy &&
           std::equal(left.aliases.begin(), left.aliases.end(), right.aliases.begin(), right.aliases.end(), same_alias) &&
           std::equal(left.ranges.begin(), left.ranges.end(), right.ranges.begin(), right.ranges.end(), same_range);
  }
};

[[nodiscard]] std::string format_genesis_hybrid_admission_plan(const GenesisHybridAdmissionPlan &plan);
// Strict parse of the text format; nullopt (with `error` set when given) on any deviation.
[[nodiscard]] std::optional<GenesisHybridAdmissionPlan> parse_genesis_hybrid_admission_plan(std::string_view text,
                                                                                            std::string *error = nullptr);

// The maximal runs of `admitted` over the sorted broad identity addresses `universe`: every range contains only admitted identities,
// so the plan admits exactly `admitted ∩ universe`.
[[nodiscard]] std::vector<FrontendProgram::ImmutableRomAotRange> genesis_hybrid_admission_ranges(
    const std::vector<std::uint32_t> &universe, const std::vector<std::uint32_t> &admitted);

// The broad-universe fingerprint of the sorted distinct execution addresses `universe`.
[[nodiscard]] std::string genesis_hybrid_admission_universe_digest(const std::vector<std::uint32_t> &universe);

// True when `address` lies in one of the sorted disjoint half-open `ranges`.
[[nodiscard]] bool genesis_hybrid_admission_contains(const std::vector<FrontendProgram::ImmutableRomAotRange> &ranges,
                                                     std::uint32_t address);

// Validates `plan` against `program` (which must have broad immutable-ROM AOT enabled and carry the emission's aliases) and the
// emission's `rom_sha256`, then filters `entries` (the broad identities of the analysed program) to the admitted ones. Returns nullopt
// on success (including a `broad` plan, which filters nothing), or the generic reason of the first failed check, in which case
// `entries` is untouched.
[[nodiscard]] std::optional<std::string> apply_genesis_hybrid_admission(
    const FrontendProgram &program, std::string_view rom_sha256, const GenesisHybridAdmissionPlan &plan,
    std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries);

}  // namespace segarecomp
