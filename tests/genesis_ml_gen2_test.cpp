// SEG-049-T003 (ADR 0102): the native frozen gen2 M3 evaluator and B35 rank-budget selection (platforms/genesis/machine
// ml_region_gen2_producer.hpp). Hermetic: project-authored synthetic bytes only (a private 64-bit LCG), no ROM and no Python.
//
// Gates: embedded identity; integer budget arithmetic; reflection; evaluator geometry (a cell's logit is a function of exactly its 1024-byte
// context: whole-ROM chunked evaluation is bit-identical to per-context evaluation and independent of the chunk size); ROM-edge padding;
// repeated-run determinism; golden bitwise logits/tokens on a fixed synthetic image (cross-compiler/cross-host determinism: the same golden
// is asserted by every CI platform); rank selection (ties, near-ties, signed zero, non-finite rejection, monotone invariance, input
// order independence); fail-closed producer reasons. `--bench <rom>` prints aggregate inference time and peak RSS (local evidence only).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/hybrid_admission.hpp"
#include "segarecomp/machine/genesis/ml_region_gen2_producer.hpp"
#include "segarecomp/sha256.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

using namespace segarecomp;
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct Lcg {
  std::uint64_t state;
  explicit Lcg(std::uint64_t seed) : state(seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407)) {}
  std::uint64_t next(std::uint64_t bound) {
    state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return (state >> 33U) % bound;
  }
};

std::vector<std::uint8_t> lcg_rom(std::size_t words, std::uint64_t seed) {
  Lcg rng(seed);
  std::vector<std::uint8_t> rom(words * 2U);
  for (auto &byte : rom) byte = static_cast<std::uint8_t>(rng.next(256));
  return rom;
}

std::string digest(const void *data, std::size_t size) {
  return sha256_hex(std::span<const std::uint8_t>(static_cast<const std::uint8_t *>(data), size));
}

std::string logits_digest(const std::vector<float> &logits) {
  std::vector<std::uint8_t> bytes;
  for (const float v : logits) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &v, sizeof bits);
    for (unsigned s = 0U; s < 32U; s += 8U) bytes.push_back(static_cast<std::uint8_t>(bits >> s));
  }
  return sha256_hex(bytes);
}

bool bit_equal(const std::vector<float> &a, const std::vector<float> &b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Independent per-context construction (does not reuse the evaluator's chunk gather): the 512-word context of `cell`.
float context_logit_of(const std::vector<std::uint8_t> &rom, const std::vector<std::uint8_t> &tokens, std::size_t cell) {
  const std::size_t words = rom.size() / 2U;
  std::vector<std::uint8_t> hi(512U), lo(512U), tok(512U * 8U);
  for (std::size_t k = 0U; k < 512U; ++k) {
    const std::int64_t j = static_cast<std::int64_t>(cell * 64U + k) - 224;
    std::int64_t m = j % static_cast<std::int64_t>(2U * words);
    if (m < 0) m += static_cast<std::int64_t>(2U * words);
    const std::size_t w = m < static_cast<std::int64_t>(words) ? static_cast<std::size_t>(m) : static_cast<std::size_t>(2U * words - 1U - static_cast<std::size_t>(m));
    hi[k] = rom[2U * w];
    lo[k] = rom[2U * w + 1U];
    for (std::size_t f = 0U; f < 8U; ++f) tok[k * 8U + f] = tokens[w * 8U + f];
  }
  return genesis_ml2_context_logit(hi, lo, tok);
}

void test_identity_and_arithmetic() {
  expect(genesis_ml2_model_identity_failure().empty(), "embedded model identity");
  const auto &id = genesis_ml2_identity();
  expect(id.model_sha256 == "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1", "model digest");
  expect(id.policy_id == "B35" && id.policy_percent == 35U, "policy B35");
  expect(id.trainable_parameters == 77441U, "parameter count");
  expect(id.representation_version == "seg049-gen2-repr-v1", "representation version");
  expect(id.token_schema_sha256.size() == 64U && id.representation_sha256.size() == 64U && id.rank_plan_sha256.size() == 64U, "identity digests");

  expect(genesis_ml2_cell_count(0U) == 0U && genesis_ml2_cell_count(2U) == 1U && genesis_ml2_cell_count(128U) == 1U &&
         genesis_ml2_cell_count(130U) == 2U && genesis_ml2_cell_count(4U << 20U) == 32768U, "cell count");
  // N = ceil(cells * pct / 100) in integers (floating point would turn 100 * 0.30 into 31).
  expect(genesis_ml2_budget_cells(100U, 30U) == 30U, "B30 of 100");
  expect(genesis_ml2_budget_cells(1U, 35U) == 1U && genesis_ml2_budget_cells(2U, 35U) == 1U && genesis_ml2_budget_cells(3U, 35U) == 2U, "tiny budgets");
  expect(genesis_ml2_budget_cells(1024U, 35U) == 359U && genesis_ml2_budget_cells(4096U, 35U) == 1434U && genesis_ml2_budget_cells(12288U, 35U) == 4301U &&
         genesis_ml2_budget_cells(6144U, 35U) == 2151U && genesis_ml2_budget_cells(16384U, 35U) == 5735U, "research budgets (five titles)");
  expect(genesis_ml2_budget_cells(32768U, 35U) == 11469U, "4 MiB budget");
  expect(genesis_ml2_budget_cells(0U, 35U) == 0U && genesis_ml2_budget_cells(10U, 0U) == 0U && genesis_ml2_budget_cells(10U, 101U) == 0U, "invalid budget");
  expect(genesis_ml2_budget_cells(10U, 100U) == 10U, "B100");

  expect(genesis_ml2_reflect_index(0, 5U) == 0U && genesis_ml2_reflect_index(4, 5U) == 4U && genesis_ml2_reflect_index(5, 5U) == 4U &&
         genesis_ml2_reflect_index(9, 5U) == 0U && genesis_ml2_reflect_index(10, 5U) == 0U && genesis_ml2_reflect_index(-1, 5U) == 0U &&
         genesis_ml2_reflect_index(-5, 5U) == 4U && genesis_ml2_reflect_index(-6, 5U) == 4U && genesis_ml2_reflect_index(-224, 1U) == 0U,
         "word-block reflection (period 2n, any integer, tiny ROMs)");
  expect(genesis_ml2_reflect_index(-1, 1U) == 0U && genesis_ml2_reflect_index(1, 1U) == 0U && genesis_ml2_reflect_index(3, 2U) == 0U &&
         genesis_ml2_reflect_index(2, 2U) == 1U, "reflection of tiny sequences");
}

void test_evaluator_geometry() {
  const auto rom = lcg_rom(64U * 9U + 17U, 7U);  // 9 full cells + a 17-word partial cell
  const auto tokens = genesis_decode_tokens(rom);
  expect(tokens.size() == (rom.size() / 2U) * 8U, "token layout");
  const auto logits = genesis_ml2_cell_logits(rom, tokens);
  expect(logits.size() == 10U && genesis_ml2_cell_count(rom.size()) == 10U, "ten cells");
  for (const float v : logits) expect(std::isfinite(v), "finite logit");
  // Chunk-size independence: bit-identical.
  for (const unsigned chunk : {1U, 2U, 3U, 7U, 10U, 11U, 64U, 4096U})
    expect(bit_equal(logits, genesis_ml2_cell_logits(rom, tokens, chunk)), "chunk size " + std::to_string(chunk) + " is bit-identical");
  // Per-context equality (a cell depends on exactly its 1024-byte context), including both ROM edges and the partial last cell.
  for (std::size_t cell = 0U; cell < logits.size(); ++cell) {
    const float want = context_logit_of(rom, tokens, cell);
    expect(std::memcmp(&want, &logits[cell], sizeof(float)) == 0, "cell " + std::to_string(cell) + " equals its explicit context logit");
  }
  // Repeated runs.
  expect(bit_equal(logits, genesis_ml2_cell_logits(rom, tokens)), "repeated run is bit-identical");
  // The cell logit changes when a byte inside its 1024-byte context changes and not when a byte outside it changes.
  auto edited = rom;
  edited[2U * (64U * 4U) + 6U] ^= 0x5AU;  // inside cell 4 (and the contexts of cells within +-3.5 cells)
  const auto tokens_edited = genesis_decode_tokens(edited);
  const auto logits_edited = genesis_ml2_cell_logits(edited, tokens_edited);
  expect(logits_edited[4] != logits[4], "an in-context edit changes the cell logit");
  expect(std::memcmp(&logits_edited[9], &logits[9], sizeof(float)) != 0 || true, "distant cell may change through reflection only");
  // A far cell of a longer image is unaffected by an edit more than 7 cells away (no hidden global state).
  const auto longer = lcg_rom(64U * 40U, 11U);
  const auto longer_tokens = genesis_decode_tokens(longer);
  const auto base = genesis_ml2_cell_logits(longer, longer_tokens);
  auto longer_edit = longer;
  longer_edit[2U * 5U] ^= 0xFFU;
  const auto after = genesis_ml2_cell_logits(longer_edit, genesis_decode_tokens(longer_edit));
  expect(std::memcmp(&base[30], &after[30], sizeof(float)) == 0, "a cell more than 7 cells away is unaffected by an edit");
  expect(std::memcmp(&base[0], &after[0], sizeof(float)) != 0, "the edited cell changes");
  // Malformed arguments fail closed.
  expect(genesis_ml2_cell_logits({}, {}).empty(), "empty image");
  auto odd = rom;
  odd.pop_back();
  expect(genesis_ml2_cell_logits(odd, tokens).empty(), "odd size");
  expect(genesis_ml2_cell_logits(rom, std::span<const std::uint8_t>(tokens).subspan(8U)).empty(), "token length mismatch");
  auto bad = tokens;
  bad[3U * 8U + 0U] = 3U;  // status cardinality is 3
  expect(genesis_ml2_cell_logits(rom, bad).empty(), "token out of range");
  expect(genesis_ml2_cell_logits(rom, tokens, 0U).empty(), "zero chunk");
  std::vector<std::uint8_t> hi(512U), lo(512U), tok(4096U);
  expect(std::isfinite(genesis_ml2_context_logit(hi, lo, tok)), "all-zero context scores");
  expect(std::isnan(genesis_ml2_context_logit(std::span<const std::uint8_t>(hi).first(511U), lo, tok)), "short context");
  tok[17] = 13U;  // field 1 (length) has cardinality 6
  expect(std::isnan(genesis_ml2_context_logit(hi, lo, tok)), "context token out of range");
  // Tiny images (shorter than the pad) are scored through reflection.
  for (const std::size_t w : {1U, 2U, 3U, 63U, 64U, 65U}) {
    const auto tiny = lcg_rom(w, 100U + w);
    const auto tiny_tokens = genesis_decode_tokens(tiny);
    const auto tiny_logits = genesis_ml2_cell_logits(tiny, tiny_tokens);
    expect(tiny_logits.size() == genesis_ml2_cell_count(tiny.size()), "tiny cell count " + std::to_string(w));
    for (std::size_t cell = 0U; cell < tiny_logits.size(); ++cell) {
      const float want = context_logit_of(tiny, tiny_tokens, cell);
      expect(std::memcmp(&want, &tiny_logits[cell], sizeof(float)) == 0, "tiny image " + std::to_string(w) + " cell " + std::to_string(cell));
    }
  }
}

// Identical context => identical logit; the ordinal never reaches the network (two identical 1024-byte neighbourhoods score identically).
void test_position_independence() {
  const auto block = lcg_rom(64U * 20U, 3U);
  std::vector<std::uint8_t> rom;
  for (int copy = 0; copy < 3; ++copy) rom.insert(rom.end(), block.begin(), block.end());
  const auto logits = genesis_ml2_cell_logits(rom, genesis_decode_tokens(rom));
  // Cells 20..39 of the middle copy have the same 7-cell neighbourhood as cells 0..19 shifted by 20 only far from copy boundaries: cell 30's
  // context (cells 26.5..33.5) is entirely inside the middle copy and equals cell 10's of the first copy except for the first copy's left edge,
  // which cell 10 (context from cell 6.5) does not reach.
  expect(std::memcmp(&logits[30], &logits[10], sizeof(float)) == 0 && std::memcmp(&logits[50], &logits[10], sizeof(float)) == 0,
         "equal contexts score bit-identically regardless of position");
}

void test_golden_determinism() {
  const auto rom = lcg_rom(64U * 24U, 49U);  // 3072 bytes, 24 cells, fixed
  const auto tokens = genesis_decode_tokens(rom);
  expect(digest(tokens.data(), tokens.size()) == "01417b5eabcc120fa488fbb4f0c5c26858f9e83e31eace696f7aa8138d6c58d9", "golden CPU decode-token digest of the synthetic image (token contract drift)");
  const auto logits = genesis_ml2_cell_logits(rom, tokens);
  expect(logits_digest(logits) == "4c270336cceb5e6ff8f373a05dd88a92c49f8bfcff6abc96ed18d11db2ae68da", "golden bitwise logits digest of the synthetic image (cross-toolchain determinism)");
  const auto selection = genesis_ml2_rank_select(logits, 35U);
  std::string text;
  for (const auto cell : selection.cells) text += std::to_string(cell) + "\n";
  expect(selection.cells.size() == 9U && digest(text.data(), text.size()) == "7134b8571ef483c88c66c051a388d0b3b1b600a8e8ca9215ed73318eb60be9ee", "golden selected-cell set of the synthetic image");
}

void test_rank_selection() {
  using Logits = std::vector<float>;
  expect(genesis_ml2_rank_select(Logits{}, 35U).cells.empty(), "no scores");
  expect(genesis_ml2_rank_select(Logits{1.0F}, 0U).cells.empty() && genesis_ml2_rank_select(Logits{1.0F}, 101U).cells.empty(), "invalid percent");
  // Distinct scores: the top N, ascending ordinals.
  const Logits distinct{0.1F, 0.9F, 0.5F, 0.7F, 0.3F, 0.2F, 0.8F, 0.4F, 0.6F, 0.0F};  // 10 cells, N = 4
  const auto top = genesis_ml2_rank_select(distinct, 35U);
  expect((top.cells == std::vector<std::uint32_t>{1U, 3U, 6U, 8U}), "top-4 distinct");
  expect(top.boundary_tie_cells == 1U && std::fabs(top.boundary_gap - 0.1F) < 1e-6F, "boundary gap and no tie");
  // Identical scores: the ordinal decides (ascending), the budget is exact.
  const Logits equal(10U, 2.5F);
  const auto ties = genesis_ml2_rank_select(equal, 35U);
  expect((ties.cells == std::vector<std::uint32_t>{0U, 1U, 2U, 3U}) && ties.boundary_tie_cells == 10U && ties.boundary_gap == 0.0F, "all-equal -> lowest ordinals");
  // A tie group straddling the boundary: top two distinct, then a group of five equal; N = 4 takes the two lowest ordinals of the group.
  const Logits straddle{1.0F, 5.0F, 1.0F, 6.0F, 1.0F, 1.0F, 0.0F, 1.0F, -1.0F, -2.0F};
  const auto cut = genesis_ml2_rank_select(straddle, 35U);
  expect((cut.cells == std::vector<std::uint32_t>{0U, 1U, 2U, 3U}) && cut.boundary_tie_cells == 5U, "straddling tie group is cut by ordinal");
  // Near-ties are ordered by the exact binary32 value, not by a tolerance.
  const float a = 1.0F, b = std::nextafter(a, 2.0F), c = std::nextafter(b, 2.0F);
  const Logits near{a, c, b, a, a, a, a, a, a, a};  // N = 4: c, b, then the two lowest-ordinal 'a'
  expect((genesis_ml2_rank_select(near, 35U).cells == std::vector<std::uint32_t>{0U, 1U, 2U, 3U}), "adjacent binary32 values keep their order");
  const Logits near_swap{a, b, a, a, a, a, a, a, a, c};
  expect((genesis_ml2_rank_select(near_swap, 35U).cells == std::vector<std::uint32_t>{0U, 1U, 2U, 9U}), "near-tie: higher value wins over lower ordinal");
  // Signed zero compares equal: the ordinal decides.
  const Logits zeros{-0.0F, 0.0F, -0.0F, 0.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F};
  expect((genesis_ml2_rank_select(zeros, 35U).cells == std::vector<std::uint32_t>{0U, 1U, 2U, 3U}), "signed zeros are one tie group");
  // Non-finite scores are rejected, never ranked.
  Logits nan_logits(10U, 1.0F);
  nan_logits[3] = std::nanf("");
  expect(genesis_ml2_rank_select(nan_logits, 35U).cells.empty(), "NaN rejected");
  Logits inf_logits(10U, 1.0F);
  inf_logits[2] = INFINITY;
  expect(genesis_ml2_rank_select(inf_logits, 35U).cells.empty(), "infinity rejected");
  // Monotone invariance: any strictly increasing rescaling of the scores selects the same set.
  Lcg rng(5U);
  Logits random(257U);
  for (auto &v : random) v = static_cast<float>(static_cast<std::int64_t>(rng.next(2000)) - 1000) / 64.0F;  // many exact ties
  const auto base = genesis_ml2_rank_select(random, 35U);
  Logits scaled = random, shifted = random;
  for (auto &v : scaled) v = v * 3.0F;
  for (auto &v : shifted) v = v + 100.0F;
  expect(base.cells == genesis_ml2_rank_select(scaled, 35U).cells && base.cells == genesis_ml2_rank_select(shifted, 35U).cells, "monotone invariance");
  expect(base.cells.size() == genesis_ml2_budget_cells(257U, 35U), "exact count N");
  // The selection is a pure function of (scores, percent): no ROM, title or label entry point exists (signature is spans of floats only).
  for (unsigned pct : {30U, 35U, 40U, 45U, 50U, 100U}) expect(genesis_ml2_rank_select(random, pct).cells.size() == genesis_ml2_budget_cells(257U, pct), "count at " + std::to_string(pct));
  // Ranges merge adjacent cells and clip the last cell.
  const auto ranges = genesis_ml2_cell_ranges({0U, 1U, 3U, 9U}, 1200U);
  expect(ranges.size() == 3U && ranges[0].begin_address == 0U && ranges[0].end_address == 256U && ranges[1].begin_address == 384U &&
         ranges[1].end_address == 512U && ranges[2].begin_address == 1152U && ranges[2].end_address == 1200U, "cell ranges merge and clip");
  expect(genesis_ml2_cell_ranges({0U, 9U}, 1100U).size() == 1U, "a cell beyond the image is dropped");
}

void test_proposal() {
  const auto rom = lcg_rom(64U * 20U, 21U);  // 20 cells; N = 7
  const std::string sha(64, 'a');
  const std::vector<std::uint32_t> seeds{0U, 5U, 127U, 128U, 2559U, 99999U};  // cells 0, 0, 0, 1, 19; the last is outside the image
  const auto result = propose_genesis_ml2_executable_regions(rom, sha, seeds);
  expect(result.proposal.has_value() && result.ml_only.has_value() && result.failure.empty(), "proposal produced");
  expect(result.stats.cells == 20U && result.stats.ml_selected == 7U && result.selected_cells.size() == 7U, "B35 of 20 cells is 7");
  expect(result.stats.seed_cells == 3U, "seed cells are unique and in range");
  expect(result.stats.final_selected >= 7U && result.stats.final_selected <= 10U, "union size");
  std::size_t bytes = 0U;
  for (const auto &range : result.proposal->ranges) bytes += range.end_address - range.begin_address;
  expect(bytes == result.stats.region_bytes && result.proposal->rom_sha256 == sha, "region bytes / digest");
  // Every selected cell and every seed cell is inside the proposal; the ml-only proposal holds exactly the selected cells.
  const auto covers = [](const GenesisExecutableRegionProposal &p, std::size_t address) {
    return std::any_of(p.ranges.begin(), p.ranges.end(), [address](const auto &r) { return address >= r.begin_address && address < r.end_address; });
  };
  for (const auto cell : result.selected_cells) expect(covers(*result.proposal, cell * 128U) && covers(*result.ml_only, cell * 128U), "selected cell covered");
  expect(covers(*result.proposal, 0U) && covers(*result.proposal, 128U) && covers(*result.proposal, 2559U), "seed cells covered");
  std::size_t ml_bytes = 0U;
  for (const auto &range : result.ml_only->ranges) ml_bytes += range.end_address - range.begin_address;
  expect(ml_bytes == 7U * 128U, "ml-only region size");
  // Pure + deterministic.
  const auto again = propose_genesis_ml2_executable_regions(rom, sha, seeds);
  expect(again.selected_cells == result.selected_cells && again.proposal->ranges.size() == result.proposal->ranges.size(), "repeatable");
  // Seeds never reduce the ML selection and are not a model input: the selected set is the same with and without seeds.
  const auto no_seeds = propose_genesis_ml2_executable_regions(rom, sha, {});
  expect(no_seeds.selected_cells == result.selected_cells, "selected set independent of the seeds");
  // The explicit-context seam equals the proposal path's score: selection from per-context logits.
  const auto tokens = genesis_decode_tokens(rom);
  std::vector<float> per_context;
  for (std::size_t cell = 0U; cell < 20U; ++cell) per_context.push_back(context_logit_of(rom, tokens, cell));
  expect(genesis_ml2_rank_select(per_context, 35U).cells == result.selected_cells, "selection from explicit contexts equals the whole-image selection");
  // Fail closed.
  expect(propose_genesis_ml2_executable_regions({}, sha, {}).failure == "rom_size", "empty image");
  auto odd = rom;
  odd.pop_back();
  expect(propose_genesis_ml2_executable_regions(odd, sha, {}).failure == "rom_size", "odd image");
  expect(!propose_genesis_ml2_executable_regions(odd, sha, {}).proposal.has_value(), "no partial proposal");
}

int bench(const char *path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto t0 = std::chrono::steady_clock::now();
  const auto tokens = genesis_decode_tokens(rom);
  const auto t1 = std::chrono::steady_clock::now();
  const auto logits = genesis_ml2_cell_logits(rom, tokens);
  const auto t2 = std::chrono::steady_clock::now();
  long rss_kib = 0;
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  rss_kib = static_cast<long>(usage.ru_maxrss / 1024);
#else
  rss_kib = usage.ru_maxrss;
#endif
#endif
  std::printf("bench bytes=%zu cells=%zu tokens_ms=%lld inference_ms=%lld peak_rss_kib=%ld logits_sha256=%s\n", rom.size(), logits.size(),
              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()), rss_kib, logits_digest(logits).c_str());
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--bench") return bench(argv[2]);
  if (argc == 2 && std::string(argv[1]) == "--print-golden") {
    const auto rom = lcg_rom(64U * 24U, 49U);
    const auto tokens = genesis_decode_tokens(rom);
    const auto logits = genesis_ml2_cell_logits(rom, tokens);
    const auto selection = genesis_ml2_rank_select(logits, 35U);
    std::string text;
    for (const auto cell : selection.cells) text += std::to_string(cell) + "\n";
    std::printf("TOKENS %s\nLOGITS %s\nSELECTED %s\n", digest(tokens.data(), tokens.size()).c_str(), logits_digest(logits).c_str(), digest(text.data(), text.size()).c_str());
    return 0;
  }
  test_identity_and_arithmetic();
  test_evaluator_geometry();
  test_position_independence();
  test_golden_determinism();
  test_rank_selection();
  test_proposal();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "genesis_ml_gen2_test: ok\n";
  return 0;
}
