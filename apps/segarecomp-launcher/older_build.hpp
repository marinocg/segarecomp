#pragma once

// SEG-069-T005: playing an executable that an EARLIER release built. The cache identity (`entry_dir()`) decides whether generated output may be
// REUSED for a build request and stays exact. Whether a finished executable can still be RUN is a different, narrower question answered here, and it
// fails closed: every fact below must be established from the build's own records, never from a version number alone.
//
//   * the directory is a real child of the cache `games/` folder named like a build key, with an executable and a successful status.json;
//   * metadata.json ties it to THIS library ROM (SHA-256) and platform, to this host (OS + CPU), and for Master System to the declared mapper/profile;
//   * its recorded policy member is complete, and it was built for the SAME mode (Compatibility / Optimized) the game currently requests (the model
//     family may differ: it is reported as it is, never relabelled);
//   * the host interface of the generated program is the one this launcher provides: `runtime_contract` (see below) and an installed SDL3 library.
//
// `runtime_contract` is bumped only when a change would stop a previously built program from running under the launcher (its environment/arguments, the
// SDL3 major version, the directory layout it relies on). New builds record it in metadata.json. Builds made before the marker existed are accepted
// only from the explicitly listed releases whose interface is the current one (`legacy_contract_releases`), never by "older than this release".

#include "launcher_core.hpp"
#include "library_catalog.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

// Releases that produced programs without the marker but with exactly contract 1 (the SDL3 launcher + shared viewer of 0.5.0).
[[nodiscard]] const std::vector<std::string> &legacy_contract_releases();

struct OlderBuild {
  bool playable = false;
  std::filesystem::path dir;     // the executable's build folder when playable
  std::string key;
  std::string version;           // segarecomp_version recorded by the build ("" when unknown)
  PolicyReport policy;           // the build's own recorded policy (what it ACTUALLY is)
  std::string mode;              // "optimized" | "compatibility": the program that actually runs
  std::string model;             // Genesis Optimized model family recorded by the build ("v1"/"gen2"), else empty
  bool contract_marked = false;  // the build recorded runtime_contract (else it is a listed legacy release)
  bool candidate_seen = false;   // at least one recorded older build exists
  std::string reason;            // when not playable: why the best (newest) candidate is refused
};

// Classifies the catalog-recorded builds of `entry` other than `current_key`, newest first, and returns the first that can be played (or the refusal
// of the newest candidate). `requested_policy` is the mode the game currently requests ("optimized"/"compatibility"). Reads only small files.
[[nodiscard]] OlderBuild find_playable_older_build(const LibraryEntry &entry, const std::string &current_key, const std::string &requested_policy,
                                                   const Layout &layout, const std::filesystem::path &games_root);

} // namespace launcher
