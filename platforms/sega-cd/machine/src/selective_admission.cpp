#include "segarecomp/machine/sega_cd/selective_admission.hpp"

#include <algorithm>
#include <iterator>
#include <sstream>

#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

std::string validate_region_proposal(const RegionProposal &proposal, std::uint32_t image_bytes, std::span<const std::uint32_t> seed_offsets) {
  if (!proposal.ok) return proposal.failure.empty() ? "empty_proposal" : proposal.failure;
  if (proposal.ranges.empty()) return "empty_proposal";
  std::uint32_t previous_end = 0;
  bool first = true;
  for (const auto &[begin, end] : proposal.ranges) {
    if (begin >= end || ((begin | end) & 1U) != 0U) return "validator_rejected:range_malformed";
    if (end > image_bytes) return "validator_rejected:range_outside_image";
    if (!first && begin < previous_end) return "validator_rejected:range_order";
    previous_end = end;
    first = false;
  }
  for (const auto seed : seed_offsets)
    if (!ranges_contain(proposal.ranges, seed)) return "validator_rejected:seed_not_in_proposal";
  return {};
}

bool ranges_contain(const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges, std::uint32_t offset) noexcept {
  const auto it = std::upper_bound(ranges.begin(), ranges.end(), offset, [](std::uint32_t value, const auto &range) { return value < range.first; });
  return it != ranges.begin() && offset < std::prev(it)->second;
}

std::string admitted_set_digest(const std::vector<std::pair<std::string, std::vector<std::uint32_t>>> &rows) {
  std::ostringstream text;
  text << kSelectiveAdmissionContract << "\n";
  for (const auto &[name, offsets] : rows) {
    text << "image " << name << " " << offsets.size();
    for (const auto offset : offsets) text << " " << offset;
    text << "\n";
  }
  const std::string s = text.str();
  return sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(s.data()), s.size()));
}

}  // namespace segarecomp::machine::sega_cd
