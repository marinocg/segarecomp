#pragma once

// SEG-079 (ADR 0179): the static producer of a Sega CD sub runnable-epoch image. A recipe names ordered bounded decode steps over the
// immutable BIOS image; the image is zero-initialised and each step writes one decoded stream into it. The only decoder is the public
// Kosinski LZ decoder. Build time only: the emitter reconstructs the image (authority `static_proof`: static reasoning over an
// immutable input, no guest execution); the generated program still verifies the live PRG RAM against it at the epoch (ADR 0142
// decision 4), so a wrong recipe stops `materialized_image_unverified` before the sub retires an instruction.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kMaxEpochRecipeSteps = 8U;
inline constexpr std::uint32_t kMaxEpochRecipeImage = 0x80000U;  // PRG RAM
inline constexpr std::uint32_t kEpochRecipeGranule = 0x100U;

struct EpochRecipeStep {
  std::uint32_t source_offset{};  // BIOS image offset of a Kosinski stream
  std::uint32_t dest{};           // PRG RAM offset of its first output byte
};

struct EpochRecipe {
  std::uint32_t image_length{};
  std::vector<EpochRecipeStep> steps;
};

enum class EpochRecipeError : std::uint8_t {
  none,
  recipe_bounds,
  source_exhausted,
  reference_before_stream,
  decoded_length_exceeded,
  token_bound_exceeded,
  step_out_of_bounds,
  steps_overlap,
  image_not_whole_snapshot,
};
[[nodiscard]] const char *epoch_recipe_error_name(EpochRecipeError error) noexcept;

// Canonical text "<image_length>:<source>@<dest>[,<source>@<dest>]*" (decimal): the CLI form and the identity input.
[[nodiscard]] std::string epoch_recipe_text(const EpochRecipe &recipe);
[[nodiscard]] std::optional<EpochRecipe> parse_epoch_recipe(const std::string &text);

// Reconstruct the epoch image from the BIOS image alone. On failure returns nullopt and sets `error`.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> reconstruct_epoch_image(std::span<const std::uint8_t> bios, const EpochRecipe &recipe,
                                                                               EpochRecipeError *error);

}  // namespace segarecomp::machine::sega_cd
