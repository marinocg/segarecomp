#pragma once

// SEG-141-T003/T006/T007 (ADR 0327): the ONE preparation of a program that runs an ordered mod set, for every console that composes. It verifies the pinned packages, the declared relations, the verified
// composition (F19 engine), what a built set supports, compiles the scripts with the trusted compiler, derives the composite identity (the same key as `mod set variant`), merges the function-hook sites and
// writes the member table the program links. Everything it does not support is refused here with a stable class; nothing is dropped.
#include "luau_build.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/mod_set.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace segarecomp_cli {

struct SetHookBinding {
  std::string id;                 // generated site id
  std::uint32_t hook_id = 0, modes = 0, reads = 0;
};

// One mod that RUNS (a data-only member only shapes the composed image).
struct SetRuntimeMember {
  std::size_t index = 0;          // position in the set
  bool native = false;
  std::vector<std::string> granted;
  std::vector<segarecomp::mod::MemoryRange> ranges;
  LuauBuild luau;
  std::vector<SetHookBinding> hooks;
  std::uint32_t hook_phases = 0;
  std::vector<std::pair<std::filesystem::path, std::string>> units;   // staged native-c11 files (path, sha256), after stage_set_sources
  std::filesystem::path source_dir;
};

struct SetBuild {
  bool ok = false;
  std::string code, message;
  std::vector<segarecomp::mod::SetViolation> violations;
  std::string platform, mapper, original_sha256, effective_sha256, identity;   // `identity`: the composite variant key
  std::vector<std::uint8_t> image;                                            // the composed image the program is compiled from
  std::vector<segarecomp::mod::Package> packages;                             // every member, in set order
  std::vector<SetRuntimeMember> runtime;
  int drawing = -1;               // index into `runtime` of the one mod that declares presentation output
  std::string hook_plan;          // the emitter's merged function-hook plan; empty = no hooks
};

// `packages[i]` must be the package of exactly `pinned[i]` (the identity the user selected and, for native code, approved).
[[nodiscard]] SetBuild prepare_mod_set(const std::vector<std::filesystem::path> &packages, const std::vector<std::string> &pinned, const std::vector<std::uint8_t> &original);
// Stages every native member's native-c11 files under <dir>/m<i>/src. False = a source could not be written.
[[nodiscard]] bool stage_set_sources(SetBuild &set, const std::filesystem::path &dir);
// The generated member table of the console's program (platforms/<console> declares the row type).
[[nodiscard]] std::string set_artifact_source(const SetBuild &set);
// `,"mod_set":{...}` for status.json.
[[nodiscard]] std::string set_status_json(const SetBuild &set);

}  // namespace segarecomp_cli
