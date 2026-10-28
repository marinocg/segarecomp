#include "sega_cd_command.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "segarecomp/disc.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/bios_profile.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"
#include "segarecomp/machine/sega_cd/loader_scan.hpp"
#include "segarecomp/machine/sega_cd/modules.hpp"
#include "segarecomp/machine/sega_cd/program.hpp"
#include "segarecomp/machine/sega_cd/source_resolve.hpp"
#include "segarecomp/sha256.hpp"

namespace {

namespace scd = segarecomp::machine::sega_cd;

std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) >= 0x7FU) {  // ASCII-only JSON (disc bytes are untrusted)
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buffer;
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

std::string descriptor_json(const segarecomp::SegaCdBootDescriptor &d) {
  std::ostringstream out;
  out << "{\"disc_address\":" << d.disc_address << ",\"load_size\":" << d.load_size << ",\"entry_offset\":" << d.entry_offset
      << ",\"work_ram_size\":" << d.work_ram_size << "}";
  return out.str();
}

std::string processor_json(const scd::ProcessorImages &p) {
  const auto &image = p.images.images.front();
  std::ostringstream out;
  out << "{\"processor\":\"" << scd::processor_name(p.processor) << "\",\"cpu\":\"mc68000\",\"entry_pc\":" << p.entry_pc
      << ",\"execution_base\":" << image.mappings.front().execution_base << ",\"length\":" << image.mappings.front().length
      << ",\"producer\":\"" << image.provenance.producer << "\",\"authority\":\""
      << segarecomp::image_authority_name(image.provenance.authority) << "\",\"verification\":\""
      << segarecomp::image_verification_name(image.verification) << "\",\"sha256\":\"" << segarecomp::sha256_hex(image.bytes)
      << "\",\"provenance\":" << segarecomp::format_image_provenance_json(segarecomp::count_image_provenance(p.images)) << "}";
  return out.str();
}

// SEG-063: read, parse and admit a module list. Returns the error JSON object (without the closing brace context) on failure.
struct ModulesLoaded {
  std::optional<scd::ModuleAdmission> admission;
  std::string error_json;
};

ModulesLoaded load_modules(const std::string &path, const segarecomp::DiscImage &disc, scd::BootImages &images) {
  ModulesLoaded out;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  std::ifstream in(path, std::ios::binary);
  if (ec || !std::filesystem::is_regular_file(path, ec) || size > scd::kModuleManifestMaxBytes || !in) {
    out.error_json = "{\"ok\":false,\"stage\":\"modules\",\"error\":\"manifest_unreadable\"}";
    return out;
  }
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = scd::parse_module_manifest(text.str());
  if (!parsed.manifest) {
    out.error_json = "{\"ok\":false,\"stage\":\"modules\",\"error\":\"" + std::string(scd::module_error_name(parsed.error)) +
                     "\",\"line\":" + std::to_string(parsed.line) + "}";
    return out;
  }
  auto admission = scd::admit_modules(disc, *parsed.manifest, images);
  if (admission.error != scd::ModuleError::none) {
    out.error_json = "{\"ok\":false,\"stage\":\"modules\",\"error\":\"" + std::string(scd::module_error_name(admission.error)) +
                     "\",\"iso9660\":\"" + segarecomp::iso9660_error_name(admission.iso_error) + "\",\"validation\":\"" +
                     std::string(segarecomp::image_validation_error_name(admission.validation)) + "\",\"module\":" +
                     json_string(admission.module) + "}";
    return out;
  }
  out.admission = std::move(admission);
  return out;
}

std::string modules_json(const scd::ModuleAdmission &admission) {
  std::ostringstream out;
  out << "{\"identity\":\"" << admission.identity << "\",\"modules\":[";
  for (std::size_t i = 0; i < admission.modules.size(); ++i) {
    const auto &m = admission.modules[i];
    out << (i ? "," : "") << "{\"name\":" << json_string(m.name) << ",\"processor\":\"" << scd::processor_name(m.processor)
        << "\",\"path\":" << json_string(m.path) << ",\"block\":" << m.extent.block << ",\"size\":" << m.extent.size
        << ",\"sha256\":\"" << m.sha256 << "\",\"image_id\":" << m.image_id << ",\"bases\":[";
    for (std::size_t b = 0; b < m.bases.size(); ++b) out << (b ? "," : "") << m.bases[b];
    out << "],\"entries\":[";
    for (std::size_t e = 0; e < m.entries.size(); ++e) out << (e ? "," : "") << m.entries[e];
    out << "]}";
  }
  out << "]}";
  return out.str();
}

// The ISO9660 directory tree of track 1 (bounded depth and count), or the typed error.
std::string iso_tree_json(const segarecomp::DiscImage &disc) {
  if (disc.tracks().empty() || disc.tracks().front().type == segarecomp::DiscTrackType::audio)
    return "{\"ok\":false,\"error\":\"read_failed\"}";
  const auto read = segarecomp::iso9660_track_reader(disc, 0);
  const auto volume = segarecomp::read_iso9660_volume(read, disc.tracks().front().sector_count);
  if (!volume.volume) return std::string("{\"ok\":false,\"error\":\"") + segarecomp::iso9660_error_name(volume.error) + "\"}";
  std::ostringstream out;
  out << "{\"ok\":true,\"volume_id\":" << json_string(volume.volume->volume_id) << ",\"volume_space\":" << volume.volume->volume_space
      << ",\"root\":{\"block\":" << volume.volume->root.block << ",\"size\":" << volume.volume->root.size << "},\"entries\":[";
  struct Pending {
    std::string path;
    segarecomp::Iso9660Extent extent;
    unsigned depth;
  };
  std::vector<Pending> work{{"", volume.volume->root, 0U}};
  bool first = true;
  std::size_t count = 0;
  while (!work.empty()) {
    const Pending dir = work.front();
    work.erase(work.begin());
    const auto listing = segarecomp::list_iso9660_directory(read, *volume.volume, dir.extent);
    if (listing.error != segarecomp::Iso9660Error::none)
      return std::string("{\"ok\":false,\"error\":\"") + segarecomp::iso9660_error_name(listing.error) + "\"}";
    for (const auto &entry : listing.entries) {
      if (++count > 1024U) return "{\"ok\":false,\"error\":\"too_many_entries\"}";
      const std::string path = dir.path + "/" + entry.identifier;
      out << (first ? "" : ",") << "{\"path\":" << json_string(path) << ",\"directory\":" << (entry.directory ? "true" : "false")
          << ",\"block\":" << entry.extent.block << ",\"size\":" << entry.extent.size << "}";
      first = false;
      if (entry.directory) {
        if (dir.depth + 1U >= segarecomp::kIso9660MaxDepth) return "{\"ok\":false,\"error\":\"path_invalid\"}";
        work.push_back({path, entry.extent, dir.depth + 1U});
      }
    }
  }
  out << "]}";
  return out.str();
}

int inspect(int argc, char **argv) {
  std::optional<std::string> cue, expected, modules;
  bool sector_digests = false, iso_tree = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--expect-identity" && i + 1 < argc) expected = argv[++i];
    else if (arg == "--sector-digests") sector_digests = true;
    else if (arg == "--iso-tree") iso_tree = true;
    else if (arg == "--modules" && i + 1 < argc) modules = argv[++i];
    else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  if (!cue) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  const auto loaded = segarecomp::load_cue_disc(*cue, expected ? std::optional<std::string_view>(*expected) : std::nullopt);
  if (!loaded.disc) {
    std::cout << "{\"ok\":false,\"stage\":\"disc\",\"error\":\"" << segarecomp::disc_error_name(loaded.error)
              << "\",\"detail\":" << json_string(loaded.detail) << "}\n";
    return 3;
  }
  const auto &disc = *loaded.disc;
  std::ostringstream out;
  out << "{\"ok\":true,\"platform\":\"sega_cd\",\"identity\":\"" << disc.identity() << "\",\"total_sectors\":" << disc.total_sectors()
      << ",\"files\":[";
  for (std::size_t f = 0; f < disc.files().size(); ++f)
    out << (f ? "," : "") << "{\"name\":" << json_string(disc.files()[f].name) << ",\"size\":" << disc.files()[f].size
        << ",\"sha256\":\"" << disc.files()[f].sha256 << "\"}";
  out << "],\"tracks\":[";
  for (std::size_t t = 0; t < disc.tracks().size(); ++t) {
    const auto &track = disc.tracks()[t];
    out << (t ? "," : "") << "{\"number\":" << track.number << ",\"type\":\"" << segarecomp::disc_track_type_name(track.type)
        << "\",\"file\":" << track.file_index << ",\"file_offset\":" << track.file_offset << ",\"sector_size\":" << track.sector_size
        << ",\"pregap\":" << track.pregap << ",\"index0\":" << track.index0_sectors << ",\"first_lba\":" << track.first_lba << ",\"sector_count\":" << track.sector_count << "}";
  }
  out << "]";
  if (sector_digests) {
    out << ",\"sector_sha256\":{";
    bool first = true;
    std::uint8_t block[segarecomp::kDiscUserBytes];
    for (const auto &track : disc.tracks()) {
      if (track.type == segarecomp::DiscTrackType::audio) continue;
      for (std::uint32_t s = 0; s < track.sector_count; ++s) {
        const auto lba = track.first_lba + s;
        if (disc.read_user_data(lba, block) != segarecomp::DiscError::none) {
          std::cout << "{\"ok\":false,\"stage\":\"read\",\"error\":\"io_error\"}\n";
          return 3;
        }
        out << (first ? "" : ",") << "\"" << lba << "\":\"" << segarecomp::sha256_hex(block) << "\"";
        first = false;
      }
    }
    out << "}";
  }
  if (iso_tree) out << ",\"iso9660\":" << iso_tree_json(disc);
  const auto produced = scd::produce_boot_images(disc);
  if (!produced.images) {
    out << ",\"boot\":{\"ok\":false,\"error\":\"" << scd::boot_image_error_name(produced.error) << "\",\"boot_error\":\""
        << segarecomp::sega_cd_boot_error_name(produced.boot_error) << "\",\"validation\":\""
        << segarecomp::image_validation_error_name(produced.validation) << "\"}}\n";
    std::cout << out.str();
    return 4;
  }
  auto images = *produced.images;
  if (modules) {
    const auto loaded_modules = load_modules(*modules, disc, images);
    if (!loaded_modules.admission) {
      out << ",\"modules\":" << loaded_modules.error_json << "}\n";
      std::cout << out.str();
      return 5;
    }
    out << ",\"modules\":" << modules_json(*loaded_modules.admission);
  }
  const auto &h = images.header;
  out << ",\"boot\":{\"ok\":true,\"system_id\":" << json_string(h.system_id) << ",\"volume_name\":" << json_string(h.volume_name)
      << ",\"system_name\":" << json_string(h.system_name) << ",\"hardware_id\":" << json_string(h.hardware_id)
      << ",\"product_code\":" << json_string(h.product_code) << ",\"region\":" << json_string(h.region)
      << ",\"ip\":" << descriptor_json(h.ip) << ",\"sp\":" << descriptor_json(h.sp) << ",\"images\":["
      << processor_json(images.main) << "," << processor_json(images.sub) << "]}}\n";
  std::cout << out.str();
  return 0;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string &path, std::uintmax_t limit) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || std::filesystem::is_directory(status)) return std::nullopt;
  // A regular file, or (SEG-064-T003) a pipe such as /dev/fd/N, so that a locally held BIOS can be streamed without a copy on disk.
  // Either way at most `limit` bytes are accepted: one more byte is a refusal.
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(limit) + 1U);
  in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (in.bad()) return std::nullopt;
  const auto got = static_cast<std::size_t>(in.gcount());
  if (got > limit) return std::nullopt;
  bytes.resize(got);
  return bytes;
}

std::optional<std::uint32_t> parse_u32(const std::string &text) {
  if (text.empty() || text.size() > 12U || !std::isdigit(static_cast<unsigned char>(text.front()))) return std::nullopt;
  try {
    std::size_t used = 0;
    const unsigned long value = std::stoul(text, &used, 0);
    if (used != text.size() || value > 0xFFFFFFFFUL) return std::nullopt;
    return static_cast<std::uint32_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<std::string> split(const std::string &text, char sep) {
  std::vector<std::string> out;
  std::string piece;
  std::istringstream in(text);
  while (std::getline(in, piece, sep)) out.push_back(piece);
  return out;
}

std::optional<scd::Processor> parse_processor(const std::string &text) {
  if (text == "main") return scd::Processor::main;
  if (text == "sub") return scd::Processor::sub;
  return std::nullopt;
}

// SEG-064-T002 (ADR 0142): the BIOS-image profile emission. Returns the process status.
// The bytes of a track-1 extent through the verified disc reader; an empty string on success, else the typed error.
std::string read_track1_extent(const segarecomp::DiscImage &disc, std::uint32_t off, std::uint32_t len, std::vector<std::uint8_t> &bytes) {
  if (disc.tracks().empty() || disc.tracks().front().type == segarecomp::DiscTrackType::audio) return "first_track_not_data";
  const auto &first = disc.tracks().front();
  const std::uint64_t end = static_cast<std::uint64_t>(off) + len;
  if ((end + segarecomp::kDiscUserBytes - 1U) / segarecomp::kDiscUserBytes > first.sector_count) return "disc_image_outside_track";
  bytes.clear();
  bytes.reserve(len);
  std::array<std::uint8_t, segarecomp::kDiscUserBytes> block{};
  for (std::uint32_t at = off; at < end;) {
    const std::uint32_t lba = first.first_lba + at / segarecomp::kDiscUserBytes;
    if (disc.read_user_data(lba, std::span<std::uint8_t, segarecomp::kDiscUserBytes>(block)) != segarecomp::DiscError::none) return "disc_image_unreadable";
    const std::uint32_t inside = at % segarecomp::kDiscUserBytes;
    const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(segarecomp::kDiscUserBytes - inside, end - at));
    bytes.insert(bytes.end(), block.begin() + inside, block.begin() + inside + take);
    at += take;
  }
  return {};
}

int emit_bios_profile(const segarecomp::DiscImage &disc, const std::string &bios_path, const std::vector<std::string> &copies,
                      const std::vector<std::string> &admits, const std::vector<std::string> &disc_images,
                      std::optional<std::uint32_t> application_entry, const std::string &out_path, std::uint32_t partition_window) {
  const auto bad = [](const std::string &stage, const std::string &error) {
    std::cout << "{\"ok\":false,\"stage\":\"" << stage << "\",\"error\":\"" << error << "\"}\n";
    return 7;
  };
  const auto bios_bytes = read_file(bios_path, scd::kBiosWindowBytes);
  if (!bios_bytes) return bad("bios", "bios_unreadable");
  auto admitted = scd::admit_bios(*bios_bytes);
  if (!admitted.bios) return bad("bios", scd::bios_error_name(admitted.error));
  scd::BiosProfileInputs inputs;
  inputs.bios = std::move(*admitted.bios);
  for (const auto &text : copies) {
    const auto f = split(text, ':');
    const auto proc = f.size() == 4U ? parse_processor(f[0]) : std::nullopt;
    const auto base = f.size() == 4U ? parse_u32(f[1]) : std::nullopt;
    const auto off = f.size() == 4U ? parse_u32(f[2]) : std::nullopt;
    const auto len = f.size() == 4U ? parse_u32(f[3]) : std::nullopt;
    if (!proc || !base || !off || !len) return bad("copy", "copy_syntax");
    inputs.copies.push_back(scd::CopyImage{*proc, *base, *off, *len});
  }
  for (const auto &text : disc_images) {  // main|sub:<track-1 byte offset>:<base>:<length> (SEG-064-T004)
    const auto f = split(text, ':');
    const auto proc = f.size() == 4U ? parse_processor(f[0]) : std::nullopt;
    const auto off = f.size() == 4U ? parse_u32(f[1]) : std::nullopt;
    const auto base = f.size() == 4U ? parse_u32(f[2]) : std::nullopt;
    const auto len = f.size() == 4U ? parse_u32(f[3]) : std::nullopt;
    if (!proc || !off || !base || !len || *len == 0U || *len > 0x80000U) return bad("disc_image", "disc_image_syntax");
    // The extent is read through the verified disc reader from the logical user data of track 1 (cooked 2048-byte blocks).
    scd::DiscImageSpec spec{*proc, *off, *base, *len, {}};
    if (const auto error = read_track1_extent(disc, *off, *len, spec.bytes); !error.empty()) return bad("disc_image", error);
    inputs.disc_images.push_back(std::move(spec));
  }
  if (application_entry) inputs.application_entry = *application_entry;
  for (const auto &text : admits) {
    auto f = split(text, ':');
    scd::Admission a;
    if (f.size() >= 3U && f[0] == "epoch") {  // the snapshot path is the remainder (it may contain ':')
      const auto second = text.find(':', text.find(':') + 1U);
      f.resize(3U);
      f[2] = text.substr(second + 1U);
      const auto bytes = read_file(f[2], scd::kPrgRamBytes);
      if (!bytes) return bad("admit", "snapshot_unreadable");
      a.kind = scd::AdmissionKind::epoch;
      a.pass_identity = f[1];
      a.bytes = *bytes;
    } else if (text.rfind("epoch-recipe:", 0) == 0) {  // SEG-079 (ADR 0179): a static producer, no pass identity
      const auto recipe = scd::parse_epoch_recipe(text.substr(13));
      if (!recipe) return bad("admit", "epoch_recipe_syntax");
      a.kind = scd::AdmissionKind::epoch_recipe;
      a.recipe = *recipe;
    } else if (f.size() == 5U && f[0] == "template") {
      const auto proc = parse_processor(f[2]);
      const auto pc = parse_u32(f[3]);
      if (!proc || !pc || f[4].size() % 2U != 0U || f[4].size() > 32U) return bad("admit", "template_syntax");
      a.kind = scd::AdmissionKind::operand_template;
      a.pass_identity = f[1];
      a.processor = *proc;
      a.address = *pc;
      for (std::size_t i = 0; i < f[4].size(); i += 2U) {
        const auto byte = parse_u32("0x" + f[4].substr(i, 2));
        if (!byte) return bad("admit", "template_syntax");
        a.bytes.push_back(static_cast<std::uint8_t>(*byte));
      }
    } else if (f.size() == 6U && f[0] == "extent") {  // SEG-078-T003 (ADR 0205): extent:<pass>:<cpu>:<track-1 offset>:<base>:<length>
      const auto proc = parse_processor(f[2]);
      const auto off = parse_u32(f[3]);
      const auto base = parse_u32(f[4]);
      const auto len = parse_u32(f[5]);
      if (!proc || !off || !base || !len || *len == 0U || *len > 0x80000U) return bad("admit", "extent_syntax");
      a.kind = scd::AdmissionKind::disc_extent;
      a.pass_identity = f[1];
      a.processor = *proc;
      a.address = *base;
      a.track_offset = *off;
      if (const auto error = read_track1_extent(disc, *off, *len, a.bytes); !error.empty()) return bad("admit", error);
    } else if (f.size() == 6U && f[0] == "template-run") {  // SEG-079-T002 (ADR 0180): template-run:<pass>:<cpu>:<pc>:<slots>:<6*slots bytes hex>
      const auto proc = parse_processor(f[2]);
      const auto pc = parse_u32(f[3]);
      const auto slots = parse_u32(f[4]);
      if (!proc || !pc || !slots || *slots < 2U || *slots > scd::kMaxTemplateRunSlots ||
          f[5].size() != std::size_t{*slots} * scd::kTemplateSlotBytes * 2U)
        return bad("admit", "template_syntax");
      a.kind = scd::AdmissionKind::operand_template;
      a.pass_identity = f[1];
      a.processor = *proc;
      a.address = *pc;
      a.slots = *slots;
      for (std::size_t i = 0; i < f[5].size(); i += 2U) {
        const auto byte = parse_u32("0x" + f[5].substr(i, 2));
        if (!byte) return bad("admit", "template_syntax");
        a.bytes.push_back(static_cast<std::uint8_t>(*byte));
      }
    } else {
      return bad("admit", "admit_syntax");
    }
    inputs.admissions.push_back(std::move(a));
  }
  const auto emission = scd::emit_sega_cd_bios_program(disc, inputs, scd::BiosProfileEmitOptions{partition_window});
  if (emission.error != scd::BiosProfileError::none) {
    std::cout << "{\"ok\":false,\"stage\":\"emit\",\"error\":\"" << scd::bios_profile_error_name(emission.error) << "\",\"index\":"
              << emission.error_index << ",\"validation\":\"" << segarecomp::image_validation_error_name(emission.validation) << "\"}\n";
    return 6;
  }
  // SEG-064-T003: the shared dispatch unit goes to --out, every partition unit next to it under its own name (listed, in order, in
  // the report; the driver compiles exactly the listed units).
  const auto write_text = [](const std::filesystem::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file.flush()) {
      std::cerr << "segarecomp: cannot write " << path.string() << "\n";
      return false;
    }
    return true;
  };
  for (const auto &unit : emission.units)
    if (std::filesystem::path(out_path).filename() == unit.name) return bad("emit", "out_path_names_a_unit");
  if (!write_text(out_path, emission.c_source)) return 1;
  std::size_t unit_bytes = 0;
  for (const auto &unit : emission.units) {
    if (!write_text(std::filesystem::path(out_path).parent_path() / unit.name, unit.c_source)) return 1;
    unit_bytes += unit.c_source.size();
  }
  std::ostringstream report;
  report << "{\"ok\":true,\"profile\":\"bios_image\",\"disc_identity\":\"" << disc.identity() << "\",\"program_identity\":\""
         << emission.program_identity << "\",\"admissions\":" << inputs.admissions.size() << ",\"functions\":" << emission.functions
         << ",\"templates\":" << emission.templates << ",\"deduplicated\":" << emission.deduplicated << ",\"c_bytes\":"
         << emission.c_source.size() + unit_bytes << ",\"partition_window\":" << partition_window << ",\"units\":[";
  for (std::size_t i = 0; i < emission.units.size(); ++i) {
    const auto &u = emission.units[i];
    report << (i ? "," : "") << "{\"name\":\"" << u.name << "\",\"cpu\":" << u.cpu << ",\"first_pc\":" << u.first_pc
           << ",\"functions\":" << u.functions << ",\"c_bytes\":" << u.c_source.size() << "}";
  }
  report << "],\"processors\":[";
  for (unsigned k = 0; k < 2U; ++k)
    report << (k ? "," : "") << "{\"entries\":" << emission.entries[k] << ",\"variant_pcs\":" << emission.variant_pcs[k] << "}";
  report << "],\"images\":[";
  for (std::size_t i = 0; i < emission.images.size(); ++i) {
    const auto &s = emission.images[i];
    report << (i ? "," : "") << "{\"processor\":\"" << scd::processor_name(s.processor) << "\",\"name\":\"" << s.name
           << "\",\"authority\":\"" << s.authority << "\",\"start\":" << s.start << ",\"length\":" << s.length
           << ",\"compiled\":" << s.compiled << ",\"rejected\":" << s.rejected << "}";
  }
  report << "]}\n";
  std::cout << report.str();
  return 0;
}

int emit(int argc, char **argv) {
  std::optional<std::string> cue, out_path, expected, modules, bios;
  std::vector<std::string> copies, admits, disc_images;
  std::optional<std::uint32_t> partition_window, application_entry;
  scd::SegaCdEmitOptions emit_options;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--position-independent") emit_options.position_independent = true;  // SEG-097-T002 (ADR 0207)
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (arg == "--expect-identity" && i + 1 < argc) expected = argv[++i];
    else if (arg == "--modules" && i + 1 < argc) modules = argv[++i];
    else if (arg == "--bios" && i + 1 < argc) bios = argv[++i];
    else if (arg == "--copy-image" && i + 1 < argc) copies.emplace_back(argv[++i]);
    else if (arg == "--disc-image" && i + 1 < argc) disc_images.emplace_back(argv[++i]);
    else if (arg == "--application-entry" && i + 1 < argc && !application_entry) {
      application_entry = parse_u32(argv[++i]);
      if (!application_entry || *application_entry < 0x00200000U || *application_entry > 0x00FFFFFFU || (*application_entry & 1U) != 0U) {
        std::cerr << "segarecomp: --application-entry must be an even address in $200000-$FFFFFF (Word RAM or work RAM)\n";
        return 2;
      }
    } else if (arg == "--admit" && i + 1 < argc) admits.emplace_back(argv[++i]);
    else if (arg == "--partition-window" && i + 1 < argc && !partition_window) {
      partition_window = parse_u32(argv[++i]);
      if (!partition_window || !scd::valid_partition_window(*partition_window)) {
        std::cerr << "segarecomp: --partition-window must be a power of two in [256, 16 MiB]\n";
        return 2;
      }
    } else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  if (!cue || !out_path || (!bios && (!copies.empty() || !admits.empty() || !disc_images.empty() || application_entry || partition_window)) || (bios && modules) || (bios && emit_options.position_independent)) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  const auto loaded = segarecomp::load_cue_disc(*cue, expected ? std::optional<std::string_view>(*expected) : std::nullopt);
  if (!loaded.disc) {
    std::cout << "{\"ok\":false,\"stage\":\"disc\",\"error\":\"" << segarecomp::disc_error_name(loaded.error) << "\"}\n";
    return 3;
  }
  if (bios) return emit_bios_profile(*loaded.disc, *bios, copies, admits, disc_images, application_entry, *out_path, partition_window.value_or(0U));
  const auto produced = scd::produce_boot_images(*loaded.disc);
  if (!produced.images) {
    std::cout << "{\"ok\":false,\"stage\":\"boot\",\"error\":\"" << scd::boot_image_error_name(produced.error) << "\"}\n";
    return 4;
  }
  auto images = *produced.images;
  std::optional<scd::ModuleAdmission> admission;
  if (modules) {
    auto loaded_modules = load_modules(*modules, *loaded.disc, images);
    if (!loaded_modules.admission) {
      std::cout << loaded_modules.error_json << "\n";
      return 5;
    }
    admission = std::move(loaded_modules.admission);
  }
  const auto emission = scd::emit_sega_cd_program(*loaded.disc, images, admission ? std::string_view(admission->identity) : std::string_view(), emit_options);
  if (emission.error != scd::EmissionError::none) {
    std::cout << "{\"ok\":false,\"stage\":\"emit\",\"error\":\"" << scd::emission_error_name(emission.error) << "\"}\n";
    return 6;
  }
  std::ofstream file(*out_path, std::ios::binary);
  file << emission.c_source;
  if (!file.flush()) {
    std::cerr << "segarecomp: cannot write " << *out_path << "\n";
    return 1;
  }
  std::ostringstream report;
  report << "{\"ok\":true,\"disc_identity\":\"" << loaded.disc->identity() << "\",\"program_identity\":\""
         << emission.program_identity << "\",\"processors\":[";
  for (std::size_t i = 0; i < emission.stats.size(); ++i) {
    const auto &s = emission.stats[i];
    report << (i ? "," : "") << "{\"processor\":\"" << scd::processor_name(s.processor) << "\",\"compiled\":" << s.compiled
           << ",\"unsupported\":" << s.unsupported << ",\"out_of_image_targets\":" << s.out_of_image_targets
           << ",\"vector_roots\":" << s.vector_roots << ",\"bodies\":" << s.bodies << ",\"shared_bodies\":" << s.shared_bodies
           << ",\"shared_instances\":" << s.shared_instances << ",\"fallback_instances\":" << s.fallback_instances << ",\"fallback_reasons\":{";
    for (std::size_t r = 0; r < s.fallback_reasons.size(); ++r)
      report << (r ? "," : "") << "\"" << s.fallback_reasons[r].first << "\":" << s.fallback_reasons[r].second;
    report << "}}";
  }
  report << "],\"position_independent\":" << (emit_options.position_independent ? "true" : "false") << ",\"overlay_groups\":" << emission.overlay_groups << ",\"images\":[";
  for (std::size_t i = 0; i < emission.images.size(); ++i) {
    const auto &image = emission.images[i];
    report << (i ? "," : "") << "{\"processor\":\"" << scd::processor_name(image.processor) << "\",\"image_id\":" << image.image_id
           << ",\"name\":" << json_string(image.name) << ",\"producer\":\"" << image.producer << "\",\"mapping\":"
           << image.mapping_index << ",\"start\":" << image.start << ",\"length\":" << image.length << ",\"roots\":" << image.roots
           << ",\"compiled\":" << image.compiled << ",\"unsupported\":" << image.unsupported << ",\"out_of_image_targets\":"
           << image.out_of_image_targets << ",\"overlay_group\":" << image.overlay_group << "}";
  }
  report << "]";
  if (admission) report << ",\"modules\":" << modules_json(*admission);
  report << "}\n";
  std::cout << report.str();
  return 0;
}

// SEG-078-T003 (ADR 0205): resolve the disc source of a frontier's RAM window.
int resolve_source_command(int argc, char **argv) {
  std::optional<std::string> cue, region_path, cpu_text;
  std::optional<std::uint32_t> pc, region_base;
  std::string delivered_text;
  bool overflow = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--cpu" && i + 1 < argc) cpu_text = argv[++i];
    else if (arg == "--pc" && i + 1 < argc) pc = parse_u32(argv[++i]);
    else if (arg == "--region" && i + 1 < argc) region_path = argv[++i];
    else if (arg == "--region-base" && i + 1 < argc) region_base = parse_u32(argv[++i]);
    else if (arg == "--delivered" && i + 1 < argc) delivered_text = argv[++i];
    else if (arg == "--delivered-overflow" && i + 1 < argc) overflow = std::string_view(argv[++i]) == "1";
    else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  const auto processor = cpu_text ? parse_processor(*cpu_text) : std::nullopt;
  if (!cue || !region_path || !pc || !region_base || !processor) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  const auto refuse = [](const std::string &error) {
    std::cout << "{\"ok\":false,\"error\":\"" << error << "\"}\n";
    return 8;
  };
  const auto region = read_file(*region_path, 0x80000U);
  if (!region) return refuse("bad_evidence");
  scd::SourceEvidence evidence;
  evidence.processor = *processor;
  evidence.pc = *pc;
  evidence.region_base = *region_base;
  evidence.region = *region;
  evidence.delivered_overflow = overflow;
  if (!delivered_text.empty()) {
    for (const auto &item : split(delivered_text, ',')) {
      const auto f = split(item, ':');
      const auto first = f.size() == 2U ? parse_u32(f[0]) : std::nullopt;
      const auto count = f.size() == 2U ? parse_u32(f[1]) : std::nullopt;
      if (!first || !count) return refuse("bad_evidence");
      if (evidence.delivered.size() >= 4096U) return refuse("bad_evidence");
      evidence.delivered.push_back({*first, *count});
    }
  }
  const auto loaded = segarecomp::load_cue_disc(*cue);
  if (!loaded.disc) return refuse(std::string("disc_") + segarecomp::disc_error_name(loaded.error));
  const auto result = scd::resolve_source(*loaded.disc, evidence);
  if (!result.source) return refuse(scd::resolve_error_name(result.error));
  std::cout << "{\"ok\":true,\"processor\":\"" << scd::processor_name(*processor) << "\",\"offset\":" << result.source->disc_offset
            << ",\"base\":" << result.source->base << ",\"length\":" << result.source->length << ",\"candidates\":" << result.source->candidates << "}\n";
  return 0;
}

// SEG-097-T001 (ADR 0206): static discovery of disc-loaded modules. Pure function of the disc; executes nothing.
int discover_modules_command(int argc, char **argv) {
  std::optional<std::string> cue, out_path;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  if (!cue) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  const auto loaded = segarecomp::load_cue_disc(*cue);
  if (!loaded.disc) {
    std::cout << "{\"ok\":false,\"stage\":\"disc\",\"error\":\"" << segarecomp::disc_error_name(loaded.error) << "\"}\n";
    return 3;
  }
  const auto produced = scd::produce_boot_images(*loaded.disc);
  if (!produced.images) {
    std::cout << "{\"ok\":false,\"stage\":\"boot\",\"error\":\"" << scd::boot_image_error_name(produced.error) << "\"}\n";
    return 4;
  }
  const auto scan = scd::scan_sega_cd_loaders(*loaded.disc, *produced.images);
  std::ostringstream report;
  report << "{\"ok\":" << (!scan.modules.empty() && scan.complete ? "true" : "false") << ",\"complete\":" << (scan.complete ? "true" : "false")
         << ",\"disc_identity\":\"" << loaded.disc->identity() << "\",\"identity\":\"" << scan.identity << "\",\"states\":" << scan.states
         << ",\"steps\":" << scan.steps << ",\"operations\":[";
  for (std::size_t i = 0; i < scan.operations.size(); ++i) {
    const auto &o = scan.operations[i];
    report << (i ? "," : "") << "{\"lba\":" << o.lba << ",\"destination\":" << o.destination << ",\"bytes\":" << o.bytes << ",\"entries\":[";
    for (std::size_t e = 0; e < o.entries.size(); ++e) report << (e ? "," : "") << o.entries[e];
    report << "]}";
  }
  report << "],\"modules\":[";
  for (std::size_t i = 0; i < scan.modules.size(); ++i) {
    const auto &m = scan.modules[i];
    report << (i ? "," : "") << "{\"name\":" << json_string(m.name) << ",\"block\":" << m.block << ",\"size\":" << m.size << ",\"sha256\":\"" << m.sha256
           << "\",\"range\":{\"offset\":" << m.range_offset << ",\"length\":" << m.range_length << "},\"bases\":[";
    for (std::size_t b = 0; b < m.bases.size(); ++b) report << (b ? "," : "") << m.bases[b];
    report << "],\"entries\":[";
    for (std::size_t e = 0; e < m.entries.size(); ++e) report << (e ? "," : "") << m.entries[e];
    report << "]}";
  }
  report << "],\"refusals\":[";
  for (std::size_t i = 0; i < scan.refusals.size(); ++i)
    report << (i ? "," : "") << "{\"reason\":\"" << scd::loader_refusal_name(scan.refusals[i].kind) << "\",\"pc\":" << scan.refusals[i].pc << "}";
  report << "]}\n";
  if (out_path && !scan.manifest.empty()) {
    std::ofstream out(*out_path, std::ios::binary);
    out << scan.manifest;
    if (!out) {
      std::cout << "{\"ok\":false,\"stage\":\"write\",\"error\":\"out_unwritable\"}\n";
      return 6;
    }
  }
  std::cout << report.str();
  return !scan.modules.empty() && scan.complete ? 0 : 9;
}

}  // namespace

int segarecomp_sega_cd_command(int argc, char **argv) {
  if (argc >= 3 && std::string_view(argv[2]) == "inspect") return inspect(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "emit") return emit(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "build") return segarecomp_sega_cd_build(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "resolve-source") return resolve_source_command(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "discover-modules") return discover_modules_command(argc, argv);
  std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
  return 2;
}
