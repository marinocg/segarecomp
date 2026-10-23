#include "segarecomp/machine/sega_cd/epoch_recipe.hpp"

#include <cstddef>

namespace segarecomp::machine::sega_cd {

namespace {

constexpr std::uint64_t kMaxTokens = 1U << 22;

struct Stop {
  EpochRecipeError error;
};

// Kosinski decoder (see tools/sega_cd_epoch_recipe.py for the format). Throws Stop; a copy may only reference bytes this stream produced.
class Kosinski {
 public:
  Kosinski(std::span<const std::uint8_t> src, std::size_t pos, std::size_t limit) : src_(src), pos_(pos), limit_(limit) {}

  std::vector<std::uint8_t> run() {
    refill();
    for (std::uint64_t n = 0; n < kMaxTokens; ++n) {
      if (bit()) {
        if (out_.size() >= limit_) throw Stop{EpochRecipeError::decoded_length_exceeded};
        out_.push_back(static_cast<std::uint8_t>(byte()));
      } else if (bit()) {
        const unsigned lo = byte();
        const unsigned hi = byte();
        const int displacement = static_cast<int>(0xE000U | ((hi & 0xF8U) << 5U) | lo) - 0x10000;
        const unsigned count = hi & 7U;
        if (count != 0U) {
          copy(displacement, count + 2U);
        } else {
          const unsigned extra = byte();
          if (extra == 0U) return std::move(out_);
          if (extra != 1U) copy(displacement, extra + 1U);
        }
      } else {
        unsigned count = bit() << 1U;
        count |= bit();
        copy(static_cast<int>(byte()) - 256, count + 2U);
      }
    }
    throw Stop{EpochRecipeError::token_bound_exceeded};
  }

 private:
  unsigned byte() {
    if (pos_ >= src_.size()) throw Stop{EpochRecipeError::source_exhausted};
    return src_[pos_++];
  }
  void refill() {
    const unsigned lo = byte();
    bits_ = lo | (byte() << 8U);
    left_ = 16U;
  }
  unsigned bit() {
    const unsigned b = bits_ & 1U;
    bits_ >>= 1U;
    if (--left_ == 0U) refill();
    return b;
  }
  void copy(int displacement, unsigned count) {
    const std::size_t back = static_cast<std::size_t>(-displacement);
    if (back > out_.size()) throw Stop{EpochRecipeError::reference_before_stream};
    if (out_.size() + count > limit_) throw Stop{EpochRecipeError::decoded_length_exceeded};
    for (unsigned i = 0; i < count; ++i) out_.push_back(out_[out_.size() - back]);
  }

  std::span<const std::uint8_t> src_;
  std::size_t pos_;
  std::size_t limit_;
  std::vector<std::uint8_t> out_;
  unsigned bits_{};
  unsigned left_{};
};

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
    std::vector<std::uint8_t> data;
    try {
      data = Kosinski(bios, step.source_offset, recipe.image_length - step.dest).run();
    } catch (const Stop &stop) {
      return fail(stop.error);
    }
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
