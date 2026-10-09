// SEG-047 (ADR 0096): native seg046-features-v1 extraction (and, from T003, the folded scorer). See ml_region_producer.hpp.

#include "segarecomp/machine/genesis/ml_region_producer.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <cmath>
#include <cstring>
#include <limits>

#include "segarecomp/sha256.hpp"

#include "zlib.h"

namespace segarecomp {

namespace {
#include "ml_region_model_v1_data.inc"
}  // namespace

std::size_t genesis_ml_zlib_level6_length(std::span<const std::uint8_t> bytes) {
  z_stream stream{};
  if (deflateInit(&stream, 6) != Z_OK) return 0U;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(deflateBound(&stream, static_cast<uLong>(bytes.size()))));
  stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(bytes.data()));  // zlib's API is not const-correct; input is read only
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int status = deflate(&stream, Z_FINISH);
  const std::size_t length = status == Z_STREAM_END ? static_cast<std::size_t>(stream.total_out) : 0U;
  (void)deflateEnd(&stream);
  return length;
}

std::array<double, genesis_ml_byte_feature_count> genesis_ml_byte_features(std::span<const std::uint8_t> window) {
  std::array<double, genesis_ml_byte_feature_count> out{};
  const std::size_t size = window.size();
  if (size == 0U) return out;
  const double n = static_cast<double>(size);
  std::array<std::uint64_t, 256> counts{};
  for (const auto byte : window) ++counts[byte];
  // The v1 reference sums the entropy terms with Python's builtin sum(), which since CPython 3.12 is Neumaier-compensated; the frozen
  // features were produced that way, so the same compensated left-to-right (ascending byte value) summation is the contract.
  double sum = 0.0, compensation = 0.0;
  for (const auto c : counts) {
    if (c == 0U) continue;
    const double p = static_cast<double>(c) / n;
    const double term = p * std::log2(p);
    const double total = sum + term;
    if (std::fabs(sum) >= std::fabs(term)) compensation += (sum - total) + term;
    else compensation += (term - total) + sum;
    sum = total;
  }
  if (compensation != 0.0 && std::isfinite(compensation)) sum += compensation;
  const double entropy = -sum;
  std::uint64_t printable = 0U, distinct = 0U;
  for (std::size_t b = 0x20U; b < 0x7FU; ++b) printable += counts[b];
  for (const auto c : counts) distinct += c != 0U ? 1U : 0U;
  std::size_t longest = 1U, run = 1U;
  for (std::size_t i = 1U; i < size; ++i) {
    run = window[i] == window[i - 1U] ? run + 1U : 1U;
    longest = std::max(longest, run);
  }
  std::vector<std::uint16_t> words;
  for (std::size_t i = 0U; i + 1U < size; i += 2U)
    words.push_back(static_cast<std::uint16_t>((static_cast<unsigned>(window[i]) << 8U) | window[i + 1U]));
  std::size_t repeats = 0U;
  for (std::size_t i = 1U; i < words.size(); ++i) repeats += words[i] == words[i - 1U] ? 1U : 0U;
  const std::size_t denominator = std::max<std::size_t>(words.empty() ? 0U : words.size() - 1U, 1U);
  const double compressed = static_cast<double>(genesis_ml_zlib_level6_length(window)) / n;
  out[0] = entropy / 8.0;
  out[1] = static_cast<double>(counts[0]) / n;
  out[2] = static_cast<double>(counts[255]) / n;
  out[3] = static_cast<double>(printable) / n;
  out[4] = static_cast<double>(distinct) / 256.0;
  out[5] = static_cast<double>(longest) / n;
  out[6] = static_cast<double>(repeats) / static_cast<double>(denominator);
  out[7] = std::min(compressed, 2.0);
  for (std::size_t bin = 0U; bin < 8U; ++bin) {
    std::uint64_t total = 0U;
    for (std::size_t b = bin * 32U; b < (bin + 1U) * 32U; ++b) total += counts[b];
    out[8U + bin] = static_cast<double>(total) / n;
  }
  return out;
}

namespace {
// Column order of genesis_window_feature_rows: ident len2 len4 len6 len8p fam_move fam_arith fam_logic_bit fam_shift fam_control fam_other
// cond_branch uncond_direct call_direct call_any ret indirect terminator exception edge_out edge_same edge_adjacent edge_far_ident
// edge_dangling fall_dangling edge_in_local edge_in_external.
constexpr std::size_t column_count = 27U;
constexpr std::size_t col_edge_out = 19U, col_edge_same = 20U, col_edge_adjacent = 21U, col_edge_far = 22U, col_edge_dangling = 23U,
                      col_edge_in_local = 25U;
}  // namespace

std::array<double, genesis_ml_decoder_feature_count> genesis_ml_decoder_features(const std::vector<std::uint64_t> *row) {
  std::array<double, genesis_ml_decoder_feature_count> out{};
  if (row == nullptr || row->size() != column_count) return out;
  const double words = static_cast<double>(genesis_ml_window_bytes) / 2.0;
  const double ident = static_cast<double>((*row)[0]);
  std::size_t at = 0U;
  out[at++] = ident / words;
  for (std::size_t column = 1U; column < column_count; ++column) {
    const double value = static_cast<double>((*row)[column]);
    if (column >= col_edge_in_local) out[at++] = value / words;
    else out[at++] = (*row)[0] != 0U ? value / ident : 0.0;
  }
  const double edge_out = static_cast<double>((*row)[col_edge_out]);
  for (const std::size_t column : {col_edge_same, col_edge_adjacent, col_edge_dangling, col_edge_far})
    out[at++] = (*row)[col_edge_out] != 0U ? static_cast<double>((*row)[column]) / edge_out : 0.0;
  return out;
}

std::vector<double> genesis_ml_window_matrix(std::span<const std::uint8_t> rom,
                                             const std::map<std::uint32_t, std::vector<std::uint64_t>> &rows) {
  const std::size_t count = (rom.size() + genesis_ml_window_bytes - 1U) / genesis_ml_window_bytes;
  std::vector<double> base(count * genesis_ml_base_feature_count, 0.0);
  for (std::size_t w = 0U; w < count; ++w) {
    const std::size_t begin = w * genesis_ml_window_bytes;
    const std::size_t end = std::min(begin + genesis_ml_window_bytes, rom.size());
    const auto bytes = genesis_ml_byte_features(rom.subspan(begin, end - begin));
    const auto found = rows.find(static_cast<std::uint32_t>(w));
    const auto decoder = genesis_ml_decoder_features(found == rows.end() ? nullptr : &found->second);
    double *dst = base.data() + w * genesis_ml_base_feature_count;
    std::copy(bytes.begin(), bytes.end(), dst);
    std::copy(decoder.begin(), decoder.end(), dst + genesis_ml_byte_feature_count);
  }
  std::vector<double> matrix(count * genesis_ml_feature_count, 0.0);
  for (std::size_t w = 0U; w < count; ++w) {
    double *dst = matrix.data() + w * genesis_ml_feature_count;
    const double *self = base.data() + w * genesis_ml_base_feature_count;
    std::copy(self, self + genesis_ml_base_feature_count, dst);
    if (w > 0U) std::copy(self - genesis_ml_base_feature_count, self, dst + genesis_ml_base_feature_count);
    if (w + 1U < count) std::copy(self + genesis_ml_base_feature_count, self + 2U * genesis_ml_base_feature_count, dst + 2U * genesis_ml_base_feature_count);
  }
  return matrix;
}

double genesis_ml_fold_score(const double *weights, double bias, const double *features, std::size_t count) noexcept {
  double logit = bias;
  for (std::size_t i = 0U; i < count; ++i) {
    const double product = weights[i] * features[i];  // stored before the add: no fused multiply-add
    logit += product;
  }
  return logit;
}

double genesis_ml_logit(std::span<const double> features) {
  if (features.size() != genesis_ml_feature_count) return std::numeric_limits<double>::quiet_NaN();
  return genesis_ml_fold_score(ml_folded_weight, ml_folded_bias, features.data(), genesis_ml_feature_count);
}

double genesis_ml_logit_threshold() noexcept { return ml_logit_threshold; }

std::string genesis_ml_model_identity_failure() {
  // Frozen v1 identity constants (ADR 0096 section 2). The embedded table carries its own copy; both must agree.
  constexpr std::string_view frozen_schema = "d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570";
  constexpr std::string_view frozen_artifact = "b5ddae5aa0fe6571455803c244d2a6be4a2c3349e3436c8780bfc3d8aa8fa62b";
  constexpr double frozen_probability_threshold = 0.00835406801187952;
  if (std::string_view(ml_feature_version) != genesis_ml_feature_version) return "model_identity";
  if (sizeof(ml_feature_names) / sizeof(ml_feature_names[0]) != genesis_ml_feature_count) return "model_identity";
  // json.dumps({"version", "window_bytes", "features"}, sort_keys=True): keys sorted features < version < window_bytes.
  std::string payload = "{\"features\": [";
  for (std::size_t i = 0U; i < genesis_ml_feature_count; ++i) {
    if (i != 0U) payload += ", ";
    payload += '"';
    payload += ml_feature_names[i];
    payload += '"';
  }
  payload += "], \"version\": \"" + std::string(genesis_ml_feature_version) + "\", \"window_bytes\": " + std::to_string(genesis_ml_window_bytes) + "}";
  const std::string digest = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()));
  if (digest != frozen_schema || std::string_view(ml_feature_schema_sha256) != frozen_schema) return "model_identity";
  if (std::string_view(ml_original_artifact_sha256) != frozen_artifact) return "model_identity";
  if (ml_probability_threshold != frozen_probability_threshold) return "model_identity";
  // The values actually used by the scorer are bound too: the logit threshold, the folded bias and a digest of the folded weight table
  // (little-endian binary64, index order) must equal the frozen v1 constants, so any edited parameter fails closed.
  constexpr double frozen_logit_threshold = -4.776617512896361, frozen_folded_bias = -18.84590147386012;
  constexpr std::string_view frozen_weights_digest = "72a61409e3fc82020a7515019f0bf85e7bfd55573b4be0f2286d07392a9a8986";
  if (ml_logit_threshold != frozen_logit_threshold || ml_folded_bias != frozen_folded_bias) return "model_identity";
  std::vector<std::uint8_t> weight_bytes;
  weight_bytes.reserve(sizeof(ml_folded_weight));
  for (const double w : ml_folded_weight) {
    if (!std::isfinite(w)) return "model_identity";
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &w, sizeof bits);
    for (unsigned shift = 0U; shift < 64U; shift += 8U) weight_bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
  }
  if (sha256_hex(weight_bytes) != frozen_weights_digest) return "model_identity";
  return {};
}

std::vector<std::uint32_t> genesis_ml_select_windows(std::span<const double> matrix) {
  std::vector<std::uint32_t> out;
  if (matrix.size() % genesis_ml_feature_count != 0U) return out;
  const std::size_t windows = matrix.size() / genesis_ml_feature_count;
  for (std::size_t w = 0U; w < windows; ++w) {
    const double logit = genesis_ml_logit(matrix.subspan(w * genesis_ml_feature_count, genesis_ml_feature_count));
    if (logit >= ml_logit_threshold) out.push_back(static_cast<std::uint32_t>(w));  // NaN never selects
  }
  return out;
}

std::vector<FrontendProgram::ImmutableRomAotRange> genesis_ml_window_ranges(const std::vector<std::uint32_t> &windows, std::size_t rom_size) {
  std::vector<FrontendProgram::ImmutableRomAotRange> runs;
  for (const std::uint32_t w : windows) {
    const std::uint64_t begin = static_cast<std::uint64_t>(w) * genesis_ml_window_bytes;
    if (begin >= rom_size) continue;
    const std::uint64_t end = std::min<std::uint64_t>(begin + genesis_ml_window_bytes, rom_size);
    if (!runs.empty() && runs.back().end_address == begin) {
      runs.back().end_address = static_cast<std::uint32_t>(end);
    } else {
      FrontendProgram::ImmutableRomAotRange range{};
      range.begin_address = static_cast<std::uint32_t>(begin);
      range.end_address = static_cast<std::uint32_t>(end);
      runs.push_back(range);
    }
  }
  return runs;
}

GenesisMlRegionResult propose_genesis_ml_executable_regions(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                                                            const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries,
                                                            std::span<const std::uint32_t> seed_addresses) {
  GenesisMlRegionResult result;
  if (!genesis_ml_model_identity_failure().empty()) {
    result.failure = "model_identity";
    return result;
  }
  if (rom.empty() || (rom.size() & 1U) != 0U || rom.size() > genesis_ml_max_rom_bytes) {
    result.failure = "rom_size";
    return result;
  }
  const auto rows = genesis_window_feature_rows(entries, static_cast<std::uint32_t>(genesis_ml_window_bytes));
  if (!rows.has_value()) {
    result.failure = "model_identity";
    return result;
  }
  const std::vector<double> matrix = genesis_ml_window_matrix(rom, *rows);
  const std::size_t windows = matrix.size() / genesis_ml_feature_count;
  std::vector<std::uint32_t> ml;
  double margin = std::numeric_limits<double>::infinity();
  for (std::size_t w = 0U; w < windows; ++w) {
    const double logit = genesis_ml_logit(std::span<const double>(matrix).subspan(w * genesis_ml_feature_count, genesis_ml_feature_count));
    margin = std::min(margin, std::fabs(logit - ml_logit_threshold));
    if (logit >= ml_logit_threshold) ml.push_back(static_cast<std::uint32_t>(w));
  }
  std::vector<std::uint32_t> seeds;
  for (const std::uint32_t address : seed_addresses)
    if (address < rom.size()) seeds.push_back(static_cast<std::uint32_t>(address / genesis_ml_window_bytes));
  std::sort(seeds.begin(), seeds.end());
  seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
  std::vector<std::uint32_t> all;
  std::set_union(ml.begin(), ml.end(), seeds.begin(), seeds.end(), std::back_inserter(all));
  if (all.empty()) {
    result.failure = "empty_proposal";
    return result;
  }
  GenesisExecutableRegionProposal final_proposal{std::string(rom_sha256), genesis_ml_window_ranges(all, rom.size())};
  GenesisExecutableRegionProposal ml_proposal{std::string(rom_sha256), genesis_ml_window_ranges(ml, rom.size())};
  for (const auto &range : final_proposal.ranges) result.stats.region_bytes += range.end_address - range.begin_address;
  result.stats.windows = windows;
  result.stats.ml_selected = ml.size();
  result.stats.seed_windows = seeds.size();
  result.stats.final_selected = all.size();
  result.stats.min_logit_margin = margin;
  result.proposal = std::move(final_proposal);
  if (!ml.empty()) result.ml_only = std::move(ml_proposal);
  return result;
}

}  // namespace segarecomp
