#include "sega_cd_command.hpp"

#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"
#include "segarecomp/sha256.hpp"

namespace {

namespace scd = segarecomp::machine::sega_cd;

std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20U) {
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

int inspect(int argc, char **argv) {
  std::optional<std::string> cue, expected;
  bool sector_digests = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--cue" && i + 1 < argc) cue = argv[++i];
    else if (arg == "--expect-identity" && i + 1 < argc) expected = argv[++i];
    else if (arg == "--sector-digests") sector_digests = true;
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
        << ",\"pregap\":" << track.pregap << ",\"first_lba\":" << track.first_lba << ",\"sector_count\":" << track.sector_count << "}";
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
  const auto produced = scd::produce_boot_images(disc);
  if (!produced.images) {
    out << ",\"boot\":{\"ok\":false,\"error\":\"" << scd::boot_image_error_name(produced.error) << "\",\"boot_error\":\""
        << segarecomp::sega_cd_boot_error_name(produced.boot_error) << "\",\"validation\":\""
        << segarecomp::image_validation_error_name(produced.validation) << "\"}}\n";
    std::cout << out.str();
    return 4;
  }
  const auto &images = *produced.images;
  const auto &h = images.header;
  out << ",\"boot\":{\"ok\":true,\"system_id\":" << json_string(h.system_id) << ",\"volume_name\":" << json_string(h.volume_name)
      << ",\"system_name\":" << json_string(h.system_name) << ",\"hardware_id\":" << json_string(h.hardware_id)
      << ",\"product_code\":" << json_string(h.product_code) << ",\"region\":" << json_string(h.region)
      << ",\"ip\":" << descriptor_json(h.ip) << ",\"sp\":" << descriptor_json(h.sp) << ",\"images\":["
      << processor_json(images.main) << "," << processor_json(images.sub) << "]}}\n";
  std::cout << out.str();
  return 0;
}

}  // namespace

int segarecomp_sega_cd_command(int argc, char **argv) {
  if (argc >= 3 && std::string_view(argv[2]) == "inspect") return inspect(argc, argv);
  std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
  return 2;
}
