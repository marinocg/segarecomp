#include "segarecomp/machine/sega_cd/epoch_recipe.hpp"

#include <cstddef>

#include "segarecomp/core/kosinski.hpp"

namespace segarecomp::machine::sega_cd {

namespace {

// The Kosinski decoder is the shared public implementation (libs/core kosinski.hpp; see tools/sega_cd_epoch_recipe.py for the format).
// A copy may only reference bytes this stream produced.
EpochRecipeError map_kosinski_error(KosinskiError error) noexcept {
  switch (error) {
    case KosinskiError::source_exhausted: return EpochRecipeError::source_exhausted;
    case KosinskiError::reference_before_stream: return EpochRecipeError::reference_before_stream;
    case KosinskiError::decoded_length_exceeded: return EpochRecipeError::decoded_length_exceeded;
    case KosinskiError::token_bound_exceeded: return EpochRecipeError::token_bound_exceeded;
    case KosinskiError::none: break;
  }
  return EpochRecipeError::none;
}

bool parse_number(const std::string &text, std::size_t &i, std::uint32_t &value) {
  if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
  std::uint64_t v = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    v = v * 10U + static_cast<unsigned>(text[i++] - '0');
    if (v > 0xFFFFFFFFULL) return false;
  }
  value = static_cast<std::uint32_t>(v);
  return true;
}

}  // namespace

const char *epoch_recipe_error_name(EpochRecipeError error) noexcept {
  switch (error) {
  case EpochRecipeError::none: return "none";
  case EpochRecipeError::recipe_bounds: return "recipe_bounds";
  case EpochRecipeError::source_exhausted: return "source_exhausted";
  case EpochRecipeError::reference_before_stream: return "reference_before_stream";
  case EpochRecipeError::decoded_length_exceeded: return "decoded_length_exceeded";
  case EpochRecipeError::token_bound_exceeded: return "token_bound_exceeded";
  case EpochRecipeError::step_out_of_bounds: return "step_out_of_bounds";
  case EpochRecipeError::steps_overlap: return "steps_overlap";
  case EpochRecipeError::image_not_whole_snapshot: return "image_not_whole_snapshot";
  }
  return "unknown";
}

std::string epoch_recipe_text(const EpochRecipe &recipe) {
  std::string out = std::to_string(recipe.image_length) + ":";
  for (std::size_t i = 0; i < recipe.steps.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(recipe.steps[i].source_offset) + "@" + std::to_string(recipe.steps[i].dest);
  }
  return out;
}

std::optional<EpochRecipe> parse_epoch_recipe(const std::string &text) {
  EpochRecipe recipe;
  std::size_t i = 0;
  if (!parse_number(text, i, recipe.image_length) || i >= text.size() || text[i++] != ':') return std::nullopt;
  for (;;) {
    EpochRecipeStep step;
    if (!parse_number(text, i, step.source_offset) || i >= text.size() || text[i++] != '@' || !parse_number(text, i, step.dest))
      return std::nullopt;
    recipe.steps.push_back(step);
    if (recipe.steps.size() > kMaxEpochRecipeSteps) return std::nullopt;
    if (i == text.size()) break;
    if (text[i++] != ',') return std::nullopt;
  }
  if (epoch_recipe_text(recipe) != text) return std::nullopt;  // canonical form only (no leading zeros, no alternative spellings)
  return recipe;
}

std::optional<std::vector<std::uint8_t>> reconstruct_epoch_image(std::span<const std::uint8_t> bios, const EpochRecipe &recipe,
                                                                 EpochRecipeError *error) {
  const auto fail = [&](EpochRecipeError e) -> std::optional<std::vector<std::uint8_t>> {
    if (error != nullptr) *error = e;
    return std::nullopt;
  };
  if (error != nullptr) *error = EpochRecipeError::none;
  if (recipe.image_length == 0U || recipe.image_length > kMaxEpochRecipeImage || recipe.image_length % kEpochRecipeGranule != 0U ||
      recipe.steps.empty() || recipe.steps.size() > kMaxEpochRecipeSteps)
    return fail(EpochRecipeError::recipe_bounds);
  std::vector<std::uint8_t> image(recipe.image_length, 0U);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> covered;
  for (const auto &step : recipe.steps) {
    if (step.source_offset >= bios.size() || step.dest >= recipe.image_length) return fail(EpochRecipeError::recipe_bounds);
    KosinskiResult decoded = kosinski_decode(bios, step.source_offset, recipe.image_length - step.dest);
    if (decoded.error != KosinskiError::none) return fail(map_kosinski_error(decoded.error));
    std::vector<std::uint8_t> data = std::move(decoded.output);
    const std::uint64_t hi = static_cast<std::uint64_t>(step.dest) + data.size();
    if (data.empty() || hi > recipe.image_length) return fail(EpochRecipeError::step_out_of_bounds);
    for (const auto &[a, b] : covered)
      if (step.dest < b && a < hi) return fail(EpochRecipeError::steps_overlap);
    covered.emplace_back(step.dest, static_cast<std::uint32_t>(hi));
    std::copy(data.begin(), data.end(), image.begin() + step.dest);
  }
  bool tail_nonzero = false;
  for (std::size_t i = image.size() - kEpochRecipeGranule; i < image.size(); ++i) tail_nonzero = tail_nonzero || image[i] != 0U;
  if (!tail_nonzero) return fail(EpochRecipeError::image_not_whole_snapshot);
  return image;
}

}  // namespace segarecomp::machine::sega_cd
