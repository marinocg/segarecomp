#include "json_lite.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <charconv>
#include <limits>

namespace launcher::json {

const Value *Value::get(std::string_view key) const {
  if (kind != Kind::object) return nullptr;
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) return &items[i];
  return nullptr;
}

std::string Value::str(std::string_view key, const std::string &fallback) const {
  const Value *v = get(key);
  return v != nullptr && v->kind == Kind::string ? v->string : fallback;
}

long long Value::num(std::string_view key, long long fallback) const {
  const Value *v = get(key);
  if (v == nullptr) return fallback;
  if (v->kind == Kind::integer) return v->integer;
  // The upper bound is exclusive: LLONG_MAX rounds UP to 2^63 as a double.
  if (v->kind == Kind::real && std::isfinite(v->real) && std::trunc(v->real) == v->real &&
      v->real >= static_cast<double>(std::numeric_limits<long long>::min()) &&
      v->real < -static_cast<double>(std::numeric_limits<long long>::min())) return static_cast<long long>(v->real);
  return fallback;
}

bool Value::flag(std::string_view key, bool fallback) const {
  const Value *v = get(key);
  return v != nullptr && v->kind == Kind::boolean ? v->boolean : fallback;
}

Value &Value::set(std::string key, Value value) {
  kind = Kind::object;
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) { items[i] = std::move(value); return items[i]; }
  keys.push_back(std::move(key));
  items.push_back(std::move(value));
  return items.back();
}

Value &Value::push(Value value) {
  kind = Kind::array;
  items.push_back(std::move(value));
  return items.back();
}

namespace {

constexpr std::size_t max_text = 1U << 20U;
constexpr int max_depth = 24;

struct Parser {
  std::string_view text;
  std::size_t at = 0;
  std::string error;

  bool fail(const char *message) {
    if (error.empty()) error = std::string(message) + " at byte " + std::to_string(at);
    return false;
  }
  void skip() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
  }
  bool literal(const char *word) {
    const std::size_t n = std::strlen(word);
    if (text.substr(at, n) != word) return false;
    at += n;
    return true;
  }
  static void append_utf8(std::string &out, unsigned code) {
    if (code < 0x80U) out.push_back(static_cast<char>(code));
    else if (code < 0x800U) { out.push_back(static_cast<char>(0xC0U | (code >> 6U))); out.push_back(static_cast<char>(0x80U | (code & 0x3FU))); }
    else if (code < 0x10000U) {
      out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
  }
  bool hex4(unsigned &code) {
    if (at + 4 > text.size()) return fail("truncated \\u escape");
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text[at++];
      code <<= 4U;
      if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }
  bool string_body(std::string &out) {
    // `at` is just past the opening quote.
    while (at < text.size()) {
      const auto c = static_cast<unsigned char>(text[at++]);
      if (c == '"') return true;
      if (c < 0x20U) return fail("control character in string");
      if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
      if (at >= text.size()) break;
      const char e = text[at++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          if (!hex4(code)) return false;
          if (code >= 0xD800U && code < 0xDC00U) {  // surrogate pair
            unsigned low = 0;
            if (text.substr(at, 2) != "\\u") return fail("lone surrogate");
            at += 2;
            if (!hex4(low) || low < 0xDC00U || low > 0xDFFFU) return fail("bad surrogate pair");
            code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
          } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            return fail("lone surrogate");
          }
          append_utf8(out, code);
          break;
        }
        default: return fail("invalid escape");
      }
    }
    return fail("unterminated string");
  }
  bool number(Value &out) {
    const std::size_t begin = at;
    if (at < text.size() && text[at] == '-') ++at;
    if (at >= text.size() || text[at] < '0' || text[at] > '9') return fail("bad number");
    if (text[at] == '0' && at + 1 < text.size() && text[at + 1] >= '0' && text[at + 1] <= '9') return fail("leading zero");
    bool real = false;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    if (at < text.size() && text[at] == '.') {
      real = true;
      ++at;
      if (at >= text.size() || text[at] < '0' || text[at] > '9') return fail("bad fraction");
      while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
      real = true;
      ++at;
      if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
      if (at >= text.size() || text[at] < '0' || text[at] > '9') return fail("bad exponent");
      while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    }
    const std::string token(text.substr(begin, at - begin));
    if (!real) {
      const auto result = std::from_chars(token.data(), token.data() + token.size(), out.integer);
      if (result.ec != std::errc{}) return fail("integer out of range");
      out.kind = Value::Kind::integer;
    } else {
      out.kind = Value::Kind::real;
      out.real = std::strtod(token.c_str(), nullptr);
      if (!std::isfinite(out.real)) return fail("nonfinite number");
    }
    return true;
  }
  bool value(Value &out, int depth) {
    if (depth > max_depth) return fail("nesting too deep");
    skip();
    if (at >= text.size()) return fail("unexpected end");
    const char c = text[at];
    if (c == '{') {
      ++at;
      out = Value::make_object();
      skip();
      if (at < text.size() && text[at] == '}') { ++at; return true; }
      for (;;) {
        skip();
        if (at >= text.size() || text[at] != '"') return fail("expected member name");
        ++at;
        std::string key;
        if (!string_body(key)) return false;
        skip();
        if (at >= text.size() || text[at] != ':') return fail("expected ':'");
        ++at;
        Value member;
        if (!value(member, depth + 1)) return false;
        out.set(std::move(key), std::move(member));
        skip();
        if (at < text.size() && text[at] == ',') { ++at; continue; }
        if (at < text.size() && text[at] == '}') { ++at; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++at;
      out = Value::make_array();
      skip();
      if (at < text.size() && text[at] == ']') { ++at; return true; }
      for (;;) {
        Value item;
        if (!value(item, depth + 1)) return false;
        out.items.push_back(std::move(item));
        skip();
        if (at < text.size() && text[at] == ',') { ++at; continue; }
        if (at < text.size() && text[at] == ']') { ++at; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { ++at; out = Value{}; out.kind = Value::Kind::string; return string_body(out.string); }
    if (literal("true")) { out = Value::make_bool(true); return true; }
    if (literal("false")) { out = Value::make_bool(false); return true; }
    if (literal("null")) { out = Value{}; return true; }
    return number(out);
  }
};

void escape_into(std::string &out, const std::string &text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20U) { char buffer[8]; std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c)); out += buffer; }
        else out.push_back(raw);
    }
  }
  out.push_back('"');
}

void dump_into(std::string &out, const Value &v, int indent) {
  const std::string pad(static_cast<std::size_t>(indent) * 2U, ' '), pad_in(static_cast<std::size_t>(indent + 1) * 2U, ' ');
  switch (v.kind) {
    case Value::Kind::null: out += "null"; break;
    case Value::Kind::boolean: out += v.boolean ? "true" : "false"; break;
    case Value::Kind::integer: out += std::to_string(v.integer); break;
    case Value::Kind::real: { char buffer[40]; std::snprintf(buffer, sizeof buffer, "%.9g", v.real); out += buffer; break; }
    case Value::Kind::string: escape_into(out, v.string); break;
    case Value::Kind::array:
      if (v.items.empty()) { out += "[]"; break; }
      out += "[\n";
      for (std::size_t i = 0; i < v.items.size(); ++i) {
        out += pad_in;
        dump_into(out, v.items[i], indent + 1);
        out += i + 1 < v.items.size() ? ",\n" : "\n";
      }
      out += pad + "]";
      break;
    case Value::Kind::object:
      if (v.items.empty()) { out += "{}"; break; }
      out += "{\n";
      for (std::size_t i = 0; i < v.items.size(); ++i) {
        out += pad_in;
        escape_into(out, v.keys[i]);
        out += ": ";
        dump_into(out, v.items[i], indent + 1);
        out += i + 1 < v.items.size() ? ",\n" : "\n";
      }
      out += pad + "}";
      break;
  }
}

} // namespace

std::optional<Value> parse(std::string_view text, std::string *error) {
  if (text.size() > max_text) { if (error) *error = "document too large"; return std::nullopt; }
  Parser parser{text, 0, {}};
  // A UTF-8 byte order mark is tolerated.
  if (text.substr(0, 3) == "\xEF\xBB\xBF") parser.at = 3;
  Value root;
  if (!parser.value(root, 0)) { if (error) *error = parser.error; return std::nullopt; }
  parser.skip();
  if (parser.at != text.size()) { if (error) *error = "trailing characters"; return std::nullopt; }
  return root;
}

std::string dump(const Value &value) {
  std::string out;
  dump_into(out, value, 0);
  out.push_back('\n');
  return out;
}

} // namespace launcher::json
