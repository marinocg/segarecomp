// SEG-047-T003 (ADR 0096): the native folded logistic scorer and its frozen model identity, on synthetic inputs only.
// Properties: the embedded identity matches the frozen v1 constants; folded scoring equals canonical (unfolded) scoring to < 1e-12 on
// synthetic parameters; the decision flips exactly at the frozen logit threshold at ulp granularity (non-strict compare); the decision is
// independent of fused vs unfused accumulation away from the boundary; malformed widths fail closed; window runs merge and clip.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/ml_region_producer.hpp"

namespace {
using namespace segarecomp;
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
constexpr std::size_t N = genesis_ml_feature_count;

std::uint32_t lcg_state = 12345U;
double next_unit() {
  lcg_state = lcg_state * 1664525U + 1013904223U;
  return static_cast<double>(lcg_state >> 8U) / 16777216.0;
}
}  // namespace

int main() {
  expect(genesis_ml_model_identity_failure().empty(), "embedded model identity matches the frozen v1 constants");
  expect(genesis_ml_logit_threshold() == -4.776617512896361, "frozen logit threshold");
  const double p = 0.00835406801187952;
  expect(std::fabs(std::log(p / (1.0 - p)) - genesis_ml_logit_threshold()) <= 1e-15, "logit threshold = logit(probability threshold)");

  // Fold semantics on synthetic parameters (independent of the embedded model).
  std::vector<double> mean(N), scale(N), coef(N), x(N), folded(N);
  for (std::size_t i = 0; i < N; ++i) {
    mean[i] = next_unit() * 2.0 - 0.5;
    scale[i] = 0.05 + next_unit();
    coef[i] = next_unit() * 2.0 - 1.0;
    folded[i] = coef[i] / scale[i];
  }
  const double intercept = -3.25;
  long double bias_ld = intercept;
  for (std::size_t i = 0; i < N; ++i) bias_ld -= static_cast<long double>(coef[i]) * mean[i] / scale[i];
  const double bias = static_cast<double>(bias_ld);
  for (int trial = 0; trial < 200; ++trial) {
    for (auto &v : x) v = next_unit() * 3.0 - 1.0;
    long double unfolded = intercept;
    for (std::size_t i = 0; i < N; ++i) unfolded += static_cast<long double>(coef[i]) * ((x[i] - mean[i]) / scale[i]);
    const double score = genesis_ml_fold_score(folded.data(), bias, x.data(), N);
    expect(std::fabs(static_cast<double>(unfolded) - score) < 1e-12, "folded equals canonical unfolded score");
  }
  std::vector<double> zero(N, 0.0);
  expect(genesis_ml_fold_score(folded.data(), bias, zero.data(), N) == bias, "zero vector scores the folded bias exactly");
  std::vector<double> unit(N, 0.0);
  unit[7] = 1.0;
  expect(genesis_ml_fold_score(folded.data(), bias, unit.data(), N) == bias + folded[7], "unit vector adds exactly one weight");
  expect(genesis_ml_fold_score(folded.data(), bias, x.data(), 0) == bias, "empty vector scores the bias");

  // Embedded model: width checks and the exact boundary.
  expect(std::isnan(genesis_ml_logit(std::vector<double>(N - 1, 0.0))), "short vector => NaN (fail closed)");
  expect(std::isnan(genesis_ml_logit(std::vector<double>(N + 1, 0.0))), "long vector => NaN (fail closed)");
  expect(genesis_ml_select_windows(std::vector<double>(N + 3, 0.0)).empty(), "malformed matrix width selects nothing");
  expect(genesis_ml_select_windows({}).empty(), "empty matrix selects nothing");
  std::vector<double> row(N, 0.0);
  const double bias_logit = genesis_ml_logit(row);
  expect(bias_logit < genesis_ml_logit_threshold(), "an all-zero window is not selected (below threshold)");
  // Bisect feature k (positive weight) to adjacent doubles around the threshold.
  const std::size_t k = 1;  // b_zero; weight sign discovered below
  row[k] = 1.0;
  const bool increasing = genesis_ml_logit(row) > bias_logit;
  const double lo_start = increasing ? 0.0 : 1.0;
  double below = lo_start, above = increasing ? 1.0e6 : -1.0e6;
  row[k] = above;
  expect(genesis_ml_logit(row) >= genesis_ml_logit_threshold(), "far point is selected");
  for (int i = 0; i < 400; ++i) {
    const double mid = below + (above - below) / 2.0;
    if (mid == below || mid == above) break;
    row[k] = mid;
    if (genesis_ml_logit(row) >= genesis_ml_logit_threshold()) above = mid; else below = mid;
  }
  row[k] = below;
  expect(genesis_ml_select_windows(row).empty(), "adjacent double below the boundary is rejected");
  row[k] = above;
  expect(genesis_ml_select_windows(row) == std::vector<std::uint32_t>{0U}, "adjacent double at/above the boundary is selected");
  expect(genesis_ml_logit(row) >= genesis_ml_logit_threshold() && std::nextafter(above, below) != above, "boundary is ulp-adjacent");
  // Two windows: ordering and ascending indices.
  std::vector<double> two(2 * N, 0.0);
  std::copy(row.begin(), row.end(), two.begin() + static_cast<std::ptrdiff_t>(N));
  expect(genesis_ml_select_windows(two) == std::vector<std::uint32_t>{1U}, "selection indices are window ordinals");

  // Fused vs unfused accumulation agree on the decision for random rows away from the boundary.
  int disagreements = 0, near_boundary = 0;
  std::vector<double> weights(N);
  {
    // recover the embedded weights through unit-vector probes: w_i = logit(e_i) - logit(0)
    for (std::size_t i = 0; i < N; ++i) {
      std::vector<double> e(N, 0.0);
      e[i] = 1.0;
      weights[i] = genesis_ml_logit(e) - bias_logit;
    }
  }
  for (int trial = 0; trial < 20000; ++trial) {
    std::vector<double> r(N);
    for (auto &v : r) v = next_unit() * 4.0 - 1.0;
    double fused = bias_logit;
    for (std::size_t i = 0; i < N; ++i) fused = std::fma(weights[i], r[i], fused);
    const double plain = genesis_ml_logit(r);
    if (std::fabs(plain - genesis_ml_logit_threshold()) < 1e-6) { ++near_boundary; continue; }
    if ((fused >= genesis_ml_logit_threshold()) != (plain >= genesis_ml_logit_threshold())) ++disagreements;
  }
  expect(disagreements == 0, "decision independent of fused accumulation away from the boundary");
  expect(near_boundary < 20, "random rows are not accidentally boundary-bound");

  // Window runs.
  using Range = FrontendProgram::ImmutableRomAotRange;
  const auto same = [](const std::vector<Range> &a, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> b) {
    if (a.size() != b.size()) return false;
    std::size_t i = 0;
    for (const auto &[begin, end] : b) {
      if (a[i].begin_address != begin || a[i].end_address != end) return false;
      ++i;
    }
    return true;
  };
  expect(same(genesis_ml_window_ranges({0U, 1U, 2U, 5U, 6U}, 4096U), {{0U, 1536U}, {2560U, 3584U}}), "adjacent windows merge");
  expect(same(genesis_ml_window_ranges({1U, 3U}, 1536U), {{512U, 1024U}}), "window past the end is dropped, not clipped to empty");
  expect(same(genesis_ml_window_ranges({0U, 2U}, 1300U), {{0U, 512U}, {1024U, 1300U}}), "final short window clips to the image size");
  expect(genesis_ml_window_ranges({}, 4096U).empty(), "no windows => no ranges");
  // Proposal wrapper: fail-closed reasons and the certain-code union (no entries => decoder columns are zero).
  const std::string sha(64, 'a');
  const std::vector<FrontendAnalysis::ImmutableRomAotEntry> no_entries;
  const std::vector<std::uint8_t> zero_rom(4096, 0U);
  expect(propose_genesis_ml_executable_regions({}, sha, no_entries, {}).failure == "rom_size", "empty image is rejected");
  expect(propose_genesis_ml_executable_regions(std::span<const std::uint8_t>(zero_rom).first(4095), sha, no_entries, {}).failure == "rom_size", "odd-sized image is rejected");
  const auto none = propose_genesis_ml_executable_regions(zero_rom, sha, no_entries, {});
  expect(!none.proposal.has_value() && none.failure == "empty_proposal", "no ML window and no seed => empty_proposal (never an empty plan)");
  const std::vector<std::uint32_t> seeds{0x0U, 0x200U, 0xA02U, 0xFFFFFFFFU};  // windows 0, 1, 5; out-of-image address ignored
  const auto seeded = propose_genesis_ml_executable_regions(zero_rom, sha, no_entries, seeds);
  expect(seeded.proposal.has_value() && seeded.stats.seed_windows == 3U && seeded.stats.ml_selected == 0U && !seeded.ml_only.has_value(), "seed windows form the proposal");
  expect(seeded.proposal && seeded.proposal->ranges.size() == 2U && seeded.proposal->ranges[0].begin_address == 0U &&
             seeded.proposal->ranges[0].end_address == 1024U && seeded.proposal->ranges[1].begin_address == 2560U &&
             seeded.proposal->ranges[1].end_address == 3072U,
         "seed windows become half-open even ranges");
  expect(seeded.stats.region_bytes == 1536U && seeded.stats.windows == 8U && seeded.stats.min_logit_margin > 0.0, "stats");
  expect(seeded.proposal && seeded.proposal->rom_sha256 == sha, "proposal is bound to the ROM digest");
  if (failures != 0) return 1;
  std::cout << "genesis_ml_scorer_test: ok\n";
  return 0;
}
