#include "segarecomp/iso9660.hpp"

#include <array>
#include <cstring>

namespace segarecomp {

const char *iso9660_error_name(Iso9660Error error) noexcept {
  switch (error) {
  case Iso9660Error::none: return "none";
  case Iso9660Error::read_failed: return "read_failed";
  case Iso9660Error::no_primary_descriptor: return "no_primary_descriptor";
  case Iso9660Error::descriptor_invalid: return "descriptor_invalid";
  case Iso9660Error::duplicate_primary: return "duplicate_primary";
  case Iso9660Error::both_endian_mismatch: return "both_endian_mismatch";
  case Iso9660Error::unsupported_block_size: return "unsupported_block_size";
  case Iso9660Error::volume_space_invalid: return "volume_space_invalid";
  case Iso9660Error::unsupported_volume_set: return "unsupported_volume_set";
  case Iso9660Error::record_invalid: return "record_invalid";
  case Iso9660Error::record_crosses_sector: return "record_crosses_sector";
  case Iso9660Error::unsupported_feature: return "unsupported_feature";
  case Iso9660Error::directory_invalid: return "directory_invalid";
  case Iso9660Error::extent_out_of_range: return "extent_out_of_range";
  case Iso9660Error::path_invalid: return "path_invalid";
  case Iso9660Error::path_not_found: return "path_not_found";
  case Iso9660Error::path_ambiguous: return "path_ambiguous";
  case Iso9660Error::not_a_file: return "not_a_file";
  case Iso9660Error::not_a_directory: return "not_a_directory";
  }
  return "unknown";
}

namespace {

using Block = std::array<std::uint8_t, kIso9660BlockBytes>;

std::uint32_t le32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint32_t be32(const std::uint8_t *p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}
std::uint32_t le16(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t be16(const std::uint8_t *p) { return (static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]); }

// Both-endian fields (ECMA-119 7.3.3 / 7.2.3): the two halves must agree.
bool both32(const std::uint8_t *p, std::uint32_t &out) {
  out = le32(p);
  return out == be32(p + 4);
}
bool both16(const std::uint8_t *p, std::uint32_t &out) {
  out = le16(p);
  return out == be16(p + 2);
}

std::uint64_t blocks_of(std::uint32_t size) { return (static_cast<std::uint64_t>(size) + kIso9660BlockBytes - 1U) / kIso9660BlockBytes; }

bool extent_fits(const Iso9660Extent &extent, std::uint32_t volume_space) {
  return static_cast<std::uint64_t>(extent.block) + blocks_of(extent.size) <= volume_space && extent.block < volume_space;
}

struct Record {
  Iso9660Entry entry;
  std::uint32_t name_length{};
  std::uint8_t first_name_byte{};
};

// One directory record at `p` with `available` bytes left in its sector.
Iso9660Error parse_record(const std::uint8_t *p, std::uint32_t available, Record &out) {
  const std::uint32_t length = p[0];
  if (length < 34U) return Iso9660Error::record_invalid;
  if (length > available) return Iso9660Error::record_crosses_sector;
  const std::uint32_t name_length = p[32];
  if (name_length == 0U || 33U + name_length > length) return Iso9660Error::record_invalid;
  if (p[1] != 0U) return Iso9660Error::unsupported_feature;              // extended attribute record
  if (p[26] != 0U || p[27] != 0U) return Iso9660Error::unsupported_feature;  // interleaved file
  const std::uint8_t flags = p[25];
  if ((flags & 0x80U) != 0U || (flags & 0x04U) != 0U) return Iso9660Error::unsupported_feature;  // multi-extent, associated
  std::uint32_t sequence = 0;
  if (!both32(p + 2, out.entry.extent.block) || !both32(p + 10, out.entry.extent.size) || !both16(p + 28, sequence))
    return Iso9660Error::both_endian_mismatch;
  if (sequence != 1U) return Iso9660Error::unsupported_volume_set;
  out.entry.directory = (flags & 0x02U) != 0U;
  out.entry.identifier.assign(reinterpret_cast<const char *>(p + 33), name_length);
  out.name_length = name_length;
  out.first_name_byte = p[33];
  return Iso9660Error::none;
}

Iso9660Error check_directory_extent(const Iso9660Extent &extent, std::uint32_t volume_space) {
  if (extent.size == 0U || extent.size % kIso9660BlockBytes != 0U || extent.size > kIso9660MaxDirectoryBytes)
    return Iso9660Error::directory_invalid;
  if (!extent_fits(extent, volume_space)) return Iso9660Error::extent_out_of_range;
  return Iso9660Error::none;
}

}  // namespace

Iso9660VolumeResult read_iso9660_volume(const Iso9660BlockReader &read, std::uint32_t track_blocks) {
  Iso9660VolumeResult result;
  const auto fail = [&](Iso9660Error error) {
    result.error = error;
    result.volume.reset();
    return result;
  };
  Block block{};
  std::optional<Iso9660Volume> primary;
  bool terminated = false;
  std::uint32_t index = 0;
  for (; index < kIso9660MaxDescriptors; ++index) {
    if (!read(kIso9660FirstDescriptor + index, block)) return fail(Iso9660Error::read_failed);
    if (std::memcmp(block.data() + 1, "CD001", 5) != 0 || block[6] != 1U) return fail(Iso9660Error::descriptor_invalid);
    const std::uint8_t type = block[0];
    if (type == 255U) {
      terminated = true;
      break;
    }
    if (type == 0U || type == 2U || type == 3U) continue;  // boot record, supplementary (Joliet) or partition: not used
    if (type != 1U) return fail(Iso9660Error::descriptor_invalid);
    if (primary) return fail(Iso9660Error::duplicate_primary);
    if (block[881] != 1U) return fail(Iso9660Error::descriptor_invalid);  // file structure version
    Iso9660Volume volume;
    std::uint32_t set_size = 0, sequence = 0, block_size = 0;
    if (!both32(block.data() + 80, volume.volume_space) || !both16(block.data() + 120, set_size) ||
        !both16(block.data() + 124, sequence) || !both16(block.data() + 128, block_size))
      return fail(Iso9660Error::both_endian_mismatch);
    if (block_size != kIso9660BlockBytes) return fail(Iso9660Error::unsupported_block_size);
    if (set_size != 1U || sequence != 1U) return fail(Iso9660Error::unsupported_volume_set);
    Record root;
    if (const auto error = parse_record(block.data() + 156, 34U, root); error != Iso9660Error::none) return fail(error);
    if (block[156] != 34U || root.name_length != 1U || root.first_name_byte != 0U || !root.entry.directory)
      return fail(Iso9660Error::record_invalid);
    volume.root = root.entry.extent;
    std::string id(reinterpret_cast<const char *>(block.data() + 40), 32);
    while (!id.empty() && (id.back() == ' ' || id.back() == '\0')) id.pop_back();
    volume.volume_id = id;
    primary = volume;
  }
  if (!terminated) return fail(Iso9660Error::descriptor_invalid);
  if (!primary) return fail(Iso9660Error::no_primary_descriptor);
  if (primary->volume_space > track_blocks || primary->volume_space <= kIso9660FirstDescriptor + index)
    return fail(Iso9660Error::volume_space_invalid);
  if (const auto error = check_directory_extent(primary->root, primary->volume_space); error != Iso9660Error::none) return fail(error);
  result.volume = primary;
  return result;
}

Iso9660ListResult list_iso9660_directory(const Iso9660BlockReader &read, const Iso9660Volume &volume, const Iso9660Extent &directory) {
  Iso9660ListResult result;
  const auto fail = [&](Iso9660Error error) {
    result.entries.clear();
    result.error = error;
    return result;
  };
  if (const auto error = check_directory_extent(directory, volume.volume_space); error != Iso9660Error::none) return fail(error);
  Block block{};
  std::uint32_t seen = 0;
  for (std::uint32_t b = 0; b < directory.size / kIso9660BlockBytes; ++b) {
    if (!read(directory.block + b, block)) return fail(Iso9660Error::read_failed);
    std::uint32_t at = 0;
    while (at < kIso9660BlockBytes) {
      if (block[at] == 0U) break;  // the rest of the sector is padding
      Record record;
      if (const auto error = parse_record(block.data() + at, kIso9660BlockBytes - at, record); error != Iso9660Error::none)
        return fail(error);
      at += block[at];
      if (seen < 2U) {  // "." then ".." (ECMA-119 6.8.2.2)
        if (record.name_length != 1U || record.first_name_byte != seen || !record.entry.directory)
          return fail(Iso9660Error::record_invalid);
        if (seen == 0U && (record.entry.extent.block != directory.block || record.entry.extent.size != directory.size))
          return fail(Iso9660Error::record_invalid);
        ++seen;
        continue;
      }
      if (record.name_length == 1U && record.first_name_byte <= 1U) return fail(Iso9660Error::record_invalid);
      if (record.entry.directory) {
        if (const auto error = check_directory_extent(record.entry.extent, volume.volume_space); error != Iso9660Error::none)
          return fail(error);
      } else if (!extent_fits(record.entry.extent, volume.volume_space)) {
        return fail(Iso9660Error::extent_out_of_range);
      }
      result.entries.push_back(std::move(record.entry));
    }
  }
  if (seen < 2U) return fail(Iso9660Error::record_invalid);
  return result;
}

Iso9660LookupResult lookup_iso9660_file(const Iso9660BlockReader &read, const Iso9660Volume &volume, std::string_view path) {
  Iso9660LookupResult result;
  if (path.empty() || path.front() != '/') {
    result.error = Iso9660Error::path_invalid;
    return result;
  }
  std::vector<std::string_view> components;
  std::size_t at = 1;
  while (at <= path.size()) {
    const std::size_t next = path.find('/', at);
    const std::string_view component = path.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
    if (component.empty()) {
      result.error = Iso9660Error::path_invalid;
      return result;
    }
    components.push_back(component);
    if (next == std::string_view::npos) break;
    at = next + 1;
  }
  if (components.empty() || components.size() > kIso9660MaxDepth) {
    result.error = Iso9660Error::path_invalid;
    return result;
  }
  Iso9660Extent directory = volume.root;
  for (std::size_t c = 0; c < components.size(); ++c) {
    const auto listing = list_iso9660_directory(read, volume, directory);
    if (listing.error != Iso9660Error::none) {
      result.error = listing.error;
      return result;
    }
    const Iso9660Entry *found = nullptr;
    for (const auto &entry : listing.entries) {
      if (entry.identifier != components[c]) continue;
      if (found != nullptr) {
        result.error = Iso9660Error::path_ambiguous;
        return result;
      }
      found = &entry;
    }
    if (found == nullptr) {
      result.error = Iso9660Error::path_not_found;
      return result;
    }
    const bool last = c + 1 == components.size();
    if (last && found->directory) {
      result.error = Iso9660Error::not_a_file;
      return result;
    }
    if (!last && !found->directory) {
      result.error = Iso9660Error::not_a_directory;
      return result;
    }
    if (last) result.extent = found->extent;
    else directory = found->extent;
  }
  return result;
}

Iso9660BlockReader iso9660_track_reader(const DiscImage &disc, std::size_t track_index) {
  return [&disc, track_index](std::uint32_t block, std::span<std::uint8_t, kIso9660BlockBytes> out) {
    if (track_index >= disc.tracks().size()) return false;
    const auto &track = disc.tracks()[track_index];
    if (track.type == DiscTrackType::audio || block >= track.sector_count) return false;
    return disc.read_user_data(track.first_lba + block, out) == DiscError::none;
  };
}

}  // namespace segarecomp
