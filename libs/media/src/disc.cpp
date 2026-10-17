#include "segarecomp/disc.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace segarecomp {

const char *disc_error_name(DiscError error) noexcept {
  switch (error) {
  case DiscError::none: return "none";
  case DiscError::cue_unreadable: return "cue_unreadable";
  case DiscError::cue_syntax: return "cue_syntax";
  case DiscError::cue_unsupported: return "cue_unsupported";
  case DiscError::file_missing: return "file_missing";
  case DiscError::file_ambiguous: return "file_ambiguous";
  case DiscError::path_unsafe: return "path_unsafe";
  case DiscError::file_unreadable: return "file_unreadable";
  case DiscError::file_size_not_multiple: return "file_size_not_multiple";
  case DiscError::file_truncated: return "file_truncated";
  case DiscError::mixed_sector_sizes: return "mixed_sector_sizes";
  case DiscError::track_numbering: return "track_numbering";
  case DiscError::track_layout: return "track_layout";
  case DiscError::index_order: return "index_order";
  case DiscError::pregap_conflict: return "pregap_conflict";
  case DiscError::disc_too_large: return "disc_too_large";
  case DiscError::raw_sync: return "raw_sync";
  case DiscError::raw_header: return "raw_header";
  case DiscError::header_conflict: return "header_conflict";
  case DiscError::identity_mismatch: return "identity_mismatch";
  case DiscError::lba_out_of_range: return "lba_out_of_range";
  case DiscError::lba_audio: return "lba_audio";
  case DiscError::io_error: return "io_error";
  }
  return "unknown";
}

const char *disc_track_type_name(DiscTrackType type) noexcept {
  switch (type) {
  case DiscTrackType::mode1_2048: return "mode1_2048";
  case DiscTrackType::mode1_2352: return "mode1_2352";
  case DiscTrackType::audio: return "audio";
  }
  return "unknown";
}

std::string disc_bcd_msf(std::uint32_t lba) {
  const std::uint32_t msf = lba + kDiscMsfLeadIn;
  char text[16];
  std::snprintf(text, sizeof text, "%02u:%02u:%02u", static_cast<unsigned>(msf / 75U / 60U), static_cast<unsigned>(msf / 75U % 60U),
                static_cast<unsigned>(msf % 75U));
  return text;
}

namespace {

constexpr std::uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

std::uint8_t bcd(std::uint32_t value) { return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U)); }

struct CueTrack {
  unsigned number{};
  DiscTrackType type{};
  std::optional<std::uint32_t> index00;  // in-file pregap start (file-relative frames)
  std::optional<std::uint32_t> index01;
  std::optional<std::uint32_t> pregap;   // PREGAP (not stored in the file)
  std::size_t line{};
};

struct CueFile {
  std::string name;
  std::vector<CueTrack> tracks;
};

bool tokenize(const std::string &line, std::vector<std::string> &out) {
  out.clear();
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) break;
    if (line[i] == '"') {
      const auto end = line.find('"', i + 1);
      if (end == std::string::npos) return false;
      out.push_back(line.substr(i + 1, end - i - 1));
      i = end + 1;
      if (i < line.size() && line[i] != ' ' && line[i] != '\t') return false;
    } else {
      const auto start = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
        if (line[i] == '"') return false;
        ++i;
      }
      out.push_back(line.substr(start, i - start));
    }
  }
  return true;
}

std::optional<std::uint32_t> parse_digits(const std::string &text, std::size_t max_digits) {
  if (text.empty() || text.size() > max_digits) return std::nullopt;
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10U + static_cast<std::uint32_t>(c - '0');
  }
  return value;
}

// "MM:SS:FF" -> frames; seconds < 60, frames < 75.
std::optional<std::uint32_t> parse_time(const std::string &text) {
  if (text.size() != 8 || text[2] != ':' || text[5] != ':') return std::nullopt;
  const auto m = parse_digits(text.substr(0, 2), 2), s = parse_digits(text.substr(3, 2), 2), f = parse_digits(text.substr(6, 2), 2);
  if (!m || !s || !f || *s >= 60U || *f >= 75U) return std::nullopt;
  return (*m * 60U + *s) * 75U + *f;
}

bool plain_file_name(const std::string &name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255U) return false;
  for (const char c : name)
    if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20U || c == 0x7F) return false;
  return true;
}

}  // namespace

struct DiscLoader {
  static DiscLoadResult fail(DiscError error, std::string detail) { return DiscLoadResult{std::nullopt, error, std::move(detail)}; }

  static DiscLoadResult load(const std::filesystem::path &cue, std::optional<std::string_view> expected) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cue, ec) || std::filesystem::is_symlink(cue, ec)) return fail(DiscError::cue_unreadable, "cue");
    const auto cue_size = std::filesystem::file_size(cue, ec);
    if (ec || cue_size > kCueMaxBytes) return fail(DiscError::cue_unreadable, "cue size");
    std::ifstream in(cue, std::ios::binary);
    if (!in) return fail(DiscError::cue_unreadable, "cue open");
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    const auto directory = std::filesystem::canonical(std::filesystem::absolute(cue).parent_path(), ec);
    if (ec) return fail(DiscError::cue_unreadable, "cue directory");

    // --- parse -------------------------------------------------------------------------------------------------------------------
    std::vector<CueFile> files;
    std::vector<std::string> tokens;
    std::size_t line_number = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      ++line_number;
      const std::string at = "line " + std::to_string(line_number);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line_number == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
          static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
        line.erase(0, 3);
      if (!tokenize(line, tokens)) return fail(DiscError::cue_syntax, at);
      if (tokens.empty()) continue;
      std::string keyword = tokens[0];
      std::transform(keyword.begin(), keyword.end(), keyword.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      if (keyword == "REM" || keyword == "TITLE" || keyword == "PERFORMER" || keyword == "CATALOG" || keyword == "FLAGS") continue;
      if (keyword == "FILE") {
        if (tokens.size() != 3) return fail(DiscError::cue_syntax, at);
        if (tokens[2] != "BINARY") return fail(DiscError::cue_unsupported, at);
        if (!plain_file_name(tokens[1])) return fail(DiscError::path_unsafe, at);
        for (const auto &existing : files)
          if (existing.name == tokens[1]) return fail(DiscError::file_ambiguous, at);
        if (!files.empty() && files.back().tracks.empty()) return fail(DiscError::file_missing, at);
        files.push_back(CueFile{tokens[1], {}});
      } else if (keyword == "TRACK") {
        if (tokens.size() != 3) return fail(DiscError::cue_syntax, at);
        if (files.empty()) return fail(DiscError::file_missing, at);
        const auto number = parse_digits(tokens[1], 2);
        if (!number) return fail(DiscError::cue_syntax, at);
        CueTrack track;
        track.number = *number;
        track.line = line_number;
        if (tokens[2] == "MODE1/2048") track.type = DiscTrackType::mode1_2048;
        else if (tokens[2] == "MODE1/2352") track.type = DiscTrackType::mode1_2352;
        else if (tokens[2] == "AUDIO") track.type = DiscTrackType::audio;
        else return fail(DiscError::cue_unsupported, at);
        files.back().tracks.push_back(track);
      } else if (keyword == "INDEX") {
        if (tokens.size() != 3) return fail(DiscError::cue_syntax, at);
        if (files.empty() || files.back().tracks.empty()) return fail(DiscError::cue_syntax, at);
        const auto index = parse_digits(tokens[1], 2);
        const auto time = parse_time(tokens[2]);
        if (!index || !time) return fail(DiscError::cue_syntax, at);
        if (*index > 1U) return fail(DiscError::cue_unsupported, at);
        auto &track = files.back().tracks.back();
        if (*index == 0U) {
          // INDEX 00 is the start of an in-file pregap: it must precede the track's INDEX 01, once, and is exclusive with PREGAP.
          // Track 1 has no addressable pregap (its INDEX 01 is LBA 0), so an in-file pregap there is outside the subset.
          if (track.index00 || track.index01) return fail(DiscError::index_order, at);
          if (track.pregap) return fail(DiscError::pregap_conflict, at);
          if (track.number == 1U) return fail(DiscError::cue_unsupported, at);
          track.index00 = *time;
        } else {
          if (track.index01) return fail(DiscError::index_order, at);
          if (track.index00 && *track.index00 > *time) return fail(DiscError::index_order, at);
          track.index01 = *time;
        }
      } else if (keyword == "PREGAP") {
        if (tokens.size() != 2) return fail(DiscError::cue_syntax, at);
        if (files.empty() || files.back().tracks.empty()) return fail(DiscError::cue_syntax, at);
        auto &track = files.back().tracks.back();
        const auto time = parse_time(tokens[1]);
        if (!time) return fail(DiscError::cue_syntax, at);
        if (track.index01 || track.pregap) return fail(DiscError::cue_syntax, at);
        if (track.index00) return fail(DiscError::pregap_conflict, at);
        track.pregap = *time;
      } else {
        return fail(DiscError::cue_unsupported, at);
      }
    }
    if (files.empty() || files.back().tracks.empty()) return fail(DiscError::file_missing, "no track");

    // --- resolve files, number tracks, compute the layout ---------------------------------------------------------------------
    DiscImage disc;
    unsigned expected_number = 1;
    std::uint64_t lba = 0;
    for (std::size_t f = 0; f < files.size(); ++f) {
      const auto &cue_file = files[f];
      const auto path = directory / cue_file.name;
      if (std::filesystem::is_symlink(path, ec)) return fail(DiscError::path_unsafe, "file " + std::to_string(f));
      if (!std::filesystem::is_regular_file(path, ec)) return fail(DiscError::file_unreadable, "file " + std::to_string(f));
      const auto canonical = std::filesystem::canonical(path, ec);
      if (ec || canonical.parent_path() != directory) return fail(DiscError::path_unsafe, "file " + std::to_string(f));
      const auto size = std::filesystem::file_size(canonical, ec);
      if (ec) return fail(DiscError::file_unreadable, "file " + std::to_string(f));
      const std::uint32_t sector_size = cue_file.tracks.front().type == DiscTrackType::mode1_2048 ? kDiscUserBytes : kDiscRawBytes;
      for (const auto &track : cue_file.tracks) {
        const std::uint32_t size_of = track.type == DiscTrackType::mode1_2048 ? kDiscUserBytes : kDiscRawBytes;
        if (size_of != sector_size) return fail(DiscError::mixed_sector_sizes, "file " + std::to_string(f));
      }
      if (size == 0U || size % sector_size != 0U) return fail(DiscError::file_size_not_multiple, "file " + std::to_string(f));
      const std::uint64_t file_sectors = size / sector_size;
      for (std::size_t t = 0; t < cue_file.tracks.size(); ++t) {
        const auto &track = cue_file.tracks[t];
        const std::string at = "track " + std::to_string(track.number);
        if (track.number != expected_number || !track.index01) return fail(DiscError::track_numbering, at);
        ++expected_number;
        // The track occupies file sectors [start, end): start is its INDEX 00 when present (the in-file pregap) else its INDEX 01;
        // end is the next track's start in the same file, so a track never absorbs the following track's indexed pregap.
        const std::uint32_t start = track.index00 ? *track.index00 : *track.index01;
        if (t == 0 && start != 0U) return fail(DiscError::track_layout, at);
        if (*track.index01 >= file_sectors) return fail(DiscError::file_truncated, at);
        std::uint64_t end = file_sectors;
        if (t + 1 < cue_file.tracks.size()) {
          const auto &next = cue_file.tracks[t + 1];
          if (!next.index01) return fail(DiscError::track_numbering, "track " + std::to_string(next.number));
          const std::uint32_t next_start = next.index00 ? *next.index00 : *next.index01;
          if (next_start <= *track.index01) return fail(DiscError::track_layout, at);
          if (next_start > file_sectors || *next.index01 > file_sectors) return fail(DiscError::file_truncated, at);
          end = next_start;
        }
        DiscTrack out;
        out.number = track.number;
        out.type = track.type;
        out.file_index = static_cast<std::uint32_t>(f);
        out.sector_size = sector_size;
        out.file_offset = static_cast<std::uint64_t>(*track.index01) * sector_size;
        out.pregap = track.pregap.value_or(0U);
        out.index0_sectors = *track.index01 - start;
        lba += out.pregap;
        lba += out.index0_sectors;  // the indexed pregap is addressable disc space (its sectors are in the file, not a track's data)
        out.first_lba = static_cast<std::uint32_t>(std::min<std::uint64_t>(lba, kDiscMaxSectors + 1U));
        out.sector_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(end - *track.index01, kDiscMaxSectors + 1U));
        lba += end - *track.index01;
        if (lba > kDiscMaxSectors) return fail(DiscError::disc_too_large, at);
        disc.tracks_.push_back(out);
      }
      disc.files_.push_back(DiscFile{cue_file.name, canonical, size, {}});
    }
    disc.total_sectors_ = static_cast<std::uint32_t>(lba);

    // --- hash files and verify raw sectors --------------------------------------------------------------------------------------
    std::vector<std::uint8_t> chunk(1U << 16U);
    for (auto &file : disc.files_) {
      std::ifstream stream(file.path, std::ios::binary);
      if (!stream) return fail(DiscError::file_unreadable, "file open");
      Sha256Stream hash;
      std::uint64_t remaining = file.size;
      while (remaining != 0U) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
        if (!stream.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(count)))
          return fail(DiscError::io_error, "file read");
        hash.update(std::span<const std::uint8_t>(chunk.data(), count));
        remaining -= count;
      }
      file.sha256 = hash.finish_hex();
    }
    for (const auto &track : disc.tracks_) {
      if (track.type == DiscTrackType::audio) continue;
      std::ifstream stream(disc.files_[track.file_index].path, std::ios::binary);
      std::vector<std::uint8_t> sector(track.sector_size);
      for (std::uint32_t s = 0; s < track.sector_count; ++s) {
        stream.seekg(static_cast<std::streamoff>(track.file_offset + static_cast<std::uint64_t>(s) * track.sector_size));
        if (!stream.read(reinterpret_cast<char *>(sector.data()), static_cast<std::streamsize>(sector.size())))
          return fail(DiscError::io_error, "sector read");
        const std::string at = "track " + std::to_string(track.number) + " sector " + std::to_string(s);
        const bool synced = std::equal(std::begin(kSync), std::end(kSync), sector.begin());
        if (track.type == DiscTrackType::mode1_2048) {
          if (s == 0U && synced) return fail(DiscError::header_conflict, at);
          continue;
        }
        if (!synced) return fail(DiscError::raw_sync, at);
        const std::uint32_t msf = track.first_lba + s + kDiscMsfLeadIn;
        if (sector[12] != bcd(msf / 75U / 60U) || sector[13] != bcd(msf / 75U % 60U) || sector[14] != bcd(msf % 75U) || sector[15] != 1U)
          return fail(DiscError::raw_header, at);
      }
    }

    // --- normalized table and identity --------------------------------------------------------------------------------------------
    std::ostringstream table;
    table << "segarecomp.disc.v1\n";
    for (std::size_t f = 0; f < disc.files_.size(); ++f)
      table << "file " << f << " size " << disc.files_[f].size << " sha256 " << disc.files_[f].sha256 << " name " << disc.files_[f].name
            << "\n";
    for (const auto &track : disc.tracks_) {
      table << "track " << track.number << ' ' << disc_track_type_name(track.type) << " file " << track.file_index << " offset "
            << track.file_offset << " sector " << track.sector_size << " pregap " << track.pregap << " lba " << track.first_lba
            << " count " << track.sector_count;
      // SEG-063-T003: only a track with an in-file INDEX 00 pregap carries the field, so every INDEX 01-only disc keeps its identity.
      if (track.index0_sectors != 0U) table << " index0 " << track.index0_sectors;
      table << "\n";
    }
    table << "total " << disc.total_sectors_ << "\n";
    disc.table_ = table.str();
    disc.identity_ = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(disc.table_.data()), disc.table_.size()));
    if (expected && *expected != disc.identity_) return fail(DiscError::identity_mismatch, "identity");
    return DiscLoadResult{std::move(disc), DiscError::none, {}};
  }
};

DiscLoadResult load_cue_disc(const std::filesystem::path &cue, std::optional<std::string_view> expected_identity) {
  return DiscLoader::load(cue, expected_identity);
}

const DiscTrack *DiscImage::track_of(std::uint32_t lba) const noexcept {
  for (const auto &track : tracks_)
    if (lba >= track.first_lba && lba - track.first_lba < track.sector_count) return &track;
  return nullptr;
}

DiscError DiscImage::read_user_data(std::uint32_t lba, std::span<std::uint8_t, kDiscUserBytes> out) const {
  const DiscTrack *track = track_of(lba);
  if (track == nullptr) return DiscError::lba_out_of_range;
  if (track->type == DiscTrackType::audio) return DiscError::lba_audio;
  const std::uint64_t offset = track->file_offset + static_cast<std::uint64_t>(lba - track->first_lba) * track->sector_size +
                               (track->type == DiscTrackType::mode1_2352 ? 16U : 0U);
  std::ifstream stream(files_[track->file_index].path, std::ios::binary);
  if (!stream) return DiscError::io_error;
  stream.seekg(static_cast<std::streamoff>(offset));
  if (!stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()))) return DiscError::io_error;
  return DiscError::none;
}

// --------------------------------------------------------------------------------------------------------------------------------

const char *sega_cd_boot_error_name(SegaCdBootError error) noexcept {
  switch (error) {
  case SegaCdBootError::none: return "none";
  case SegaCdBootError::first_track_not_data: return "first_track_not_data";
  case SegaCdBootError::system_area_short: return "system_area_short";
  case SegaCdBootError::system_id: return "system_id";
  case SegaCdBootError::descriptor_zero_size: return "descriptor_zero_size";
  case SegaCdBootError::descriptor_odd: return "descriptor_odd";
  case SegaCdBootError::descriptor_outside: return "descriptor_outside";
  case SegaCdBootError::descriptor_overlap: return "descriptor_overlap";
  case SegaCdBootError::entry_outside: return "entry_outside";
  }
  return "unknown";
}

namespace {
std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | bytes[offset + 3];
}
std::string printable(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t length) {
  std::string out;
  for (std::size_t i = 0; i < length; ++i) {
    const auto c = bytes[offset + i];
    out.push_back(c >= 0x20U && c < 0x7FU ? static_cast<char>(c) : '.');
  }
  return out;
}
SegaCdBootDescriptor descriptor(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return SegaCdBootDescriptor{be32(bytes, offset), be32(bytes, offset + 4), be32(bytes, offset + 8), be32(bytes, offset + 12)};
}
SegaCdBootError check_descriptor(const SegaCdBootDescriptor &d) {
  if (d.load_size == 0U) return SegaCdBootError::descriptor_zero_size;
  if ((d.disc_address | d.load_size | d.entry_offset) & 1U) return SegaCdBootError::descriptor_odd;
  if (d.disc_address < kSegaCdHeaderEnd || d.disc_address >= kSegaCdSystemAreaBytes ||
      d.load_size > kSegaCdSystemAreaBytes - d.disc_address)
    return SegaCdBootError::descriptor_outside;
  if (d.entry_offset >= d.load_size) return SegaCdBootError::entry_outside;
  return SegaCdBootError::none;
}
}  // namespace

SegaCdBootResult parse_sega_cd_system_area(std::span<const std::uint8_t> area) {
  if (area.size() < kSegaCdSystemAreaBytes) return {std::nullopt, SegaCdBootError::system_area_short};
  static constexpr char kId[] = "SEGADISCSYSTEM  ";
  if (!std::equal(kId, kId + 16, area.begin(), [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }))
    return {std::nullopt, SegaCdBootError::system_id};
  SegaCdBootHeader header;
  header.system_id = printable(area, 0x000, 16);
  header.volume_name = printable(area, 0x010, 11);
  header.system_name = printable(area, 0x020, 11);
  header.ip = descriptor(area, 0x030);
  header.sp = descriptor(area, 0x040);
  header.hardware_id = printable(area, 0x100, 16);
  header.product_code = printable(area, 0x180, 14);
  for (std::size_t i = 0x1F0; i < 0x1F3; ++i)
    if (area[i] > 0x20U && area[i] < 0x7FU) header.region.push_back(static_cast<char>(area[i]));
  if (const auto e = check_descriptor(header.ip); e != SegaCdBootError::none) return {std::nullopt, e};
  if (const auto e = check_descriptor(header.sp); e != SegaCdBootError::none) return {std::nullopt, e};
  if (header.ip.disc_address < header.sp.disc_address + header.sp.load_size &&
      header.sp.disc_address < header.ip.disc_address + header.ip.load_size)
    return {std::nullopt, SegaCdBootError::descriptor_overlap};
  return {header, SegaCdBootError::none};
}

SegaCdBootResult read_sega_cd_boot(const DiscImage &disc, std::vector<std::uint8_t> *system_area_out) {
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio)
    return {std::nullopt, SegaCdBootError::first_track_not_data};
  const auto &first = disc.tracks().front();
  if (first.sector_count < 16U) return {std::nullopt, SegaCdBootError::system_area_short};
  std::vector<std::uint8_t> area(kSegaCdSystemAreaBytes);
  for (std::uint32_t s = 0; s < 16U; ++s) {
    if (disc.read_user_data(first.first_lba + s, std::span<std::uint8_t, kDiscUserBytes>(area.data() + s * kDiscUserBytes, kDiscUserBytes)) !=
        DiscError::none)
      return {std::nullopt, SegaCdBootError::system_area_short};
  }
  auto result = parse_sega_cd_system_area(area);
  if (system_area_out != nullptr) *system_area_out = std::move(area);
  return result;
}

}  // namespace segarecomp
