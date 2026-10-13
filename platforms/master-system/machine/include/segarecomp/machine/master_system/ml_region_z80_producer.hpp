#pragma once

// SEG-057-T002 (ADR 0119): the NATIVE Master System Z80 instruction-start proposal producer for the frozen D512HP ensemble
// (SEG-056, ADR 0117), consumed through the SEG-057-T001 admission contract (codegen::z80::AdmissionPlan, ADR 0118).
//
//   ROM -> frozen D512HP x3 byte logits (mean) -> 32-byte cell score (mean of the two largest byte logits) -> structural root cells
//       UNION top ceil(65% * cells) cells -> image-offset admission sets -> AdmissionPlan -> existing Z80 emitter.
//
// This runs at build time only; the generated program contains no model, no interpreter and no runtime decoder. The ML result is a
// heuristic PROPOSAL: the admission contract validates identity and integrity, it does not prove that omitted starts are never
// executed. A start the plan omitted stops the generated program with the typed Z80_ERROR_UNADMITTED_PC.
//
// The evaluator is deliberately not a general inference framework: it evaluates exactly the one frozen architecture whose parameters are
// embedded in the binary (527,278-byte SGZ80W01 artifact, SHA-256 81434de0...c2ac; ensemble digest 6bee7772...4f11).
//
// Numeric contract: IEEE-754 binary32, a fixed accumulation order (bias, then tap-major, then input channel), no FMA contraction, no
// fast-math (the translation unit is built with -ffp-contract=off): logits are bit-identical on every IEEE-754 host and independent of
// the thread count (windows are independent). They are NOT bit-identical to PyTorch (different summation order): the PyTorch gate is a
// documented tolerance on the byte logits plus exact equality of the selected cell set. Ranking: score descending, lower cell index as
// the only tie-break; N = ceil(cells * 65 / 100) in integer arithmetic; non-finite scores fail closed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/codegen/c11/z80.hpp"
#include "segarecomp/machine/master_system/cartridge.hpp"

namespace segarecomp::machine::master_system {

inline constexpr std::size_t kMlCellBytes = 32;
inline constexpr unsigned kMlBudgetPercent = 65;
inline constexpr std::size_t kMlMarginBytes = 512;    // context either side; >= the receptive-field radius (504)
inline constexpr std::size_t kMlCentralBytes = 4096;  // scored bytes per window (any multiple of 4 gives identical logits)
inline constexpr std::size_t kMlMaxRomBytes = std::size_t{8} << 20U;  // resource bound; the SMS mapper tops out far below it
inline constexpr std::size_t kMlNetworks = 3;

struct MlIdentity {
  std::string_view producer_id;       // "sms_z80_d512hp"
  std::string_view artifact_sha256;   // SHA-256 of the embedded SGZ80W01 blob
  std::string_view ensemble_sha256;   // original SEG-056 ensemble digest (name + float32 bytes, seed order)
  std::string_view architecture;      // "D512HP"
  std::string_view rank_policy;       // cell size, reduction, budget, roots
  std::string_view schema;            // token / output schema
  std::string_view contract;          // admission contract version
};
[[nodiscard]] const MlIdentity& ml_identity() noexcept;
// The ROM-independent policy identity used in cache keys: changes with the weights, architecture, rank policy, schema or contract.
[[nodiscard]] std::string ml_policy_identity();
// The ROM-independent identity printed by `segarecomp aot-policy-identity --platform master-system` for a requested policy
// ("compatibility" | "optimized"); a launcher keys its cache on it. Compatibility and Optimized never collide, and every component of the
// model, rank policy, schema and admission contract is part of the Optimized one.
[[nodiscard]] std::string sms_aot_policy_identity(std::string_view requested_policy);
// The identity of one finished build: image, request, what actually ran and the admitted set. A fallback never aliases an Optimized build.
[[nodiscard]] std::string sms_aot_build_identity(std::string_view rom_sha256, std::string_view requested_policy, std::string_view effective,
                                                 std::string_view plan_sha256);

// Empty when the embedded artifact verifies (digests, strict format, tensor names/shapes, finite values); otherwise the stable reason
// "model_identity".
[[nodiscard]] std::string ml_identity_failure();

// The same verification applied to an arbitrary byte string (the test seam for mutated / truncated / non-finite artifacts).
[[nodiscard]] std::string ml_verify_blob(std::span<const std::uint8_t> blob);

// N = ceil(cells * percent / 100) in integer arithmetic (0 for cells == 0 or percent outside 1..100).
[[nodiscard]] std::size_t ml_budget_cells(std::size_t cells, unsigned percent) noexcept;

// One float32 logit per ROM byte (ensemble mean). Empty on an invalid argument, an unverifiable model or a non-finite logit. `threads` 0 =
// hardware concurrency (bounded); the result does not depend on it. `central` must be a positive multiple of 4 (tests vary it).
[[nodiscard]] std::vector<float> ml_byte_logits(std::span<const std::uint8_t> rom, unsigned threads = 0,
                                                std::size_t central = kMlCentralBytes);
// Cell scores: mean of the two largest byte logits of each 32-byte cell (a final short cell uses what it has). Empty on non-finite input.
[[nodiscard]] std::vector<float> ml_cell_scores(std::span<const float> byte_logits);
// Cell indices best-first (score descending, lower index first). Empty on a non-finite score.
[[nodiscard]] std::vector<std::uint32_t> ml_rank_cells(std::span<const float> scores);
// The retained ROM cell set: the structural root cells (cells holding ROM offsets 0x0000, 0x0038 and 0x0066) UNION the top
// ceil(percent * cells / 100) ranked cells. Sorted ascending. Empty on failure.
[[nodiscard]] std::vector<std::uint32_t> ml_select_cells(std::span<const float> scores, unsigned percent = kMlBudgetPercent);

struct MlProposal {
  bool ok = false;
  std::string failure;                       // stable sanitized reason when !ok: model_identity | producer_input | inference
  codegen::z80::AdmissionPlan plan;          // bound to `images`
  std::size_t cells = 0, selected_cells = 0;  // ROM cells, retained cells (roots included)
  std::size_t admitted_offsets = 0;           // image offsets named by the plan
  double inference_seconds = 0;
};
// Runs the whole producer for a validated cartridge and maps the retained ROM cells to image-offset admission sets over `images` (the
// emitter input of the SAME cartridge). Never throws; a failure is a classified fallback to the broad AOT.
[[nodiscard]] MlProposal ml_propose(const IngestResult& cartridge, const codegen::z80::ImageSet& images, unsigned threads = 0);

// SHA-256 over a canonical serialization of the plan (per image: identity, size, content digest, offset count, offsets), the identity of the
// selected set for build/cache reports. No address is exposed by the digest.
[[nodiscard]] std::string ml_plan_sha256(const codegen::z80::AdmissionPlan& plan);

}  // namespace segarecomp::machine::master_system
