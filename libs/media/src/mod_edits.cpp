// Typed ROM edits and per-revision symbol resolution (SEG-136-T001/T002, ADR 0290; format in mod.hpp). Pure byte/text processing: no file, no clock, no host byte order, so every host
// produces the identical bytes. Diagnostics name the line and class of a failure but never print image bytes.
#include "segarecomp/mod.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <limits>

namespace segarecomp::mod {

namespace {

struct Fail {
  std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) { throw Fail{std::move(code), std::move(message)}; }
std::string at(std::size_t line) { return "line " + std::to_string(line) + ": "; }

int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

// Lower-case hex digits only (1..16).
bool hex_value(std::string_view s, std::uint64_t &out) {
  if (s.empty() || s.size() > 16) return false;
  out = 0;
  for (const char c : s) {
    const int d = hex_digit(c);
    if (d < 0) return false;
    out = (out << 4U) | static_cast<std::uint64_t>(d);
  }
  return true;
}

bool literal_hex(std::string_view token, std::size_t max_digits, std::uint64_t &out) {   // `0x` + 1..max_digits digits
  return token.size() > 2 && token.substr(0, 2) == "0x" && token.size() - 2 <= max_digits && hex_value(token.substr(2), out);
}

std::string hex_string(std::uint64_t v) {
  std::string out;
  do { out.insert(out.begin(), "0123456789abcdef"[v & 15U]); v >>= 4U; } while (v != 0);
  return out;
}

std::vector<std::string_view> split_spaces(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

bool bytes_from_hex(std::string_view s, std::vector<std::uint8_t> &out) {
  if (s.empty() || s.size() % 2 != 0 || s.size() / 2 > kMaxRomEditBytes) return false;
  out.clear();
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const int hi = hex_digit(s[i]), lo = hex_digit(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return true;
}

// An integer of `width` bytes in the requested byte order, as image bytes.
std::vector<std::uint8_t> encode_int(std::uint64_t v, std::size_t width, bool big) {
  std::vector<std::uint8_t> out(width);
  for (std::size_t i = 0; i < width; ++i) {
    const std::size_t shift = 8U * (big ? width - 1 - i : i);
    out[i] = static_cast<std::uint8_t>(v >> shift);
  }
  return out;
}

bool has_revision(const Manifest &m, std::string_view label) {
  return std::any_of(m.revisions.begin(), m.revisions.end(), [&](const Revision &r) { return r.label == label; });
}

RomEdit parse_line(const Manifest &m, std::size_t line_no, std::string_view line, bool default_big) {
  const std::vector<std::string_view> t = split_spaces(line);
  if (t.size() != 5) fail("E_EDIT_SYNTAX", at(line_no) + "an edit has exactly 5 fields: revision type site expected value");
  RomEdit e;
  e.line = line_no;
  if (t[0] != "*") {
    e.revision = std::string(t[0]);
    if (!has_revision(m, e.revision)) fail("E_EDIT_REVISION", at(line_no) + "the revision \"" + e.revision + "\" is not declared");
  }
  // type
  const std::string_view type = t[1];
  bool bytes = false, big = default_big;
  std::size_t width = 0;
  if (type == "u8") width = 1;
  else if (type == "u16") width = 2;
  else if (type == "u32") width = 4;
  else if (type == "u16be" || type == "u16le") { width = 2; big = type == "u16be"; }
  else if (type == "u32be" || type == "u32le") { width = 4; big = type == "u32be"; }
  else if (type == "bytes") bytes = true;
  else fail("E_EDIT_SYNTAX", at(line_no) + "the edit type is not one of u8 u16 u32 u16be u16le u32be u32le bytes");
  // site
  std::uint64_t n = 0;
  if (!t[2].empty() && t[2][0] == '@') {
    std::string_view name = t[2].substr(1);
    const std::size_t plus = name.find('+');
    if (plus != std::string_view::npos) {
      if (!literal_hex(name.substr(plus + 1), 8, n)) fail("E_EDIT_SYNTAX", at(line_no) + "a symbol addend is 0x followed by 1 to 8 lower-case hex digits");
      e.site = static_cast<std::uint32_t>(n);
      name = name.substr(0, plus);
    }
    if (!symbol_name_ok(name)) fail("E_EDIT_SYNTAX", at(line_no) + "the symbol name is not a valid token");
    e.symbol = std::string(name);
  } else {
    if (!literal_hex(t[2], 8, n)) fail("E_EDIT_SYNTAX", at(line_no) + "a site is 0x followed by 1 to 8 lower-case hex digits, or @symbol[+0xN]");
    e.site = static_cast<std::uint32_t>(n);
  }
  // values
  if (bytes) {
    if (!bytes_from_hex(t[3], e.expected) || !bytes_from_hex(t[4], e.value)) fail("E_EDIT_SYNTAX", at(line_no) + "bytes values are non-empty lower-case hex of even length");
    if (e.expected.size() != e.value.size()) fail("E_EDIT_SYNTAX", at(line_no) + "expected and value must have the same length");
  } else {
    std::uint64_t ex = 0, va = 0;
    const bool ok = t[3].size() == 2 + 2 * width && t[4].size() == 2 + 2 * width && literal_hex(t[3], 2 * width, ex) && literal_hex(t[4], 2 * width, va);
    if (!ok) fail("E_EDIT_SYNTAX", at(line_no) + "an integer value is 0x followed by exactly " + std::to_string(2 * width) + " lower-case hex digits");
    e.expected = encode_int(ex, width, big);
    e.value = encode_int(va, width, big);
  }
  return e;
}

struct Resolved {
  std::uint64_t offset;
  const RomEdit *edit;
};

// The edits that apply to one declared revision, resolved to image offsets, ordered and checked for overlap. Independent of every other revision.
std::vector<Resolved> resolve(const Manifest &m, const RomEditSet &set, const std::string &revision) {
  std::vector<Resolved> out;
  for (const RomEdit &e : set.edits) {
    if (!e.revision.empty() && e.revision != revision) continue;
    std::uint64_t offset = e.site;
    if (!e.symbol.empty()) {
      const Symbol *found = nullptr;
      for (const Symbol &s : m.symbols) if (s.name == e.symbol && s.revision == revision) found = &s;
      if (found == nullptr) fail("E_SYMBOL_UNKNOWN", at(e.line) + "the symbol \"" + e.symbol + "\" has no entry for the revision \"" + revision + "\"");
      if (found->provenance != "verified")
        fail("E_SYMBOL_NOT_VERIFIED", at(e.line) + "the symbol \"" + e.symbol + "\" is " + found->provenance + " for the revision \"" + revision + "\" and cannot be a patch site");
      offset = static_cast<std::uint64_t>(found->address) + e.site;
    }
    if (offset + e.value.size() > kMaxImageBytes) fail("E_EDIT_RANGE", at(e.line) + "the edit lies beyond the largest supported image");
    out.push_back({offset, &e});
  }
  std::stable_sort(out.begin(), out.end(), [](const Resolved &a, const Resolved &b) { return a.offset < b.offset; });
  for (std::size_t i = 1; i < out.size(); ++i)
    if (out[i - 1].offset + out[i - 1].edit->value.size() > out[i].offset)
      fail("E_EDIT_OVERLAP", at(out[i].edit->line) + "the edit overlaps the edit of line " + std::to_string(out[i - 1].edit->line) + " in the revision \"" + revision + "\"");
  return out;
}

RomEditSet parse_set(std::span<const std::uint8_t> text, const Manifest &m) {
  const Adapter *adapter = find_adapter(m.platform);
  const bool big = adapter == nullptr || adapter->rom_big_endian;
  RomEditSet set;
  std::size_t line_no = 1, start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const std::uint8_t b = text[i];
    if (b != '\n' && (b < 0x20 || b > 0x7E)) fail("E_EDIT_SYNTAX", at(line_no) + "an edit file is ASCII text with LF line ends");
    if (b == '\n') ++line_no;
  }
  line_no = 1;
  const std::string_view all(reinterpret_cast<const char *>(text.data()), text.size());
  while (start <= all.size()) {
    const std::size_t nl = all.find('\n', start);
    const std::string_view line = all.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    const std::size_t first = line.find_first_not_of(' ');
    if (first != std::string_view::npos && line[first] != '#') {
      if (set.edits.size() >= kMaxRomEdits) fail("E_PAYLOAD_CAP", "a rom-edit-set holds at most 4096 edits");
      set.edits.push_back(parse_line(m, line_no, line, big));
    }
    if (nl == std::string_view::npos) break;
    start = nl + 1;
    ++line_no;
  }
  set.ok = true;
  return set;
}

}  // namespace

bool symbol_name_ok(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; });
}

RomEditSet parse_rom_edit_set(std::span<const std::uint8_t> text, const Manifest &manifest) {
  try {
    return parse_set(text, manifest);
  } catch (const Fail &f) {
    RomEditSet r;
    r.rejection = {f.code, f.message};
    return r;
  }
}

std::optional<Rejection> check_rom_edit_set(const Manifest &m, std::span<const std::uint8_t> text, unsigned declared_edits) {
  try {
    const RomEditSet set = parse_set(text, m);
    if (set.edits.size() != declared_edits) fail("E_EDIT_COUNT", "the payload holds " + std::to_string(set.edits.size()) + " edits but declares " + std::to_string(declared_edits));
    for (const Revision &r : m.revisions) (void)resolve(m, set, r.label);
    return std::nullopt;
  } catch (const Fail &f) {
    return Rejection{f.code, f.message};
  }
}

EditedImage apply_rom_edits(const Manifest &m, std::span<const std::uint8_t> text, std::string_view revision, std::span<const std::uint8_t> original) {
  EditedImage out;
  try {
    const auto rev = std::find_if(m.revisions.begin(), m.revisions.end(), [&](const Revision &r) { return r.label == revision; });
    if (rev == m.revisions.end()) fail("E_EDIT_REVISION", "the revision \"" + std::string(revision) + "\" is not declared");
    if (sha256_hex(original) != rev->sha256) fail("E_IMAGE_MISMATCH", "the image is not the exact image of the revision \"" + rev->label + "\"");
    const RomEditSet set = parse_set(text, m);
    const std::vector<Resolved> plan = resolve(m, set, rev->label);
    for (const Resolved &r : plan) {   // assert everything before changing anything
      if (r.offset + r.edit->expected.size() > original.size()) fail("E_EDIT_RANGE", at(r.edit->line) + "the edit lies beyond the end of the image");
      if (!std::equal(r.edit->expected.begin(), r.edit->expected.end(), original.begin() + static_cast<std::ptrdiff_t>(r.offset)))
        fail("E_EDIT_EXPECTED_MISMATCH", at(r.edit->line) + "the original bytes at offset 0x" + hex_string(r.offset) + " differ from the expected value");
    }
    out.image.assign(original.begin(), original.end());
    for (const Resolved &r : plan) std::copy(r.edit->value.begin(), r.edit->value.end(), out.image.begin() + static_cast<std::ptrdiff_t>(r.offset));
    out.applied = static_cast<unsigned>(plan.size());
    out.ok = true;
  } catch (const Fail &f) {
    out.rejection = {f.code, f.message};
    out.image.clear();
  }
  return out;
}

SymbolHints parse_symbol_hints(std::string_view text, std::string_view revision) {
  SymbolHints out;
  const auto bad = [&](std::string code, std::string msg) { out.symbols.clear(); out.rejection = {std::move(code), std::move(msg)}; return out; };
  std::size_t line_no = 0, start = 0;
  bool first_data = true;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    std::string line(text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
    start = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::vector<std::string> tok;
    std::string cur;
    for (const char c : line) {
      if (c == '"' || c == '\r') continue;
      if (c == ',' || c == ' ' || c == '\t') { if (!cur.empty()) tok.push_back(std::move(cur)); cur.clear(); } else cur.push_back(c);
    }
    if (!cur.empty()) tok.push_back(cur);
    if (tok.empty()) continue;
    if (tok.size() < 2) return bad("E_SYMBOL_HINT", at(line_no) + "a hint line is a name and an address");
    // address: 0x hex, $ hex or a decimal without leading zeros (a bare zero-padded token is ambiguous and refused)
    const std::string &a = tok[1];
    std::uint64_t v = 0;
    bool ok = false;
    if (a.size() > 2 && (a.substr(0, 2) == "0x" || a.substr(0, 2) == "0X")) {
      std::string low = a.substr(2);
      for (char &c : low) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
      ok = low.size() <= 8 && hex_value(low, v);
    } else if (a.size() > 1 && a[0] == '$') {
      std::string low = a.substr(1);
      for (char &c : low) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
      ok = low.size() <= 8 && hex_value(low, v);
    } else if (!a.empty() && a.size() <= 10 && (a.size() == 1 || a[0] != '0') && std::all_of(a.begin(), a.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      v = std::stoull(a);
      ok = v <= std::numeric_limits<std::uint32_t>::max();
    }
    if (ok && v >= kMaxImageBytes) return bad("E_SYMBOL_HINT", at(line_no) + "the address lies beyond the largest supported image");
    if (!ok) {
      if (first_data && (tok[0] == "Name" || tok[0] == "name" || tok[0] == "Symbol" || tok[0] == "symbol")) { first_data = false; continue; }   // only a header row such as `Name,Location,...`
      return bad("E_SYMBOL_HINT", at(line_no) + "the address is not 0x hex, $ hex or plain decimal");
    }
    first_data = false;
    std::string name = tok[0];
    for (char &c : name) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) c = '_';
    }
    if (!symbol_name_ok(name)) return bad("E_SYMBOL_HINT", at(line_no) + "the name is empty or longer than 64 characters");
    if (std::any_of(out.symbols.begin(), out.symbols.end(), [&](const Symbol &s) { return s.name == name; })) return bad("E_SYMBOL_DUPLICATE", at(line_no) + "the name is repeated after folding");
    if (out.symbols.size() >= kMaxSymbols) return bad("E_SYMBOL_HINT", "too many hints (at most 255)");
    out.symbols.push_back({name, std::string(revision), "imported-hint", static_cast<std::uint32_t>(v)});
  }
  out.ok = true;
  return out;
}

}  // namespace segarecomp::mod
