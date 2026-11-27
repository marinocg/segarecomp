#pragma once

// The evidence and the verdicts of `mod diff-run --audio` (SEG-145-T003, ADR 0323). Pure functions over the text this repository's own build and runtime wrote: no file access, no process.
// The gate FAILS CLOSED: success needs every piece of evidence present and well formed for Vanilla, Modded and the repeated Modded run. Anything missing or malformed is E_RUN_FAILED, never "no fault".
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace segarecomp_cli::sound_gate {

struct Refusal {
  std::string code, message;
};

// What a successful build recorded about the program's sound (status.json).
struct BuildSound {
  std::string status;          // "supported" | "native" | "degraded"
  std::string implementation;  // audio_implementation.kind: "original_z80_aot" | "native_driver"
  std::string outcome;         // the typed Z80 outcome of a degraded build ("" when there is none); informational
};
// What one compared run reported about its own sound (the payload of its SOUND_SUMMARY line).
struct RunSound {
  std::uint64_t sound_fault = 0, mixer_fault = 0;
  std::string native_fault;  // "none" or the replacement's fault token
  std::string pcm_sha256;    // 64 lower-case hex digits
};

// nullopt = the text is not one complete, well-formed document, a key is repeated, or a required member is missing from its containing object or malformed.
[[nodiscard]] std::optional<BuildSound> parse_build_sound(std::string_view status_json);
[[nodiscard]] std::optional<RunSound> parse_run_sound(std::string_view summary_json);

// The verdict on the two builds; nullopt = they can be compared.
[[nodiscard]] std::optional<Refusal> judge_builds(const std::optional<BuildSound> &vanilla, const std::optional<BuildSound> &modded);
// The verdict on what ONE compared run did; nullopt = its sound ran without a fault.
[[nodiscard]] std::optional<Refusal> judge_run(const std::optional<RunSound> &run, bool vanilla, std::string_view which);

}  // namespace segarecomp_cli::sound_gate
