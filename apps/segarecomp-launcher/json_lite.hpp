#pragma once

// A small strict JSON reader/writer for the launcher's own persistent files (library catalog, launcher settings).
// Not a general-purpose library: bounded depth and size, UTF-8 strings only, numbers kept as 64-bit integers or doubles.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher::json {

struct Value {
  enum class Kind { null, boolean, integer, real, string, array, object };
  Kind kind = Kind::null;
  bool boolean = false;
  long long integer = 0;
  double real = 0.0;
  std::string string;
  std::vector<Value> items;         // array elements, or object member values
  std::vector<std::string> keys;    // object member names (parallel to `items`, insertion order)

  static Value make_object() { Value v; v.kind = Kind::object; return v; }
  static Value make_array() { Value v; v.kind = Kind::array; return v; }
  static Value make_string(std::string text) { Value v; v.kind = Kind::string; v.string = std::move(text); return v; }
  static Value make_int(long long n) { Value v; v.kind = Kind::integer; v.integer = n; return v; }
  static Value make_bool(bool b) { Value v; v.kind = Kind::boolean; v.boolean = b; return v; }

  [[nodiscard]] bool is_object() const { return kind == Kind::object; }
  [[nodiscard]] bool is_array() const { return kind == Kind::array; }
  [[nodiscard]] const Value *get(std::string_view key) const;
  [[nodiscard]] std::string str(std::string_view key, const std::string &fallback = {}) const;
  [[nodiscard]] long long num(std::string_view key, long long fallback = 0) const;
  [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const;
  Value &set(std::string key, Value value);  // object only: replaces an existing member
  Value &push(Value value);                  // array only
};

// Parses a complete document. Returns nullopt (and a message in `error`) for any malformed or oversized input:
// trailing garbage, depth over 24, more than 1 MiB of text, invalid escapes or control characters in strings.
[[nodiscard]] std::optional<Value> parse(std::string_view text, std::string *error = nullptr);
// Serializes with two-space indentation and a trailing newline. Output is deterministic (member order is insertion order).
[[nodiscard]] std::string dump(const Value &value);

} // namespace launcher::json
