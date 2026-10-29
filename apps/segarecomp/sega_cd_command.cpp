#include "sega_cd_command.hpp"

#include "sega_cd_aot.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <iostream>
#include <optional>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <atomic>

#include "segarecomp/disc.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/bios_profile.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"
#include "segarecomp/machine/sega_cd/file_candidates.hpp"
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

// The extents the static producer can emit for this disc, computed once per process: for each proven module and guest base the executable `hull` extent (the default policy) and the whole
// `loaded` extent (`--static-range loaded`), from the scan with and without the recursive walk (the build may run either). The declared range must be exactly one of them: containment in a
// proven source is not authority for an arbitrary subrange (a header, a partial range, a range crossing the hull boundary).
bool static_extent_is_proven(const segarecomp::DiscImage &disc, scd::Processor processor, std::uint32_t off, std::uint32_t base, std::uint32_t len) {
  static std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> authorized;   // (track-1 offset, guest base, length)
  static bool computed = false;
  if (!computed) {
    computed = true;
    const auto produced = scd::produce_boot_images(disc);
    if (produced.images) {
      for (const bool recursive : {true, false}) {
        scd::LoaderScanOptions options;
        options.module_table_roots = options.constant_dispatch = options.work_state = options.state_convergence = true;
        options.recursive_modules = recursive;
        const auto scan = scd::scan_sega_cd_loaders(disc, *produced.images, options);
        if (!scan.complete) continue;   // an incomplete scan proves nothing
        for (const auto &m : scan.modules)
          for (const auto b : m.bases) {
            const std::uint64_t lo = static_cast<std::uint64_t>(m.block) * 2048U;
            authorized.emplace(lo + m.range_offset, static_cast<std::uint64_t>(b) + m.range_offset, m.range_length);   // hull
            authorized.emplace(lo, b, m.size);                                                                         // loaded
          }
      }
    }
  }
  return processor == scd::Processor::sub && authorized.count({off, base, len}) != 0U;
}

int emit_bios_profile(const segarecomp::DiscImage &disc, const std::string &bios_path, const std::vector<std::string> &copies,
                      const std::vector<std::string> &admits, const std::vector<std::string> &disc_images,
                      std::optional<std::uint32_t> application_entry, const std::string &out_path, std::uint32_t partition_window,
                      const segarecomp_app::SegaCdAotOptions &aot, const std::optional<scd::SelectiveAdmissionRequest> &aot_request) {
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
    } else if (f.size() == 5U && f[0] == "static-extent") {  // SEG-098-T002 (ADR 0211): static-extent:<cpu>:<track-1 offset>:<base>:<length> (the static scan's proof; no pass)
      const auto proc = parse_processor(f[1]);
      const auto off = parse_u32(f[2]);
      const auto base = parse_u32(f[3]);
      const auto len = parse_u32(f[4]);
      if (!proc || !off || !base || !len || *len == 0U || *len > 0x80000U) return bad("admit", "extent_syntax");
      a.kind = scd::AdmissionKind::static_disc_extent;
      a.processor = *proc;
      a.address = *base;
      a.track_offset = *off;
      // SEG-099-T005 (ADR 0219): the kind's name is a claim - the static loader scan proved this source-to-mapping. The emitter does not trust the coordinate string: it recomputes the scan (a pure
      // function of the disc) and refuses an extent that no proven module mapping contains.
      if (!static_extent_is_proven(disc, *proc, *off, *base, *len)) return bad("admit", "static_extent_unproven");
      if (const auto error = read_track1_extent(disc, *off, *len, a.bytes); !error.empty()) return bad("admit", error);
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
  scd::BiosProfileEmitOptions bios_options{partition_window};
  if (aot_request) {  // SEG-097-T003 (ADR 0208): the experimental request enters the program identity and the provenance chain
    inputs.aot_policy = aot_request->policy_identity;
    bios_options.selective = &*aot_request;
  }
  const auto emission = scd::emit_sega_cd_bios_program(disc, inputs, bios_options);
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
           << ",\"compiled\":" << s.compiled << ",\"rejected\":" << s.rejected << ",\"omitted\":" << s.omitted << "}";
  }
  report << "],\"aot_policy\":" << segarecomp_app::sega_cd_aot_policy_json(aot, emission.admission) << "}\n";
  std::cout << report.str();
  return 0;
}

int emit(int argc, char **argv) {
  std::optional<std::string> cue, out_path, expected, modules, bios;
  std::vector<std::string> copies, admits, disc_images;
  std::optional<std::uint32_t> partition_window, application_entry;
  scd::SegaCdEmitOptions emit_options;
  segarecomp_app::SegaCdAotOptions aot;
  bool report_offsets = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--position-independent") emit_options.position_independent = true;  // SEG-097-T002 (ADR 0207)
    else if (arg == "--report-offsets") report_offsets = true;  // SEG-097-T003: list each mapping's compiled source-relative offsets in the report (evidence tooling)
    else if ((arg == "--aot-policy" || arg == "--aot-model" || arg == "--aot-scope") && i + 1 < argc) {  // SEG-097-T003 (ADR 0208): the product's policy vocabulary
      if (!segarecomp_app::set_sega_cd_aot_option(aot, arg, argv[++i])) {
        std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
        return 2;
      }
    }
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
  if (!cue || !out_path || (!bios && (!copies.empty() || !admits.empty() || !disc_images.empty() || application_entry || partition_window)) || (bios && modules) || (bios && emit_options.position_independent) || !segarecomp_app::sega_cd_aot_options_valid(aot)) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  const auto loaded = segarecomp::load_cue_disc(*cue, expected ? std::optional<std::string_view>(*expected) : std::nullopt);
  if (!loaded.disc) {
    std::cout << "{\"ok\":false,\"stage\":\"disc\",\"error\":\"" << segarecomp::disc_error_name(loaded.error) << "\"}\n";
    return 3;
  }
  const auto aot_request = segarecomp_app::make_sega_cd_aot_request(aot);
  if (bios) return emit_bios_profile(*loaded.disc, *bios, copies, admits, disc_images, application_entry, *out_path, partition_window.value_or(0U), aot, aot_request);
  if (aot_request) emit_options.selective = &*aot_request;
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
  report << "],\"aot_policy\":" << segarecomp_app::sega_cd_aot_policy_json(aot, emission.admission) << ",\"position_independent\":"
         << (emit_options.position_independent ? "true" : "false") << ",\"overlay_groups\":" << emission.overlay_groups << ",\"images\":[";
  for (std::size_t i = 0; i < emission.images.size(); ++i) {
    const auto &image = emission.images[i];
    report << (i ? "," : "") << "{\"processor\":\"" << scd::processor_name(image.processor) << "\",\"image_id\":" << image.image_id
           << ",\"name\":" << json_string(image.name) << ",\"producer\":\"" << image.producer << "\",\"mapping\":"
           << image.mapping_index << ",\"start\":" << image.start << ",\"length\":" << image.length << ",\"roots\":" << image.roots
           << ",\"compiled\":" << image.compiled << ",\"unsupported\":" << image.unsupported << ",\"out_of_image_targets\":"
           << image.out_of_image_targets << ",\"overlay_group\":" << image.overlay_group;
    if (report_offsets) {
      report << ",\"source_offsets\":[";
      for (std::size_t o = 0; o < image.source_offsets.size(); ++o) report << (o ? "," : "") << image.source_offsets[o];
      report << "]";
    }
    report << "}";
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
  std::optional<std::string> cue, out_path, extents_path, windows_path;
  bool table_roots = false, computed_continue = false, constant_dispatch = false, work_state = false, recursive_modules = false, state_convergence = false, converge_always = false;
  std::uint32_t widen_after = 24U, scan_order = 0U;
  std::string static_scope = "hull";   // SEG-098-T004: `loaded` = every loaded byte of a proven module (all aligned starts), `hull` = the traversed executable subrange
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (arg == "--constant-dispatch") constant_dispatch = true;   // SEG-098-T003 (ADR 0212)
    else if (arg == "--recursive-modules") recursive_modules = true;   // SEG-099-T004 (ADR 0218)
    else if (arg == "--state-convergence") state_convergence = true;   // SEG-099-T003 (ADR 0217)
    else if (arg == "--converge-always") { state_convergence = true; converge_always = true; }   // test-only
    else if (arg == "--widen-after" && i + 1 < argc) widen_after = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));   // diagnostic: the join threshold
    else if (arg == "--scan-order" && i + 1 < argc) scan_order = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));   // test-only exploration order
    else if (arg == "--work-state") work_state = true;   // SEG-099-T002 (ADR 0216): work-variable memory, condition codes and the documented BIOS CD services
    else if (arg == "--computed-call-whatif") computed_continue = true;   // SEG-098-T001 survey what-if (never used by a build)
    else if (arg == "--static-extents" && i + 1 < argc) extents_path = argv[++i];   // SEG-098-T002: one `<cpu>:<offset>:<base>:<length>` per verified mapping
    else if (arg == "--static-windows" && i + 1 < argc) windows_path = argv[++i];   // SEG-098-T004: `<cpu>:<base>:<bytes>` per loaded module window (classification of a later stop)
    else if (arg == "--static-scope" && i + 1 < argc) { static_scope = argv[++i]; if (static_scope != "hull" && static_scope != "loaded") { std::cerr << "usage:\n" << segarecomp_sega_cd_usage; return 2; } }
    else if (arg == "--module-table-roots") table_roots = true;   // SEG-098-T001: walk the SP module jump table, not only the descriptor entry
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
  scd::LoaderScanOptions scan_options;
  scan_options.module_table_roots = table_roots;
  scan_options.continue_after_computed_call = computed_continue;
  scan_options.constant_dispatch = constant_dispatch;
  scan_options.work_state = work_state;
  scan_options.recursive_modules = recursive_modules;
  scan_options.state_convergence = state_convergence;
  scan_options.widen_after = widen_after;
  scan_options.converge_always = converge_always;
  scan_options.order = scan_order;
  const auto scan = scd::scan_sega_cd_loaders(*loaded.disc, *produced.images, scan_options);
  std::ostringstream report;
  report << "{\"ok\":" << (!scan.modules.empty() && scan.complete ? "true" : "false") << ",\"complete\":" << (scan.complete ? "true" : "false")
         << ",\"disc_identity\":\"" << loaded.disc->identity() << "\",\"identity\":\"" << scan.identity << "\",\"states\":" << scan.states
         << ",\"steps\":" << scan.steps;
  {
    const auto &sv = scan.survey;
    report << ",\"survey\":{\"roots\":" << sv.roots << ",\"module_table_found\":" << (sv.module_table_found ? "true" : "false") << ",\"cdd_commands\":" << sv.cdd_commands
           << ",\"cdc_transfer_triggers\":" << sv.cdc_transfer_triggers << ",\"bios_service_calls\":" << sv.bios_service_calls << ",\"bios_service_unknown_function\":"
           << sv.bios_service_unknown_function << ",\"foreign_calls_other\":" << sv.foreign_calls_other << ",\"foreign_calls_unknown\":" << sv.foreign_calls_unknown
           << ",\"copy_loop_candidates\":" << sv.copy_loop_candidates << ",\"counted_loops\":" << sv.counted_loops << ",\"dispatch_resolved\":" << sv.dispatch_resolved << ",\"table_reads\":" << sv.table_reads << ",\"memory_carried_reads\":" << sv.memory_carried_reads
           << ",\"bios_modelled_calls\":" << sv.bios_modelled_calls << ",\"bios_unmodelled_calls\":" << sv.bios_unmodelled_calls << ",\"sector_transfers\":" << sv.sector_transfers
           << ",\"sector_transfers_unresolved\":" << sv.sector_transfers_unresolved << ",\"memory_args_resolved\":" << sv.memory_args_resolved << ",\"memory_args_unresolved\":" << sv.memory_args_unresolved
           << ",\"module_entries_walked\":" << sv.module_entries_walked << ",\"convergence_used\":" << (sv.convergence_used ? "true" : "false") << ",\"joins\":" << sv.joins << ",\"widenings\":" << sv.widenings << ",\"branches_decided\":" << sv.branches_decided << ",\"async_volatile_ranges\":" << sv.async_volatile_ranges << ",\"async_volatile_all\":" << (sv.async_volatile_all ? "true" : "false") << ",\"bios_functions\":{";
    bool first = true;
    for (const auto &[fn, n] : sv.bios_functions) { report << (first ? "" : ",") << "\"" << fn << "\":" << n; first = false; }
    report << "},\"path_ends\":{";
    first = true;
    for (const auto &[k, n] : sv.path_ends) { report << (first ? "" : ",") << "\"" << k << "\":" << n; first = false; }
    report << "}}";
  }
  report << ",\"operations\":[";
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
  if (extents_path) {   // the static admissions of the BIOS-image build: only a complete scan proposes extents (an exhausted inventory proposes nothing)
    std::ostringstream lines;
    if (scan.complete)
      for (const auto &m : scan.modules)
        for (const auto base : m.bases)
          lines << "sub:" << (static_cast<std::uint64_t>(m.block) * 2048U + (static_scope == "loaded" ? 0U : m.range_offset)) << ":" << (base + (static_scope == "loaded" ? 0U : m.range_offset)) << ":"
                << (static_scope == "loaded" ? m.size : m.range_length) << "\n";
    std::ofstream out(*extents_path, std::ios::binary);
    out << lines.str();
    if (!out) {
      std::cout << "{\"ok\":false,\"stage\":\"write\",\"error\":\"out_unwritable\"}\n";
      return 6;
    }
  }
  if (windows_path) {
    std::ostringstream lines;
    if (scan.complete)
      for (const auto &m : scan.modules)
        for (const auto base : m.bases) lines << "sub:" << base << ":" << m.size << "\n";
    std::ofstream out(*windows_path, std::ios::binary);
    out << lines.str();
  }
  if (out_path && scan.manifest.empty()) {   // never leave an earlier run's list behind; a module inventory above the list bound has no list text (kModuleMaxCount)
    std::error_code rm_ec;
    std::filesystem::remove(*out_path, rm_ec);
  }
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

// SEG-100-T001 (ADR 0221): `file-survey` - the aggregate, privacy-safe census of the disc's ISO9660 files for file-first broad AOT.
// Counts only (and, with --per-file, one record per file identified by its extent and digest for a local join with execution evidence:
// never a name). `--max-file-bytes` bounds which files are lowered: a larger file is reported `oversize` with its size, never silently dropped.
int file_survey_command(int argc, char **argv) {
  std::optional<std::string> cue, expected;
  std::uint32_t max_bytes = 0x80000U, jobs = 4U;
  bool per_file = false, census = true, structural = false;
  scd::StructuralOptions closure_options;
  std::optional<std::string> probe_path, oracle_path;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--expect-identity" && i + 1 < argc) expected = argv[++i];
    else if (arg == "--max-file-bytes" && i + 1 < argc) {
      const auto v = parse_u32(argv[++i]);
      if (!v) { std::cerr << "usage:\n" << segarecomp_sega_cd_usage; return 2; }
      max_bytes = *v;
    } else if (arg == "--jobs" && i + 1 < argc) {
      const auto v = parse_u32(argv[++i]);
      if (!v || *v == 0U || *v > 64U) { std::cerr << "usage:\n" << segarecomp_sega_cd_usage; return 2; }
      jobs = *v;
    } else if (arg == "--per-file") per_file = true;
    else if (arg == "--no-census") census = false;
    else if (arg == "--structural") structural = true;
    else if (arg == "--structural-open") { structural = true; closure_options.leave_file_is_boundary = true; }   // evaluation only (ADR 0221)
    else if (arg == "--structural-faults") { structural = true; closure_options.faults_are_terminators = true; }   // evaluation only (ADR 0221)
    else if (arg == "--probe-offsets" && i + 1 < argc) probe_path = argv[++i];
    else if (arg == "--oracle" && i + 1 < argc) oracle_path = argv[++i];
    else { std::cerr << "usage:\n" << segarecomp_sega_cd_usage; return 2; }
  }
  if (!cue) { std::cerr << "usage:\n" << segarecomp_sega_cd_usage; return 2; }
  const auto loaded = segarecomp::load_cue_disc(*cue, expected ? std::optional<std::string_view>(*expected) : std::nullopt);
  if (!loaded.disc) {
    std::cout << "{\"ok\":false,\"stage\":\"disc\",\"error\":\"" << segarecomp::disc_error_name(loaded.error) << "\"}\n";
    return 3;
  }
  // SEG-100-T001: `--probe-offsets` is a local file with one line per instruction start an independent execution observed; a line is one or more
  // comma-separated track-1 byte offsets (several when the executed address lies in RAM that more than one candidate source overlaid, so the
  // source is not attributable). Only counts of how the structural policy classifies them are reported; the offsets themselves are never printed.
  std::vector<std::vector<std::uint64_t>> probe_groups;
  std::vector<std::uint64_t> probes;   // the distinct offsets of all groups, sorted
  if (probe_path) {
    std::ifstream in(*probe_path);
    for (std::string line; std::getline(in, line);) {
      std::vector<std::uint64_t> group;
      std::stringstream ss(line);
      for (std::string item; std::getline(ss, item, ',');) {
        if (item.empty()) continue;
        group.push_back(std::strtoull(item.c_str(), nullptr, 10));
        probes.push_back(group.back());
      }
      if (!group.empty()) probe_groups.push_back(std::move(group));
    }
    std::sort(probes.begin(), probes.end());
    probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
  }
  // SEG-100-T001: `--oracle` is a local file of "<sha256> <class-map path>" lines: an independent, source-backed per-byte class map (I instruction
  // start, c continuation, D data, U unknown) for files whose bytes are known by digest. It is an evaluation control for the candidate policy and is never an
  // input of it; only aggregate counts are printed.
  std::map<std::string, std::string> oracle;
  if (oracle_path) {
    std::ifstream in(*oracle_path);
    for (std::string sha, path; in >> sha >> path;) oracle[sha] = path;
  }
  struct OracleCounts {
    std::uint64_t modules = 0, size_mismatch = 0, starts = 0, starts_decodable = 0, starts_valid = 0, valid_total = 0, valid_on_start = 0, valid_on_continuation = 0, valid_on_data = 0, valid_on_unknown = 0;
    std::uint64_t decodable_on_data = 0, min_recall_ppm = 1000000U, modules_full_recall = 0;
    std::uint64_t missed_by_defect[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // true instruction starts outside the closure, by root defect (StructuralClosure::Defect)
  } oc;
  std::mutex probe_mutex;
  std::unordered_map<std::uint64_t, std::uint8_t> probe_flags;   // offset -> bit0 in a lowered file, bit1 decodes, bit2 closure-valid, bit3 odd
  const auto list = scd::enumerate_file_candidates(*loaded.disc);
  if (!list.complete()) {
    std::cout << "{\"ok\":false,\"stage\":\"enumerate\",\"error\":\"" << scd::file_candidate_error_name(list.error) << "\",\"iso9660\":\""
              << segarecomp::iso9660_error_name(list.iso_error) << "\"}\n";
    return 4;
  }
  struct Record {
    bool oversize{}, unreadable{};
    std::string sha256;
    scd::BroadCensus census;
    std::uint64_t decodable{}, closure_valid{};
  };
  std::vector<Record> records(list.files.size());
  std::atomic<std::size_t> next{0};
  std::mutex unique_mutex;
  std::unordered_map<std::string, std::uint32_t> unique_bodies;  // instruction bytes -> text bytes of its body (sharing across offsets and files)
  const auto work = [&]() {
    for (std::size_t i = next++; i < list.files.size(); i = next++) {
      auto &r = records[i];
      const auto &file = list.files[i];
      if (file.extent.size > max_bytes) { r.oversize = true; continue; }
      const auto bytes = scd::read_file_candidate(*loaded.disc, file.extent);
      if (bytes.size() != file.extent.size) { r.unreadable = true; continue; }
      r.sha256 = segarecomp::sha256_hex(bytes);
      if (census) {
        std::unordered_map<std::string, std::uint32_t> local;
        scd::StructuralClosure closure;
        if (structural) {
          closure = scd::structural_closure(bytes, closure_options);
          r.decodable = closure.decodable;
          r.closure_valid = closure.valid_offsets;
          if (const auto it = oracle.find(r.sha256); it != oracle.end()) {
            std::ifstream cin_(it->second, std::ios::binary);
            const std::string classes((std::istreambuf_iterator<char>(cin_)), std::istreambuf_iterator<char>());
            const std::lock_guard<std::mutex> lock(probe_mutex);
            if (classes.size() != bytes.size()) {
              ++oc.size_mismatch;
            } else {
              ++oc.modules;
              std::uint64_t starts = 0, starts_valid = 0;
              for (std::size_t o = 0; o < classes.size(); ++o) {
                const char cls = classes[o];
                const bool even = (o & 1U) == 0U;
                const bool valid = even && o / 2U < closure.valid.size() && closure.valid[o / 2U] != 0U;
                const bool decoded = even && o / 2U < closure.decoded.size() && closure.decoded[o / 2U] != 0U;
                if (cls == 'I') {
                  ++starts;
                  oc.starts_decodable += decoded ? 1U : 0U;
                  if (valid) ++starts_valid;
                  else if (even && o / 2U < closure.defect.size()) ++oc.missed_by_defect[closure.defect[o / 2U] & 7U];
                }
                if (valid) {
                  ++oc.valid_total;
                  if (cls == 'I') ++oc.valid_on_start; else if (cls == 'c') ++oc.valid_on_continuation; else if (cls == 'D') ++oc.valid_on_data; else ++oc.valid_on_unknown;
                } else if (decoded && cls == 'D') {
                  ++oc.decodable_on_data;
                }
              }
              oc.starts += starts;
              oc.starts_valid += starts_valid;
              if (starts != 0U) oc.min_recall_ppm = std::min<std::uint64_t>(oc.min_recall_ppm, starts_valid * 1000000U / starts);
              if (starts_valid == starts) ++oc.modules_full_recall;
            }
          }
          const std::uint64_t lo = static_cast<std::uint64_t>(file.extent.block) * 2048U, hi = lo + file.extent.size;
          std::vector<std::pair<std::uint64_t, std::uint8_t>> local_flags;
          for (auto it = std::lower_bound(probes.begin(), probes.end(), lo); it != probes.end() && *it < hi; ++it) {
            const std::uint64_t rel = *it - lo;
            std::uint8_t flags = 1U;
            if ((rel & 1U) != 0U || rel / 2U >= closure.valid.size()) flags |= 8U;
            else flags = static_cast<std::uint8_t>(flags | (closure.decoded[rel / 2U] ? 2U : 0U) | (closure.valid[rel / 2U] ? 4U : 0U) | ((closure.defect[rel / 2U] & 7U) << 4U));
            local_flags.emplace_back(*it, flags);
          }
          if (!local_flags.empty()) {
            const std::lock_guard<std::mutex> lock(probe_mutex);
            for (const auto &[offset, flags] : local_flags) probe_flags[offset] = flags;
          }
        }
        r.census = scd::broad_census(bytes, [&](std::uint32_t, const scd::DynamicBody &b) {
          local.emplace(std::string(b.raw.begin(), b.raw.end()),
                        static_cast<std::uint32_t>(b.body.size() + b.slots.size() + b.timing.size() + b.refresh_skip.size()));
        }, structural ? std::span<const std::uint8_t>(closure.valid) : std::span<const std::uint8_t>());
        const std::lock_guard<std::mutex> lock(unique_mutex);
        unique_bodies.merge(local);
      }
    }
  };
  std::vector<std::thread> threads;
  for (std::uint32_t t = 0; t < jobs; ++t) threads.emplace_back(work);
  for (auto &t : threads) t.join();
  std::ostringstream out;
  scd::BroadCensus sum;
  std::uint64_t total_decodable = 0, total_closure_valid = 0;
  std::uint64_t oversize_files = 0, oversize_bytes = 0, lowered_files = 0, lowered_bytes = 0, total_bytes = 0, unreadable = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto &r = records[i];
    total_bytes += list.files[i].extent.size;
    if (r.oversize) { ++oversize_files; oversize_bytes += list.files[i].extent.size; continue; }
    if (r.unreadable) { ++unreadable; continue; }
    ++lowered_files;
    total_decodable += r.decodable;
    total_closure_valid += r.closure_valid;
    lowered_bytes += list.files[i].extent.size;
    sum.even_offsets += r.census.even_offsets; sum.not_decoded += r.census.not_decoded; sum.not_lifted += r.census.not_lifted;
    sum.not_lowered += r.census.not_lowered; sum.redecode += r.census.redecode;
    sum.refused_token_structure += r.census.refused_token_structure; sum.refused_literal_not_linear += r.census.refused_literal_not_linear;
    sum.refused_literal_suffix += r.census.refused_literal_suffix; sum.refused_constant_context += r.census.refused_constant_context;
    sum.compiled += r.census.compiled; sum.pc_dependent += r.census.pc_dependent; sum.generated_text_bytes += r.census.generated_text_bytes;
  }
  std::uint64_t pg_missed_defect[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::uint64_t pg_total = 0, pg_single = 0, pg_multi = 0, pg_in_file = 0, pg_decoded_all = 0, pg_valid_all = 0, pg_valid_any = 0, pg_multi_valid_all = 0, pg_multi_valid_any = 0, pg_single_valid = 0;
  for (const auto &group : probe_groups) {
    ++pg_total;
    bool in_file_any = false, all_decoded = true, all_valid = true, any_valid = false;
    for (const auto offset : group) {
      const auto it = probe_flags.find(offset);
      const std::uint8_t f = it == probe_flags.end() ? 0U : it->second;
      in_file_any = in_file_any || (f & 1U) != 0U;
      all_decoded = all_decoded && (f & 2U) != 0U;
      all_valid = all_valid && (f & 4U) != 0U;
      any_valid = any_valid || (f & 4U) != 0U;
    }
    (group.size() == 1U ? pg_single : pg_multi) += 1U;
    if (!in_file_any) continue;
    ++pg_in_file;
    pg_decoded_all += all_decoded ? 1U : 0U;
    pg_valid_all += all_valid ? 1U : 0U;
    pg_valid_any += any_valid ? 1U : 0U;
    if (group.size() == 1U) {
      pg_single_valid += all_valid ? 1U : 0U;
      if (!all_valid) {
        const auto it = probe_flags.find(group.front());
        if (it != probe_flags.end()) ++pg_missed_defect[(it->second >> 4U) & 7U];
      }
    }
    else { pg_multi_valid_all += all_valid ? 1U : 0U; pg_multi_valid_any += any_valid ? 1U : 0U; }
  }
  out << "{\"ok\":true,\"identity\":\"" << loaded.disc->identity() << "\",\"files\":" << list.files.size() << ",\"directories\":" << list.directories
      << ",\"file_bytes\":" << total_bytes << ",\"max_file_bytes\":" << max_bytes << ",\"oversize_files\":" << oversize_files
      << ",\"oversize_bytes\":" << oversize_bytes << ",\"unreadable_files\":" << unreadable << ",\"lowered_files\":" << lowered_files
      << ",\"lowered_bytes\":" << lowered_bytes << ",\"census\":" << (census ? "true" : "false") << ",\"structural\":" << (structural ? "true" : "false")
      << ",\"decodable_offsets\":" << total_decodable << ",\"closure_valid_offsets\":" << total_closure_valid
      << ",\"oracle\":{\"modules\":" << oc.modules << ",\"size_mismatch\":" << oc.size_mismatch << ",\"instruction_starts\":" << oc.starts << ",\"starts_decoded\":" << oc.starts_decodable
      << ",\"starts_in_closure\":" << oc.starts_valid << ",\"modules_with_full_recall\":" << oc.modules_full_recall << ",\"min_module_recall_ppm\":" << (oc.modules ? oc.min_recall_ppm : 0U)
      << ",\"closure_valid_total\":" << oc.valid_total << ",\"closure_valid_on_start\":" << oc.valid_on_start << ",\"closure_valid_on_continuation\":" << oc.valid_on_continuation
      << ",\"closure_valid_on_data\":" << oc.valid_on_data << ",\"closure_valid_on_unknown\":" << oc.valid_on_unknown << ",\"decodable_not_valid_on_data\":" << oc.decodable_on_data << ",\"missed_starts_by_defect\":{\"not_decoded\":" << oc.missed_by_defect[1]
      << ",\"always_faults\":" << oc.missed_by_defect[2] << ",\"not_understood\":" << oc.missed_by_defect[3] << ",\"target_outside_file\":" << oc.missed_by_defect[4]
      << ",\"target_odd\":" << oc.missed_by_defect[5] << ",\"fall_off_file\":" << oc.missed_by_defect[6] << "}}"
      << ",\"probes\":{\"groups\":" << pg_total << ",\"attributable\":" << pg_single << ",\"overlaid\":" << pg_multi << ",\"in_lowered_files\":" << pg_in_file
      << ",\"decoded_in_every_candidate\":" << pg_decoded_all << ",\"closure_valid_in_every_candidate\":" << pg_valid_all
      << ",\"closure_valid_in_some_candidate\":" << pg_valid_any << ",\"attributable_closure_valid\":" << pg_single_valid
      << ",\"overlaid_closure_valid_in_every_candidate\":" << pg_multi_valid_all << ",\"overlaid_closure_valid_in_some_candidate\":" << pg_multi_valid_any << ",\"attributable_missed_by_defect\":{\"not_decoded\":" << pg_missed_defect[1]
      << ",\"always_faults\":" << pg_missed_defect[2] << ",\"not_understood\":" << pg_missed_defect[3] << ",\"target_outside_file\":" << pg_missed_defect[4]
      << ",\"target_odd\":" << pg_missed_defect[5] << ",\"fall_off_file\":" << pg_missed_defect[6] << "}}"
      << ",\"totals\":{\"even_offsets\":" << sum.even_offsets
      << ",\"compiled\":" << sum.compiled << ",\"pc_dependent\":" << sum.pc_dependent << ",\"not_decoded\":" << sum.not_decoded
      << ",\"not_lifted\":" << sum.not_lifted << ",\"not_lowered\":" << sum.not_lowered << ",\"redecode\":" << sum.redecode
      << ",\"refused_token_structure\":" << sum.refused_token_structure << ",\"refused_literal_not_linear\":" << sum.refused_literal_not_linear
      << ",\"refused_literal_suffix\":" << sum.refused_literal_suffix << ",\"refused_constant_context\":" << sum.refused_constant_context
      << ",\"generated_text_bytes\":" << sum.generated_text_bytes << "}";
  {
    std::uint64_t unique_text = 0;
    for (const auto &[raw, text] : unique_bodies) unique_text += text;
    out << ",\"unique_encodings\":" << unique_bodies.size() << ",\"unique_text_bytes\":" << unique_text;
  }
  if (per_file) {
    out << ",\"records\":[";
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto &r = records[i];
      out << (i ? "," : "") << "{\"block\":" << list.files[i].extent.block << ",\"size\":" << list.files[i].extent.size << ",\"oversize\":" << (r.oversize ? 1 : 0)
          << ",\"sha256\":\"" << r.sha256 << "\",\"even\":" << r.census.even_offsets << ",\"compiled\":" << r.census.compiled << ",\"decodable\":" << r.decodable << ",\"valid\":" << r.closure_valid
          << ",\"pc_dependent\":" << r.census.pc_dependent << ",\"not_decoded\":" << r.census.not_decoded << ",\"refused\":"
          << (r.census.refused_token_structure + r.census.refused_literal_not_linear + r.census.refused_literal_suffix + r.census.refused_constant_context + r.census.redecode + r.census.not_lifted + r.census.not_lowered)
          << "}";
    }
    out << "]";
  }
  out << "}\n";
  std::cout << out.str();
  return 0;
}

}  // namespace

int segarecomp_sega_cd_command(int argc, char **argv) {
  if (argc >= 3 && std::string_view(argv[2]) == "inspect") return inspect(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "emit") return emit(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "build") return segarecomp_sega_cd_build(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "resolve-source") return resolve_source_command(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "file-survey") return file_survey_command(argc, argv);
  if (argc >= 3 && std::string_view(argv[2]) == "discover-modules") return discover_modules_command(argc, argv);
  std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
  return 2;
}
