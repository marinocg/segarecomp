// Internal to libs/luau_host: the stored-form constants and the one constructor path of a CompiledMod.
#pragma once

#include "segarecomp/luau_host.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>

namespace segarecomp::luau {

inline constexpr std::string_view kMagic = "SEGLUAUC1\n";

inline std::string digest_of(std::span<const std::uint8_t> bytes) { return sha256_hex(bytes); }
inline std::span<const std::uint8_t> as_bytes(std::string_view s) { return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}; }

inline bool hex64(std::string_view s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

inline mod::Rejection mismatch(std::string text) { return {"E_LUAU_COMPILED_MISMATCH", std::move(text)}; }


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


}  // namespace segarecomp::luau
