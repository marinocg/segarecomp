// `mod.toml` project model (SEG-135-T002), native C11 source policy (SEG-135-T003) and the `mod check` pipeline. A project maps 1:1 onto a schema 3 manifest; the manifest is then re-read
// through the one schema 3 reader, so there is a single validation path. No ambient data (time, absolute paths, environment) can enter a manifest.
#include "segarecomp/mod.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace segarecomp::mod {

namespace {

struct Fail {
  std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) { throw Fail{std::move(code), std::move(message)}; }

constexpr std::size_t kMaxTomlBytes = 64U << 10U, kMaxTomlKeys = 1024;

// ---- the closed TOML subset: [table], [[array-of-tables]], key = "string" | integer | ["strings"], # comments ----
struct Val {
  enum class Kind { string, number, strings } kind = Kind::string;
  std::string text;
  std::uint64_t number = 0;
  std::vector<std::string> list;
};
struct Section {
  std::string name;
  bool array = false;
  std::vector<std::pair<std::string, Val>> entries;
  const Val *find(const std::string &k) const {
    for (const auto &e : entries) if (e.first == k) return &e.second;
    return nullptr;
  }
};

bool bare_key(const std::string &s) {
  return !s.empty() && s.size() <= 48 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; });
}

std::string parse_basic_string(const std::string &line, std::size_t &i) {
  std::string out;
  ++i;   // opening quote
  while (i < line.size()) {
    const char c = line[i++];
    if (c == '"') return out;
    if (c == '\\') {
      if (i >= line.size() || (line[i] != '"' && line[i] != '\\')) fail("E_MANIFEST_SYNTAX", "mod.toml uses an unsupported string escape");
      out.push_back(line[i++]);
    } else {
      out.push_back(c);
    }
    if (out.size() > 256) fail("E_MANIFEST_SYNTAX", "a mod.toml string is too long");
  }
  fail("E_MANIFEST_SYNTAX", "a mod.toml string is not terminated");
}

std::vector<Section> parse_toml(std::string_view text) {
  if (text.size() > kMaxTomlBytes) fail("E_MANIFEST_SYNTAX", "mod.toml is larger than allowed");
  std::vector<Section> sections;
  sections.push_back(Section{"", false, {}});
  std::size_t keys = 0, pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string line(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    for (const char c : line) if ((static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\r') || static_cast<unsigned char>(c) >= 0x7F) fail("E_MANIFEST_SYNTAX", "mod.toml holds a control or non-ASCII character");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::size_t i = 0;
    const auto skip = [&] { while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i; };
    skip();
    if (i >= line.size() || line[i] == '#') continue;
    if (line[i] == '[') {
      const bool array = i + 1 < line.size() && line[i + 1] == '[';
      i += array ? 2 : 1;
      const std::size_t close = line.find(array ? "]]" : "]", i);
      if (close == std::string::npos) fail("E_MANIFEST_SYNTAX", "a mod.toml table header is not closed");
      std::string name = line.substr(i, close - i);
      name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
      i = close + (array ? 2 : 1);
      skip();
      if (i < line.size() && line[i] != '#') fail("E_MANIFEST_SYNTAX", "mod.toml has text after a table header");
      if (!bare_key(name)) fail("E_MANIFEST_SYNTAX", "a mod.toml table name is invalid");
      const bool repeated = name == "target.revision" || name == "source" || name == "audio.mapping" || name == "audio.mapping.value";
      if (repeated != array && (repeated || name == "mod" || name == "target" || name == "capabilities" || name == "abi" || name == "state" || name == "limits" || name == "geometry" || name == "presentation"))
        fail("E_MANIFEST_SYNTAX", "the table [" + name + "] must be written " + (repeated ? "as [[" + name + "]]" : "as [" + name + "]"));
      if (!array) for (const Section &s : sections) if (s.name == name) fail("E_MANIFEST_SYNTAX", "mod.toml repeats the table [" + name + "]");
      sections.push_back(Section{name, array, {}});
      continue;
    }
    std::size_t eq = line.find('=', i);
    if (eq == std::string::npos) fail("E_MANIFEST_SYNTAX", "a mod.toml line is not key = value");
    std::string key = line.substr(i, eq - i);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    if (!bare_key(key)) fail("E_MANIFEST_SYNTAX", "a mod.toml key is invalid");
    i = eq + 1;
    skip();
    Val v;
    if (i < line.size() && line[i] == '"') {
      v.text = parse_basic_string(line, i);
    } else if (i < line.size() && line[i] == '[') {
      v.kind = Val::Kind::strings;
      ++i;
      for (;;) {
        skip();
        if (i < line.size() && line[i] == ']') { ++i; break; }
        if (i >= line.size() || line[i] != '"') fail("E_MANIFEST_SYNTAX", "a mod.toml array holds only strings on one line");
        v.list.push_back(parse_basic_string(line, i));
        if (v.list.size() > 64) fail("E_MANIFEST_SYNTAX", "a mod.toml array is too long");
        skip();
        if (i < line.size() && line[i] == ',') { ++i; continue; }
        if (i < line.size() && line[i] == ']') { ++i; break; }
        fail("E_MANIFEST_SYNTAX", "a mod.toml array is malformed");
      }
    } else if (i < line.size() && line[i] >= '0' && line[i] <= '9') {
      v.kind = Val::Kind::number;
      if (line[i] == '0' && i + 1 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '9') fail("E_MANIFEST_SYNTAX", "mod.toml integers have no leading zeros");
      int digits = 0;
      while (i < line.size() && line[i] >= '0' && line[i] <= '9') { v.number = v.number * 10 + static_cast<std::uint64_t>(line[i++] - '0'); if (++digits > 10 || v.number > 2147483647U) fail("E_FIELD_VALUE", "a mod.toml integer is too large"); }
    } else {
      fail("E_MANIFEST_SYNTAX", "a mod.toml value is not a string, integer or string array");
    }
    skip();
    if (i < line.size() && line[i] != '#') fail("E_MANIFEST_SYNTAX", "mod.toml has text after a value");
    Section &cur = sections.back();
    if (cur.find(key) != nullptr) fail("E_MANIFEST_SYNTAX", "mod.toml repeats the key \"" + key + "\"");
    if (++keys > kMaxTomlKeys) fail("E_MANIFEST_SYNTAX", "mod.toml has too many keys");
    cur.entries.emplace_back(std::move(key), std::move(v));
  }
  return sections;
}

// ---- section accessors ----
void only(const Section &s, std::initializer_list<const char *> allowed) {
  for (const auto &e : s.entries) {
    bool known = false;
    for (const char *a : allowed) known = known || e.first == a;
    if (!known) fail("E_FIELD_UNKNOWN", "[" + s.name + "] has an unknown key \"" + e.first + "\"");
  }
}
std::string text_of(const Section &s, const char *k, bool required = true, const char *missing_code = "E_FIELD_MISSING") {
  const Val *v = s.find(k);
  if (v == nullptr) { if (required) fail(missing_code, "[" + s.name + "] needs \"" + k + "\""); return {}; }
  if (v->kind != Val::Kind::string) fail("E_FIELD_VALUE", std::string("\"") + k + "\" must be a string");
  return v->text;
}
unsigned number_of(const Section &s, const char *k, bool required = true) {
  const Val *v = s.find(k);
  if (v == nullptr) { if (required) fail("E_FIELD_MISSING", "[" + s.name + "] needs \"" + k + "\""); return 0; }
  if (v->kind != Val::Kind::number) fail("E_FIELD_VALUE", std::string("\"") + k + "\" must be an integer");
  return static_cast<unsigned>(v->number);
}
std::vector<std::string> list_of(const Section &s, const char *k) {
  const Val *v = s.find(k);
  if (v == nullptr) return {};
  if (v->kind != Val::Kind::strings) fail("E_FIELD_VALUE", std::string("\"") + k + "\" must be an array of strings");
  return v->list;
}

std::size_t source_limit(const std::string &kind) {
  if (kind == "bps-image") return kMaxImageBytes;
  if (kind == "rom-edit-set") return kMaxRomEditBytes;
  if (kind == "guest-code") return kMaxGuestCodeBytes;
  if (kind == "native-c11") return kMaxNativeFileBytes;
  if (kind == "luau-source") return kMaxLuauFileBytes;
  return kMaxAssetBytes;
}

bool project_path_ok(const std::string &p) {   // same rule as a package path: relative, lower-case, <= 3 segments, no `..`, no leading dot
  if (p.empty() || p.size() > 96) return false;
  std::size_t segs = 0, start = 0;
  for (;;) {
    const std::size_t slash = p.find('/', start);
    const std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg[0] == '.' || ++segs > 3) return false;
    for (const char c : seg) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

Project build_project(const std::vector<Section> &sections, FileReader read, void *context) {
  Project project;
  Manifest &m = project.manifest;
  std::set<std::string> seen_tables;
  bool have_mod = false, have_target = false;
  const Section *last_mapping = nullptr;
  std::vector<Mapping> mappings;
  std::size_t sources = 0;
  if (!sections.front().entries.empty()) fail("E_FIELD_UNKNOWN", "mod.toml keys must be inside a table");
  std::string previous;
  for (const Section &s : sections) {
    if (s.name.empty()) continue;
    if (s.name == "audio.mapping.value" && previous != "audio.mapping" && previous != "audio.mapping.value") fail("E_FIELD_UNKNOWN", "[[audio.mapping.value]] must directly follow its [[audio.mapping]]");
    previous = s.name;
    if (s.name == "mod") {
      only(s, {"id", "version", "name", "author", "license", "purpose"});
      m.id = text_of(s, "id"); m.version = text_of(s, "version"); m.name = text_of(s, "name");
      m.author = text_of(s, "author", false); m.license = text_of(s, "license", false); m.purpose = text_of(s, "purpose", false);
      have_mod = true;
    } else if (s.name == "target") {
      only(s, {"platform", "kind", "disc_identity"});
      m.platform = text_of(s, "platform");
      const Adapter *a = find_adapter(m.platform);
      if (a == nullptr) fail("E_PLATFORM_UNKNOWN", "the platform \"" + m.platform + "\" has no registered adapter");
      m.source_kind = s.find("kind") != nullptr ? text_of(s, "kind") : std::string(a->source_kind);
      m.disc_identity = text_of(s, "disc_identity", false);
      have_target = true;
    } else if (s.name == "target.revision") {
      only(s, {"label", "sha256", "mapper"});
      Revision r;
      r.label = text_of(s, "label"); r.sha256 = text_of(s, "sha256", true, "E_SOURCE_DIGEST"); r.mapper = text_of(s, "mapper", false);
      m.revisions.push_back(std::move(r));
    } else if (s.name == "capabilities") {
      only(s, {"required", "optional"});
      m.capabilities = list_of(s, "required"); m.capabilities_optional = list_of(s, "optional");
      for (auto *v : {&m.capabilities, &m.capabilities_optional}) {   // canonical manifest form: sorted; a repeated name is an error
        std::sort(v->begin(), v->end());
        if (std::adjacent_find(v->begin(), v->end()) != v->end()) fail("E_FIELD_VALUE", "a capability is listed twice");
      }
    } else if (s.name == "abi") {
      only(s, {"native"});
      m.abi_native = number_of(s, "native");
    } else if (s.name == "state") {
      only(s, {"schema"});
      m.state_schema = number_of(s, "schema");
    } else if (s.name == "limits") {
      only(s, {"luau_memory_bytes", "luau_budget_units"});
      m.has_limits = true; m.luau_memory_bytes = number_of(s, "luau_memory_bytes", false); m.luau_budget_units = number_of(s, "luau_budget_units", false);
    } else if (s.name == "geometry") {   // reserved schema 3 key: only the empty declaration is representable until the geometry task defines scenes
      only(s, {"scenes"});
      if (s.find("scenes") != nullptr && (s.find("scenes")->kind != Val::Kind::strings || !s.find("scenes")->list.empty())) fail("E_FIELD_UNKNOWN", "geometry scenes are not defined yet; only an empty declaration is accepted");
      m.has_geometry = true;
    } else if (s.name == "presentation") {
      only(s, {"widescreen", "side_style"});
      m.default_widescreen = text_of(s, "widescreen", false); m.default_side_style = text_of(s, "side_style", false);
    } else if (s.name == "source") {
      only(s, {"kind", "path", "target_sha256", "edits", "cpu", "role", "asset_class", "format", "width", "height", "audio_format", "sample_rate", "channels", "frames", "loop_start", "loop_end"});
      if (++sources > kMaxPayloads) fail("E_PAYLOAD_CAP", "a project declares too many sources");
      Payload p;
      p.kind = text_of(s, "kind"); p.path = text_of(s, "path");
      if (!project_path_ok(p.path)) fail("E_ARCHIVE_PATH", "a source path is not a legal project-relative path");
      const std::size_t limit = source_limit(p.kind);
      const auto bytes = read(context, p.path, limit);
      if (!bytes) fail("E_ARCHIVE_ENTRY", "the source file \"" + p.path + "\" cannot be read");
      if (bytes->size() > limit) fail("E_PAYLOAD_CAP", "the source file \"" + p.path + "\" exceeds its kind cap");
      p.size = bytes->size();
      p.sha256 = sha256_hex(*bytes);
      p.target_sha256 = text_of(s, "target_sha256", false); p.edits = number_of(s, "edits", false); p.cpu = text_of(s, "cpu", false);
      p.role = s.find("role") != nullptr ? text_of(s, "role") : (p.kind == "native-c11" ? "provider" : p.kind == "luau-source" ? "script" : "");
      p.asset_class = text_of(s, "asset_class", false); p.format = text_of(s, "format", false);
      p.width = number_of(s, "width", false); p.height = number_of(s, "height", false); p.audio_format = text_of(s, "audio_format", false);
      p.sample_rate = number_of(s, "sample_rate", false); p.channels = number_of(s, "channels", false); p.frames = number_of(s, "frames", false);
      if (s.find("loop_start") != nullptr || s.find("loop_end") != nullptr) { p.has_loop = true; p.loop_start = number_of(s, "loop_start"); p.loop_end = number_of(s, "loop_end"); }
      project.files[p.path] = *bytes;
      m.payloads.push_back(std::move(p));
    } else if (s.name == "audio.mapping") {
      only(s, {"id", "kind", "space", "offset", "width", "asset", "target", "use", "revision", "length", "bank", "track"});
      Mapping mp;
      mp.id = text_of(s, "id"); mp.kind = text_of(s, "kind");
      mp.space = text_of(s, "space", false); mp.offset = number_of(s, "offset", false); mp.width = number_of(s, "width", false);
      mp.asset = text_of(s, "asset", false); mp.target = text_of(s, "target", false); mp.use = text_of(s, "use", false); mp.revision = text_of(s, "revision", false);
      if (s.find("length") != nullptr) { mp.has_site = true; mp.site_offset = mp.offset; mp.site_length = number_of(s, "length"); if (s.find("offset") == nullptr) fail("E_FIELD_MISSING", "a site needs offset and length"); }
      if (s.find("bank") != nullptr) { mp.has_bank = true; mp.bank = number_of(s, "bank"); }
      if (s.find("track") != nullptr) { mp.has_track = true; mp.track = number_of(s, "track"); }
      if (mp.kind == "asset-binding") {
        if (s.find("space") != nullptr || s.find("width") != nullptr) fail("E_FIELD_UNKNOWN", "an asset-binding mapping has no space or width");
        if (s.find("offset") != nullptr && s.find("length") == nullptr) fail("E_FIELD_MISSING", "a site needs offset and length");
        mp.offset = 0;   // an asset-binding keeps its site in site_offset / site_length
      } else if (s.find("asset") != nullptr || s.find("target") != nullptr || s.find("use") != nullptr || s.find("revision") != nullptr || s.find("length") != nullptr ||
                 s.find("bank") != nullptr || s.find("track") != nullptr) {
        fail("E_FIELD_UNKNOWN", "a command or event mapping has only space, offset and width plus its values");
      }
      mappings.push_back(std::move(mp));
      last_mapping = &s;
    } else if (s.name == "audio.mapping.value") {
      only(s, {"name", "value"});
      if (last_mapping == nullptr) fail("E_FIELD_UNKNOWN", "[[audio.mapping.value]] needs a preceding [[audio.mapping]]");
      mappings.back().values.emplace_back(text_of(s, "name"), number_of(s, "value"));
    } else {
      fail("E_FIELD_UNKNOWN", "mod.toml has an unknown table [" + s.name + "]");
    }
    if (!s.array && !seen_tables.insert(s.name).second) fail("E_MANIFEST_SYNTAX", "mod.toml repeats a table");
  }
  if (!have_mod) fail("E_FIELD_MISSING", "mod.toml needs a [mod] table");
  if (!have_target) fail("E_FIELD_MISSING", "mod.toml needs a [target] table");
  if (m.revisions.empty() && m.source_kind == "cartridge-image") fail("E_SOURCE_DIGEST", "a cartridge-image target needs at least one [[target.revision]] with an exact digest");
  if (m.source_kind == "disc-image" && !m.revisions.empty()) fail("E_FIELD_UNKNOWN", "a disc-image target has no revisions");
  std::sort(m.payloads.begin(), m.payloads.end(), [](const Payload &a, const Payload &b) { return a.path < b.path; });
  m.mappings = std::move(mappings);
  project.ok = true;
  return project;
}

Project failed_project(std::string code, std::string message) {
  Project p;
  p.rejection = Rejection{std::move(code), std::move(message)};
  return p;
}

// ---- native source policy ----
std::string joined_text(const std::vector<std::uint8_t> &bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == '\\' && i + 1 < bytes.size() && bytes[i + 1] == '\n') { ++i; continue; }
    if (bytes[i] == '\\' && i + 2 < bytes.size() && bytes[i + 1] == '\r' && bytes[i + 2] == '\n') { i += 2; continue; }
    out.push_back(static_cast<char>(bytes[i]));
  }
  return out;
}

std::optional<Rejection> scan_includes(const std::string &path, const std::string &raw, const Adapter &adapter, const std::set<std::string> &local) {
  const auto forbid = [&](const std::string &why) { return Rejection{"E_INCLUDE_FORBIDDEN", path + ": " + why}; };
  // Spellings that compilers treat as splices or whitespace in ways the scan below does not model are refused outright.
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '\0' || c == '\f' || c == '\v') return forbid("NUL, form-feed and vertical-tab bytes are not allowed");
    if (c == '\r' && !(i + 1 < raw.size() && raw[i + 1] == '\n')) return forbid("a lone carriage return is not allowed");
    if (c == '\\') {
      std::size_t j = i + 1;
      while (j < raw.size() && (raw[j] == ' ' || raw[j] == '\t')) ++j;
      if (j > i + 1 && j < raw.size() && (raw[j] == '\n' || raw[j] == '\r')) return forbid("whitespace after a line-continuation backslash is not allowed");
    }
  }
  if (raw.find("incbin") != std::string::npos || raw.find("_Pragma") != std::string::npos || raw.find("GCC dependency") != std::string::npos) return forbid("assembler file inclusion and _Pragma are not allowed");
  const std::string &t = raw;
  if (t.find("%:") != std::string::npos) return forbid("digraph spellings are not allowed");
  for (std::size_t q = t.find("??"); q != std::string::npos; q = t.find("??", q + 1))
    if (q + 2 < t.size() && std::string_view("=()/'<>!-").find(t[q + 2]) != std::string_view::npos) return forbid("trigraphs are not allowed");
  const auto skip = [&](std::size_t &i) {   // whitespace and block comments between the tokens of a directive
    for (;;) {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
      if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '*') {
        const std::size_t end = t.find("*/", i + 2);
        if (end == std::string::npos) { i = t.size(); return; }
        i = end + 2;
      } else {
        return;
      }
    }
  };
  for (std::size_t h = t.find('#'); h != std::string::npos; h = t.find('#', h + 1)) {
    std::size_t i = h + 1;
    skip(i);
    std::size_t j = i;
    while (j < t.size() && ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= 'A' && t[j] <= 'Z') || t[j] == '_' || (t[j] >= '0' && t[j] <= '9'))) ++j;
    const std::string word = t.substr(i, j - i);
    if (word == "import" || word == "embed" || word == "include_next") return forbid("#" + word + " is not allowed");
    if (word != "include") continue;
    i = j;
    skip(i);
    if (i >= t.size() || (t[i] != '"' && t[i] != '<')) return forbid("computed includes are not allowed");
    const char close = t[i] == '"' ? '"' : '>';
    const std::size_t end = t.find(close, i + 1);
    const std::size_t eol = t.find('\n', i);
    if (end == std::string::npos || (eol != std::string::npos && end > eol)) return forbid("an include is not terminated");
    const std::string name = t.substr(i + 1, end - i - 1);
    if (close == '"') {
      if (local.count(name) == 0) return forbid("\"" + name + "\" is not a package-local file");
      continue;
    }
    if (name == "segarecomp_mod.h" || name == "stdint.h" || name == "stddef.h" || name == adapter.sdk_header) continue;
    for (const Adapter &other : adapters()) if (name == other.sdk_header) return Rejection{"E_ABI_PLATFORM", path + ": <" + name + "> belongs to another platform than " + std::string(adapter.platform)};
    if (name == "segarecomp_genesis_patch.h") return Rejection{"E_ABI_MIXED", path + ": the ABI 1 header cannot be mixed into an ABI 2 package"};
    return forbid("<" + name + "> is outside the SDK allowlist");
  }
  return std::nullopt;
}

}  // namespace

std::optional<Rejection> check_native_sources(const Manifest &m, const std::map<std::string, std::vector<std::uint8_t>> &files, const Adapter &adapter) {
  std::set<std::string> local;
  bool any_c = false;
  for (const Payload &p : m.payloads) if (p.kind == "native-c11") { local.insert(p.path); any_c = any_c || p.path.back() == 'c'; }
  if (local.empty()) return std::nullopt;
  if (!any_c) return Rejection{"E_FIELD_VALUE", "a native-c11 package needs at least one .c file"};
  for (const std::string &path : local) {   // std::set order = deterministic
    const auto it = files.find(path);
    if (it == files.end()) return Rejection{"E_ARCHIVE_ENTRY", "a native source is missing"};
    const std::string raw(it->second.begin(), it->second.end());
    if (auto r = scan_includes(path, raw, adapter, local)) return r;
    if (auto r = scan_includes(path, joined_text(it->second), adapter, local)) return r;
  }
  return std::nullopt;
}

Project parse_project(std::string_view toml, FileReader read, void *context) {
  try {
    return build_project(parse_toml(toml), read, context);
  } catch (const Fail &f) {
    return failed_project(f.code, f.message);
  }
}

namespace {
struct DirContext {
  std::filesystem::path root;
};

std::optional<std::vector<std::uint8_t>> read_from_dir(void *context, const std::string &relative, std::size_t limit) {
  const auto &root = static_cast<DirContext *>(context)->root;
  std::filesystem::path at = root;
  std::error_code ec;
  std::size_t start = 0;
  for (;;) {   // no component may be a link: a project never reads outside its folder
    const std::size_t slash = relative.find('/', start);
    at /= relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (std::filesystem::is_symlink(at, ec) || ec) return std::nullopt;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (!std::filesystem::is_regular_file(at, ec)) return std::nullopt;
  const std::uintmax_t size = std::filesystem::file_size(at, ec);
  if (ec) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uintmax_t>(size, limit + 1)));
  std::ifstream in(at, std::ios::binary);
  if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
  return bytes;
}
}  // namespace

Project load_project(const std::filesystem::path &dir) {
  DirContext ctx{dir};
  const auto toml = read_from_dir(&ctx, "mod.toml", kMaxTomlBytes);
  if (!toml) return failed_project("E_ARCHIVE_ENTRY", "mod.toml cannot be read");
  if (toml->size() > kMaxTomlBytes) return failed_project("E_MANIFEST_SYNTAX", "mod.toml is larger than allowed");
  return parse_project(std::string_view(reinterpret_cast<const char *>(toml->data()), toml->size()), read_from_dir, &ctx);
}

Checked check_project(const std::filesystem::path &dir) {
  Checked out;
  out.project = load_project(dir);
  if (!out.project.ok) { out.rejection = out.project.rejection; return out; }
  // The adapter decides first what the platform can do (a capability it lacks is a stable class even where a later stage would reserve the source kind).
  if (auto r = check_adapter(out.project.manifest)) { out.rejection = *r; return out; }
  out.package = pack_package(out.project.manifest, out.project.files);
  out.reread = read_package(out.package);
  if (!out.reread.ok) { out.rejection = out.reread.rejection; return out; }
  if (!(out.reread.manifest == out.project.manifest)) { out.rejection = Rejection{"E_FIELD_VALUE", "the packed manifest differs from the project (internal consistency check)"}; return out; }
  const Adapter *adapter = find_adapter(out.reread.manifest.platform);
  if (auto r = check_native_sources(out.reread.manifest, out.reread.files, *adapter)) { out.rejection = *r; return out; }
  out.ok = true;
  return out;
}

std::string init_project_text(std::string_view platform, std::string_view id) {
  const Adapter *a = find_adapter(platform);
  std::string t = "# segarecomp mod project (docs/product/mod-projects.md). Paths are project-relative; nothing here runs or downloads anything.\n[mod]\nid = \"" + std::string(id) +
                  "\"\nversion = \"0.1.0\"\nname = \"" + std::string(id) + "\"\nlicense = \"CC0-1.0\"\n\n[target]\nplatform = \"" + std::string(platform) + "\"\n\n";
  if (a != nullptr && a->source_kind == "cartridge-image") {
    t += "# One block per exact original image this mod supports. Replace the digest with the SHA-256 of that image.\n[[target.revision]]\nlabel = \"rev-a\"\nsha256 = \"" + std::string(64, '0') + "\"\n";
    if (a->platform == "master-system") t += "mapper = \"sega\"\n";
  }
  t += "\n# Sources are project-relative files; each is size-limited and digest-listed in the package. Luau is stored, never compiled or run by this release.\n[[source]]\nkind = \"luau-source\"\npath = \"scripts/main.luau\"\n";
  return t;
}

}  // namespace segarecomp::mod
