#include "segarecomp/patch.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>

namespace segarecomp::patch {

namespace {

// ---- bounded raw-DEFLATE decoder (RFC 1951); output is limited to an exact declared size ----
class Inflater {
 public:
  Inflater(std::span<const std::uint8_t> in, std::size_t expected) : in_(in), expected_(expected) { out_.reserve(expected); }

  bool run(std::vector<std::uint8_t> &out) {
    for (;;) {
      const int final_block = bits(1);
      const int type = bits(2);
      if (failed_) return false;
      if (type == 0) {
        bit_count_ = 0; bit_buf_ = 0;
        if (pos_ + 4 > in_.size()) return false;
        const std::size_t len = in_[pos_] | (static_cast<std::size_t>(in_[pos_ + 1]) << 8U);
        const std::size_t nlen = in_[pos_ + 2] | (static_cast<std::size_t>(in_[pos_ + 3]) << 8U);
        pos_ += 4;
        if ((len ^ 0xFFFFU) != nlen || len > in_.size() - pos_ || len > expected_ - out_.size()) return false;
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_), in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
      } else if (type == 1) {
        Huff lit, dist;
        std::uint8_t l[288] = {};
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        std::uint8_t d[30];
        for (std::uint8_t &x : d) x = 5;
        if (!lit.build(l, 288) || !dist.build(d, 30) || !block(lit, dist)) return false;
      } else if (type == 2) {
        const int hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
        if (failed_ || hlit > 286 || hdist > 30) return false;
        static constexpr int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        std::uint8_t cl[19] = {};
        for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<std::uint8_t>(bits(3));
        if (failed_) return false;
        Huff clh;
        if (!clh.build(cl, 19)) return false;
        std::uint8_t lens[316] = {};
        int n = 0;
        while (n < hlit + hdist) {
          const int sym = decode(clh);
          if (sym < 0) return false;
          if (sym < 16) { lens[n++] = static_cast<std::uint8_t>(sym); continue; }
          int rep = 0; std::uint8_t val = 0;
          if (sym == 16) { if (n == 0) return false; val = lens[n - 1]; rep = 3 + bits(2); }
          else if (sym == 17) rep = 3 + bits(3);
          else rep = 11 + bits(7);
          if (failed_ || n + rep > hlit + hdist) return false;
          while (rep-- > 0) lens[n++] = val;
        }
        Huff lit, dist;
        if (!lit.build(lens, hlit) || !dist.build(lens + hlit, hdist) || !block(lit, dist)) return false;
      } else {
        return false;
      }
      if (failed_) return false;
      if (final_block != 0) break;
    }
    if (out_.size() != expected_) return false;
    out = std::move(out_);
    return true;
  }

 private:
  struct Huff {
    std::uint16_t count[16] = {};
    std::uint16_t symbol[320] = {};
    bool build(const std::uint8_t *lengths, int n) {
      for (std::uint16_t &c : count) c = 0;
      for (int i = 0; i < n; ++i) ++count[lengths[i]];
      int left = 1;
      for (int len = 1; len <= 15; ++len) { left <<= 1; left -= count[len]; if (left < 0) return false; }
      std::uint16_t offs[16] = {};
      for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count[len]);
      for (int i = 0; i < n; ++i) if (lengths[i] != 0) symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
      return true;
    }
  };

  int bits(int need) {
    while (bit_count_ < need) {
      if (pos_ >= in_.size()) { failed_ = true; return 0; }
      bit_buf_ |= static_cast<std::uint32_t>(in_[pos_++]) << bit_count_;
      bit_count_ += 8;
    }
    const int v = static_cast<int>(bit_buf_ & ((1U << need) - 1U));
    bit_buf_ >>= need;
    bit_count_ -= need;
    return v;
  }

  int decode(const Huff &h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      code |= bits(1);
      if (failed_) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
  }

  bool block(const Huff &lit, const Huff &dist) {
    static constexpr int lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      const int sym = decode(lit);
      if (sym < 0) return false;
      if (sym < 256) {
        if (out_.size() >= expected_) return false;
        out_.push_back(static_cast<std::uint8_t>(sym));
      } else if (sym == 256) {
        return true;
      } else {
        const int li = sym - 257;
        if (li >= 29) return false;
        const int len = lbase[li] + bits(lext[li]);
        const int ds = decode(dist);
        if (ds < 0 || ds >= 30) return false;
        const std::size_t d = static_cast<std::size_t>(dbase[ds] + bits(dext[ds]));
        if (failed_ || d > out_.size() || static_cast<std::size_t>(len) > expected_ - out_.size()) return false;
        for (int i = 0; i < len; ++i) out_.push_back(out_[out_.size() - d]);
      }
    }
  }

  std::span<const std::uint8_t> in_;
  std::size_t expected_;
  std::size_t pos_ = 0;
  std::uint32_t bit_buf_ = 0;
  int bit_count_ = 0;
  bool failed_ = false;
  std::vector<std::uint8_t> out_;
};

// ---- ZIP (classic, central-directory driven) ----
std::uint32_t u16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8U; }
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) { return u16(b, at) | u16(b, at + 2) << 16U; }

struct ZipEntry {
  std::string name;
  std::uint32_t method = 0, crc = 0, csize = 0, usize = 0, local = 0;
  std::vector<std::uint8_t> data;
};

bool safe_name(const std::string &n) {
  if (n.empty() || n.size() > 64) return false;
  if (n == "." || n == ".." || n.front() == '.') return false;
  for (const char c : n) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;  // rejects separators, drive letters, NUL, control characters and non-ASCII
  }
  return true;
}

bool read_zip(std::span<const std::uint8_t> z, std::map<std::string, ZipEntry> &entries, std::string &error) {
  if (z.size() < 22 || z.size() > kMaxPackageBytes) { error = "the package is not a valid archive"; return false; }
  std::size_t eocd = std::string::npos;
  const std::size_t lowest = z.size() > 22 + 65535 ? z.size() - 22 - 65535 : 0;
  for (std::size_t p = z.size() - 22 + 1; p-- > lowest;) {
    if (u32(z, p) == 0x06054B50U) { eocd = p; break; }
  }
  if (eocd == std::string::npos) { error = "the package is not a ZIP archive"; return false; }
  const std::size_t total = u16(z, eocd + 10), cd_size = u32(z, eocd + 12), cd_off = u32(z, eocd + 16);
  if (u16(z, eocd + 4) != 0 || u16(z, eocd + 6) != 0 || u16(z, eocd + 8) != total) { error = "multi-disk archives are not supported"; return false; }
  if (u32(z, eocd + 12) == 0xFFFFFFFFU || u32(z, eocd + 16) == 0xFFFFFFFFU || total == 0xFFFF) { error = "ZIP64 archives are not supported"; return false; }
  if (u16(z, eocd + 20) != 0 || eocd + 22 != z.size()) { error = "the archive has a trailing comment or extra data"; return false; }
  if (total == 0 || total > kMaxArchiveEntries) { error = "the archive has an unexpected number of entries"; return false; }
  if (cd_off > eocd || cd_size > eocd - cd_off || cd_off + cd_size != eocd) { error = "the archive directory is inconsistent"; return false; }
  std::size_t at = cd_off;
  std::vector<std::pair<std::size_t, std::size_t>> ranges;  // [local header, end of data) for overlap detection
  for (std::size_t i = 0; i < total; ++i) {
    if (at + 46 > eocd || u32(z, at) != 0x02014B50U) { error = "the archive directory is damaged"; return false; }
    ZipEntry e;
    const std::uint32_t made_by = u16(z, at + 4), flags = u16(z, at + 8);
    e.method = u16(z, at + 10); e.crc = u32(z, at + 16); e.csize = u32(z, at + 20); e.usize = u32(z, at + 24);
    const std::size_t nlen = u16(z, at + 28), xlen = u16(z, at + 30), clen = u16(z, at + 32);
    const std::uint32_t ext_attr = u32(z, at + 38);
    e.local = u32(z, at + 42);
    if (at + 46 + nlen + xlen + clen > eocd) { error = "the archive directory is damaged"; return false; }
    e.name.assign(reinterpret_cast<const char *>(z.data() + at + 46), nlen);
    at += 46 + nlen + xlen + clen;
    if ((flags & 0x1U) != 0U || (flags & 0x40U) != 0U) { error = "encrypted entries are not supported"; return false; }
    if ((made_by >> 8U) == 3U) {  // Unix host: the high 16 bits of the external attributes hold st_mode
      const std::uint32_t mode = ext_attr >> 16U;
      if ((mode & 0xF000U) != 0U && (mode & 0xF000U) != 0x8000U) { error = "the archive contains a link, device or directory entry"; return false; }
    }
    if ((ext_attr & 0x10U) != 0U) { error = "the archive contains a directory entry"; return false; }
    if (!safe_name(e.name)) { error = "the archive contains an unsafe entry name"; return false; }
    if (e.method != 0 && e.method != 8) { error = "unsupported compression method"; return false; }
    if (e.csize == 0xFFFFFFFFU || e.usize == 0xFFFFFFFFU) { error = "ZIP64 entries are not supported"; return false; }
    if (entries.count(e.name) != 0) { error = "the archive contains duplicate entries"; return false; }
    entries.emplace(e.name, std::move(e));
  }
  if (at != eocd) { error = "the archive directory has unexpected trailing bytes"; return false; }
  std::size_t total_uncompressed = 0;
  for (auto &[name, e] : entries) {
    const std::size_t limit = name == "manifest.json" ? kMaxManifestBytes : kMaxPatchBytes;
    if (e.usize > limit) { error = "an archive entry is larger than allowed"; return false; }
    total_uncompressed += e.usize;
    if (total_uncompressed > kMaxManifestBytes + kMaxPatchBytes) { error = "the archive expands to more than allowed"; return false; }
    if (static_cast<std::size_t>(e.local) + 30 > cd_off || u32(z, e.local) != 0x04034B50U) { error = "an archive entry header is damaged"; return false; }
    const std::uint32_t lflags = u16(z, e.local + 6);
    const std::size_t lnlen = u16(z, e.local + 26), lxlen = u16(z, e.local + 28);
    if ((lflags & 0x1U) != 0U || u16(z, e.local + 8) != e.method) { error = "an archive entry header is inconsistent"; return false; }
    const std::size_t start = static_cast<std::size_t>(e.local) + 30 + lnlen + lxlen;
    if (start > cd_off || e.csize > cd_off - start) { error = "an archive entry runs past the archive"; return false; }
    if (lnlen != name.size() || std::memcmp(z.data() + e.local + 30, name.data(), lnlen) != 0) { error = "an archive entry header names a different file"; return false; }
    ranges.emplace_back(e.local, start + e.csize);
    const auto data = z.subspan(start, e.csize);
    if (e.method == 0) {
      if (e.csize != e.usize) { error = "a stored archive entry has inconsistent sizes"; return false; }
      e.data.assign(data.begin(), data.end());
    } else if (!Inflater(data, e.usize).run(e.data)) {
      error = "an archive entry cannot be decompressed within its declared size";
      return false;
    }
    if (crc32(e.data) != e.crc) { error = "an archive entry fails its CRC-32 check"; return false; }
  }
  std::sort(ranges.begin(), ranges.end());
  for (std::size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second) { error = "archive entries overlap"; return false; }
  return true;
}

// ---- strict manifest parser (a closed JSON subset: objects, ASCII strings, one integer; duplicate or unknown keys are errors) ----
struct Value {
  enum class Kind { string, number, object, array } kind = Kind::string;
  std::vector<Value> items;
  std::string text;
  std::uint64_t number = 0;
  std::map<std::string, Value> members;
};

class JsonReader {
 public:
  explicit JsonReader(std::string_view t) : t_(t) {}
  bool parse(Value &out) { skip(); if (!value(out, 0)) return false; skip(); return pos_ == t_.size(); }
  std::string error = "the manifest is not valid JSON";

 private:
  void skip() { while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\n' || t_[pos_] == '\r' || t_[pos_] == '\t')) ++pos_; }
  bool string(std::string &out) {
    if (pos_ >= t_.size() || t_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < t_.size()) {
      const unsigned char c = static_cast<unsigned char>(t_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20 || c >= 0x7F) { error = "the manifest contains a control or non-ASCII character"; return false; }
      if (c == '\\') {
        if (pos_ >= t_.size() || (t_[pos_] != '"' && t_[pos_] != '\\' && t_[pos_] != '/')) { error = "the manifest uses an unsupported string escape"; return false; }
        out.push_back(t_[pos_++]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      if (out.size() > 256) { error = "a manifest string is too long"; return false; }
    }
    return false;
  }
  bool value(Value &out, int depth) {
    if (depth > 3 || pos_ >= t_.size()) return false;
    if (t_[pos_] == '"') { out.kind = Value::Kind::string; return string(out.text); }
    if (t_[pos_] >= '0' && t_[pos_] <= '9') {
      out.kind = Value::Kind::number;
      if (t_[pos_] == '0' && pos_ + 1 < t_.size() && t_[pos_ + 1] >= '0' && t_[pos_ + 1] <= '9') return false;
      int digits = 0;
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') { out.number = out.number * 10 + static_cast<std::uint64_t>(t_[pos_++] - '0'); if (++digits > 9) return false; }
      return true;
    }
    if (t_[pos_] == '[') {
      out.kind = Value::Kind::array;
      ++pos_; skip();
      if (pos_ < t_.size() && t_[pos_] == ']') { ++pos_; return true; }
      for (;;) {
        skip();
        Value v;
        if (out.items.size() >= 16 || !value(v, depth + 1)) return false;
        out.items.push_back(std::move(v));
        skip();
        if (pos_ >= t_.size()) return false;
        if (t_[pos_] == ',') { ++pos_; continue; }
        if (t_[pos_] == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (t_[pos_] != '{') return false;
    out.kind = Value::Kind::object;
    ++pos_; skip();
    if (pos_ < t_.size() && t_[pos_] == '}') { ++pos_; return true; }
    for (;;) {
      skip();
      std::string key;
      if (!string(key)) return false;
      skip();
      if (pos_ >= t_.size() || t_[pos_++] != ':') return false;
      skip();
      Value v;
      if (!value(v, depth + 1)) return false;
      if (!out.members.emplace(std::move(key), std::move(v)).second) { error = "the manifest repeats a key"; return false; }
      skip();
      if (pos_ >= t_.size()) return false;
      if (t_[pos_] == ',') { ++pos_; continue; }
      if (t_[pos_] == '}') { ++pos_; return true; }
      return false;
    }
  }
  std::string_view t_;
  std::size_t pos_ = 0;
};

bool is_hex64(const std::string &s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool token(const std::string &s, std::size_t max, bool allow_upper_and_dot) {
  if (s.empty() || s.size() > max) return false;
  return std::all_of(s.begin(), s.end(), [&](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || (allow_upper_and_dot && ((c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '_'));
  });
}

bool take_string(const Value &obj, const char *key, std::string &out) {
  const auto it = obj.members.find(key);
  if (it == obj.members.end() || it->second.kind != Value::Kind::string) return false;
  out = it->second.text;
  return true;
}

bool only_keys(const Value &obj, std::initializer_list<const char *> keys) {
  if (obj.members.size() != keys.size()) return false;
  for (const char *k : keys) if (obj.members.count(k) == 0) return false;
  return true;
}

Package bad(std::string why) {
  Package p;
  p.error = std::move(why);
  return p;
}

}  // namespace

Package read_package(std::span<const std::uint8_t> zip) {
  std::map<std::string, ZipEntry> entries;
  std::string error;
  if (!read_zip(zip, entries, error)) return bad(error);
  const auto man = entries.find("manifest.json");
  if (man == entries.end()) return bad("the package has no manifest.json");
  Value root;
  JsonReader reader(std::string_view(reinterpret_cast<const char *>(man->second.data.data()), man->second.data.size()));
  if (!reader.parse(root) || root.kind != Value::Kind::object) return bad(reader.error);
  static const char *const required[] = {"format", "schema_version", "id", "version", "name", "platform", "purpose", "source_sha256", "payloads"};
  static const char *const optional[] = {"requires", "widescreen_width_max"};
  for (const char *k : required) if (root.members.count(k) == 0) return bad(std::string("the manifest is missing \"") + k + "\"");
  for (const auto &[k, v] : root.members) {
    (void)v;
    bool known = false;
    for (const char *r : required) known = known || k == r;
    for (const char *o : optional) known = known || k == o;
    if (!known) return bad("the manifest has an unknown field \"" + k + "\"");
  }
  PackageInfo info;
  std::string format;
  if (!take_string(root, "format", format) || format != "segarecomp-patch") return bad("the manifest is not a segarecomp patch manifest");
  const Value &ver = root.members.at("schema_version");
  if (ver.kind != Value::Kind::number) return bad("the manifest schema_version is invalid");
  if (ver.number != 1) return bad("unsupported patch package schema version " + std::to_string(ver.number));
  if (!take_string(root, "id", info.id) || !token(info.id, 64, false)) return bad("the manifest id is invalid");
  if (!take_string(root, "version", info.version) || !token(info.version, 32, true)) return bad("the manifest version is invalid");
  if (!take_string(root, "name", info.name) || info.name.empty() || info.name.size() > 64) return bad("the manifest name is invalid");
  if (!take_string(root, "platform", info.platform) || info.platform != "genesis") return bad("the manifest platform is not supported");
  if (!take_string(root, "purpose", info.purpose) || info.purpose != "widescreen") return bad("the manifest purpose is not supported");
  if (!take_string(root, "source_sha256", info.source_sha256) || !is_hex64(info.source_sha256)) return bad("the manifest source_sha256 is invalid");
  if (const auto w = root.members.find("widescreen_width_max"); w != root.members.end()) {
    if (w->second.kind != Value::Kind::number || w->second.number > 100) return bad("the manifest widescreen_width_max must be an integer from 0 to 100");
    info.widescreen_width_max = static_cast<int>(w->second.number);
  }
  if (const auto r = root.members.find("requires"); r != root.members.end()) {
    if (r->second.kind != Value::Kind::array || r->second.items.size() > 8) return bad("the manifest requires list is invalid");
    for (const Value &item : r->second.items) {
      if (item.kind != Value::Kind::string || !token(item.text, 48, false)) return bad("the manifest requires list is invalid");
      bool supported = false;
      for (const char *const *c = kSupportedCapabilities; *c != nullptr; ++c) supported = supported || item.text == *c;
      if (!supported) return bad("this segarecomp does not provide the required capability \"" + item.text + "\"");
      info.requires_capabilities.push_back(item.text);
    }
  }
  const Value &payloads = root.members.at("payloads");
  if (payloads.kind != Value::Kind::object || payloads.members.empty()) return bad("the manifest payloads are invalid");
  for (const auto &[name, v] : payloads.members) {
    (void)v;
    if (name != "rom") return bad("the manifest names an unsupported payload type \"" + name + "\"");
  }
  const Value &rom = payloads.members.at("rom");
  if (rom.kind != Value::Kind::object || !only_keys(rom, {"format", "file", "sha256", "target_sha256"})) return bad("the rom payload is invalid");
  std::string pformat;
  if (!take_string(rom, "format", pformat) || pformat != "bps") return bad("the rom payload format is not supported");
  if (!take_string(rom, "file", info.rom_file) || !safe_name(info.rom_file) || info.rom_file == "manifest.json") return bad("the rom payload file name is invalid");
  if (!take_string(rom, "sha256", info.rom_payload_sha256) || !is_hex64(info.rom_payload_sha256)) return bad("the rom payload sha256 is invalid");
  if (!take_string(rom, "target_sha256", info.target_sha256) || !is_hex64(info.target_sha256)) return bad("the rom payload target_sha256 is invalid");
  if (info.source_sha256 == info.target_sha256) return bad("the manifest source and target are identical");
  info.has_rom = true;
  if (entries.size() != 2) return bad("the package must contain exactly manifest.json and the declared payload");
  const auto pit = entries.find(info.rom_file);
  if (pit == entries.end()) return bad("the payload named by the manifest is missing from the package");
  if (sha256_hex(pit->second.data) != info.rom_payload_sha256) return bad("the payload SHA-256 does not match the manifest");
  info.identity = sha256_hex(man->second.data);
  Package out;
  out.ok = true;
  out.info = std::move(info);
  out.payload = std::move(pit->second.data);
  return out;
}

Applied apply_package(const Package &package, std::span<const std::uint8_t> rom) {
  Applied a;
  if (!package.ok) { a.error = "the package was not verified"; return a; }
  if (rom.size() > kMaxImageBytes) { a.error = "the original image is too large"; return a; }
  if (sha256_hex(rom) != package.info.source_sha256) { a.error = "the original image is not the exact image this patch was made for"; return a; }
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
  if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::is_symlink(path, ec)) { error = "the file is not a regular file"; return false; }
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
  r.target_sha256 = pkg.info.target_sha256;
  if (!out.empty()) {
    std::error_code ec;
    const std::filesystem::path tmp = out.string() + ".partial";
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
