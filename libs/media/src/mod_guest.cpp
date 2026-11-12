// Bounded guest-code assembly and injection (SEG-136-T003/T004, ADR 0292), collision and header checks (T005) and the verified target image (ADR 0293).
// Pure text/byte processing: no file, no clock, no host byte order, no include/macro facility, so every host produces identical bytes. The assembler supports the documented subset of the M68K
// (Genesis main CPU) and the Z80 (Master System, Genesis sound CPU); anything else is a stable rejection, never a guess. Diagnostics name the line and a class and never print image bytes.
#include "segarecomp/mod.hpp"

#include "segarecomp/patch.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

namespace segarecomp::mod {

namespace {

struct Fail {
  std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) { throw Fail{std::move(code), std::move(message)}; }
std::string at(std::size_t line) { return "line " + std::to_string(line) + ": "; }

enum class Cpu { m68k, z80 };

// ------------------------------------------------------------------ source model
struct Stmt {
  std::size_t line = 0;
  std::string label, op, size;
  std::vector<std::string> args;
};
struct Free {
  std::string rev;
  std::uint64_t start = 0, length = 0;
  std::uint8_t fill = 0;
  std::size_t line = 0;
};
struct Block {
  std::string rev;
  std::uint64_t offset = 0, address = 0;
  bool has_at = false;
  std::size_t line = 0;
  std::vector<Stmt> stmts;
};
struct Source {
  std::vector<Free> frees;
  std::vector<Block> blocks;
};

std::string hex(std::uint64_t v) {
  std::string out;
  do { out.insert(out.begin(), "0123456789abcdef"[v & 15U]); v >>= 4U; } while (v != 0);
  return out;
}

bool label_ok(std::string_view s) {
  if (s.empty() || s.size() > 64 || (s[0] >= '0' && s[0] <= '9')) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });
}

bool hex_lit(std::string_view t, std::uint64_t &out) {   // `0x` + 1..8 lower-case hex digits
  if (t.size() < 3 || t.size() > 10 || t.substr(0, 2) != "0x") return false;
  out = 0;
  for (const char c : t.substr(2)) {
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    out = out * 16 + static_cast<std::uint64_t>(d);
  }
  return true;
}

bool int_lit(std::string_view t, std::int64_t &out) {   // [-]decimal (<= 10 digits) or [-]0x hex
  bool neg = false;
  if (!t.empty() && t[0] == '-') { neg = true; t.remove_prefix(1); }
  std::uint64_t v = 0;
  if (t.size() > 2 && t.substr(0, 2) == "0x") {
    if (!hex_lit(t, v)) return false;
  } else {
    if (t.empty() || t.size() > 10 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    for (const char c : t) v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
  return true;
}

std::vector<std::string> split_ws(std::string_view line) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t s = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > s) out.emplace_back(line.substr(s, i - s));
  }
  return out;
}

std::vector<std::string> split_operands(std::string_view rest, std::size_t line) {   // commas outside parentheses; spaces inside an operand are removed
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (const char c : rest) {
    if (c == '(') ++depth;
    if (c == ')' && --depth < 0) fail("E_GUEST_SYNTAX", at(line) + "unbalanced parenthesis");
    if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
    if (c != ' ' && c != '\t') cur.push_back(c);
  }
  if (depth != 0) fail("E_GUEST_SYNTAX", at(line) + "unbalanced parenthesis");
  if (!cur.empty() || !out.empty()) out.push_back(cur);
  for (const std::string &o : out) if (o.empty()) fail("E_GUEST_SYNTAX", at(line) + "an operand is empty");
  return out;
}

Source parse_source(std::span<const std::uint8_t> text, const Manifest &m) {
  Source src;
  for (const std::uint8_t b : text) if (b != '\n' && b != '\t' && (b < 0x20 || b > 0x7E)) fail("E_GUEST_SYNTAX", "a guest-asm file is ASCII text with LF line ends");
  const std::string_view all(reinterpret_cast<const char *>(text.data()), text.size());
  const auto declared = [&](const std::string &rev) { return rev == "*" || std::any_of(m.revisions.begin(), m.revisions.end(), [&](const Revision &r) { return r.label == rev; }); };
  std::size_t line_no = 0, start = 0, lines = 0;
  Block *cur = nullptr;
  while (start <= all.size()) {
    const std::size_t nl = all.find('\n', start);
    std::string line(all.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
    start = nl == std::string_view::npos ? all.size() + 1 : nl + 1;
    ++line_no;
    if (const auto semi = line.find(';'); semi != std::string::npos) line.resize(semi);
    std::vector<std::string> words = split_ws(line);
    if (words.empty()) continue;
    if (++lines > kMaxGuestLines) fail("E_GUEST_SYNTAX", "a guest-asm file has at most 8192 statements");
    if (std::any_of(line.begin(), line.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) fail("E_GUEST_SYNTAX", at(line_no) + "guest-asm is lower-case only");
    if (words[0] == "free") {
      if (words.size() != 5) fail("E_GUEST_SYNTAX", at(line_no) + "free <revision|*> <start> <length> <fill>");
      Free f;
      std::uint64_t fill = 0;
      f.rev = words[1];
      f.line = line_no;
      if (!declared(f.rev)) fail("E_GUEST_REVISION", at(line_no) + "the revision \"" + f.rev + "\" is not declared");
      if (!hex_lit(words[2], f.start) || !hex_lit(words[3], f.length) || f.length == 0 || !hex_lit(words[4], fill) || fill > 0xFF) fail("E_GUEST_SYNTAX", at(line_no) + "free needs 0x hex start, length and a one-byte fill");
      f.fill = static_cast<std::uint8_t>(fill);
      if (f.start + f.length > kMaxImageBytes) fail("E_GUEST_RANGE", at(line_no) + "the free range lies beyond the largest supported image");
      if (src.frees.size() >= kMaxGuestFreeRanges) fail("E_GUEST_SYNTAX", "a guest-asm file declares at most 64 free ranges");
      src.frees.push_back(f);
      cur = nullptr;
      continue;
    }
    if (words[0] == "org") {
      if (words.size() != 3 && words.size() != 5) fail("E_GUEST_SYNTAX", at(line_no) + "org <revision|*> <offset> [at <address>]");
      Block b;
      b.rev = words[1];
      b.line = line_no;
      if (!declared(b.rev)) fail("E_GUEST_REVISION", at(line_no) + "the revision \"" + b.rev + "\" is not declared");
      if (!hex_lit(words[2], b.offset)) fail("E_GUEST_SYNTAX", at(line_no) + "the org offset is 0x lower-case hex");
      b.address = b.offset;
      if (words.size() == 5) {
        if (words[3] != "at" || !hex_lit(words[4], b.address)) fail("E_GUEST_SYNTAX", at(line_no) + "org ... at <0x address>");
        b.has_at = true;
      }
      if (src.blocks.size() >= kMaxGuestBlocks) fail("E_GUEST_SYNTAX", "a guest-asm file holds at most 64 blocks");
      src.blocks.push_back(std::move(b));
      cur = &src.blocks.back();
      continue;
    }
    Stmt s;
    s.line = line_no;
    std::string rest = line;
    if (words[0].back() == ':') {
      s.label = words[0].substr(0, words[0].size() - 1);
      if (!label_ok(s.label)) fail("E_GUEST_SYNTAX", at(line_no) + "a label is a token of a-z 0-9 _ . (not starting with a digit), at most 64 characters");
      rest = line.substr(line.find(':') + 1);
    }
    if (cur == nullptr) fail("E_GUEST_SYNTAX", at(line_no) + "a statement needs an org line first");
    const std::vector<std::string> w2 = split_ws(rest);
    if (!w2.empty()) {
      std::string op = w2[0];
      if (const auto dot = op.find('.'); dot != std::string::npos) { s.size = op.substr(dot + 1); op.resize(dot); }
      s.op = op;
      s.args = split_operands(std::string_view(rest).substr(rest.find(w2[0]) + w2[0].size()), line_no);
    }
    cur->stmts.push_back(std::move(s));
  }
  return src;
}

// ------------------------------------------------------------------ evaluation
struct Ctx {
  const Manifest &m;
  Cpu cpu;
  std::string rev;
  const std::map<std::string, std::int64_t> &labels;
  const std::set<std::string> &data_labels;   // labels defined after a `data` statement: never a control-transfer target
  bool final = false;   // pass 2: unknown labels and value ranges are real errors
  std::int64_t pc = 0;
  std::size_t line = 0;
};

struct Ev {
  std::int64_t value = 0;
  bool symbolic = false;   // involves a label or @symbol (a numeric literal is not a valid branch target)
};

Ev eval(const Ctx &c, std::string_view expr) {
  if (expr.empty()) fail("E_GUEST_OPERAND", at(c.line) + "an operand is empty");
  std::size_t op = std::string_view::npos;
  for (std::size_t i = 1; i < expr.size(); ++i) if (expr[i] == '+' || expr[i] == '-') { op = i; break; }
  const std::string_view base = expr.substr(0, op);
  std::int64_t addend = 0;
  if (op != std::string_view::npos) {
    std::int64_t n = 0;
    if (!int_lit(expr.substr(op + 1), n) || n < 0) fail("E_GUEST_OPERAND", at(c.line) + "the offset after + or - is a non-negative number");
    addend = expr[op] == '-' ? -n : n;
  }
  Ev r;
  std::int64_t n = 0;
  if (int_lit(base, n)) {
    r.value = n + addend;
    if (r.value < -(std::int64_t{1} << 31) || r.value >= (std::int64_t{1} << 32)) fail("E_GUEST_VALUE_RANGE", at(c.line) + "a value does not fit 32 bits");
    return r;
  }
  r.symbolic = true;
  if (base[0] == '@') {
    const std::string name(base.substr(1));
    Rejection why;
    const auto a = verified_symbol(c.m, name, c.rev, why);
    if (!a) fail(why.code, at(c.line) + why.message);
    r.value = static_cast<std::int64_t>(*a) + addend;
    return r;
  }
  if (!label_ok(base)) fail("E_GUEST_OPERAND", at(c.line) + "the operand \"" + std::string(expr) + "\" is not a number, label or @symbol");
  const auto it = c.labels.find(std::string(base));
  if (it == c.labels.end()) {
    if (c.final) fail("E_GUEST_LABEL_UNKNOWN", at(c.line) + "the label \"" + std::string(base) + "\" is not defined for the revision \"" + c.rev + "\"");
    return r;   // pass 1: sizes never depend on label values
  }
  r.value = it->second + addend;
  return r;
}

Ev target_of(const Ctx &c, std::string_view expr) {   // a control-transfer destination is a CODE label or a verified @symbol (+ a small offset): never a number, data or label arithmetic
  const Ev e = eval(c, expr);
  if (!e.symbolic) fail("E_GUEST_BRANCH_TARGET", at(c.line) + "a branch target is a label of the injected code or a verified @symbol, not a number");
  const std::size_t op = expr.find_first_of("+-", 1);
  const std::string_view base = expr.substr(0, op);
  if (base[0] != '@') {
    if (op != std::string_view::npos) fail("E_GUEST_BRANCH_TARGET", at(c.line) + "a branch target is a bare label; offsets are only allowed on @symbols");
    if (c.data_labels.count(std::string(base)) != 0) fail("E_GUEST_BRANCH_TARGET", at(c.line) + "the branch target is a data label; data is never executed");
  } else if (op != std::string_view::npos && (e.value < 0 || e.value > 0xFFFFFF)) {
    fail("E_GUEST_BRANCH_TARGET", at(c.line) + "the @symbol offset leaves the image");
  }
  return e;
}

void range(const Ctx &c, std::int64_t v, std::int64_t lo, std::int64_t hi, const char *what) {
  if (c.final && (v < lo || v > hi)) fail("E_GUEST_VALUE_RANGE", at(c.line) + what + " is out of range");
}

struct Insn {
  std::size_t pos = 0, len = 0;
  bool code = true;
  std::vector<std::pair<std::uint64_t, unsigned>> writes;   // statically-known absolute write destinations (address, bytes)
};

void put_be16(std::vector<std::uint8_t> &out, std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v >> 8U)); out.push_back(static_cast<std::uint8_t>(v)); }

// ------------------------------------------------------------------ M68K
enum class Ek { dn, an, ind, post, pre, disp, pcrel, absl, imm };
struct Ea {
  Ek kind = Ek::dn;
  unsigned reg = 0;
  std::string expr;
};

bool reg_of(std::string_view t, char letter, unsigned &r) {
  if (t == "sp" && letter == 'a') { r = 7; return true; }
  if (t.size() == 2 && t[0] == letter && t[1] >= '0' && t[1] <= '7') { r = static_cast<unsigned>(t[1] - '0'); return true; }
  return false;
}

Ea parse_ea(const Ctx &c, const std::string &t) {
  Ea e;
  if (t == "sr" || t == "usp") fail("E_GUEST_FORBIDDEN_INSTRUCTION", at(c.line) + "access to the status register or user stack pointer is not allowed in injected code");
  if (t == "ccr") fail("E_GUEST_OPERAND", at(c.line) + "the condition code register operand is not supported");
  if (reg_of(t, 'd', e.reg)) { e.kind = Ek::dn; return e; }
  if (reg_of(t, 'a', e.reg)) { e.kind = Ek::an; return e; }
  if (t[0] == '#') { e.kind = Ek::imm; e.expr = t.substr(1); return e; }
  if (t.size() >= 4 && t.compare(0, 2, "-(") == 0 && t.back() == ')' && reg_of(std::string_view(t).substr(2, t.size() - 3), 'a', e.reg)) { e.kind = Ek::pre; return e; }
  if (t.size() >= 5 && t.compare(t.size() - 2, 2, ")+") == 0 && t[0] == '(' && reg_of(std::string_view(t).substr(1, t.size() - 3), 'a', e.reg)) { e.kind = Ek::post; return e; }
  if (t[0] == '(' && t.back() == ')' && reg_of(std::string_view(t).substr(1, t.size() - 2), 'a', e.reg)) { e.kind = Ek::ind; return e; }
  if (t.back() == ')') {
    const auto open = t.rfind('(');
    if (open == std::string::npos || open == 0) fail("E_GUEST_OPERAND", at(c.line) + "unsupported addressing mode \"" + t + "\"");
    const std::string_view inner = std::string_view(t).substr(open + 1, t.size() - open - 2);
    e.expr = t.substr(0, open);
    if (inner == "pc") { e.kind = Ek::pcrel; return e; }
    if (reg_of(inner, 'a', e.reg)) { e.kind = Ek::disp; return e; }
    fail("E_GUEST_OPERAND", at(c.line) + "unsupported addressing mode \"" + t + "\"");
  }
  e.kind = Ek::absl;
  e.expr = t;
  return e;
}

unsigned ea_bits(const Ea &e) {
  switch (e.kind) {
    case Ek::dn: return e.reg;
    case Ek::an: return 8U | e.reg;
    case Ek::ind: return 16U | e.reg;
    case Ek::post: return 24U | e.reg;
    case Ek::pre: return 32U | e.reg;
    case Ek::disp: return 40U | e.reg;
    case Ek::pcrel: return 58U;
    case Ek::absl: return 57U;
    case Ek::imm: return 60U;
  }
  return 0;
}
bool mem_alt(Ek k) { return k == Ek::ind || k == Ek::post || k == Ek::pre || k == Ek::disp || k == Ek::absl; }
bool data_alt(Ek k) { return k == Ek::dn || mem_alt(k); }
bool control(Ek k) { return k == Ek::ind || k == Ek::disp || k == Ek::pcrel || k == Ek::absl; }

unsigned size_code(const Ctx &c, const Stmt &s, bool allow_b = true) {   // b=0 w=1 l=2
  if (s.size == "b" && allow_b) return 0;
  if (s.size == "w") return 1;
  if (s.size == "l") return 2;
  fail("E_GUEST_SYNTAX", at(c.line) + "the instruction needs a size suffix (" + std::string(allow_b ? ".b, " : "") + ".w or .l)");
}

struct W {   // the words of one instruction: [0] = opcode
  std::vector<std::uint16_t> w;
};

void ext_words(const Ctx &c, W &o, const Ea &e, unsigned szc, std::vector<std::pair<std::uint64_t, unsigned>> *writes, bool is_dest, unsigned write_bytes) {
  switch (e.kind) {
    case Ek::disp: {
      const Ev v = eval(c, e.expr);
      range(c, v.value, -32768, 32767, "a displacement");
      o.w.push_back(static_cast<std::uint16_t>(v.value));
      break;
    }
    case Ek::pcrel: {
      const Ev v = eval(c, e.expr);
      const std::int64_t d = v.value - (c.pc + 2 * static_cast<std::int64_t>(o.w.size()));
      range(c, d, -32768, 32767, "a PC-relative displacement");
      o.w.push_back(static_cast<std::uint16_t>(d));
      break;
    }
    case Ek::absl: {
      const Ev v = eval(c, e.expr);
      const auto a = static_cast<std::uint32_t>(v.value);
      o.w.push_back(static_cast<std::uint16_t>(a >> 16U));
      o.w.push_back(static_cast<std::uint16_t>(a));
      if (is_dest && writes != nullptr) writes->push_back({a & 0xFFFFFFU, write_bytes});
      break;
    }
    case Ek::imm: {
      const Ev v = eval(c, e.expr);
      if (szc == 0) { range(c, v.value, -128, 255, "an immediate byte"); o.w.push_back(static_cast<std::uint16_t>(v.value & 0xFF)); }
      else if (szc == 1) { range(c, v.value, -32768, 65535, "an immediate word"); o.w.push_back(static_cast<std::uint16_t>(v.value)); }
      else { o.w.push_back(static_cast<std::uint16_t>(static_cast<std::uint32_t>(v.value) >> 16U)); o.w.push_back(static_cast<std::uint16_t>(v.value)); }
      break;
    }
    default: break;
  }
}

unsigned bytes_of(unsigned szc) { return 1U << szc; }

std::vector<std::uint8_t> flatten(const W &o) {
  std::vector<std::uint8_t> out;
  for (const std::uint16_t w : o.w) put_be16(out, w);
  return out;
}

const std::pair<std::string_view, unsigned> kConds[] = {{"bra", 0}, {"bsr", 1}, {"bhi", 2}, {"bls", 3}, {"bcc", 4}, {"bhs", 4}, {"bcs", 5}, {"blo", 5}, {"bne", 6},
                                                         {"beq", 7}, {"bvc", 8}, {"bvs", 9}, {"bpl", 10}, {"bmi", 11}, {"bge", 12}, {"blt", 13}, {"bgt", 14}, {"ble", 15}};

std::vector<std::uint8_t> m68k_encode(const Ctx &c, const Stmt &s, Insn &meta) {
  const std::string &op = s.op;
  const auto &a = s.args;
  W o;
  const auto need = [&](std::size_t n) { if (a.size() != n) fail("E_GUEST_SYNTAX", at(c.line) + "\"" + op + "\" takes " + std::to_string(n) + " operand(s)"); };
  const auto no_size = [&] { if (!s.size.empty()) fail("E_GUEST_SYNTAX", at(c.line) + "\"" + op + "\" takes no size suffix"); };
  const struct { std::string_view name; std::uint16_t word; } kFixed[] = {{"nop", 0x4E71}, {"rts", 0x4E75}, {"rtr", 0x4E77}, {"rte", 0x4E73}, {"reset", 0x4E70}, {"trapv", 0x4E76}, {"illegal", 0x4AFC}};
  for (const auto &f : kFixed) if (op == f.name) { need(0); no_size(); o.w.push_back(f.word); return flatten(o); }
  if (op == "stop") { need(1); no_size(); const Ea e = parse_ea(c, a[0]); if (e.kind != Ek::imm) fail("E_GUEST_OPERAND", at(c.line) + "stop takes an immediate"); o.w.push_back(0x4E72); ext_words(c, o, e, 1, nullptr, false, 0); return flatten(o); }
  if (op == "trap") { need(1); no_size(); const Ea e = parse_ea(c, a[0]); if (e.kind != Ek::imm) fail("E_GUEST_OPERAND", at(c.line) + "trap takes #vector"); const Ev v = eval(c, e.expr); range(c, v.value, 0, 15, "a trap vector"); o.w.push_back(static_cast<std::uint16_t>(0x4E40 | (v.value & 15))); return flatten(o); }
  if (op == "moveq") {
    need(2); no_size();
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (src.kind != Ek::imm || dst.kind != Ek::dn) fail("E_GUEST_OPERAND", at(c.line) + "moveq #imm,dn");
    const Ev v = eval(c, src.expr);
    range(c, v.value, -128, 127, "a moveq immediate");
    o.w.push_back(static_cast<std::uint16_t>(0x7000 | (dst.reg << 9U) | (v.value & 0xFF)));
    return flatten(o);
  }
  if (op == "move" || op == "movea") {
    need(2);
    const unsigned szc = size_code(c, s, op == "move");
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (dst.kind == Ek::an || op == "movea") {
      if (dst.kind != Ek::an || szc == 0) fail("E_GUEST_OPERAND", at(c.line) + "movea needs an address-register destination and .w or .l");
    } else if (!data_alt(dst.kind)) {
      fail("E_GUEST_OPERAND", at(c.line) + "the destination is not alterable");
    }
    if (src.kind == Ek::an && szc == 0) fail("E_GUEST_OPERAND", at(c.line) + "an address register cannot be a byte source");
    const unsigned code = szc == 0 ? 1U : szc == 1 ? 3U : 2U;
    o.w.push_back(static_cast<std::uint16_t>((code << 12U) | ((ea_bits(dst) & 7U) << 9U) | ((ea_bits(dst) >> 3U) << 6U) | ea_bits(src)));
    ext_words(c, o, src, szc, nullptr, false, 0);
    ext_words(c, o, dst, szc, &meta.writes, true, bytes_of(szc));
    return flatten(o);
  }
  static const std::pair<std::string_view, std::uint16_t> kArith[] = {{"add", 0xD000}, {"sub", 0x9000}, {"and", 0xC000}, {"or", 0x8000}, {"cmp", 0xB000}};
  for (const auto &ar : kArith) {
    if (op != ar.first) continue;
    need(2);
    const unsigned szc = size_code(c, s);
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (dst.kind == Ek::dn) {
      if (src.kind == Ek::an && (szc == 0 || op == "and" || op == "or")) fail("E_GUEST_OPERAND", at(c.line) + "an address register is not a legal source here");
      o.w.push_back(static_cast<std::uint16_t>(ar.second | (dst.reg << 9U) | (szc << 6U) | ea_bits(src)));
      ext_words(c, o, src, szc, nullptr, false, 0);
    } else if (src.kind == Ek::dn && mem_alt(dst.kind) && op != "cmp") {
      o.w.push_back(static_cast<std::uint16_t>(ar.second | (src.reg << 9U) | ((4U + szc) << 6U) | ea_bits(dst)));
      ext_words(c, o, dst, szc, &meta.writes, true, bytes_of(szc));
    } else {
      fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" needs a data register as source or destination (adda/suba are not supported; use lea or addq)");
    }
    return flatten(o);
  }
  static const std::pair<std::string_view, std::uint16_t> kImm[] = {{"ori", 0x0000}, {"andi", 0x0200}, {"subi", 0x0400}, {"addi", 0x0600}, {"eori", 0x0A00}, {"cmpi", 0x0C00}};
  for (const auto &im : kImm) {
    if (op != im.first) continue;
    need(2);
    const unsigned szc = size_code(c, s);
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (src.kind != Ek::imm || !data_alt(dst.kind)) fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" takes #imm and a data-alterable destination");
    o.w.push_back(static_cast<std::uint16_t>(im.second | (szc << 6U) | ea_bits(dst)));
    ext_words(c, o, src, szc, nullptr, false, 0);
    ext_words(c, o, dst, szc, &meta.writes, true, bytes_of(szc));
    return flatten(o);
  }
  if (op == "addq" || op == "subq") {
    need(2);
    const unsigned szc = size_code(c, s);
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (src.kind != Ek::imm || !(data_alt(dst.kind) || (dst.kind == Ek::an && szc != 0))) fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" takes #1..#8 and an alterable destination");
    const Ev v = eval(c, src.expr);
    range(c, v.value, 1, 8, "a quick immediate");
    o.w.push_back(static_cast<std::uint16_t>((op == "addq" ? 0x5000 : 0x5100) | ((v.value & 7) << 9U) | (szc << 6U) | ea_bits(dst)));
    ext_words(c, o, dst, szc, &meta.writes, true, bytes_of(szc));
    return flatten(o);
  }
  static const std::pair<std::string_view, std::uint16_t> kUnary[] = {{"clr", 0x4200}, {"neg", 0x4400}, {"not", 0x4600}, {"tst", 0x4A00}};
  for (const auto &u : kUnary) {
    if (op != u.first) continue;
    need(1);
    const unsigned szc = size_code(c, s);
    const Ea dst = parse_ea(c, a[0]);
    if (!data_alt(dst.kind)) fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" needs a data-alterable operand");
    o.w.push_back(static_cast<std::uint16_t>(u.second | (szc << 6U) | ea_bits(dst)));
    ext_words(c, o, dst, szc, op == "tst" ? nullptr : &meta.writes, true, bytes_of(szc));
    return flatten(o);
  }
  if (op == "lea") {
    need(2); no_size();
    const Ea src = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (!control(src.kind) || dst.kind != Ek::an) fail("E_GUEST_OPERAND", at(c.line) + "lea <control ea>,an");
    o.w.push_back(static_cast<std::uint16_t>(0x41C0 | (dst.reg << 9U) | ea_bits(src)));
    ext_words(c, o, src, 2, nullptr, false, 0);
    return flatten(o);
  }
  if (op == "pea" || op == "jmp" || op == "jsr") {
    need(1); no_size();
    const Ea e = parse_ea(c, a[0]);
    if (!control(e.kind)) fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" needs a control addressing mode");
    if ((op != "pea") && (e.kind == Ek::absl || e.kind == Ek::pcrel)) (void)target_of(c, e.expr);
    o.w.push_back(static_cast<std::uint16_t>((op == "pea" ? 0x4840 : op == "jmp" ? 0x4EC0 : 0x4E80) | ea_bits(e)));
    ext_words(c, o, e, 2, nullptr, false, 0);
    return flatten(o);
  }
  if (op == "swap") { need(1); no_size(); const Ea e = parse_ea(c, a[0]); if (e.kind != Ek::dn) fail("E_GUEST_OPERAND", at(c.line) + "swap dn"); o.w.push_back(static_cast<std::uint16_t>(0x4840 | e.reg)); return flatten(o); }
  if (op == "ext") {
    need(1);
    const Ea e = parse_ea(c, a[0]);
    if (e.kind != Ek::dn || (s.size != "w" && s.size != "l")) fail("E_GUEST_OPERAND", at(c.line) + "ext.w dn or ext.l dn");
    o.w.push_back(static_cast<std::uint16_t>((s.size == "w" ? 0x4880 : 0x48C0) | e.reg));
    return flatten(o);
  }
  static const struct { std::string_view name; unsigned type, dir; } kShift[] = {{"asr", 0, 0}, {"asl", 0, 1}, {"lsr", 1, 0}, {"lsl", 1, 1}, {"ror", 3, 0}, {"rol", 3, 1}};
  for (const auto &sh : kShift) {
    if (op != sh.name) continue;
    need(2);
    const unsigned szc = size_code(c, s);
    const Ea cnt = parse_ea(c, a[0]), dst = parse_ea(c, a[1]);
    if (dst.kind != Ek::dn) fail("E_GUEST_OPERAND", at(c.line) + "\"" + op + "\" shifts a data register");
    unsigned count = 0, ir = 0;
    if (cnt.kind == Ek::imm) { const Ev v = eval(c, cnt.expr); range(c, v.value, 1, 8, "a shift count"); count = static_cast<unsigned>(v.value) & 7U; }
    else if (cnt.kind == Ek::dn) { count = cnt.reg; ir = 1; }
    else fail("E_GUEST_OPERAND", at(c.line) + "the shift count is #1..#8 or a data register");
    o.w.push_back(static_cast<std::uint16_t>(0xE000 | (count << 9U) | (sh.dir << 8U) | (szc << 6U) | (ir << 5U) | (sh.type << 3U) | dst.reg));
    return flatten(o);
  }
  if (op == "dbra" || op == "dbf") {
    need(2);
    const Ea r = parse_ea(c, a[0]);
    if (r.kind != Ek::dn) fail("E_GUEST_OPERAND", at(c.line) + "dbra dn,label");
    const Ev t = target_of(c, a[1]);
    const std::int64_t d = t.value - (c.pc + 2);
    if (c.final && (d < -32768 || d > 32767)) fail("E_GUEST_BRANCH_RANGE", at(c.line) + "the branch target is out of range");
    o.w.push_back(static_cast<std::uint16_t>(0x51C8 | r.reg));
    o.w.push_back(static_cast<std::uint16_t>(d));
    return flatten(o);
  }
  for (const auto &cd : kConds) {
    if (op != cd.first) continue;
    need(1);
    const Ev t = target_of(c, a[0]);
    const std::int64_t d = t.value - (c.pc + 2);
    const bool byte = s.size == "s";
    if (!s.size.empty() && s.size != "s" && s.size != "w") fail("E_GUEST_SYNTAX", at(c.line) + "a branch size is .s or .w");
    if (byte) {
      if (c.final && (d < -128 || d > 127 || d == 0 || d == -1)) fail("E_GUEST_BRANCH_RANGE", at(c.line) + "the short branch target is out of range");
      o.w.push_back(static_cast<std::uint16_t>(0x6000 | (cd.second << 8U) | (d & 0xFF)));
    } else {
      if (c.final && (d < -32768 || d > 32767)) fail("E_GUEST_BRANCH_RANGE", at(c.line) + "the branch target is out of range");
      o.w.push_back(static_cast<std::uint16_t>(0x6000 | (cd.second << 8U)));
      o.w.push_back(static_cast<std::uint16_t>(d));
    }
    return flatten(o);
  }
  fail("E_GUEST_SYNTAX", at(c.line) + "unknown or unsupported mnemonic \"" + op + "\"");
}

// ------------------------------------------------------------------ Z80
int r8_of(std::string_view t) {
  static const char *const names[] = {"b", "c", "d", "e", "h", "l", "(hl)", "a"};
  for (int i = 0; i < 8; ++i) if (t == names[i]) return i;
  return -1;
}
int rr_of(std::string_view t, bool af) {
  static const char *const names[] = {"bc", "de", "hl", "sp"};
  for (int i = 0; i < 4; ++i) if (t == names[i] && !(af && i == 3)) return i;
  if (af && t == "af") return 3;
  return -1;
}
int cc_of(std::string_view t) {
  static const char *const names[] = {"nz", "z", "nc", "c", "po", "pe", "p", "m"};
  for (int i = 0; i < 8; ++i) if (t == names[i]) return i;
  return -1;
}
bool is_mem(std::string_view t) { return t.size() > 2 && t.front() == '(' && t.back() == ')' && r8_of(t) < 0 && t != "(bc)" && t != "(de)"; }

std::vector<std::uint8_t> z80_encode(const Ctx &c, const Stmt &s, Insn &meta) {
  const std::string &op = s.op;
  const auto &a = s.args;
  std::vector<std::uint8_t> o;
  if (!s.size.empty()) fail("E_GUEST_SYNTAX", at(c.line) + "the Z80 mnemonic \"" + op + "\" takes no size suffix");
  const auto need = [&](std::size_t n) { if (a.size() != n) fail("E_GUEST_SYNTAX", at(c.line) + "\"" + op + "\" takes " + std::to_string(n) + " operand(s)"); };
  const auto imm8 = [&](const std::string &t) { const Ev v = eval(c, t); range(c, v.value, -128, 255, "an 8-bit value"); o.push_back(static_cast<std::uint8_t>(v.value)); };
  const auto imm16 = [&](const std::string &t) { const Ev v = eval(c, t); range(c, v.value, -32768, 65535, "a 16-bit value"); o.push_back(static_cast<std::uint8_t>(v.value)); o.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v.value) >> 8U)); };
  const auto mem_addr = [&](const std::string &t) { const Ev v = eval(c, t.substr(1, t.size() - 2)); range(c, v.value, 0, 65535, "an address"); return v.value; };
  const struct { std::string_view name; std::uint8_t b0, b1; } kFixed[] = {{"nop", 0x00, 0}, {"halt", 0x76, 0}, {"di", 0xF3, 0}, {"ei", 0xFB, 0}, {"scf", 0x37, 0}, {"ccf", 0x3F, 0}, {"cpl", 0x2F, 0},
                                                                         {"rlca", 0x07, 0}, {"rrca", 0x0F, 0}, {"reti", 0xED, 0x4D}, {"retn", 0xED, 0x45}};
  for (const auto &f : kFixed) if (op == f.name) { need(0); o.push_back(f.b0); if (f.b1 != 0) o.push_back(f.b1); return o; }
  if (op == "ret") {
    if (a.empty()) { o.push_back(0xC9); return o; }
    need(1);
    const int cc = cc_of(a[0]);
    if (cc < 0) fail("E_GUEST_OPERAND", at(c.line) + "ret takes a condition");
    o.push_back(static_cast<std::uint8_t>(0xC0 | (cc << 3)));
    return o;
  }
  if (op == "rst") { need(1); const Ev v = eval(c, a[0]); range(c, v.value, 0, 0x38, "an rst vector"); o.push_back(static_cast<std::uint8_t>(0xC7 | (v.value & 0x38))); return o; }
  if (op == "im") { need(1); const Ev v = eval(c, a[0]); range(c, v.value, 0, 2, "an interrupt mode"); o.push_back(0xED); o.push_back(v.value == 0 ? 0x46 : v.value == 1 ? 0x56 : 0x5E); return o; }
  if (op == "ex") { need(2); if (a[0] != "de" || a[1] != "hl") fail("E_GUEST_OPERAND", at(c.line) + "only ex de,hl is supported"); o.push_back(0xEB); return o; }
  if (op == "out" || op == "in") {
    need(2);
    const bool is_out = op == "out";
    const std::string &port = is_out ? a[0] : a[1], &reg = is_out ? a[1] : a[0];
    if (reg != "a" || !is_mem(port)) fail("E_GUEST_OPERAND", at(c.line) + (is_out ? "out (n),a" : "in a,(n)"));
    o.push_back(is_out ? 0xD3 : 0xDB);
    imm8(port.substr(1, port.size() - 2));
    return o;
  }
  if (op == "ld") {
    need(2);
    const std::string &d = a[0], &r = a[1];
    if (d == "i" && r == "a") { o = {0xED, 0x47}; return o; }
    if (d == "r" && r == "a") { o = {0xED, 0x4F}; return o; }
    if (d == "a" && r == "i") { o = {0xED, 0x57}; return o; }
    if (d == "a" && r == "r") { o = {0xED, 0x5F}; return o; }
    if (d == "sp" && r == "hl") { o.push_back(0xF9); return o; }
    const int d8 = r8_of(d), s8 = r8_of(r), drr = rr_of(d, false);
    if (d8 >= 0 && s8 >= 0) {
      if (d8 == 6 && s8 == 6) fail("E_GUEST_OPERAND", at(c.line) + "ld (hl),(hl) is not an instruction");
      o.push_back(static_cast<std::uint8_t>(0x40 | (d8 << 3) | s8));
      return o;
    }
    if (d8 >= 0 && r[0] != '(' && rr_of(r, false) < 0 && r != "i" && r != "r") { o.push_back(static_cast<std::uint8_t>(0x06 | (d8 << 3))); imm8(r); return o; }
    if (drr >= 0 && r[0] != '(' && rr_of(r, false) < 0) { o.push_back(static_cast<std::uint8_t>(0x01 | (drr << 4))); imm16(r); return o; }
    if (d == "a" && r == "(bc)") { o.push_back(0x0A); return o; }
    if (d == "a" && r == "(de)") { o.push_back(0x1A); return o; }
    if (d == "(bc)" && r == "a") { o.push_back(0x02); return o; }
    if (d == "(de)" && r == "a") { o.push_back(0x12); return o; }
    if (d == "a" && is_mem(r)) { o.push_back(0x3A); imm16(r.substr(1, r.size() - 2)); return o; }
    if (is_mem(d) && r == "a") { meta.writes.push_back({static_cast<std::uint64_t>(mem_addr(d)), 1}); o.push_back(0x32); imm16(d.substr(1, d.size() - 2)); return o; }
    if (d == "hl" && is_mem(r)) { o.push_back(0x2A); imm16(r.substr(1, r.size() - 2)); return o; }
    if (is_mem(d) && r == "hl") { meta.writes.push_back({static_cast<std::uint64_t>(mem_addr(d)), 2}); o.push_back(0x22); imm16(d.substr(1, d.size() - 2)); return o; }
    fail("E_GUEST_OPERAND", at(c.line) + "unsupported ld operands");
  }
  if (op == "inc" || op == "dec") {
    need(1);
    const int r8 = r8_of(a[0]), rr = rr_of(a[0], false);
    if (r8 >= 0) { o.push_back(static_cast<std::uint8_t>((op == "inc" ? 0x04 : 0x05) | (r8 << 3))); return o; }
    if (rr >= 0) { o.push_back(static_cast<std::uint8_t>((op == "inc" ? 0x03 : 0x0B) | (rr << 4))); return o; }
    fail("E_GUEST_OPERAND", at(c.line) + "inc/dec take an 8-bit register, (hl) or a 16-bit pair");
  }
  static const std::pair<std::string_view, std::uint8_t> kAlu[] = {{"add", 0x80}, {"adc", 0x88}, {"sub", 0x90}, {"sbc", 0x98}, {"and", 0xA0}, {"xor", 0xA8}, {"or", 0xB0}, {"cp", 0xB8}};
  for (const auto &al : kAlu) {
    if (op != al.first) continue;
    if (op == "add" && a.size() == 2 && a[0] == "hl") {
      const int rr = rr_of(a[1], false);
      if (rr < 0) fail("E_GUEST_OPERAND", at(c.line) + "add hl,rr");
      o.push_back(static_cast<std::uint8_t>(0x09 | (rr << 4)));
      return o;
    }
    const bool two = (op == "add" || op == "adc" || op == "sbc");   // `add a,x`; the others may omit the `a,`
    if (a.size() == 2 && a[0] != "a") fail("E_GUEST_OPERAND", at(c.line) + "the destination of \"" + op + "\" is a");
    if (a.size() != 1 && !(two && a.size() == 2)) fail("E_GUEST_SYNTAX", at(c.line) + "\"" + op + "\" takes an operand");
    const std::string &src = a.back();
    const int r8 = r8_of(src);
    if (r8 >= 0) o.push_back(static_cast<std::uint8_t>(al.second | r8));
    else { o.push_back(static_cast<std::uint8_t>(al.second | 0x46)); imm8(src); }
    return o;
  }
  if (op == "push" || op == "pop") {
    need(1);
    const int rr = rr_of(a[0], true);
    if (rr < 0) fail("E_GUEST_OPERAND", at(c.line) + "push/pop take bc, de, hl or af");
    o.push_back(static_cast<std::uint8_t>((op == "push" ? 0xC5 : 0xC1) | (rr << 4)));
    return o;
  }
  if (op == "jp" || op == "call") {
    const std::uint8_t base = op == "jp" ? 0xC2 : 0xC4, plain = op == "jp" ? 0xC3 : 0xCD;
    if (op == "jp" && a.size() == 1 && a[0] == "(hl)") { o.push_back(0xE9); return o; }
    if (a.size() == 1) { (void)target_of(c, a[0]); o.push_back(plain); imm16(a[0]); return o; }
    need(2);
    const int cc = cc_of(a[0]);
    if (cc < 0) fail("E_GUEST_OPERAND", at(c.line) + "unknown condition");
    (void)target_of(c, a[1]);
    o.push_back(static_cast<std::uint8_t>(base | (cc << 3)));
    imm16(a[1]);
    return o;
  }
  if (op == "jr" || op == "djnz") {
    std::uint8_t opc = op == "jr" ? 0x18 : 0x10;
    if (op == "jr" && a.size() == 2) {
      const int cc = cc_of(a[0]);
      if (cc < 0 || cc > 3) fail("E_GUEST_OPERAND", at(c.line) + "jr conditions are nz, z, nc, c");
      opc = static_cast<std::uint8_t>(0x20 | (cc << 3));
    } else {
      need(1);
    }
    const Ev t = target_of(c, a.back());
    const std::int64_t d = t.value - (c.pc + 2);
    if (c.final && (d < -128 || d > 127)) fail("E_GUEST_BRANCH_RANGE", at(c.line) + "the relative branch target is out of range");
    o.push_back(opc);
    o.push_back(static_cast<std::uint8_t>(d));
    return o;
  }
  fail("E_GUEST_SYNTAX", at(c.line) + "unknown or unsupported mnemonic \"" + op + "\"");
}

// ------------------------------------------------------------------ data directives
std::vector<std::uint8_t> data_encode(const Ctx &c, const Stmt &s) {
  const unsigned width = s.op == "dc" ? (s.size == "b" ? 1U : s.size == "w" ? 2U : s.size == "l" ? 4U : 0U) : s.op == "db" ? 1U : 2U;
  if (width == 0) fail("E_GUEST_SYNTAX", at(c.line) + "dc needs .b, .w or .l");
  if (s.args.empty()) fail("E_GUEST_SYNTAX", at(c.line) + "a data directive needs at least one value");
  std::vector<std::uint8_t> out;
  const bool big = c.cpu == Cpu::m68k;
  for (const std::string &t : s.args) {
    const Ev v = eval(c, t);
    if (width == 1) { range(c, v.value, -128, 255, "a byte"); out.push_back(static_cast<std::uint8_t>(v.value)); continue; }
    if (width == 2) range(c, v.value, -32768, 65535, "a word");
    const auto u = static_cast<std::uint32_t>(v.value);
    for (unsigned i = 0; i < width; ++i) {
      const unsigned shift = 8U * (big ? width - 1 - i : i);
      out.push_back(static_cast<std::uint8_t>(u >> shift));
    }
  }
  return out;
}

// ------------------------------------------------------------------ assembly of one revision
struct OutBlock {
  std::uint64_t offset = 0, address = 0;
  bool has_at = false;
  std::size_t line = 0;
  std::vector<std::uint8_t> bytes;
  std::vector<Insn> insns;
};
struct Assembled {
  std::vector<Free> frees;
  std::vector<OutBlock> blocks;
};

bool is_data_op(const std::string &op) { return op == "dc" || op == "db" || op == "dw"; }

Assembled assemble_revision(const Manifest &m, Cpu cpu, const Source &src, const Revision &rev) {
  Assembled out;
  for (const Free &f : src.frees) if (f.rev == "*" || f.rev == rev.label) out.frees.push_back(f);
  std::vector<const Block *> blocks;
  for (const Block &b : src.blocks) if (b.rev == "*" || b.rev == rev.label) blocks.push_back(&b);
  std::map<std::string, std::int64_t> labels;
  std::set<std::string> data_labels;
  for (int pass = 0; pass < 2; ++pass) {
    const bool final = pass == 1;
    if (final) out.blocks.clear();
    for (const Block *b : blocks) {
      OutBlock ob;
      ob.offset = b->offset;
      ob.address = b->address;
      ob.has_at = b->has_at;
      ob.line = b->line;
      bool data_mode = false;
      std::string last_op;
      std::size_t last_args = 0;
      for (const Stmt &s : b->stmts) {
        const auto pc = static_cast<std::int64_t>(b->address + ob.bytes.size());
        Ctx c{m, cpu, rev.label, labels, data_labels, final, pc, s.line};
        if (!s.label.empty()) {
          if (!final && !labels.emplace(s.label, pc).second) fail("E_GUEST_LABEL_DUPLICATE", at(s.line) + "the label \"" + s.label + "\" is defined twice for the revision \"" + rev.label + "\"");
          if (!final && data_mode) data_labels.insert(s.label);
        }
        if (s.op.empty()) continue;
        if (s.op == "data" || s.op == "code") {
          if (!s.args.empty() || !s.size.empty()) fail("E_GUEST_SYNTAX", at(s.line) + "data/code take no operands");
          if (s.op == "data" && !last_op.empty()) {   // execution must not run into data: the instruction before it ends the control flow
            const bool m = cpu == Cpu::m68k;
            const bool ends = m ? (last_op == "rts" || last_op == "rtr" || last_op == "jmp" || last_op == "bra")
                                : ((last_op == "ret" && last_args == 0) || (last_op == "jp" && last_args == 1) || (last_op == "jr" && last_args == 1));
            if (!ends) fail("E_GUEST_SYNTAX", at(s.line) + "`data` must follow an unconditional transfer (rts, jmp, bra / ret, jp, jr): code may not fall into data");
          }
          data_mode = s.op == "data";
          last_op.clear();
          continue;
        }
        if (s.op == "even") {
          if (!s.args.empty() || !s.size.empty()) fail("E_GUEST_SYNTAX", at(s.line) + "even takes no operands");
          if ((pc & 1) != 0) ob.bytes.push_back(0);
          continue;
        }
        Insn meta;
        meta.pos = ob.bytes.size();
        std::vector<std::uint8_t> bytes;
        if (is_data_op(s.op)) {
          if (cpu == Cpu::m68k ? s.op != "dc" : s.op == "dc") fail("E_GUEST_SYNTAX", at(s.line) + "\"" + s.op + "\" does not exist for this CPU");
          bytes = data_encode(c, s);
          if (!data_mode) fail("E_GUEST_SYNTAX", at(s.line) + "raw data belongs after a `data` statement; the code stream holds instructions only");
          meta.code = false;
        } else {
          if (data_mode) fail("E_GUEST_SYNTAX", at(s.line) + "an instruction after `data` needs a `code` statement first");
          if (cpu == Cpu::m68k && (pc & 1) != 0) fail("E_GUEST_ALIGN", at(s.line) + "an M68K instruction starts at an odd address");
          bytes = cpu == Cpu::m68k ? m68k_encode(c, s, meta) : z80_encode(c, s, meta);
          last_op = s.op;
          last_args = s.args.size();
        }
        meta.len = bytes.size();
        ob.bytes.insert(ob.bytes.end(), bytes.begin(), bytes.end());
        if (final) ob.insns.push_back(std::move(meta));
        if (ob.bytes.size() > kMaxGuestCodeBytes) fail("E_GUEST_RANGE", "the injected code exceeds the guest code cap");
      }
      if (ob.bytes.empty()) fail("E_GUEST_SYNTAX", at(b->line) + "a block holds no code or data");
      if (final) out.blocks.push_back(std::move(ob));
    }
  }
  return out;
}

// ------------------------------------------------------------------ static validation of the emitted bytes
bool m68k_forbidden(std::uint16_t w) {
  if (w == 0x4E70 || w == 0x4E72 || w == 0x4E73 || w == 0x4E76 || w == 0x4AFC || w == 0x4E7A || w == 0x4E7B) return true;   // reset stop rte trapv illegal movec
  if ((w & 0xFFF0) == 0x4E40 || (w & 0xFFF0) == 0x4E60) return true;                                                       // trap, move usp
  if ((w & 0xFFC0) == 0x46C0 || (w & 0xFFC0) == 0x40C0) return true;                                                       // move to / from sr
  if (w == 0x027C || w == 0x007C || w == 0x0A7C) return true;                                                              // andi/ori/eori to sr
  return (w & 0xF000) == 0xA000 || (w & 0xF000) == 0xF000;                                                                 // line-A / line-F
}
bool z80_forbidden(const std::vector<std::uint8_t> &b, std::size_t pos) {
  const std::uint8_t b0 = b[pos];
  if (b0 == 0x76 || b0 == 0xF3 || b0 == 0xFB || b0 == 0xD3 || b0 == 0xDB || (b0 & 0xC7) == 0xC7) return true;   // halt di ei out in rst
  if (b0 == 0xED && pos + 1 < b.size()) {
    const std::uint8_t b1 = b[pos + 1];
    if (b1 == 0x45 || b1 == 0x4D || b1 == 0x46 || b1 == 0x56 || b1 == 0x5E || b1 == 0x47 || b1 == 0x4F || b1 == 0x57 || b1 == 0x5F || (b1 & 0xC7) == 0x40 || (b1 & 0xC7) == 0x41) return true;
  }
  return false;
}

void validate_block(const Manifest &m, Cpu cpu, const OutBlock &b) {
  std::uint64_t lo = 0, hi = 0;
  if (cpu == Cpu::m68k) { lo = 0xFF0000; hi = 0x1000000; }
  else if (m.platform == "master-system") { lo = 0xC000; hi = 0xE000; }
  else { lo = 0; hi = 0x2000; }
  for (const Insn &i : b.insns) {
    if (!i.code) continue;
    const bool bad = cpu == Cpu::m68k ? i.len >= 2 && m68k_forbidden(static_cast<std::uint16_t>((b.bytes[i.pos] << 8U) | b.bytes[i.pos + 1])) : z80_forbidden(b.bytes, i.pos);
    if (bad) fail("E_GUEST_FORBIDDEN_INSTRUCTION", "the injected code at offset 0x" + hex(b.offset + i.pos) + " contains an instruction the platform contract forbids (privileged, trap, port I/O, interrupt control or halt)");
    for (const auto &w : i.writes)
      if (w.first < lo || w.first + w.second > hi) fail("E_GUEST_WRITE_OUTSIDE_RAM", "the injected code writes to an absolute address outside the declared writable RAM window");
  }
}

// ------------------------------------------------------------------ placement checks (image independent)
void check_placement(const Manifest &m, Cpu cpu, const Revision &rev, const Assembled &a, const std::vector<Range> &edits) {
  std::vector<Free> frees = a.frees;
  std::sort(frees.begin(), frees.end(), [](const Free &x, const Free &y) { return x.start < y.start; });
  for (std::size_t i = 1; i < frees.size(); ++i)
    if (frees[i - 1].start + frees[i - 1].length > frees[i].start) fail("E_GUEST_COLLISION", at(frees[i].line) + "the free range overlaps the free range of line " + std::to_string(frees[i - 1].line) + " in the revision \"" + rev.label + "\"");
  for (const Range &e : edits)
    for (const Free &f : frees)
      if (e.start < f.start + f.length && f.start < e.end) fail("E_GUEST_COLLISION", at(f.line) + "a ROM edit overlaps the free range in the revision \"" + rev.label + "\"");
  std::vector<const OutBlock *> bs;
  for (const OutBlock &b : a.blocks) bs.push_back(&b);
  std::sort(bs.begin(), bs.end(), [](const OutBlock *x, const OutBlock *y) { return x->offset < y->offset; });
  for (std::size_t i = 0; i < bs.size(); ++i) {
    const OutBlock &b = *bs[i];
    const std::uint64_t end = b.offset + b.bytes.size();
    if (i > 0 && bs[i - 1]->offset + bs[i - 1]->bytes.size() > b.offset) fail("E_GUEST_COLLISION", at(b.line) + "the block overlaps the block of line " + std::to_string(bs[i - 1]->line) + " in the revision \"" + rev.label + "\"");
    if (std::none_of(frees.begin(), frees.end(), [&](const Free &f) { return b.offset >= f.start && end <= f.start + f.length; }))
      fail("E_GUEST_OUTSIDE_FREE_RANGE", at(b.line) + "the injected block does not lie wholly inside one declared free range of the revision \"" + rev.label + "\"");
    for (const Range &e : edits) if (e.start < end && b.offset < e.end) fail("E_GUEST_COLLISION", at(b.line) + "a ROM edit overlaps the injected block in the revision \"" + rev.label + "\"");
    // CPU address model (a per-platform table, mod-platform-adapters.md): the injected bytes must be executable at the address the code was assembled for
    if (cpu == Cpu::m68k) {
      if (m.platform != "genesis") fail("E_GUEST_ADDRESS", "M68K guest code exists only on genesis");
      if (b.address != b.offset) fail("E_GUEST_ADDRESS", at(b.line) + "M68K code is placed at its cartridge address; `at` is not allowed");
      if ((b.offset & 1) != 0) fail("E_GUEST_ALIGN", at(b.line) + "an M68K block starts at an even offset");
      if (end > 0x400000) fail("E_MAPPER_UNSUPPORTED", at(b.line) + "the block lies beyond the 4 MiB cartridge window; bank-switched mappers are not supported");
    } else if (m.platform == "master-system") {
      if (rev.mapper != "rom_only") fail("E_MAPPER_UNSUPPORTED", at(b.line) + "the revision's mapper is not supported for injection (only rom_only; bank-switched mappers are out of scope)");
      if (b.address != b.offset || end > 0xC000) fail("E_GUEST_ADDRESS", at(b.line) + "Z80 code on a rom_only Master System image is placed at its offset below 0xC000");
    } else {
      if (!b.has_at) fail("E_GUEST_ADDRESS", at(b.line) + "Genesis Z80 code needs an `at` Z80 sound-RAM address (its ROM offset is not where it runs)");
      if (b.address + b.bytes.size() > 0x2000) fail("E_GUEST_ADDRESS", at(b.line) + "Genesis Z80 code is assembled for sound-RAM addresses below 0x2000");
    }
  }
}

Cpu cpu_of(const Manifest &m, std::string_view cpu) {
  if (cpu == "m68k") { if (m.platform != "genesis") fail("E_PAYLOAD_PLATFORM", "m68k guest code exists only on genesis"); return Cpu::m68k; }
  if (cpu == "z80") return Cpu::z80;
  fail("E_FIELD_VALUE", "guest-asm cpu must be m68k or z80");
}

const Payload *payload_of(const Manifest &m, std::string_view kind) {
  for (const Payload &p : m.payloads) if (p.kind == kind) return &p;
  return nullptr;
}

std::span<const std::uint8_t> file_of(const std::map<std::string, std::vector<std::uint8_t>> &files, const Payload *p) {
  if (p == nullptr) return {};
  const auto it = files.find(p->path);
  return it == files.end() ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>(it->second);
}

// ------------------------------------------------------------------ Genesis header
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at_) {
  return (static_cast<std::uint32_t>(b[at_]) << 24U) | (static_cast<std::uint32_t>(b[at_ + 1]) << 16U) | (static_cast<std::uint32_t>(b[at_ + 2]) << 8U) | b[at_ + 3];
}
void set_be32(std::vector<std::uint8_t> &b, std::size_t at_, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) b[at_ + i] = static_cast<std::uint8_t>(v >> (24U - 8U * i)); }
bool ssf_mapper(std::span<const std::uint8_t> image) { return image.size() >= 0x108 && std::equal(image.begin() + 0x100, image.begin() + 0x108, "SEGA SSF"); }

}  // namespace

std::uint16_t genesis_checksum(std::span<const std::uint8_t> image, std::uint32_t end_exclusive) {
  std::uint32_t sum = 0;
  for (std::size_t i = 0x200; i + 1 < end_exclusive && i + 1 < image.size(); i += 2) sum += (static_cast<std::uint32_t>(image[i]) << 8U) | image[i + 1];
  return static_cast<std::uint16_t>(sum & 0xFFFFU);
}

bool genesis_header_verifies(std::span<const std::uint8_t> image) {
  if (image.size() < 0x200 || (image.size() & 1) != 0 || be32(image, 0x1A0) != 0) return false;
  const std::uint32_t end = be32(image, 0x1A4);
  if (static_cast<std::uint64_t>(end) + 1 != image.size()) return false;
  return genesis_checksum(image, end + 1) == static_cast<std::uint16_t>((image[0x18E] << 8U) | image[0x18F]);
}

std::optional<Rejection> check_guest_asm(const Manifest &m, std::string_view cpu_name, std::span<const std::uint8_t> text, std::span<const std::uint8_t> edit_set) {
  try {
    const Cpu cpu = cpu_of(m, cpu_name);
    const Source src = parse_source(text, m);
    if (src.blocks.empty()) fail("E_GUEST_SYNTAX", "a guest-asm file holds no org block");
    for (const Revision &rev : m.revisions) {
      const Assembled a = assemble_revision(m, cpu, src, rev);
      for (const OutBlock &b : a.blocks) validate_block(m, cpu, b);
      std::vector<Range> edits;
      if (!edit_set.empty()) {
        const EditRanges er = rom_edit_ranges(m, edit_set, rev.label);
        if (!er.ok) return er.rejection;
        edits = er.ranges;
      }
      check_placement(m, cpu, rev, a, edits);
    }
    return std::nullopt;
  } catch (const Fail &f) {
    return Rejection{f.code, f.message};
  }
}

bool has_target_edits(const Manifest &m) { return payload_of(m, "rom-edit-set") != nullptr || payload_of(m, "guest-asm") != nullptr; }

TargetImage build_target_image(const Manifest &m, const std::map<std::string, std::vector<std::uint8_t>> &files, std::span<const std::uint8_t> original) {
  TargetImage out;
  try {
    const std::string digest = sha256_hex(original);
    const auto rev = std::find_if(m.revisions.begin(), m.revisions.end(), [&](const Revision &r) { return r.sha256 == digest; });
    if (rev == m.revisions.end()) fail("E_IMAGE_MISMATCH", "the original image is not one of the declared target revisions");
    out.revision = rev->label;
    out.image.assign(original.begin(), original.end());
    if (const Payload *edits = payload_of(m, "rom-edit-set")) {
      const EditedImage e = apply_rom_edits(m, file_of(files, edits), rev->label, original);
      if (!e.ok) { out.rejection = e.rejection; out.image.clear(); return out; }
      out.image = e.image;
      out.edits_applied = e.applied;
    }
    if (const Payload *gp = payload_of(m, "guest-asm")) {
      const Cpu cpu = cpu_of(m, gp->cpu);
      const Source src = parse_source(file_of(files, gp), m);
      const Assembled a = assemble_revision(m, cpu, src, *rev);
      for (const OutBlock &b : a.blocks) validate_block(m, cpu, b);
      std::vector<Range> edits;
      if (const Payload *ep = payload_of(m, "rom-edit-set")) {
        const EditRanges er = rom_edit_ranges(m, file_of(files, ep), rev->label);
        if (!er.ok) { out.rejection = er.rejection; out.image.clear(); return out; }
        edits = er.ranges;
      }
      check_placement(m, cpu, *rev, a, edits);
      if (m.platform == "genesis" && (ssf_mapper(original) || original.size() > 0x400000)) fail("E_MAPPER_UNSUPPORTED", "the image uses a bank-switched cartridge mapper, which guest injection does not support");
      for (const Free &f : a.frees) {
        if (f.start + f.length > original.size()) fail("E_GUEST_RANGE", at(f.line) + "the free range lies beyond the end of the image");
        for (std::uint64_t i = f.start; i < f.start + f.length; ++i)
          if (original[static_cast<std::size_t>(i)] != f.fill) fail("E_GUEST_RANGE_NOT_FREE", at(f.line) + "the original bytes of the free range are not all the declared fill value");
      }
      for (const OutBlock &b : a.blocks) {
        std::copy(b.bytes.begin(), b.bytes.end(), out.image.begin() + static_cast<std::ptrdiff_t>(b.offset));
        ++out.blocks_injected;
        out.injected_bytes += b.bytes.size();
      }
    }
    // header repair (ADR 0293): only the documented Genesis header; `none` is an explicit opt-out; absent = repair when the original verifies and the image changed
    const bool changed = !std::equal(out.image.begin(), out.image.end(), original.begin(), original.end());
    const bool original_ok = m.platform == "genesis" && genesis_header_verifies(original);
    if (m.header_repair == "genesis-checksum") {
      if (m.platform != "genesis") fail("E_HEADER_UNSUPPORTED", "header repair exists only for the Genesis header");
      if (out.image.size() < 0x200 || (out.image.size() & 1) != 0 || out.image.size() > 0x400000 || be32(out.image, 0x1A0) != 0) fail("E_HEADER_UNSUPPORTED", "the image does not carry a supported Genesis header (even size 0x200..4 MiB, ROM start 0)");
      if (ssf_mapper(original)) fail("E_MAPPER_UNSUPPORTED", "the image uses a bank-switched cartridge mapper; its header cannot be repaired");
      set_be32(out.image, 0x1A4, static_cast<std::uint32_t>(out.image.size() - 1));
      const std::uint16_t sum = genesis_checksum(out.image, static_cast<std::uint32_t>(out.image.size()));
      out.image[0x18E] = static_cast<std::uint8_t>(sum >> 8U);
      out.image[0x18F] = static_cast<std::uint8_t>(sum);
      out.header_repaired = true;
    } else if (m.header_repair.empty() && changed && original_ok) {
      const std::uint16_t sum = genesis_checksum(out.image, static_cast<std::uint32_t>(out.image.size()));
      out.image[0x18E] = static_cast<std::uint8_t>(sum >> 8U);
      out.image[0x18F] = static_cast<std::uint8_t>(sum);
      out.header_repaired = true;
    } else if (m.header_repair == "none" && changed && original_ok && !genesis_header_verifies(out.image)) {
      out.warnings.push_back("W_CHECKSUM_NOT_REPAIRED");
    }
    out.ok = true;
  } catch (const Fail &f) {
    out.rejection = {f.code, f.message};
    out.image.clear();
  }
  return out;
}

TargetBps build_target_bps(const Manifest &m, const std::map<std::string, std::vector<std::uint8_t>> &files, std::span<const std::uint8_t> original) {
  TargetBps out;
  out.target = build_target_image(m, files, original);
  if (!out.target.ok) { out.rejection = out.target.rejection; return out; }
  out.revision = out.target.revision;
  const patch::BpsResult built = patch::build_bps(original, out.target.image);
  if (!built.ok) { out.rejection = {"E_BPS_BUILD", built.error}; return out; }
  const patch::BpsResult back = patch::apply_bps(built.output, original);   // independent confirmation through the reader, by digest
  if (!back.ok || sha256_hex(back.output) != sha256_hex(out.target.image)) { out.rejection = {"E_BPS_BUILD", "the constructed BPS does not reproduce the verified target image"}; return out; }
  out.source_sha256 = sha256_hex(original);
  out.target_sha256 = sha256_hex(out.target.image);
  out.patch_sha256 = sha256_hex(built.output);
  out.patch = built.output;
  out.ok = true;
  return out;
}

}  // namespace segarecomp::mod
