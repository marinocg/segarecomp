// SEG-047-T002 (ADR 0096): exact seg046-features-v1 semantics of the NATIVE feature extraction, on project-authored synthetic buffers only.
// Golden values were produced by the committed pure-Python reference (tools/segarecomp_ml_region.py, Python 3.13 / system zlib 1.3.1) and
// are compared with the vendored zlib 1.3.1 and native code: lengths exactly, features exactly except the log2-based entropy (<= 1e-15).

#include <cmath>
#include <cstdint>
#include <iostream>
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

std::vector<std::uint8_t> lcg(std::size_t n, std::uint32_t seed) {
  std::vector<std::uint8_t> out;
  std::uint32_t x = seed;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1103515245U + 12345U;
    out.push_back(static_cast<std::uint8_t>((x >> 16U) & 255U));
  }
  return out;
}

std::vector<std::uint8_t> build(const std::string &name) {
  std::vector<std::uint8_t> b;
  if (name == "zeros512") b.assign(512, 0);
  else if (name == "ff512") b.assign(512, 255);
  else if (name == "ramp512" || name == "ramp101") for (std::size_t i = 0; i < (name == "ramp512" ? 512U : 101U); ++i) b.push_back(static_cast<std::uint8_t>(i & 255U));
  else if (name == "text512") for (std::size_t i = 0; i < 512; ++i) b.push_back(static_cast<std::uint8_t>(0x20U + (i * 7U) % 95U));
  else if (name == "lcg512") b = lcg(512, 1);
  else if (name == "nopmix512") {
    for (std::size_t i = 0; i < 300; ++i) b.push_back((i & 1U) != 0U ? 0x71 : 0x4E);
    const auto tail = lcg(212, 7);
    b.insert(b.end(), tail.begin(), tail.end());
  } else if (name == "lcg100") b = lcg(100, 3);
  else if (name == "runs512") for (std::size_t i = 0; i < 512; ++i) b.push_back(static_cast<std::uint8_t>((i / 37U) & 255U));
  return b;
}

struct GoldenByteCase { const char *name; std::size_t zlib_length; double features[16]; };
constexpr GoldenByteCase golden_byte_cases[] = {
    {"zeros512", 14, {-0.0, 1.0, 0.0, 0.0, 0.00390625, 1.0, 1.0, 0.02734375, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {"ff512", 14, {-0.0, 0.0, 1.0, 0.0, 0.00390625, 1.0, 1.0, 0.02734375, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0}},
    {"ramp512", 282, {1.0, 0.00390625, 0.00390625, 0.37109375, 1.0, 0.001953125, 0.0, 0.55078125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125}},
    {"text512", 109, {0.8205027288385176, 0.0, 0.0, 1.0, 0.37109375, 0.001953125, 0.0, 0.212890625, 0.0, 0.33984375, 0.33984375, 0.3203125, 0.0, 0.0, 0.0, 0.0}},
    {"lcg512", 523, {0.9521242470784145, 0.001953125, 0.001953125, 0.396484375, 0.87890625, 0.00390625, 0.0, 1.021484375, 0.138671875, 0.11328125, 0.1484375, 0.13671875, 0.130859375, 0.12890625, 0.10546875, 0.09765625}},
    {"nopmix512", 238, {0.5592380324146052, 0.0, 0.0, 0.734375, 0.58203125, 0.001953125, 0.5843137254901961, 0.46484375, 0.05078125, 0.044921875, 0.349609375, 0.341796875, 0.064453125, 0.029296875, 0.064453125, 0.0546875}},
    {"lcg100", 111, {0.7970384143441362, 0.0, 0.01, 0.47, 0.33984375, 0.01, 0.0, 1.11, 0.12, 0.13, 0.22, 0.12, 0.08, 0.11, 0.11, 0.11}},
    {"ramp101", 109, {0.8322764353439742, 0.009900990099009901, 0.0, 0.6831683168316832, 0.39453125, 0.009900990099009901, 0.0, 1.0792079207920793, 0.31683168316831684, 0.31683168316831684, 0.31683168316831684, 0.04950495049504951, 0.0, 0.0, 0.0, 0.0}},
    {"runs512", 41, {0.47575020642345756, 0.072265625, 0.0, 0.0, 0.0546875, 0.072265625, 0.9215686274509803, 0.080078125, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
};
constexpr std::uint64_t golden_decoder_rows[3][27] = {
    {10U, 2U, 5U, 2U, 1U, 6U, 2U, 1U, 0U, 1U, 0U, 2U, 1U, 1U, 1U, 1U, 0U, 2U, 0U, 12U, 5U, 3U, 2U, 2U, 0U, 4U, 6U},
    {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
    {3U, 3U, 0U, 0U, 0U, 3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
};
constexpr double golden_decoder_features[3][31] = {
    {0.0390625, 0.2, 0.5, 0.2, 0.1, 0.6, 0.2, 0.1, 0.0, 0.1, 0.0, 0.2, 0.1, 0.1, 0.1, 0.1, 0.0, 0.2, 0.0, 1.2, 0.5, 0.3, 0.2, 0.2, 0.0, 0.015625, 0.0234375, 0.4166666666666667, 0.25, 0.16666666666666666, 0.16666666666666666},
    {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.01171875, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.3333333333333333, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
};

}  // namespace

int main() {
  for (const auto &golden : golden_byte_cases) {
    const auto bytes = build(golden.name);
    expect(!bytes.empty(), std::string("case built: ") + golden.name);
    expect(genesis_ml_zlib_level6_length(bytes) == golden.zlib_length, std::string("zlib level-6 length: ") + golden.name);
    const auto features = genesis_ml_byte_features(bytes);
    for (std::size_t i = 0; i < features.size(); ++i) {
      const bool ok = i == 0U ? std::fabs(features[i] - golden.features[i]) <= 1e-15 : features[i] == golden.features[i];
      expect(ok, std::string("byte feature ") + std::to_string(i) + " of " + golden.name);
    }
    expect(genesis_ml_byte_features(bytes) == features, std::string("deterministic: ") + golden.name);
  }
  expect(genesis_ml_zlib_level6_length({}) > 0U, "empty input still yields a zlib stream header/trailer length");
  const auto empty = genesis_ml_byte_features({});
  for (const auto v : empty) expect(v == 0.0, "empty window yields zeros");

  for (std::size_t r = 0; r < 3U; ++r) {
    std::vector<std::uint64_t> row(std::begin(golden_decoder_rows[r]), std::end(golden_decoder_rows[r]));
    const auto features = genesis_ml_decoder_features(&row);
    for (std::size_t i = 0; i < features.size(); ++i) expect(features[i] == golden_decoder_features[r][i], "decoder feature " + std::to_string(i) + " row " + std::to_string(r));
  }
  for (const auto v : genesis_ml_decoder_features(nullptr)) expect(v == 0.0, "absent window row yields zeros");
  const std::vector<std::uint64_t> malformed(5, 1U);
  for (const auto v : genesis_ml_decoder_features(&malformed)) expect(v == 0.0, "wrong-width row fails closed to zeros");

  // Matrix: 3 windows (the last one short), neighbour context, missing neighbours are all-zero base vectors.
  std::vector<std::uint8_t> rom;
  for (const char *name : {"lcg512", "text512", "lcg100"}) {
    const auto part = build(name);
    rom.insert(rom.end(), part.begin(), part.end());
  }
  std::map<std::uint32_t, std::vector<std::uint64_t>> rows;
  rows[1] = std::vector<std::uint64_t>(std::begin(golden_decoder_rows[0]), std::end(golden_decoder_rows[0]));
  const auto matrix = genesis_ml_window_matrix(rom, rows);
  expect(matrix.size() == 3U * genesis_ml_feature_count, "3 windows x 141 features");
  const auto base = [&](std::size_t w) {
    std::vector<double> v;
    const auto bytes = genesis_ml_byte_features(std::span<const std::uint8_t>(rom).subspan(w * 512U, std::min<std::size_t>(512U, rom.size() - w * 512U)));
    v.assign(bytes.begin(), bytes.end());
    std::vector<std::uint64_t> r = w == 1U ? rows[1] : std::vector<std::uint64_t>();
    const auto dec = genesis_ml_decoder_features(w == 1U ? &r : nullptr);
    v.insert(v.end(), dec.begin(), dec.end());
    return v;
  };
  for (std::size_t w = 0; w < 3U; ++w) {
    const auto self = base(w);
    const auto prev = w > 0U ? base(w - 1U) : std::vector<double>(genesis_ml_base_feature_count, 0.0);
    const auto next = w + 1U < 3U ? base(w + 1U) : std::vector<double>(genesis_ml_base_feature_count, 0.0);
    std::vector<double> expected = self;
    expected.insert(expected.end(), prev.begin(), prev.end());
    expected.insert(expected.end(), next.begin(), next.end());
    const std::vector<double> got(matrix.begin() + static_cast<std::ptrdiff_t>(w * genesis_ml_feature_count),
                                  matrix.begin() + static_cast<std::ptrdiff_t>((w + 1U) * genesis_ml_feature_count));
    expect(got == expected, "window " + std::to_string(w) + " = base ++ prev ++ next");
  }
  expect(genesis_ml_window_matrix({}, {}).empty(), "empty image has no windows");
  if (failures != 0) return 1;
  std::cout << "genesis_ml_features_test: ok\n";
  return 0;
}
