#include "older_build.hpp"

#include "json_lite.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace launcher {

namespace {

bool read_small(const fs::path &path, std::string &out, std::uintmax_t limit = 65536U) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > limit) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return true;
}

bool is_build_key(const std::string &name) {
  return name.size() == 32U && std::all_of(name.begin(), name.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Is an SDL3 shared library installed where the launcher tells programs to look for it?
bool sdl3_runtime_present(const Layout &layout) {
  std::error_code ec;
  if (!fs::is_directory(layout.sdl_lib, ec)) return false;
  for (fs::directory_iterator it(layout.sdl_lib, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind("libSDL3", 0) == 0 || name.rfind("SDL3", 0) == 0) return true;
  }
  return false;
}

// Empty when the candidate is playable; otherwise the plain-language reason it is not.
std::string evaluate(const LibraryEntry &entry, const std::string &requested_policy, const Layout &layout, const fs::path &games_root, const BuildRecord &record,
                     OlderBuild &out) {
  if (!is_build_key(record.key)) return "its build record is not a valid build identity";
  const fs::path dir = games_root / record.key;
  std::error_code ec;
  const auto status = fs::symlink_status(dir, ec);
  if (ec || status.type() != fs::file_type::directory) return "its build folder is gone or is not a plain folder";
  if (!entry_ready(dir)) return "its executable or successful-build record is missing";

  std::string meta_text;
  if (!read_small(dir / "metadata.json", meta_text)) return "its build metadata is missing";
  const auto meta = json::parse(meta_text);
  if (!meta || !meta->is_object()) return "its build metadata is unreadable";
  if (meta->str("rom_sha256") != entry.sha256) return "its build metadata names a different ROM";
  if (meta->str("platform", "genesis") != entry.platform_id) return "it was built for a different platform";
  if (meta->str("host") != host_description()) return "it was built for a different operating system or processor (" + meta->str("host", "unknown") + ")";
  if (entry.platform_id == "master-system") {
    if (meta->str("profile") != sms_profile) return "it was built for a different Master System profile";
    if (meta->str("mapper_declaration") != "option" || entry.mapper.empty() || meta->str("mapper") != entry.mapper)
      return "its cartridge mapper declaration cannot be matched to this game's";
  }
  // The one build option the metadata may not record in older builds: a build made with H-interrupt discovery changes the generated program.
  const bool want_hint = entry.hint_alias_discovery && entry.platform_id == "genesis";
  if (meta->get("hint_alias_discovery") != nullptr) {
    if (meta->flag("hint_alias_discovery") != want_hint) return "its H-interrupt discovery setting differs from this game's";
  } else if (want_hint) {
    return "it does not record its H-interrupt discovery setting, which this game turns on";
  }

  // Recorded policy: complete, and the same mode that is requested now.
  std::string status_text;
  if (!read_small(dir / "status.json", status_text, 16U << 20U)) return "its build status is unreadable";
  const PolicyReport report = read_policy_report(dir);
  const bool has_member = status_text.find("\"aot_policy\":{") != std::string::npos;
  if (has_member && !report.present) return "its recorded policy is incomplete";
  const std::string actual_request = report.present ? report.requested : "compatibility";
  if (actual_request != "compatibility" && actual_request != "optimized") return "its recorded policy is not recognised";
  if (actual_request == "optimized") {
    if (report.policy_identity.empty() || report.validator.empty()) return "its Optimized policy record is incomplete, so it cannot be verified";
    if (entry.platform_id == "genesis" && report.requested_model != "v1" && report.requested_model != "gen2") return "its Optimized model is not recorded";
  }
  if (actual_request != requested_policy) return std::string("it was built in the ") + (actual_request == "optimized" ? "Optimized" : "Compatibility") + " mode, not the one requested now";

  // Host interface.
  const json::Value *marker = meta->get("runtime_contract");
  const std::string version = meta->str("segarecomp_version");
  bool marked = false;
  if (marker != nullptr) {
    if (meta->num("runtime_contract", -1) != runtime_contract) return "it was built for a different runtime contract than this release provides";
    marked = true;
  } else {
    const auto &legacy = legacy_contract_releases();
    if (std::find(legacy.begin(), legacy.end(), version) == legacy.end())
      return version.empty() ? "it does not record which release built it" : "it was built by release " + version + ", which this release cannot vouch for";
  }
  if (!sdl3_runtime_present(layout)) return "the SDL3 runtime library of this installation is missing";

  out.playable = true;
  out.dir = dir;
  out.key = record.key;
  out.version = version;
  out.policy = report;
  out.mode = report.present ? effective_mode(report) : "compatibility";
  out.model = entry.platform_id == "genesis" && report.present && actual_request == "optimized" ? report.requested_model : std::string();
  out.contract_marked = marked;
  return {};
}

} // namespace

const std::vector<std::string> &legacy_contract_releases() {
  static const std::vector<std::string> releases = {"0.5.0"};
  return releases;
}

OlderBuild find_playable_older_build(const LibraryEntry &entry, const std::string &current_key, const std::string &requested_policy, const Layout &layout,
                                     const fs::path &games_root) {
  OlderBuild first_refusal;
  for (auto it = entry.builds.rbegin(); it != entry.builds.rend(); ++it) {  // history is oldest first
    if (it->key == current_key) continue;
    OlderBuild candidate;
    const std::string refusal = evaluate(entry, requested_policy, layout, games_root, *it, candidate);
    if (refusal.empty()) {
      candidate.candidate_seen = true;
      return candidate;
    }
    if (!first_refusal.candidate_seen) {
      first_refusal.candidate_seen = true;
      first_refusal.reason = refusal;
      first_refusal.key = it->key;
    }
  }
  return first_refusal;
}

} // namespace launcher
