#include "segarecomp/machine/sega_cd/modules.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

const char *module_error_name(ModuleError error) noexcept {
  switch (error) {
  case ModuleError::none: return "none";
  case ModuleError::manifest_unreadable: return "manifest_unreadable";
  case ModuleError::manifest_syntax: return "manifest_syntax";
  case ModuleError::manifest_version: return "manifest_version";
  case ModuleError::disc_identity_mismatch: return "disc_identity_mismatch";
  case ModuleError::name_invalid: return "name_invalid";
  case ModuleError::duplicate_name: return "duplicate_name";
  case ModuleError::processor_invalid: return "processor_invalid";
  case ModuleError::source_missing: return "source_missing";
  case ModuleError::iso9660: return "iso9660";
  case ModuleError::extent_mismatch: return "extent_mismatch";
  case ModuleError::extent_outside_track: return "extent_outside_track";
  case ModuleError::size_invalid: return "size_invalid";
  case ModuleError::digest_mismatch: return "digest_mismatch";
  case ModuleError::mapping_missing: return "mapping_missing";
  case ModuleError::mapping_invalid: return "mapping_invalid";
  case ModuleError::mapping_outside_executable_ram: return "mapping_outside_executable_ram";
  case ModuleError::entry_missing: return "entry_missing";
  case ModuleError::entry_invalid: return "entry_invalid";
  case ModuleError::range_invalid: return "range_invalid";
  case ModuleError::verification_unsupported: return "verification_unsupported";
  case ModuleError::duplicate_module: return "duplicate_module";
  case ModuleError::too_many_modules: return "too_many_modules";
  case ModuleError::image_validation: return "image_validation";
  }
  return "unknown";
}

namespace {

bool parse_number(std::string_view text, std::uint32_t &out) {
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty()) return false;
  const auto *end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, out, base);
  return ec == std::errc{} && ptr == end;
}

bool is_hex64(std::string_view text) {
  return text.size() == 64U && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= 32U &&
         std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::vector<std::string_view> tokens_of(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    if (at >= line.size()) break;
    std::size_t end = at;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    out.push_back(line.substr(at, end - at));
    at = end;
  }
  return out;
}

// The executable RAM regions of each processor's bus view (contract sections 3, 4): a module mapping must lie inside one.
struct Region {
  std::uint32_t begin, end;
};
constexpr std::array<Region, 3> kMainRegions{{{0x00020000U, 0x00040000U}, {0x00200000U, 0x00240000U}, {0x00FF0000U, 0x01000000U}}};
constexpr std::array<Region, 3> kSubRegions{{{0x00000000U, 0x00080000U}, {0x00080000U, 0x000C0000U}, {0x000C0000U, 0x000E0000U}}};

bool inside_executable_ram(Processor processor, std::uint32_t base, std::uint32_t size) {
  const std::uint64_t end = static_cast<std::uint64_t>(base) + size;
  const auto check = [&](const auto &regions) {
    return std::any_of(regions.begin(), regions.end(), [&](const Region &r) { return base >= r.begin && end <= r.end; });
  };
  return processor == Processor::main ? check(kMainRegions) : check(kSubRegions);
}

std::string hex32(std::uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

}  // namespace

ModuleManifestResult parse_module_manifest(std::string_view text) {
  ModuleManifestResult result;
  ModuleManifest manifest;
  const auto fail = [&](ModuleError error, unsigned line) {
    result.error = error;
    result.line = line;
    result.manifest.reset();
    return result;
  };
  bool have_version = false, have_disc = false, in_module = false;
  std::array<bool, 6> seen{};  // processor, file, extent, sha256, verify, range
  ModuleSpec current;
  unsigned number = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t next = text.find('\n', at);
    std::string_view line = text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
    at = next == std::string_view::npos ? text.size() + 1 : next + 1;
    ++number;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    const auto t = tokens_of(line);
    if (t.empty()) continue;
    if (!have_version) {
      if (t.size() != 1 || t[0] != kModuleManifestVersion) return fail(ModuleError::manifest_version, number);
      have_version = true;
      continue;
    }
    const std::string_view key = t[0];
    if (!in_module) {
      if (key == "disc" && t.size() == 2 && !have_disc) {
        if (!is_hex64(t[1])) return fail(ModuleError::manifest_syntax, number);
        manifest.disc_identity = std::string(t[1]);
        have_disc = true;
      } else if (key == "module" && t.size() == 2) {
        if (!have_disc) return fail(ModuleError::manifest_syntax, number);
        if (!valid_name(t[1])) return fail(ModuleError::name_invalid, number);
        for (const auto &m : manifest.modules)
          if (m.name == t[1]) return fail(ModuleError::duplicate_name, number);
        if (manifest.modules.size() >= kModuleMaxCount) return fail(ModuleError::too_many_modules, number);
        current = ModuleSpec{};
        current.name = std::string(t[1]);
        current.line = number;
        seen.fill(false);
        in_module = true;
      } else {
        return fail(ModuleError::manifest_syntax, number);
      }
      continue;
    }
    const auto once = [&](std::size_t slot) {
      if (seen[slot]) return false;
      seen[slot] = true;
      return true;
    };
    if (key == "processor" && t.size() == 2 && once(0)) {
      if (t[1] == "main") current.processor = Processor::main;
      else if (t[1] == "sub") current.processor = Processor::sub;
      else return fail(ModuleError::processor_invalid, number);
    } else if (key == "file" && t.size() == 2 && once(1)) {
      current.path = std::string(t[1]);
    } else if (key == "extent" && t.size() == 3 && once(2)) {
      Iso9660Extent extent;
      if (!parse_number(t[1], extent.block) || !parse_number(t[2], extent.size)) return fail(ModuleError::manifest_syntax, number);
      current.extent = extent;
    } else if (key == "sha256" && t.size() == 2 && once(3)) {
      if (!is_hex64(t[1])) return fail(ModuleError::manifest_syntax, number);
      current.sha256 = std::string(t[1]);
    } else if (key == "map" && t.size() == 2) {
      std::uint32_t base = 0;
      if (!parse_number(t[1], base)) return fail(ModuleError::manifest_syntax, number);
      current.bases.push_back(base);
    } else if (key == "entry" && t.size() == 2) {
      std::uint32_t offset = 0;
      if (!parse_number(t[1], offset)) return fail(ModuleError::manifest_syntax, number);
      current.entries.push_back(offset);
    } else if (key == "range" && t.size() == 3 && once(5)) {
      std::uint32_t first = 0, length = 0;
      if (!parse_number(t[1], first) || !parse_number(t[2], length)) return fail(ModuleError::manifest_syntax, number);
      current.range = std::make_pair(first, length);
    } else if (key == "verify" && t.size() == 2 && once(4)) {
      current.verification = std::string(t[1]);
    } else if (key == "end" && t.size() == 1) {
      if (!seen[0] || !seen[3] || !seen[4]) return fail(ModuleError::manifest_syntax, number);
      manifest.modules.push_back(std::move(current));
      in_module = false;
    } else {
      return fail(ModuleError::manifest_syntax, number);
    }
  }
  if (!have_version) return fail(ModuleError::manifest_version, number);
  if (!have_disc || in_module) return fail(ModuleError::manifest_syntax, number);
  result.manifest = std::move(manifest);
  return result;
}

ModuleAdmission admit_modules(const DiscImage &disc, const ModuleManifest &manifest, BootImages &images) {
  ModuleAdmission result;
  const auto fail = [&](ModuleError error, const std::string &module) {
    result.error = error;
    result.module = module;
    result.modules.clear();
    return result;
  };
  if (manifest.disc_identity != disc.identity()) return fail(ModuleError::disc_identity_mismatch, "");
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return fail(ModuleError::extent_outside_track, "");
  const auto &track = disc.tracks().front();
  const auto read = iso9660_track_reader(disc, 0);
  std::optional<Iso9660Volume> volume;
  BootImages staged = images;
  std::ostringstream canonical;
  canonical << kModuleManifestVersion << "\ndisc " << manifest.disc_identity << "\n";
  for (const auto &spec : manifest.modules) {
    if (!spec.path && !spec.extent) return fail(ModuleError::source_missing, spec.name);
    Iso9660Extent extent{};
    if (spec.path) {
      if (!volume) {
        const auto opened = read_iso9660_volume(read, track.sector_count);
        if (!opened.volume) {
          result.iso_error = opened.error;
          return fail(ModuleError::iso9660, spec.name);
        }
        volume = opened.volume;
      }
      const auto found = lookup_iso9660_file(read, *volume, *spec.path);
      if (!found.extent) {
        result.iso_error = found.error;
        return fail(ModuleError::iso9660, spec.name);
      }
      extent = *found.extent;
      if (spec.extent && (spec.extent->block != extent.block || spec.extent->size != extent.size))
        return fail(ModuleError::extent_mismatch, spec.name);
    } else {
      extent = *spec.extent;
    }
    if (extent.size == 0U || extent.size > kModuleMaxBytes || (extent.size & 1U) != 0U) return fail(ModuleError::size_invalid, spec.name);
    const std::uint64_t blocks = (static_cast<std::uint64_t>(extent.size) + kDiscUserBytes - 1U) / kDiscUserBytes;
    if (static_cast<std::uint64_t>(extent.block) + blocks > track.sector_count || extent.block < kIso9660FirstDescriptor)
      return fail(ModuleError::extent_outside_track, spec.name);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(blocks) * kDiscUserBytes);
    for (std::uint64_t b = 0; b < blocks; ++b) {
      const auto block = std::span<std::uint8_t, kDiscUserBytes>(bytes.data() + b * kDiscUserBytes, kDiscUserBytes);
      if (disc.read_user_data(track.first_lba + extent.block + static_cast<std::uint32_t>(b), block) != DiscError::none)
        return fail(ModuleError::extent_outside_track, spec.name);
    }
    bytes.resize(extent.size);
    const std::string digest = sha256_hex(bytes);
    if (digest != spec.sha256) return fail(ModuleError::digest_mismatch, spec.name);
    // The executable subrange (SEG-097-T001): by default the whole extent; a `range` narrows what may execute (the digest still covers the whole extent).
    std::uint32_t first_offset = 0U, range_length = extent.size;
    if (spec.range) {
      first_offset = spec.range->first;
      range_length = spec.range->second;
      if ((first_offset & 1U) != 0U || (range_length & 1U) != 0U || range_length == 0U ||
          static_cast<std::uint64_t>(first_offset) + range_length > extent.size)
        return fail(ModuleError::range_invalid, spec.name);
    }
    if (spec.bases.empty()) return fail(ModuleError::mapping_missing, spec.name);
    for (const auto base : spec.bases) {
      if ((base & 1U) != 0U) return fail(ModuleError::mapping_invalid, spec.name);
      if (!inside_executable_ram(spec.processor, base + first_offset, range_length))
        return fail(ModuleError::mapping_outside_executable_ram, spec.name);
    }
    if (spec.entries.empty()) return fail(ModuleError::entry_missing, spec.name);
    for (const auto entry : spec.entries)
      if ((entry & 1U) != 0U || entry < first_offset || entry >= first_offset + range_length) return fail(ModuleError::entry_invalid, spec.name);
    if (spec.verification != "byte_identity") return fail(ModuleError::verification_unsupported, spec.name);
    auto &processor = spec.processor == Processor::main ? staged.main : staged.sub;
    for (const auto &other : result.modules) {
      if (other.processor != spec.processor || other.sha256 != digest) continue;
      for (const auto a : other.bases)
        for (const auto b : spec.bases)
          if (a + other.first_offset < b + first_offset + range_length && b + first_offset < a + other.first_offset + other.length)
            return fail(ModuleError::duplicate_module, spec.name);
    }
    ExecutableImage image;
    image.id = ImageId{static_cast<std::uint32_t>(processor.images.images.size() + 1U)};
    image.bytes = bytes;
    for (const auto base : spec.bases) image.mappings.push_back(ImageMapping{base, first_offset, range_length});
    image.provenance.authority = ImageAuthority::immutable_input;
    image.provenance.producer = "sega_cd.disc_module";
    image.provenance.evidence = {{"module", spec.name},
                                 {"disc.extent", "track:1 block:" + std::to_string(extent.block) + " length:" + std::to_string(extent.size)},
                                 {"sha256", digest}};
    if (spec.path) image.provenance.evidence.push_back({"iso9660.path", *spec.path});
    image.verification = ImageVerification::byte_identity;
    processor.images.platform_selector = true;
    processor.images.images.push_back(std::move(image));
    std::vector<std::uint32_t> entries = spec.entries;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    processor.roles.push_back(ImageRole{spec.name, true, entries});
    AdmittedModule admitted;
    admitted.name = spec.name;
    admitted.processor = spec.processor;
    admitted.path = spec.path.value_or("");
    admitted.extent = extent;
    admitted.sha256 = digest;
    admitted.bases = spec.bases;
    admitted.entries = entries;
    admitted.first_offset = first_offset;
    admitted.length = range_length;
    admitted.image_id = processor.images.images.back().id.value;
    result.modules.push_back(admitted);
    canonical << "module " << spec.name << " processor " << processor_name(spec.processor) << " block " << extent.block << " size "
              << extent.size << " sha256 " << digest << " path " << (spec.path ? *spec.path : std::string("-"));
    for (const auto base : spec.bases) canonical << " map " << hex32(base);
    if (spec.range) canonical << " range " << hex32(first_offset) << " " << hex32(range_length);  // absent: byte-identical to the SEG-063 identity
    for (const auto entry : entries) canonical << " entry " << hex32(entry);
    canonical << " verify byte_identity\n";
  }
  for (const auto *set : {&staged.main.images, &staged.sub.images}) {
    const auto validation = validate_executable_image_set(*set);
    if (!validation.ok()) {
      result.validation = validation.error;
      return fail(ModuleError::image_validation, "");
    }
  }
  const std::string text = canonical.str();
  result.identity = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
  images = std::move(staged);
  return result;
}

}  // namespace segarecomp::machine::sega_cd
