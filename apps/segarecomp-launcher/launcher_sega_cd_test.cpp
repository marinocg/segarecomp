// SEG-078-T002 (ADR 0204): the launcher's Sega CD core - identity, the program's statement, verification, typed refusals, import, run arguments
// and outcomes - and the library catalog's Sega CD record (schema, byte-compatibility of existing libraries). No window, no product CLI, no
// compiler: a program double stands in for the generated viewer program. Every expected digest is computed in this file from the bytes it writes.
//
// usage: launcher_sega_cd_test <fake_scd_program>

#include "json_lite.hpp"
#include "launcher_core.hpp"
#include "library_catalog.hpp"
#include "sega_cd.hpp"

#include "segarecomp/sha256.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
std::string slurp(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); }
void spit(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string sha(const std::string &bytes) { return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}); }
std::string u8(const fs::path &p) { const auto t = p.u8string(); return std::string(reinterpret_cast<const char *>(t.data()), t.size()); }

// Project-authored bytes: stand-ins for a BIOS image and two disc files. They are NOT console data.
const std::string bios_bytes = std::string("SYNTHETIC BIOS-LIKE IMAGE FOR THE LAUNCHER TEST ") + std::string(4000, 'x');
const std::string track1 = std::string("SYNTHETIC TRACK ONE ") + std::string(3000, '1');
const std::string track2 = std::string("SYNTHETIC TRACK TWO ") + std::string(5000, '2');

std::string statement(bool viewer, int abi, const std::string &bios_digest, std::uint64_t bios_size, const std::string &name1 = "Game (Track 1).bin",
                      const std::string &digest1 = {}, const std::string &profile = "bios_image") {
  std::ostringstream o;
  o << "{\"describe\":1,\"runtime_abi\":" << abi << ",\"profile\":\"" << profile << "\",\"viewer\":" << (viewer ? "true" : "false")
    << ",\"program_identity\":\"" << std::string(64, '7') << "\",\"disc_identity\":\"" << std::string(64, '8') << "\",\"bios\":{\"size\":" << bios_size
    << ",\"sha256\":\"" << bios_digest << "\"},\"disc_files\":[{\"name\":\"" << name1 << "\",\"size\":" << track1.size() << ",\"sha256\":\""
    << (digest1.empty() ? sha(track1) : digest1) << "\"},{\"name\":\"Game (Track 2).bin\",\"size\":" << track2.size() << ",\"sha256\":\"" << sha(track2)
    << "\"}]}\n";
  return o.str();
}

// A path as it appears inside a JSON string (a Windows path has backslashes to escape).
std::string json_escaped(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

struct Fixture {
  fs::path dir, bios, cue, program;
  explicit Fixture(const fs::path &base, const fs::path &fake_program, const std::string &name = "fx") {
    dir = base / name;
    fs::remove_all(dir);
    fs::create_directories(dir / "disc");
    bios = dir / "bios.bin";
    cue = dir / "disc" / "Game.cue";
    #if defined(_WIN32)
    program = dir / "prog" / "game-viewer.exe";  // a program double must be an executable name to be started
#else
    program = dir / "prog" / "game-viewer";
#endif
    spit(bios, bios_bytes);
    spit(cue, "FILE \"Game (Track 1).bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\nFILE \"Game (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    spit(dir / "disc" / "Game (Track 1).bin", track1);
    spit(dir / "disc" / "Game (Track 2).bin", track2);
    fs::create_directories(program.parent_path());
    fs::copy_file(fake_program, program, fs::copy_options::overwrite_existing);
    describe(statement(true, scd::supported_runtime_abi, sha(bios_bytes), bios_bytes.size()));
  }
  void describe(const std::string &text) const { spit(fs::path(program).concat(".describe"), text); }
};

std::string library_fixture() {  // a library written by the release BEFORE Sega CD existed (Genesis + Master System), byte for byte
  return R"JSON({
  "schema": 1,
  "games": [
    {
      "platform": "genesis",
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "detected_title": "Fictional Genesis Game",
      "display_title": "Renamed Genesis",
      "rom_paths": [
        "/roms/Sonic Fictional.md"
      ],
      "rom_path_unknown": false,
      "mapper": "",
      "mapper_manifest_sha256": "",
      "aot_policy": "compatibility",
      "aot_model": "gen2",
      "hint_alias_discovery": true,
      "region": "japan",
      "cover": "",
      "favorite": true,
      "archived": false,
      "added_at": 1700000000,
      "last_built_at": 1700000100,
      "last_played_at": 1700000200,
      "last_run": "clean",
      "last_exit_code": 0,
      "current_build_key": "cccccccccccccccccccccccccccccccc",
      "builds": [
        {
          "key": "cccccccccccccccccccccccccccccccc",
          "built_at": 1700000100,
          "mode": "compatibility",
          "version": "0.9.0",
          "toolchain": "zig 0.15.2"
        }
      ]
    },
    {
      "platform": "master-system",
      "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "detected_title": "Fictional SMS Game",
      "display_title": "",
      "rom_paths": [
        "/roms/Fictional.sms",
        "/other/Fictional.sms"
      ],
      "rom_path_unknown": false,
      "mapper": "sega",
      "mapper_manifest_sha256": "",
      "aot_policy": "optimized",
      "aot_model": "v1",
      "hint_alias_discovery": false,
      "region": "usa",
      "cover": "",
      "favorite": false,
      "archived": false,
      "added_at": 1700000300,
      "last_built_at": 0,
      "last_played_at": 0,
      "last_run": "",
      "last_exit_code": 0,
      "current_build_key": "",
      "builds": []
    }
  ]
}
)JSON";
}

void test_identity_and_hash(const fs::path &base) {
  // printf 'segarecomp-sega-cd-entry-1\nbios aaaa...\ncue bbbb...\n' | sha256sum
  expect(scd::entry_identity(std::string(64, 'a'), std::string(64, 'b')) == "5fc131bcc07e42dbfc45af306036dca12adbb7c906ed2e48ce10ee864a57a83d", "entry identity is the documented digest");
  expect(scd::entry_identity(std::string(64, 'a'), std::string(64, 'c')) != scd::entry_identity(std::string(64, 'a'), std::string(64, 'b')), "a different CUE is a different game");
  expect(scd::entry_identity(std::string(64, 'c'), std::string(64, 'b')) != scd::entry_identity(std::string(64, 'a'), std::string(64, 'b')), "a different BIOS is a different game");
  spit(base / "abc.bin", "abc");
  const scd::Digest d = scd::hash_file(base / "abc.bin", 100);
  expect(d.ok && d.size == 3 && d.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc' (FIPS 180-4 vector)");
  expect(!scd::hash_file(base / "abc.bin", 2).ok && scd::hash_file(base / "abc.bin", 2).error == "too_large", "a file above the limit is refused before reading");
  expect(scd::hash_file(base / "none.bin", 100).error == "missing", "a missing file is reported as missing");
  std::atomic<std::uint64_t> done{0};
  std::atomic<bool> cancel{true};
  expect(scd::hash_file(base / "abc.bin", 100, &done, &cancel).error == "cancelled", "a cancelled hash stops before reading");
}

void test_claims() {
  const std::string good = statement(true, 1, std::string(64, 'a'), 128);
  const scd::Claims c = scd::parse_claims(good);
  expect(c.refusal == scd::Refusal::none && c.viewer && c.runtime_abi == 1 && c.disc_files.size() == 2 && c.disc_files[0].name == "Game (Track 1).bin" && c.bios_size == 128,
         "a well-formed statement parses");
  const auto bad = [&](const std::string &text, const std::string &why) { expect(scd::parse_claims(text).refusal == scd::Refusal::program_unverifiable, "unverifiable: " + why); };
  bad("", "empty");
  bad("not json", "not json");
  bad(std::string(9000, ' '), "oversized");
  bad("{\"describe\":2}", "unknown statement version");
  bad("[]", "not an object");
  std::string t = good;
  t.replace(t.find("\"viewer\":true,"), 14, "");
  bad(t, "missing viewer flag");
  t = good; t.replace(t.find(std::string(64, '7')), 64, "xyz");
  bad(t, "malformed program identity");
  t = good; t.replace(t.find("Game (Track 1).bin"), 18, "../Game (Track 1)");
  bad(t, "a directory in a file name");
  t = good; t.replace(t.find("Game (Track 1).bin"), 18, "Game (Track 2).bin");
  bad(t, "a duplicate file name");
  t = good; t.replace(t.find("\"bios_image\""), 11, "\"synthetic\"");
  expect(scd::parse_claims(t).refusal == scd::Refusal::program_unverifiable, "a BIOS-image statement without profile bios_image is not accepted as BIOS-less synthetic");
  bad(statement(true, 1, std::string(64, 'a'), 0), "a zero-size BIOS");
  bad(statement(true, 1, std::string(63, 'a'), 128), "a short BIOS digest");
  bad(statement(true, 1, std::string(64, 'a'), 0x20001), "a BIOS larger than the limit");
  std::string many = "{\"describe\":1,\"runtime_abi\":1,\"profile\":\"bios_image\",\"viewer\":true,\"program_identity\":\"" + std::string(64, '7') + "\",\"disc_identity\":\"" +
                     std::string(64, '8') + "\",\"bios\":{\"size\":4,\"sha256\":\"" + std::string(64, 'a') + "\"},\"disc_files\":[";
  for (int i = 0; i < 9; ++i) many += std::string(i ? "," : "") + "{\"name\":\"f" + std::to_string(i) + "\",\"size\":1,\"sha256\":\"" + std::string(64, 'b') + "\"}";
  many += "]}";
  bad(many, "more than eight disc files");
}

scd::EntryRecord imported(const Fixture &f, scd::ImportResult *out = nullptr) {
  scd::ImportRequest req;
  req.bios = f.bios;
  req.cue = f.cue;
  req.program = f.program;
  scd::ImportResult r = scd::import_entry(req);
  if (out != nullptr) *out = r;
  return r.record;
}

void test_import_and_status(const fs::path &base, const fs::path &fake) {
  Fixture f(base, fake);
  scd::ImportResult res;
  const scd::EntryRecord rec = imported(f, &res);
  expect(res.refusal == scd::Refusal::none && res.program_attached && res.title == "Game", "import: a coherent BIOS, disc and program are accepted (" + res.detail + ")");
  expect(rec.bios_sha256 == sha(bios_bytes) && rec.bios_size == bios_bytes.size() && rec.cue_sha256 == sha(slurp(f.cue)), "import: the BIOS and CUE digests are the measured ones");
  expect(rec.disc_verified && rec.disc_files.size() == 2 && rec.program.sha256 == sha(slurp(f.program)) && rec.program.viewer, "import: disc digests measured, program hashed, viewer variant");
  expect(scd::quick_status(rec).state == scd::State::ready, "status: ready to play");

  // no program: the entry exists, the automatic build is unsupported
  scd::ImportRequest only;
  only.bios = f.bios; only.cue = f.cue;
  const scd::ImportResult bare = scd::import_entry(only);
  expect(bare.refusal == scd::Refusal::none && !bare.program_attached && scd::quick_status(bare.record).state == scd::State::no_program, "no program: state no_program");
  expect(std::string(scd::code(scd::Refusal::unsupported_build)) == "unsupported_build" && scd::message(scd::Refusal::unsupported_build).find("Import a verified program") != std::string::npos,
         "the unsupported automatic build is a stable typed state with an actionable message");

  // inputs not configured
  scd::EntryRecord blank;
  expect(scd::quick_status(blank).state == scd::State::inputs_not_configured && scd::quick_status(blank).refusal == scd::Refusal::inputs_missing, "inputs not configured");
  scd::ImportRequest no_bios;
  no_bios.cue = f.cue;
  expect(scd::import_entry(no_bios).refusal == scd::Refusal::inputs_missing, "import without a BIOS is refused");
  scd::ImportRequest missing;
  missing.bios = f.dir / "nope.bin"; missing.cue = f.cue;
  expect(scd::import_entry(missing).refusal == scd::Refusal::bios_missing, "import with a missing BIOS file is refused");
  spit(f.dir / "huge.bin", std::string(0x20001, 'h'));
  missing.bios = f.dir / "huge.bin";
  expect(scd::import_entry(missing).refusal == scd::Refusal::bios_too_large, "import with an oversized BIOS is refused before hashing");
  scd::ImportRequest bad_cue;
  bad_cue.bios = f.bios; bad_cue.cue = f.dir / "nope.cue";
  expect(scd::import_entry(bad_cue).refusal == scd::Refusal::cue_missing, "import with a missing CUE is refused");
  spit(f.dir / "big.cue", std::string(70000, 'c'));
  bad_cue.cue = f.dir / "big.cue";
  expect(scd::import_entry(bad_cue).refusal == scd::Refusal::cue_changed, "a CUE file above 64 KiB is not a CUE sheet");

  {  // the BIOS file changed (mismatched), then missing (inputs not configured)
    scd::EntryRecord r = rec;
    spit(f.bios, bios_bytes + "!");
    expect(scd::quick_status(r).state == scd::State::mismatched && scd::quick_status(r).refusal == scd::Refusal::bios_changed, "a changed BIOS file is mismatched");
    fs::remove(f.bios);
    expect(scd::quick_status(r).state == scd::State::inputs_not_configured && scd::quick_status(r).refusal == scd::Refusal::bios_missing, "a moved BIOS file is inputs not configured");
    spit(f.bios, bios_bytes);
  }
  {  // disc files
    scd::EntryRecord r = rec;
    fs::rename(f.dir / "disc" / "Game (Track 2).bin", f.dir / "disc" / "moved.bin");
    expect(scd::quick_status(r).state == scd::State::inputs_not_configured && scd::quick_status(r).refusal == scd::Refusal::disc_file_missing, "a missing disc file is inputs not configured");
    fs::rename(f.dir / "disc" / "moved.bin", f.dir / "disc" / "Game (Track 2).bin");
    spit(f.dir / "disc" / "Game (Track 2).bin", track2 + "x");
    expect(scd::quick_status(r).state == scd::State::mismatched && scd::quick_status(r).refusal == scd::Refusal::disc_file_size_mismatch, "a disc file of another size is mismatched");
    std::string same_size = track2;
    same_size[100] = '!';
    spit(f.dir / "disc" / "Game (Track 2).bin", same_size);
    expect(scd::quick_status(r).state == scd::State::ready, "status: a same-size change is not seen by the quick check ...");
    const scd::Plan plan = scd::prepare_launch(r, true);
    expect(plan.status.state == scd::State::mismatched && plan.status.refusal == scd::Refusal::disc_file_changed, "... but the full check measures the digest: disc_file_changed");
    expect(scd::prepare_launch(r, false).status.state == scd::State::ready, "the launch check without digests still passes (the program re-verifies every file itself)");
    spit(f.dir / "disc" / "Game (Track 2).bin", track2);
    expect(scd::prepare_launch(r, true).status.state == scd::State::ready, "the restored disc passes the full check");
  }
  {  // the program: changed bytes, touched, missing
    scd::EntryRecord r = rec;
    const std::string original = slurp(f.program);
    const auto old_time = fs::last_write_time(f.program);
    spit(f.program, original + " ");
    expect(scd::quick_status(r).state == scd::State::stale && scd::quick_status(r).refusal == scd::Refusal::program_changed, "a changed program is stale");
    expect(scd::prepare_launch(r, false).status.refusal == scd::Refusal::program_changed, "the full check refuses a program whose SHA-256 differs");
    spit(f.program, original);
    fs::last_write_time(f.program, old_time + std::chrono::hours(5));
    expect(scd::quick_status(r).state == scd::State::stale, "a touched program is stale at the quick check ...");
    const scd::Plan plan = scd::prepare_launch(r, false);
    expect(plan.status.state == scd::State::ready && plan.program_mtime != r.program.mtime, "... and ready after the full check: the hash is the authority, the new stamp is handed back");
    fs::remove(f.program);
    expect(scd::quick_status(r).refusal == scd::Refusal::program_file_missing && scd::quick_status(r).state == scd::State::stale, "a removed program is stale");
    fs::copy_file(fake, f.program, fs::copy_options::overwrite_existing);
    fs::last_write_time(f.program, old_time);
  }
}

void test_claims_against_files(const fs::path &base, const fs::path &fake) {
  Fixture f(base, fake, "claims");
  const scd::EntryRecord rec = imported(f);
  const std::string bios_digest = sha(bios_bytes);
  const auto try_statement = [&](const std::string &text, const std::string &what, scd::Refusal want) {
    f.describe(text);
    const scd::Plan plan = scd::prepare_launch(rec, true);
    expect(plan.status.refusal == want, what + " (got " + scd::code(plan.status.refusal) + ")");
    scd::ImportRequest req;
    req.bios = f.bios; req.cue = f.cue; req.program = f.program;
    const scd::ImportResult res = scd::import_entry(req);
    expect(!res.program_attached && res.refusal == want, what + ": the same refusal at import, the entry itself is still created");
    return plan;
  };
  try_statement(statement(false, 1, bios_digest, bios_bytes.size()), "a headless-only program", scd::Refusal::program_headless_only);
  expect(scd::prepare_launch(rec, false).status.state == scd::State::headless_only, "headless-only state");
  try_statement(statement(true, 2, bios_digest, bios_bytes.size()), "another runtime contract", scd::Refusal::program_abi_unsupported);
  try_statement(statement(true, 1, std::string(64, 'e'), bios_bytes.size()), "built for another BIOS", scd::Refusal::program_bios_mismatch);
  expect(scd::prepare_launch(rec, false).status.state == scd::State::mismatched, "other-BIOS state is mismatched");
  try_statement(statement(true, 1, bios_digest, bios_bytes.size() + 1), "another BIOS size", scd::Refusal::program_bios_mismatch);
  try_statement(statement(true, 1, bios_digest, bios_bytes.size(), "Other (Track 1).bin"), "a disc file not next to the CUE", scd::Refusal::disc_file_missing);
  try_statement(statement(true, 1, bios_digest, bios_bytes.size(), "Game (Track 1).bin", std::string(64, 'd')), "a disc file with another digest", scd::Refusal::disc_file_changed);
  try_statement(statement(true, 1, bios_digest, bios_bytes.size(), "Game (Track 1).bin", {}, "synthetic"), "a synthetic-profile program", scd::Refusal::program_bios_mismatch);
  try_statement("garbage that is not a statement", "an unparsable statement", scd::Refusal::program_unverifiable);
  fs::remove(fs::path(f.program).concat(".describe"));
  try_statement("", "an older program that rejects --describe", scd::Refusal::program_unverifiable);
  expect(!scd::prepare_launch(rec, false).status.detail.empty(), "an unverifiable program says why");
  f.describe(statement(true, 1, bios_digest, bios_bytes.size()));
  // identity drift: the same file now states a different build identity than the one verified at import
  scd::EntryRecord drift = rec;
  drift.program.program_identity = std::string(64, '1');
  expect(scd::prepare_launch(drift, false).status.refusal == scd::Refusal::program_changed, "a program whose identity differs from the verified one is refused");
  // describe that never answers, and garbage on stdout
  SDL_setenv_unsafe("FAKE_SCD_MODE", "garbage", 1);
  expect(scd::describe_program(f.program).refusal == scd::Refusal::program_unverifiable, "a program printing nonsense is unverifiable");
  SDL_setenv_unsafe("FAKE_SCD_MODE", "hang", 1);
  const Uint64 started = SDL_GetTicks();
  const scd::Claims hung = scd::describe_program(f.program, 400);
  expect(hung.refusal == scd::Refusal::program_unverifiable && SDL_GetTicks() - started < 10000, "a program that never answers is cut off by the time limit and unverifiable");
  SDL_unsetenv_unsafe("FAKE_SCD_MODE");

  {  // replacing the program of a verified disc by one built for different files
    scd::EntryRecord r = rec;
    Fixture g(base, fake, "other");
    g.describe(statement(true, 1, bios_digest, bios_bytes.size()));
    // the other disc's table (renamed track) is checked against r's verified table before any file is read
    const std::string text = statement(true, 1, bios_digest, bios_bytes.size(), "Game (Track 1).bin", sha("different"));
    g.describe(text);
    r.cue_path = u8(g.cue); r.bios_path = u8(g.bios);
    const scd::ImportResult res = scd::attach_program(r, g.program);
    expect(!res.program_attached && res.refusal == scd::Refusal::program_disc_mismatch, "a program built for another disc than the verified one is program_disc_mismatch");
  }
}

void test_run_arguments_and_outcomes(const fs::path &base, const fs::path &fake) {
  Fixture f(base, fake, "run");
  const scd::EntryRecord rec = imported(f);
  const scd::Plan plan = scd::prepare_launch(rec, true);
  expect(plan.status.state == scd::State::ready, "run: ready");
  scd::RunOptions opt;
  opt.report = base / "r.json";
  opt.backup_ram = base / "bram.bin";
  auto args = scd::run_arguments(plan, opt);
  expect(args.size() == 10 && args[6] == "--backup-ram" && args[7] == u8(opt.backup_ram) && args[8] == "--report" && args[9] == u8(opt.report), "run arguments: the formatted backup RAM is passed");
  opt.until_tick = 12345;
  args = scd::run_arguments(plan, opt);
  expect(std::find(args.begin(), args.end(), "--until-tick") != args.end() && *(std::find(args.begin(), args.end(), "--until-tick") + 1) == "12345", "a replay without a window is given its end tick");
  opt.until_tick = 0;
  spit(base / "end.txt", "segarecomp-input-script 1\n1000000 1 press UP\n2000000 1 release UP\n");
  expect(scd::replay_end_tick(base / "end.txt", 10) == 2000000 + 10 * UINT64_C(448020000000) && scd::replay_end_tick(base / "none.txt", 1) == UINT64_C(55929000000) + UINT64_C(448020000000),
         "the end tick of a replay is its last event plus the margin (the first onset plus the margin without events)");
  opt.backup_ram.clear();
  args = scd::run_arguments(plan, opt);
  expect(args.size() == 8 && args[0] == "--disc-dir" && args[1] == u8(f.dir / "disc") && args[2] == "--bios" && args[3] == u8(f.bios) && args[4] == "--instruction-budget" &&
             args[5] == std::to_string(1ULL << 50U) && args[6] == "--report" && args[7] == u8(opt.report),
         "run arguments: disc directory beside the CUE, BIOS, a finite budget, the report");
  opt.record_to = base / "rec.txt";
  args = scd::run_arguments(plan, opt);
  expect(args.size() == 10 && args[8] == "--record-input" && args[9] == u8(opt.record_to), "recording adds --record-input");
  opt.record_to.clear();
  opt.replay_from = base / "rec.txt";
  args = scd::run_arguments(plan, opt);
  expect(args.size() == 10 && args[8] == "--input-script" && args[9] == u8(opt.replay_from), "replay adds --input-script and no recording");
  opt.record_to = base / "other.txt";
  args = scd::run_arguments(plan, opt);
  expect(args.size() == 10 && args[8] == "--record-input", "recording wins when both are given (never a replay that records)");

  spit(base / "stop.json", R"({"result":"stopped","stop":{"class":"entry_not_compiled","cpu":0,"pc":1},"input":{"events":3},"live":{"outcome":"machine_stop","frames":120,"presented":119}})");
  scd::Outcome o = scd::read_outcome(base / "stop.json", 10);
  expect(o.report_read && o.stop_class == "entry_not_compiled" && o.frames == 120 && o.presented == 119 && o.input_events == 3, "outcome: the typed stop class and the counters are read from the report");
  expect(scd::describe_outcome(o).find("entry_not_compiled") != std::string::npos && scd::describe_outcome(o).find("Typed execution stop") != std::string::npos, "outcome text names the typed stop");
  spit(base / "quit.json", R"({"result":"deadline","live":{"outcome":"quit","frames":9,"presented":8}})");
  o = scd::read_outcome(base / "quit.json", 0);
  expect(scd::describe_outcome(o) == "Closed." && !scd::is_refusal_exit(0), "outcome: the user closing the viewer is Closed");
  for (int code : {20, 21, 22, 23, 24, 25, 26, 27}) expect(scd::is_refusal_exit(code) && !scd::describe_outcome(scd::read_outcome({}, code)).empty(), "typed exit " + std::to_string(code) + " has a plain message");
  expect(!scd::is_refusal_exit(10) && !scd::is_refusal_exit(11) && !scd::is_refusal_exit(19) && !scd::is_refusal_exit(28), "stops and budgets are not refusals");
  expect(scd::describe_outcome(scd::read_outcome({}, 139)).find("139") != std::string::npos, "an unknown status is shown as it is");
  spit(base / "bad.json", "{{{");
  expect(!scd::read_outcome(base / "bad.json", 0).report_read, "a damaged report is not trusted");
}

void test_backup_ram(const fs::path &base) {
  const std::vector<std::uint8_t> image = scd::formatted_backup_ram();
  const std::string text(image.begin(), image.end());
  // tools/sega_cd_backup_ram.py formatted_empty(): SHA-256 of the 8 KiB image, computed there from the same published layout
  expect(image.size() == 8192 && sha(text) == "92e9da5c5773baa515ca0f87742eaa8ba8f89167e2d83da4e06fde8dbedf5652", "the formatted backup RAM image is the documented one (8 KiB, SHA-256 of tools/sega_cd_backup_ram.py's image)");
  expect(std::all_of(image.begin(), image.end() - 64, [](std::uint8_t b) { return b == 0; }) && text.substr(8192 - 16) == "RAM_CARTRIDGE___" && text.substr(8192 - 64, 11) == std::string(11, '_'),
         "it is zero except its last 64 bytes: the volume name, the free-block counts and the format signatures");
  const fs::path file = base / "bram" / "backup-ram.bin";
  expect(scd::ensure_backup_ram(file) && slurp(file) == text, "ensure_backup_ram writes the image (and its folder)");
  spit(file, "changed");
  expect(scd::ensure_backup_ram(file) && slurp(file) == text, "a changed file is replaced: it is not user data");
}

void test_catalog(const fs::path &base, const fs::path &fake) {
  const fs::path file = base / "cat" / "library.json";
  fs::create_directories(file.parent_path());
  // 1. a library from before Sega CD loads and re-saves byte for byte (schema 1, no new members)
  spit(file, library_fixture());
  {
    Catalog c(file);
    expect(c.load() == CatalogLoad::loaded && c.entries().size() == 2 && !c.read_only(), "a pre-Sega CD library loads");
    expect(c.save(), "it saves");
    expect(slurp(file) == library_fixture(), "a Genesis + Master System library is re-saved byte for byte (schema 1, no Sega CD members)");
  }
  // 2. adding a Sega CD game raises the schema to 2 and leaves the other records byte-identical
  Fixture f(base, fake, "catfx");
  scd::ImportResult imp;
  const scd::EntryRecord rec = imported(f, &imp);
  {
    Catalog c(file);
    c.load();
    const auto added = c.import_sega_cd(rec, imp.title, 1800000000);
    expect(added.added && added.entry != nullptr && added.entry->platform_id == "sega-cd" && added.entry->sha256 == scd::entry_identity(rec.bios_sha256, rec.cue_sha256), "a Sega CD game is added under its identity");
    expect(added.entry->id() == "sega-cd:" + scd::entry_identity(rec.bios_sha256, rec.cue_sha256) && added.entry->title() == "Game", "entry id and title");
    expect(c.save(), "save with a Sega CD game");
    const std::string text = slurp(file);
    expect(text.find("\"schema\": 2") != std::string::npos, "schema 2 once a Sega CD game exists");
    const std::string head = library_fixture().substr(library_fixture().find("\"games\""));
    const std::string old_games = head.substr(0, head.rfind("\n  ]") );
    expect(text.find(old_games.substr(0, old_games.size())) != std::string::npos, "the Genesis and Master System records are written exactly as before");
    expect(text.find("\"sega_cd\"") != std::string::npos && text.find(json_escaped(u8(f.program))) != std::string::npos, "the Sega CD member holds the references");
    // nothing but references and digests: no file bytes
    expect(text.find("SYNTHETIC") == std::string::npos, "no BIOS, disc or program content is stored");
  }
  {
    Catalog c(file);
    expect(c.load() == CatalogLoad::loaded && c.entries().size() == 3, "reload keeps all three games");
    LibraryEntry *e = c.find("sega-cd", scd::entry_identity(rec.bios_sha256, rec.cue_sha256));
    expect(e != nullptr && e->sega_cd.program.sha256 == rec.program.sha256 && e->sega_cd.disc_verified && e->sega_cd.disc_files.size() == 2 && e->rom_paths.size() == 1 && e->rom_paths[0] == rec.cue_path,
           "the Sega CD record round-trips (references, table, program stamp)");
    const auto again = c.import_sega_cd(scd::import_entry({f.bios, f.cue, {}}).record, "Game", 1800000100);
    expect(!again.added && c.entries().size() == 3 && !again.entry->sega_cd.program.path.empty() && again.entry->sega_cd.disc_verified, "adding the same game again keeps the imported program");
    expect(c.find("genesis", std::string(64, 'a')) != nullptr && c.find("master-system", std::string(64, 'b')) != nullptr, "the earlier games are untouched");
    // a record whose key is not the identity of its digests is dropped, the rest survives
    std::string text = slurp(file);
    const std::string key = scd::entry_identity(rec.bios_sha256, rec.cue_sha256);
    text.replace(text.find(key), key.size(), std::string(64, '9'));
    spit(base / "cat" / "damaged.json", text);
    Catalog d(base / "cat" / "damaged.json");
    expect(d.load() == CatalogLoad::loaded && d.entries().size() == 2, "a Sega CD record with an inconsistent identity is dropped, the library survives");
    // an unknown identity layout is not guessed at
    text = slurp(file);
    text.replace(text.find("\"identity_version\": 1"), 21, "\"identity_version\": 9");
    spit(base / "cat" / "future.json", text);
    Catalog g(base / "cat" / "future.json");
    expect(g.load() == CatalogLoad::loaded && g.entries().size() == 2, "a Sega CD record with an unknown identity layout is dropped");
    // a library from a newer release stays read-only
    text = slurp(file);
    text.replace(text.find("\"schema\": 2"), 11, "\"schema\": 3");
    spit(base / "cat" / "newer.json", text);
    Catalog n(base / "cat" / "newer.json");
    expect(n.load() == CatalogLoad::read_only_newer && n.read_only() && !n.save(), "schema 3 is opened read-only");
    // removing the Sega CD game returns to schema 1, and the file is the pre-change one again
    expect(c.remove("sega-cd:" + key) && c.save() && slurp(file) == library_fixture(), "without a Sega CD game the library is again byte-identical to the pre-change file");
  }
  {  // a record with no program yet
    Catalog c(base / "cat" / "bare.json");
    c.load();
    const auto bare = c.import_sega_cd(scd::import_entry({f.bios, f.cue, {}}).record, "Game", 5);
    expect(bare.added && bare.entry->sega_cd.program.path.empty() && scd::quick_status(bare.entry->sega_cd).state == scd::State::no_program, "an entry without a program is stored and shows no_program");
    expect(!c.import_sega_cd(scd::EntryRecord{}, "x", 1).entry && !c.import_sega_cd(scd::EntryRecord{}, "x", 1).error.empty(), "an entry without BIOS and CUE digests is refused");
  }
}

// SEG-097-T003 (ADR 0208): the EXPERIMENTAL Sega CD policy at the launcher - an explicit choice that is off by default, passed to the product as its own
// option, read back from the product's own report, and persisted without touching any record that never chose it.
void test_experimental_policy(const fs::path &base, const fs::path &fake) {
  expect(scd_build_policy_arguments(ScdBuildMode::compatibility).empty(), "policy: Compatibility passes no policy option to the product (the legacy command)");
  expect((scd_build_policy_arguments(ScdBuildMode::optimized) == std::vector<std::string>{"--aot-policy", "optimized", "--aot-model", "gen2"}), "policy: the experimental choice is the product's own option, nothing else");
  expect((scd_build_policy_arguments(ScdBuildMode::file_first) == std::vector<std::string>{"--file-first", "structural"}), "file-first: the product's own option, and no aot policy option");
  {
    const auto ff = scd_build_policy_arguments(ScdBuildMode::file_first), op = scd_build_policy_arguments(ScdBuildMode::optimized);
    bool overlap = false;
    for (const auto &a : ff) for (const auto &b : op) overlap = overlap || a == b;
    expect(!overlap && std::find(ff.begin(), ff.end(), "--aot-policy") == ff.end() && std::find(op.begin(), op.end(), "--file-first") == op.end(),
           "file-first and Optimized are one choice: no request carries both option families (the product CLI refuses the pair)");
  }
  expect(ScdBuildRequest{}.mode == ScdBuildMode::compatibility, "policy: a build request is Compatibility unless another mode is chosen");
  const std::string selective =
      "{\"status\":\"ok\",\"aot_policy\":{\"requested\":\"optimized\",\"experimental\":true,\"effective\":\"selective\",\"fallback\":false,\"reason\":\"none\",\"failed_image\":\"\","
      "\"k_sha256\":\"ab\",\"broad_entries\":10439,\"admitted_entries\":9280,\"m3_images\":1,\"m3_broad_entries\":1865,\"m3_admitted_entries\":706,\"images\":[{\"name\":\"bios\",\"broad_entries\":1}]},\"frontier\":null}";
  const auto sel = read_scd_policy_report(selective);
  expect(sel.present && sel.requested == "optimized" && sel.effective == "selective" && !sel.fallback && sel.broad_entries == 10439 && sel.admitted_entries == 9280 && sel.m3_images == 1 && sel.m3_admitted_entries == 706,
         "policy: the product's aot_policy member is read (requested, effective, no fallback, counts)");
  expect(scd_policy_sentence(sel).find("706 of the 1865") != std::string::npos && scd_policy_sentence(sel).find("Experimental") != std::string::npos &&
             scd_policy_sentence(sel).find("uncompiled entry") != std::string::npos && scd_policy_sentence(sel).find("BIOS and the game's boot code stay fully compiled") != std::string::npos,
         "policy: the summary says it is experimental, how much it compiled, and that it can stop at an uncompiled entry");
  const std::string fallback =
      "{\"aot_policy\":{\"requested\":\"optimized\",\"experimental\":true,\"effective\":\"broad\",\"fallback\":true,\"reason\":\"rom_size\",\"failed_image\":\"main:bios\","
      "\"broad_entries\":10,\"admitted_entries\":0,\"images\":[]}}";
  const auto fb = read_scd_policy_report(fallback);
  expect(fb.fallback && fb.effective == "broad" && fb.reason == "rom_size" && scd_policy_sentence(fb).find("fell back: rom_size") != std::string::npos &&
             scd_policy_sentence(fb).find("Experimental Optimized: the program") == std::string::npos,
         "policy: a fallback is stated as Compatibility with the exact reason, never as an experimental result");
  const auto compat = read_scd_policy_report("{\"aot_policy\":{\"requested\":\"compatibility\",\"effective\":\"broad\",\"fallback\":false,\"reason\":\"none\",\"images\":[]}}");
  expect(compat.present && scd_policy_sentence(compat).empty() && !read_scd_policy_report("{\"status\":\"ok\"}").present && scd_policy_sentence(read_scd_policy_report("{}")).empty(),
         "policy: a Compatibility build (or an older product without the member) says nothing about the experiment");
  expect(scd::is_uncompiled_entry_stop("entry_not_compiled") && scd::is_uncompiled_entry_stop("image_entry_not_compiled") && !scd::is_uncompiled_entry_stop("pcm_unsupported") &&
             !scd::is_uncompiled_entry_stop("") && !scd::is_uncompiled_entry_stop("code_bytes_mismatch"),
         "policy: only the uncompiled-entry stops offer the Compatibility rebuild");
  // SEG-103-T007: file-first is read from status.json.file_first, independently of aot_policy
  const std::string file_first_status =
      "{\"status\":\"ok\",\"aot_policy\":{\"requested\":\"compatibility\",\"effective\":\"broad\",\"fallback\":false,\"reason\":\"none\",\"images\":[]},"
      "\"file_first\":{\"policy\":\"file_first.v1:structural:max_bytes=524288:max_offsets=0\",\"files\":131,\"oversize_files\":4,\"duplicate_files\":4,\"no_structure_files\":2,\"complete\":false,"
      "\"images\":120,\"compiled_offsets\":1536888,\"refused_reasons\":{}},\"frontier\":null}";
  const auto ffr = read_scd_file_first_report(file_first_status);
  expect(ffr.present && !ffr.complete && ffr.files == 131 && ffr.images == 120 && ffr.oversize_files == 4 && ffr.no_structure_files == 2 && ffr.compiled_offsets == 1536888,
         "file-first: the product's file_first member is read (counts, complete = false)");
  expect(scd_effective_build_mode(file_first_status) == "file_first" && read_scd_policy_report(file_first_status).requested == "compatibility",
         "file-first: the effective mode comes from file_first although the aot_policy member says compatibility");
  expect(scd_file_first_sentence(ffr).find("incomplete") != std::string::npos && scd_file_first_sentence(ffr).find("uncompiled entry") != std::string::npos &&
             scd_file_first_sentence(ffr).find("every eligible file") == std::string::npos,
         "file-first: an incomplete inventory is never described as complete");
  expect(scd_effective_build_mode(selective) == "optimized" && scd_effective_build_mode(fallback) == "compatibility" && scd_effective_build_mode("{\"status\":\"ok\"}") == "compatibility" &&
             !read_scd_file_first_report("{\"status\":\"ok\",\"file_first\":null}").present && !read_scd_file_first_report("{\"file_first\":{\"files\":1").present,
         "file-first: null, absent or malformed file_first claims nothing; Optimized and fallback read as before");
  expect(std::string(scd_build_mode_code(ScdBuildMode::file_first)) == "file_first" && std::string(scd::selected_build_mode(scd::EntryRecord{})) == "compatibility",
         "file-first: stable mode codes; a record is Compatibility unless it chose otherwise");
  {
    scd::EntryRecord r;
    scd::select_build_mode(r, "file_first");
    expect(r.file_first && !r.experimental_m3 && std::string(scd::selected_build_mode(r)) == "file_first", "file-first: selecting it");
    scd::select_build_mode(r, "optimized");
    expect(!r.file_first && r.experimental_m3, "file-first: selecting Optimized clears file-first (never both)");
    scd::select_build_mode(r, "compatibility");
    expect(!r.file_first && !r.experimental_m3, "file-first: selecting Compatibility clears both");
  }
  // persistence: written only when chosen, kept when the game is added again, byte-identical records otherwise
  Fixture f(base, fake, "policyfx");
  scd::ImportResult imp;
  const scd::EntryRecord rec = imported(f, &imp);
  const fs::path file = base / "policyfx-catalog.json";
  fs::remove(file);
  {
    Catalog c(file);
    c.load();
    auto added = c.import_sega_cd(rec, "Game", 1800000000);
    expect(added.entry != nullptr && !added.entry->sega_cd.experimental_m3 && !added.entry->sega_cd.program_experimental, "policy: a new game is Compatibility");
    expect(c.save() && slurp(file).find("experimental") == std::string::npos, "policy: a record that never chose the option is written without the members");
    expect(slurp(file).find("file_first") == std::string::npos && slurp(file).find("program_built_mode") == std::string::npos, "file-first: a record that never chose it is byte-compatible (no new members)");
    added.entry->sega_cd.experimental_m3 = true;
    added.entry->sega_cd.program_experimental = true;
    expect(c.save() && slurp(file).find("\"experimental_m3\": true") != std::string::npos, "policy: the choice and the program's effective policy are persisted");
  }
  {
    Catalog c(file);
    expect(c.load() == CatalogLoad::loaded, "policy: reload");
    LibraryEntry *e = c.find("sega-cd", scd::entry_identity(rec.bios_sha256, rec.cue_sha256));
    expect(e != nullptr && e->sega_cd.experimental_m3 && e->sega_cd.program_experimental, "policy: the choice survives a reload");
    const auto again = c.import_sega_cd(scd::import_entry({f.bios, f.cue, {}}).record, "Game", 1800000100);
    expect(again.entry->sega_cd.experimental_m3 && again.entry->sega_cd.program_experimental && !again.entry->sega_cd.program.path.empty(),
           "policy: adding the same game again keeps the user's choice and the program's policy");
    expect(again.entry->sega_cd.program_built_mode == "optimized", "migration: a record written by an earlier release (program_experimental only) reads as an Optimized-built program");
  }
  {   // SEG-103-T007: the file-first selection and the built mode persist and migrate
    const fs::path file2 = base / "policyfx-catalog2.json";
    fs::remove(file2);
    {
      Catalog c(file2);
      c.load();
      auto added = c.import_sega_cd(rec, "Game", 1800000000);
      scd::select_build_mode(added.entry->sega_cd, "file_first");
      added.entry->sega_cd.program_built_mode = "compatibility";
      expect(c.save() && slurp(file2).find("\"file_first\": true") != std::string::npos && slurp(file2).find("\"program_built_mode\": \"compatibility\"") != std::string::npos &&
                 slurp(file2).find("experimental_m3") == std::string::npos,
             "file-first: the selection and the (different) built mode are persisted separately");
    }
    {
      Catalog c(file2);
      expect(c.load() == CatalogLoad::loaded, "file-first: reload");
      LibraryEntry *e = c.find("sega-cd", scd::entry_identity(rec.bios_sha256, rec.cue_sha256));
      expect(e != nullptr && e->sega_cd.file_first && !e->sega_cd.experimental_m3 && e->sega_cd.program_built_mode == "compatibility" && std::string(scd::selected_build_mode(e->sega_cd)) == "file_first",
             "file-first: the selection survives a reload and is not reinterpreted as the built mode");
      const auto again = c.import_sega_cd(scd::import_entry({f.bios, f.cue, {}}).record, "Game", 1800000100);
      expect(again.entry->sega_cd.file_first && again.entry->sega_cd.program_built_mode == "compatibility", "file-first: adding the game again keeps the selection and how the program was built");
      e->sega_cd.program.mtime = -4644358762LL;   // a non-Unix file-clock epoch gives a negative stamp; it must survive a reload (else a reopened launcher calls a built program changed)
      e->sega_cd.program_built_mode = "file_first";
      e->sega_cd.program_file_first_inventory = "incomplete";
      expect(c.save() && slurp(file2).find("\"program_file_first_inventory\": \"incomplete\"") != std::string::npos, "file-first: an incomplete inventory is persisted as incomplete");
      {
        Catalog again(file2);
        again.load();
        LibraryEntry *g = again.find("sega-cd", scd::entry_identity(rec.bios_sha256, rec.cue_sha256));
        expect(g != nullptr && g->sega_cd.program.mtime == -4644358762LL && g->sega_cd.program_built_mode == "file_first" && g->sega_cd.program_file_first_inventory == "incomplete",
               "file-first: a negative import stamp and the built mode round-trip");
      }
    }
    {   // hand-edited and invalid values
      std::string text = slurp(file2);
      const auto at = text.find("\"file_first\": true");
      text.replace(at, 18, "\"experimental_m3\": true, \"file_first\": true");
      const auto built = text.find("\"program_built_mode\": \"file_first\"");
      text.replace(built, 34, "\"program_built_mode\": \"bogus\"");
      std::ofstream(file2, std::ios::binary) << text;
      Catalog c(file2);
      expect(c.load() == CatalogLoad::loaded, "file-first: reload of a hand-edited record");
      LibraryEntry *e = c.find("sega-cd", scd::entry_identity(rec.bios_sha256, rec.cue_sha256));
      expect(e != nullptr && e->sega_cd.experimental_m3 && !e->sega_cd.file_first && std::string(scd::selected_build_mode(e->sega_cd)) == "optimized" && e->sega_cd.program_built_mode.empty() &&
                 e->sega_cd.program_file_first_inventory.empty(),
             "file-first: both flags read as Optimized, an unknown built mode reads as unknown, an inventory without a file-first program is dropped");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) { std::cerr << "usage: launcher_sega_cd_test <fake_scd_program>\n"; return 2; }
  const fs::path fake = fs::absolute(argv[1]);
  const fs::path base = fs::temp_directory_path() / "segarecomp_launcher_sega_cd_test";
  fs::remove_all(base);
  fs::create_directories(base);
  SDL_setenv_unsafe("FAKE_SCD_MODE", "", 1);
  SDL_unsetenv_unsafe("FAKE_SCD_MODE");
  test_identity_and_hash(base);
  test_claims();
  test_import_and_status(base, fake);
  test_claims_against_files(base, fake);
  test_run_arguments_and_outcomes(base, fake);
  test_backup_ram(base);
  test_catalog(base, fake);
  test_experimental_policy(base, fake);
  std::cout << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
