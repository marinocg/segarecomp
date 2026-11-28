// Sega CD verified disc edits and main-M68K guest patching (ADR 0287 contract, ADR 0328; SEG-148-T002/T003). Declaration checks, the plan over a verified original disc, the layout rules,
// streaming materialization and the code-eligibility rules. Originals are only read.
#include "segarecomp/mod_disc.hpp"

#include "segarecomp/iso9660.hpp"
#include "segarecomp/patch.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <tuple>

namespace segarecomp::mod {

namespace {

namespace fs = std::filesystem;

struct Fail {
  std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) { throw Fail{std::move(code), std::move(message)}; }

constexpr const char *kSub = "unsupported_code_class:sub_m68k", *kZ80 = "unsupported_code_class:z80", *kLayout = "unsupported_code_class:layout_changing", *kNotProven = "unsupported_code_class:not_disc_proven",
                     *kDynamic = "unsupported_code_class:dynamic", *kModule = "unsupported_code_class:unrecognized_module", *kUndeclared = "unsupported_code_class:undeclared_code";
constexpr std::size_t kChunk = 1U << 20U;
constexpr std::uint32_t kRawUserAt = 16;   // sync + header of a MODE1/2352 sector

bool hex64(const std::string &s) { return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool printable_path(const std::string &s) {
  return s.size() > 1 && s.size() <= 200 && s[0] == '/' && std::all_of(s.begin(), s.end(), [](char c) { return c > 0x20 && c < 0x7F && c != '"' && c != '\\'; });
}

std::vector<std::uint8_t> read_range(const DiscFile &file, std::uint64_t offset, std::uint64_t length) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
  std::ifstream in(file.path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(offset));
  if (!in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()))) fail("E_DISC_LOAD", "an original disc file cannot be read");
  return out;
}

// A contiguous piece of one file.
struct Piece {
  std::uint32_t file = 0;
  std::uint64_t offset = 0, length = 0;
};

const DiscTrack &data_track_one(const DiscImage &disc, const char *code) {
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) fail(code, "track 1 is not a data track");
  return disc.tracks().front();
}

std::uint64_t user_bytes(const DiscTrack &t) { return static_cast<std::uint64_t>(t.sector_count) * kDiscUserBytes; }

// Track-1 user-data range -> the file pieces that hold it (one per sector on a raw track). The caller has checked the range lies inside the track.
std::vector<Piece> user_pieces(const DiscTrack &t, std::uint64_t offset, std::uint64_t length) {
  std::vector<Piece> out;
  const std::uint64_t header = t.sector_size == kDiscRawBytes ? kRawUserAt : 0U;
  while (length != 0) {
    const std::uint64_t within = offset % kDiscUserBytes, take = std::min<std::uint64_t>(length, kDiscUserBytes - within);
    const std::uint64_t at = t.file_offset + offset / kDiscUserBytes * t.sector_size + header + within;
    if (!out.empty() && out.back().offset + out.back().length == at) out.back().length += take;
    else out.push_back({t.file_index, at, take});
    offset += take;
    length -= take;
  }
  return out;
}

// A raw CUE FILE range: inside the file, wholly in data-track sectors (never an INDEX 00 pregap or audio), inside the named track if one is named.
void check_raw_range(const DiscImage &disc, const Payload &p) {
  const char *range_class = p.code_cpu.empty() ? "E_DISC_RANGE" : kLayout;
  if (p.disc_target >= disc.files().size()) fail("E_DISC_RANGE", "\"" + p.path + "\" targets a CUE FILE index the disc does not have");
  const std::uint64_t begin = p.disc_start, end = begin + p.disc_length;
  if (end > disc.files()[p.disc_target].size) fail(range_class, "\"" + p.path + "\" extends past the end of its file (a disc patch never changes a file size)");
  std::uint64_t covered = 0;
  bool named = !p.has_disc_track;
  for (const DiscTrack &t : disc.tracks()) {
    if (t.file_index != p.disc_target) continue;
    const std::uint64_t data_end = t.file_offset + static_cast<std::uint64_t>(t.sector_count) * t.sector_size;
    const std::uint64_t pregap_begin = t.file_offset - static_cast<std::uint64_t>(t.index0_sectors) * t.sector_size;
    if (begin < t.file_offset && end > pregap_begin) fail("E_DISC_PREGAP", "\"" + p.path + "\" touches the INDEX 00 pregap of track " + std::to_string(t.number));
    if (p.has_disc_track && t.number == p.disc_track) {
      if (begin < t.file_offset || end > data_end) fail("E_DISC_RANGE", "\"" + p.path + "\" leaves the track it names");
      named = true;
    }
    const std::uint64_t lo = std::max(begin, t.file_offset), hi = std::min(end, data_end);
    if (lo >= hi) continue;
    if (t.type == DiscTrackType::audio) fail("E_DISC_AUDIO_TARGET", "\"" + p.path + "\" edits audio track " + std::to_string(t.number) + "; a disc-extent-edit edits data tracks only");
    covered += hi - lo;
  }
  if (!named) fail("E_DISC_RANGE", "\"" + p.path + "\" names a track that is not in its file");
  if (covered != p.disc_length) fail("E_DISC_RANGE", "\"" + p.path + "\" does not lie wholly inside the data tracks of its file");
}

// The user-data runs of a raw range that falls in track 1 (nothing for bytes elsewhere: headers, EDC/ECC, later tracks).
std::vector<std::pair<std::uint64_t, std::uint64_t>> user_runs(const DiscImage &disc, std::uint32_t file, std::uint64_t begin, std::uint64_t end) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return out;
  const DiscTrack &t = disc.tracks().front();
  if (t.file_index != file) return out;
  const std::uint64_t header = t.sector_size == kDiscRawBytes ? kRawUserAt : 0U;
  const std::uint64_t area_end = t.file_offset + static_cast<std::uint64_t>(t.sector_count) * t.sector_size;
  const std::uint64_t lo = std::max(begin, t.file_offset), hi = std::min(end, area_end);
  if (lo >= hi) return out;
  for (std::uint64_t s = (lo - t.file_offset) / t.sector_size; t.file_offset + s * t.sector_size < hi; ++s) {
    const std::uint64_t user_at = t.file_offset + s * t.sector_size + header;
    const std::uint64_t a = std::max(lo, user_at), b = std::min(hi, user_at + kDiscUserBytes);
    if (a >= b) continue;
    const std::uint64_t offset = s * kDiscUserBytes + (a - user_at);
    if (!out.empty() && out.back().first + out.back().second == offset) out.back().second += b - a;
    else out.emplace_back(offset, b - a);
  }
  return out;
}

// A raw (2352) data sector's sync pattern and header are layout, not content: an edit may not change them.
void check_sector_headers(const DiscImage &disc, const std::string &path, const Piece &piece, const std::vector<std::uint8_t> &before, const std::uint8_t *after) {
  for (const DiscTrack &t : disc.tracks()) {
    if (t.file_index != piece.file || t.sector_size != kDiscRawBytes || t.type == DiscTrackType::audio) continue;
    const std::uint64_t end = t.file_offset + static_cast<std::uint64_t>(t.sector_count) * t.sector_size;
    for (std::uint64_t i = 0; i < piece.length; ++i) {
      const std::uint64_t at = piece.offset + i;
      if (at < t.file_offset || at >= end || (at - t.file_offset) % t.sector_size >= kRawUserAt) continue;
      if (before[static_cast<std::size_t>(i)] != after[i]) fail("E_DISC_SECTOR_HEADER", "\"" + path + "\" changes the sync pattern or header of a raw sector");
    }
  }
}

Iso9660Extent iso_extent(const DiscImage &disc, const std::string &path, const char *code) {
  const DiscTrack &t = data_track_one(disc, code);
  const auto read = iso9660_track_reader(disc, 0);
  const auto volume = read_iso9660_volume(read, t.sector_count);
  if (!volume.volume) fail(code, std::string("the ISO9660 volume cannot be read: ") + iso9660_error_name(volume.error));
  const auto found = lookup_iso9660_file(read, *volume.volume, path);
  if (!found.extent) fail(code, "\"" + path + "\" does not name a file of the volume: " + iso9660_error_name(found.error));
  return *found.extent;
}

void apply_edit(const Payload &p, const std::vector<std::uint8_t> &payload, const DiscImage &disc, DiscPlan &plan) {
  const bool code = !p.code_cpu.empty();
  std::vector<Piece> pieces;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
  if (!p.iso_file.empty()) {
    const DiscTrack &t = data_track_one(disc, "E_DISC_ISO_FILE");
    if (p.disc_target != t.file_index || (p.has_disc_track && p.disc_track != t.number)) fail("E_DISC_ISO_FILE", "\"" + p.path + "\": an ISO9660 file lives in track 1, so the target is track 1's CUE FILE");
    const Iso9660Extent e = iso_extent(disc, p.iso_file, "E_DISC_ISO_FILE");
    if (p.disc_start + p.disc_length > e.size) fail(code ? kLayout : "E_DISC_RANGE", "\"" + p.path + "\" extends past the end of " + p.iso_file + " (a disc patch never changes a file size)");
    const std::uint64_t at = static_cast<std::uint64_t>(e.block) * kDiscUserBytes + p.disc_start;
    if (at + p.disc_length > user_bytes(t)) fail("E_DISC_RANGE", "\"" + p.path + "\" leaves track 1");
    pieces = user_pieces(t, at, p.disc_length);
    runs.emplace_back(at, p.disc_length);
  } else {
    check_raw_range(disc, p);
    pieces.push_back({p.disc_target, p.disc_start, p.disc_length});
    runs = user_runs(disc, p.disc_target, p.disc_start, p.disc_start + p.disc_length);
  }
  if (code && (runs.size() != 1 || runs.front().second != p.disc_length))
    fail(kNotProven, "\"" + p.path + "\": a code patch is one contiguous range of the user data of track 1, the only bytes the build can prove a load address for");
  std::vector<std::uint8_t> original;
  original.reserve(static_cast<std::size_t>(p.disc_length));
  std::vector<std::vector<std::uint8_t>> before;
  for (const Piece &piece : pieces) {
    before.push_back(read_range(disc.files()[piece.file], piece.offset, piece.length));
    original.insert(original.end(), before.back().begin(), before.back().end());
  }
  if (sha256_hex(original) != p.original_sha256) fail("E_DISC_ORIGINAL_MISMATCH", "\"" + p.path + "\": the original bytes of the range do not have the declared SHA-256");
  std::vector<std::uint8_t> value = payload;
  if (p.replacement == "bps") {
    const patch::BpsResult r = patch::apply_bps(payload, original);
    if (!r.ok) fail("E_DISC_DELTA", "\"" + p.path + "\": the BPS delta does not apply to the original range: " + r.error);
    value = r.output;
  }
  if (value.size() != p.disc_length) fail(code ? kLayout : "E_DISC_PAYLOAD_SIZE", "\"" + p.path + "\": the replacement is not exactly the size of the range");
  std::uint64_t done = 0;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    check_sector_headers(disc, p.path, pieces[i], before[i], value.data() + done);
    plan.writes.push_back({pieces[i].file, pieces[i].offset, {value.begin() + static_cast<std::ptrdiff_t>(done), value.begin() + static_cast<std::ptrdiff_t>(done + pieces[i].length)}});
    done += pieces[i].length;
  }
  for (const auto &run : runs) plan.spans.push_back({run.first, run.second, p.load_address, code, "edit " + p.path});
  plan.bytes_written += p.disc_length;
  ++plan.edits_applied;
}

// Symbol names the guest text uses (comments excluded).
std::vector<Symbol> used_symbols(const Manifest &m, const std::vector<std::uint8_t> &text) {
  std::vector<Symbol> out;
  bool comment = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = static_cast<char>(text[i]);
    if (c == '\n') comment = false;
    else if (c == ';') comment = true;
    if (comment || c != '@') continue;
    std::size_t end = i + 1;
    while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || (text[end] >= '0' && text[end] <= '9') || text[end] == '_' || text[end] == '.')) ++end;   // `-` is the addend sign
    const std::string name(text.begin() + static_cast<std::ptrdiff_t>(i + 1), text.begin() + static_cast<std::ptrdiff_t>(end));
    for (const Symbol &y : m.symbols)
      if (y.name == name && std::none_of(out.begin(), out.end(), [&](const Symbol &u) { return u.name == name; })) out.push_back(y);
  }
  return out;
}

void inject(const Manifest &m, const std::map<std::string, std::vector<std::uint8_t>> &files, const Payload &p, const DiscImage &disc, DiscPlan &plan) {
  const GuestListingResult listed = guest_listing(m, files, "");
  if (!listed.ok) fail(listed.rejection.code, listed.rejection.message);
  const DiscTrack &t = data_track_one(disc, kNotProven);
  const std::vector<std::uint8_t> &text = files.at(p.path);
  std::vector<Piece> reserved;   // the free ranges' file bytes: no other edit may touch them
  // The assembler already checked that every block lies in one free range and that blocks and free ranges do not overlap each other.
  for (std::size_t i = 0; i < listed.listing.free_ranges.size(); ++i) {
    const Range &f = listed.listing.free_ranges[i];
    if (f.end > user_bytes(t)) fail(kLayout, "a free range extends past the end of track 1 (injection never grows a disc)");
    for (const Piece &piece : user_pieces(t, f.start, f.end - f.start)) {
      const std::vector<std::uint8_t> bytes = read_range(disc.files()[piece.file], piece.offset, piece.length);
      if (std::any_of(bytes.begin(), bytes.end(), [&](std::uint8_t b) { return b != listed.listing.free_fills[i]; })) fail("E_GUEST_RANGE_NOT_FREE", "the original bytes of a free range are not all the declared fill value");
      reserved.push_back(piece);
    }
  }
  for (const DiscWrite &w : plan.writes)
    for (const Piece &r : reserved)
      if (w.file == r.file && w.offset < r.offset + r.length && r.offset < w.offset + w.bytes.size()) fail("E_GUEST_COLLISION", "a disc edit overlaps a free range of the injected code");
  for (const GuestBlock &b : listed.listing.blocks) {
    std::uint64_t done = 0;
    for (const Piece &piece : user_pieces(t, b.image_offset, b.bytes.size())) {
      plan.writes.push_back({piece.file, piece.offset, {b.bytes.begin() + static_cast<std::ptrdiff_t>(done), b.bytes.begin() + static_cast<std::ptrdiff_t>(done + piece.length)}});
      done += piece.length;
    }
    plan.spans.push_back({b.image_offset, b.bytes.size(), b.address, true, "injected block (guest-asm line " + std::to_string(b.line) + ")"});
    plan.bytes_written += b.bytes.size();
    ++plan.blocks_injected;
  }
  plan.symbol_uses = used_symbols(m, text);
}

bool inside(const fs::path &path, const fs::path &folder) {
  std::error_code ec;
  for (fs::path at = path; !at.empty(); at = at.parent_path()) {
    if (fs::equivalent(at, folder, ec) && !ec) return true;
    if (at == at.parent_path()) break;
  }
  return false;
}

// Streams every original file through the planned writes. `sink` receives (file index, offset, expected effective chunk). Fails if an original changed since it was verified.
template <class Sink>
void stream_effective(const DiscPlan &plan, const DiscImage &original, Sink &&sink) {
  std::vector<std::uint8_t> chunk(kChunk);
  std::size_t next = 0;
  for (std::uint32_t f = 0; f < original.files().size(); ++f) {
    const DiscFile &file = original.files()[f];
    std::ifstream in(file.path, std::ios::binary);
    if (!in) fail("E_DISC_LOAD", "an original disc file cannot be read");
    Sha256Stream hash;
    while (next < plan.writes.size() && plan.writes[next].file < f) ++next;
    std::size_t first = next;
    for (std::uint64_t at = 0; at < file.size;) {
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, file.size - at));
      if (!in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(n))) fail("E_DISC_LOAD", "an original disc file cannot be read");
      hash.update({chunk.data(), n});
      while (first < plan.writes.size() && plan.writes[first].file == f && plan.writes[first].offset + plan.writes[first].bytes.size() <= at) ++first;
      for (std::size_t w = first; w < plan.writes.size() && plan.writes[w].file == f && plan.writes[w].offset < at + n; ++w) {
        const DiscWrite &write = plan.writes[w];
        const std::uint64_t lo = std::max(at, write.offset), hi = std::min<std::uint64_t>(at + n, write.offset + write.bytes.size());
        if (lo < hi) std::copy(write.bytes.begin() + static_cast<std::ptrdiff_t>(lo - write.offset), write.bytes.begin() + static_cast<std::ptrdiff_t>(hi - write.offset), chunk.begin() + static_cast<std::ptrdiff_t>(lo - at));
      }
      sink(f, at, std::span<const std::uint8_t>(chunk.data(), n));
      at += n;
    }
    if (hash.finish_hex() != file.sha256) fail("E_DISC_IDENTITY", "an original disc file changed while it was being read");
  }
}

}  // namespace

bool DiscPlan::has_code() const {
  return std::any_of(spans.begin(), spans.end(), [](const DiscSpan &s) { return s.code; });
}

std::optional<Rejection> check_disc_payload(const Payload &p, bool has_code_cpu, bool has_load_address, bool has_iso_file) {
  const auto bad = [](std::string code, std::string message) { return std::optional<Rejection>(Rejection{std::move(code), std::move(message)}); };
  if (has_code_cpu) {
    if (p.code_cpu == "sub") return bad(kSub, "\"" + p.path + "\": Sega CD sub-CPU code cannot be patched");
    if (p.code_cpu == "z80") return bad(kZ80, "\"" + p.path + "\": Sega CD Z80 driver code cannot be patched");
    if (p.code_cpu != "main") return bad("E_FIELD_VALUE", "code_cpu must be main, sub or z80 (omit it for a data edit)");
  }
  if (has_code_cpu != has_load_address) return bad("E_FIELD_VALUE", "code_cpu and load_address are declared together");
  if (has_load_address && p.load_address >= 0xA00000 && p.load_address < 0xA10000) return bad(kZ80, "\"" + p.path + "\": the load address lies in the Z80 area; Z80 driver code cannot be patched");
  if (has_load_address && p.load_address >= 0x1000000) return bad("E_FIELD_VALUE", "load_address is a 24-bit main-CPU address");
  if (p.disc_target >= 99) return bad("E_FIELD_VALUE", "target is a 0-based CUE FILE index");
  if (p.has_disc_track && (p.disc_track == 0 || p.disc_track > 99)) return bad("E_FIELD_VALUE", "track is a track number 1..99");
  if (has_iso_file && !printable_path(p.iso_file)) return bad("E_FIELD_VALUE", "iso_file is an absolute ISO9660 path");
  if (p.disc_length == 0 || p.disc_length > kMaxDiscPayloadBytes) return bad("E_PAYLOAD_CAP", "\"" + p.path + "\": a disc range is 1 byte to 128 MiB");
  if (p.disc_start > kMaxDiscOffset || p.disc_start + p.disc_length > kMaxDiscOffset) return bad("E_DISC_RANGE", "\"" + p.path + "\": the range lies beyond any CD image");
  if (!hex64(p.original_sha256)) return bad("E_FIELD_VALUE", "original_sha256 is not 64 lowercase hex digits");
  if (p.replacement != "inline" && p.replacement != "bps") return bad("E_FIELD_VALUE", "replacement must be inline or bps");
  if (p.replacement == "inline" && p.size != p.disc_length)
    return bad(has_code_cpu ? kLayout : "E_DISC_PAYLOAD_SIZE", "\"" + p.path + "\": an inline replacement holds exactly `length` bytes (a disc patch never changes a size)");
  if (p.replacement == "bps" && p.disc_length > patch::kMaxImageBytes) return bad("E_PAYLOAD_CAP", "\"" + p.path + "\": a BPS replacement covers at most 16 MiB");
  return std::nullopt;
}

std::optional<Rejection> check_bios_requirement(const Manifest &m) {
  if (m.bios_requirement == "digest") {
    if (m.bios_sha256s.empty() || m.bios_sha256s.size() > kMaxBiosDigests) return Rejection{"E_SOURCE_DIGEST", "a digest bios requirement lists 1 to 8 BIOS digests"};
    for (std::size_t i = 0; i < m.bios_sha256s.size(); ++i)
      if (!hex64(m.bios_sha256s[i]) || (i > 0 && !(m.bios_sha256s[i - 1] < m.bios_sha256s[i]))) return Rejection{"E_SOURCE_DIGEST", "bios sha256s are 64 lowercase hex digits, strictly ascending"};
    return std::nullopt;
  }
  if (m.bios_requirement != "class") return Rejection{"E_FIELD_VALUE", "the bios requirement must be digest or class"};
  if (m.bios_class.empty() || m.bios_class.size() > 64 || m.bios_class.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
    return Rejection{"E_FIELD_VALUE", "a bios class is a profile token"};
  return std::nullopt;
}

std::optional<Rejection> check_disc_symbol(const Symbol &s) {
  if (s.cpu != "main" && s.cpu != "sub" && s.cpu != "z80") return Rejection{"E_FIELD_VALUE", "a disc symbol names its cpu: main, sub or z80"};
  if (s.module != "boot" && !printable_path(s.module)) return Rejection{"E_FIELD_VALUE", "a disc symbol names its module: boot, or an absolute ISO9660 path"};
  return std::nullopt;
}

std::optional<Rejection> check_disc_overlap(const Manifest &m) {
  std::vector<const Payload *> edits;
  for (const Payload &p : m.payloads) if (p.kind == "disc-extent-edit") edits.push_back(&p);
  const auto key = [](const Payload *p) { return std::tie(p->disc_target, p->iso_file, p->disc_start); };
  std::sort(edits.begin(), edits.end(), [&](const Payload *a, const Payload *b) { return key(a) < key(b); });
  for (std::size_t i = 1; i < edits.size(); ++i) {
    const Payload &a = *edits[i - 1], &b = *edits[i];
    if (a.disc_target == b.disc_target && a.iso_file == b.iso_file && a.disc_start + a.disc_length > b.disc_start)
      return Rejection{"E_DISC_OVERLAP", "the disc edits \"" + a.path + "\" and \"" + b.path + "\" overlap"};
  }
  return std::nullopt;
}

DiscPlan plan_disc_patch(const Manifest &m, const std::map<std::string, std::vector<std::uint8_t>> &files, const DiscImage &disc) {
  DiscPlan plan;
  try {
    if (m.platform != "sega-cd") fail("E_SOURCE_PLATFORM", "disc edits exist only for a sega-cd package");
    if (disc.identity() != m.disc_identity) fail("E_DISC_IDENTITY", "the supplied disc is not the disc the package declares");
    const Payload *guest = nullptr;
    for (const Payload &p : m.payloads) {
      if (p.kind == "guest-asm") { guest = &p; continue; }
      if (p.kind != "disc-extent-edit") fail("E_PAYLOAD_KIND_RESERVED", "the payload kind \"" + p.kind + "\" is not implemented on sega-cd");
      const auto it = files.find(p.path);
      if (it == files.end()) fail("E_ARCHIVE_ENTRY", "a disc payload is missing");
      apply_edit(p, it->second, disc, plan);
    }
    if (guest != nullptr) inject(m, files, *guest, disc, plan);
    if (plan.writes.empty()) fail("E_FIELD_MISSING", "the package has no disc-extent-edit or guest-asm payload");
    std::sort(plan.writes.begin(), plan.writes.end(), [](const DiscWrite &a, const DiscWrite &b) { return std::tie(a.file, a.offset) < std::tie(b.file, b.offset); });
    for (std::size_t i = 1; i < plan.writes.size(); ++i)
      if (plan.writes[i - 1].file == plan.writes[i].file && plan.writes[i - 1].offset + plan.writes[i - 1].bytes.size() > plan.writes[i].offset)
        fail("E_DISC_OVERLAP", "two disc writes cover the same bytes of a file");
    plan.ok = true;
  } catch (const Fail &f) {
    plan = DiscPlan{};
    plan.rejection = {f.code, f.message};
  }
  return plan;
}

DiscOpened open_disc(const fs::path &cue) {
  DiscOpened out;
  DiscLoadResult loaded = load_cue_disc(cue);
  if (loaded.disc) {
    out.disc = std::move(loaded.disc);
    return out;
  }
  bool later_index = false;
  if (loaded.error == DiscError::cue_unsupported) {
    std::error_code ec;
    if (fs::is_regular_file(cue, ec) && fs::file_size(cue, ec) <= kCueMaxBytes) {
      std::ifstream in(cue, std::ios::binary);
      for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string op, number;
        if (words >> op >> number && op == "INDEX" && !number.empty() && number.find_first_not_of("0123456789") == std::string::npos && number.find_first_not_of('0') != std::string::npos &&
            number != "1" && number != "01")
          later_index = true;
      }
    }
  }
  out.rejection = later_index ? Rejection{"E_DISC_INDEX_UNSUPPORTED", "INDEX 02 and later are not supported"}
                              : Rejection{"E_DISC_LOAD", std::string("the disc cannot be loaded: ") + disc_error_name(loaded.error) + (loaded.detail.empty() ? "" : " (" + loaded.detail + ")")};
  return out;
}

std::optional<Rejection> check_disc_layout(const DiscImage &a, const DiscImage &b) {
  const auto bad = [](const char *code, std::string message) { return std::optional<Rejection>(Rejection{code, std::move(message)}); };
  if (a.files().size() != b.files().size()) return bad("E_DISC_FILE_SIZE", "the number of CUE FILEs differs");
  for (std::size_t i = 0; i < a.files().size(); ++i) {
    if (a.files()[i].name != b.files()[i].name) return bad("E_DISC_FILE_SIZE", "a CUE FILE is renamed or reordered");
    if (a.files()[i].size != b.files()[i].size) return bad("E_DISC_FILE_SIZE", "a file size differs");
  }
  if (a.tracks().size() != b.tracks().size()) return bad("E_DISC_TRACK_COUNT", "the number of tracks differs");
  const auto shapes = [](const DiscImage &d) {
    std::vector<std::tuple<std::uint32_t, int, std::uint32_t>> v;
    for (const DiscTrack &t : d.tracks()) v.emplace_back(t.file_index, static_cast<int>(t.type), t.sector_count);
    return v;
  };
  auto left = shapes(a), right = shapes(b);
  if (left != right) {
    auto sorted_left = left, sorted_right = right;
    std::sort(sorted_left.begin(), sorted_left.end());
    std::sort(sorted_right.begin(), sorted_right.end());
    if (sorted_left == sorted_right) return bad("E_DISC_TRACK_ORDER", "the tracks are reordered");
  }
  for (std::size_t i = 0; i < a.tracks().size(); ++i) {   // pregaps first: moving an INDEX 00 also changes the length of the track before it
    const DiscTrack &x = a.tracks()[i], &y = b.tracks()[i];
    if (x.number == y.number && (x.pregap != y.pregap || x.index0_sectors != y.index0_sectors)) return bad("E_DISC_PREGAP", "track " + std::to_string(x.number) + " changed its pregap or INDEX 00 data");
  }
  for (std::size_t i = 0; i < a.tracks().size(); ++i) {
    const DiscTrack &x = a.tracks()[i], &y = b.tracks()[i];
    const std::string at = "track " + std::to_string(x.number);
    if (x.number != y.number || x.file_index != y.file_index) return bad("E_DISC_TRACK_ORDER", at + " moved");
    if (x.type != y.type || x.sector_size != y.sector_size) return bad("E_DISC_SECTOR_SIZE", at + " changed its mode or sector size");
    if (x.sector_count != y.sector_count || x.file_offset != y.file_offset || x.first_lba != y.first_lba) return bad("E_DISC_TRACK_LENGTH", at + " changed its length or position");
  }
  if (a.total_sectors() != b.total_sectors()) return bad("E_DISC_TRACK_LENGTH", "the total sector count differs");
  return std::nullopt;
}

std::optional<Rejection> verify_patched_disc(const DiscPlan &plan, const DiscImage &original, const DiscImage &effective) {
  if (const auto r = check_disc_layout(original, effective)) return r;
  try {
    std::ifstream in;
    std::uint32_t open = 0xFFFFFFFFU;
    std::vector<std::uint8_t> got(kChunk);
    stream_effective(plan, original, [&](std::uint32_t f, std::uint64_t, std::span<const std::uint8_t> expected) {
      if (open != f) {
        in.close();
        in.clear();
        in.open(effective.files()[f].path, std::ios::binary);
        open = f;
      }
      if (!in.read(reinterpret_cast<char *>(got.data()), static_cast<std::streamsize>(expected.size()))) fail("E_DISC_LOAD", "a patched disc file cannot be read");
      if (!std::equal(expected.begin(), expected.end(), got.begin())) fail("E_DISC_UNDECLARED_CHANGE", "the patched disc differs from the original outside the declared ranges, or inside them from the declared bytes");
    });
  } catch (const Fail &f) {
    return Rejection{f.code, f.message};
  }
  return std::nullopt;
}

DiscMaterialized materialize_disc(const DiscPlan &plan, const DiscImage &original, const fs::path &original_cue, const fs::path &out_dir) {
  DiscMaterialized result;
  fs::path partial;
  try {
    if (!plan.ok) fail(plan.rejection.code, plan.rejection.message);
    std::error_code ec;
    const fs::path out = fs::absolute(out_dir, ec).lexically_normal();
    if (ec || out.filename().empty()) fail("E_DISC_OUTPUT", "the output folder is not usable");
    const fs::path source = fs::absolute(original_cue, ec).parent_path();
    if (inside(fs::weakly_canonical(out, ec), source)) fail("E_DISC_OUTPUT", "the output folder is the original disc's folder or lies inside it; originals are never written");
    const bool exists = fs::exists(fs::symlink_status(out, ec));
    if (exists && (!fs::is_directory(fs::symlink_status(out, ec)) || !fs::is_empty(out, ec))) fail("E_DISC_OUTPUT", "the output folder exists and is not empty");
    fs::create_directories(out.parent_path(), ec);
    std::random_device random;
    partial = out.parent_path() / (out.filename().string() + ".partial-" + std::to_string(random()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(partial, ec) || ec) fail("E_DISC_OUTPUT", "the output folder cannot be created");
    std::ofstream file;
    std::uint32_t open = 0xFFFFFFFFU;
    const auto close = [&] {
      if (!file.is_open()) return;
      file.close();
      if (!file) fail("E_DISC_OUTPUT", "a patched disc file cannot be written");
    };
    stream_effective(plan, original, [&](std::uint32_t f, std::uint64_t, std::span<const std::uint8_t> bytes) {
      if (open != f) {
        close();
        file.open(partial / original.files()[f].name, std::ios::binary | std::ios::trunc);
        open = f;
      }
      if (!file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) fail("E_DISC_OUTPUT", "a patched disc file cannot be written");
    });
    close();
    const fs::path cue_name = original_cue.filename();
    if (!fs::copy_file(original_cue, partial / cue_name, ec) || ec) fail("E_DISC_OUTPUT", "the CUE cannot be written");   // never overwrites: a FILE of that name is refused
    const DiscOpened effective = open_disc(partial / cue_name);
    if (!effective.disc) fail(effective.rejection.code, effective.rejection.message);
    if (const auto r = check_disc_layout(original, *effective.disc)) fail(r->code, r->message);
    result.effective_identity = effective.disc->identity();
    if (exists) fs::remove(out, ec);
    fs::rename(partial, out, ec);
    if (ec) fail("E_DISC_OUTPUT", "the patched disc cannot be published");
    result.cue = out / cue_name;
    result.ok = true;
  } catch (const Fail &f) {
    result = DiscMaterialized{};
    result.rejection = {f.code, f.message};
  }
  if (!result.ok && !partial.empty()) {
    std::error_code ec;
    fs::remove_all(partial, ec);
  }
  return result;
}

std::optional<Rejection> check_disc_eligibility(const DiscPlan &plan, const DiscImage &disc, const DiscEvidence &evidence) {
  const auto overlaps = [](const DiscProof &p, const DiscSpan &s) { return p.offset < s.offset + s.length && s.offset < p.offset + p.length; };
  for (const DiscSpan &s : plan.spans) {
    for (const DiscProof &p : evidence.proofs)
      if (p.cpu == "sub" && overlaps(p, s)) return Rejection{kSub, s.origin + " writes bytes the build proved to be sub-CPU code; sub-CPU code cannot be patched"};
    if (!s.code) {
      for (const DiscProof &p : evidence.proofs)
        if (p.cpu == "main" && overlaps(p, s)) return Rejection{kUndeclared, s.origin + " writes bytes the build proved to be main-CPU code; declare it as a code patch (code_cpu and load_address)"};
      continue;
    }
    const bool proven = std::any_of(evidence.proofs.begin(), evidence.proofs.end(), [&](const DiscProof &p) {
      return p.cpu == "main" && p.origin != "boot_contradicted" && s.offset >= p.offset && s.offset + s.length <= p.offset + p.length && p.base + (s.offset - p.offset) == s.address;
    });
    if (proven) continue;
    for (const DiscDynamic &d : evidence.dynamic)
      if (d.cpu == "main" && s.address < d.base + d.length && d.base < s.address + s.length)
        return Rejection{kDynamic, s.origin + " targets code that is made or observed in RAM at run time, not copied from the disc; it cannot be patched"};
    return Rejection{kNotProven, s.origin + ": the build did not prove that these disc bytes are the main-CPU code that runs at the declared address"};
  }
  for (const Symbol &y : plan.symbol_uses) {
    std::uint64_t lo = 0, hi = kDiscBootAreaBytes;
    if (y.module != "boot") {
      try {
        const Iso9660Extent e = iso_extent(disc, y.module, kModule);
        lo = static_cast<std::uint64_t>(e.block) * kDiscUserBytes;
        hi = lo + e.size;
      } catch (const Fail &f) {
        return Rejection{f.code, "the symbol \"" + y.name + "\": " + f.message};
      }
    }
    const bool found = std::any_of(evidence.proofs.begin(), evidence.proofs.end(), [&](const DiscProof &p) {
      if (p.cpu != "main" || p.origin == "boot_contradicted" || y.address < p.base || y.address >= p.base + p.length) return false;
      const std::uint64_t at = p.offset + (y.address - p.base);
      return at >= lo && at < hi;
    });
    if (!found) return Rejection{kModule, "the symbol \"" + y.name + "\" names the module \"" + y.module + "\", and the build proved no main-CPU code of that module at its address"};
  }
  return std::nullopt;
}

}  // namespace segarecomp::mod
