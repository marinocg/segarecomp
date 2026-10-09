#pragma once

// SEG-047 (ADR 0096): the NATIVE Genesis M68K executable-region proposal producer for the frozen SEG-046 v1 model.
//
// ML is a heuristic *proposal* producer for an executable region `R` only. This owner computes the exact `seg046-features-v1` vector of
// every 512-byte ROM window (byte statistics here, MC68000 decoder/control columns from `genesis_window_feature_rows`, previous/next
// context with an all-zero base vector beyond the image ends), scores it with the folded logistic model and selects windows whose
// logit >= the frozen logit threshold. It never proves anything: pruning, the admission validator and the generated-native dispatcher
// remain the correctness authority. No ML runs in generated programs; there is no interpreter, JIT or runtime decoder.
//
// Numeric contract (ADR 0096 section 4): IEEE-754 binary64, fixed left-to-right accumulation, no contraction/fast-math (the translation
// unit is built with -ffp-contract=off), non-strict threshold compare, set equality (not bit equality) with the canonical reference.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace segarecomp {

inline constexpr std::size_t genesis_ml_window_bytes = 512U;
inline constexpr std::size_t genesis_ml_byte_feature_count = 16U;
inline constexpr std::size_t genesis_ml_decoder_feature_count = 31U;
inline constexpr std::size_t genesis_ml_base_feature_count = genesis_ml_byte_feature_count + genesis_ml_decoder_feature_count;  // 47
inline constexpr std::size_t genesis_ml_feature_count = 3U * genesis_ml_base_feature_count;                                      // 141
inline constexpr std::string_view genesis_ml_feature_version = "seg046-features-v1";

// Length of `zlib.compress(bytes, 6)` (zlib wrapper, default strategy/windowBits/memLevel) computed by the vendored zlib 1.3.1; 0 on a
// zlib failure (never produced for a window).
[[nodiscard]] std::size_t genesis_ml_zlib_level6_length(std::span<const std::uint8_t> bytes);

// The 16 byte-statistic features of one window (empty window => zeros), in schema order.
[[nodiscard]] std::array<double, genesis_ml_byte_feature_count> genesis_ml_byte_features(std::span<const std::uint8_t> window);

// The 31 decoder/control features from the 27 raw window columns; nullptr (no identity in the window) => zeros.
[[nodiscard]] std::array<double, genesis_ml_decoder_feature_count> genesis_ml_decoder_features(const std::vector<std::uint64_t> *row);

// Row-major (windows x 141) feature matrix of the whole image. Windows are ceil(size/512); a final short window is accepted.
// `rows` is `genesis_window_feature_rows(entries, 512)`.
[[nodiscard]] std::vector<double> genesis_ml_window_matrix(std::span<const std::uint8_t> rom,
                                                           const std::map<std::uint32_t, std::vector<std::uint64_t>> &rows);

// ---- Folded logistic scorer (T003) ----------------------------------------------------------------------------------------------
// `logit = bias; for i in 0..140: logit += weight[i] * x[i]` (left to right, one binary64 multiply and add per feature, no contraction).
// Free function so tests can score synthetic parameter sets independently of the embedded model.
[[nodiscard]] double genesis_ml_fold_score(const double *weights, double bias, const double *features, std::size_t count) noexcept;

// The embedded frozen model: identity constants and the folded parameters (generated from tools/segarecomp_ml_region.model.json).
[[nodiscard]] double genesis_ml_logit(std::span<const double> features);  // 141 values, embedded folded model; NaN on a wrong width
[[nodiscard]] double genesis_ml_logit_threshold() noexcept;
// Fail-closed identity check of the embedded model against the frozen v1 constants (feature version, count, window size, the SHA-256 of
// the canonical schema payload built from the embedded feature names, and the original artifact digest). Empty string => ok; otherwise a
// stable sanitized reason ("model_identity").
[[nodiscard]] std::string genesis_ml_model_identity_failure();

// Windows (indices) whose logit >= the frozen logit threshold (non-strict), ascending. Empty on a malformed matrix.
[[nodiscard]] std::vector<std::uint32_t> genesis_ml_select_windows(std::span<const double> matrix);
// Merges ascending window indices (512 B each, final range clipped to `rom_size`) into half-open ascending disjoint ranges.
[[nodiscard]] std::vector<FrontendProgram::ImmutableRomAotRange> genesis_ml_window_ranges(const std::vector<std::uint32_t> &windows,
                                                                                       std::size_t rom_size);

// ---- Region proposal (T004/T005) ------------------------------------------------------------------------------------------------
// Resource bound on the image the producer accepts (defence in depth; a Genesis cartridge is at most 4 MiB).
inline constexpr std::size_t genesis_ml_max_rom_bytes = std::size_t{32} << 20U;

struct GenesisMlRegionStats {
  std::size_t windows{};          // ceil(size / 512)
  std::size_t ml_selected{};      // windows scored at/above the threshold
  std::size_t seed_windows{};     // windows holding a precise direct-control-discovery identity (includes machine roots)
  std::size_t final_selected{};   // |ML ∪ seeds|
  std::size_t region_bytes{};     // bytes of the final proposal
  double min_logit_margin{};      // min over windows of |logit - threshold| (reported for the exact-parity gate)
};

struct GenesisMlRegionResult {
  std::optional<GenesisExecutableRegionProposal> proposal;  // R = ML-selected ∪ certain-code windows (nullopt on failure)
  std::optional<GenesisExecutableRegionProposal> ml_only;   // ML-selected windows only (diagnostic / parity)
  GenesisMlRegionStats stats;
  std::string failure;  // stable sanitized reason when `proposal` is empty: model_identity | rom_size | empty_proposal
};

// The frozen v1 proposal: features of every 512-byte window of `rom` (decoder columns from the broad identities `entries`), folded scoring,
// threshold, union with the windows of `seed_addresses` (the precise direct-control discovery instruction addresses). Pure and
// deterministic; never mutates its inputs; fail-closed with a stable reason. It does not validate or prune anything.
[[nodiscard]] GenesisMlRegionResult propose_genesis_ml_executable_regions(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                                                                          const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries,
                                                                          std::span<const std::uint32_t> seed_addresses);

}  // namespace segarecomp
