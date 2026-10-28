#pragma once

// SEG-096-T003 (ADR 0208): the experimental Sega CD AOT policy at the product CLI. The Sega CD machine library defines only the seam
// (selective_admission.hpp); the frozen gen2 M3 + B35 producer is the existing native evaluator of the Genesis machine library, used here on the
// immutable source bytes of one image at a time. Nothing is trained, retuned or added; Genesis policies, weights and identities are untouched.

#include <optional>
#include <string>
#include <string_view>

#include "segarecomp/machine/sega_cd/selective_admission.hpp"

namespace segarecomp_app {

// `--aot-policy compatibility|optimized [--aot-model gen2]` as every other platform spells it. For Sega CD `optimized` is EXPERIMENTAL (it may omit
// code and can stop at an uncompiled entry); Compatibility is the default and the fallback.
struct SegaCdAotOptions {
  std::string policy = "compatibility";
  bool model_given = false;
  [[nodiscard]] bool optimized() const { return policy == "optimized"; }
};

// false: the pair is not accepted (unknown policy, `--aot-model` with Compatibility, or a model other than gen2).
[[nodiscard]] bool set_sega_cd_aot_option(SegaCdAotOptions &options, std::string_view key, std::string_view value);
[[nodiscard]] bool sega_cd_aot_options_valid(const SegaCdAotOptions &options);

// The frozen gen2 M3 + B35 request, or nullopt for Compatibility. `policy_identity` is ROM-independent (producer components + analysis contract).
[[nodiscard]] std::optional<segarecomp::machine::sega_cd::SelectiveAdmissionRequest> make_sega_cd_aot_request(const SegaCdAotOptions &options);
[[nodiscard]] std::string sega_cd_aot_policy_identity(const SegaCdAotOptions &options);
// Empty when the embedded producer is compatible; otherwise the stable reason ("model_identity").
[[nodiscard]] std::string sega_cd_aot_producer_failure();

// The machine-readable `aot_policy` object of a report / status.json: requested, effective, fallback and its exact reason, producer identities,
// admitted-set digest and per-image counts (names are platform roles, never content).
[[nodiscard]] std::string sega_cd_aot_policy_json(const SegaCdAotOptions &options, const segarecomp::machine::sega_cd::SelectiveAdmissionOutcome &outcome);

}  // namespace segarecomp_app
