#include "segarecomp/patch.hpp"

#include <algorithm>
#include <array>

namespace segarecomp::patch {

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFU;
  for (const std::uint8_t b : bytes) c = table[(c ^ b) & 0xFFU] ^ (c >> 8U);
  return c ^ 0xFFFFFFFFU;
}

namespace {

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8U | static_cast<std::uint32_t>(b[at + 2]) << 16U |
         static_cast<std::uint32_t>(b[at + 3]) << 24U;
}

BpsResult fail(const char *why) {
  BpsResult r;
  r.error = why;
  return r;
}

// byuu's variable-length number. Overflow of 64 bits is rejected.
bool read_number(std::span<const std::uint8_t> p, std::size_t &at, std::size_t end, std::uint64_t &out) {
  std::uint64_t data = 0, shift = 1;
  for (;;) {
    if (at >= end) return false;
    const std::uint8_t x = p[at++];
    const std::uint64_t add = static_cast<std::uint64_t>(x & 0x7FU);
    if (add != 0 && shift > (UINT64_MAX - data) / add) return false;
    data += add * shift;
    if ((x & 0x80U) != 0U) break;
    if (shift > (UINT64_MAX >> 7U)) return false;
    shift <<= 7U;
    if (data > UINT64_MAX - shift) return false;
    data += shift;
  }
  out = data;
  return true;
}

}  // namespace

BpsResult apply_bps(std::span<const std::uint8_t> patch, std::span<const std::uint8_t> source, std::size_t max_output) {
  if (patch.size() > kMaxPatchBytes) return fail("the BPS payload is too large");
  if (patch.size() < 4 + 3 + 12) return fail("the BPS payload is truncated");
  if (patch[0] != 'B' || patch[1] != 'P' || patch[2] != 'S' || patch[3] != '1') return fail("the payload is not a BPS1 patch");
  const std::size_t footer = patch.size() - 12;
  if (crc32(patch.first(patch.size() - 4)) != le32(patch, patch.size() - 4)) return fail("the BPS patch checksum does not match");
  std::size_t at = 4;
  std::uint64_t source_size = 0, target_size = 0, metadata_size = 0;
  if (!read_number(patch, at, footer, source_size) || !read_number(patch, at, footer, target_size) || !read_number(patch, at, footer, metadata_size))
    return fail("the BPS header is malformed");
  if (source_size != source.size()) return fail("the BPS source size does not match the original image");
  if (target_size > max_output) return fail("the BPS target size exceeds the allowed output size");
  if (metadata_size > footer - at) return fail("the BPS metadata runs past the patch");
  at += static_cast<std::size_t>(metadata_size);
  if (crc32(source) != le32(patch, footer)) return fail("the BPS source checksum does not match the original image");

  BpsResult result;
  std::vector<std::uint8_t> &out = result.output;
  out.resize(static_cast<std::size_t>(target_size));
  std::size_t out_at = 0;
  std::uint64_t source_rel = 0, target_rel = 0;
  while (out_at < out.size()) {
    std::uint64_t word = 0;
    if (at >= footer || !read_number(patch, at, footer, word)) return fail("the BPS command stream is truncated");
    const std::uint64_t length = (word >> 2U) + 1U;
    if (length > out.size() - out_at) return fail("a BPS command writes past the target size");
    const std::size_t n = static_cast<std::size_t>(length);
    switch (word & 3U) {
      case 0:  // SourceRead
        if (out_at + n > source.size()) return fail("a BPS SourceRead runs past the source");
        for (std::size_t i = 0; i < n; ++i) out[out_at + i] = source[out_at + i];
        break;
      case 1:  // TargetRead
        if (n > footer - at) return fail("a BPS TargetRead runs past the patch");
        for (std::size_t i = 0; i < n; ++i) out[out_at + i] = patch[at + i];
        at += n;
        break;
      case 2:    // SourceCopy
      case 3: {  // TargetCopy
        std::uint64_t raw = 0;
        if (!read_number(patch, at, footer, raw)) return fail("a BPS copy offset is truncated");
        const bool negative = (raw & 1U) != 0U;
        const std::uint64_t magnitude = raw >> 1U;
        std::uint64_t &rel = (word & 3U) == 2U ? source_rel : target_rel;
        if (negative) {
          if (magnitude > rel) return fail("a BPS copy offset is out of range");
          rel -= magnitude;
        } else {
          if (magnitude > UINT64_MAX - rel) return fail("a BPS copy offset is out of range");
          rel += magnitude;
        }
        if ((word & 3U) == 2U) {
          if (rel > source.size() || n > source.size() - static_cast<std::size_t>(rel)) return fail("a BPS SourceCopy runs past the source");
          for (std::size_t i = 0; i < n; ++i) out[out_at + i] = source[static_cast<std::size_t>(rel) + i];
        } else {
          if (rel >= out_at) return fail("a BPS TargetCopy reads output that does not exist yet");
          for (std::size_t i = 0; i < n; ++i) out[out_at + i] = out[static_cast<std::size_t>(rel) + i];  // may overlap: byte order is the format
        }
        rel += n;
        break;
      }
      default: break;
    }
    out_at += n;
  }
  if (at != footer) return fail("the BPS command stream has trailing bytes");
  if (crc32(out) != le32(patch, footer + 4)) return fail("the BPS target checksum does not match the patched image");
  result.ok = true;
  return result;
}

namespace {

void put_number(std::vector<std::uint8_t> &out, std::uint64_t n) {   // byuu variable-length number (the inverse of read_number)
  for (;;) {
    const std::uint8_t x = static_cast<std::uint8_t>(n & 0x7FU);
    n >>= 7U;
    if (n == 0) { out.push_back(static_cast<std::uint8_t>(0x80U | x)); return; }
    out.push_back(x);
    --n;
  }
}

void put_le32(std::vector<std::uint8_t> &out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}

}  // namespace

BpsResult build_bps(std::span<const std::uint8_t> source, std::span<const std::uint8_t> target) {
  if (source.size() > kMaxImageBytes || target.size() > kMaxImageBytes) return fail("an image exceeds the supported image size");
  std::vector<std::uint8_t> p = {'B', 'P', 'S', '1'};
  put_number(p, source.size());
  put_number(p, target.size());
  put_number(p, 0);   // no metadata
  const auto same = [&](std::size_t i) { return i < source.size() && source[i] == target[i]; };
  std::size_t i = 0, literal_start = 0;
  const auto flush_literal = [&](std::size_t end) {
    if (end > literal_start) {
      put_number(p, (static_cast<std::uint64_t>(end - literal_start - 1) << 2U) | 1U);
      p.insert(p.end(), target.begin() + static_cast<std::ptrdiff_t>(literal_start), target.begin() + static_cast<std::ptrdiff_t>(end));
    }
  };
  while (i < target.size()) {
    if (!same(i)) { ++i; continue; }
    std::size_t j = i;
    while (j < target.size() && same(j)) ++j;
    if (j - i >= kBpsMinCopyRun || j == target.size()) {   // a long run (or the tail) is a SourceRead; short interior runs stay in the literal
      flush_literal(i);
      put_number(p, static_cast<std::uint64_t>(j - i - 1) << 2U);
      literal_start = j;
    }
    i = j;
  }
  flush_literal(target.size());
  put_le32(p, crc32(source));
  put_le32(p, crc32(target));
  put_le32(p, crc32(p));
  if (p.size() > kMaxPatchBytes) return fail("the BPS patch exceeds the supported patch size");
  const BpsResult back = apply_bps(p, source, target.size());   // round trip: the writer is only trusted through the reader
  if (!back.ok || back.output.size() != target.size() || !std::equal(back.output.begin(), back.output.end(), target.begin())) return fail("the constructed BPS does not reproduce the target");
  BpsResult r;
  r.ok = true;
  r.output = std::move(p);
  return r;
}

}  // namespace segarecomp::patch
