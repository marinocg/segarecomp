// SEG-111 (ADR 0255): see relocation.hpp.
#include "segarecomp/machine/sega_cd/relocation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace segarecomp::machine::sega_cd {

const char *fragment_error_name(FragmentError error) noexcept {
  switch (error) {
    case FragmentError::none: return "none";
    case FragmentError::bad_evidence: return "bad_evidence";
    case FragmentError::no_stream_evidence: return "no_stream_evidence";
    case FragmentError::unsupported_region: return "unsupported_region";
    case FragmentError::no_stream: return "no_stream";
    case FragmentError::stream_evicted: return "stream_evicted";
    case FragmentError::stream_overwritten: return "stream_overwritten";
    case FragmentError::stream_dropped: return "stream_dropped";
    case FragmentError::stream_too_large: return "stream_too_large";
    case FragmentError::stream_not_live: return "stream_not_live";
    case FragmentError::foreign_writer: return "foreign_writer";
    case FragmentError::volatile_input: return "volatile_input";
    case FragmentError::too_short: return "too_short";
    case FragmentError::not_a_copy: return "not_a_copy";
    case FragmentError::source_discontiguous: return "source_discontiguous";
    case FragmentError::source_not_admitted: return "source_not_admitted";
    case FragmentError::ambiguous_source: return "ambiguous_source";
    case FragmentError::source_modified: return "source_modified";
    case FragmentError::ledger_overflow: return "ledger_overflow";
    case FragmentError::no_delivery_evidence: return "no_delivery_evidence";
    case FragmentError::source_not_delivered: return "source_not_delivered";
    case FragmentError::first_track_not_data: return "first_track_not_data";
    case FragmentError::disc_unreadable: return "disc_unreadable";
    case FragmentError::not_computed: return "not_computed";
  }
  return "unknown";
}

namespace {

constexpr std::uint32_t kWorkBase = 0x00FF0000U, kWorkBytes = 0x10000U, kPrgBytes = 0x80000U;

// A strict sequential reader: every literal and number must be exactly where the runtime's fixed-order writer puts it.
struct Cursor {
  std::string_view text;
  std::size_t at{};
  bool ok{true};
  void expect(std::string_view literal) {
    if (!ok || text.substr(at, literal.size()) != literal) { ok = false; return; }
    at += literal.size();
  }
  std::uint32_t number() {
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (ok && at < text.size() && text[at] >= '0' && text[at] <= '9') {
      value = value * 10U + static_cast<std::uint64_t>(text[at] - '0');
      if (value > 0xFFFFFFFFULL) ok = false;
      ++at;
      ++digits;
    }
    if (digits == 0U || (digits > 1U && text[at - digits] == '0')) ok = false;   // no leading zeros, at least one digit
    return static_cast<std::uint32_t>(value);
  }
  bool flag() {
    const std::uint32_t v = number();
    if (v > 1U) ok = false;
    return v == 1U;
  }
  std::string_view hex() {
    const std::size_t begin = at;
    while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f'))) ++at;
    return text.substr(begin, at - begin);
  }
};

struct Owner {
  const WriteStream *stream{};
  std::uint32_t address{};   // the stream's first byte in the frontier processor's space
  std::uint32_t base{};      // the claimed (even) range
  std::uint32_t length{};
};

struct OwnerResult {
  Owner owner;
  FragmentError error{FragmentError::none};
};

OwnerResult select_owner(const FrontierEvidence &f, const StreamEvidence &evidence) {
  const auto fail = [](FragmentError e) { return OwnerResult{{}, e}; };
  if (!evidence.present) return fail(FragmentError::no_stream_evidence);
  const bool main_cpu = f.processor == Processor::main;
  const std::uint32_t space = main_cpu ? kWorkBase : 0U;
  if (main_cpu ? !(f.pc >= kWorkBase) : !(f.pc < kPrgBytes)) return fail(FragmentError::unsupported_region);
  if (f.pc < f.region_base || f.pc - f.region_base >= f.region.size()) return fail(FragmentError::bad_evidence);
  const WriteStream *clean = nullptr, *dirty = nullptr;
  for (const WriteStream &s : evidence.items) {
    if (s.work_ram != main_cpu) continue;
    const std::uint32_t at = space + s.destination;
    if (f.pc < at || f.pc - at >= s.length) continue;
    if (s.clobbered) { dirty = &s; continue; }
    if (clean != nullptr) return fail(FragmentError::bad_evidence);   // two clean owners of one byte cannot exist: refuse the evidence
    clean = &s;
  }
  if (clean == nullptr) return fail(dirty != nullptr ? FragmentError::stream_overwritten : evidence.evicted != 0U ? FragmentError::stream_evicted : FragmentError::no_stream);
  const WriteStream &s = *clean;
  if (s.writer_cpu != static_cast<std::uint32_t>(f.processor)) return fail(FragmentError::foreign_writer);
  if (s.dropped) return fail(FragmentError::stream_dropped);
  if (s.volatile_input) return fail(FragmentError::volatile_input);
  if (s.too_large || s.length > kMaxStreamBytes) return fail(FragmentError::stream_too_large);
  if (s.bytes.size() != s.length) return fail(FragmentError::bad_evidence);
  const std::uint32_t address = space + s.destination;
  if (address < f.region_base || address + s.length > f.region_base + f.region.size()) return fail(FragmentError::bad_evidence);
  if (std::memcmp(s.bytes.data(), f.region.data() + (address - f.region_base), s.length) != 0) return fail(FragmentError::stream_not_live);
  const std::uint32_t base = address + (address & 1U);
  const std::uint32_t end = address + s.length;
  if (end <= base) return fail(FragmentError::too_short);
  const std::uint32_t length = (end - base) & ~1U;
  if (length < kMinFragmentBytes || (f.pc & 1U) != 0U || f.pc < base || f.pc - base >= length) return fail(FragmentError::too_short);
  return OwnerResult{Owner{&s, address, base, length}, FragmentError::none};
}

std::vector<std::uint8_t> claimed_bytes(const Owner &o) {
  const auto first = o.stream->bytes.begin() + (o.base - o.address);
  return std::vector<std::uint8_t>(first, first + o.length);
}

}  // namespace

std::optional<StreamEvidence> parse_stream_evidence(std::string_view json) {
  const std::size_t at = json.find("\"streams\":{");
  if (at == std::string_view::npos) return StreamEvidence{};
  Cursor c{json, at};
  StreamEvidence out;
  out.present = true;
  c.expect("\"streams\":{\"version\":1,\"slots\":");
  if (c.number() != kMaxStreams) c.ok = false;
  c.expect(",\"max_bytes\":");
  if (c.number() != kMaxStreamBytes) c.ok = false;
  c.expect(",\"writes\":");
  (void)c.number();
  c.expect(",\"evicted\":");
  out.evicted = c.number();
  c.expect(",\"items\":[");
  for (bool first = true; c.ok && !(c.at < json.size() && json[c.at] == ']'); first = false) {
    if (out.items.size() >= kMaxStreams) { c.ok = false; break; }
    if (!first) c.expect(",");
    WriteStream s;
    c.expect("{\"writer_cpu\":");
    s.writer_cpu = c.number();
    if (s.writer_cpu > 1U) c.ok = false;
    c.expect(",\"memory\":\"");
    if (json.substr(c.at, 4) == "work") { s.work_ram = true; c.at += 4; }
    else if (json.substr(c.at, 3) == "prg") { s.work_ram = false; c.at += 3; }
    else c.ok = false;
    c.expect("\",\"destination\":");
    s.destination = c.number();
    c.expect(",\"length\":");
    s.length = c.number();
    c.expect(",\"pc\":");
    s.pc = c.number();
    c.expect(",\"paired\":");
    s.paired = c.flag();
    c.expect(",\"source\":");
    s.source = c.number();
    c.expect(",\"source_class\":");
    s.source_class = c.number();
    c.expect(",\"source_gap\":");
    s.source_gap = c.flag();
    c.expect(",\"clobbered\":");
    s.clobbered = c.flag();
    c.expect(",\"dropped\":");
    s.dropped = c.flag();
    c.expect(",\"too_large\":");
    s.too_large = c.flag();
    c.expect(",\"volatile\":");
    s.volatile_input = c.flag();
    c.expect(",\"bytes\":\"");
    const std::string_view hex = c.hex();
    c.expect("\"}");
    if (!c.ok) break;
    const std::uint64_t ram = s.work_ram ? kWorkBytes : kPrgBytes;
    const std::uint32_t stored = std::min(s.length, kMaxStreamBytes);
    if (s.length == 0U || s.destination >= ram || static_cast<std::uint64_t>(s.destination) + s.length > ram || hex.size() != std::size_t{stored} * 2U ||
        s.too_large != (s.length > kMaxStreamBytes) || s.source_class > 5U) {
      c.ok = false;
      break;
    }
    s.bytes.reserve(stored);
    for (std::size_t i = 0; i < hex.size(); i += 2U) {
      const auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
      s.bytes.push_back(static_cast<std::uint8_t>(nibble(hex[i]) * 16 + nibble(hex[i + 1U])));
    }
    out.items.push_back(std::move(s));
  }
  c.expect("]}");
  if (!c.ok) return std::nullopt;
  return out;
}

std::optional<FrontierCapture> parse_frontier_capture(std::string_view json, std::vector<std::uint8_t> region) {
  Cursor c{json, 0};
  FrontierCapture out;
  c.expect("{\"kind\":\"frontier\",\"pass_identity\":\"");
  const std::string_view id = c.hex();
  out.pass_identity = std::string(id);
  c.expect("\",\"cpu\":");
  const std::uint32_t cpu = c.number();
  if (cpu > 1U) c.ok = false;
  out.frontier.processor = cpu == 0U ? Processor::main : Processor::sub;
  c.expect(",\"pc\":");
  out.frontier.pc = c.number();
  c.expect(",\"bytes\":\"");
  (void)c.hex();
  c.expect("\"");
  if (c.ok && json.substr(c.at, 8) == ",\"run\":{") {   // an operand-template slot run (not a relocation concern): skip its object
    const std::size_t close = json.find('}', c.at);
    if (close == std::string_view::npos) return std::nullopt;
    c.at = close + 1U;
  }
  c.expect(",\"region\":{\"base\":");
  out.frontier.region_base = c.number();
  c.expect(",\"length\":");
  const std::uint32_t length = c.number();
  c.expect(",\"track1_first_lba\":");
  (void)c.number();
  c.expect(",\"delivered_overflow\":");
  out.frontier.delivered_overflow = c.flag();
  c.expect(",\"delivered\":[");
  for (bool first = true; c.ok && !(c.at < json.size() && json[c.at] == ']'); first = false) {
    if (!first) c.expect(",");
    c.expect("[");
    LedgerRange r;
    r.first_lba = c.number();
    c.expect(",");
    r.count = c.number();
    c.expect("]");
    if (out.frontier.delivered.size() >= 4096U) c.ok = false;
    if (c.ok) out.frontier.delivered.push_back(r);
  }
  c.expect("]}");
  if (!c.ok || id.size() != 64U || length != region.size() || region.empty()) return std::nullopt;
  out.frontier.region = std::move(region);
  return out;
}

FragmentError read_track1_span(const DiscImage &disc, std::uint32_t offset, std::uint32_t length, std::vector<std::uint8_t> &bytes) {
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return FragmentError::first_track_not_data;
  const DiscTrack &first = disc.tracks().front();
  const std::uint64_t end = static_cast<std::uint64_t>(offset) + length;
  if ((end + kDiscUserBytes - 1U) / kDiscUserBytes > first.sector_count) return FragmentError::disc_unreadable;
  bytes.clear();
  bytes.reserve(length);
  std::array<std::uint8_t, kDiscUserBytes> block{};
  for (std::uint64_t at = offset; at < end;) {
    const std::uint32_t lba = first.first_lba + static_cast<std::uint32_t>(at / kDiscUserBytes);
    if (disc.read_user_data(lba, std::span<std::uint8_t, kDiscUserBytes>(block)) != DiscError::none) return FragmentError::disc_unreadable;
    const std::uint32_t inside = static_cast<std::uint32_t>(at % kDiscUserBytes);
    const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(kDiscUserBytes - inside, end - at));
    bytes.insert(bytes.end(), block.begin() + inside, block.begin() + inside + take);
    at += take;
  }
  return FragmentError::none;
}

FragmentResult derive_relocation(const DiscImage &disc, const FrontierEvidence &f, const StreamEvidence &streams, const std::vector<VerifiedExtent> &extents) {
  const auto fail = [](FragmentError e) { return FragmentResult{std::nullopt, e}; };
  const OwnerResult owned = select_owner(f, streams);
  if (owned.error != FragmentError::none) return fail(owned.error);
  const Owner &o = owned.owner;
  const WriteStream &s = *o.stream;
  if (!s.paired) return fail(FragmentError::not_a_copy);
  if (s.source_gap) return fail(FragmentError::source_discontiguous);
  if (s.source_class != 1U && s.source_class != 2U && s.source_class != 3U) return fail(FragmentError::source_not_admitted);
  const std::uint64_t source = static_cast<std::uint64_t>(s.source) + (o.base - o.address);
  // the source must lie wholly inside an admitted extent of this processor; every covering extent must map it to the same disc bytes
  std::set<std::uint64_t> disc_offsets;
  for (const VerifiedExtent &e : extents)
    if (e.processor == f.processor && source >= e.base && source + o.length <= static_cast<std::uint64_t>(e.base) + e.length)
      disc_offsets.insert(static_cast<std::uint64_t>(e.disc_offset) + (source - e.base));
  if (disc_offsets.empty()) return fail(FragmentError::source_not_admitted);
  if (disc_offsets.size() > 1U) return fail(FragmentError::ambiguous_source);
  const std::uint64_t offset = *disc_offsets.begin();
  if (offset > 0xFFFFFFFFULL) return fail(FragmentError::source_not_admitted);
  // the disc blocks must have been delivered (an undelivered span is never an observed input) and the delivery evidence must be complete
  if (f.delivered_overflow) return fail(FragmentError::ledger_overflow);
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return fail(FragmentError::first_track_not_data);
  const DiscTrack &track = disc.tracks().front();
  const std::vector<LedgerRange> merged = merge_delivered(track, f.delivered);
  if (merged.empty()) return fail(FragmentError::no_delivery_evidence);
  const std::uint64_t lo = static_cast<std::uint64_t>(track.first_lba) + offset / kDiscUserBytes;
  const std::uint64_t hi = static_cast<std::uint64_t>(track.first_lba) + (offset + o.length - 1U) / kDiscUserBytes;
  const bool delivered = std::any_of(merged.begin(), merged.end(), [&](const LedgerRange &r) {
    return lo >= r.first_lba && hi < static_cast<std::uint64_t>(r.first_lba) + r.count;
  });
  if (!delivered) return fail(FragmentError::source_not_delivered);
  std::vector<std::uint8_t> disc_bytes;
  if (const FragmentError error = read_track1_span(disc, static_cast<std::uint32_t>(offset), o.length, disc_bytes); error != FragmentError::none) return fail(error);
  std::vector<std::uint8_t> bytes = claimed_bytes(o);
  if (disc_bytes != bytes) return fail(FragmentError::source_modified);
  return FragmentResult{Fragment{true, f.processor, static_cast<std::uint32_t>(offset), o.base, o.length, std::move(bytes)}, FragmentError::none};
}

FragmentResult derive_produced_image(const FrontierEvidence &f, const StreamEvidence &streams) {
  const OwnerResult owned = select_owner(f, streams);
  if (owned.error != FragmentError::none) return FragmentResult{std::nullopt, owned.error};
  if (owned.owner.stream->paired) return FragmentResult{std::nullopt, FragmentError::not_computed};
  return FragmentResult{Fragment{false, f.processor, 0U, owned.owner.base, owned.owner.length, claimed_bytes(owned.owner)}, FragmentError::none};
}

}  // namespace segarecomp::machine::sega_cd
