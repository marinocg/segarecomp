// SEG-138-T009 (ADR 0300): the scheduled-mod seam of a HEADLESS generated-native program. `segarecomp build --luau-package` (without a viewer) links this file, the Luau VM, the ONE mod scheduler
// (mod_scheduler.c) and a constant artifact (luau_artifact.c: compiled scripts, identity, granted capabilities, the declared memory ranges of the exact source revision) into the program. The headless
// frame capture (frame_capture.c, built with -DSEGARECOMP_CAPTURE_MOD) calls begin/checkpoint/end around its own run loop, so `mod diff-run` and `mod test` observe the SAME guest execution the
// scheduler drives: frame hashes, work-RAM region samples, and one MOD_SUMMARY line (fault state, replay witness, overlay digest).
//
// Nothing here reaches outside the scheduler: every guest access is a scheduler service checked against the declared ranges; a fault disables only the mod and the guest carries on. Strict determinism:
// no clock, no host RNG. The optional SEGARECOMP_MOD_CHECKPOINT=<frame> performs a save/restore round trip (capture, restore into the same machine, capture again) at the first slice boundary at or
// after that guest frame; the run then continues, so equal final digests with and without it prove the restore lost nothing.
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_phase_provider.hpp"
#include "segarecomp/sha256.hpp"

extern "C" {
#include "frame_capture.h"
#include "genesis_snapshot.h"
#include "mod_scheduler.h"
#include "runtime.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

extern "C" {
extern const unsigned char segarecomp_luau_artifact[];
extern const unsigned long segarecomp_luau_artifact_size;
extern const char segarecomp_luau_identity[];
extern const char segarecomp_luau_bytecode_digest[];
extern const char segarecomp_luau_capabilities[];
extern const unsigned long segarecomp_luau_memory_bytes, segarecomp_luau_budget_units;
extern const char segarecomp_luau_mod_id[];
// The scheduled-path constants (written by the build next to the artifact).
extern const char segarecomp_mod_effective_sha256[];
extern const unsigned long segarecomp_mod_state_schema;
extern const unsigned long segarecomp_mod_range_count;
extern const unsigned long segarecomp_mod_ranges[][4];   // space, offset, length, modes
}

namespace {

using namespace segarecomp;

struct Mod {
  std::unique_ptr<luau::PhaseProvider> provider;
  std::unique_ptr<GenesisModScheduler> scheduler;
  GenesisSnapshotModState snapshot{};
  GenesisModRange ranges[GENESIS_MOD_RANGES_MAX]{};
  std::string fault_text;
  bool attached = false;
  bool refused = false;
  std::string refusal;
  // round trip
  std::uint64_t checkpoint_frame = 0;
  bool checkpoint_wanted = false, checkpoint_done = false, checkpoint_identical = false, checkpoint_restored = false;
  std::string checkpoint_status, checkpoint_digest, save_path, loaded_digest;
  bool loaded = false;
  std::uint8_t last_overlay_digest[32]{};
  bool have_overlay = false;
  std::uint64_t overlay_ops = 0;
};

Mod g;

std::string hex(const std::uint8_t *p, std::size_t n) {
  static const char d[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) { out += d[p[i] >> 4U]; out += d[p[i] & 15U]; }
  return out;
}

std::string json_escaped(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) ? '?' : c;
  }
  return out;
}

void log_sink(void *, const char *line, std::uint32_t length) {
  std::fprintf(stderr, "MOD_LOG {\"line\":\"%s\"}\n", json_escaped(std::string(line, length)).c_str());
}

void identity_bytes(std::uint8_t out[32]) {
  for (unsigned i = 0; i < 32U; ++i) out[i] = static_cast<std::uint8_t>(std::strtoul(std::string(segarecomp_luau_identity + i * 2U, 2U).c_str(), nullptr, 16));
}

void snapshot_identity(GenesisSnapshotIdentity &id) {
  std::memset(&id, 0, sizeof id);
  for (unsigned i = 0; i < 32U; ++i) id.source_sha256[i] = static_cast<std::uint8_t>(std::strtoul(std::string(segarecomp_mod_effective_sha256 + i * 2U, 2U).c_str(), nullptr, 16));
  identity_bytes(id.variant_sha256);
}

}  // namespace

extern "C" {

// 0 = the mod is attached (or there is none to attach); nonzero = refused (the program must not run a half-configured mod: the caller exits).
int segarecomp_capture_mod_begin(GenesisRuntime *runtime) {
  const std::span<const std::uint8_t> stored(segarecomp_luau_artifact, static_cast<std::size_t>(segarecomp_luau_artifact_size));
  const luau::CompileResult loaded = luau::load_stored(segarecomp_luau_identity, stored, segarecomp_luau_bytecode_digest);
  if (!loaded.ok) { g.refused = true; g.refusal = loaded.rejection.code; std::fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", loaded.rejection.code.c_str()); return 1; }
  luau::HostConfig host;
  for (const char *p = segarecomp_luau_capabilities; *p != '\0';) {
    const char *end = std::strchr(p, ',');
    const std::string name = end != nullptr ? std::string(p, end) : std::string(p);
    if (!name.empty()) host.capabilities.push_back(name);
    if (end == nullptr) break;
    p = end + 1;
  }
  luau::Limits limits = luau::policy_limits();
  if (segarecomp_luau_memory_bytes != 0 && segarecomp_luau_memory_bytes < limits.memory_bytes) limits.memory_bytes = segarecomp_luau_memory_bytes;
  if (segarecomp_luau_budget_units != 0 && segarecomp_luau_budget_units < limits.callback_units) limits.callback_units = segarecomp_luau_budget_units;
  std::string error;
  g.provider = luau::PhaseProvider::create(segarecomp_luau_identity, loaded.compiled, limits, host, [](const char *cause) { genesis_mod_fault_current(cause); }, error);
  if (!g.provider) { g.refused = true; g.refusal = error; std::fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", error.c_str()); return 1; }
  g.scheduler = std::make_unique<GenesisModScheduler>();
  GenesisModConfig c{};
  c.mod_id = segarecomp_luau_mod_id;
  c.provider = g.provider->provider();
  c.provider_state = g.provider->state();
  c.phase_mask = g.provider->phase_mask();
  g.provider->capabilities(c.required);
  for (unsigned long i = 0; i < segarecomp_mod_range_count && i < GENESIS_MOD_RANGES_MAX; ++i)
    g.ranges[i] = GenesisModRange{static_cast<std::uint32_t>(segarecomp_mod_ranges[i][0]), static_cast<std::uint32_t>(segarecomp_mod_ranges[i][1]), static_cast<std::uint32_t>(segarecomp_mod_ranges[i][2]), static_cast<std::uint32_t>(segarecomp_mod_ranges[i][3])};
  c.ranges = g.ranges;
  c.range_count = static_cast<std::uint32_t>(segarecomp_mod_range_count);
  c.state_schema = static_cast<std::uint32_t>(segarecomp_mod_state_schema);
  c.log_sink = log_sink;
  identity_bytes(c.identity_sha256);
  const char *why = "";
  if (genesis_mod_scheduler_attach(g.scheduler.get(), runtime, &c, &why) != 0) {
    g.refused = true; g.refusal = why;
    std::fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", why);
    return 1;
  }
  g.attached = true;
  genesis_mod_snapshot_state(g.scheduler.get(), &g.snapshot);
  if (const char *text = std::getenv("SEGARECOMP_MOD_CHECKPOINT"); text != nullptr && *text != '\0') {
    char *end = nullptr;
    g.checkpoint_frame = std::strtoull(text, &end, 10);
    g.checkpoint_wanted = end != nullptr && *end == '\0';
  }
  if (const char *path = std::getenv("SEGARECOMP_MOD_SAVE"); path != nullptr && *path != '\0') g.save_path = path;   // the checkpoint's container is also written here (a fresh program can load it)
  if (const char *path = std::getenv("SEGARECOMP_MOD_LOAD"); path != nullptr && *path != '\0') {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    GenesisSnapshotMachine machine{};
    machine.runtime = runtime;
    machine.mod = &g.snapshot;
    GenesisSnapshotIdentity id;
    snapshot_identity(id);
    const GenesisSnapshotStatus st = bytes.empty() ? GENESIS_SNAPSHOT_TRUNCATED : genesis_snapshot_restore(&machine, &id, bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    if (st != GENESIS_SNAPSHOT_OK) {
      g.refused = true; g.refusal = std::string("E_STATE_RESTORE:") + genesis_snapshot_status_text(st);
      std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_STATE_RESTORE\",\"status\":\"%s\"}\n", genesis_snapshot_status_text(st));
      return 1;
    }
    g.loaded = true;
    g.loaded_digest = hex(bytes.data() + bytes.size() - 32U, 32U);
  }
  std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"package_identity\":\"%s\",\"bytecode_digest\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", segarecomp_luau_mod_id, segarecomp_luau_identity,
               segarecomp_luau_bytecode_digest, segarecomp_luau_capabilities, segarecomp_mod_range_count);
  return 0;
}

// Between runner slices: the optional save/restore round trip.
void segarecomp_capture_mod_checkpoint(GenesisRuntime *runtime) {
  if (!g.attached || !g.checkpoint_wanted || g.checkpoint_done || runtime->virtual_frame_count < g.checkpoint_frame) return;
  g.checkpoint_done = true;
  GenesisSnapshotMachine machine{};
  machine.runtime = runtime;
  machine.mod = &g.snapshot;
  GenesisSnapshotIdentity id;
  snapshot_identity(id);
  GenesisSnapshotBuffer first{nullptr, 0}, second{nullptr, 0};
  GenesisSnapshotStatus st = genesis_snapshot_capture(&machine, &id, nullptr, &first);
  if (st != GENESIS_SNAPSHOT_OK) { g.checkpoint_status = std::string("capture:") + genesis_snapshot_status_text(st); return; }
  st = genesis_snapshot_restore(&machine, &id, first.data, first.size);
  g.checkpoint_restored = st == GENESIS_SNAPSHOT_OK;
  if (!g.checkpoint_restored) { g.checkpoint_status = std::string("restore:") + genesis_snapshot_status_text(st); genesis_snapshot_buffer_free(&first); return; }
  st = genesis_snapshot_capture(&machine, &id, nullptr, &second);
  g.checkpoint_identical = st == GENESIS_SNAPSHOT_OK && first.size == second.size && std::memcmp(first.data, second.data, first.size) == 0;
  g.checkpoint_status = g.checkpoint_identical ? "ok" : "differs";
  g.checkpoint_digest = hex(first.data + first.size - 32U, 32U);
  if (!g.save_path.empty()) {
    std::ofstream out(g.save_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(first.data), static_cast<std::streamsize>(first.size));
    if (!out) g.checkpoint_status = "save_failed";
  }
  genesis_snapshot_buffer_free(&first);
  genesis_snapshot_buffer_free(&second);
}

// After the run: the MOD_SUMMARY line and the detach.
void segarecomp_capture_mod_end(GenesisRuntime *runtime) {
  (void)runtime;
  if (!g.attached) return;
  GenesisModScheduler &s = *g.scheduler;
  std::uint8_t digest[32];
  genesis_mod_write_log_digest(&s, digest);
  const luau::OverlayImage image = luau::rasterize_overlay(g.provider->overlay());
  const std::string overlay_hex = g.provider->overlay().empty() ? std::string() : (sha256_hex(std::span<const std::uint8_t>(image.rgba.data(), image.rgba.size())));
  std::fprintf(stderr,
               "MOD_SUMMARY {\"mod\":\"%s\",\"faulted\":%s,\"fault\":\"%s\",\"callbacks\":%llu,\"committed_writes\":%llu,\"write_digest\":\"%s\",\"overlay_ops\":%llu,\"overlay_sha256\":\"%s\","
               "\"nondeterministic\":%s,\"roundtrip\":{\"requested\":%s,\"frame\":%llu,\"status\":\"%s\",\"state_sha256\":\"%s\"},\"loaded_state_sha256\":\"%s\"}\n",
               segarecomp_luau_mod_id, s.fault.faulted != 0U ? "true" : "false", s.fault.faulted != 0U ? json_escaped(s.fault.text).c_str() : "", static_cast<unsigned long long>(s.callbacks_run),
               static_cast<unsigned long long>(s.write_total), hex(digest, 32).c_str(), static_cast<unsigned long long>(g.provider->overlay().size()), overlay_hex.c_str(),
               g.provider->nondeterministic() ? "true" : "false", g.checkpoint_wanted ? "true" : "false", static_cast<unsigned long long>(g.checkpoint_frame),
               g.checkpoint_wanted ? (g.checkpoint_done ? g.checkpoint_status.c_str() : "not_reached") : "none", g.checkpoint_digest.c_str(), g.loaded_digest.c_str());
  genesis_mod_scheduler_detach(&s);
}

}  // extern "C"
