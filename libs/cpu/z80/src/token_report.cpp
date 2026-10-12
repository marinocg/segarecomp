#include "segarecomp/cpu/z80/token_report.hpp"

#include <algorithm>

#include "segarecomp/cpu/z80/decode.hpp"

namespace segarecomp::cpu::z80 {

std::string decode_token_report(std::span<const std::uint8_t> image) {
  std::string out;
  out.reserve(12U + image.size() * 4U);
  out.append("Z80TOK01", 8U);
  const auto count = static_cast<std::uint32_t>(image.size());
  for (unsigned shift = 0; shift < 32U; shift += 8U) out.push_back(static_cast<char>((count >> shift) & 0xFFU));
  for (std::size_t block = 0; block < image.size(); block += kTokenReportBlockBytes) {
    TableFetch fetch;  // default: every address non_code
    const std::size_t window = std::min<std::size_t>(0x10000U, image.size() - block);
    for (std::size_t a = 0; a < window; ++a) {
      FetchedByte b;
      b.kind = FetchKind::byte;
      b.value = image[block + a];
      b.image_id = 0U;
      b.image_offset = static_cast<std::uint32_t>(block + a);
      fetch.set(static_cast<std::uint16_t>(a), b);
    }
    const std::size_t block_end = std::min<std::size_t>(block + kTokenReportBlockBytes, image.size());
    for (std::size_t p = block; p < block_end; ++p) {
      const auto start = decode_at(fetch, static_cast<std::uint16_t>(p - block));
      std::uint8_t kind = 3U;
      std::uint8_t length = 0U;
      std::uint16_t form = kNoForm;
      switch (start.kind) {
        case StartKind::decoded:
          kind = 0U;
          length = static_cast<std::uint8_t>(std::min<std::uint32_t>(255U, logical_length(start.instruction)));
          form = start.instruction.form;
          break;
        case StartKind::prefix_lock: kind = 1U; break;
        case StartKind::unresolved_fetch_mapping: kind = 2U; break;
        default: break;
      }
      out.push_back(static_cast<char>(kind));
      out.push_back(static_cast<char>(length));
      out.push_back(static_cast<char>(form & 0xFFU));
      out.push_back(static_cast<char>((form >> 8U) & 0xFFU));
    }
  }
  return out;
}

}  // namespace segarecomp::cpu::z80
