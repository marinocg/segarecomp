#pragma once

// SEG-049-T003 (ADR 0102): the NATIVE Genesis M68K executable-region proposal producer for the frozen second-generation (gen2) model.
//
//   ROM -> CPU-owned decode tokens -> frozen native M3 byte+token CNN -> B35 rank-budget selection -> (existing structural prune and
//   admission validator, owned elsewhere) -> generated C.
//
// ML is a heuristic *proposal* producer for an executable region `R` only. Pruning, the admission validator and the generated-native
// dispatcher remain the correctness authority; nothing here proves that unselected code is never executed. No ML runs in generated
// programs; there is no interpreter, JIT, runtime decoder, Python, PyTorch or ONNX. The evaluator is deliberately NOT a general inference
// framework: it evaluates exactly the one frozen architecture (ADR 0100 "M3") whose parameters are embedded in the binary.
//
// Representation (ADR 0100 section 4, frozen, `seg049-gen2-repr-v1`): decision cells of 128 bytes; each cell is scored from a 1024-byte
// (512-word) context with the cell in its middle (448 bytes either side); ROM edges use symmetric word-block reflection (period 2n, bytes
// inside a word keep their order). Inputs are the hi/lo byte of every context word and its CPU-owned 8-field decode token. No address,
// ordinal, ROM size, title or label is an input.
//
// Numeric contract (ADR 0102 section 3): IEEE-754 binary32 for the network, fixed accumulation order (tap-major, then input channel, then
// the fixed head order), no fused multiply-add, no fast-math (the translation unit is built with -ffp-contract=off -fno-fast-math). The
// inference-time BatchNorm is folded into the convolutions at first use in binary64 (+ - * / sqrt only: correctly rounded, hence
// portable). Under that contract the logits are bit-identical on every IEEE-754 host. They are NOT bit-identical to the PyTorch/stdlib
// research evaluators (different summation order); the gate against those is a documented tolerance on the logits PLUS exact equality of
// the selected cell set. Ranking uses the binary32 logit itself (not a sigmoid): score descending, cell ordinal ascending as the only
// tie-break, N = ceil(cells * percent / 100) in integer arithmetic. The ordinal is never a model input.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace segarecomp {

inline constexpr std::size_t genesis_ml2_cell_bytes = 128U;
inline constexpr std::size_t genesis_ml2_cell_words = genesis_ml2_cell_bytes / 2U;     // 64
inline constexpr std::size_t genesis_ml2_context_words = 512U;                           // 1024 bytes
inline constexpr std::size_t genesis_ml2_pad_words = (genesis_ml2_context_words - genesis_ml2_cell_words) / 2U;  // 224
inline constexpr std::size_t genesis_ml2_trainable_parameters = 77441U;
inline constexpr unsigned genesis_ml2_default_chunk_cells = 64U;
// Resource bound on the image the producer accepts (same defence in depth as the v1 producer; a Genesis cartridge is at most 4 MiB).
inline constexpr std::size_t genesis_ml2_max_rom_bytes = std::size_t{32} << 20U;

// Frozen identities embedded in the binary (all derived from the committed research freeze and cross-checked by the generator).
struct GenesisMl2Identity {
  std::string_view model_sha256;           // SHA-256 of tools/segarecomp_ml_region_gen2.model.json (LF form)
  std::string_view representation_version; // "seg049-gen2-repr-v1"
  std::string_view representation_sha256;
  std::string_view token_schema_sha256;    // CPU decode-token contract (segarecomp.m68k_decode_tokens.v1)
  std::string_view rank_plan_sha256;       // pre-registered rank-budget plan
  std::string_view weight_table_sha256;    // SHA-256 of the embedded binary32 tensors in canonical order
  std::string_view policy_id;              // "B35"
  unsigned policy_percent;                 // 35
  std::size_t trainable_parameters;
};
[[nodiscard]] const GenesisMl2Identity &genesis_ml2_identity() noexcept;
// Fail-closed identity check of the embedded tables against constants pinned in the source (model, representation, token schema, rank plan,
// policy, and a recomputed SHA-256 of the weight table). Empty => ok; otherwise the stable sanitized reason "model_identity".
[[nodiscard]] std::string genesis_ml2_model_identity_failure();

// Symmetric reflection with period 2n for any integer `i` (n > 0).
[[nodiscard]] std::size_t genesis_ml2_reflect_index(std::int64_t i, std::size_t n) noexcept;
// ceil(rom_bytes / 128). 0 for an empty image.
[[nodiscard]] std::size_t genesis_ml2_cell_count(std::size_t rom_bytes) noexcept;
// N = ceil(cells * percent / 100) in exact integer arithmetic; 0 for cells == 0 or an invalid percent (0 or > 100).
[[nodiscard]] std::size_t genesis_ml2_budget_cells(std::size_t cells, unsigned percent) noexcept;

// Logit of the cell at words [224, 288) of ONE explicit 512-word context: `hi`/`lo` hold 512 bytes each, `tok` 512 * 8 token bytes. NaN on a
// malformed argument (wrong length, token value outside its field cardinality). This is the parity seam against the research evaluators.
[[nodiscard]] float genesis_ml2_context_logit(std::span<const std::uint8_t> hi, std::span<const std::uint8_t> lo, std::span<const std::uint8_t> tok);

// Logits of every 128-byte cell of the image (cells = ceil(size/128); a final short cell is scored with reflected padding). `tokens` is
// `genesis_decode_tokens(rom)`. The whole image is evaluated through a shared convolutional trunk in chunks of `chunk_cells` cells with
// bounded memory; the result is independent of `chunk_cells` (bit-identical, tested). Empty on a malformed input (size, token range).
[[nodiscard]] std::vector<float> genesis_ml2_cell_logits(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> tokens,
                                                         unsigned chunk_cells = genesis_ml2_default_chunk_cells);

struct GenesisMl2Selection {
  std::vector<std::uint32_t> cells;  // selected cell ordinals, ascending
  std::size_t boundary_tie_cells{};  // cells whose logit equals the weakest selected logit (the tie group straddling the budget cut)
  float boundary_gap{};              // weakest selected logit minus the strongest unselected logit (0 when all or none are selected)
};
// The B-percent rank selection of `logits`: score descending, ordinal ascending. Empty `cells` when a logit is not finite or the argument is
// invalid (never a partial result).
[[nodiscard]] GenesisMl2Selection genesis_ml2_rank_select(std::span<const float> logits, unsigned percent);

// Merges ascending cell ordinals (128 B each, the final range clipped to `rom_size`) into half-open ascending disjoint ranges.
[[nodiscard]] std::vector<FrontendProgram::ImmutableRomAotRange> genesis_ml2_cell_ranges(const std::vector<std::uint32_t> &cells, std::size_t rom_size);

struct GenesisMl2RegionStats {
  std::size_t cells{};            // ceil(size / 128)
  std::size_t ml_selected{};      // N = ceil(cells * 35 / 100)
  std::size_t seed_cells{};       // cells holding a precise direct-control-discovery identity
  std::size_t final_selected{};   // |ML U seeds|
  std::size_t region_bytes{};     // bytes of the final proposal
  std::size_t boundary_tie_cells{};
  float boundary_gap{};
};

struct GenesisMl2RegionResult {
  std::optional<GenesisExecutableRegionProposal> proposal;  // R = rank-selected cells U certain-code seed cells (nullopt on failure)
  std::optional<GenesisExecutableRegionProposal> ml_only;   // rank-selected cells only (diagnostic / parity)
  std::vector<std::uint32_t> selected_cells;                // the rank-selected cell set (ascending), exposed for exact-parity evidence
  GenesisMl2RegionStats stats;
  std::string failure;  // stable sanitized reason when `proposal` is empty: model_identity | rom_size | token_range | non_finite_score | empty_proposal
};

// The frozen gen2 proposal. Pure and deterministic; never mutates its inputs; fail-closed with a stable reason. It does not validate or prune.
[[nodiscard]] GenesisMl2RegionResult propose_genesis_ml2_executable_regions(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                                                                            std::span<const std::uint32_t> seed_addresses);

}  // namespace segarecomp
