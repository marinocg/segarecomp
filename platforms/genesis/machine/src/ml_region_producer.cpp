// SEG-047 (ADR 0096): native seg046-features-v1 extraction (and, from T003, the folded scorer). See ml_region_producer.hpp.

#include "segarecomp/machine/genesis/ml_region_producer.hpp"

#include <algorithm>
#include <cmath>

#include "zlib.h"

namespace segarecomp {

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

}  // namespace segarecomp
