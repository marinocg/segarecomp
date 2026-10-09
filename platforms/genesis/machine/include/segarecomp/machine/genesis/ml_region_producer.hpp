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

}  // namespace segarecomp
