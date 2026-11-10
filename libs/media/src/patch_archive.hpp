#pragma once

// Internal to libs/media: the bounded ZIP reader, raw-DEFLATE decoder and closed-JSON reader shared by the schema 1/2 reader (patch_package.cpp) and the schema 3 reader
// (mod_package.cpp). Every limit is a parameter of the caller's policy; nothing here knows about a payload kind.

#include "segarecomp/patch.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::patch::detail {

struct ZipEntry {
  std::string name;
  std::uint32_t method = 0, crc = 0, csize = 0, usize = 0, local = 0;
  std::vector<std::uint8_t> data;
};

struct ZipPolicy {
  std::size_t max_entries = kMaxArchiveEntries;
  std::size_t manifest_limit = kMaxManifestBytes;   // per-entry uncompressed limit of manifest.json
  std::size_t entry_limit = kMaxPatchBytes;         // per-entry uncompressed limit of every other entry
  std::size_t total_limit = kMaxManifestBytes + kMaxPatchBytes;
  std::uint64_t max_ratio = 0;                      // 0 = unchecked; else a deflated entry may expand to at most csize * max_ratio + 4096 bytes
  bool fold_case_duplicates = false;                // refuse names that differ only by ASCII case
  bool (*name_ok)(const std::string &) = nullptr;
};

[[nodiscard]] bool safe_name(const std::string &name);   // the schema 1/2 flat entry-name rule
// Reads a classic ZIP under `policy`. On failure `error` is readable and `*code` (optional) is the schema 3 archive class (E_ARCHIVE_ENTRY | E_ARCHIVE_SIZE | E_ARCHIVE_PATH).
[[nodiscard]] bool read_zip(std::span<const std::uint8_t> zip, const ZipPolicy &policy, std::map<std::string, ZipEntry> &entries, std::string &error, const char **code = nullptr);

struct JsonLimits {
  std::size_t max_items = 16, max_members = 16, max_string = 256;
  int max_depth = 4;
  std::uint64_t max_number = 999999999U;
  bool allow_slash_escape = true;
  bool allow_whitespace = true;
};

struct Value {
  enum class Kind { string, number, object, array } kind = Kind::string;
  std::vector<Value> items;
  std::string text;
  std::uint64_t number = 0;
  std::vector<std::string> keys;   // insertion order, parallel to `members`
  std::vector<Value> members;
  [[nodiscard]] const Value *find(const std::string &key) const {
    for (std::size_t i = 0; i < keys.size(); ++i) if (keys[i] == key) return &members[i];
    return nullptr;
  }
  [[nodiscard]] const Value &at(const std::string &key) const { return *find(key); }   // callers check presence first
};

// Parses `text` as the closed JSON subset (objects, arrays, ASCII strings, non-negative integers; duplicate keys are errors) under `limits`.
[[nodiscard]] bool parse_json(std::string_view text, const JsonLimits &limits, Value &out, std::string &error);

[[nodiscard]] bool printable(const std::string &s);
[[nodiscard]] bool is_hex64(const std::string &s);
[[nodiscard]] bool token(const std::string &s, std::size_t max, bool allow_upper_and_dot);
[[nodiscard]] bool take_string(const Value &obj, const char *key, std::string &out);
[[nodiscard]] bool only_keys(const Value &obj, std::initializer_list<const char *> keys);

}  // namespace segarecomp::patch::detail
