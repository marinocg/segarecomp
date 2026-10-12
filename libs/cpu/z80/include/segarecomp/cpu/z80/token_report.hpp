#pragma once

// SEG-051: report-only Z80-owned decode tokens of every byte position of an image (offline executable-region ML
// experiment input). The Z80 decoder (`decode_at`) is the sole authority for forms, prefixes and lengths; nothing here
// is a second decoder and nothing here is runtime or product-admission behavior.
//
// Wire format (little endian): 8-byte magic "Z80TOK01", u32 position count N, then N records of 4 bytes:
//   byte 0  kind   0 decoded, 1 prefix_lock, 2 unresolved_fetch_mapping, 3 mutable_code/other
//   byte 1  length total logical length (superseded prefixes included), saturated at 255 (a longer ignored-prefix run
//           reports 255; consumers clip), 0 unless decoded
//   byte 2..3 form FormId (u16), 0xFFFF unless decoded
// Position p decodes with a logical window of up to 65,536 bytes of the image starting at the 16 KiB-aligned block
// containing p (address = p - block start), so an instruction continues linearly into following image bytes. A byte past
// the image end is `non_code` (kind 3 when an instruction needs it). This linear view is a feature surface, not
// architectural bank mapping.

#include <cstdint>
#include <span>
#include <string>

namespace segarecomp::cpu::z80 {

inline constexpr std::uint32_t kTokenReportBlockBytes = 0x4000U;
std::string decode_token_report(std::span<const std::uint8_t> image);

}  // namespace segarecomp::cpu::z80
