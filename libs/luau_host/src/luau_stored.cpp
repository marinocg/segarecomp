// SEG-137-T003/T007 (ADR 0294/0296): the stored form of compiled Luau and its strict reader. This file needs neither the compiler nor the package reader, so the generated-native game program links it.
#include "luau_internal.hpp"

#include <cstring>

namespace segarecomp::luau {

std::string_view compiler_pin() { return SEGARECOMP_LUAU_PIN; }

std::string Diagnostic::text() const { return file + ":" + std::to_string(line) + ": " + message; }

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

std::string stored_digest(std::span<const std::uint8_t> stored) {
  Reader in{stored};
  std::string l, digest;
  if (stored.size() < kMagic.size() || std::memcmp(stored.data(), kMagic.data(), kMagic.size()) != 0) return {};
  in.pos = kMagic.size();
  if (!in.line(l) || !in.line(digest) || !take_prefix(digest, "digest ") || !hex64(digest)) return {};
  return digest;
}


// The runtime-side load: accepts stored compiled bytes ONLY if they are well formed, name `expected_identity` and hash to `expected_digest` (the value recorded when the build compiled and verified them).
CompileResult load_stored(std::string_view expected_identity, std::span<const std::uint8_t> stored, std::string_view expected_digest) {
  CompileResult r;
  const auto fail = [&r](std::string why) { r.rejection = mismatch(std::move(why)); return std::move(r); };
  if (!hex64(expected_digest)) return fail("the recorded bytecode digest is malformed");
  if (stored.size() > kMaxBytecodeBytes + (64U << 10U)) return fail("the stored compiled output is too large");
  Reader in{stored};
  std::string l;
  if (stored.size() < kMagic.size() || std::memcmp(stored.data(), kMagic.data(), kMagic.size()) != 0) return fail("the stored compiled output is not a compiled mod");
  in.pos = kMagic.size();
  if (!in.line(l) || !take_prefix(l, "identity ") || l != expected_identity) return fail("the compiled output belongs to another package identity");
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
  CompiledMod rebuilt = CompiledAccess::make(std::string(expected_identity), std::move(scripts));
  if (rebuilt.digest() != header_digest || rebuilt.digest() != expected_digest) return fail("the compiled output was modified after build");
  r.compiled = std::move(rebuilt);
  r.ok = true;
  return r;
}

}  // namespace segarecomp::luau
