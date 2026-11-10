// `segarecomp mod init|check|pack|build|inspect|install|variant|convert-audio`: the CLI seam of the modding platform (ADR 0280/0281/0283/0286). One JSON object on stdout; the launcher links the same libs/media
// implementation. `check` and `build` never execute a payload; `build` runs the HOST's C compiler (flags owned by the host, kNativeCompileFlags) over the package's own C11 units only.
#include "process_run.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace segarecomp;

std::string esc(const std::string &s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
    else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) o.push_back(c);
    else o.push_back('?');
  }
  return o;
}

int refuse(const mod::Rejection &r) {
  std::cout << "{\"ok\":false,\"class\":\"" << esc(r.code) << "\",\"error\":\"" << esc(r.message) << "\"}\n";
  return 1;
}

std::string json_list(const std::vector<std::string> &v) {
  std::string o = "[";
  for (std::size_t i = 0; i < v.size(); ++i) o += (i ? ",\"" : "\"") + esc(v[i]) + "\"";
  return o + "]";
}

std::string capability_report(const mod::Adapter &a) {
  std::string o = "{";
  bool first = true;
  for (const mod::CapabilityInfo &c : mod::capability_catalogue()) {
    o += std::string(first ? "\"" : ",\"") + std::string(c.name) + "\":\"" + mod::support_name(mod::capability_support(a, c)) + "\"";
    first = false;
  }
  return o + "}";
}

std::string adapter_json(const mod::Adapter &a) {
  return "{\"platform\":\"" + std::string(a.platform) + "\",\"source_identity_kind\":\"" + std::string(a.source_kind) + "\",\"sdk_header\":\"" + std::string(a.sdk_header) + "\",\"capabilities\":" + capability_report(a) + "}";
}

std::string package_json(const mod::Manifest &m, const std::string &identity, const std::string &content) {
  std::size_t native = 0, luau = 0, assets = 0, other = 0;
  for (const mod::Payload &p : m.payloads) (p.kind == "native-c11" ? native : p.kind == "luau-source" ? luau : p.kind == "asset" ? assets : other)++;
  std::string revs = "[";
  for (std::size_t i = 0; i < m.revisions.size(); ++i) revs += std::string(i ? "," : "") + "{\"label\":\"" + esc(m.revisions[i].label) + "\",\"sha256\":\"" + m.revisions[i].sha256 + "\"}";
  revs += "]";
  return "\"id\":\"" + esc(m.id) + "\",\"version\":\"" + esc(m.version) + "\",\"name\":\"" + esc(m.name) + "\",\"platform\":\"" + m.platform + "\",\"source_identity_kind\":\"" + m.source_kind +
         "\",\"revisions\":" + revs + ",\"trust_class\":\"" + mod::trust_class(m) + "\",\"capabilities\":" + json_list(m.capabilities) + ",\"capabilities_optional\":" + json_list(m.capabilities_optional) +
         ",\"abi_native\":" + std::to_string(m.abi_native) + ",\"payloads\":{\"native\":" + std::to_string(native) + ",\"luau\":" + std::to_string(luau) + ",\"assets\":" + std::to_string(assets) +
         ",\"other\":" + std::to_string(other) + "},\"package_identity\":\"" + identity + "\",\"content_digest\":\"" + content + "\"";
}

bool read_all(const fs::path &path, std::size_t limit, std::vector<std::uint8_t> &out) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return false;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  out.resize(static_cast<std::size_t>(size));
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size())));
}

bool write_atomic(const fs::path &out, std::span<const std::uint8_t> bytes) {
  fs::path tmp = out;
  tmp += ".partial";
  std::error_code ec;
  fs::remove(tmp, ec);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) { f.close(); fs::remove(tmp, ec); return false; }
  }
  fs::rename(tmp, out, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

// Compiles every .c unit of a native package in path order with host-owned flags against the ABI 2 SDK headers. Compile-only: nothing is linked or run here.
int compile_natives(const mod::Package &pkg, const std::string &cc, const fs::path &sdk_root, std::string &class_out, std::string &detail) {
  const mod::Manifest &m = pkg.manifest;
  const mod::Adapter &adapter = *mod::find_adapter(m.platform);
  std::random_device entropy;   // a private scratch folder; its name never reaches an output
  char suffix[17];
  std::snprintf(suffix, sizeof suffix, "%08x%08x", entropy(), entropy());
  const std::string stem = "segarecomp-mod-" + std::string(suffix);
  const fs::path work = fs::temp_directory_path() / stem;
  std::error_code ec;
  fs::remove_all(work, ec);
  for (const auto &[path, bytes] : pkg.files) {
    const mod::Payload *p = nullptr;
    for (const mod::Payload &q : m.payloads) if (q.path == path) p = &q;
    if (p == nullptr || p->kind != "native-c11") continue;
    const fs::path at = work / "src" / path;
    fs::create_directories(at.parent_path(), ec);
    std::ofstream f(at, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) { class_out = "E_COMPILE_FAILED"; detail = "the sources could not be staged"; fs::remove_all(work, ec); return 1; }
  }
  int rc = 0;
  std::vector<std::string> objects;
  std::vector<std::string> base = {cc};
  for (const std::string_view flag : mod::kNativeCompileFlags) base.emplace_back(flag);
  base.push_back("-DSEGA_MOD_TARGET_PLATFORM=" + std::to_string(adapter.platform_id));
  base.push_back("-DSEGA_MOD_TARGET_ABI_MAJOR=" + std::to_string(m.abi_native) + "U");
  for (const fs::path &inc : {sdk_root / "sdk", sdk_root / std::string(m.platform) / "sdk", work / "src"}) { base.push_back("-I"); base.push_back(inc.string()); }
  const fs::path log = work / "compile.log";
  const auto fail_with = [&](const std::string &where, bool link) {
    std::ifstream in(log);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto has = [&](const char *needle) { return text.find(needle) != std::string::npos; };
    // The entry is missing only when the linker reports THAT symbol as undefined (GNU ld, Apple ld, lld spellings); any other link error is E_LINK_FAILED.
    if (link) class_out = has("undefined reference to `segarecomp_mod_provider'") || has("\"_segarecomp_mod_provider\"") || has("undefined symbol: segarecomp_mod_provider") || has("undefined symbol: _segarecomp_mod_provider") || has("unresolved external symbol segarecomp_mod_provider") || has("unresolved external symbol _segarecomp_mod_provider") ? "E_ABI_ENTRY_MISSING" : "E_LINK_FAILED";
    else class_out = text.find("E_ABI_PLATFORM") != std::string::npos ? "E_ABI_PLATFORM" : text.find("E_ABI_HEADER_MISMATCH") != std::string::npos ? "E_ABI_HEADER_MISMATCH" : "E_COMPILE_FAILED";
    detail = where + ": " + text.substr(0, 1500);
  };
  for (const auto &[path, bytes] : pkg.files) {   // std::map order: deterministic compile order
    (void)bytes;
    if (path.size() < 3 || path.compare(path.size() - 2, 2, ".c") != 0) continue;
    const mod::Payload *p = nullptr;
    for (const mod::Payload &q : m.payloads) if (q.path == path) p = &q;
    if (p == nullptr || p->kind != "native-c11") continue;
    std::vector<std::string> argv = base;
    argv.push_back((work / "src" / path).string());
    argv.push_back("-o");
    argv.push_back((work / ("unit" + std::to_string(objects.size()) + ".o")).string());
    objects.push_back(argv.back());
    rc = segarecomp_cli::run_process(argv, log, {}, 120);
    if (rc != 0) { fail_with(path, false); break; }
  }
  if (rc == 0) {   // disposable link validation: every unit together plus a driver that REQUIRES the ABI 2 entry symbol. The probe is linked, never run.
    { std::ofstream d(work / "driver.c"); d << "extern const void *segarecomp_mod_provider(void **);\nint main(void) { void *state = 0; return segarecomp_mod_provider(&state) != 0 ? 0 : 1; }\n"; }
    std::vector<std::string> argv = base;
    argv.push_back((work / "driver.c").string());
    argv.erase(std::remove(argv.begin(), argv.end(), std::string("-c")), argv.end());
    argv.insert(argv.end(), objects.begin(), objects.end());
    argv.push_back("-o");
    argv.push_back((work / "probe").string());
    rc = segarecomp_cli::run_process(argv, log, {}, 120);
    if (rc != 0) fail_with("link", true);
  }
  fs::remove_all(work, ec);
  return rc;
}

const char *const kProviderTemplate =
    "/* Minimal ABI 2 provider (template): lifecycle only; it reads and writes no guest state. */\n"
    "#include <segarecomp_mod_genesis.h>\n\n"
    "static int calls;\n\n"
    "static int provider_init(void *state, const SegaModHost *host) {\n  (void)state;\n  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;\n}\n\n"
    "static void provider_phase(void *state, const SegaModHost *host) {\n  (void)state;\n  (void)host;\n  ++calls;\n}\n\n"
    "static const SegaModProvider provider = {\n    sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, \"template-provider\", 1U, provider_init, provider_phase, NULL, NULL, NULL,\n"
    "    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};\n\n"
    "const SegaModProvider *segarecomp_mod_provider(void **state_out) {\n  if (state_out != NULL) *state_out = &calls;\n  return &provider;\n}\n";

bool hex64(const std::string &v) { return v.size() == 64 && v.find_first_not_of("0123456789abcdef") == std::string::npos; }

std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_text(const fs::path &p, const std::string &text) {
  return write_atomic(p, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

// `mod install`: verify the package against the exact original image, copy the verified bytes into the library, and record the user's approval of that exact identity (native packages only).
int install(const fs::path &package_path, const fs::path &rom, const fs::path &library, const std::string &approve) {
  const mod::Package p = mod::read_package_file(package_path);
  if (!p.ok) return refuse(p.rejection);
  std::vector<std::uint8_t> image, raw;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the original image cannot be read"});
  const mod::EffectiveImage e = mod::effective_image(p, image);
  if (!e.ok) return refuse(e.rejection);
  if (!approve.empty() && approve != p.identity) return refuse({"E_APPROVAL_MISMATCH", "the approval names another package identity"});
  if (!read_all(package_path, mod::kMaxArchiveBytes, raw)) return refuse({"E_ARCHIVE_ENTRY", "the package cannot be read"});
  const fs::path dir = library / "mods";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path dest = dir / (p.identity + ".segapatch");
  const bool added = !fs::exists(dest, ec);
  if (added && !write_atomic(dest, raw)) return refuse({"E_ARCHIVE_ENTRY", "the package cannot be installed"});
  if (!approve.empty() && !write_text(dir / (p.identity + ".approved"), p.identity + "\n")) return refuse({"E_ARCHIVE_ENTRY", "the approval cannot be recorded"});
  const bool approved = fs::exists(dir / (p.identity + ".approved"), ec);
  std::cout << "{\"ok\":true,\"installed\":true,\"added\":" << (added ? "true" : "false") << ",\"package_identity\":\"" << p.identity << "\",\"trust_class\":\"" << mod::trust_class(p.manifest)
            << "\",\"approved\":" << (approved ? "true" : "false") << ",\"effective_image\":\"" << e.effective_sha256 << "\"}\n";
  return 0;
}

// `mod variant`: key the single-mod variant by its full identity; reuse an identical build, otherwise (re)build and publish it atomically. Only <library>/cache/variants is ever written, so
// the original image, Vanilla builds, saves and earlier variants are untouched, and a failed build publishes nothing.
int variant(const fs::path &library, const std::string &identity, const fs::path &rom, const std::string &cc, const fs::path &sdk) {
  if (!hex64(identity)) return refuse({"E_FIELD_VALUE", "--identity must be a 64 digit package identity"});
  const fs::path installed = library / "mods" / (identity + ".segapatch");
  std::error_code ec;
  if (!fs::is_regular_file(installed, ec)) return refuse({"E_NOT_INSTALLED", "no package with this identity is installed"});
  const mod::Package p = mod::read_package_file(installed);
  if (!p.ok) return refuse(p.rejection);
  if (p.identity != identity) return refuse({"E_PACKAGE_CHANGED", "the installed package no longer has the identity that was installed"});
  const bool native = mod::trust_class(p.manifest) == "native";
  if (native && slurp(library / "mods" / (identity + ".approved")) != identity + "\n") return refuse({"E_NOT_APPROVED", "a package with native code needs the approval of its exact identity"});
  std::vector<std::uint8_t> image;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the original image cannot be read"});
  const mod::EffectiveImage e = mod::effective_image(p, image);
  if (!e.ok) return refuse(e.rejection);
  const mod::VariantIdentity v = mod::variant_identity(p.manifest, e.original_sha256, e.effective_sha256);
  const fs::path root = library / "cache" / "variants";
  const fs::path entry = root / v.key;
  const fs::path index = root / "index" / (p.manifest.id + "-" + e.original_sha256.substr(0, 16) + ".txt");
  std::string cache = "miss";
  std::vector<std::string> reasons;
  if (mod::parse_components(slurp(entry / "variant.txt")) == v.components && fs::is_regular_file(entry / "package.segapatch", ec)) {
    cache = "hit";
  } else {
    const std::vector<mod::VariantComponent> previous = mod::parse_components(slurp(index));
    reasons = previous.empty() ? std::vector<std::string>{"no_previous_build"} : mod::changed_components(previous, v.components);
    if (native) {
      if (cc.empty() || sdk.empty()) return refuse({"E_COMPILE_FAILED", "a package with native-c11 sources needs --cc and --sdk-dir"});
      std::string klass, detail;
      if (compile_natives(p, cc, sdk, klass, detail) != 0) return refuse({klass, detail});
    }
    const fs::path partial = root / (".partial-" + v.key);
    fs::remove_all(partial, ec);
    fs::create_directories(partial, ec);
    std::vector<std::uint8_t> raw;
    if (!read_all(installed, mod::kMaxArchiveBytes, raw) || !write_atomic(partial / "package.segapatch", raw) || !write_text(partial / "variant.txt", mod::serialize_components(v.components))) {
      fs::remove_all(partial, ec);
      return refuse({"E_ARCHIVE_ENTRY", "the variant cannot be published"});
    }
    fs::remove_all(entry, ec);
    fs::rename(partial, entry, ec);
    if (ec) { fs::remove_all(partial, ec); return refuse({"E_ARCHIVE_ENTRY", "the variant cannot be published"}); }
    fs::create_directories(index.parent_path(), ec);
    if (!write_text(index, mod::serialize_components(v.components))) return refuse({"E_ARCHIVE_ENTRY", "the variant index cannot be written"});
  }
  std::cout << "{\"ok\":true,\"cache\":\"" << cache << "\",\"reasons\":" << json_list(reasons) << ",\"variant_key\":\"" << v.key << "\",\"package_identity\":\"" << p.identity << "\",\"components\":{";
  for (std::size_t i = 0; i < v.components.size(); ++i) std::cout << (i ? ",\"" : "\"") << v.components[i].name << "\":\"" << v.components[i].value << "\"";
  std::cout << "}}\n";
  return 0;
}

int usage() {
  std::cerr << "usage: segarecomp mod init <dir> --platform <genesis|master-system|sega-cd> --id <id> [--template <c11-provider|luau|rom-edit>] [--rom-sha256 <hex>]\n"
               "       segarecomp mod check <dir>\n"
               "       segarecomp mod pack <dir> --out <package.segapatch>\n"
               "       segarecomp mod build <dir> --out <package.segapatch> [--cc <compiler> --sdk-dir <platforms-dir>]\n"
               "       segarecomp mod install <package.segapatch> --rom <original-image> --library <dir> [--approve <package-identity>]\n"
               "       segarecomp mod variant --library <dir> --identity <package-identity> --rom <original-image> [--cc <compiler> --sdk-dir <platforms-dir>]\n"
               "       segarecomp mod inspect [--json] [<dir|package.segapatch>]\n"
               "       segarecomp mod convert-audio <file.wav> --out <file.u8>\n";
  return 2;
}

}  // namespace

int segarecomp_mod_command(int argc, char **argv) {
  const std::string_view sub = argc >= 3 ? argv[2] : "";
  std::string cc, platform, id, tmpl, rom_sha, approve, identity;
  fs::path out, sdk, rom, library;
  std::string target;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto value = [&](std::string &dst) { if (i + 1 >= argc) return false; dst = argv[++i]; return true; };
    std::string tmp;
    if (a == "--cc") { if (!value(cc)) return usage(); }
    else if (a == "--platform") { if (!value(platform)) return usage(); }
    else if (a == "--id") { if (!value(id)) return usage(); }
    else if (a == "--out") { if (!value(tmp)) return usage(); out = tmp; }
    else if (a == "--sdk-dir") { if (!value(tmp)) return usage(); sdk = tmp; }
    else if (a == "--template") { if (!value(tmpl)) return usage(); }
    else if (a == "--rom-sha256") { if (!value(rom_sha)) return usage(); }
    else if (a == "--approve") { if (!value(approve)) return usage(); }
    else if (a == "--identity") { if (!value(identity)) return usage(); }
    else if (a == "--rom") { if (!value(tmp)) return usage(); rom = tmp; }
    else if (a == "--library") { if (!value(tmp)) return usage(); library = tmp; }
    else if (a == "--json") continue;   // output is always JSON
    else if (a.substr(0, 2) == "--" || !target.empty()) return usage();
    else target = std::string(a);
  }
  if (sub == "inspect") {
    if (target.empty()) {
      std::string o = "{\"ok\":true,\"adapters\":[";
      bool first = true;
      for (const mod::Adapter &a : mod::adapters()) { o += (first ? "" : ",") + adapter_json(a); first = false; }
      std::cout << o << "]}\n";
      return 0;
    }
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
      const mod::Checked c = mod::check_project(target);
      if (!c.project.ok) return refuse(c.rejection);
      const mod::Package &p = c.reread;
      if (!p.ok) return refuse(c.rejection);
      std::cout << "{\"ok\":true,\"checked\":" << (c.ok ? "true" : "false") << ",";
      if (!c.ok) std::cout << "\"class\":\"" << esc(c.rejection.code) << "\",\"error\":\"" << esc(c.rejection.message) << "\",";
      std::cout << package_json(p.manifest, p.identity, p.content_digest) << ",\"adapter\":" << adapter_json(*mod::find_adapter(p.manifest.platform)) << "}\n";
      return c.ok ? 0 : 1;
    }
    const mod::Package p = mod::read_package_file(target);
    if (!p.ok) return refuse(p.rejection);
    std::cout << "{\"ok\":true," << package_json(p.manifest, p.identity, p.content_digest) << ",\"adapter\":" << adapter_json(*mod::find_adapter(p.manifest.platform)) << "}\n";
    return 0;
  }
  if (sub == "init" && !target.empty() && !platform.empty() && !id.empty()) {
    if (mod::find_adapter(platform) == nullptr) return refuse({"E_PLATFORM_UNKNOWN", "the platform has no registered adapter"});
    if (id.size() > 64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return refuse({"E_FIELD_VALUE", "the id is invalid"});
    std::error_code ec;
    fs::create_directories(target, ec);
    const fs::path toml = fs::path(target) / "mod.toml";
    if (fs::exists(toml, ec)) return refuse({"E_ARCHIVE_ENTRY", "mod.toml already exists"});
    std::string text = mod::init_project_text(platform, id);
    if (!rom_sha.empty()) {
      const bool hex = rom_sha.size() == 64 && rom_sha.find_first_not_of("0123456789abcdef") == std::string::npos;
      if (!hex) return refuse({"E_FIELD_VALUE", "--rom-sha256 must be 64 lowercase hex digits"});
      const auto at = text.find(std::string(64, '0'));
      if (at != std::string::npos) text.replace(at, 64, rom_sha);
    }
    if (!tmpl.empty()) {
      if (tmpl != "c11-provider" && tmpl != "luau" && tmpl != "rom-edit") return refuse({"E_FIELD_VALUE", "the template is unknown"});
      if (tmpl == "c11-provider" && platform != "genesis") return refuse({"E_PLATFORM_UNKNOWN", "the c11-provider template exists for genesis only"});
    }
    if (tmpl == "c11-provider") {   // a minimal ABI 2 provider: lifecycle only, touches no guest state
      text += "\n[[source]]\nkind = \"native-c11\"\npath = \"src/provider.c\"\n";
      std::string head = "[mod]\nid = \"" + id + "\"\n";
      const auto at = text.find(head);
      if (at != std::string::npos) text.insert(at + head.size(), "author = \"you\"\n");
      text += "\n[abi]\nnative = 2\n";
    }
    std::ofstream f(toml, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return refuse({"E_ARCHIVE_ENTRY", "mod.toml cannot be written"});
    f.close();
    fs::create_directories(fs::path(target) / "scripts", ec);
    const fs::path script = fs::path(target) / "scripts" / "main.luau";
    if (!fs::exists(script, ec)) { std::ofstream sf(script, std::ios::binary); sf << "-- entry point of " << id << "\n"; }
    std::string created = "\"mod.toml\",\"scripts/main.luau\"";
    if (tmpl == "c11-provider") {
      fs::create_directories(fs::path(target) / "src", ec);
      std::ofstream pf(fs::path(target) / "src" / "provider.c", std::ios::binary);
      pf << kProviderTemplate;
      if (!pf) return refuse({"E_ARCHIVE_ENTRY", "the template cannot be written"});
      created += ",\"src/provider.c\"";
    } else if (tmpl == "rom-edit") {   // placeholder: ROM edits arrive with the edit-set milestone; nothing is declared yet
      std::ofstream nf(fs::path(target) / "NOTES.txt", std::ios::binary);
      nf << "Placeholder: rom-edit-set payloads are reserved and not accepted by this release. Keep the revision digest above; add an edit source when it ships.\n";
      created += ",\"NOTES.txt\"";
    }
    std::cout << "{\"ok\":true,\"created\":[" << created << "]}\n";
    return 0;
  }
  if (sub == "install" && !target.empty() && !rom.empty() && !library.empty()) return install(target, rom, library, approve);
  if (sub == "variant" && target.empty() && !rom.empty() && !library.empty()) return variant(library, identity, rom, cc, sdk);
  if ((sub == "check" || sub == "pack" || sub == "build") && !target.empty()) {
    const mod::Checked c = mod::check_project(target);
    if (!c.ok) return refuse(c.rejection);
    const mod::Manifest &m = c.reread.manifest;
    if (c.package.size() > mod::kMaxArchiveBytes) return refuse({"E_ARCHIVE_SIZE", "the package would exceed the archive limit"});
    bool native = false;
    for (const mod::Payload &p : m.payloads) native = native || p.kind == "native-c11";
    if (sub == "pack" || sub == "build") {
      if (out.empty()) return usage();
      if (native && sub == "build") {
        if (cc.empty() || sdk.empty()) return refuse({"E_COMPILE_FAILED", "a package with native-c11 sources needs --cc and --sdk-dir"});
        std::string klass, detail;
        if (compile_natives(c.reread, cc, sdk, klass, detail) != 0) return refuse({klass, detail});
      }
      if (!write_atomic(out, c.package)) return refuse({"E_ARCHIVE_ENTRY", "the package cannot be written"});
    }
    std::cout << "{\"ok\":true," << package_json(m, c.reread.identity, c.reread.content_digest) << ",\"package_bytes\":" << c.package.size() << (sub == "build" ? ",\"built\":true" : sub == "pack" ? ",\"packed\":true" : "") << "}\n";
    return 0;
  }
  if (sub == "convert-audio" && !target.empty() && !out.empty()) {
    std::vector<std::uint8_t> bytes;
    if (!read_all(target, mod::kMaxArchiveBytes, bytes)) return refuse({"E_ARCHIVE_ENTRY", "the audio file cannot be read"});
    const mod::Wav w = mod::parse_wav_pcm_s16le(bytes);
    if (!w.ok) return refuse({"E_FIELD_VALUE", w.error});
    if (bytes.size() > mod::kMaxAssetBytes) return refuse({"E_PAYLOAD_CAP", "the audio file exceeds the asset size limit"});
    const std::vector<std::uint8_t> raw = mod::convert_pcm_u8_mono(w);
    if (!write_atomic(out, raw)) return refuse({"E_ARCHIVE_ENTRY", "the converted audio cannot be written"});
    std::cout << "{\"ok\":true,\"target\":\"u8-mono\",\"sample_rate\":" << w.sample_rate << ",\"frames\":" << w.frames << ",\"output_sha256\":\"" << sha256_hex(raw) << "\"}\n";
    return 0;
  }
  return usage();
}
