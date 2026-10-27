#include "sega_cd.hpp"

#include "json_lite.hpp"
#include "launcher_core.hpp"

#include "segarecomp/sha256.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace launcher::scd {
namespace {

std::string u8(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
fs::path from_u8(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }

bool hex_digest(const std::string &s) {
  return s.size() == 64U && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A disc file name in the program's table: a plain name (no directory, no drive, no control character).
bool plain_name(const std::string &name) {
  if (name.empty() || name.size() > 255U || name == "." || name == "..") return false;
  return std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20U; });
}

long long mtime_of(const fs::path &path) {
  std::error_code ec;
  const auto t = fs::last_write_time(path, ec);
  if (ec) return 0;
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

Status fail(State s, Refusal r, std::string detail = {}) {
  Status st;
  st.state = s;
  st.refusal = r;
  st.detail = std::move(detail);
  return st;
}

fs::path disc_dir_of(const EntryRecord &r) { return from_u8(r.cue_path).parent_path(); }

} // namespace

std::string entry_identity(const std::string &bios_sha256, const std::string &cue_sha256) {
  const std::string text = "segarecomp-sega-cd-entry-1\nbios " + bios_sha256 + "\ncue " + cue_sha256 + "\n";
  return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

const char *code(Refusal r) {
  switch (r) {
    case Refusal::none: return "none";
    case Refusal::inputs_missing: return "inputs_missing";
    case Refusal::bios_missing: return "bios_missing";
    case Refusal::bios_unreadable: return "bios_unreadable";
    case Refusal::bios_too_large: return "bios_too_large";
    case Refusal::bios_changed: return "bios_changed";
    case Refusal::cue_missing: return "cue_missing";
    case Refusal::cue_changed: return "cue_changed";
    case Refusal::program_missing: return "program_missing";
    case Refusal::program_file_missing: return "program_file_missing";
    case Refusal::program_changed: return "program_changed";
    case Refusal::program_unverifiable: return "program_unverifiable";
    case Refusal::program_abi_unsupported: return "program_abi_unsupported";
    case Refusal::program_headless_only: return "program_headless_only";
    case Refusal::program_bios_mismatch: return "program_bios_mismatch";
    case Refusal::program_disc_mismatch: return "program_disc_mismatch";
    case Refusal::disc_file_missing: return "disc_file_missing";
    case Refusal::disc_file_size_mismatch: return "disc_file_size_mismatch";
    case Refusal::disc_file_changed: return "disc_file_changed";
    case Refusal::unsupported_build: return "unsupported_build";
  }
  return "unknown";
}

std::string message(Refusal r, const std::string &detail) {
  std::string text;
  switch (r) {
    case Refusal::none: return {};
    case Refusal::inputs_missing: text = "Choose the BIOS image and the disc (CUE) for this game."; break;
    case Refusal::bios_missing: text = "The BIOS file is not at its recorded location. Choose it again."; break;
    case Refusal::bios_unreadable: text = "The BIOS file could not be read."; break;
    case Refusal::bios_too_large: text = "That file is too large to be a Sega CD BIOS image (the limit is 128 KiB)."; break;
    case Refusal::bios_changed: text = "The BIOS file is not the one this game was added with (its size or SHA-256 differs). Add the game again with this BIOS."; break;
    case Refusal::cue_missing: text = "The disc (CUE) file is not at its recorded location. Choose it again."; break;
    case Refusal::cue_changed: text = "The CUE file has changed since the game was added. Add the game again."; break;
    case Refusal::program_missing: text = "No verified program is imported for this game."; break;
    case Refusal::program_file_missing: text = "The imported program is no longer at its recorded location. Import it again."; break;
    case Refusal::program_changed: text = "The imported program has changed since it was verified (size, time or SHA-256). Import it again."; break;
    case Refusal::program_unverifiable: text = "The program could not describe itself, so it cannot be verified (it may be an older build). Rebuild it with the current developer procedure."; break;
    case Refusal::program_abi_unsupported: text = "The program was built for a different runtime contract than this Segarecomp speaks (stale build). Rebuild it with the current developer procedure."; break;
    case Refusal::program_headless_only: text = "This program is a headless build and cannot be played in the viewer. Build it with --viewer."; break;
    case Refusal::program_bios_mismatch: text = "The program was built for a different BIOS image than the one chosen for this game."; break;
    case Refusal::program_disc_mismatch: text = "The program was built for a different disc than the one this game uses."; break;
    case Refusal::disc_file_missing: text = "A disc file the program needs was not found next to the CUE file."; break;
    case Refusal::disc_file_size_mismatch: text = "A disc file next to the CUE file has a different size than the program expects."; break;
    case Refusal::disc_file_changed: text = "A disc file next to the CUE file is not the one the program was built from (its SHA-256 differs)."; break;
    case Refusal::unsupported_build: text = "Segarecomp cannot build Sega CD games yet. Import a verified program built with the developer procedure."; break;
  }
  if (!detail.empty()) text += " (" + detail + ")";
  return text;
}

const char *state_label(State s) {
  switch (s) {
    case State::inputs_not_configured: return "Inputs not configured";
    case State::no_program: return "No program (automatic build unsupported)";
    case State::headless_only: return "Headless-only build";
    case State::stale: return "Stale program";
    case State::mismatched: return "Mismatched";
    case State::ready: return "Ready to play";
  }
  return "";
}

// ---- hashing ----

Digest hash_file(const fs::path &path, std::uintmax_t limit, std::atomic<std::uint64_t> *done, const std::atomic<bool> *cancel) {
  Digest d;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) { d.error = "missing"; return d; }
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) { d.error = "unreadable"; return d; }
  if (size > limit) { d.error = "too_large"; d.size = size; return d; }
  std::ifstream in(path, std::ios::binary);
  if (!in) { d.error = "unreadable"; return d; }
  segarecomp::Sha256Stream sha;
  std::vector<std::uint8_t> buffer(1U << 20U);
  std::uint64_t total = 0;
  while (in) {
    if (cancel != nullptr && cancel->load()) { d.error = "cancelled"; return d; }
    in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    sha.update({buffer.data(), static_cast<std::size_t>(got)});
    total += static_cast<std::uint64_t>(got);
    if (done != nullptr) done->fetch_add(static_cast<std::uint64_t>(got));
  }
  if (in.bad() || total != size) { d.error = "unreadable"; return d; }
  d.ok = true;
  d.size = size;
  d.sha256 = sha.finish_hex();
  return d;
}

// ---- the program's statement ----

Claims parse_claims(const std::string &text) {
  Claims c;
  const auto unverifiable = [&](const std::string &why) {
    c = Claims{};
    c.refusal = Refusal::program_unverifiable;
    c.detail = why;
    return c;
  };
  if (text.empty() || text.size() > 8192U) return unverifiable("no statement");
  std::string error;
  const auto root = json::parse(text, &error);
  if (!root || !root->is_object()) {
    std::string head;  // a short, printable-ASCII head of what the program printed (diagnostics only)
    for (std::size_t i = 0; i < text.size() && head.size() < 48; ++i) head.push_back(text[i] >= 0x20 && text[i] < 0x7F ? text[i] : '?');
    return unverifiable("not a statement: " + (root ? std::string("not an object") : error) + " [" + head + "]");
  }
  if (root->num("describe", 0) != 1) return unverifiable("unknown statement version");
  c.runtime_abi = static_cast<int>(std::clamp(root->num("runtime_abi", 0), 0LL, 1000000LL));
  c.profile = root->str("profile");
  const json::Value *viewer = root->get("viewer");
  if (viewer == nullptr || viewer->kind != json::Value::Kind::boolean) return unverifiable("missing viewer flag");
  c.viewer = viewer->boolean;
  c.program_identity = root->str("program_identity");
  c.disc_identity = root->str("disc_identity");
  if (c.runtime_abi <= 0 || (c.profile != "bios_image" && c.profile != "synthetic") || !hex_digest(c.program_identity) || !hex_digest(c.disc_identity))
    return unverifiable("missing or malformed identity");
  if (const json::Value *bios = root->get("bios"); bios != nullptr && bios->is_object()) {
    c.bios_size = static_cast<std::uint64_t>(std::max(0LL, bios->num("size")));
    c.bios_sha256 = bios->str("sha256");
    if (!hex_digest(c.bios_sha256) || c.bios_size == 0 || c.bios_size > max_bios_bytes) return unverifiable("malformed BIOS statement");
  } else if (c.profile == "bios_image") {
    return unverifiable("the BIOS-image program states no BIOS");
  }
  const json::Value *files = root->get("disc_files");
  if (files == nullptr || !files->is_array() || files->items.empty() || files->items.size() > max_disc_files) return unverifiable("malformed disc table");
  std::set<std::string> seen;
  for (const auto &f : files->items) {
    FileFact fact;
    fact.name = f.is_object() ? f.str("name") : std::string();
    const long long size = f.is_object() ? f.num("size", -1) : -1;
    fact.sha256 = f.is_object() ? f.str("sha256") : std::string();
    if (!plain_name(fact.name) || size < 0 || !hex_digest(fact.sha256) || !seen.insert(fact.name).second) return unverifiable("malformed disc file entry");
    fact.size = static_cast<std::uint64_t>(size);
    c.disc_files.push_back(std::move(fact));
  }
  return c;
}

Claims describe_program(const fs::path &program, unsigned timeout_ms) {
  std::error_code ec;
  if (!fs::is_regular_file(program, ec)) {
    Claims c;
    c.refusal = Refusal::program_file_missing;
    return c;
  }
  const std::string exe = u8(program);
  std::vector<const char *> argv{exe.c_str(), "--describe", nullptr};
  // The statement goes to a temporary FILE, not a pipe: a redirected file behaves the same on every platform (a pipe read can return empty while the
  // program still has output - seen on Windows after the first 512 bytes).
  static std::atomic<unsigned> counter{0};
  const fs::path capture = fs::temp_directory_path() / ("segarecomp-describe-" + std::to_string(SDL_GetTicksNS() & 0xFFFFFFFFULL) + "-" + std::to_string(counter.fetch_add(1)) + ".txt");
  SDL_IOStream *sink = SDL_IOFromFile(u8(capture).c_str(), "wb");
  Claims fail_claims;
  fail_claims.refusal = Refusal::program_unverifiable;
  if (sink == nullptr) { fail_claims.detail = "its output could not be captured"; return fail_claims; }
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, sink);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_Process *process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  SDL_CloseIO(sink);
  if (process == nullptr) { fs::remove(capture, ec); fail_claims.detail = "it could not be started"; return fail_claims; }
  const Uint64 deadline = SDL_GetTicks() + timeout_ms;
  bool timed_out = false;
  int code_out = -1;
  while (!SDL_WaitProcess(process, false, &code_out)) {
    if (SDL_GetTicks() > deadline) { timed_out = true; break; }
    SDL_Delay(5);
  }
  if (timed_out) { SDL_KillProcess(process, true); SDL_WaitProcess(process, true, &code_out); }
  SDL_DestroyProcess(process);
  std::string out;
  if (!timed_out && code_out == 0) {
    std::ifstream in(capture, std::ios::binary);
    char buffer[4096];
    while (in && out.size() < 12000U) { in.read(buffer, sizeof buffer); out.append(buffer, static_cast<std::size_t>(in.gcount())); }
  }
  fs::remove(capture, ec);
  if (timed_out) { fail_claims.detail = "it did not answer"; return fail_claims; }
  if (code_out != 0) { fail_claims.detail = "it does not understand the statement request (status " + std::to_string(code_out) + ")"; return fail_claims; }
  return parse_claims(out);
}

// ---- status ----

Status quick_status(const EntryRecord &r) {
  if (r.bios_path.empty() || r.cue_path.empty()) return fail(State::inputs_not_configured, Refusal::inputs_missing);
  std::error_code ec;
  const fs::path bios = from_u8(r.bios_path), cue = from_u8(r.cue_path);
  const Digest b = hash_file(bios, max_bios_bytes);
  if (!b.ok) return fail(State::inputs_not_configured, b.error == "missing" ? Refusal::bios_missing : b.error == "too_large" ? Refusal::bios_too_large : Refusal::bios_unreadable);
  if (b.size != r.bios_size || b.sha256 != r.bios_sha256) return fail(State::mismatched, Refusal::bios_changed);
  const Digest c = hash_file(cue, max_cue_bytes);
  if (!c.ok) return fail(State::inputs_not_configured, c.error == "missing" ? Refusal::cue_missing : Refusal::cue_changed);
  if (c.sha256 != r.cue_sha256) return fail(State::mismatched, Refusal::cue_changed);
  if (r.program.path.empty()) { Status st = fail(State::no_program, Refusal::program_missing); st.bios_ok = true; return st; }
  Status st;
  st.bios_ok = true;
  const fs::path program = from_u8(r.program.path);
  if (!fs::is_regular_file(program, ec)) return fail(State::stale, Refusal::program_file_missing);
  if (fs::file_size(program, ec) != r.program.size || mtime_of(program) != r.program.mtime) return fail(State::stale, Refusal::program_changed);
  if (r.program.runtime_abi != supported_runtime_abi) return fail(State::stale, Refusal::program_abi_unsupported);
  if (!r.program.viewer) return fail(State::headless_only, Refusal::program_headless_only);
  const fs::path dir = disc_dir_of(r);
  for (const auto &f : r.disc_files) {
    const fs::path file = dir / from_u8(f.name);
    if (!fs::is_regular_file(file, ec)) return fail(State::inputs_not_configured, Refusal::disc_file_missing, f.name);
    if (fs::file_size(file, ec) != f.size) return fail(State::mismatched, Refusal::disc_file_size_mismatch, f.name);
  }
  st.disc_ok = true;
  st.program_ok = true;
  st.state = State::ready;
  return st;
}

namespace {

// The claims against the measured files; `Refusal::none` when they agree.
Refusal check_claims(const Claims &claims, const EntryRecord &r, std::string &detail) {
  if (claims.refusal != Refusal::none) { detail = claims.detail; return claims.refusal; }
  if (claims.runtime_abi != supported_runtime_abi) { detail = "runtime contract " + std::to_string(claims.runtime_abi); return Refusal::program_abi_unsupported; }
  if (claims.profile != "bios_image") { detail = "not a BIOS-image program"; return Refusal::program_bios_mismatch; }
  if (!claims.viewer) return Refusal::program_headless_only;
  if (claims.bios_sha256 != r.bios_sha256 || claims.bios_size != r.bios_size) return Refusal::program_bios_mismatch;
  return Refusal::none;
}

} // namespace

Plan prepare_launch(const EntryRecord &r, bool check_disc_digests, std::atomic<std::uint64_t> *done, const std::atomic<bool> *cancel) {
  Plan plan;
  plan.status = quick_status(r);
  // A "stale" quick status (changed stamp) is re-examined below: the file hash is the authority, not the modification time.
  const bool stamp_only = plan.status.state == State::stale && plan.status.refusal == Refusal::program_changed;
  if (plan.status.state != State::ready && !stamp_only) return plan;
  plan.program = from_u8(r.program.path);
  plan.bios = from_u8(r.bios_path);
  plan.disc_dir = disc_dir_of(r);
  const Digest p = hash_file(plan.program, max_program_bytes, done, cancel);
  if (!p.ok) { plan.status = fail(State::stale, p.error == "missing" ? Refusal::program_file_missing : Refusal::program_changed, p.error); return plan; }
  if (p.size != r.program.size || p.sha256 != r.program.sha256) { plan.status = fail(State::stale, Refusal::program_changed); return plan; }
  plan.program_mtime = mtime_of(plan.program);
  if (r.program.runtime_abi != supported_runtime_abi) { plan.status = fail(State::stale, Refusal::program_abi_unsupported); return plan; }
  plan.claims = describe_program(plan.program);
  std::string detail;
  const Refusal claim = check_claims(plan.claims, r, detail);
  if (claim != Refusal::none) {
    const State s = claim == Refusal::program_headless_only ? State::headless_only
                    : claim == Refusal::program_bios_mismatch || claim == Refusal::program_disc_mismatch ? State::mismatched : State::stale;
    plan.status = fail(s, claim, detail);
    return plan;
  }
  if (plan.claims.program_identity != r.program.program_identity || plan.claims.disc_identity != r.program.disc_identity) {
    plan.status = fail(State::stale, Refusal::program_changed, "its identity differs from the one verified at import");
    return plan;
  }
  // The disc files the program states, measured next to the CUE: sizes always, digests on request (the program re-verifies them again itself).
  std::error_code ec;
  for (const auto &f : plan.claims.disc_files) {
    const fs::path file = plan.disc_dir / from_u8(f.name);
    if (!fs::is_regular_file(file, ec)) { plan.status = fail(State::inputs_not_configured, Refusal::disc_file_missing, f.name); return plan; }
    if (fs::file_size(file, ec) != f.size) { plan.status = fail(State::mismatched, Refusal::disc_file_size_mismatch, f.name); return plan; }
    if (check_disc_digests) {
      const Digest d = hash_file(file, f.size, done, cancel);
      if (!d.ok) { plan.status = fail(State::inputs_not_configured, d.error == "cancelled" ? Refusal::disc_file_changed : Refusal::disc_file_missing, f.name); return plan; }
      if (d.sha256 != f.sha256) { plan.status = fail(State::mismatched, Refusal::disc_file_changed, f.name); return plan; }
    }
  }
  plan.status = Status{};
  plan.status.state = State::ready;
  plan.status.bios_ok = plan.status.disc_ok = plan.status.program_ok = true;
  return plan;
}

// ---- import ----

namespace {

ImportResult refuse(Refusal r, std::string detail = {}) {
  ImportResult out;
  out.refusal = r;
  out.detail = std::move(detail);
  return out;
}

bool same_table(const std::vector<FileFact> &a, const std::vector<FileFact> &b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].size != b[i].size || a[i].sha256 != b[i].sha256) return false;
  return true;
}

// Verifies `program` for `r` (BIOS and CUE already measured) and fills the program, disc table and verification flag. On failure `r` is untouched.
ImportResult verify_program_for(EntryRecord r, const fs::path &program, std::atomic<std::uint64_t> *done, const std::atomic<bool> *cancel) {
  ImportResult out;
  out.record = r;
  const Digest p = hash_file(program, max_program_bytes, done, cancel);
  if (!p.ok) {
    out.refusal = p.error == "missing" ? Refusal::program_file_missing : Refusal::program_unverifiable;
    out.detail = p.error;
    return out;
  }
  const Claims claims = describe_program(program);
  std::string detail;
  const Refusal claim = check_claims(claims, r, detail);
  if (claim != Refusal::none) { out.refusal = claim; out.detail = detail; return out; }
  // Replacing a program of an entry whose disc is already verified: the new one must be built for the same disc files.
  if (r.disc_verified && !same_table(r.disc_files, claims.disc_files)) { out.refusal = Refusal::program_disc_mismatch; return out; }
  std::error_code ec;
  const fs::path dir = from_u8(r.cue_path).parent_path();
  for (const auto &f : claims.disc_files) {
    const fs::path file = dir / from_u8(f.name);
    if (!fs::is_regular_file(file, ec)) { out.refusal = Refusal::disc_file_missing; out.detail = f.name; return out; }
    if (fs::file_size(file, ec) != f.size) { out.refusal = Refusal::disc_file_size_mismatch; out.detail = f.name; return out; }
    const Digest d = hash_file(file, f.size, done, cancel);
    if (!d.ok) { out.refusal = d.error == "cancelled" ? Refusal::disc_file_changed : Refusal::disc_file_missing; out.detail = f.name; return out; }
    if (d.sha256 != f.sha256) { out.refusal = Refusal::disc_file_changed; out.detail = f.name; return out; }
  }
  r.disc_files = claims.disc_files;
  r.disc_verified = true;
  r.program.path = u8(program);
  r.program.size = p.size;
  r.program.mtime = mtime_of(program);
  r.program.sha256 = p.sha256;
  r.program.program_identity = claims.program_identity;
  r.program.disc_identity = claims.disc_identity;
  r.program.runtime_abi = claims.runtime_abi;
  r.program.viewer = claims.viewer;
  r.program.verified_at = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  out.record = r;
  out.program_attached = true;
  return out;
}

} // namespace

ImportResult import_entry(const ImportRequest &request, std::atomic<std::uint64_t> *done, const std::atomic<bool> *cancel) {
  if (request.bios.empty() || request.cue.empty()) return refuse(Refusal::inputs_missing);
  EntryRecord r;
  const Digest b = hash_file(request.bios, max_bios_bytes, done, cancel);
  if (!b.ok) return refuse(b.error == "missing" ? Refusal::bios_missing : b.error == "too_large" ? Refusal::bios_too_large : Refusal::bios_unreadable);
  const Digest c = hash_file(request.cue, max_cue_bytes, done, cancel);
  if (!c.ok) return refuse(c.error == "too_large" ? Refusal::cue_changed : Refusal::cue_missing, c.error == "too_large" ? "that is not a CUE sheet (too large)" : std::string());
  r.bios_path = u8(request.bios);
  r.bios_size = b.size;
  r.bios_sha256 = b.sha256;
  r.cue_path = u8(request.cue);
  r.cue_size = c.size;
  r.cue_sha256 = c.sha256;
  ImportResult out;
  if (!request.program.empty()) {
    out = verify_program_for(r, request.program, done, cancel);
    if (!out.program_attached) { const Refusal why = out.refusal; const std::string detail = out.detail; out = ImportResult{}; out.refusal = why; out.detail = detail; out.record = r; }
  } else {
    out.record = r;
  }
  out.title = clean_title(u8(request.cue.stem()));
  if (out.title.empty()) out.title = "Sega CD game";
  return out;
}

ImportResult attach_program(const EntryRecord &record, const fs::path &program, std::atomic<std::uint64_t> *done, const std::atomic<bool> *cancel) {
  ImportResult out = verify_program_for(record, program, done, cancel);
  if (!out.program_attached) { const Refusal why = out.refusal; const std::string detail = out.detail; out = ImportResult{}; out.refusal = why; out.detail = detail; out.record = record; }
  return out;
}

// ---- running ----

std::uint64_t replay_end_tick(const fs::path &script, unsigned margin_frames) {
  constexpr std::uint64_t frame = UINT64_C(262) * 3420U * 500000U, first_onset = UINT64_C(111858) * 500000U;
  std::uint64_t last = 0;
  std::ifstream in(script, std::ios::binary);
  std::string line;
  std::size_t lines = 0;
  while (std::getline(in, line) && ++lines <= 8192) {  // the grammar allows 4,096 events
    std::uint64_t tick = 0;
    std::size_t digits = 0;
    while (digits < line.size() && digits < 19 && line[digits] >= '0' && line[digits] <= '9') tick = tick * 10 + static_cast<std::uint64_t>(line[digits++] - '0');
    if (digits > 0 && digits < line.size() && line[digits] == ' ' && tick > last) last = tick;
  }
  return (last != 0 ? last : first_onset) + static_cast<std::uint64_t>(margin_frames) * frame;
}

std::vector<std::uint8_t> formatted_backup_ram() {
  constexpr std::size_t size = 0x2000, block = 64;
  std::vector<std::uint8_t> image(size, 0);
  std::vector<std::uint8_t> trailer;
  trailer.insert(trailer.end(), 11, static_cast<std::uint8_t>('_'));
  trailer.insert(trailer.end(), {0, 0, 0, 0, 0x40});
  const std::uint16_t free_blocks = static_cast<std::uint16_t>(size / block - 3);  // 125 for an empty internal RAM
  for (int i = 0; i < 4; ++i) { trailer.push_back(static_cast<std::uint8_t>(free_blocks >> 8)); trailer.push_back(static_cast<std::uint8_t>(free_blocks & 0xFF)); }
  trailer.insert(trailer.end(), 8, 0);
  for (const char c : std::string("SEGA_CD_ROM")) trailer.push_back(static_cast<std::uint8_t>(c));
  trailer.insert(trailer.end(), {0, 1, 0, 0, 0});
  for (const char c : std::string("RAM_CARTRIDGE___")) trailer.push_back(static_cast<std::uint8_t>(c));
  std::copy(trailer.begin(), trailer.end(), image.end() - static_cast<std::ptrdiff_t>(block));
  return image;
}

bool ensure_backup_ram(const fs::path &path) {
  const std::vector<std::uint8_t> want = formatted_backup_ram();
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == want.size()) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (have == want) return true;
  }
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(want.data()), static_cast<std::streamsize>(want.size()));
  out.flush();
  return static_cast<bool>(out);
}

std::vector<std::string> run_arguments(const Plan &plan, const RunOptions &o) {
  std::vector<std::string> args{"--disc-dir", u8(plan.disc_dir), "--bios", u8(plan.bios), "--instruction-budget", std::to_string(o.instruction_budget)};
  if (!o.backup_ram.empty()) { args.emplace_back("--backup-ram"); args.push_back(u8(o.backup_ram)); }
  if (!o.report.empty()) { args.emplace_back("--report"); args.push_back(u8(o.report)); }
  if (o.until_tick != 0) { args.emplace_back("--until-tick"); args.push_back(std::to_string(o.until_tick)); }
  if (!o.record_to.empty()) { args.emplace_back("--record-input"); args.push_back(u8(o.record_to)); }
  else if (!o.replay_from.empty()) { args.emplace_back("--input-script"); args.push_back(u8(o.replay_from)); }
  return args;
}

Outcome read_outcome(const fs::path &report, int exit_code) {
  Outcome o;
  o.exit_code = exit_code;
  std::error_code ec;
  if (report.empty() || !fs::is_regular_file(report, ec) || fs::file_size(report, ec) > (8U << 20U)) return o;
  std::ifstream in(report, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto root = json::parse(text);  // the parser bounds size and depth itself
  if (!root || !root->is_object()) return o;
  o.report_read = true;
  o.result = root->str("result");
  if (const json::Value *stop = root->get("stop"); stop != nullptr && stop->is_object() && o.result == "stopped") o.stop_class = stop->str("class");
  if (const json::Value *live = root->get("live"); live != nullptr && live->is_object()) {
    o.live_outcome = live->str("outcome");
    o.frames = static_cast<std::uint64_t>(std::max(0LL, live->num("frames")));
    o.presented = static_cast<std::uint64_t>(std::max(0LL, live->num("presented")));
  }
  if (const json::Value *input = root->get("input"); input != nullptr && input->is_object()) o.input_events = static_cast<std::uint64_t>(std::max(0LL, input->num("events")));
  return o;
}

bool is_refusal_exit(int exit_code) { return exit_code >= 20 && exit_code <= 27; }

std::string describe_outcome(const Outcome &o) {
  switch (o.exit_code) {
    case 0:
      if (o.live_outcome == "quit") return "Closed.";
      return "The program finished.";
    case 10: return "Typed execution stop: " + (o.stop_class.empty() ? std::string("(class not reported)") : o.stop_class) + ". The program stops rather than guess.";
    case 11: return o.result == "budget_exhausted" ? "The run reached its instruction budget." : "The run ended at its time limit.";
    case 20: return "The disc files do not match the program (refused before any guest instruction).";
    case 21: return "The BIOS image does not match the program (refused before any guest instruction).";
    case 22: return "The backup RAM input was refused.";
    case 23: return "The input script was refused.";
    case 24: return "The viewer could not run (window, input or presentation failure).";
    case 25: return "The input recording reached its size limit; the part recorded so far was kept.";
    case 26: return "The input recorder refused a transition.";
    case 27: return "The input recording could not be written.";
    default: return "The program ended unexpectedly (status " + std::to_string(o.exit_code) + ").";
  }
}

} // namespace launcher::scd
