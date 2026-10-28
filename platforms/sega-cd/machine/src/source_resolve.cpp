// SEG-078-T003 (ADR 0205): see source_resolve.hpp.
#include "segarecomp/machine/sega_cd/source_resolve.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace segarecomp::machine::sega_cd {

const char *resolve_error_name(ResolveError error) noexcept {
  switch (error) {
    case ResolveError::none: return "none";
    case ResolveError::bad_evidence: return "bad_evidence";
    case ResolveError::ledger_overflow: return "ledger_overflow";
    case ResolveError::no_delivery_evidence: return "no_delivery_evidence";
    case ResolveError::first_track_not_data: return "first_track_not_data";
    case ResolveError::delivered_too_large: return "delivered_too_large";
    case ResolveError::disc_unreadable: return "disc_unreadable";
    case ResolveError::no_source: return "no_source";
    case ResolveError::too_short: return "too_short";
    case ResolveError::ambiguous_source: return "ambiguous_source";
  }
  return "unknown";
}

namespace {

// One contiguous run of delivered track-1 blocks, read into `bytes`; `offset` is its track-1 user-data offset.
struct Segment {
  std::uint64_t offset{};
  std::vector<std::uint8_t> bytes;
};

struct Candidate {
  std::uint64_t disc_offset{};  // of the first byte of the run
  std::uint32_t region_begin{}; // index of the first byte of the run in the window
  std::uint32_t length{};
};

}  // namespace

ResolveResult resolve_source(const DiscImage &disc, const SourceEvidence &e) {
  const auto fail = [](ResolveError error) { return ResolveResult{std::nullopt, error}; };
  const std::uint64_t n = e.region.size();
  if (e.pc < e.region_base || e.pc - e.region_base >= n || n < kResolveAnchorBytes) return fail(ResolveError::bad_evidence);
  if (e.delivered_overflow) return fail(ResolveError::ledger_overflow);
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return fail(ResolveError::first_track_not_data);
  const DiscTrack &track = disc.tracks().front();
  // The delivered blocks of track 1, clipped to it, sorted, merged into contiguous segments.
  std::vector<LedgerRange> ranges;
  for (const LedgerRange &r : e.delivered) {
    const std::uint64_t lo = std::max<std::uint64_t>(r.first_lba, track.first_lba);
    const std::uint64_t hi = std::min<std::uint64_t>(static_cast<std::uint64_t>(r.first_lba) + r.count, static_cast<std::uint64_t>(track.first_lba) + track.sector_count);
    if (lo < hi) ranges.push_back({static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi - lo)});
  }
  std::sort(ranges.begin(), ranges.end(), [](const LedgerRange &a, const LedgerRange &b) { return a.first_lba < b.first_lba; });
  std::vector<LedgerRange> merged;
  for (const LedgerRange &r : ranges) {
    if (!merged.empty() && r.first_lba <= merged.back().first_lba + merged.back().count) {
      const std::uint32_t end = std::max(merged.back().first_lba + merged.back().count, r.first_lba + r.count);
      merged.back().count = end - merged.back().first_lba;
    } else {
      merged.push_back(r);
    }
  }
  std::uint64_t total = 0;
  for (const LedgerRange &r : merged) total += static_cast<std::uint64_t>(r.count) * kDiscUserBytes;
  if (total == 0) return fail(ResolveError::no_delivery_evidence);
  if (total > kResolveMaxDeliveredBytes) return fail(ResolveError::delivered_too_large);
  std::vector<Segment> segments;
  for (const LedgerRange &r : merged) {
    Segment s;
    s.offset = static_cast<std::uint64_t>(r.first_lba - track.first_lba) * kDiscUserBytes;
    s.bytes.resize(static_cast<std::size_t>(r.count) * kDiscUserBytes);
    for (std::uint32_t k = 0; k < r.count; ++k) {
      std::span<std::uint8_t, kDiscUserBytes> block(s.bytes.data() + static_cast<std::size_t>(k) * kDiscUserBytes, kDiscUserBytes);
      if (disc.read_user_data(r.first_lba + k, block) != DiscError::none) return fail(ResolveError::disc_unreadable);
    }
    segments.push_back(std::move(s));
  }

  const std::uint32_t pc_index = e.pc - e.region_base;
  const std::uint32_t anchor_at = pc_index + kResolveAnchorBytes <= n ? pc_index : static_cast<std::uint32_t>(n - kResolveAnchorBytes);
  const std::uint8_t *anchor = e.region.data() + anchor_at;
  std::vector<Candidate> candidates;
  std::uint32_t occurrences = 0;
  for (const Segment &s : segments) {
    const std::uint8_t *begin = s.bytes.data(), *end = begin + s.bytes.size();
    for (const std::uint8_t *at = begin; at + kResolveAnchorBytes <= end;) {
      at = static_cast<const std::uint8_t *>(memchr(at, anchor[0], static_cast<std::size_t>(end - at)));
      if (at == nullptr || at + kResolveAnchorBytes > end) break;
      if (memcmp(at, anchor, kResolveAnchorBytes) == 0) {
        if (++occurrences > kResolveMaxCandidates) return fail(ResolveError::ambiguous_source);
        // extend both ways while the window equals the delivered segment
        std::uint32_t lo = anchor_at, hi = anchor_at + kResolveAnchorBytes;
        const std::uint8_t *sl = at, *sh = at + kResolveAnchorBytes;
        while (lo > 0 && sl > begin && e.region[lo - 1U] == sl[-1]) { --lo; --sl; }
        while (hi < n && sh < end && e.region[hi] == *sh) { ++hi; ++sh; }
        candidates.push_back({s.offset + static_cast<std::uint64_t>(sl - begin), lo, hi - lo});
      }
      ++at;
    }
  }
  if (candidates.empty()) return fail(ResolveError::no_source);
  std::uint32_t best = 0;
  for (const Candidate &c : candidates) best = std::max(best, c.length);
  std::vector<const Candidate *> top;
  for (const Candidate &c : candidates)
    if (c.length == best) top.push_back(&c);
  if (top.size() > 1) return fail(ResolveError::ambiguous_source);
  const Candidate &c = *top.front();
  // the extent: an even base and an even length
  std::uint32_t begin_index = c.region_begin, length = c.length;
  std::uint64_t offset = c.disc_offset;
  if (((e.region_base + begin_index) & 1U) != 0U) { ++begin_index; ++offset; --length; }
  if ((length & 1U) != 0U) --length;
  if (length < kResolveMinRun) return fail(ResolveError::too_short);
  ResolvedSource out;
  out.disc_offset = static_cast<std::uint32_t>(offset);
  out.base = e.region_base + begin_index;
  out.length = length;
  out.candidates = occurrences;
  return ResolveResult{out, ResolveError::none};
}

}  // namespace segarecomp::machine::sega_cd
