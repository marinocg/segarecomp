// The Sega CD disc commands of the modding platform (SEG-148-T002/T003, ADR 0328). Orchestration only: the verified edits, layout rules and code eligibility are libs/media (mod_disc), the build is
// the product's own `sega-cd build` run as a child process. One JSON object on stdout. Originals are only read; every derived disc is a local artifact inside the caller's output folder.
//
// `disc-build` never trusts an evidence file from the author: it builds the ORIGINAL disc itself, judges every planned write against what that build proved, materializes the patched disc, builds
// it under the variant identity and judges the writes again against the Patched build's own evidence.
#include "mod_disc_command.hpp"

#include "mod_set_build.hpp"
#include "patch_archive.hpp"
#include "process_run.hpp"
#include "segarecomp/machine/sega_cd/boot_contract.hpp"
#include "segarecomp/mod_disc.hpp"
#include "segarecomp/mod_set.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp_cli {

namespace {

namespace fs = std::filesystem;
namespace scd = segarecomp::machine::sega_cd;
using namespace segarecomp;

std::string esc(const std::string &s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
    else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) o.push_back(c);
    else o.push_back('?');
  }
  return o;
}

int refuse(const mod::Rejection &r) {
  std::cout << "{\"ok\":false,\"class\":\"" << esc(r.code) << "\",\"error\":\"" << esc(r.message) << "\"}\n";
  return 1;
}

int usage() {
  std::cerr << "usage:\n" << kModDiscUsage;
  return 2;
}

std::string digest_of(const std::string &text) { return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}); }

bool read_text(const fs::path &path, std::uintmax_t limit, std::string &out) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > limit || ec) return false;
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return static_cast<bool>(in);
}

std::optional<mod::Rejection> load_disc_package(const fs::path &target, mod::Package &p) {
  std::error_code ec;
  if (fs::is_directory(target, ec)) {
    const mod::Checked c = mod::check_project(target);
    if (!c.ok) return c.rejection;
    p = c.reread;
  } else {
    p = mod::read_package_file(target);
    if (!p.ok) return p.rejection;
  }
  if (p.manifest.platform != "sega-cd") return mod::Rejection{"E_SOURCE_PLATFORM", "the disc commands take a sega-cd package"};
  if (const auto r = mod::check_standalone(p.manifest)) return r;   // ADR 0326: a required other mod is never silently absent
  return std::nullopt;
}

// The declared BIOS requirement against the user's BIOS (never packaged): an exact digest, or a runtime BIOS profile that the BIOS image matches.
std::optional<mod::Rejection> check_bios(const mod::Manifest &m, const fs::path &path, const DiscImage &disc, std::string &sha_out) {
  std::string bytes;
  if (!read_text(path, scd::kBiosWindowBytes, bytes)) return mod::Rejection{"E_DISC_BIOS", "the BIOS image cannot be read"};
  const std::span<const std::uint8_t> view(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
  const scd::BiosAdmission admitted = scd::admit_bios(view);
  if (!admitted.bios) return mod::Rejection{"E_DISC_BIOS", std::string("the BIOS image is not usable: ") + scd::bios_error_name(admitted.error)};
  sha_out = admitted.bios->sha256;
  if (m.bios_requirement == "digest") {
    if (std::find(m.bios_sha256s.begin(), m.bios_sha256s.end(), sha_out) == m.bios_sha256s.end()) return mod::Rejection{"E_DISC_BIOS", "the BIOS image is not one of the digests the mod requires"};
    return std::nullopt;
  }
  const scd::BootContract contract = scd::derive_boot_contract(*admitted.bios, disc, scd::BootProfileScope::authentic);
  if (contract.profile != m.bios_class) return mod::Rejection{"E_DISC_BIOS", "the BIOS image does not match the BIOS profile \"" + m.bios_class + "\" the mod requires"};
  return std::nullopt;
}

// `path` (existing or not) is `folder` or lies inside it, links resolved.
bool inside_folder(const fs::path &path, const fs::path &folder) {
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(fs::absolute(path, ec), ec);
  if (ec) return true;
  for (fs::path at = resolved; !at.empty(); at = at.parent_path()) {
    std::error_code same;
    if (fs::equivalent(at, folder, same) && !same) return true;
    if (at == at.parent_path()) break;
  }
  return false;
}

struct Opened {
  mod::Package package;
  DiscImage original;
  mod::DiscPlan plan;
};

// Every image-level assertion of the package against the original disc, before any output exists.
std::optional<mod::Rejection> open_and_plan(const fs::path &target, const fs::path &cue, Opened &out) {
  if (const auto r = load_disc_package(target, out.package)) return r;
  mod::DiscOpened disc = mod::open_disc(cue);
  if (!disc.disc) return disc.rejection;
  out.original = std::move(*disc.disc);
  out.plan = mod::plan_disc_patch(out.package.manifest, out.package.files, out.original);
  if (!out.plan.ok) return out.plan.rejection;
  return std::nullopt;
}

std::string plan_json(const Opened &o) {
  return "\"original_disc_identity\":\"" + o.original.identity() + "\",\"edits_applied\":" + std::to_string(o.plan.edits_applied) + ",\"blocks_injected\":" + std::to_string(o.plan.blocks_injected) +
         ",\"bytes_written\":" + std::to_string(o.plan.bytes_written);
}

struct Options {
  fs::path target, cue, bios, out, patched, runtime_dir;
  std::string cc, budget, timeout;
  std::vector<std::pair<std::string, std::string>> build;   // forwarded to `sega-cd build`
};

// The `sega-cd build` options a disc build forwards: host tools and the machine's inputs. Deliberately absent: `--emit-arg` (hand-admitted extents are never evidence), every option that turns a
// proof source off (`--static-boot`, `--static-discovery`, which would weaken the refusals), and the test-only controls (`--boot-fixture-profiles`, `--pass-set`, `--viewer-host-source`).
constexpr std::string_view kForwarded[] = {"--jobs", "--cache-dir", "--cc-arg", "--cxx", "--cxx-arg", "--link-arg", "--pass-timeout", "--optimize", "--viewer", "--sdl3-include", "--sdl3-lib",
                                           "--input-script", "--backup-ram", "--until-tick"};

bool parse(int argc, char **argv, Options &o, const std::string &sub) {
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--", 0) != 0) {
      if (!o.target.empty()) return false;
      o.target = a;
      continue;
    }
    if (a == "--json") continue;
    if (i + 1 >= argc) return false;
    const std::string v = argv[++i];
    if (a == "--cue" && o.cue.empty()) o.cue = v;
    else if (a == "--bios" && o.bios.empty()) o.bios = v;
    else if (a == "--out" && (sub == "apply" || sub == "disc-build") && o.out.empty()) o.out = v;
    else if (a == "--patched" && sub == "verify-disc" && o.patched.empty()) o.patched = v;
    else if (a == "--runtime-dir" && sub == "disc-build" && o.runtime_dir.empty()) o.runtime_dir = v;
    else if (a == "--cc" && sub == "disc-build" && o.cc.empty()) o.cc = v;
    else if (a == "--budget" && (sub == "disc-build" || sub == "disc-diff-run") && o.budget.empty() && !v.empty() && v.size() <= 12 && v.find_first_not_of("0123456789") == std::string::npos && v.find_first_not_of('0') != std::string::npos) o.budget = v;
    else if (a == "--timeout" && sub == "disc-diff-run" && o.timeout.empty() && !v.empty() && v.size() <= 6 && v.find_first_not_of("0123456789") == std::string::npos && v.find_first_not_of('0') != std::string::npos) o.timeout = v;
    else if (sub == "disc-build" && std::find(std::begin(kForwarded), std::end(kForwarded), a) != std::end(kForwarded)) o.build.emplace_back(a, v);
    else return false;
  }
  return !o.target.empty();
}

int apply(const Options &o) {
  if (o.cue.empty() || o.out.empty()) return usage();
  Opened opened;
  if (const auto r = open_and_plan(o.target, o.cue, opened)) return refuse(*r);
  if (opened.plan.has_code())
    return refuse({"E_DISC_CODE_EVIDENCE", "the package patches code; code is patched only by `mod disc-build`, which first proves where the bytes of the original disc run"});
  if (!o.bios.empty()) {
    std::string sha;
    if (const auto r = check_bios(opened.package.manifest, o.bios, opened.original, sha)) return refuse(*r);
  }
  const mod::DiscMaterialized made = mod::materialize_disc(opened.plan, opened.original, o.cue, o.out);
  if (!made.ok) return refuse(made.rejection);
  // `code_checked` is false by design: without a build there is no evidence about which bytes are code; a data edit is a data edit as declared.
  std::cout << "{\"ok\":true," << plan_json(opened) << ",\"effective_disc_identity\":\"" << made.effective_identity << "\",\"cue\":\"" << esc(made.cue.string()) << "\",\"code_checked\":false}\n";
  return 0;
}

int verify(const Options &o) {
  if (o.cue.empty() || o.patched.empty()) return usage();
  Opened opened;
  if (const auto r = open_and_plan(o.target, o.cue, opened)) return refuse(*r);
  const mod::DiscOpened patched = mod::open_disc(o.patched);
  if (!patched.disc) return refuse(patched.rejection);
  if (const auto r = mod::verify_patched_disc(opened.plan, opened.original, *patched.disc)) return refuse(*r);
  std::cout << "{\"ok\":true," << plan_json(opened) << ",\"effective_disc_identity\":\"" << patched.disc->identity() << "\"}\n";
  return 0;
}

// The `disc_sources` object of a successful build's status.json (written by sega_cd_build.cpp), read with the strict closed-JSON reader.
std::optional<mod::DiscEvidence> build_evidence(const fs::path &build_dir, std::string &program) {
  std::string status;
  if (!read_text(build_dir / "status.json", 16U << 20U, status) || status.rfind("{\"status\":\"ok\",\"program\":\"", 0) != 0) return std::nullopt;
  const std::size_t name = std::string_view("{\"status\":\"ok\",\"program\":\"").size();
  program = status.substr(name, status.find('"', name) - name);
  const std::string key = "\"disc_sources\":";
  const std::size_t at = status.find(key);
  if (at == std::string::npos) return std::nullopt;
  std::size_t end = at + key.size();
  int depth = 0;
  bool quoted = false;
  for (; end < status.size(); ++end) {
    const char c = status[end];
    if (c == '"') quoted = !quoted;
    else if (!quoted && c == '{') ++depth;
    else if (!quoted && c == '}' && --depth == 0) break;
  }
  if (end >= status.size()) return std::nullopt;
  patch::detail::JsonLimits limits;
  limits.max_items = 4096; limits.max_members = 8; limits.max_depth = 4; limits.max_number = 0xFFFFFFFFU; limits.allow_whitespace = false;
  patch::detail::Value root;
  std::string error;
  if (!patch::detail::parse_json(std::string_view(status).substr(at + key.size(), end + 1 - at - key.size()), limits, root, error)) return std::nullopt;
  const auto *extents = root.find("extents"), *dynamic = root.find("dynamic");
  if (extents == nullptr || dynamic == nullptr) return std::nullopt;
  mod::DiscEvidence out;
  const auto text = [](const patch::detail::Value &v, const char *k) { const auto *f = v.find(k); return f == nullptr ? std::string() : f->text; };
  const auto number = [](const patch::detail::Value &v, const char *k) { const auto *f = v.find(k); return f == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(f->number); };
  for (const auto &v : extents->items)
    out.proofs.push_back({text(v, "cpu"), text(v, "origin"), number(v, "offset"), number(v, "length"), static_cast<std::uint32_t>(number(v, "base"))});
  for (const auto &v : dynamic->items)
    out.dynamic.push_back({text(v, "cpu"), text(v, "kind"), static_cast<std::uint32_t>(number(v, "base")), number(v, "length")});
  return out;
}

int run_build(const Options &o, const fs::path &cue, const fs::path &output, const fs::path &cache, const std::string &variant, const fs::path &log) {
  std::vector<std::string> argv{self_executable().string(), "sega-cd", "build", "--cue", cue.string(), "--bios", o.bios.string(), "--output", output.string(),
                                "--runtime-dir", o.runtime_dir.string(), "--cc", o.cc, "--budget", o.budget};
  if (std::none_of(o.build.begin(), o.build.end(), [](const auto &e) { return e.first == "--cache-dir"; })) argv.insert(argv.end(), {"--cache-dir", cache.string()});
  for (const auto &e : o.build) argv.insert(argv.end(), {e.first, e.second});
  if (!variant.empty()) argv.insert(argv.end(), {"--variant-identity", variant});
  return run_process(argv, log);
}

int disc_build(Options o) {
  if (o.cue.empty() || o.bios.empty() || o.out.empty() || o.runtime_dir.empty() || o.cc.empty() || o.budget.empty()) return usage();
  Opened opened;
  if (const auto r = open_and_plan(o.target, o.cue, opened)) return refuse(*r);
  std::string bios_sha;
  if (const auto r = check_bios(opened.package.manifest, o.bios, opened.original, bios_sha)) return refuse(*r);
  std::error_code ec;
  // Judged and USED in one resolved form: `new/..` collapses when the check resolves a path that does not exist yet, but creating the unresolved spelling would still create `new`.
  const auto resolved = [](const fs::path &path) {
    std::error_code failed;
    const fs::path absolute = fs::absolute(path, failed);
    const fs::path full = fs::weakly_canonical(absolute, failed);
    return failed ? fs::path() : full;
  };
  const fs::path out = resolved(o.out);
  if (out.empty()) return refuse({"E_DISC_OUTPUT", "the build folder path cannot be resolved"});
  for (auto &e : o.build)
    if (e.first == "--cache-dir") {
      const fs::path cache_dir = resolved(e.second);
      if (cache_dir.empty()) return refuse({"E_DISC_OUTPUT", "the cache folder path cannot be resolved"});
      e.second = cache_dir.string();
    }
  const fs::path source = fs::absolute(o.cue, ec).parent_path();
  // Every folder this command writes is judged BEFORE anything is created: nothing may appear in the original disc's folder.
  if (inside_folder(out, source)) return refuse({"E_DISC_OUTPUT", "the build folder is the original disc's folder or lies inside it; originals are never written"});
  for (const auto &e : o.build)
    if (e.first == "--cache-dir" && inside_folder(e.second, source)) return refuse({"E_DISC_OUTPUT", "the cache folder is the original disc's folder or lies inside it; originals are never written"});
  if (fs::exists(fs::symlink_status(out, ec)) && (!fs::is_directory(fs::symlink_status(out, ec)) || !fs::is_empty(out, ec)))
    return refuse({"E_DISC_OUTPUT", "the build folder exists and is not empty"});
  fs::create_directories(out, ec);
  if (!fs::is_directory(out, ec)) return refuse({"E_DISC_OUTPUT", "the build folder cannot be created"});
  const fs::path vanilla = out / "vanilla", modded = out / "modded", disc_dir = out / "disc", cache = out / "objects";

  if (run_build(o, o.cue, vanilla, cache, {}, out / "vanilla.log") != 0) return refuse({"E_DISC_BUILD", "the build of the original disc failed (vanilla.log)"});
  std::string vanilla_program, modded_program;
  const auto evidence = build_evidence(vanilla, vanilla_program);
  if (!evidence) return refuse({"E_DISC_BUILD", "the build of the original disc reports no disc-source evidence"});
  if (const auto r = mod::check_disc_eligibility(opened.plan, opened.original, *evidence)) return refuse(*r);

  const mod::DiscMaterialized made = mod::materialize_disc(opened.plan, opened.original, o.cue, disc_dir);
  if (!made.ok) return refuse(made.rejection);
  const auto discard = [&](const mod::Rejection &r) {   // a refused build keeps no derived disc and no program built from it
    std::error_code removed;
    fs::remove_all(disc_dir, removed);
    fs::remove_all(modded, removed);
    return refuse(r);
  };
  std::string payloads;
  for (const mod::Payload &p : opened.package.manifest.payloads) payloads += p.path + ":" + p.sha256 + ",";
  const std::vector<mod::VariantComponent> components = {{"original_disc", opened.original.identity()},
                                                         {"effective_disc", made.effective_identity},
                                                         {"bios", bios_sha},
                                                         {"package_identity", opened.package.identity},
                                                         {"content_digest", opened.package.content_digest},
                                                         {"payloads", digest_of(payloads)}};
  const std::string key = digest_of(mod::serialize_components(components));
  if (run_build(o, made.cue, modded, cache, key, out / "modded.log") != 0) return discard({"E_DISC_BUILD", "the build of the patched disc failed (modded.log)"});
  const auto after = build_evidence(modded, modded_program);
  if (!after) return discard({"E_DISC_BUILD", "the build of the patched disc reports no disc-source evidence"});
  const mod::DiscOpened patched = mod::open_disc(made.cue);
  if (!patched.disc || patched.disc->identity() != made.effective_identity) return discard({"E_DISC_IDENTITY", "the patched disc changed during its build"});
  if (const auto r = mod::check_disc_eligibility(opened.plan, *patched.disc, *after)) return discard({r->code, "in the build of the patched disc: " + r->message});

  std::ostringstream record;
  record << "{\"ok\":true," << plan_json(opened) << ",\"effective_disc_identity\":\"" << made.effective_identity << "\",\"code_patched\":" << (opened.plan.has_code() ? "true" : "false")
         << ",\"bios_sha256\":\"" << bios_sha << "\",\"package_identity\":\"" << opened.package.identity << "\",\"variant_key\":\"" << key << "\",\"vanilla_program\":\"vanilla/" << esc(vanilla_program)
         << "\",\"modded_program\":\"modded/" << esc(modded_program) << "\",\"patched_cue\":\"disc/" << esc(made.cue.filename().string()) << "\"}\n";
  std::ofstream file(out / "disc-mod.json", std::ios::binary | std::ios::trunc);
  file << record.str();
  if (!file.flush()) return discard({"E_DISC_OUTPUT", "the build record cannot be written"});
  std::cout << record.str();
  return 0;
}

// The members of one JSON object as raw value text, in order (the runtime's own report; no value is interpreted). Empty on anything else.
std::vector<std::pair<std::string, std::string>> members(const std::string &text) {
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t i = text.find('{');
  if (i == std::string::npos) return {};
  ++i;
  const auto space = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i; };
  for (;;) {
    space();
    if (i < text.size() && text[i] == '}') return out;
    if (i >= text.size() || text[i] != '"') return {};
    const std::size_t key_end = text.find('"', i + 1);
    if (key_end == std::string::npos) return {};
    const std::string key = text.substr(i + 1, key_end - i - 1);
    i = key_end + 1;
    space();
    if (i >= text.size() || text[i++] != ':') return {};
    space();
    const std::size_t begin = i;
    int depth = 0;
    bool quoted = false;
    for (; i < text.size(); ++i) {
      const char c = text[i];
      if (quoted) {
        if (c == '\\') ++i;
        else if (c == '"') quoted = false;
        continue;
      }
      if (c == '"') quoted = true;
      else if (c == '{' || c == '[') ++depth;
      else if ((c == '}' || c == ']') && depth > 0) --depth;
      else if (depth == 0 && (c == ',' || c == '}')) break;
    }
    if (i >= text.size()) return {};
    std::size_t end = i;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    out.emplace_back(key, text.substr(begin, end - begin));
    if (text[i] == ',') ++i;
  }
}

// Runs the Vanilla and the Patched program of a disc build with the same explicit instruction budget and timeout, each on its own disc, and reports which report fields differ.
int disc_diff(const Options &o) {
  if (o.cue.empty() || o.bios.empty() || o.budget.empty() || o.timeout.empty()) return usage();
  std::error_code ec;
  const fs::path dir = fs::absolute(o.target, ec);
  std::string record;
  if (!read_text(dir / "disc-mod.json", 1U << 16U, record) || record.rfind("{\"ok\":true,", 0) != 0) return refuse({"E_DISC_BUILD", "the folder does not hold a successful disc build"});
  std::map<std::string, std::string> fields;
  for (const auto &m : members(record)) fields[m.first] = m.second.size() > 1 && m.second.front() == '"' ? m.second.substr(1, m.second.size() - 2) : m.second;
  // The record is a file in a folder: the only programs it may name are the two this command's build writes, and the patched CUE is a plain file name under `disc/`.
  for (const char *which : {"vanilla", "modded"}) {
    const std::string &program = fields[std::string(which) + "_program"];
    if (program != std::string(which) + "/program" && program != std::string(which) + "/program.exe") return refuse({"E_DISC_BUILD", "the build record names a program this command does not build"});
  }
  const std::string &cue_field = fields["patched_cue"];
  if (cue_field.rfind("disc/", 0) != 0 || cue_field.size() <= 5 || cue_field.find_first_of("/\\", 5) != std::string::npos || cue_field.find("..") != std::string::npos)
    return refuse({"E_DISC_BUILD", "the build record names a patched disc outside the build folder"});
  const auto run = [&](const char *which, const fs::path &disc_dir, std::string &report) {
    const fs::path program = dir / fields[std::string(which) + "_program"], out = dir / (std::string(which) + "-report.json"), log = dir / (std::string(which) + "-run.log");
    std::error_code removed;
    fs::remove(out, removed);
    const int code = run_process({program.string(), "--disc-dir", disc_dir.string(), "--bios", o.bios.string(), "--instruction-budget", o.budget, "--report", out.string()}, log, {},
                                 static_cast<unsigned>(std::stoul(o.timeout)));
    return code != -1 && code != kProcessTimedOut && read_text(out, 16U << 20U, report);
  };
  std::string a, b;
  if (!run("vanilla", fs::absolute(o.cue, ec).parent_path(), a)) return refuse({"E_DISC_RUN", "the Vanilla program did not produce a report within the timeout (vanilla-run.log)"});
  if (!run("modded", dir / fs::path(fields["patched_cue"]).parent_path(), b)) return refuse({"E_DISC_RUN", "the Patched program did not produce a report within the timeout (modded-run.log)"});
  const auto left = members(a), right = members(b);
  if (left.empty() || right.empty()) return refuse({"E_DISC_RUN", "a program report cannot be read"});
  const auto value = [](const std::string &v) { return v.size() <= 512 ? v : "{\"sha256\":\"" + digest_of(v) + "\"}"; };
  std::vector<std::string> keys;
  std::map<std::string, std::string> l(left.begin(), left.end()), r(right.begin(), right.end());
  for (const auto &m : left) keys.push_back(m.first);
  for (const auto &m : right) if (l.count(m.first) == 0) keys.push_back(m.first);
  std::string differences;
  for (const std::string &k : keys) {
    if (k == "disc_identity" || k == "program_identity") continue;   // different by construction; reported by the build record
    if (l[k] == r[k]) continue;
    differences += std::string(differences.empty() ? "" : ",") + "{\"field\":\"" + esc(k) + "\",\"vanilla\":" + (l.count(k) && !l[k].empty() ? value(l[k]) : "null") + ",\"modded\":" +
                   (r.count(k) && !r[k].empty() ? value(r[k]) : "null") + "}";
  }
  std::cout << "{\"ok\":true,\"budget\":" << o.budget << ",\"identical\":" << (differences.empty() ? "true" : "false") << ",\"differences\":[" << differences << "]}\n";
  return 0;
}

}  // namespace

int run_mod_disc_command(int argc, char **argv) {
  const std::string sub = argv[2];
  Options o;
  if (!parse(argc, argv, o, sub)) return usage();
  if (sub == "apply") return apply(o);
  if (sub == "verify-disc") return verify(o);
  if (sub == "disc-build") return disc_build(o);
  return disc_diff(o);
}

}  // namespace segarecomp_cli
