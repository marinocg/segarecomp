#pragma once

// The trusted Luau compile step of `segarecomp mod build/variant/session` and `segarecomp build --luau-package` (SEG-137-T003/T007, ADR 0294): runs the bounded child `segarecomp-luau-compile`
// next to this executable (5 s kill, metered 256 MiB) and re-verifies its output with verify_compiled before it is used.
#include "segarecomp/luau_host.hpp"

#include <filesystem>
#include <span>
#include <string>

namespace segarecomp_cli {

struct LuauBuild {
  bool ok = true;
  segarecomp::mod::Rejection rejection;
  segarecomp::luau::CompiledMod compiled;
  std::string json;   // `,"luau":{...}` or ""
};

void set_self_path(const std::string &argv0);
[[nodiscard]] std::filesystem::path self_executable();   // this executable (beside argv[0], else found on PATH), for commands that run `segarecomp build` as a child   // where to find the helper: beside argv[0], else on PATH
[[nodiscard]] LuauBuild build_luau(const segarecomp::mod::Package &package, std::span<const std::uint8_t> package_bytes);

}  // namespace segarecomp_cli
