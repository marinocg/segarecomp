#include "segarecomp/machine/master_system/ml_region_z80_producer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

#include "segarecomp/sha256.hpp"
#include "sms_mapper_contract.h"

namespace segarecomp::machine::master_system {
namespace {

#include "ml_region_z80_weights.inc"  // ml_z80_weights_blob[] (generated at build time from tools/segarecomp_ml_region_z80_gen2.weights.sgz80w)

constexpr const char *kArtifactSha256 = "81434de0da4755654e14c370fc32de7898532193cbc5a514033268ba6669c2ac";
constexpr const char *kEnsembleSha256 = "6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11";
constexpr std::size_t kBlobBytes = 527278;
constexpr std::uint32_t kTokenPad = 256;

// ---- frozen D512HP architecture -------------------------------------------------------------------------------------------------
constexpr std::size_t kEmb = 16, kL1 = 24, kCtx = 32, kFuse = 32;
constexpr std::array<std::size_t, 6> kDilations{1, 2, 4, 8, 16, 32};

struct TensorSpec {
  const char *name;
  std::array<std::uint32_t, 3> shape;
  int rank;
};
// Sorted by name exactly as the artifact stores them (strictly increasing bytes).
constexpr std::array<TensorSpec, 23> kSpecs{{
    {"ctx.0.bias", {32, 0, 0}, 1}, {"ctx.0.weight", {32, 32, 5}, 3}, {"ctx.1.bias", {32, 0, 0}, 1}, {"ctx.1.weight", {32, 32, 5}, 3},
    {"ctx.2.bias", {32, 0, 0}, 1}, {"ctx.2.weight", {32, 32, 5}, 3}, {"ctx.3.bias", {32, 0, 0}, 1}, {"ctx.3.weight", {32, 32, 5}, 3},
    {"ctx.4.bias", {32, 0, 0}, 1}, {"ctx.4.weight", {32, 32, 5}, 3}, {"ctx.5.bias", {32, 0, 0}, 1}, {"ctx.5.weight", {32, 32, 5}, 3},
    {"down.bias", {32, 0, 0}, 1},  {"down.weight", {32, 16, 4}, 3},  {"emb.weight", {257, 16, 0}, 2}, {"fuse.bias", {32, 0, 0}, 1},
    {"fuse.weight", {32, 56, 1}, 3}, {"head.bias", {1, 0, 0}, 1},    {"head.weight", {1, 32, 1}, 3},   {"l1.bias", {24, 0, 0}, 1},
    {"l1.weight", {24, 16, 5}, 3}, {"l2.bias", {24, 0, 0}, 1},       {"l2.weight", {24, 24, 5}, 3},
}};

// One convolution with the weights re-laid as [tap][in][out] so the output-channel loop is contiguous.
struct Conv {
  std::size_t cin = 0, cout = 0, taps = 0;
  std::vector<float> w, b;
};

struct Net {
  std::vector<float> emb;  // 257 x 16
  Conv l1, l2, down, fuse, head;
  std::array<Conv, 6> ctx;
};

struct Model {
  bool ok = false;
  std::array<Net, kMlNetworks> nets;
};

float read_f32(const std::uint8_t *p) {
  std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8U | static_cast<std::uint32_t>(p[2]) << 16U |
                       static_cast<std::uint32_t>(p[3]) << 24U;
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

Conv make_conv(const std::vector<float> &weight, const std::vector<float> &bias, std::size_t cout, std::size_t cin, std::size_t taps) {
  Conv c;
  c.cin = cin;
  c.cout = cout;
  c.taps = taps;
  c.b = bias;
  c.w.resize(taps * cin * cout);
  for (std::size_t o = 0; o < cout; ++o)
    for (std::size_t i = 0; i < cin; ++i)
      for (std::size_t t = 0; t < taps; ++t) c.w[(t * cin + i) * cout + o] = weight[(o * cin + i) * taps + t];
  return c;
}

// Strict SGZ80W01 parse + verification. Returns false (model not ok) on any deviation.
bool load_model(Model &model, std::span<const std::uint8_t> blob) {
  if (blob.size() != kBlobBytes || sha256_hex(blob) != kArtifactSha256) return false;
  std::size_t pos = 0;
  const auto take = [&](std::size_t n) -> const std::uint8_t * {
    if (n > blob.size() - pos) return nullptr;
    const std::uint8_t *p = blob.data() + pos;
    pos += n;
    return p;
  };
  const auto u32 = [&](std::uint32_t &out) {
    const std::uint8_t *p = take(4);
    if (!p) return false;
    out = static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8U | static_cast<std::uint32_t>(p[2]) << 16U |
          static_cast<std::uint32_t>(p[3]) << 24U;
    return true;
  };
  const std::uint8_t *magic = take(8);
  if (!magic || std::memcmp(magic, "SGZ80W01", 8) != 0) return false;
  std::uint32_t version = 0, count = 0;
  if (!u32(version) || !u32(count) || version != 1 || count != kMlNetworks) return false;
  std::vector<std::uint8_t> digest_input;  // the original ensemble identity: per model, per tensor: name bytes then float32 bytes
  digest_input.reserve(blob.size());
  for (std::size_t m = 0; m < kMlNetworks; ++m) {
    std::uint32_t seed = 0, tensors = 0;
    if (!u32(seed) || !u32(tensors) || seed != m + 1 || tensors != kSpecs.size()) return false;
    std::map<std::string, std::vector<float>> by_name;
    for (const TensorSpec &spec : kSpecs) {
      const std::uint8_t *len = take(2);
      if (!len) return false;
      const std::size_t name_len = len[0] | static_cast<std::size_t>(len[1]) << 8U;
      const std::uint8_t *name = take(name_len);
      const std::uint8_t *head = take(2);
      if (!name || !head || name_len != std::strlen(spec.name) || std::memcmp(name, spec.name, name_len) != 0) return false;
      if (head[0] != 0 || head[1] != spec.rank) return false;
      std::size_t elements = 1;
      for (int d = 0; d < spec.rank; ++d) {
        std::uint32_t dim = 0;
        if (!u32(dim) || dim != spec.shape[static_cast<std::size_t>(d)]) return false;
        elements *= dim;
      }
      const std::uint8_t *data = take(elements * 4);
      if (!data) return false;
      digest_input.insert(digest_input.end(), name, name + name_len);
      digest_input.insert(digest_input.end(), data, data + elements * 4);
      std::vector<float> values(elements);
      for (std::size_t i = 0; i < elements; ++i) {
        values[i] = read_f32(data + 4 * i);
        if (!std::isfinite(values[i])) return false;
      }
      by_name.emplace(spec.name, std::move(values));
    }
    Net &net = model.nets[m];
    net.emb = by_name.at("emb.weight");
    net.l1 = make_conv(by_name.at("l1.weight"), by_name.at("l1.bias"), kL1, kEmb, 5);
    net.l2 = make_conv(by_name.at("l2.weight"), by_name.at("l2.bias"), kL1, kL1, 5);
    net.down = make_conv(by_name.at("down.weight"), by_name.at("down.bias"), kCtx, kEmb, 4);
    for (std::size_t k = 0; k < 6; ++k)
      net.ctx[k] = make_conv(by_name.at("ctx." + std::to_string(k) + ".weight"), by_name.at("ctx." + std::to_string(k) + ".bias"), kCtx, kCtx, 5);
    net.fuse = make_conv(by_name.at("fuse.weight"), by_name.at("fuse.bias"), kFuse, kL1 + kCtx, 1);
    net.head = make_conv(by_name.at("head.weight"), by_name.at("head.bias"), 1, kFuse, 1);
  }
  if (pos != blob.size()) return false;
  if (sha256_hex(digest_input) != kEnsembleSha256) return false;
  model.ok = true;
  return true;
}

const Model &model() {
  static const Model instance = [] {
    Model m;
    if (!load_model(m, std::span<const std::uint8_t>(ml_z80_weights_blob, sizeof ml_z80_weights_blob))) m.ok = false;
    return m;
  }();
  return instance;
}

// y[pos] = act(bias + sum_t sum_ci x[pos*stride + t*dil - pad] * w[t][ci][.]) with zero padding outside [0, in_len). Accumulation order:
// bias, then taps ascending, then input channels ascending; each output channel is an independent accumulator (lane-wise, so any
// vectorization of the channel loop keeps the order and the result).
void conv(const Conv &c, const float *x, std::size_t in_len, std::size_t out_len, std::size_t stride, std::size_t dil, std::size_t pad, bool relu,
          float *y) {
  float acc[32];
  for (std::size_t pos = 0; pos < out_len; ++pos) {
    for (std::size_t o = 0; o < c.cout; ++o) acc[o] = c.b[o];
    for (std::size_t t = 0; t < c.taps; ++t) {
      const std::size_t shifted = pos * stride + t * dil;  // index + pad
      if (shifted < pad || shifted - pad >= in_len) continue;
      const float *xi = x + (shifted - pad) * c.cin;
      const float *wt = c.w.data() + t * c.cin * c.cout;
      for (std::size_t i = 0; i < c.cin; ++i) {
        const float xv = xi[i];
        const float *wi = wt + i * c.cout;
        for (std::size_t o = 0; o < c.cout; ++o) acc[o] += xv * wi[o];
      }
    }
    float *yo = y + pos * c.cout;
    for (std::size_t o = 0; o < c.cout; ++o) yo[o] = relu && acc[o] < 0.0f ? 0.0f : acc[o];
  }
}

struct Scratch {
  std::vector<float> e, a, loc, c, tmp;
};

// One window of `length` tokens (length % 4 == 0) -> logits for token positions [margin, length - margin).
void run_net(const Net &net, const std::uint16_t *tokens, std::size_t length, Scratch &s, float *out_central) {
  const std::size_t quarter = length / 4;
  s.e.resize(length * kEmb);
  for (std::size_t i = 0; i < length; ++i) std::memcpy(&s.e[i * kEmb], &net.emb[static_cast<std::size_t>(tokens[i]) * kEmb], kEmb * sizeof(float));
  s.a.resize(length * kL1);
  s.loc.resize(length * kL1);
  conv(net.l1, s.e.data(), length, length, 1, 1, 2, true, s.a.data());
  conv(net.l2, s.a.data(), length, length, 1, 1, 2, true, s.loc.data());
  s.c.resize(quarter * kCtx);
  s.tmp.resize(quarter * kCtx);
  conv(net.down, s.e.data(), length, quarter, 4, 1, 0, true, s.c.data());
  for (std::size_t k = 0; k < 6; ++k) {
    const std::size_t d = kDilations[k];
    conv(net.ctx[k], s.c.data(), quarter, quarter, 1, d, 2 * d, true, s.tmp.data());
    for (std::size_t i = 0; i < s.c.size(); ++i) s.c[i] = s.c[i] + s.tmp[i];  // residual
  }
  float x[kFuse];
  for (std::size_t pos = kMlMarginBytes; pos < length - kMlMarginBytes; ++pos) {
    const float *loc = &s.loc[pos * kL1];
    const float *ctx = &s.c[(pos / 4) * kCtx];  // repeat_interleave(4)
    for (std::size_t o = 0; o < kFuse; ++o) {
      float acc = net.fuse.b[o];
      for (std::size_t i = 0; i < kL1; ++i) acc += loc[i] * net.fuse.w[i * kFuse + o];
      for (std::size_t i = 0; i < kCtx; ++i) acc += ctx[i] * net.fuse.w[(kL1 + i) * kFuse + o];
      x[o] = acc < 0.0f ? 0.0f : acc;
    }
    float logit = net.head.b[0];
    for (std::size_t o = 0; o < kFuse; ++o) logit += x[o] * net.head.w[o];
    out_central[pos - kMlMarginBytes] = logit;
  }
}

}  // namespace

const MlIdentity &ml_identity() noexcept {
  static const MlIdentity identity{"sms_z80_d512hp",
                                   kArtifactSha256,
                                   kEnsembleSha256,
                                   "D512HP/3seeds",
                                   "cell32-top2mean-roots(0000,0038,0066)-budget65-ceil-tie:lower-index",
                                   "sgz80w01/tokens256+pad/logit-per-byte/context512",
                                   "segarecomp.z80_admission.v1"};
  return identity;
}

std::string sms_aot_policy_identity(std::string_view requested_policy) {
  const std::string text = "segarecomp.sms_aot_policy_identity.v1|requested=" + std::string(requested_policy) +
                           (requested_policy == "optimized" ? "|" + ml_policy_identity() + "|artifact=" + std::string(ml_identity().artifact_sha256) : std::string());
  return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

std::string sms_aot_build_identity(std::string_view rom_sha256, std::string_view requested_policy, std::string_view effective, std::string_view plan_sha256) {
  const std::string text = "segarecomp.sms_aot_build.v1|rom=" + std::string(rom_sha256) + "|requested=" + std::string(requested_policy) + "|effective=" +
                           std::string(effective) + "|policy=" + sms_aot_policy_identity(requested_policy) + "|plan=" + (plan_sha256.empty() ? std::string("-") : std::string(plan_sha256));
  return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

std::string ml_policy_identity() {
  const MlIdentity &i = ml_identity();
  return std::string(i.producer_id) + ":" + std::string(i.ensemble_sha256) + ":" + std::string(i.architecture) + ":" +
         std::string(i.rank_policy) + ":" + std::string(i.schema) + ":" + std::string(i.contract);
}

std::string ml_verify_blob(std::span<const std::uint8_t> blob) {
  Model scratch;
  return load_model(scratch, blob) ? std::string() : std::string("model_identity");
}

std::string ml_identity_failure() { return model().ok ? std::string() : std::string("model_identity"); }

std::string_view ml_fallback_class(std::string_view reason) noexcept {
  if (reason == "model_identity") return "producer_incompatible";
  if (reason == "producer_input") return "producer_input";
  if (reason == "inference") return "producer_failed";
  if (reason == "admission_rejected") return "proposal_rejected";
  return "none";
}

std::size_t ml_budget_cells(std::size_t cells, unsigned percent) noexcept {
  if (cells == 0 || percent == 0 || percent > 100) return 0;
  return (cells * percent + 99U) / 100U;
}

std::vector<float> ml_byte_logits(std::span<const std::uint8_t> rom, unsigned threads, std::size_t central) {
  const Model &m = model();
  if (!m.ok || rom.empty() || rom.size() > kMlMaxRomBytes || central == 0 || central % 4 != 0 || central > (std::size_t{1} << 20U)) return {};
  std::vector<float> out(rom.size());
  const std::size_t windows = (rom.size() + central - 1) / central;
  unsigned count = threads != 0 ? threads : std::max(1U, std::min(8U, std::thread::hardware_concurrency()));
  count = static_cast<unsigned>(std::min<std::size_t>(count, windows));
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    Scratch scratch;
    std::vector<std::uint16_t> tokens;
    std::vector<float> net_out, sum;
    for (;;) {
      const std::size_t w = next.fetch_add(1);
      if (w >= windows) return;
      const std::size_t start = w * central;
      const std::size_t scored = std::min(central, rom.size() - start);
      const std::size_t padded = (scored + 3U) / 4U * 4U;  // bytes beyond `scored` are PAD context, never read back
      const std::size_t length = padded + 2 * kMlMarginBytes;
      tokens.resize(length);
      for (std::size_t i = 0; i < length; ++i) {
        const std::int64_t index = static_cast<std::int64_t>(start) - static_cast<std::int64_t>(kMlMarginBytes) + static_cast<std::int64_t>(i);
        tokens[i] = index >= 0 && index < static_cast<std::int64_t>(rom.size()) ? rom[static_cast<std::size_t>(index)] : static_cast<std::uint16_t>(kTokenPad);
      }
      net_out.resize(padded);
      sum.assign(padded, 0.0f);
      for (const Net &net : m.nets) {
        run_net(net, tokens.data(), length, scratch, net_out.data());
        for (std::size_t i = 0; i < padded; ++i) sum[i] += net_out[i];  // seed order
      }
      for (std::size_t i = 0; i < scored; ++i) out[start + i] = sum[i] / 3.0f;
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < count; ++t) {
    try {
      pool.emplace_back(worker);
    } catch (...) {
      break;  // no more threads available: the calling thread (and those started) drain the window queue; the result does not depend on it
    }
  }
  worker();
  for (std::thread &t : pool) t.join();
  for (const float v : out)
    if (!std::isfinite(v)) return {};
  return out;
}

std::vector<float> ml_cell_scores(std::span<const float> byte_logits) {
  std::vector<float> scores;
  scores.reserve((byte_logits.size() + kMlCellBytes - 1) / kMlCellBytes);
  for (std::size_t base = 0; base < byte_logits.size(); base += kMlCellBytes) {
    const std::size_t end = std::min(base + kMlCellBytes, byte_logits.size());
    float first = byte_logits[base], second = -INFINITY;  // the two largest (duplicates allowed)
    bool have_second = false;
    for (std::size_t i = base + 1; i < end; ++i) {
      const float v = byte_logits[i];
      if (!std::isfinite(v)) return {};
      if (v > first) {
        second = first;
        have_second = true;
        first = v;
      } else if (!have_second || v > second) {
        second = v;
        have_second = true;
      }
    }
    if (!std::isfinite(first)) return {};
    scores.push_back(have_second ? (first + second) / 2.0f : first);
  }
  return scores;
}

std::vector<std::uint32_t> ml_rank_cells(std::span<const float> scores) {
  for (const float s : scores)
    if (!std::isfinite(s)) return {};
  std::vector<std::uint32_t> order(scores.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
  std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return scores[a] != scores[b] ? scores[a] > scores[b] : a < b; });
  return order;
}

std::vector<std::uint32_t> ml_select_cells(std::span<const float> scores, unsigned percent) {
  const std::vector<std::uint32_t> order = ml_rank_cells(scores);
  if (order.empty()) return {};
  const std::size_t budget = std::min(order.size(), std::max<std::size_t>(1, ml_budget_cells(order.size(), percent)));
  std::vector<std::uint32_t> chosen(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(budget));
  for (const std::size_t root : {std::size_t{0x0000}, std::size_t{0x0038}, std::size_t{0x0066}})
    if (root / kMlCellBytes < order.size()) chosen.push_back(static_cast<std::uint32_t>(root / kMlCellBytes));
  std::sort(chosen.begin(), chosen.end());
  chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
  return chosen;
}

MlProposal ml_propose(const IngestResult &cartridge, const codegen::z80::ImageSet &images, unsigned threads) {
  MlProposal result;
  if (!cartridge.ok() || cartridge.rom.empty() || cartridge.rom.size() > kMlMaxRomBytes) {
    result.failure = "producer_input";
    return result;
  }
  if (!ml_identity_failure().empty()) {
    result.failure = "model_identity";
    return result;
  }
  const auto start = std::chrono::steady_clock::now();
  const std::vector<float> logits = ml_byte_logits(cartridge.rom, threads);
  const std::vector<float> scores = logits.empty() ? std::vector<float>{} : ml_cell_scores(logits);
  const std::vector<std::uint32_t> selected = scores.empty() ? std::vector<std::uint32_t>{} : ml_select_cells(scores);
  result.inference_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (selected.empty()) {
    result.failure = "inference";
    return result;
  }
  result.cells = scores.size();
  result.selected_cells = selected.size();
  std::vector<char> keep(scores.size(), 0);
  for (const std::uint32_t cell : selected) keep[cell] = 1;
  const bool sega = cartridge.identity.mapper == SMS_MAPPER_SEGA;
  for (const codegen::z80::CodeImage &image : images.images) {
    // Platform-owned mapping of an image offset to the ROM byte it holds (machine contract 4.2).
    std::size_t base;
    if (!sega) base = 0;
    else if (image.identity == SMS_IMAGE_FIXED) base = 0;
    else if (image.identity >= SMS_IMAGE_BANK_FIRST) base = static_cast<std::size_t>(image.identity - SMS_IMAGE_BANK_FIRST) * SMS_BANK_SIZE;
    else {
      result.failure = "producer_input";
      return result;
    }
    std::vector<std::uint32_t> offsets;
    for (std::size_t o = 0; o < image.bytes.size(); ++o) {
      if (base + o >= cartridge.rom.size()) {
        result.failure = "producer_input";
        return result;
      }
      if (keep[(base + o) / kMlCellBytes]) offsets.push_back(static_cast<std::uint32_t>(o));
    }
    result.admitted_offsets += offsets.size();
    result.plan.images.push_back(codegen::z80::bind_admission(image, std::move(offsets)));
  }
  result.ok = true;
  return result;
}

std::string ml_plan_sha256(const codegen::z80::AdmissionPlan &plan) {
  std::vector<std::uint8_t> bytes;
  const auto put = [&bytes](std::uint64_t value, int width) {
    for (int i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  };
  put(plan.images.size(), 4);
  for (const codegen::z80::ImageAdmission &image : plan.images) {
    put(image.identity, 4);
    put(image.byte_count, 4);
    put(image.content_digest, 8);
    put(image.offsets.size(), 4);
    for (const std::uint32_t offset : image.offsets) put(offset, 4);
  }
  return sha256_hex(bytes);
}

}  // namespace segarecomp::machine::master_system
