// `segarecomp mod init|check|pack|build|inspect|install|variant|convert-audio|api|session`: the CLI seam of the modding platform (ADR 0280/0281/0283/0286). One JSON object on stdout; the launcher links the same libs/media
// implementation. `check` and `build` never execute a payload; `build` runs the HOST's C compiler (flags owned by the host, kNativeCompileFlags) over the package's own C11 units only, and the pinned
// trusted Luau compiler over its Luau sources (SEG-137-T003; the bytecode is never packaged and never run by `build`).
#include "luau_build.hpp"
#include "mod_run.hpp"
#include "process_run.hpp"
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_host.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/patch.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
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

// The trusted compile step for a package with Luau is shared with `segarecomp build` (luau_build.cpp).
using segarecomp_cli::LuauBuild;
using segarecomp_cli::build_luau;
bool write_atomic(const fs::path &out, std::span<const std::uint8_t> bytes);
std::string slurp(const fs::path &p);

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
  std::size_t edits = 0;
  for (const mod::Payload &p : m.payloads) if (p.kind == "rom-edit-set") edits += p.edits;
  std::string symbols = "[";   // provenance is part of the package and always shown (ADR 0290)
  for (std::size_t i = 0; i < m.symbols.size(); ++i) {
    const mod::Symbol &y = m.symbols[i];
    char addr[16];
    std::snprintf(addr, sizeof addr, "0x%08x", static_cast<unsigned>(y.address));
    symbols += std::string(i ? "," : "") + "{\"name\":\"" + esc(y.name) + "\",\"revision\":\"" + esc(y.revision) + "\",\"address\":\"" + addr + "\",\"provenance\":\"" + y.provenance + "\"}";
  }
  symbols += "]";
  std::string hooks;   // ADR 0298: shown only when the package declares hooks, so every other inspect output is unchanged
  if (!m.hooks.empty()) {
    hooks = ",\"hooks\":[";
    for (std::size_t i = 0; i < m.hooks.size(); ++i) {
      const mod::Hook &h = m.hooks[i];
      hooks += std::string(i ? "," : "") + "{\"id\":\"" + esc(h.id) + "\",\"symbol\":\"" + esc(h.symbol) + "\",\"revision\":\"" + esc(h.revision) + "\",\"cpu\":\"" + esc(h.cpu) + "\",\"mode\":\"" + h.mode +
               "\",\"reads\":" + json_list(h.reads) + ",\"writes\":" + json_list(h.writes) + ",\"cost\":" + std::to_string(h.cost) + "}";
    }
    hooks += "]";
  }
  std::string geometry;   // ADR 0302: shown only when a range is declared, so every other inspect output is unchanged; `declaration` is the compact text the viewer host reads (SEGARECOMP_PATCH_GEOMETRY)
  if (!m.aspect_max.empty()) {
    geometry = ",\"geometry\":{\"aspect_min\":\"" + esc(m.aspect_min.empty() ? std::string("4:3") : m.aspect_min) + "\",\"aspect_max\":\"" + esc(m.aspect_max) + "\",\"scenes\":[";
    for (std::size_t i = 0; i < m.geometry_scenes.size(); ++i) geometry += std::string(i ? "," : "") + "{\"id\":\"" + esc(m.geometry_scenes[i].id) + "\",\"components\":" + json_list(m.geometry_scenes[i].components) + "}";
    geometry += "],\"declaration\":\"" + esc(mod::geometry_declaration_text(m)) + "\"}";
  }
  std::string revs = "[";
  for (std::size_t i = 0; i < m.revisions.size(); ++i) revs += std::string(i ? "," : "") + "{\"label\":\"" + esc(m.revisions[i].label) + "\",\"sha256\":\"" + m.revisions[i].sha256 + "\"}";
  revs += "]";
  return "\"id\":\"" + esc(m.id) + "\",\"version\":\"" + esc(m.version) + "\",\"name\":\"" + esc(m.name) + "\",\"platform\":\"" + m.platform + "\",\"source_identity_kind\":\"" + m.source_kind +
         "\",\"revisions\":" + revs + ",\"trust_class\":\"" + mod::trust_class(m) + "\",\"capabilities\":" + json_list(m.capabilities) + ",\"capabilities_optional\":" + json_list(m.capabilities_optional) +
         ",\"abi_native\":" + std::to_string(m.abi_native) + ",\"payloads\":{\"native\":" + std::to_string(native) + ",\"luau\":" + std::to_string(luau) + ",\"assets\":" + std::to_string(assets) +
         ",\"other\":" + std::to_string(other) + "},\"rom_edits\":" + std::to_string(edits) + ",\"symbols\":" + symbols + hooks + geometry + ",\"package_identity\":\"" + identity + "\",\"content_digest\":\"" + content + "\"";
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
// The install sheet statement of the package's trust class (Luau policy section 6): shown by `install`, recorded nowhere else.
std::string consent_text(const mod::Manifest &m) {
  const std::string klass = mod::trust_class(m);
  if (klass == "data") return "modifies the game image; no code runs";
  std::string caps;
  for (const std::string &c : m.capabilities) caps += (caps.empty() ? "" : ", ") + c;
  for (const std::string &c : m.capabilities_optional) caps += (caps.empty() ? "" : ", ") + c + " (optional)";
  if (klass == "luau")
    return "includes scripts that run while you play; they can change gameplay and draw overlays within the capabilities listed (" + (caps.empty() ? std::string("none") : caps) +
           "); they cannot access your files or network; not a security guarantee";
  return "includes native code that can do anything on your computer; install only from a source you trust";
}

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
            << "\",\"approved\":" << (approved ? "true" : "false") << ",\"approval_kind\":\"" << (mod::trust_class(p.manifest) == "data" ? "none" : mod::trust_class(p.manifest) == "luau" ? "consent" : "native")
            << "\",\"install_sheet\":\"" << esc(consent_text(p.manifest)) << "\",\"effective_image\":\"" << e.effective_sha256 << "\"}\n";
  return 0;
}

// Fail-closed guard shared by every command that writes an output next to input images: the output must not be the same file as any input (same path, equivalent path, symlink or hard link).
// Checked before anything is written; an unrelated existing output is still replaced as before.
std::optional<mod::Rejection> output_clobbers_input(const fs::path &out, const std::vector<fs::path> &inputs) {
  fs::path staging = out;
  staging += ".partial";   // write_atomic() deletes and recreates this path first, so it is protected like the output itself
  for (const fs::path &in : inputs) {
    std::error_code e1, e2, e3;
    if (!fs::exists(in, e1)) continue;
    if ((fs::exists(out, e2) && fs::equivalent(out, in, e2) && !e2) || (fs::exists(staging, e3) && fs::equivalent(staging, in, e3) && !e3))
      return mod::Rejection{"E_OUTPUT_IS_INPUT", "--out (or its .partial staging file) is the same file as an input; refusing to overwrite it"};
  }
  return std::nullopt;
}

// Loads a project directory (through the full check pipeline) or a package file.
std::optional<mod::Rejection> load_package(const fs::path &target, mod::Package &p) {
  std::error_code ec;
  if (fs::is_directory(target, ec)) {
    const mod::Checked c = mod::check_project(target);
    if (!c.ok) return c.rejection;
    p = c.reread;
  } else {
    p = mod::read_package_file(target);
    if (!p.ok) return p.rejection;
  }
  return std::nullopt;
}

std::string warnings_json(const std::vector<std::string> &w) {
  std::string out = "[";
  for (std::size_t i = 0; i < w.size(); ++i) out += (i ? ",\"" : "\"") + w[i] + "\"";
  return out + "]";
}

// `mod apply`: the verified target image of a project or package (typed ROM edits, injected guest code, header repair; ADR 0290/0292/0293) built from the exact original image of ONE declared
// revision (selected by the image's digest). Every assertion is made first; a mismatch, collision, forbidden instruction or unsupported header refuses and writes nothing.
int apply_edits(const fs::path &target, const fs::path &rom, const fs::path &out) {
  if (const auto g = output_clobbers_input(out, {rom, target})) return refuse(*g);
  mod::Package p;
  if (const auto r = load_package(target, p)) return refuse(*r);
  std::vector<std::uint8_t> image;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the original image cannot be read"});
  const std::string original = sha256_hex(image);
  if (std::none_of(p.manifest.revisions.begin(), p.manifest.revisions.end(), [&](const mod::Revision &r) { return r.sha256 == original; })) return refuse({"E_IMAGE_MISMATCH", "the original image is not one of the declared target revisions"});
  if (!mod::has_target_edits(p.manifest)) return refuse({"E_FIELD_MISSING", "the package has no rom-edit-set or guest-asm payload"});
  const mod::TargetImage t = mod::build_target_image(p.manifest, p.files, image);
  if (!t.ok) return refuse(t.rejection);
  if (!write_atomic(out, t.image)) return refuse({"E_ARCHIVE_ENTRY", "the edited image cannot be written"});
  std::cout << "{\"ok\":true,\"revision\":\"" << esc(t.revision) << "\",\"edits_applied\":" << t.edits_applied << ",\"blocks_injected\":" << t.blocks_injected << ",\"injected_bytes\":" << t.injected_bytes
            << ",\"header_repaired\":" << (t.header_repaired ? "true" : "false") << ",\"warnings\":" << warnings_json(t.warnings) << ",\"original_sha256\":\"" << original << "\",\"effective_sha256\":\""
            << sha256_hex(t.image) << "\"}\n";
  return 0;
}

// `mod bps`: the deterministic BPS of original -> verified target (SEG-136-T006), proved by applying it back before it is written.
int build_bps_file(const fs::path &target, const fs::path &rom, const fs::path &out) {
  if (const auto g = output_clobbers_input(out, {rom, target})) return refuse(*g);
  mod::Package p;
  if (const auto r = load_package(target, p)) return refuse(*r);
  std::vector<std::uint8_t> image;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the original image cannot be read"});
  if (!mod::has_target_edits(p.manifest)) return refuse({"E_FIELD_MISSING", "the package has no rom-edit-set or guest-asm payload"});
  const mod::TargetBps b = mod::build_target_bps(p.manifest, p.files, image);
  if (!b.ok) return refuse(b.rejection);
  if (!write_atomic(out, b.patch)) return refuse({"E_ARCHIVE_ENTRY", "the BPS cannot be written"});
  std::cout << "{\"ok\":true,\"revision\":\"" << esc(b.revision) << "\",\"source_sha256\":\"" << b.source_sha256 << "\",\"target_sha256\":\"" << b.target_sha256 << "\",\"bps_sha256\":\"" << b.patch_sha256
            << "\",\"bps_bytes\":" << b.patch.size() << ",\"warnings\":" << warnings_json(b.target.warnings) << "}\n";
  return 0;
}

// `mod verify-bps`: applies a BPS to an original image and confirms the result is exactly the verified target the mod builds; a corrupted BPS or a stale source/target digest is refused.
int verify_bps_file(const fs::path &target, const fs::path &rom, const fs::path &bps) {
  mod::Package p;
  if (const auto r = load_package(target, p)) return refuse(*r);
  std::vector<std::uint8_t> image, patch_bytes;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the original image cannot be read"});
  if (!read_all(bps, patch::kMaxPatchBytes, patch_bytes)) return refuse({"E_IMAGE_MISMATCH", "the BPS cannot be read"});
  const mod::TargetImage t = mod::build_target_image(p.manifest, p.files, image);
  if (!t.ok) return refuse(t.rejection);
  const patch::BpsResult applied = patch::apply_bps(patch_bytes, image);
  if (!applied.ok) return refuse({"E_BPS_INVALID", applied.error});
  if (sha256_hex(applied.output) != sha256_hex(t.image)) return refuse({"E_BPS_STALE", "the BPS does not produce the verified target image of this mod"});
  std::cout << "{\"ok\":true,\"revision\":\"" << esc(t.revision) << "\",\"source_sha256\":\"" << sha256_hex(image) << "\",\"target_sha256\":\"" << sha256_hex(t.image) << "\"}\n";
  return 0;
}

// `mod import-hints`: tool output (Ghidra/discovery symbol lists) becomes `imported-hint` [[symbol]] blocks appended to mod.toml. Hints are never verified: they cannot be patch sites until the author
// promotes an entry to `verified` by editing it. An existing (name, revision) entry is never overwritten.
int import_hints(const fs::path &dir, const std::string &revision, const fs::path &list) {
  const mod::Project project = mod::load_project(dir);
  if (!project.ok) return refuse(project.rejection);
  const mod::Manifest &m = project.manifest;
  if (std::none_of(m.revisions.begin(), m.revisions.end(), [&](const mod::Revision &r) { return r.label == revision; })) return refuse({"E_SYMBOL_REVISION", "the revision is not declared by the project"});
  std::vector<std::uint8_t> raw;
  if (!read_all(list, 1U << 20U, raw)) return refuse({"E_SYMBOL_HINT", "the hint file cannot be read (or exceeds 1 MiB)"});
  const mod::SymbolHints h = mod::parse_symbol_hints(std::string_view(reinterpret_cast<const char *>(raw.data()), raw.size()), revision);
  if (!h.ok) return refuse(h.rejection);
  if (m.symbols.size() + h.symbols.size() > mod::kMaxSymbols) return refuse({"E_SYMBOL_HINT", "the project would exceed 255 symbols"});
  for (const mod::Symbol &y : h.symbols)
    if (std::any_of(m.symbols.begin(), m.symbols.end(), [&](const mod::Symbol &o) { return o.name == y.name && o.revision == y.revision; })) return refuse({"E_SYMBOL_DUPLICATE", "the project already has an entry for a hinted symbol"});
  std::string text = slurp(dir / "mod.toml");
  if (!text.empty() && text.back() != '\n') text += '\n';
  for (const mod::Symbol &y : h.symbols) {
    char addr[16];
    std::snprintf(addr, sizeof addr, "0x%08x", static_cast<unsigned>(y.address));
    text += "\n[[symbol]]\nname = \"" + y.name + "\"\nrevision = \"" + revision + "\"\naddress = \"" + addr + "\"\nprovenance = \"imported-hint\"\n";
  }
  if (!write_text(dir / "mod.toml", text)) return refuse({"E_ARCHIVE_ENTRY", "mod.toml cannot be written"});
  std::cout << "{\"ok\":true,\"imported\":" << h.symbols.size() << ",\"provenance\":\"imported-hint\",\"revision\":\"" << esc(revision) << "\"}\n";
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
  std::vector<std::uint8_t> installed_bytes;
  if (!read_all(installed, mod::kMaxArchiveBytes, installed_bytes)) return refuse({"E_ARCHIVE_ENTRY", "the installed package cannot be read"});
  const LuauBuild lb = build_luau(p, installed_bytes);   // the trusted compile step: its digest is part of the variant identity
  if (!lb.ok) return refuse(lb.rejection);
  const mod::VariantIdentity v = mod::variant_identity(p, e.original_sha256, e.effective_sha256, lb.compiled.empty() ? std::string_view{} : std::string_view(lb.compiled.digest()));
  const fs::path root = library / "cache" / "variants";
  const fs::path entry = root / v.key;
  const fs::path index = root / "index" / (p.manifest.id + "-" + e.original_sha256.substr(0, 16) + ".txt");
  std::string cache = "miss";
  std::vector<std::string> reasons;
  // A hit also needs the stored compiled Luau to verify against the digest recorded in the variant (a modified compiled output is a miss and is rebuilt, never loaded).
  bool luau_ok = true;
  if (!lb.compiled.empty()) {
    const std::string stored = slurp(entry / "luau.bin");
    luau_ok = luau::verify_compiled(p, {reinterpret_cast<const std::uint8_t *>(stored.data()), stored.size()}, lb.compiled.digest()).ok;
  }
  if (luau_ok && mod::parse_components(slurp(entry / "variant.txt")) == v.components && fs::is_regular_file(entry / "package.segapatch", ec)) {
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
    if (!read_all(installed, mod::kMaxArchiveBytes, raw) || !write_atomic(partial / "package.segapatch", raw) || !write_text(partial / "variant.txt", mod::serialize_components(v.components)) ||
        (!lb.compiled.empty() && !write_atomic(partial / "luau.bin", lb.compiled.serialize()))) {
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

constexpr const char *kEditTemplate =
    "# Typed ROM edits (docs/product/mod-projects.md). One edit per line:\n"
    "#   <revision|*> <type> <site> <expected> <value>\n"
    "# type: u8 u16 u32 (the platform's byte order) u16be u16le u32be u32le bytes\n"
    "# site: 0x<hex offset> or @symbol[+0xN] (a verified [[symbol]] of that revision)\n"
    "# Example (replace the revision label, offset and values with your own):\n"
    "#   rev-a u16 0x00000200 0x1234 0x5678\n";

// SEG-137-T005: the static check of a package's Luau sources against the API list; a refusal names the FIRST issue as the class and lists every issue.
int refuse_analysis(const std::vector<luau::AnalysisIssue> &issues) {
  std::string list = "[";
  for (std::size_t i = 0; i < issues.size(); ++i)
    list += std::string(i ? "," : "") + "{\"class\":\"" + esc(issues[i].cls) + "\",\"file\":\"" + esc(issues[i].where.file) + "\",\"line\":" + std::to_string(issues[i].where.line) + ",\"message\":\"" + esc(issues[i].where.message) + "\"}";
  std::cout << "{\"ok\":false,\"class\":\"" << esc(issues.front().cls) << "\",\"error\":\"" << esc(issues.front().where.text()) << "\",\"issues\":" << list << "]}\n";
  return 1;
}

// The starter project of `init --template luau`. The Genesis template is the committed SDK example (platforms/genesis/sdk/examples/luau-hud/scripts/main.luau; a test keeps them equal).
constexpr const char *kLuauHudScript =
    "-- Luau-only HUD example (restricted scripting: ADR 0295/0296). It runs while the game plays; it can only request overlay drawing and write log lines.\n"
    "-- The capabilities it uses are declared in mod.toml; `segarecomp mod check` rejects any other `sega.*` use.\n"
    "local PANEL = 0x000000C0     -- 0xRRGGBBAA\n"
    "local WHITE = 0xFFFFFFFF\n"
    "local GOLD = 0xFFD040FF\n"
    "\n"
    "function reset()\n"
    "  sega.log(\"hud example ready\")\n"
    "end\n"
    "\n"
    "function render(frame: number)\n"
    "  sega.overlay.rect(0, 0, 76, 24, PANEL)\n"
    "  sega.overlay.text(4, 3, \"LUAU HUD\", WHITE)\n"
    "  sega.overlay.text(4, 12, \"FRAME \" .. frame % 1000, GOLD)\n"
    "  sega.overlay.rect(0, 24, frame % 76 + 1, 3, GOLD)   -- a bar that grows with the frame counter\n"
    "end\n";
constexpr const char *kLuauLogScript =
    "-- Luau-only example (restricted scripting: ADR 0295). This platform grants the log binding only.\n"
    "function reset()\n"
    "  sega.log(\"ready\")\n"
    "end\n";
constexpr const char *kLuauRc = "{\n  \"languageMode\": \"strict\"\n}\n";
constexpr const char *kVsCodeSettings =
    "{\n  \"luau-lsp.types.definitionFiles\": [\"types/segarecomp.d.luau\"],\n  \"luau-lsp.platform.type\": \"standard\",\n  \"luau-lsp.sourcemap.enabled\": false\n}\n";

int run_api(bool declarations_only) {
  if (declarations_only) std::cout << luau::declarations();
  else std::cout << luau::api_json();
  return 0;
}

// SEG-137-T007: runs the deterministic fixture session of a Luau-only package (see luau_api.hpp). The scripts are compiled by the trusted compile child, never loaded from the package.
int run_session(const fs::path &target, const fs::path &rom, std::uint64_t frames, const fs::path &bmp) {
  mod::Package p;
  if (const auto r = load_package(target, p)) return refuse(*r);
  std::vector<std::uint8_t> image;
  if (!read_all(rom, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "the image cannot be read"});
  const mod::EffectiveImage e = mod::effective_image(p, image);
  if (!e.ok) return refuse(e.rejection);
  if (mod::trust_class(p.manifest) != "luau") return refuse({"E_TRUST_CLASS", "a fixture session runs Luau-only packages"});
  const std::vector<luau::AnalysisIssue> issues = luau::analyze_mod(p);
  if (!issues.empty()) return refuse_analysis(issues);
  std::vector<std::uint8_t> raw;
  if (fs::is_directory(target)) {
    const mod::Checked c = mod::check_project(target);
    if (!c.ok) return refuse(c.rejection);
    raw = c.package;
  } else if (!read_all(target, mod::kMaxArchiveBytes, raw)) {
    return refuse({"E_ARCHIVE_ENTRY", "the package cannot be read"});
  }
  const LuauBuild lb = build_luau(p, raw);
  if (!lb.ok) return refuse(lb.rejection);
  const luau::SessionResult s = luau::run_fixture_session(p, lb.compiled, sha256_hex(image), frames);
  if (!s.ok) return refuse({"E_SESSION", s.refusal});
  if (!bmp.empty()) {   // the last frame as a 24-bit BMP, for looking at
    std::vector<std::uint8_t> f(54);
    const std::uint32_t row = static_cast<std::uint32_t>(luau::kFixtureWidth) * 3U, size = row * static_cast<std::uint32_t>(luau::kFixtureHeight);
    const auto put32 = [&f](std::size_t at, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) f[at + i] = static_cast<std::uint8_t>(v >> (8U * i)); };
    f[0] = 'B'; f[1] = 'M'; put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, luau::kFixtureWidth); put32(22, luau::kFixtureHeight); f[26] = 1; f[28] = 24; put32(34, size);
    for (int y = luau::kFixtureHeight - 1; y >= 0; --y)
      for (int x = 0; x < luau::kFixtureWidth; ++x) {
        const std::uint8_t *px = &s.last_frame[(static_cast<std::size_t>(y) * luau::kFixtureWidth + static_cast<std::size_t>(x)) * 3U];
        f.push_back(px[2]); f.push_back(px[1]); f.push_back(px[0]);
      }
    if (!write_atomic(bmp, f)) return refuse({"E_ARCHIVE_ENTRY", "the frame cannot be written"});
  }
  std::string log = "[";
  for (std::size_t i = 0; i < s.log.size(); ++i) log += (i ? ",\"" : "\"") + esc(s.log[i]) + "\"";
  log += "]";
  std::string notes = "[";
  for (std::size_t i = 0; i < s.notes.size(); ++i) notes += std::string(i ? "," : "") + "{\"cause\":\"" + esc(s.notes[i].cause) + "\",\"message\":\"" + esc(s.notes[i].message) + "\"}";
  notes += "]";
  std::cout << "{\"ok\":true,\"tool\":\"diagnostic synthetic fixture session (not the Genesis machine)\",\"synthetic_picture\":true,\"trust_class\":\"luau\",\"package_identity\":\"" << p.identity << "\",\"image_sha256\":\"" << sha256_hex(image) << "\",\"frames\":" << s.frames << ",\"overlay_frames\":" << s.overlay_frames
            << ",\"first_frame_sha256\":\"" << s.frame_sha256.front() << "\",\"last_frame_sha256\":\"" << s.frame_sha256.back() << "\",\"session_sha256\":\"" << s.session_sha256
            << "\",\"faulted\":" << (s.fault != luau::Fault::none ? "true" : "false") << ",\"fault\":\"" << luau::fault_name(s.fault) << "\",\"diagnostic\":\"" << esc(s.diagnostic) << "\",\"nondeterministic\":"
            << (s.nondeterministic ? "true" : "false") << ",\"leaked_bytes\":" << s.leaked_bytes << ",\"log\":" << log << ",\"notes\":" << notes << "}\n";
  return 0;
}

int usage() {
  std::cerr << "usage: segarecomp mod init <dir> --platform <genesis|master-system|sega-cd> --id <id> [--template <c11-provider|luau|rom-edit>] [--rom-sha256 <hex>]\n"
               "       segarecomp mod check <dir>\n"
               "       segarecomp mod pack <dir> --out <package.segapatch> [--rom <declared-image>]...\n"
               "       segarecomp mod build <dir> --out <package.segapatch> [--cc <compiler> --sdk-dir <platforms-dir>]\n"
               "       segarecomp mod install <package.segapatch> --rom <original-image> --library <dir> [--approve <package-identity>]\n"
               "       segarecomp mod variant --library <dir> --identity <package-identity> --rom <original-image> [--cc <compiler> --sdk-dir <platforms-dir>]\n"
               "       segarecomp mod inspect [--json] [<dir|package.segapatch>]\n"
               "       segarecomp mod diff-run <dir|package> --rom <original-image> --cc <compiler> --sdk-dir <platforms-dir> --frames <n> --budget <instructions> --timeout <seconds> [--first <frame>] [--input <pad-script>] [--ram <offset:length>]... [--code <offset:length>]... [--aggregate] [--work <dir>]\n"
               "       segarecomp mod test <dir> --rom <original-image> --cc <compiler> --sdk-dir <platforms-dir> [--scenario <name>] [--aggregate] [--work <dir>]\n"
               "       segarecomp mod convert-audio <file.wav> --out <file.u8>\n"
               "       segarecomp mod apply <dir|package.segapatch> --rom <original-image> --out <edited-image>\n"
               "       segarecomp mod bps <dir|package.segapatch> --rom <original-image> --out <patch.bps>\n"
               "       segarecomp mod verify-bps <dir|package.segapatch> --rom <original-image> --bps <patch.bps>\n"
               "       segarecomp mod import-hints <dir> --revision <label> --from <hint-file>\n"
               "       segarecomp mod api [--declarations]\n"
               "       segarecomp mod session <dir|package.segapatch> --rom <image> [--frames <n>] [--bmp <last-frame.bmp>]   (DIAGNOSTIC: synthetic picture, not the Genesis machine)\n";
  return 2;
}

}  // namespace

int segarecomp_mod_command(int argc, char **argv) {
  segarecomp_cli::set_self_path(argc > 0 ? argv[0] : "");
  const std::string_view sub = argc >= 3 ? argv[2] : "";
  std::string cc, platform, id, tmpl, rom_sha, approve, identity, revision;
  fs::path hints;
  fs::path out, sdk, rom, library, bps;
  std::vector<fs::path> roms;
  std::string target;
  bool declarations_only = false;
  std::uint64_t frames = 60;
  fs::path bmp;
  segarecomp_cli::RunOptions run;   // diff-run / test (SEG-136-T007)
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
    else if (a == "--revision") { if (!value(revision)) return usage(); }
    else if (a == "--from") { if (!value(tmp)) return usage(); hints = tmp; }
    else if (a == "--rom") { if (!value(tmp)) return usage(); rom = tmp; roms.push_back(tmp); }
    else if (a == "--bps") { if (!value(tmp)) return usage(); bps = tmp; }
    else if (a == "--library") { if (!value(tmp)) return usage(); library = tmp; }
    else if (a == "--declarations") declarations_only = true;
    else if (a == "--frames") { if (!value(tmp) || tmp.empty() || tmp.size() > 5 || tmp.find_first_not_of("0123456789") != std::string::npos) return usage(); frames = std::stoull(tmp); run.has_frames = true; }
    else if (a == "--first") { if (!value(tmp) || tmp.empty() || tmp.size() > 8 || tmp.find_first_not_of("0123456789") != std::string::npos) return usage(); run.first = std::stoull(tmp); }
    else if (a == "--budget") { if (!value(tmp) || tmp.empty() || tmp.size() > 12 || tmp.find_first_not_of("0123456789") != std::string::npos) return usage(); run.budget = std::stoull(tmp); run.has_budget = true; }
    else if (a == "--timeout") { if (!value(tmp) || tmp.empty() || tmp.size() > 6 || tmp.find_first_not_of("0123456789") != std::string::npos) return usage(); run.timeout = static_cast<unsigned>(std::stoul(tmp)); run.has_timeout = true; }
    else if (a == "--input") { if (!value(run.input)) return usage(); }
    else if (a == "--ram") { if (!value(tmp)) return usage(); run.ram.push_back(tmp); }
    else if (a == "--code") { if (!value(tmp)) return usage(); run.code.push_back(tmp); }
    else if (a == "--scenario") { if (!value(run.scenario)) return usage(); }
    else if (a == "--work") { if (!value(tmp)) return usage(); run.work = tmp; }
    else if (a == "--aggregate") run.aggregate = true;
    else if (a == "--bmp") { if (!value(tmp)) return usage(); bmp = tmp; }
    else if (a == "--json") continue;   // output is always JSON
    else if (a.substr(0, 2) == "--" || !target.empty()) return usage();
    else target = std::string(a);
  }
  if (sub == "api" && target.empty()) return run_api(declarations_only);
  if (sub == "diff-run" || sub == "test") {   // SEG-136-T007 (ADR 0300): bounded Vanilla-versus-Modded workflow; always explicit budgets and timeouts
    run.target = target;
    run.rom = rom;
    run.cc = cc;
    run.sdk = sdk;
    run.frames = frames;
    return sub == "diff-run" ? segarecomp_cli::run_diff(run) : segarecomp_cli::run_test(run);
  }
  if (sub == "session" && !target.empty() && roms.size() == 1) return run_session(target, rom, frames, bmp);
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
    if (tmpl == "rom-edit") text += "\n[[source]]\nkind = \"rom-edit-set\"\npath = \"edits/main.edits\"\n";
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
    const bool luau_template = tmpl == "luau";
    const bool hud = luau_template && platform == "genesis";
    if (luau_template) text += hud ? "\n[capabilities]\nrequired = [\"host.log\", \"overlay.rgba\"]\n" : "\n[capabilities]\nrequired = [\"host.log\"]\n";
    std::ofstream f(toml, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return refuse({"E_ARCHIVE_ENTRY", "mod.toml cannot be written"});
    f.close();
    fs::create_directories(fs::path(target) / "scripts", ec);
    const fs::path script = fs::path(target) / "scripts" / "main.luau";
    if (!fs::exists(script, ec)) { std::ofstream sf(script, std::ios::binary); if (luau_template) sf << (hud ? kLuauHudScript : kLuauLogScript); else sf << "-- entry point of " << id << "\n"; }
    std::string created = "\"mod.toml\",\"scripts/main.luau\"";
    if (luau_template) {   // SEG-137-T005: the generated declarations and the editor configuration (VS Code with luau-lsp; nothing is shipped or installed)
      fs::create_directories(fs::path(target) / "types", ec);
      fs::create_directories(fs::path(target) / ".vscode", ec);
      if (!write_text(fs::path(target) / "types" / "segarecomp.d.luau", luau::declarations()) || !write_text(fs::path(target) / ".luaurc", kLuauRc) || !write_text(fs::path(target) / ".vscode" / "settings.json", kVsCodeSettings))
        return refuse({"E_ARCHIVE_ENTRY", "the template cannot be written"});
      created += ",\"types/segarecomp.d.luau\",\".luaurc\",\".vscode/settings.json\"";
    }
    if (tmpl == "c11-provider") {
      fs::create_directories(fs::path(target) / "src", ec);
      std::ofstream pf(fs::path(target) / "src" / "provider.c", std::ios::binary);
      pf << kProviderTemplate;
      if (!pf) return refuse({"E_ARCHIVE_ENTRY", "the template cannot be written"});
      created += ",\"src/provider.c\"";
    } else if (tmpl == "rom-edit") {   // a commented edit file; add edit lines, then `mod check` and `mod apply`
      fs::create_directories(fs::path(target) / "edits", ec);
      std::ofstream nf(fs::path(target) / "edits" / "main.edits", std::ios::binary);
      nf << kEditTemplate;
      if (!nf) return refuse({"E_ARCHIVE_ENTRY", "the template cannot be written"});
      created += ",\"edits/main.edits\"";
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
    if (const std::vector<luau::AnalysisIssue> issues = luau::analyze_mod(c.reread); !issues.empty()) return refuse_analysis(issues);
    if ((sub == "pack" || sub == "build") && !roms.empty() && !out.empty())
      if (const auto g = output_clobbers_input(out, roms)) return refuse(*g);
    std::string luau_json;
    std::string verified = "[";   // SEG-136-T006: with --rom, every named image is built, BPS-encoded and round-trip verified BEFORE the package is written
    if ((sub == "pack" || sub == "build") && !roms.empty()) {
      if (!mod::has_target_edits(m)) return refuse({"E_FIELD_MISSING", "--rom verification needs a rom-edit-set or guest-asm payload"});
      for (const fs::path &r : roms) {
        std::vector<std::uint8_t> image;
        if (!read_all(r, mod::kMaxImageBytes, image)) return refuse({"E_IMAGE_MISMATCH", "a --rom image cannot be read"});
        const mod::TargetBps b = mod::build_target_bps(m, c.reread.files, image);
        if (!b.ok) return refuse(b.rejection);
        verified += std::string(verified.size() > 1 ? "," : "") + "{\"revision\":\"" + esc(b.revision) + "\",\"source_sha256\":\"" + b.source_sha256 + "\",\"target_sha256\":\"" + b.target_sha256 +
                    "\",\"bps_sha256\":\"" + b.patch_sha256 + "\",\"warnings\":" + warnings_json(b.target.warnings) + "}";
      }
    }
    verified += "]";
    if (sub == "pack" || sub == "build") {
      if (out.empty()) return usage();
      if (native && sub == "build") {
        if (cc.empty() || sdk.empty()) return refuse({"E_COMPILE_FAILED", "a package with native-c11 sources needs --cc and --sdk-dir"});
        std::string klass, detail;
        if (compile_natives(c.reread, cc, sdk, klass, detail) != 0) return refuse({klass, detail});
      }
      if (sub == "build") {
        const LuauBuild lb = build_luau(c.reread, c.package);
        if (!lb.ok) return refuse(lb.rejection);
        luau_json = lb.json;
      }
      if (!write_atomic(out, c.package)) return refuse({"E_ARCHIVE_ENTRY", "the package cannot be written"});
    }
    std::cout << "{\"ok\":true," << package_json(m, c.reread.identity, c.reread.content_digest) << ",\"package_bytes\":" << c.package.size() << (sub == "build" ? ",\"built\":true" : sub == "pack" ? ",\"packed\":true" : "") << (roms.empty() ? "" : ",\"verified_images\":" + verified) << luau_json << "}\n";
    return 0;
  }
  if (sub == "apply" && !target.empty() && roms.size() == 1 && !out.empty()) return apply_edits(target, rom, out);
  if (sub == "bps" && !target.empty() && roms.size() == 1 && !out.empty()) return build_bps_file(target, rom, out);
  if (sub == "verify-bps" && !target.empty() && roms.size() == 1 && !bps.empty()) return verify_bps_file(target, rom, bps);
  if (sub == "import-hints" && !target.empty() && !revision.empty() && !hints.empty()) return import_hints(target, revision, hints);
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
