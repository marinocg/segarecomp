#include "segarecomp/codegen/c11/source_relative.hpp"

#include <cctype>
#include <cstdio>
#include <vector>

namespace segarecomp {

const char *source_relative_status_name(SourceRelativeStatus status) noexcept {
  switch (status) {
  case SourceRelativeStatus::ok: return "ok";
  case SourceRelativeStatus::token_structure: return "token_structure";
  case SourceRelativeStatus::literal_not_linear: return "literal_not_linear";
  case SourceRelativeStatus::literal_suffix: return "literal_suffix";
  case SourceRelativeStatus::constant_context: return "constant_context";
  }
  return "unknown";
}

namespace {

struct Token {
  std::size_t begin{};
  std::size_t end{};
  bool word{};
};

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::vector<Token> tokenize(const std::string &text) {
  std::vector<Token> out;
  std::size_t at = 0;
  while (at < text.size()) {
    if (is_word_char(text[at])) {
      std::size_t end = at;
      while (end < text.size() && is_word_char(text[end])) ++end;
      out.push_back({at, end, true});
      at = end;
    } else {
      out.push_back({at, at + 1U, false});
      ++at;
    }
  }
  return out;
}

struct Literal {
  bool ok{};
  std::uint64_t value{};
  std::string suffix;
};

// A numeric literal word: decimal or 0x-hexadecimal digits followed by an integer suffix (u, l).
Literal parse_literal(std::string_view word) {
  Literal out;
  if (word.empty() || std::isdigit(static_cast<unsigned char>(word[0])) == 0) return out;
  std::size_t at = 0;
  int base = 10;
  if (word.size() > 2U && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
    base = 16;
    at = 2;
  }
  const std::size_t digits_begin = at;
  std::uint64_t value = 0;
  while (at < word.size()) {
    const char c = word[at];
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
    else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
    if (digit < 0 || digit >= base) break;
    value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    if (value > 0xFFFFFFFFULL) return out;  // wider than a guest address: not a PC-derived literal
    ++at;
  }
  if (at == digits_begin) return out;
  for (std::size_t i = at; i < word.size(); ++i)
    if (word[i] != 'u' && word[i] != 'U' && word[i] != 'l' && word[i] != 'L') return out;
  out.ok = true;
  out.value = value;
  out.suffix = std::string(word.substr(at));
  return out;
}

std::string hex8(std::uint32_t v) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%08X", static_cast<unsigned>(v));
  return buffer;
}

bool ends_with(const std::string &text, std::size_t at, std::string_view tail) {
  return at >= tail.size() && text.compare(at - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

SourceRelativeText derive_source_relative_text(const std::array<std::string, 3> &texts, const std::array<std::uint32_t, 3> &pcs,
                                               std::string_view pc_symbol) {
  SourceRelativeText result;
  const auto fail = [&](SourceRelativeStatus status) {
    result.status = status;
    result.text.clear();
    return result;
  };
  const std::array<std::vector<Token>, 3> tokens{tokenize(texts[0]), tokenize(texts[1]), tokenize(texts[2])};
  if (tokens[0].size() != tokens[1].size() || tokens[0].size() != tokens[2].size()) return fail(SourceRelativeStatus::token_structure);
  const std::uint32_t d1 = pcs[1] - pcs[0], d2 = pcs[2] - pcs[0];
  const std::string &base = texts[0];
  std::string out;
  std::size_t copied = 0;  // base[0, copied) is already in `out`
  for (std::size_t i = 0; i < tokens[0].size(); ++i) {
    const Token &a = tokens[0][i], &b = tokens[1][i], &c = tokens[2][i];
    if (a.word != b.word || a.word != c.word) return fail(SourceRelativeStatus::token_structure);
    const std::string_view wa(texts[0].data() + a.begin, a.end - a.begin), wb(texts[1].data() + b.begin, b.end - b.begin),
        wc(texts[2].data() + c.begin, c.end - c.begin);
    if (!a.word) {
      if (wa != wb || wa != wc) return fail(SourceRelativeStatus::token_structure);
      continue;
    }
    if (wa == wb && wa == wc) continue;
    const Literal la = parse_literal(wa), lb = parse_literal(wb), lc = parse_literal(wc);
    if (!la.ok || !lb.ok || !lc.ok) return fail(SourceRelativeStatus::token_structure);  // an identifier (or non-literal) that embeds the PC
    if (la.suffix != lb.suffix || la.suffix != lc.suffix) return fail(SourceRelativeStatus::literal_suffix);
    const auto v1 = static_cast<std::uint32_t>(la.value), v2 = static_cast<std::uint32_t>(lb.value), v3 = static_cast<std::uint32_t>(lc.value);
    if (static_cast<std::uint32_t>(v2 - v1) != d1 || static_cast<std::uint32_t>(v3 - v1) != d2) return fail(SourceRelativeStatus::literal_not_linear);
    // `UINT32_C(<literal>)` wraps the literal in the lowerings: replace the whole macro call, not just the digits.
    std::size_t begin = a.begin, end = a.end;
    constexpr std::string_view macro = "UINT32_C(";
    if (ends_with(base, a.begin, macro) && end < base.size() && base[end] == ')') {
      begin -= macro.size();
      end += 1U;
    }
    // Where only a constant expression is allowed (a case label), a runtime expression cannot stand in.
    if (ends_with(base, begin, "case ") || ends_with(base, begin, "case\t")) return fail(SourceRelativeStatus::constant_context);
    if (begin < copied) return fail(SourceRelativeStatus::token_structure);
    out.append(base, copied, begin - copied);
    out += "(uint32_t)(" + std::string(pc_symbol) + " + UINT32_C(0x" + hex8(v1 - pcs[0]) + "))";
    copied = end;
    ++result.relative_literals;
  }
  out.append(base, copied, std::string::npos);
  result.text = std::move(out);
  return result;
}

}  // namespace segarecomp
