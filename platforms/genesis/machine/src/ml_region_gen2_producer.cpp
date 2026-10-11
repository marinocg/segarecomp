// SEG-049-T003 (ADR 0102): native frozen gen2 M3 evaluator and B35 rank-budget selection. See ml_region_gen2_producer.hpp for the contract.

#include "segarecomp/machine/genesis/ml_region_gen2_producer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#include "segarecomp/sha256.hpp"

// The numeric contract forbids fused multiply-add / contraction (also enforced by the build: -ffp-contract=off -fno-fast-math). Clang honours the
// pragma even when a consumer compiles this file outside the shipped build system.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace segarecomp {

namespace {
#include "ml_region_gen2_model_data.inc"

// ---- pinned identity constants (ADR 0101 section 8 freeze; also asserted against the generated table by the identity check) -------------
constexpr std::string_view pinned_model_sha256 = "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1";
constexpr std::string_view pinned_weight_table_sha256 = "72699dbcf8be5b8ceaffb36cb3cf4736eafe635e13ad564e752b81a8ee127d98";
constexpr std::string_view pinned_representation_sha256 = "cf43e758da215283c08b3f8c1271c6119fd83b44eee755974559a6cb5af40c67";
constexpr std::string_view pinned_token_schema_sha256 = "9a1f4f4fc03d26c63209128fc59fec1584b266bd1f61dbe8236f1105a1d61d75";
constexpr std::string_view pinned_rank_plan_sha256 = "8c4d98b9ee3874ff6a38005ce4745913c9ea01defd333d6bee9c1ea700c21e29";
constexpr std::string_view pinned_representation_version = "seg049-gen2-repr-v1";

// ---- architecture constants of the frozen M3 (tools/segarecomp_ml_region_gen2_train.py CNN_ARCH) -----------------------------------------
constexpr std::size_t kTaps = 5U;
constexpr std::size_t kIn = 36U;       // 8 + 8 byte embeddings + 20 token embedding dims
constexpr std::size_t kC1 = 48U, kC3 = 64U;
constexpr std::size_t kFinalLen = 226U, kCellLo = 97U, kCellLen = 32U;  // final positions per context, first cell position, cell positions
constexpr std::size_t kFeat = 4U * kC3;                                  // 256
constexpr std::size_t kHidden = 64U;
constexpr double kBnEps = 1e-5;
constexpr std::size_t kTokenFields = genesis_decode_token_fields;
constexpr std::uint8_t kTokenCard[kTokenFields] = {3, 6, 8, 8, 13, 13, 4, 4};
constexpr std::size_t kTokenDim[kTokenFields] = {2, 2, 3, 3, 3, 3, 2, 2};
constexpr const float *kTokenTable[kTokenFields] = {g2_tok_0_weight, g2_tok_1_weight, g2_tok_2_weight, g2_tok_3_weight,
                                                    g2_tok_4_weight, g2_tok_5_weight, g2_tok_6_weight, g2_tok_7_weight};

// The embedding tables, token dimensions and the stem input width must agree (a hand edit of one without the others fails to compile).
static_assert(sizeof(g2_tok_0_weight) / sizeof(float) == kTokenCard[0] * kTokenDim[0] && sizeof(g2_tok_1_weight) / sizeof(float) == kTokenCard[1] * kTokenDim[1] &&
                  sizeof(g2_tok_2_weight) / sizeof(float) == kTokenCard[2] * kTokenDim[2] && sizeof(g2_tok_3_weight) / sizeof(float) == kTokenCard[3] * kTokenDim[3] &&
                  sizeof(g2_tok_4_weight) / sizeof(float) == kTokenCard[4] * kTokenDim[4] && sizeof(g2_tok_5_weight) / sizeof(float) == kTokenCard[5] * kTokenDim[5] &&
                  sizeof(g2_tok_6_weight) / sizeof(float) == kTokenCard[6] * kTokenDim[6] && sizeof(g2_tok_7_weight) / sizeof(float) == kTokenCard[7] * kTokenDim[7],
              "token embedding tables disagree with the cardinality/dimension constants");
static_assert(16U + kTokenDim[0] + kTokenDim[1] + kTokenDim[2] + kTokenDim[3] + kTokenDim[4] + kTokenDim[5] + kTokenDim[6] + kTokenDim[7] == kIn,
              "stem input width disagrees with the embedding dimensions");
static_assert(sizeof(g2_emb_hi_weight) / sizeof(float) == 256U * 8U && sizeof(g2_stem_weight) / sizeof(float) == kC1 * kIn * kTaps, "table sizes");

struct TensorRef {
  const float *data;
  std::size_t count;
};
// Canonical order = tools/segarecomp_ml_region_gen2_native_data.py SHAPES (the order of the weight-table digest).
constexpr TensorRef kTensors[] = {
    {g2_emb_hi_weight, sizeof(g2_emb_hi_weight) / sizeof(float)}, {g2_emb_lo_weight, sizeof(g2_emb_lo_weight) / sizeof(float)},
    {g2_tok_0_weight, sizeof(g2_tok_0_weight) / sizeof(float)}, {g2_tok_1_weight, sizeof(g2_tok_1_weight) / sizeof(float)},
    {g2_tok_2_weight, sizeof(g2_tok_2_weight) / sizeof(float)}, {g2_tok_3_weight, sizeof(g2_tok_3_weight) / sizeof(float)},
    {g2_tok_4_weight, sizeof(g2_tok_4_weight) / sizeof(float)}, {g2_tok_5_weight, sizeof(g2_tok_5_weight) / sizeof(float)},
    {g2_tok_6_weight, sizeof(g2_tok_6_weight) / sizeof(float)}, {g2_tok_7_weight, sizeof(g2_tok_7_weight) / sizeof(float)},
    {g2_stem_weight, sizeof(g2_stem_weight) / sizeof(float)}, {g2_stem_bias, sizeof(g2_stem_bias) / sizeof(float)},
    {g2_bn1_weight, sizeof(g2_bn1_weight) / sizeof(float)}, {g2_bn1_bias, sizeof(g2_bn1_bias) / sizeof(float)},
    {g2_bn1_running_mean, sizeof(g2_bn1_running_mean) / sizeof(float)}, {g2_bn1_running_var, sizeof(g2_bn1_running_var) / sizeof(float)},
    {g2_b2_weight, sizeof(g2_b2_weight) / sizeof(float)}, {g2_b2_bias, sizeof(g2_b2_bias) / sizeof(float)},
    {g2_bn2_weight, sizeof(g2_bn2_weight) / sizeof(float)}, {g2_bn2_bias, sizeof(g2_bn2_bias) / sizeof(float)},
    {g2_bn2_running_mean, sizeof(g2_bn2_running_mean) / sizeof(float)}, {g2_bn2_running_var, sizeof(g2_bn2_running_var) / sizeof(float)},
    {g2_b3_weight, sizeof(g2_b3_weight) / sizeof(float)}, {g2_b3_bias, sizeof(g2_b3_bias) / sizeof(float)},
    {g2_bn3_weight, sizeof(g2_bn3_weight) / sizeof(float)}, {g2_bn3_bias, sizeof(g2_bn3_bias) / sizeof(float)},
    {g2_bn3_running_mean, sizeof(g2_bn3_running_mean) / sizeof(float)}, {g2_bn3_running_var, sizeof(g2_bn3_running_var) / sizeof(float)},
    {g2_b4_weight, sizeof(g2_b4_weight) / sizeof(float)}, {g2_b4_bias, sizeof(g2_b4_bias) / sizeof(float)},
    {g2_bn4_weight, sizeof(g2_bn4_weight) / sizeof(float)}, {g2_bn4_bias, sizeof(g2_bn4_bias) / sizeof(float)},
    {g2_bn4_running_mean, sizeof(g2_bn4_running_mean) / sizeof(float)}, {g2_bn4_running_var, sizeof(g2_bn4_running_var) / sizeof(float)},
    {g2_fc1_weight, sizeof(g2_fc1_weight) / sizeof(float)}, {g2_fc1_bias, sizeof(g2_fc1_bias) / sizeof(float)},
    {g2_fc2_weight, sizeof(g2_fc2_weight) / sizeof(float)}, {g2_fc2_bias, sizeof(g2_fc2_bias) / sizeof(float)},
};

std::string weight_table_digest() {
  std::vector<std::uint8_t> bytes;
  for (const TensorRef &tensor : kTensors) {
    for (std::size_t i = 0U; i < tensor.count; ++i) {
      const float value = tensor.data[i];
      if (!std::isfinite(value)) return {};
      std::uint32_t bits = 0U;
      static_assert(sizeof(bits) == sizeof(value), "binary32 expected");
      std::memcpy(&bits, &value, sizeof bits);
      for (unsigned shift = 0U; shift < 32U; shift += 8U) bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
  }
  return sha256_hex(bytes);
}

// One folded convolution: `w[(tap * cin + c) * cout + o]`, `b[o]` (BatchNorm folded in binary64, then rounded to binary32).
struct Conv {
  std::size_t cin{}, cout{}, dilation{};
  std::vector<float> w, b;
};

Conv fold_conv(const float *weight /* [cout][cin][5] */, const float *bias, const float *gamma, const float *beta, const float *mean,
               const float *var, std::size_t cin, std::size_t cout, std::size_t dilation) {
  Conv conv;
  conv.cin = cin;
  conv.cout = cout;
  conv.dilation = dilation;
  conv.w.assign(kTaps * cin * cout, 0.0F);
  conv.b.assign(cout, 0.0F);
  for (std::size_t o = 0U; o < cout; ++o) {
    const double scale = static_cast<double>(gamma[o]) / std::sqrt(static_cast<double>(var[o]) + kBnEps);
    for (std::size_t c = 0U; c < cin; ++c)
      for (std::size_t tap = 0U; tap < kTaps; ++tap)
        conv.w[(tap * cin + c) * cout + o] = static_cast<float>(static_cast<double>(weight[(o * cin + c) * kTaps + tap]) * scale);
    const double shifted = static_cast<double>(bias[o]) - static_cast<double>(mean[o]);
    const double folded = shifted * scale;
    conv.b[o] = static_cast<float>(folded + static_cast<double>(beta[o]));
  }
  return conv;
}

struct Net {
  Conv stem, b2, b3, b4;
};

const Net &net() {
  static const Net instance = {
      fold_conv(g2_stem_weight, g2_stem_bias, g2_bn1_weight, g2_bn1_bias, g2_bn1_running_mean, g2_bn1_running_var, kIn, kC1, 1U),
      fold_conv(g2_b2_weight, g2_b2_bias, g2_bn2_weight, g2_bn2_bias, g2_bn2_running_mean, g2_bn2_running_var, kC1, kC1, 2U),
      fold_conv(g2_b3_weight, g2_b3_bias, g2_bn3_weight, g2_bn3_bias, g2_bn3_running_mean, g2_bn3_running_var, kC1, kC3, 2U),
      fold_conv(g2_b4_weight, g2_b4_bias, g2_bn4_weight, g2_bn4_bias, g2_bn4_running_mean, g2_bn4_running_var, kC3, kC3, 4U),
  };
  return instance;
}

// Valid dilated convolution (kernel 5) + folded BatchNorm + ReLU over channel-last data. Accumulation per output channel:
// bias, then taps 0..4, then input channels 0..cin-1 (one binary32 multiply and one add each; a zero input contributes exactly nothing).
void conv_relu(const Conv &conv, const float *in, std::size_t in_len, float *out) {
  const std::size_t span = conv.dilation * (kTaps - 1U);
  const std::size_t out_len = in_len - span;
  constexpr std::size_t block = 16U;
  for (std::size_t t = 0U; t < out_len; ++t) {
    for (std::size_t ob = 0U; ob < conv.cout; ob += block) {
      float acc[block];
      for (std::size_t j = 0U; j < block; ++j) acc[j] = conv.b[ob + j];
      for (std::size_t tap = 0U; tap < kTaps; ++tap) {
        const float *row = in + (t + tap * conv.dilation) * conv.cin;
        const float *wt = conv.w.data() + tap * conv.cin * conv.cout + ob;
        for (std::size_t c = 0U; c < conv.cin; ++c) {
          const float x = row[c];
          if (x == 0.0F) continue;
          const float *wp = wt + c * conv.cout;
          for (std::size_t j = 0U; j < block; ++j) {
            const float product = x * wp[j];  // stored before the add: no fused multiply-add
            acc[j] += product;
          }
        }
      }
      float *dst = out + t * conv.cout + ob;
      for (std::size_t j = 0U; j < block; ++j) dst[j] = acc[j] > 0.0F ? acc[j] : 0.0F;
    }
  }
}

struct Scratch {
  std::vector<float> x, a, b, p, c, d;
};

bool tokens_in_range(const std::uint8_t *tok, std::size_t words) {
  for (std::size_t w = 0U; w < words; ++w)
    for (std::size_t f = 0U; f < kTokenFields; ++f)
      if (tok[w * kTokenFields + f] >= kTokenCard[f]) return false;
  return true;
}

// Scores the S cells of one padded span: `span = 64 * S + 448` words of (hi, lo, 8 token bytes). Writes S logits.
void score_span(const std::uint8_t *hi, const std::uint8_t *lo, const std::uint8_t *tok, std::size_t span, Scratch &s, float *out) {
  const Net &n = net();
  const std::size_t cells = (span - 2U * genesis_ml2_pad_words) / genesis_ml2_cell_words;
  s.x.resize(span * kIn);
  for (std::size_t w = 0U; w < span; ++w) {
    float *dst = s.x.data() + w * kIn;
    const float *e = g2_emb_hi_weight + static_cast<std::size_t>(hi[w]) * 8U;
    for (std::size_t i = 0U; i < 8U; ++i) *dst++ = e[i];
    e = g2_emb_lo_weight + static_cast<std::size_t>(lo[w]) * 8U;
    for (std::size_t i = 0U; i < 8U; ++i) *dst++ = e[i];
    for (std::size_t f = 0U; f < kTokenFields; ++f) {
      e = kTokenTable[f] + static_cast<std::size_t>(tok[w * kTokenFields + f]) * kTokenDim[f];
      for (std::size_t i = 0U; i < kTokenDim[f]; ++i) *dst++ = e[i];
    }
  }
  const std::size_t l1 = span - 4U, l2 = l1 - 8U, lp = l2 / 2U, l3 = lp - 8U, l4 = l3 - 16U;
  s.a.resize(l1 * kC1);
  s.b.resize(l2 * kC1);
  s.p.resize(lp * kC1);
  s.c.resize(l3 * kC3);
  s.d.resize(l4 * kC3);
  conv_relu(n.stem, s.x.data(), span, s.a.data());
  conv_relu(n.b2, s.a.data(), l1, s.b.data());
  for (std::size_t i = 0U; i < lp; ++i)
    for (std::size_t c = 0U; c < kC1; ++c) {
      const float first = s.b[(2U * i) * kC1 + c], second = s.b[(2U * i + 1U) * kC1 + c];
      s.p[i * kC1 + c] = second > first ? second : first;  // MaxPool1d(2): pairs aligned to the context start (even)
    }
  conv_relu(n.b3, s.p.data(), lp, s.c.data());
  conv_relu(n.b4, s.c.data(), l3, s.d.data());
  for (std::size_t j = 0U; j < cells; ++j) {
    float feat[kFeat];
    const float *base = s.d.data() + j * kCellLen * kC3;  // all-window start = cell start * 32 final positions
    const float *cell = base + kCellLo * kC3;
    for (std::size_t c = 0U; c < kC3; ++c) {
      float sum = 0.0F, peak = cell[c];
      for (std::size_t p = 0U; p < kCellLen; ++p) {
        const float v = cell[p * kC3 + c];
        sum += v;
        peak = v > peak ? v : peak;
      }
      feat[c] = sum / static_cast<float>(kCellLen);
      feat[kC3 + c] = peak;
      float wsum = 0.0F, wpeak = base[c];
      for (std::size_t p = 0U; p < kFinalLen; ++p) {
        const float v = base[p * kC3 + c];
        wsum += v;
        wpeak = v > wpeak ? v : wpeak;
      }
      feat[2U * kC3 + c] = wsum / static_cast<float>(kFinalLen);
      feat[3U * kC3 + c] = wpeak;
    }
    float logit = g2_fc2_bias[0];
    for (std::size_t h = 0U; h < kHidden; ++h) {
      float acc = g2_fc1_bias[h];
      const float *wr = g2_fc1_weight + h * kFeat;
      for (std::size_t i = 0U; i < kFeat; ++i) {
        const float product = wr[i] * feat[i];
        acc += product;
      }
      const float hidden = acc > 0.0F ? acc : 0.0F;
      const float product = g2_fc2_weight[h] * hidden;
      logit += product;
    }
    out[j] = logit;
  }
}

}  // namespace

const GenesisMl2Identity &genesis_ml2_identity() noexcept {
  static constexpr GenesisMl2Identity identity{g2_source_model_sha256, g2_representation_version, g2_representation_sha256, g2_token_schema_sha256,
                                               g2_rank_plan_sha256, g2_weight_table_sha256, g2_policy_id, g2_policy_percent,
                                               genesis_ml2_trainable_parameters};
  return identity;
}

std::string genesis_ml2_model_identity_failure() {
  const GenesisMl2Identity &id = genesis_ml2_identity();
  if (id.model_sha256 != pinned_model_sha256 || id.weight_table_sha256 != pinned_weight_table_sha256 ||
      id.representation_sha256 != pinned_representation_sha256 || id.token_schema_sha256 != pinned_token_schema_sha256 ||
      id.rank_plan_sha256 != pinned_rank_plan_sha256 || id.representation_version != pinned_representation_version ||
      id.policy_id != "B35" || id.policy_percent != 35U)
    return "model_identity";
  if (genesis_decode_tokens_schema != "segarecomp.m68k_decode_tokens.v1" || genesis_decode_token_fields != kTokenFields) return "model_identity";
  std::size_t parameters = 0U;
  for (const TensorRef &tensor : kTensors) parameters += tensor.count;
  // Trainable parameters exclude the BatchNorm running statistics (4 x (mean, var) vectors of 48/48/64/64 channels).
  if (parameters - 2U * (kC1 + kC1 + kC3 + kC3) != genesis_ml2_trainable_parameters) return "model_identity";
  if (weight_table_digest() != pinned_weight_table_sha256) return "model_identity";
  return {};
}

std::size_t genesis_ml2_reflect_index(std::int64_t i, std::size_t n) noexcept {
  if (n == 0U) return 0U;
  const std::int64_t period = 2 * static_cast<std::int64_t>(n);
  std::int64_t m = i % period;
  if (m < 0) m += period;
  return m < static_cast<std::int64_t>(n) ? static_cast<std::size_t>(m) : static_cast<std::size_t>(period - 1 - m);
}

std::size_t genesis_ml2_cell_count(std::size_t rom_bytes) noexcept { return (rom_bytes + genesis_ml2_cell_bytes - 1U) / genesis_ml2_cell_bytes; }

std::size_t genesis_ml2_budget_cells(std::size_t cells, unsigned percent) noexcept {
  if (cells == 0U || percent == 0U || percent > 100U) return 0U;
  return (cells * percent + 99U) / 100U;
}

float genesis_ml2_context_logit(std::span<const std::uint8_t> hi, std::span<const std::uint8_t> lo, std::span<const std::uint8_t> tok) {
  if (hi.size() != genesis_ml2_context_words || lo.size() != genesis_ml2_context_words || tok.size() != genesis_ml2_context_words * kTokenFields ||
      !tokens_in_range(tok.data(), genesis_ml2_context_words))
    return std::numeric_limits<float>::quiet_NaN();
  Scratch scratch;
  float out = 0.0F;
  score_span(hi.data(), lo.data(), tok.data(), genesis_ml2_context_words, scratch, &out);
  return out;
}

std::vector<float> genesis_ml2_cell_logits(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> tokens, unsigned chunk_cells) {
  if (rom.empty() || (rom.size() & 1U) != 0U || rom.size() > genesis_ml2_max_rom_bytes || chunk_cells == 0U) return {};
  const std::size_t words = rom.size() / 2U;
  if (tokens.size() != words * kTokenFields || !tokens_in_range(tokens.data(), words)) return {};
  const std::size_t cells = genesis_ml2_cell_count(rom.size());
  std::vector<float> logits(cells, 0.0F);
  std::vector<std::uint8_t> hi, lo, tok;
  Scratch scratch;
  for (std::size_t first = 0U; first < cells; first += chunk_cells) {
    const std::size_t count = std::min<std::size_t>(chunk_cells, cells - first);
    const std::size_t span = count * genesis_ml2_cell_words + 2U * genesis_ml2_pad_words;
    hi.resize(span);
    lo.resize(span);
    tok.resize(span * kTokenFields);
    for (std::size_t p = 0U; p < span; ++p) {
      const std::int64_t j = static_cast<std::int64_t>(first * genesis_ml2_cell_words + p) - static_cast<std::int64_t>(genesis_ml2_pad_words);
      const std::size_t w = genesis_ml2_reflect_index(j, words);
      hi[p] = rom[2U * w];
      lo[p] = rom[2U * w + 1U];
      std::copy_n(tokens.data() + w * kTokenFields, kTokenFields, tok.data() + p * kTokenFields);
    }
    score_span(hi.data(), lo.data(), tok.data(), span, scratch, logits.data() + first);
  }
  return logits;
}

GenesisMl2Selection genesis_ml2_rank_select(std::span<const float> logits, unsigned percent) {
  GenesisMl2Selection out;
  const std::size_t budget = genesis_ml2_budget_cells(logits.size(), percent);
  if (budget == 0U || logits.size() > std::numeric_limits<std::uint32_t>::max()) return out;
  for (const float v : logits)
    if (!std::isfinite(v)) return out;
  std::vector<std::uint32_t> order(logits.size());
  for (std::size_t i = 0U; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
  // Strict weak order: logit descending, ordinal ascending (+0 and -0 compare equal, so the ordinal decides).
  std::sort(order.begin(), order.end(), [&logits](std::uint32_t a, std::uint32_t b) {
    return logits[a] > logits[b] || (logits[a] == logits[b] && a < b);
  });
  out.cells.assign(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(budget));
  const float weakest = logits[order[budget - 1U]];
  for (const float v : logits) out.boundary_tie_cells += v == weakest ? 1U : 0U;
  out.boundary_gap = budget < order.size() ? weakest - logits[order[budget]] : 0.0F;
  std::sort(out.cells.begin(), out.cells.end());
  return out;
}

std::vector<FrontendProgram::ImmutableRomAotRange> genesis_ml2_cell_ranges(const std::vector<std::uint32_t> &cells, std::size_t rom_size) {
  std::vector<FrontendProgram::ImmutableRomAotRange> runs;
  for (const std::uint32_t cell : cells) {
    const std::uint64_t begin = static_cast<std::uint64_t>(cell) * genesis_ml2_cell_bytes;
    if (begin >= rom_size) continue;
    const std::uint64_t end = std::min<std::uint64_t>(begin + genesis_ml2_cell_bytes, rom_size);
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

GenesisMl2RegionResult propose_genesis_ml2_executable_regions(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                                                              std::span<const std::uint32_t> seed_addresses) {
  GenesisMl2RegionResult result;
  if (!genesis_ml2_model_identity_failure().empty()) {
    result.failure = "model_identity";
    return result;
  }
  if (rom.empty() || (rom.size() & 1U) != 0U || rom.size() > genesis_ml2_max_rom_bytes) {
    result.failure = "rom_size";
    return result;
  }
  const std::vector<std::uint8_t> tokens = genesis_decode_tokens(rom);
  const std::vector<float> logits = genesis_ml2_cell_logits(rom, tokens);
  if (logits.empty()) {
    result.failure = "token_range";
    return result;
  }
  GenesisMl2Selection selection = genesis_ml2_rank_select(logits, genesis_ml2_identity().policy_percent);
  if (selection.cells.empty()) {
    result.failure = "non_finite_score";
    return result;
  }
  std::vector<std::uint32_t> seeds;
  for (const std::uint32_t address : seed_addresses)
    if (address < rom.size()) seeds.push_back(static_cast<std::uint32_t>(address / genesis_ml2_cell_bytes));
  std::sort(seeds.begin(), seeds.end());
  seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
  std::vector<std::uint32_t> all;
  std::set_union(selection.cells.begin(), selection.cells.end(), seeds.begin(), seeds.end(), std::back_inserter(all));
  GenesisExecutableRegionProposal final_proposal{std::string(rom_sha256), genesis_ml2_cell_ranges(all, rom.size())};
  GenesisExecutableRegionProposal ml_proposal{std::string(rom_sha256), genesis_ml2_cell_ranges(selection.cells, rom.size())};
  for (const auto &range : final_proposal.ranges) result.stats.region_bytes += range.end_address - range.begin_address;
  result.stats.cells = logits.size();
  result.stats.ml_selected = selection.cells.size();
  result.stats.seed_cells = seeds.size();
  result.stats.final_selected = all.size();
  result.stats.boundary_tie_cells = selection.boundary_tie_cells;
  result.stats.boundary_gap = selection.boundary_gap;
  result.selected_cells = std::move(selection.cells);
  result.proposal = std::move(final_proposal);
  result.ml_only = std::move(ml_proposal);
  return result;
}

}  // namespace segarecomp
