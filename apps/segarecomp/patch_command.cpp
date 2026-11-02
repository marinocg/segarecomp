// `segarecomp patch inspect|verify|apply`: the CLI seam of the portable patch package (ADR 0266). One JSON object on stdout; the launcher
// links the same libs/media implementation, so there is a single validation path.
#include "segarecomp/patch.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string esc(const std::string &s) {
  std::string o;
  for (const char c : s) { if (c == '"' || c == '\\') o.push_back('\\'); if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c); }
  return o;
}

void info_fields(const segarecomp::patch::PackageInfo &i) {
  std::cout << "\"schema_version\":" << i.schema_version << ",\"has_rom\":" << (i.has_rom ? "true" : "false") << ",\"has_native\":" << (i.has_native ? "true" : "false")
            << ",\"native_capability\":\"" << esc(i.native_capability) << "\",\"native_components\":" << i.native_components << ",\"native_implementation\":" << i.native_implementation
            << ",\"native_descriptor_sha256\":\"" << i.native_payload_sha256 << "\",\"native_width_max\":" << i.native_width_max << ",";
  std::cout << "\"id\":\"" << esc(i.id) << "\",\"version\":\"" << esc(i.version) << "\",\"name\":\"" << esc(i.name) << "\",\"platform\":\"" << i.platform
            << "\",\"purpose\":\"" << i.purpose << "\",\"source_sha256\":\"" << i.source_sha256 << "\",\"target_sha256\":\"" << i.target_sha256
            << "\",\"payload_sha256\":\"" << i.rom_payload_sha256 << "\",\"widescreen_width_max\":" << i.widescreen_width_max << ",\"default_widescreen\":\"" << i.default_widescreen << "\",\"default_side_style\":\"" << i.default_side_style << "\""
            << ",\"requires\":[";
  for (std::size_t n = 0; n < i.requires_capabilities.size(); ++n) std::cout << (n ? "," : "") << "\"" << esc(i.requires_capabilities[n]) << "\"";
  std::cout << "],\"package_identity\":\"" << i.identity << "\"";
}

}  // namespace

int segarecomp_patch_command(int argc, char **argv) {
  const std::string_view sub = argc >= 3 ? argv[2] : "";
  if ((sub == "inspect" && argc == 4)) {
    const auto pkg = segarecomp::patch::read_package_file(argv[3]);
    if (!pkg.ok) { std::cout << "{\"ok\":false,\"error\":\"" << esc(pkg.error) << "\"}\n"; return 1; }
    std::cout << "{\"ok\":true,";
    info_fields(pkg.info);
    std::cout << "}\n";
    return 0;
  }
  if ((sub == "verify" || sub == "apply") && argc >= 6) {
    std::filesystem::path rom, out;
    for (int i = 4; i + 1 < argc; i += 2) {
      const std::string_view o = argv[i];
      if (o == "--rom") rom = argv[i + 1];
      else if (o == "--out" && sub == "apply") out = argv[i + 1];
      else { rom.clear(); break; }
    }
    if (!rom.empty() && (sub == "verify" || !out.empty())) {
      const auto r = segarecomp::patch::apply_package_file(argv[3], rom, out);
      if (!r.ok) { std::cout << "{\"ok\":false,\"error\":\"" << esc(r.error) << "\"}\n"; return 1; }
      std::cout << "{\"ok\":true,";
      info_fields(r.info);
      std::cout << ",\"rom_sha256\":\"" << r.rom_sha256 << "\"}\n";
      return 0;
    }
  }
  std::cerr << "usage: segarecomp patch inspect <package>\n       segarecomp patch verify <package> --rom <image>\n       segarecomp patch apply <package> --rom <image> --out <file>\n";
  return 2;
}
