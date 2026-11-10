#include "segarecomp/patch.hpp"

#include "patch_archive.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <utility>
#include <string_view>

namespace segarecomp::patch {

namespace {

// ZIP, DEFLATE and the closed-JSON reader live in patch_archive.cpp (shared with the schema 3 reader).
using namespace detail;

bool read_zip_legacy(std::span<const std::uint8_t> z, std::map<std::string, ZipEntry> &entries, std::string &error) {
  ZipPolicy policy;
  policy.name_ok = safe_name;
  return read_zip(z, policy, entries, error);
}


unsigned capability_bit(const std::string &name) {
  if (name == "frame.work_ram") return kCapFrameWorkRam;
  if (name == "frame.vdp") return kCapFrameVdp;
  if (name == "scenery.words") return kCapSceneryWords;
  if (name == "overlay.rgba") return kCapOverlayRgba;
  return 0U;
}

Package bad(std::string why) {
  Package p;
  p.error = std::move(why);
  return p;
}

}  // namespace

unsigned native_capability_bit(const std::string &name) { return capability_bit(name); }

Package read_package(std::span<const std::uint8_t> zip) {
  std::map<std::string, ZipEntry> entries;
  std::string error;
  if (!read_zip_legacy(zip, entries, error)) return bad(error);
  const auto man = entries.find("manifest.json");
  if (man == entries.end()) return bad("the package has no manifest.json");
  Value root;
  std::string json_error;
  if (!parse_json(std::string_view(reinterpret_cast<const char *>(man->second.data.data()), man->second.data.size()), JsonLimits{}, root, json_error) || root.kind != Value::Kind::object)
    return bad(json_error.empty() ? "the manifest is not valid JSON" : json_error);
  static const char *const required[] = {"format", "schema_version", "id", "version", "name", "platform", "purpose", "source_sha256", "payloads"};
  static const char *const optional[] = {"requires", "widescreen_width_max", "presentation_defaults", "author", "license"};
  for (const char *k : required) if (root.find(k) == nullptr) return bad(std::string("the manifest is missing \"") + k + "\"");
  for (const std::string &k : root.keys) {
    bool known = false;
    for (const char *r : required) known = known || k == r;
    for (const char *o : optional) known = known || k == o;
    if (!known) return bad("the manifest has an unknown field \"" + k + "\"");
  }
  PackageInfo info;
  std::string format;
  if (!take_string(root, "format", format) || format != "segarecomp-patch") return bad("the manifest is not a segarecomp patch manifest");
  const Value &ver = root.at("schema_version");
  if (ver.kind != Value::Kind::number) return bad("the manifest schema_version is invalid");
  if (ver.number != 1 && ver.number != 2) return bad("unsupported patch package schema version " + std::to_string(ver.number));
  info.schema_version = static_cast<int>(ver.number);
  if (!take_string(root, "id", info.id) || !token(info.id, 64, false)) return bad("the manifest id is invalid");
  if (!take_string(root, "version", info.version) || !token(info.version, 32, true)) return bad("the manifest version is invalid");
  if (!take_string(root, "name", info.name) || info.name.empty() || info.name.size() > 64) return bad("the manifest name is invalid");
  if (!take_string(root, "platform", info.platform) || info.platform != "genesis") return bad("the manifest platform is not supported");
  if (!take_string(root, "purpose", info.purpose) || info.purpose != "widescreen") return bad("the manifest purpose is not supported");
  if (!take_string(root, "source_sha256", info.source_sha256) || !is_hex64(info.source_sha256)) return bad("the manifest source_sha256 is invalid");
  if (const Value *w = root.find("widescreen_width_max"); w != nullptr) {
    if (w->kind != Value::Kind::number || w->number > 100) return bad("the manifest widescreen_width_max must be an integer from 0 to 100");
    info.widescreen_width_max = static_cast<int>(w->number);
  }
  if (const Value *d = root.find("presentation_defaults"); d != nullptr) {
    if (d->kind != Value::Kind::object || d->members.empty() || d->members.size() > 2) return bad("the manifest presentation_defaults are invalid");
    for (const std::string &k : d->keys) if (k != "widescreen" && k != "side_style") return bad("the manifest presentation_defaults have an unknown field \"" + k + "\"");
    if (d->find("widescreen") != nullptr) {
      if (!take_string(*d, "widescreen", info.default_widescreen) || (info.default_widescreen != "original" && info.default_widescreen != "extended_backgrounds" && info.default_widescreen != "enhanced_gameplay"))
        return bad("the manifest presentation_defaults widescreen value is not supported");
    }
    if (d->find("side_style") != nullptr) {
      if (!take_string(*d, "side_style", info.default_side_style) || (info.default_side_style != "comfort" && info.default_side_style != "borders" && info.default_side_style != "raw"))
        return bad("the manifest presentation_defaults side_style value is not supported");
    }
  }
  if (const Value *r = root.find("requires"); r != nullptr) {   // no capability is named here (ADR 0267): a provider's needs live in its `native` payload; any entry is refused, as in schema 1
    if (r->kind != Value::Kind::array || r->items.size() > 8) return bad("the manifest requires list is invalid");
    if (!r->items.empty()) return bad("this segarecomp does not provide the required capability \"" + (r->items[0].kind == Value::Kind::string ? r->items[0].text : std::string("?")) + "\"");
  }
  if (info.schema_version < 2 && (root.find("author") != nullptr || root.find("license") != nullptr)) return bad("the manifest has an unknown field (author and license need schema 2)");
  if (const Value *au = root.find("author"); au != nullptr) {
    if (au->kind != Value::Kind::string || au->text.empty() || au->text.size() > 64 || !printable(au->text)) return bad("the manifest author is invalid");
    info.author = au->text;
  }
  if (const Value *li = root.find("license"); li != nullptr) {
    if (li->kind != Value::Kind::string || !token(li->text, 32, true)) return bad("the manifest license is invalid");
    info.license = li->text;
  }
  const Value &payloads = root.at("payloads");
  if (payloads.kind != Value::Kind::object || payloads.members.empty()) return bad("the manifest payloads are invalid");
  for (const std::string &name : payloads.keys) {
    if (name != "rom" && !(name == "native" && info.schema_version >= 2)) return bad("the manifest names an unsupported payload type \"" + name + "\"");
  }
  std::size_t expected_entries = 1;
  if (const Value *rom = payloads.find("rom"); rom != nullptr) {
    if (rom->kind != Value::Kind::object || !only_keys(*rom, {"format", "file", "sha256", "target_sha256"})) return bad("the rom payload is invalid");
    std::string pformat;
    if (!take_string(*rom, "format", pformat) || pformat != "bps") return bad("the rom payload format is not supported");
    if (!take_string(*rom, "file", info.rom_file) || !safe_name(info.rom_file) || info.rom_file == "manifest.json") return bad("the rom payload file name is invalid");
    if (!take_string(*rom, "sha256", info.rom_payload_sha256) || !is_hex64(info.rom_payload_sha256)) return bad("the rom payload sha256 is invalid");
    if (!take_string(*rom, "target_sha256", info.target_sha256) || !is_hex64(info.target_sha256)) return bad("the rom payload target_sha256 is invalid");
    if (info.source_sha256 == info.target_sha256) return bad("the manifest source and target are identical");
    info.has_rom = true;
    ++expected_entries;
  }
  if (const Value *nat = payloads.find("native"); nat != nullptr) {
    if (nat->kind != Value::Kind::object || !only_keys(*nat, {"format", "file", "sha256", "abi", "capabilities", "executable"})) return bad("the native payload is invalid");
    std::string pformat, executable;
    if (!take_string(*nat, "format", pformat) || pformat != "c11-source") return bad("the native payload format is not supported");
    if (!take_string(*nat, "executable", executable) || executable != "yes") return bad("a native payload must declare that it contains executable source (\"executable\": \"yes\")");
    if (!take_string(*nat, "file", info.native_file) || info.native_file != kProviderFileName || (info.has_rom && info.native_file == info.rom_file)) return bad("the native payload file name must be provider.c");
    if (!take_string(*nat, "sha256", info.native_sha256) || !is_hex64(info.native_sha256)) return bad("the native payload sha256 is invalid");
    const Value &abi = nat->at("abi");
    if (abi.kind != Value::Kind::number || abi.number != kNativeAbiVersion) return bad("this segarecomp does not provide native patch ABI version " + (abi.kind == Value::Kind::number ? std::to_string(abi.number) : std::string("?")));
    info.native_abi = static_cast<unsigned>(abi.number);
    const Value &caps = nat->at("capabilities");
    if (caps.kind != Value::Kind::array || caps.items.empty() || caps.items.size() > 4) return bad("the native payload capabilities are invalid");
    for (const Value &c : caps.items) {
      const unsigned bit = c.kind == Value::Kind::string ? capability_bit(c.text) : 0U;
      if (bit == 0U) return bad("this segarecomp does not provide the capability \"" + (c.kind == Value::Kind::string ? c.text : std::string("?")) + "\"");
      if ((info.native_caps & bit) != 0U) return bad("the native payload repeats a capability");
      info.native_caps |= bit;
    }
    if ((info.native_caps & (kCapSceneryWords | kCapOverlayRgba)) == 0U) return bad("the native payload contributes nothing (it needs scenery.words or overlay.rgba)");
    if (info.author.empty()) return bad("a package with executable source must name its author");
    info.has_native = true;
    ++expected_entries;
  }
  if (!info.has_rom && !info.has_native) return bad("the manifest payloads are invalid");
  if (entries.size() != expected_entries) return bad("the package must contain exactly manifest.json and the declared payloads");
  Package out;
  if (info.has_rom) {
    const auto pit = entries.find(info.rom_file);
    if (pit == entries.end()) return bad("the payload named by the manifest is missing from the package");
    if (pit->second.data.size() > kMaxPatchBytes || sha256_hex(pit->second.data) != info.rom_payload_sha256) return bad("the payload SHA-256 does not match the manifest");
    out.payload = std::move(pit->second.data);
  }
  if (info.has_native) {
    const auto nit = entries.find(info.native_file);
    if (nit == entries.end()) return bad("the native payload named by the manifest is missing from the package");
    const std::vector<std::uint8_t> &src = nit->second.data;
    if (src.empty() || src.size() > kMaxProviderSourceBytes) return bad("the provider source is empty or larger than allowed");
    if (std::find(src.begin(), src.end(), static_cast<std::uint8_t>(0)) != src.end()) return bad("the provider source contains a NUL byte");
    if (sha256_hex(src) != info.native_sha256) return bad("the provider source SHA-256 does not match the manifest");
    out.native = std::move(nit->second.data);
  }
  info.identity = sha256_hex(man->second.data);
  out.ok = true;
  out.info = std::move(info);
  return out;
}

Applied apply_package(const Package &package, std::span<const std::uint8_t> rom) {
  Applied a;
  if (!package.ok) { a.error = "the package was not verified"; return a; }
  if (rom.size() > kMaxImageBytes) { a.error = "the original image is too large"; return a; }
  if (sha256_hex(rom) != package.info.source_sha256) { a.error = "the original image is not the exact image this patch was made for"; return a; }
  if (!package.info.has_rom) {   // native-only: the original image is the effective image, byte for byte
    a.ok = true;
    a.image.assign(rom.begin(), rom.end());
    return a;
  }
  BpsResult r = apply_bps(package.payload, rom);
  if (!r.ok) { a.error = r.error; return a; }
  if (sha256_hex(r.output) != package.info.target_sha256) { a.error = "the patched image does not match the manifest target SHA-256"; return a; }
  a.ok = true;
  a.image = std::move(r.output);
  return a;
}

namespace {
bool read_bounded(const std::filesystem::path &path, std::size_t limit, std::vector<std::uint8_t> &bytes, std::string &error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) { error = "the file is not a regular file"; return false; }   // a user-chosen input may be reached through a link; the bytes read are what is verified
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) { error = "the file cannot be read"; return false; }
  if (size > limit) { error = "the file is larger than allowed"; return false; }
  bytes.resize(static_cast<std::size_t>(size));
  std::ifstream in(path, std::ios::binary);
  if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) { error = "the file cannot be read"; return false; }
  return true;
}
}  // namespace

Package read_package_file(const std::filesystem::path &package) {
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_bounded(package, kMaxPackageBytes, bytes, error)) return bad("the package " + error);
  return read_package(bytes);
}

FileApplied apply_package_file(const std::filesystem::path &package, const std::filesystem::path &rom, const std::filesystem::path &out) {
  FileApplied r;
  Package pkg = read_package_file(package);
  if (!pkg.ok) { r.error = pkg.error; return r; }
  r.info = pkg.info;
  std::vector<std::uint8_t> rom_bytes;
  std::string error;
  if (!read_bounded(rom, kMaxImageBytes, rom_bytes, error)) { r.error = "the original image: " + error; return r; }
  r.rom_sha256 = sha256_hex(rom_bytes);
  Applied applied = apply_package(pkg, rom_bytes);
  if (!applied.ok) { r.error = applied.error; return r; }
  r.target_sha256 = pkg.info.has_rom ? pkg.info.target_sha256 : r.rom_sha256;
  if (!out.empty()) {
    std::error_code ec;
    if (out == rom || std::filesystem::equivalent(out, rom, ec)) { r.error = "the output would overwrite the original image"; return r; }
    ec.clear();
    std::filesystem::path tmp = out;
    tmp += ".partial";   // path-level concatenation: no narrow-string round trip of a non-ASCII path
    std::filesystem::remove(tmp, ec);
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(applied.image.data()), static_cast<std::streamsize>(applied.image.size()));
      f.flush();
      if (!f) { f.close(); std::filesystem::remove(tmp, ec); r.error = "the patched image cannot be written"; return r; }
    }
    std::filesystem::rename(tmp, out, ec);
    if (ec) { std::filesystem::remove(tmp, ec); r.error = "the patched image cannot be published"; return r; }
  }
  r.ok = true;
  return r;
}

}  // namespace segarecomp::patch
