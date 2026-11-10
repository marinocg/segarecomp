// `.segapatch` schema 3 reader, canonical serializer and canonical packer (docs/architecture/segapatch-schema-3.md; ADR 0281). Every rejection carries a stable class (section 9).
#include "segarecomp/mod.hpp"

#include "patch_archive.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace segarecomp::mod {

using patch::detail::JsonLimits;
using patch::detail::Value;

namespace {

struct Fail {
  std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) { throw Fail{std::move(code), std::move(message)}; }

// ---- syntax of fields ----
bool v3_path_ok(const std::string &p) {
  if (p.empty() || p.size() > 96) return false;
  std::size_t segments = 0, start = 0;
  for (;;) {
    const std::size_t slash = p.find('/', start);
    const std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg[0] == '.' || ++segments > 3) return false;
    for (const char c : seg) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

bool hex64(const std::string &s) { return patch::detail::is_hex64(s); }

bool name_token(const std::string &s, std::size_t max) {   // lower-case token with dashes: id, label, role, kind-like names
  if (s.empty() || s.size() > max) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'; });
}

bool id_ok(const std::string &s) {   // [a-z0-9]+(-[a-z0-9]+)*, <= 64
  if (s.empty() || s.size() > 64 || s.front() == '-' || s.back() == '-') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '-') { if (s[i - 1] == '-') return false; continue; }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

bool semver_ok(const std::string &s) {   // MAJOR.MINOR.PATCH, decimal, no leading zeros, <= 9 digits each
  std::size_t parts = 0, i = 0;
  while (i <= s.size()) {
    const std::size_t dot = s.find('.', i);
    const std::string part = s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
    if (part.empty() || part.size() > 9 || (part.size() > 1 && part[0] == '0')) return false;
    if (!std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    ++parts;
    if (dot == std::string::npos) break;
    i = dot + 1;
  }
  return parts == 3;
}

bool license_ok(const std::string &s) {
  if (s.empty() || s.size() > 32) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-'; });
}

// ---- JSON field access ----
const Value &need(const Value &obj, const char *key) {
  const Value *v = obj.find(key);
  if (v == nullptr) fail("E_FIELD_MISSING", std::string("the field \"") + key + "\" is missing");
  return *v;
}
void only(const Value &obj, std::initializer_list<const char *> allowed, const char *where) {
  for (const std::string &k : obj.keys) {
    bool known = false;
    for (const char *a : allowed) known = known || k == a;
    if (!known) fail("E_FIELD_UNKNOWN", std::string(where) + " has an unknown field \"" + k + "\"");
  }
}
const std::string &str(const Value &obj, const char *key) {
  const Value &v = need(obj, key);
  if (v.kind != Value::Kind::string) fail("E_FIELD_VALUE", std::string("the field \"") + key + "\" must be a string");
  return v.text;
}
std::uint64_t num(const Value &obj, const char *key) {
  const Value &v = need(obj, key);
  if (v.kind != Value::Kind::number) fail("E_FIELD_VALUE", std::string("the field \"") + key + "\" must be an integer");
  return v.number;
}
unsigned num32(const Value &obj, const char *key) { return static_cast<unsigned>(num(obj, key)); }   // the JSON reader caps numbers at 2147483647
const Value &object(const Value &v, const char *what) {
  if (v.kind != Value::Kind::object) fail("E_FIELD_VALUE", std::string(what) + " must be an object");
  return v;
}
const Value &array(const Value &v, const char *what) {
  if (v.kind != Value::Kind::array) fail("E_FIELD_VALUE", std::string(what) + " must be an array");
  return v;
}
std::vector<std::string> string_list(const Value &arr, const char *what) {
  array(arr, what);
  if (arr.items.size() > kMaxCapabilities) fail("E_FIELD_VALUE", std::string(what) + " has too many entries");
  std::vector<std::string> out;
  for (const Value &v : arr.items) {
    if (v.kind != Value::Kind::string) fail("E_FIELD_VALUE", std::string(what) + " must hold strings");
    out.push_back(v.text);
  }
  return out;
}

bool is_in(std::string_view needle, std::initializer_list<std::string_view> hay) {
  for (const std::string_view h : hay) if (h == needle) return true;
  return false;
}

bool known_space(const std::string &s) {
  return is_in(s, {"m68k.work_ram", "z80.ram", "z80.work_ram", "z80.cart_ram", "main.work_ram", "main.word_ram", "sub.prg_ram"});
}

// ---- kind table ----
enum class KindClass { data, luau, native };
struct KindRow {
  std::string_view kind;
  bool reserved;
  KindClass trust;
};
constexpr KindRow kKinds[] = {
    {"bps-image", false, KindClass::data}, {"rom-edit-set", false, KindClass::data}, {"guest-code", false, KindClass::data}, {"native-c11", false, KindClass::native},
    {"luau-source", false, KindClass::luau}, {"asset", false, KindClass::data}, {"disc-extent-edit", true, KindClass::data}, {"disc-track-replace", true, KindClass::data},
    {"guest-asm", true, KindClass::data},
};

const KindRow *find_kind(const std::string &kind) {
  for (const KindRow &r : kKinds) if (r.kind == kind) return &r;
  return nullptr;
}

bool has_kind(const Manifest &m, std::string_view kind) {
  return std::any_of(m.payloads.begin(), m.payloads.end(), [&](const Payload &p) { return p.kind == kind; });
}

const Payload *find_payload(const Manifest &m, const std::string &path) {
  for (const Payload &p : m.payloads) if (p.path == path) return &p;
  return nullptr;
}

bool has_capability(const Manifest &m, std::string_view name) {
  return std::find(m.capabilities.begin(), m.capabilities.end(), name) != m.capabilities.end() ||
         std::find(m.capabilities_optional.begin(), m.capabilities_optional.end(), name) != m.capabilities_optional.end();
}

// ---- manifest -> model ----
Payload parse_payload(const Value &v, const std::string &platform) {
  object(v, "a payload");
  Payload p;
  p.kind = str(v, "kind");
  const KindRow *row = find_kind(p.kind);
  if (row == nullptr) fail("E_PAYLOAD_KIND_UNKNOWN", "the payload kind \"" + p.kind + "\" is not known");
  const std::uint64_t version = num(v, "kind_version");
  p.path = str(v, "path");
  if (!v3_path_ok(p.path)) fail("E_ARCHIVE_PATH", "a payload path is not a legal package path");
  p.size = num(v, "size");
  p.sha256 = str(v, "sha256");
  if (!hex64(p.sha256)) fail("E_FIELD_VALUE", "a payload sha256 is not 64 lowercase hex digits");
  if (version != 1) fail("E_PAYLOAD_VERSION_UNKNOWN", "the payload kind \"" + p.kind + "\" has no version " + std::to_string(version));
  p.kind_version = 1;
  if (row->reserved) fail("E_PAYLOAD_KIND_RESERVED", "the payload kind \"" + p.kind + "\" is reserved and not implemented");
  const bool cartridge_only = p.kind == "bps-image" || p.kind == "rom-edit-set" || p.kind == "guest-code";
  if (cartridge_only && platform == "sega-cd") fail("E_PAYLOAD_PLATFORM", "the payload kind \"" + p.kind + "\" does not exist on sega-cd");
  if (p.kind == "bps-image") {
    only(v, {"kind", "kind_version", "path", "size", "sha256", "target_sha256"}, "a bps-image payload");
    p.target_sha256 = str(v, "target_sha256");
    if (!hex64(p.target_sha256)) fail("E_FIELD_VALUE", "target_sha256 is not 64 lowercase hex digits");
  } else if (p.kind == "rom-edit-set") {
    only(v, {"kind", "kind_version", "path", "size", "sha256", "edits"}, "a rom-edit-set payload");
    p.edits = num32(v, "edits");
  } else if (p.kind == "guest-code") {
    only(v, {"kind", "kind_version", "path", "size", "sha256", "cpu"}, "a guest-code payload");
    p.cpu = str(v, "cpu");
    if (p.cpu != "m68k" && p.cpu != "z80") fail("E_FIELD_VALUE", "guest-code cpu must be m68k or z80");
    if (p.cpu == "m68k" && platform != "genesis") fail("E_PAYLOAD_PLATFORM", "m68k guest code exists only on genesis");
  } else if (p.kind == "native-c11" || p.kind == "luau-source") {
    only(v, {"kind", "kind_version", "path", "size", "sha256", "role"}, "a source payload");
    p.role = str(v, "role");
    if (p.role != (p.kind == "native-c11" ? "provider" : "script")) fail("E_FIELD_VALUE", "the payload role is not valid for its kind");
    const std::string ext = p.kind == "native-c11" ? ".c" : ".luau";
    const bool header_ok = p.kind == "native-c11" && p.path.size() > 2 && p.path.compare(p.path.size() - 2, 2, ".h") == 0;
    if (!header_ok && (p.path.size() <= ext.size() || p.path.compare(p.path.size() - ext.size(), ext.size(), ext) != 0)) fail("E_FIELD_VALUE", "the payload path has the wrong file extension for its kind");
  } else {   // asset
    p.asset_class = str(v, "asset_class");
    if (p.asset_class == "image") {
      only(v, {"kind", "kind_version", "path", "size", "sha256", "asset_class", "width", "height", "format"}, "an image asset");
      p.width = num32(v, "width"); p.height = num32(v, "height"); p.format = str(v, "format");
      if (p.format != "rgba8" || p.width == 0 || p.height == 0 || p.width > 4096 || p.height > 4096) fail("E_FIELD_VALUE", "an image asset needs format rgba8 and dimensions 1..4096");
      if (p.size != static_cast<std::uint64_t>(p.width) * p.height * 4U) fail("E_ARCHIVE_SIZE", "an image asset size differs from width * height * 4");
    } else if (p.asset_class == "data") {
      only(v, {"kind", "kind_version", "path", "size", "sha256", "asset_class", "format"}, "a data asset");
      p.format = str(v, "format");
      if (!name_token(p.format, 32)) fail("E_FIELD_VALUE", "a data asset format must be a token");
    } else if (p.asset_class == "audio") {
      only(v, {"kind", "kind_version", "path", "size", "sha256", "asset_class", "audio_format", "sample_rate", "channels", "frames", "loop_start", "loop_end"}, "an audio asset");
      p.audio_format = str(v, "audio_format");
      if (p.audio_format != "wav-pcm-s16le") fail("E_FIELD_VALUE", "the audio_format is not supported");
      p.sample_rate = num32(v, "sample_rate"); p.channels = num32(v, "channels"); p.frames = num32(v, "frames");
      if (p.sample_rate < 8000 || p.sample_rate > 48000) fail("E_FIELD_VALUE", "the audio sample_rate must be 8000..48000");
      if (p.channels != 1 && p.channels != 2) fail("E_FIELD_VALUE", "audio channels must be 1 or 2");
      if (p.frames == 0) fail("E_FIELD_VALUE", "an audio asset needs at least one frame");
      if (static_cast<std::uint64_t>(p.frames) > static_cast<std::uint64_t>(kMaxAudioSeconds) * p.sample_rate) fail("E_PAYLOAD_CAP", "an audio asset is longer than allowed");
      const bool a = v.find("loop_start") != nullptr, b = v.find("loop_end") != nullptr;
      if (a != b) fail("E_FIELD_VALUE", "loop_start and loop_end are declared together");
      if (a) {
        p.has_loop = true; p.loop_start = num32(v, "loop_start"); p.loop_end = num32(v, "loop_end");
        if (p.loop_start >= p.loop_end || p.loop_end > p.frames) fail("E_FIELD_VALUE", "the audio loop is outside the declared frames");
      }
    } else {
      fail("E_FIELD_VALUE", "the asset_class \"" + p.asset_class + "\" is not known");
    }
  }
  return p;
}

Mapping parse_mapping(const Value &v) {
  object(v, "a mapping");
  Mapping m;
  m.id = str(v, "id");
  m.kind = str(v, "kind");
  if (!name_token(m.id, 32)) fail("E_FIELD_VALUE", "a mapping id must be a token");
  const auto values_of = [&](const char *key) {
    const Value &list = array(need(v, key), key);
    if (list.items.empty() || list.items.size() > 64) fail("E_FIELD_VALUE", std::string(key) + " must hold 1..64 entries");
    for (const Value &e : list.items) {
      object(e, "a mapping entry");
      only(e, {"name", "value"}, "a mapping entry");
      const std::string &name = str(e, "name");
      if (!name_token(name, 32)) fail("E_FIELD_VALUE", "a mapping value name must be a token");
      for (const auto &existing : m.values) if (existing.first == name) fail("E_FIELD_VALUE", "a mapping repeats a value name");
      m.values.emplace_back(name, num32(e, "value"));
    }
  };
  if (m.kind == "command" || m.kind == "event") {
    const bool command = m.kind == "command";
    only(v, {"id", "kind", "space", "offset", "width", command ? "commands" : "events"}, "a mapping");
    m.space = str(v, "space");
    if (!known_space(m.space)) fail("E_FIELD_VALUE", "the mapping space \"" + m.space + "\" is not a known space");
    m.offset = num32(v, "offset"); m.width = num32(v, "width");
    if (m.width != 1 && m.width != 2 && m.width != 4) fail("E_FIELD_VALUE", "a mapping width must be 1, 2 or 4");
    values_of(command ? "commands" : "events");
    for (const auto &e : m.values)
      if (m.width < 4 && e.second >= (1U << (8U * m.width))) fail("E_FIELD_VALUE", "a mapping value does not fit its width");
  } else if (m.kind == "asset-binding") {
    only(v, {"id", "kind", "asset", "target", "use", "revision", "offset", "length", "bank", "track"}, "a mapping");
    m.asset = str(v, "asset"); m.target = str(v, "target"); m.use = str(v, "use");
    if (!name_token(m.target, 32)) fail("E_FIELD_VALUE", "an asset-binding target must be a token");
    if (std::none_of(std::begin(kAudioUses), std::end(kAudioUses), [&](std::string_view u) { return u == m.use; })) fail("E_FIELD_VALUE", "the asset-binding use \"" + m.use + "\" is not known");
    if (v.find("revision") != nullptr) m.revision = str(v, "revision");
    if (v.find("offset") != nullptr || v.find("length") != nullptr) {
      m.has_site = true; m.site_offset = num32(v, "offset"); m.site_length = num32(v, "length");
      if (m.site_length == 0) fail("E_FIELD_VALUE", "an asset-binding site length must be positive");
    }
    if (v.find("bank") != nullptr) { m.has_bank = true; m.bank = num32(v, "bank"); }
    if (v.find("track") != nullptr) { m.has_track = true; m.track = num32(v, "track"); }
  } else {
    fail("E_FIELD_VALUE", "the mapping kind \"" + m.kind + "\" is not known");
  }
  return m;
}

Manifest parse_manifest(const Value &root) {
  object(root, "the manifest");
  only(root, {"format", "schema_version", "id", "version", "name", "platform", "author", "license", "purpose", "source", "bios", "capabilities", "capabilities_optional", "abi", "state_schema",
              "hooks", "mappings", "limits", "geometry", "presentation_defaults", "payloads"}, "the manifest");
  if (need(root, "format").kind != Value::Kind::string || str(root, "format") != "segarecomp-patch") fail("E_FIELD_VALUE", "the manifest is not a segarecomp patch manifest");
  if (need(root, "schema_version").kind != Value::Kind::number) fail("E_FIELD_VALUE", "schema_version must be an integer");
  if (num(root, "schema_version") != 3) fail("E_SCHEMA_VERSION", "the manifest is not schema 3");
  Manifest m;
  m.id = str(root, "id");
  if (!id_ok(m.id)) fail("E_FIELD_VALUE", "the manifest id is invalid");
  m.version = str(root, "version");
  if (!semver_ok(m.version)) fail("E_FIELD_VALUE", "the manifest version must be MAJOR.MINOR.PATCH");
  m.name = str(root, "name");
  if (m.name.empty() || m.name.size() > 64 || !patch::detail::printable(m.name)) fail("E_FIELD_VALUE", "the manifest name is invalid");
  m.platform = str(root, "platform");
  const Adapter *adapter = find_adapter(m.platform);
  if (adapter == nullptr) fail("E_PLATFORM_UNKNOWN", "the platform \"" + m.platform + "\" is not known");
  if (root.find("author") != nullptr) {
    m.author = str(root, "author");
    if (m.author.empty() || m.author.size() > 64 || !patch::detail::printable(m.author)) fail("E_FIELD_VALUE", "the manifest author is invalid");
  }
  if (root.find("license") != nullptr) { m.license = str(root, "license"); if (!license_ok(m.license)) fail("E_FIELD_VALUE", "the manifest license is invalid"); }
  if (root.find("purpose") != nullptr) { m.purpose = str(root, "purpose"); if (!license_ok(m.purpose)) fail("E_FIELD_VALUE", "the manifest purpose is invalid"); }

  // source identity (section 5)
  const Value &source = object(need(root, "source"), "source");
  m.source_kind = str(source, "kind");
  if (m.source_kind != "cartridge-image" && m.source_kind != "disc-image") fail("E_FIELD_VALUE", "the source kind \"" + m.source_kind + "\" is not known");
  if (m.source_kind != adapter->source_kind) fail("E_SOURCE_PLATFORM", "the source kind does not match the platform");
  if (m.source_kind == "disc-image") {
    only(source, {"kind", "disc_identity"}, "a disc-image source");
    m.disc_identity = str(source, "disc_identity");
    if (!hex64(m.disc_identity)) fail("E_SOURCE_DIGEST", "disc_identity is not 64 lowercase hex digits");
    fail("E_SOURCE_KIND_RESERVED", "the disc-image source kind is reserved and not implemented");
  }
  only(source, {"kind", "revisions"}, "a cartridge-image source");
  const Value &revs = array(need(source, "revisions"), "revisions");
  if (revs.items.empty() || revs.items.size() > kMaxRevisions) fail("E_SOURCE_DIGEST", "a source declares 1 to 16 revisions");
  std::set<std::string> labels, digests;
  for (const Value &rv : revs.items) {
    object(rv, "a revision");
    only(rv, {"label", "sha256", "mapper"}, "a revision");
    Revision r;
    r.label = str(rv, "label"); r.sha256 = str(rv, "sha256");
    if (!name_token(r.label, 32)) fail("E_FIELD_VALUE", "a revision label must be a token");
    if (!hex64(r.sha256)) fail("E_SOURCE_DIGEST", "a revision sha256 is not 64 lowercase hex digits");
    if (!labels.insert(r.label).second || !digests.insert(r.sha256).second) fail("E_SOURCE_DIGEST", "a revision label or digest is repeated");
    if (m.platform == "master-system") {
      r.mapper = str(rv, "mapper");
      if (r.mapper != "sega" && r.mapper != "rom_only") fail("E_FIELD_VALUE", "the mapper must be sega or rom_only");
    } else if (rv.find("mapper") != nullptr) {
      fail("E_FIELD_UNKNOWN", "a revision mapper exists only on master-system");
    }
    m.revisions.push_back(std::move(r));
  }
  if (root.find("bios") != nullptr) fail("E_FIELD_UNKNOWN", "bios is forbidden for a cartridge-image source");

  // capabilities (section 6)
  const auto caps = [&](const char *key, std::vector<std::string> &out) {
    if (root.find(key) == nullptr) return;
    out = string_list(root.at(key), key);
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (find_capability(out[i]) == nullptr) fail("E_CAPABILITY_UNKNOWN", "the capability \"" + out[i] + "\" is not in the catalogue");
      if (i > 0 && !(out[i - 1] < out[i])) fail("E_FIELD_VALUE", std::string(key) + " must be sorted ascending without duplicates");
    }
  };
  caps("capabilities", m.capabilities);
  caps("capabilities_optional", m.capabilities_optional);
  for (const std::string &c : m.capabilities_optional)
    if (std::find(m.capabilities.begin(), m.capabilities.end(), c) != m.capabilities.end()) fail("E_FIELD_VALUE", "capabilities_optional repeats a required capability");

  // presentation defaults (as schema 1/2)
  if (const Value *d = root.find("presentation_defaults"); d != nullptr) {
    object(*d, "presentation_defaults");
    if (d->members.empty()) fail("E_FIELD_VALUE", "presentation_defaults is empty");
    only(*d, {"widescreen", "side_style"}, "presentation_defaults");
    if (d->find("widescreen") != nullptr) {
      m.default_widescreen = str(*d, "widescreen");
      if (!is_in(m.default_widescreen, {"original", "extended_backgrounds", "enhanced_gameplay"})) fail("E_FIELD_VALUE", "the presentation_defaults widescreen value is not supported");
    }
    if (d->find("side_style") != nullptr) {
      m.default_side_style = str(*d, "side_style");
      if (!is_in(m.default_side_style, {"comfort", "borders", "raw"})) fail("E_FIELD_VALUE", "the presentation_defaults side_style value is not supported");
    }
  }

  // payloads (section 4). A native-c11 payload without author/abi is a trust failure before any other payload field is judged (schema 3 section 9, vector B).
  const Value &payloads = array(need(root, "payloads"), "payloads");
  for (const Value &pv : payloads.items)
    if (pv.kind == Value::Kind::object && pv.find("kind") != nullptr && pv.at("kind").kind == Value::Kind::string && pv.at("kind").text == "native-c11" && (root.find("author") == nullptr || root.find("abi") == nullptr))
      fail("E_TRUST_UNDECLARED", "a native-c11 payload needs author and abi");
  if (payloads.items.empty() || payloads.items.size() > kMaxPayloads) fail("E_FIELD_VALUE", "payloads holds 1 to 255 entries");
  std::set<std::string> folded;
  for (const Value &pv : payloads.items) {
    Payload p = parse_payload(pv, m.platform);
    std::string lower = p.path;
    for (char &c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (p.path == "manifest.json" || !folded.insert(lower).second) fail("E_PAYLOAD_DUPLICATE", "a payload path is repeated");
    m.payloads.push_back(std::move(p));
  }

  // reserved keys: present but empty only
  if (const Value *h = root.find("hooks"); h != nullptr) {
    if (!array(*h, "hooks").items.empty()) fail("E_FIELD_UNKNOWN", "hooks entries are not defined yet");
    m.has_hooks = true;
  }
  if (const Value *g = root.find("geometry"); g != nullptr) {
    object(*g, "geometry");
    only(*g, {"scenes"}, "geometry");
    if (g->find("scenes") != nullptr && !array(g->at("scenes"), "scenes").items.empty()) fail("E_FIELD_UNKNOWN", "geometry scenes are not defined yet");
    m.has_geometry = true;
  }
  if (const Value *l = root.find("limits"); l != nullptr) {
    object(*l, "limits");
    only(*l, {"luau_memory_bytes", "luau_budget_units"}, "limits");
    if (l->find("luau_memory_bytes") != nullptr) m.luau_memory_bytes = num(*l, "luau_memory_bytes");
    if (l->find("luau_budget_units") != nullptr) m.luau_budget_units = num(*l, "luau_budget_units");
    if (l->members.empty() || m.luau_memory_bytes > kLuauMemoryPolicyMax || m.luau_budget_units > kLuauBudgetPolicyMax ||
        (l->find("luau_memory_bytes") != nullptr && m.luau_memory_bytes == 0) || (l->find("luau_budget_units") != nullptr && m.luau_budget_units == 0))
      fail("E_LIMIT_EXCEEDS_POLICY", "a requested limit is empty, zero or above the policy default");
    m.has_limits = true;
  }

  const bool native = has_kind(m, "native-c11");
  if (const Value *a = root.find("abi"); a != nullptr) {
    object(*a, "abi");
    only(*a, {"native"}, "abi");
    const std::uint64_t v = num(*a, "native");
    if (!native) fail("E_FIELD_UNKNOWN", "abi is valid only with a native-c11 payload");
    if (v == 1) fail("E_FIELD_VALUE", "a schema 3 package declares ABI 2; ABI 1 stays schema 2");
    if (v != kNativeAbiMajor) fail("E_ABI_UNSUPPORTED", "the native ABI " + std::to_string(v) + " is not provided by this release");
    m.abi_native = kNativeAbiMajor;
  }
  if (native && (m.author.empty() || m.abi_native == 0)) fail("E_TRUST_UNDECLARED", "a native-c11 payload needs author and abi");
  if (const Value *s = root.find("state_schema"); s != nullptr) {
    if (s->kind != Value::Kind::number || s->number == 0) fail("E_FIELD_VALUE", "state_schema must be 1..2147483647");
    if (!has_capability(m, "state.mod")) fail("E_FIELD_UNKNOWN", "state_schema is valid only with the state.mod capability");
    m.state_schema = static_cast<unsigned>(s->number);
  } else if (has_capability(m, "state.mod")) {
    fail("E_FIELD_MISSING", "state.mod needs state_schema");
  }

  if (const Value *mv = root.find("mappings"); mv != nullptr) {
    array(*mv, "mappings");
    if (mv->items.empty() || mv->items.size() > kMaxMappings) fail("E_FIELD_VALUE", "mappings holds 1 to 64 entries");
    std::set<std::string> ids;
    for (const Value &e : mv->items) {
      Mapping mp = parse_mapping(e);
      if (!ids.insert(mp.id).second) fail("E_FIELD_VALUE", "a mapping id is repeated");
      m.mappings.push_back(std::move(mp));
    }
  }

  // combination rules (section 4)
  if (std::count_if(m.payloads.begin(), m.payloads.end(), [](const Payload &p) { return p.kind == "bps-image"; }) > 1) fail("E_PAYLOAD_COMBINATION", "at most one bps-image payload");
  if (has_kind(m, "bps-image") && (has_kind(m, "rom-edit-set") || has_kind(m, "guest-code"))) fail("E_PAYLOAD_COMBINATION", "bps-image cannot coexist with rom-edit-set or guest-code");
  const bool code = native || has_kind(m, "luau-source");
  for (const std::string &name : m.capabilities) {   // every capability must be consumable by a payload
    const CapabilityInfo *c = find_capability(name);
    if (!code || (!c->luau_bound && !native)) fail("E_CAPABILITY_INCONSISTENT", "the capability \"" + name + "\" needs a " + (c->luau_bound ? "native-c11 or luau-source" : "native-c11") + " payload");
  }
  for (const std::string &name : m.capabilities_optional)
    if (!code) fail("E_CAPABILITY_INCONSISTENT", "the capability \"" + name + "\" needs a native-c11 or luau-source payload");
  if (m.has_hooks && !has_capability(m, "hook.function")) fail("E_CAPABILITY_INCONSISTENT", "hooks needs the hook.function capability");
  if (m.has_limits && !has_kind(m, "luau-source")) fail("E_CAPABILITY_INCONSISTENT", "limits apply only to luau-source payloads");
  bool audio = false;
  for (const Payload &p : m.payloads) audio = audio || p.asset_class == "audio";
  if (audio && m.license.empty()) fail("E_FIELD_MISSING", "a package with audio assets must state its license");

  for (const Mapping &mp : m.mappings) {
    if (mp.kind == "command" && !has_capability(m, "audio.command.send")) fail("E_CAPABILITY_INCONSISTENT", "a command mapping needs audio.command.send");
    if (mp.kind == "event" && !has_capability(m, "audio.event.read")) fail("E_CAPABILITY_INCONSISTENT", "an event mapping needs audio.event.read");
    if (mp.kind != "asset-binding") continue;
    const Payload *asset = find_payload(m, mp.asset);
    if (asset == nullptr || asset->kind != "asset") fail("E_FIELD_VALUE", "an asset-binding names a path that is not a declared asset");
    const bool pcm = mp.use == "pcm-sample" || mp.use == "enhanced-soundtrack";
    if (pcm ? asset->asset_class != "audio" : asset->asset_class != "data") fail("E_FIELD_VALUE", "the asset class does not fit the declared use");
    if (!mp.revision.empty() && labels.count(mp.revision) == 0) fail("E_SOURCE_DIGEST", "an asset-binding names a revision that is not declared");
    if (mp.use == "enhanced-soundtrack" && !has_capability(m, "audio.host.play")) fail("E_CAPABILITY_INCONSISTENT", "an enhanced-soundtrack binding needs audio.host.play");
  }
  return m;
}

// ---- serialization ----
void put_string(std::string &out, const std::string &s) {
  out.push_back('"');
  for (const char c : s) { if (c == '"' || c == '\\') out.push_back('\\'); out.push_back(c); }
  out.push_back('"');
}
void key(std::string &out, const char *k) { out.push_back('"'); out += k; out += "\":"; }
void kv(std::string &out, const char *k, const std::string &v, bool first = false) { if (!first) out.push_back(','); key(out, k); put_string(out, v); }
void kn(std::string &out, const char *k, std::uint64_t v) { out.push_back(','); key(out, k); out += std::to_string(v); }
void string_array(std::string &out, const std::vector<std::string> &v) {
  out.push_back('[');
  for (std::size_t i = 0; i < v.size(); ++i) { if (i) out.push_back(','); put_string(out, v[i]); }
  out.push_back(']');
}

void emit_payload(std::string &out, const Payload &p) {
  out += '{';
  kv(out, "kind", p.kind, true);
  kn(out, "kind_version", p.kind_version);
  kv(out, "path", p.path);
  kn(out, "size", p.size);
  kv(out, "sha256", p.sha256);
  if (p.kind == "bps-image") kv(out, "target_sha256", p.target_sha256);
  else if (p.kind == "rom-edit-set") kn(out, "edits", p.edits);
  else if (p.kind == "guest-code") kv(out, "cpu", p.cpu);
  else if (p.kind == "native-c11" || p.kind == "luau-source") kv(out, "role", p.role);
  else {
    kv(out, "asset_class", p.asset_class);
    if (p.asset_class == "image") { kn(out, "width", p.width); kn(out, "height", p.height); kv(out, "format", p.format); }
    else if (p.asset_class == "data") kv(out, "format", p.format);
    else {
      kv(out, "audio_format", p.audio_format); kn(out, "sample_rate", p.sample_rate); kn(out, "channels", p.channels); kn(out, "frames", p.frames);
      if (p.has_loop) { kn(out, "loop_start", p.loop_start); kn(out, "loop_end", p.loop_end); }
    }
  }
  out += '}';
}

void emit_mapping(std::string &out, const Mapping &m) {
  out += '{';
  kv(out, "id", m.id, true);
  kv(out, "kind", m.kind);
  if (m.kind == "asset-binding") {
    kv(out, "asset", m.asset); kv(out, "target", m.target); kv(out, "use", m.use);
    if (!m.revision.empty()) kv(out, "revision", m.revision);
    if (m.has_site) { kn(out, "offset", m.site_offset); kn(out, "length", m.site_length); }
    if (m.has_bank) kn(out, "bank", m.bank);
    if (m.has_track) kn(out, "track", m.track);
  } else {
    kv(out, "space", m.space); kn(out, "offset", m.offset); kn(out, "width", m.width);
    out += ','; key(out, m.kind == "command" ? "commands" : "events"); out += '[';
    for (std::size_t i = 0; i < m.values.size(); ++i) {
      if (i) out += ',';
      out += '{'; kv(out, "name", m.values[i].first, true); kn(out, "value", m.values[i].second); out += '}';
    }
    out += ']';
  }
  out += '}';
}

std::uint32_t crc(std::span<const std::uint8_t> b) { return patch::crc32(b); }
void le16(std::vector<std::uint8_t> &o, std::uint32_t v) { o.push_back(static_cast<std::uint8_t>(v)); o.push_back(static_cast<std::uint8_t>(v >> 8U)); }
void le32(std::vector<std::uint8_t> &o, std::uint32_t v) { le16(o, v & 0xFFFFU); le16(o, v >> 16U); }

Package rejected(std::string code, std::string message) {
  Package p;
  p.rejection = Rejection{std::move(code), std::move(message)};
  return p;
}

}  // namespace

std::string trust_class(const Manifest &m) {
  if (has_kind(m, "native-c11")) return "native";
  return has_kind(m, "luau-source") ? "luau" : "data";
}

std::string serialize_manifest(const Manifest &m) {
  std::string o = "{";
  kv(o, "format", "segarecomp-patch", true);
  kn(o, "schema_version", 3);
  kv(o, "id", m.id); kv(o, "version", m.version); kv(o, "name", m.name); kv(o, "platform", m.platform);
  if (!m.author.empty()) kv(o, "author", m.author);
  if (!m.license.empty()) kv(o, "license", m.license);
  if (!m.purpose.empty()) kv(o, "purpose", m.purpose);
  o += ','; key(o, "source"); o += '{'; kv(o, "kind", m.source_kind, true);
  if (m.source_kind == "disc-image") {
    kv(o, "disc_identity", m.disc_identity);
  } else {
    o += ','; key(o, "revisions"); o += '[';
    for (std::size_t i = 0; i < m.revisions.size(); ++i) {
      if (i) o += ',';
      o += '{'; kv(o, "label", m.revisions[i].label, true); kv(o, "sha256", m.revisions[i].sha256);
      if (!m.revisions[i].mapper.empty()) kv(o, "mapper", m.revisions[i].mapper);
      o += '}';
    }
    o += ']';
  }
  o += '}';
  if (!m.capabilities.empty()) { o += ','; key(o, "capabilities"); string_array(o, m.capabilities); }
  if (!m.capabilities_optional.empty()) { o += ','; key(o, "capabilities_optional"); string_array(o, m.capabilities_optional); }
  if (m.abi_native != 0) { o += ','; key(o, "abi"); o += "{\"native\":" + std::to_string(m.abi_native) + "}"; }
  if (m.state_schema != 0) kn(o, "state_schema", m.state_schema);
  if (m.has_hooks) { o += ','; key(o, "hooks"); o += "[]"; }
  if (!m.mappings.empty()) {
    o += ','; key(o, "mappings"); o += '[';
    for (std::size_t i = 0; i < m.mappings.size(); ++i) { if (i) o += ','; emit_mapping(o, m.mappings[i]); }
    o += ']';
  }
  if (m.has_limits) {
    o += ','; key(o, "limits"); o += '{';
    bool first = true;
    if (m.luau_memory_bytes != 0) { key(o, "luau_memory_bytes"); o += std::to_string(m.luau_memory_bytes); first = false; }
    if (m.luau_budget_units != 0) { if (!first) o += ','; key(o, "luau_budget_units"); o += std::to_string(m.luau_budget_units); }
    o += '}';
  }
  if (m.has_geometry) { o += ','; key(o, "geometry"); o += "{\"scenes\":[]}"; }
  if (!m.default_widescreen.empty() || !m.default_side_style.empty()) {
    o += ','; key(o, "presentation_defaults"); o += '{';
    if (!m.default_widescreen.empty()) kv(o, "widescreen", m.default_widescreen, true);
    if (!m.default_side_style.empty()) kv(o, "side_style", m.default_side_style, m.default_widescreen.empty());
    o += '}';
  }
  o += ','; key(o, "payloads"); o += '[';
  for (std::size_t i = 0; i < m.payloads.size(); ++i) { if (i) o += ','; emit_payload(o, m.payloads[i]); }
  o += "]}";
  return o;
}

std::string content_digest(const Manifest &m) {
  std::vector<const Payload *> sorted;
  for (const Payload &p : m.payloads) sorted.push_back(&p);
  std::sort(sorted.begin(), sorted.end(), [](const Payload *a, const Payload *b) { return a->path < b->path; });
  std::string text = "segarecomp.mod.content.v1\nplatform " + m.platform + "\n";
  for (const Payload *p : sorted)
    text += "payload " + p->kind + " " + std::to_string(p->kind_version) + " " + p->path + " size " + std::to_string(p->size) + " sha256 " + p->sha256 + "\n";
  return sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

Package read_package(std::span<const std::uint8_t> zip) {
  try {
    patch::detail::ZipPolicy policy;
    policy.max_entries = kMaxArchiveEntries;
    policy.manifest_limit = kMaxImageBytes;   // the 64 KiB manifest cap is E_MANIFEST_SYNTAX below; the archive layer only bounds memory
    policy.entry_limit = kMaxImageBytes;
    policy.total_limit = 48U << 20U;
    policy.max_ratio = 256;
    policy.fold_case_duplicates = true;
    policy.name_ok = [](const std::string &n) { return v3_path_ok(n); };
    std::map<std::string, patch::detail::ZipEntry> entries;
    std::string error;
    const char *code = "E_ARCHIVE_ENTRY";
    if (zip.size() > kMaxArchiveBytes) return rejected("E_ARCHIVE_SIZE", "the archive is larger than allowed");
    if (!patch::detail::read_zip(zip, policy, entries, error, &code)) return rejected(code, error);
    const auto man = entries.find("manifest.json");
    if (man == entries.end()) return rejected("E_ARCHIVE_ENTRY", "the package has no manifest.json");
    if (man->second.data.size() > kMaxManifestBytes) return rejected("E_MANIFEST_SYNTAX", "the manifest is larger than allowed");
    JsonLimits limits;
    limits.max_items = kMaxPayloads; limits.max_members = 32; limits.max_depth = 5; limits.max_number = 2147483647U;
    limits.allow_slash_escape = false; limits.allow_whitespace = false;
    Value root;
    std::string json_error;
    if (!patch::detail::parse_json(std::string_view(reinterpret_cast<const char *>(man->second.data.data()), man->second.data.size()), limits, root, json_error))
      return rejected("E_MANIFEST_SYNTAX", json_error);
    Package out;
    out.manifest = parse_manifest(root);
    const Manifest &m = out.manifest;

    // archive entries must be exactly the manifest plus the declared payloads
    std::set<std::string> declared;
    for (const Payload &p : m.payloads) declared.insert(p.path);
    for (const auto &e : entries) if (e.first != "manifest.json" && declared.count(e.first) == 0) return rejected("E_ARCHIVE_ENTRY", "the archive holds an entry the manifest does not name");
    // per-kind caps and verification of every payload
    std::size_t native_n = 0, native_total = 0, luau_n = 0, luau_total = 0, asset_n = 0, guest_total = 0;
    for (const Payload &p : m.payloads) {
      const auto it = entries.find(p.path);
      if (it == entries.end()) return rejected("E_ARCHIVE_ENTRY", "a payload named by the manifest is missing from the archive");
      if (it->second.data.size() != p.size) return rejected("E_ARCHIVE_SIZE", "a payload size differs from the manifest");
      if (sha256_hex(it->second.data) != p.sha256) return rejected("E_PAYLOAD_DIGEST", "a payload SHA-256 differs from the manifest");
      const std::size_t n = it->second.data.size();
      const auto cap = [&](bool ok) { if (!ok) fail("E_PAYLOAD_CAP", "the payload \"" + p.path + "\" exceeds its kind cap"); };
      if (p.kind == "bps-image") cap(n <= kMaxImageBytes);
      else if (p.kind == "rom-edit-set") { cap(n <= kMaxRomEditBytes && p.edits <= kMaxRomEdits); }
      else if (p.kind == "guest-code") { guest_total += n; cap(guest_total <= kMaxGuestCodeBytes); }
      else if (p.kind == "native-c11") { native_total += n; cap(++native_n <= kMaxNativeFiles && n <= kMaxNativeFileBytes && native_total <= kMaxNativeTotalBytes); }
      else if (p.kind == "luau-source") {
        luau_total += n; cap(++luau_n <= kMaxLuauFiles && n <= kMaxLuauFileBytes && luau_total <= kMaxLuauTotalBytes);
        for (const std::uint8_t b : it->second.data) if (b == 0 || (b < 0x20 && b != '\t' && b != '\n' && b != '\r') || b >= 0x80) fail("E_LUAU_BYTECODE", "a luau-source payload is not Luau text");
      } else { cap(++asset_n <= kMaxAssets && n <= kMaxAssetBytes); }
      if (p.kind == "asset" && p.asset_class == "audio") {
        const Wav w = parse_wav_pcm_s16le(it->second.data);
        if (!w.ok) fail("E_FIELD_VALUE", "an audio asset is not a valid wav-pcm-s16le file: " + w.error);
        if (w.sample_rate != p.sample_rate || w.channels != p.channels || w.frames != p.frames) fail("E_FIELD_VALUE", "an audio asset does not match its declared rate, channels or frames");
      }
      out.files.emplace(p.path, it->second.data);
    }
    out.identity = sha256_hex(man->second.data);
    out.content_digest = content_digest(m);
    out.ok = true;
    return out;
  } catch (const Fail &f) {
    return rejected(f.code, f.message);
  }
}

Package read_package_file(const std::filesystem::path &path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return rejected("E_ARCHIVE_ENTRY", "the package is not a regular file");
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxArchiveBytes) return rejected("E_ARCHIVE_SIZE", "the package is unreadable or larger than allowed");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream in(path, std::ios::binary);
  if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return rejected("E_ARCHIVE_ENTRY", "the package cannot be read");
  return read_package(bytes);
}

std::vector<std::uint8_t> pack_package(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files) {
  const std::string mtext = serialize_manifest(manifest);
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  entries.emplace_back("manifest.json", std::vector<std::uint8_t>(mtext.begin(), mtext.end()));
  for (const auto &f : files) entries.emplace_back(f.first, f.second);   // std::map iterates in byte order of the path
  std::vector<std::uint8_t> out, directory;
  for (const auto &[name, data] : entries) {
    const std::uint32_t offset = static_cast<std::uint32_t>(out.size()), c = crc(data), size = static_cast<std::uint32_t>(data.size());
    const auto header = [&](std::vector<std::uint8_t> &o, bool central) {
      le32(o, central ? 0x02014B50U : 0x04034B50U);
      if (central) le16(o, 20);          // version made by (DOS, 2.0)
      le16(o, 20); le16(o, 0); le16(o, 0);   // version needed, flags, method stored
      le16(o, 0); le16(o, 0x21);         // DOS time 00:00:00, date 1980-01-01
      le32(o, c); le32(o, size); le32(o, size);
      le16(o, static_cast<std::uint32_t>(name.size())); le16(o, 0);
      if (central) { le16(o, 0); le16(o, 0); le16(o, 0); le32(o, 0); le32(o, offset); }
      o.insert(o.end(), name.begin(), name.end());
    };
    header(out, false);
    out.insert(out.end(), data.begin(), data.end());
    header(directory, true);
  }
  const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
  out.insert(out.end(), directory.begin(), directory.end());
  le32(out, 0x06054B50U); le16(out, 0); le16(out, 0);
  le16(out, static_cast<std::uint32_t>(entries.size())); le16(out, static_cast<std::uint32_t>(entries.size()));
  le32(out, static_cast<std::uint32_t>(directory.size())); le32(out, cd_offset); le16(out, 0);
  return out;
}

namespace {
// Space sizes and guest byte order (mod-platform-adapters.md section 3). Big-endian spaces require width-aligned offsets for 16/32-bit accesses.
struct SpaceInfo {
  std::string_view name;
  std::uint32_t bytes;
  bool big_endian;
};
constexpr SpaceInfo kSpaces[] = {{"m68k.work_ram", 65536, true}, {"z80.ram", 8192, false}, {"z80.work_ram", 8192, false}, {"z80.cart_ram", 32768, false},
                                 {"main.work_ram", 65536, true}, {"main.word_ram", 262144, true}, {"sub.prg_ram", 524288, true}};
}  // namespace

std::optional<Rejection> check_adapter(const Manifest &m) {
  const Adapter *adapter = find_adapter(m.platform);
  if (adapter == nullptr) return Rejection{"E_PLATFORM_UNKNOWN", "the platform \"" + m.platform + "\" has no registered adapter"};
  if (m.source_kind != adapter->source_kind) return Rejection{"E_SOURCE_PLATFORM", "the source kind does not match the platform"};
  if (auto r = check_capabilities(*adapter, m.capabilities, m.capabilities_optional)) return r;
  const auto in_list = [](std::span<const std::string_view> list, const std::string &s) { return std::find(list.begin(), list.end(), s) != list.end(); };
  for (const Mapping &mp : m.mappings) {
    if (mp.kind != "asset-binding" && std::none_of(std::begin(kSpaces), std::end(kSpaces), [&](const SpaceInfo &sp) { return sp.name == mp.space; }))
      return Rejection{"E_FIELD_VALUE", "the mapping space \"" + mp.space + "\" is not a known space"};
    if (mp.kind == "command" && mp.space != adapter->writable_space) return Rejection{"E_SPACE_READONLY", "the command mapping space \"" + mp.space + "\" is not mod-writable on " + m.platform};
    if (mp.kind == "event" && !in_list(adapter->readable_spaces, mp.space)) return Rejection{"E_SPACE_UNKNOWN", "the event mapping space \"" + mp.space + "\" is not readable on " + m.platform};
    if (mp.kind != "asset-binding") {
      for (const SpaceInfo &sp : kSpaces) {
        if (sp.name != mp.space) continue;
        if (static_cast<std::uint64_t>(mp.offset) + mp.width > sp.bytes) return Rejection{"E_ACCESS_RANGE", "the mapping \"" + mp.id + "\" lies outside " + mp.space};
        if (sp.big_endian && mp.width > 1 && mp.offset % mp.width != 0) return Rejection{"E_ACCESS_ALIGNMENT", "the mapping \"" + mp.id + "\" is not aligned to its width"};
      }
    }
    if (mp.kind == "asset-binding" && (mp.use == "pcm-sample" || mp.use == "enhanced-soundtrack") && adapter->audio_conversions.empty())
      return Rejection{"E_CAPABILITY_UNSUPPORTED", "this platform has no PCM adapter for the declared audio use"};
  }
  return std::nullopt;
}

}  // namespace segarecomp::mod
