// SEG-137-T003 (ADR 0294): the trusted Luau compile step and the bytecode provenance check. See luau_host.hpp.
#include "segarecomp/luau_host.hpp"

#include "segarecomp/sha256.hpp"

#include "luacode.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace segarecomp::luau {

namespace {

constexpr std::string_view kMagic = "SEGLUAUC1\n";
constexpr std::size_t kMaxMessage = 200;

std::string digest_of(std::span<const std::uint8_t> bytes) { return sha256_hex(bytes); }
std::span<const std::uint8_t> as_bytes(std::string_view s) { return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}; }

bool hex64(std::string_view s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

mod::Rejection mismatch(std::string text) { return {"E_LUAU_COMPILED_MISMATCH", std::move(text)}; }

}  // namespace

std::string_view compiler_pin() { return SEGARECOMP_LUAU_PIN; }

// The only constructor path of a non-empty CompiledMod.
struct CompiledAccess {
  static std::string digest(const std::string &identity, const std::vector<CompiledScript> &scripts) {
    std::string t = "segarecomp-luau-bytecode-1\ncompiler luau ";
    t += compiler_pin();
    t += " O1 g0 no-fastcall:getmetatable,tostring,math.random,math.randomseed\nidentity " + identity + "\n";
    for (const CompiledScript &s : scripts) t += "script " + s.path + " " + s.source_sha256 + " " + s.bytecode_sha256 + "\n";
    return digest_of(as_bytes(t));
  }
  static CompiledMod make(std::string identity, std::vector<CompiledScript> scripts) {
    std::sort(scripts.begin(), scripts.end(), [](const CompiledScript &a, const CompiledScript &b) { return a.path < b.path; });
    CompiledMod m;
    m.digest_ = digest(identity, scripts);
    m.identity_ = std::move(identity);
    m.scripts_ = std::move(scripts);
    return m;
  }
};

std::string Diagnostic::text() const { return file + ":" + std::to_string(line) + ": " + message; }

SourceCompile compile_source(std::string_view path, std::string_view source) {
  SourceCompile out;
  // Builtins the host replaces must not be compiled into fastcalls, which would bypass the replacement (getmetatable is restricted; math.random is deterministic).
  static const char *const disabled_builtins[] = {"getmetatable", "tostring", "math.random", "math.randomseed", nullptr};
  lua_CompileOptions options{};
  options.disabledBuiltins = disabled_builtins;
  options.optimizationLevel = 1;
  options.debugLevel = 0;
  options.typeInfoLevel = 0;
  options.coverageLevel = 0;
  std::size_t size = 0;
  char *raw = luau_compile(source.data(), source.size(), &options, &size);
  if (raw == nullptr || size == 0) {
    std::free(raw);
    out.diagnostic = {std::string(path), 1, "the compiler produced no output"};
    return out;
  }
  if (raw[0] == 0) {   // error bytecode: 0, then ":<line>: <message>"
    std::string text(raw + 1, size - 1);
    std::free(raw);
    unsigned line = 1;
    std::string message = text;
    if (!text.empty() && text[0] == ':') {
      std::size_t i = 1;
      unsigned long v = 0;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i < 10) v = v * 10 + static_cast<unsigned long>(text[i++] - '0');
      if (i > 1 && i < text.size() && text[i] == ':') {
        line = static_cast<unsigned>(v);
        message = text.substr(i + 1);
        while (!message.empty() && message.front() == ' ') message.erase(message.begin());
      }
    }
    const std::size_t nl = message.find('\n');
    if (nl != std::string::npos) message.resize(nl);
    if (message.size() > kMaxMessage) message.resize(kMaxMessage);
    out.diagnostic = {std::string(path), line, message};
    return out;
  }
  out.bytecode.assign(reinterpret_cast<const std::uint8_t *>(raw), reinterpret_cast<const std::uint8_t *>(raw) + size);
  std::free(raw);
  out.ok = true;
  return out;
}

CompileResult compile_mod(const mod::Package &package) {
  CompileResult r;
  const auto started = std::chrono::steady_clock::now();
  std::vector<CompiledScript> scripts;
  std::size_t total = 0;
  for (const mod::Payload &p : package.manifest.payloads) {
    if (p.kind != "luau-source") continue;
    const auto it = package.files.find(p.path);
    if (it == package.files.end()) {
      r.rejection = {"E_LUAU_COMPILED_MISMATCH", "a luau-source payload has no bytes"};
      return r;
    }
    const std::string_view source(reinterpret_cast<const char *>(it->second.data()), it->second.size());
    SourceCompile c = compile_source(p.path, source);
    if (!c.ok) {
      r.rejection = {"E_LUAU_SYNTAX", c.diagnostic.text()};
      r.diagnostic = c.diagnostic;
      return r;
    }
    total += c.bytecode.size();
    if (total > kMaxBytecodeBytes) {
      r.rejection = {"E_LUAU_COMPILE_LIMIT", "the compiled bytecode of the mod exceeds the per-mod limit"};
      return r;
    }
    CompiledScript s;
    s.path = p.path;
    s.source_sha256 = p.sha256;
    s.bytecode_sha256 = digest_of(c.bytecode);
    s.bytecode = std::move(c.bytecode);
    scripts.push_back(std::move(s));
  }
  if (std::chrono::steady_clock::now() - started > std::chrono::seconds(kCompileSecondsMax)) {
    r.rejection = {"E_LUAU_COMPILE_LIMIT", "compiling the mod exceeded the per-mod time limit"};
    return r;
  }
  if (!scripts.empty()) r.compiled = CompiledAccess::make(package.identity, std::move(scripts));
  r.ok = true;
  return r;
}

std::vector<std::uint8_t> CompiledMod::serialize() const {
  std::string head(kMagic);
  head += "identity " + identity_ + "\ndigest " + digest_ + "\nscripts " + std::to_string(scripts_.size()) + "\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  for (const CompiledScript &s : scripts_) {
    const std::string line = "script " + s.path + " " + s.source_sha256 + " " + std::to_string(s.bytecode.size()) + "\n";
    out.insert(out.end(), line.begin(), line.end());
    out.insert(out.end(), s.bytecode.begin(), s.bytecode.end());
    out.push_back('\n');
  }
  return out;
}

namespace {

// Strict reader over the stored form; every failure is a mismatch, never a partial result.
struct Reader {
  std::span<const std::uint8_t> data;
  std::size_t pos = 0;
  bool line(std::string &out) {
    for (std::size_t i = pos; i < data.size() && i - pos <= 512; ++i)
      if (data[i] == '\n') {
        out.assign(reinterpret_cast<const char *>(data.data() + pos), i - pos);
        pos = i + 1;
        return true;
      }
    return false;
  }
  bool bytes(std::size_t n, std::vector<std::uint8_t> &out) {
    if (n > data.size() - pos) return false;
    out.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
    return true;
  }
};

bool take_prefix(std::string &line, std::string_view prefix) {
  if (line.compare(0, prefix.size(), prefix) != 0) return false;
  line.erase(0, prefix.size());
  return true;
}

bool parse_size(const std::string &s, std::size_t &v) {
  if (s.empty() || s.size() > 9 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
  v = static_cast<std::size_t>(std::stoul(s));
  return true;
}

}  // namespace

CompileResult verify_compiled(const mod::Package &package, std::span<const std::uint8_t> stored, std::string_view expected_digest) {
  CompileResult r;
  const auto fail = [&r](std::string why) { r.rejection = mismatch(std::move(why)); return std::move(r); };
  if (!hex64(expected_digest)) return fail("the recorded bytecode digest is malformed");
  if (stored.size() > kMaxBytecodeBytes + (64U << 10U)) return fail("the stored compiled output is too large");
  Reader in{stored};
  std::string l;
  if (stored.size() < kMagic.size() || std::memcmp(stored.data(), kMagic.data(), kMagic.size()) != 0) return fail("the stored compiled output is not a compiled mod");
  in.pos = kMagic.size();
  if (!in.line(l) || !take_prefix(l, "identity ") || l != package.identity) return fail("the compiled output belongs to another package identity");
  std::string header_digest;
  if (!in.line(header_digest) || !take_prefix(header_digest, "digest ") || !hex64(header_digest)) return fail("the stored compiled output has no valid digest");
  std::size_t count = 0;
  if (!in.line(l) || !take_prefix(l, "scripts ") || !parse_size(l, count) || count == 0 || count > mod::kMaxLuauFiles) return fail("the stored compiled output has a bad script count");
  std::vector<CompiledScript> scripts;
  std::size_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    CompiledScript s;
    std::size_t n = 0;
    if (!in.line(l) || !take_prefix(l, "script ")) return fail("a stored script entry is malformed");
    const std::size_t a = l.find(' '), b = l.rfind(' ');
    if (a == std::string::npos || b == a || !parse_size(l.substr(b + 1), n)) return fail("a stored script entry is malformed");
    s.path = l.substr(0, a);
    s.source_sha256 = l.substr(a + 1, b - a - 1);
    total += n;
    if (total > kMaxBytecodeBytes || !in.bytes(n, s.bytecode) || in.pos >= stored.size() || stored[in.pos] != '\n') return fail("a stored script entry is truncated or too large");
    ++in.pos;
    if (s.bytecode.empty() || s.bytecode[0] == 0) return fail("a stored script is not compiled bytecode");
    s.bytecode_sha256 = digest_of(s.bytecode);
    scripts.push_back(std::move(s));
  }
  if (in.pos != stored.size()) return fail("the stored compiled output has trailing bytes");
  // Exactly the package's luau-source payloads, each with its verified source digest.
  std::vector<std::pair<std::string, std::string>> expected;
  for (const mod::Payload &p : package.manifest.payloads)
    if (p.kind == "luau-source") expected.emplace_back(p.path, p.sha256);
  std::sort(expected.begin(), expected.end());
  if (expected.size() != scripts.size()) return fail("the compiled output does not cover the package's scripts");
  for (std::size_t i = 0; i < scripts.size(); ++i)
    if (scripts[i].path != expected[i].first || scripts[i].source_sha256 != expected[i].second) return fail("the compiled output does not match the package's scripts");
  CompiledMod rebuilt = CompiledAccess::make(package.identity, std::move(scripts));
  if (rebuilt.digest() != header_digest || rebuilt.digest() != expected_digest) return fail("the compiled output was modified after build");
  r.compiled = std::move(rebuilt);
  r.ok = true;
  return r;
}

}  // namespace segarecomp::luau
