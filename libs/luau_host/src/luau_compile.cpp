// SEG-137-T003 (ADR 0294): the trusted Luau compile step and the bytecode provenance check. See luau_host.hpp. The stored form and its reader are in luau_stored.cpp.
#include "luau_internal.hpp"

#include "luacode.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace segarecomp::luau {

namespace {
constexpr std::size_t kMaxMessage = 200;
}  // namespace

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

CompileResult verify_compiled(const mod::Package &package, std::span<const std::uint8_t> stored, std::string_view expected_digest) {
  CompileResult r = load_stored(package.identity, stored, expected_digest);
  if (!r.ok) return r;
  const auto fail = [&r](std::string why) { r.ok = false; r.rejection = mismatch(std::move(why)); r.compiled = CompiledMod{}; return std::move(r); };
  // Exactly the package's luau-source payloads, each with its verified source digest.
  std::vector<std::pair<std::string, std::string>> expected;
  for (const mod::Payload &p : package.manifest.payloads)
    if (p.kind == "luau-source") expected.emplace_back(p.path, p.sha256);
  std::sort(expected.begin(), expected.end());
  const std::vector<CompiledScript> &scripts = r.compiled.scripts();
  if (expected.size() != scripts.size()) return fail("the compiled output does not cover the package's scripts");
  for (std::size_t i = 0; i < scripts.size(); ++i)
    if (scripts[i].path != expected[i].first || scripts[i].source_sha256 != expected[i].second) return fail("the compiled output does not match the package's scripts");
  return r;
}

}  // namespace segarecomp::luau
