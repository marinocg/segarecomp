// SEG-138-T011 (ADR 0305): the same file serves a NATIVE ABI 2 provider (-DSEGARECOMP_MOD_NATIVE: no Luau VM; the provider is the package's compiled C11 source, wrapped so its render output is committed only
// when the callback did not fault) and, with -DSEGARECOMP_MOD_VIEWER, the played SDL3 program (SEG-138-T010) -- one scheduler adapter for every route, never a second lifecycle.
//
// SEG-138-T009 (ADR 0300): the scheduled-mod seam of a HEADLESS generated-native program. `segarecomp build --luau-package` (without a viewer) links this file, the Luau VM, the ONE mod scheduler
// (mod_scheduler.c) and a constant artifact (luau_artifact.c: compiled scripts, identity, granted capabilities, the declared memory ranges of the exact source revision) into the program. The headless
// frame capture (frame_capture.c, built with -DSEGARECOMP_CAPTURE_MOD) calls begin/checkpoint/end around its own run loop, so `mod diff-run` and `mod test` observe the SAME guest execution the
// scheduler drives: frame hashes, work-RAM region samples, and one MOD_SUMMARY line (fault state, replay witness, overlay digest).
//
// Nothing here reaches outside the scheduler: every guest access is a scheduler service checked against the declared ranges; a fault disables only the mod and the guest carries on. Strict determinism:
// no clock, no host RNG. The optional SEGARECOMP_MOD_CHECKPOINT=<frame> performs a save/restore round trip (capture, restore into the same machine, capture again) at the first slice boundary at or
// after that guest frame; the run then continues, so equal final digests with and without it prove the restore lost nothing.
#include "segarecomp/luau_api.hpp"
#include "segarecomp/sha256.hpp"
#ifndef SEGARECOMP_MOD_NATIVE
#include "segarecomp/luau_phase_provider.hpp"
#endif

#ifdef SEGARECOMP_MOD_VIEWER
#include "luau_hud_pack.hpp"   // SEG-138-T010: the viewer presents the scheduled mod's render output through the existing compatibility seam
#endif

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
#ifdef SEGARECOMP_MOD_NATIVE
extern const char segarecomp_mod_identity[];             // package identity (64 hex)
extern const char segarecomp_mod_capabilities[];         // comma separated catalogue names the build granted
extern const char segarecomp_mod_id[];
extern const unsigned long long segarecomp_mod_caps_required[2], segarecomp_mod_caps_optional[2];   // bitsets by capability id
const SegaModProvider *segarecomp_mod_provider(void **state_out);                                    // the package's ABI 2 entry symbol
#define MOD_IDENTITY segarecomp_mod_identity
#define MOD_CAPABILITIES segarecomp_mod_capabilities
#define MOD_ID segarecomp_mod_id
#else
extern const unsigned char segarecomp_luau_artifact[];
extern const unsigned long segarecomp_luau_artifact_size;
extern const char segarecomp_luau_identity[];
extern const char segarecomp_luau_bytecode_digest[];
extern const char segarecomp_luau_capabilities[];
extern const unsigned long segarecomp_luau_memory_bytes, segarecomp_luau_budget_units;
extern const char segarecomp_luau_mod_id[];
#define MOD_IDENTITY segarecomp_luau_identity
#define MOD_CAPABILITIES segarecomp_luau_capabilities
#define MOD_ID segarecomp_luau_mod_id
#endif
// The scheduled-path constants (written by the build next to the artifact).
extern const char segarecomp_mod_effective_sha256[];
extern const unsigned long segarecomp_mod_state_schema;
extern const unsigned long segarecomp_mod_range_count;
extern const unsigned long segarecomp_mod_ranges[][4];   // space, offset, length, modes
#if defined(SEGARECOMP_MOD_NATIVE) && defined(SEGARECOMP_MOD_HOOKS)
// SEG-138-T012 (ADR 0308): the function hooks of the built program: the artifact's bindings (generated site id, the hook id the provider is called with), the hook phases it registers, and the generated site table.
extern const unsigned long segarecomp_mod_hook_count, segarecomp_mod_hook_phases;
struct segarecomp_mod_hook_binding { const char *id; unsigned long hook_id; };
extern const segarecomp_mod_hook_binding segarecomp_mod_hooks[];
const GenesisHookSiteInfo *genesis_generated_hook_sites(size_t *count);
#endif
}

namespace {

using namespace segarecomp;

struct Mod {
#ifndef SEGARECOMP_MOD_NATIVE
  std::unique_ptr<luau::PhaseProvider> provider;
#else
  SegaModProvider wrapper{};                 // the package's provider table with `phase` replaced (render output commit)
  void (*native_phase_entry)(void *, const SegaModHost *) = nullptr;
  luau::Output staged, visible;              // the render callback in flight / the last committed render output
  int target = 0;
  // SEG-140-T010: the typed virtual_sprites feature submits a whole GenesisVirtualSpriteSet (the one transport every route ends in); staged during the callback, committed with the overlay when it did not fault
  std::unique_ptr<GenesisVirtualSpriteSet> staged_set, visible_set;
  bool have_staged_set = false, have_visible_set = false;
  // SEG-140-T011 (ADR 0309): the typed scenery_window feature. One bounded name-table window per plane, staged during the render callback, committed with it, bound to the one frame it was rendered for.
  struct SceneryWindow {
    bool present = false;
    std::int32_t x0 = 0, y0 = 0;
    std::uint32_t cols = 0, rows = 0;
    std::vector<std::uint16_t> words;
  };
  SceneryWindow staged_scenery[2], visible_scenery[2];
  std::uint64_t visible_scenery_frame = 0;   // the guest frame whose render callback committed `visible_scenery`
#endif
  std::unique_ptr<GenesisModScheduler> scheduler;
  GenesisSnapshotModState snapshot{};
  GenesisModRange ranges[GENESIS_MOD_RANGES_MAX]{};
  GenesisModHookBinding bindings[GENESIS_MOD_HOOKS_MAX]{};   // SEG-138-T012: must outlive the scheduler
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
  std::uint32_t checkpoint_depth = 0;   // SEG-138-T012: hook call frames in flight when the round trip was taken
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
  for (unsigned i = 0; i < 32U; ++i) out[i] = static_cast<std::uint8_t>(std::strtoul(std::string(MOD_IDENTITY + i * 2U, 2U).c_str(), nullptr, 16));
}

void snapshot_identity(GenesisSnapshotIdentity &id) {
  std::memset(&id, 0, sizeof id);
  for (unsigned i = 0; i < 32U; ++i) id.source_sha256[i] = static_cast<std::uint8_t>(std::strtoul(std::string(segarecomp_mod_effective_sha256 + i * 2U, 2U).c_str(), nullptr, 16));
  identity_bytes(id.variant_sha256);
}


// The last committed render output of the attached mod, whichever route produced it.
struct RenderView {
  const std::vector<luau::OverlayOp> *ops;
  const std::vector<luau::OverlayLayerDecl> *layers;
  const std::string *scene;
  const luau::SpriteSubmission *sprites;
  const GenesisVirtualSpriteSet *set;   // SEG-140-T010: the native route's submission, already in the shared transport (null for the Luau route and when none was submitted)
};
RenderView render_view() {
#ifdef SEGARECOMP_MOD_NATIVE
  return {&g.visible.overlay, &g.visible.layers, &g.visible.scene, &g.visible.sprites, g.have_visible_set ? g.visible_set.get() : nullptr};
#else
  return {&g.provider->overlay(), &g.provider->layers(), &g.provider->scene(), &g.provider->sprites(), nullptr};
#endif
}

#ifdef SEGARECOMP_MOD_NATIVE
// ---- SEG-138-T011: the typed ABI 2 render API (SegaGenesisRenderApi). The rules are luau_api.cpp's (shared with the Luau bindings); a refusal is a fault of the callback with the scheduler's stable cause. ----
bool in_render() { return g.scheduler != nullptr && g.scheduler->in_callback != 0U && g.scheduler->phase == SEGA_MOD_PHASE_RENDER; }
int refused_phase() { genesis_mod_fault_current("phase"); return SEGA_MOD_E_PHASE; }
int answer(const luau::OverlayCall &r) {
  if (r.status == luau::OverlayCall::Status::ok) return SEGA_MOD_OK;
  const bool budget = r.status == luau::OverlayCall::Status::budget;
  genesis_mod_fault_current(budget ? "budget" : "range");
  return budget ? SEGA_MOD_E_BUDGET : SEGA_MOD_E_RANGE;
}
int api_overlay_layer(std::uint32_t index, std::uint32_t anchor, std::uint32_t reference, std::int32_t dx, std::int32_t dy, std::uint32_t w, std::uint32_t h, std::uint32_t z) {
  if (!in_render()) return refused_phase();
  luau::OverlayLayerDecl d;
  d.index = static_cast<int>(index > 1000U ? 1000U : index);
  d.anchor = static_cast<int>(anchor > 1000U ? 1000U : anchor);
  d.reference = static_cast<int>(reference > 1000U ? 1000U : reference);
  d.dx = dx;
  d.dy = dy;
  d.w = static_cast<int>(w > 100000U ? 100000U : w);
  d.h = static_cast<int>(h > 100000U ? 100000U : h);
  d.z = static_cast<int>(z > 1000U ? 1000U : z);
  return answer(luau::overlay_declare_layer(g.staged, d));
}
int api_overlay_target(std::uint32_t index) { return in_render() ? answer(luau::overlay_select_target(g.staged, g.target, index)) : refused_phase(); }
int api_overlay_rect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba) {
  return in_render() ? answer(luau::overlay_rect(g.staged, g.target, x, y, w, h, rgba)) : refused_phase();
}
int api_overlay_text(std::int32_t x, std::int32_t y, const char *ascii, std::uint32_t len, std::uint32_t rgba) {
  if (!in_render()) return refused_phase();
  if (ascii == nullptr && len != 0U) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }
  return answer(luau::overlay_text(g.staged, g.target, x, y, std::string_view(ascii != nullptr ? ascii : "", len), rgba));
}
int api_scene(const char *id, std::uint32_t len) {
  if (!in_render()) return refused_phase();
  if (id == nullptr) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }
  return answer(luau::overlay_scene(g.staged, std::string_view(id, len)));
}
int api_virtual_sprites(const GenesisVirtualSpriteSet *set) {
  if (!in_render()) return refused_phase();
  if (set == nullptr || g.have_staged_set) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }   // one set per callback, and a set is required
  if (!g.staged_set) g.staged_set = std::make_unique<GenesisVirtualSpriteSet>();
  std::memcpy(g.staged_set.get(), set, sizeof *set);
  g.have_staged_set = true;
  return SEGA_MOD_OK;
}
constexpr std::uint32_t kSceneryColsMax = 128U, kSceneryRowsMax = 64U, kSceneryWordsMax = 4096U;
int api_scenery_window(std::uint32_t plane, std::int32_t cell_x0, std::int32_t cell_y0, std::uint32_t cols, std::uint32_t rows, const std::uint16_t *words) {
  if (!in_render()) return refused_phase();
  if (!sega_mod_is_granted(&g.scheduler->host, SEGA_MOD_CAP_SCENERY_WORDS)) { genesis_mod_fault_current("capability"); return SEGA_MOD_E_CAPABILITY; }
  if (plane > 1U || words == nullptr || cols == 0U || rows == 0U || cols > kSceneryColsMax || rows > kSceneryRowsMax || cols * rows > kSceneryWordsMax || g.staged_scenery[plane].present ||
      cell_x0 < -(1 << 20) || cell_x0 > (1 << 20) || cell_y0 < -(1 << 20) || cell_y0 > (1 << 20)) {
    genesis_mod_fault_current("range");   // one window per plane per callback, inside the declared bounds
    return SEGA_MOD_E_RANGE;
  }
  Mod::SceneryWindow &w = g.staged_scenery[plane];
  w.present = true;
  w.x0 = cell_x0;
  w.y0 = cell_y0;
  w.cols = cols;
  w.rows = rows;
  w.words.assign(words, words + static_cast<std::size_t>(cols) * rows);
  return SEGA_MOD_OK;
}
SegaGenesisRenderApi g_render_api = {sizeof(SegaGenesisRenderApi), api_overlay_layer, api_overlay_target, api_overlay_rect, api_overlay_text, api_scene, api_virtual_sprites, api_scenery_window, {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}};

// The provider's `phase` entry, wrapped: a render callback starts from an empty staging area, and what it staged becomes visible only if the callback finished without a fault (a faulted callback leaves nothing).
void wrapped_phase(void *state, const SegaModHost *host) {
  const bool render = host->phase == SEGA_MOD_PHASE_RENDER;
  if (render) {
    g.staged = luau::Output{}; g.target = 0; g.visible = luau::Output{}; g.have_staged_set = false; g.have_visible_set = false;
    for (Mod::SceneryWindow &w : g.staged_scenery) w = Mod::SceneryWindow{};
    for (Mod::SceneryWindow &w : g.visible_scenery) w = Mod::SceneryWindow{};   // nothing of an earlier frame survives the start of a render callback
  }
  g.native_phase_entry(state, host);
  if (render && g.scheduler->fault_status == SEGA_MOD_OK && g.scheduler->external_cause == nullptr) {
    g.visible = std::move(g.staged);
    if (g.have_staged_set) { std::swap(g.visible_set, g.staged_set); g.have_visible_set = true; }
    for (unsigned plane = 0; plane < 2U; ++plane) std::swap(g.visible_scenery[plane], g.staged_scenery[plane]);
    g.visible_scenery_frame = host->frame;
  }
  if (render) {
    g.staged = luau::Output{}; g.have_staged_set = false;
    for (Mod::SceneryWindow &w : g.staged_scenery) w = Mod::SceneryWindow{};
  }
}
#endif

}  // namespace

extern "C" {

// 0 = the mod is attached (or there is none to attach); nonzero = refused (the program must not run a half-configured mod: the caller exits).
int segarecomp_capture_mod_begin(GenesisRuntime *runtime) {
  g.scheduler = std::make_unique<GenesisModScheduler>();
  GenesisModConfig c{};
  c.mod_id = MOD_ID;
#ifdef SEGARECOMP_MOD_NATIVE
  void *native_state = nullptr;
  const SegaModProvider *native = segarecomp_mod_provider(&native_state);
  if (native == nullptr || native->phase == nullptr) { g.refused = true; g.refusal = "E_ABI_ENTRY_MISSING"; std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_ABI_ENTRY_MISSING\"}\n"); return 1; }
  std::memcpy(&g.wrapper, native, native->size < sizeof g.wrapper ? native->size : sizeof g.wrapper);   // a table smaller than the ABI's is refused by the scheduler's negotiation (E_ABI_TABLE_SIZE), never read past
  g.native_phase_entry = native->phase;
  g.wrapper.phase = wrapped_phase;
  c.provider = &g.wrapper;
  c.provider_state = native_state;
  c.phase_mask = (1U << SEGA_MOD_PHASE_RESET) | (1U << SEGA_MOD_PHASE_FRAME_BEGIN) | (1U << SEGA_MOD_PHASE_INPUT) | (1U << SEGA_MOD_PHASE_POST_INPUT) | (1U << SEGA_MOD_PHASE_FRAME_END) | (1U << SEGA_MOD_PHASE_RENDER);
  c.required[0] = segarecomp_mod_caps_required[0]; c.required[1] = segarecomp_mod_caps_required[1];
  c.optional[0] = segarecomp_mod_caps_optional[0]; c.optional[1] = segarecomp_mod_caps_optional[1];
  c.render_api = &g_render_api;
#ifdef SEGARECOMP_MOD_HOOKS
  {   // SEG-138-T012: every binding must name a site of the generated table (the build checked the same; a program must never run with a binding that can never fire), and the hook phases are registered with it
    std::size_t site_count = 0;
    const GenesisHookSiteInfo *sites = genesis_generated_hook_sites(&site_count);
    if (segarecomp_mod_hook_count > GENESIS_MOD_HOOKS_MAX) { g.refused = true; g.refusal = "E_HOOK_BINDING"; std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_HOOK_BINDING\"}\n"); return 1; }
    for (unsigned long i = 0; i < segarecomp_mod_hook_count; ++i) {
      bool found = false;
      for (std::size_t site = 0; site < site_count; ++site) found = found || std::strcmp(sites[site].id, segarecomp_mod_hooks[i].id) == 0;
      if (!found) { g.refused = true; g.refusal = "E_HOOK_BINDING"; std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_HOOK_BINDING\"}\n"); return 1; }
      g.bindings[i] = GenesisModHookBinding{segarecomp_mod_hooks[i].id, static_cast<std::uint32_t>(segarecomp_mod_hooks[i].hook_id)};
    }
    c.hooks = g.bindings;
    c.hook_count = static_cast<std::uint32_t>(segarecomp_mod_hook_count);
    c.sites = sites;
    c.site_count = static_cast<std::uint32_t>(site_count);
    c.phase_mask |= static_cast<std::uint32_t>(segarecomp_mod_hook_phases);
  }
#endif
#else
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
  c.provider = g.provider->provider();
  c.provider_state = g.provider->state();
  c.phase_mask = g.provider->phase_mask();
  g.provider->capabilities(c.required);
#endif
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
#ifndef SEGARECOMP_MOD_VIEWER   // the played program has the viewer's own save-state / rewind service; these seams are the headless capture's
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
#endif
#if defined(SEGARECOMP_MOD_NATIVE) && defined(SEGARECOMP_MOD_HOOKS)
  std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"kind\":\"native\",\"package_identity\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu,\"hooks\":%lu}\n", MOD_ID, MOD_IDENTITY, MOD_CAPABILITIES, segarecomp_mod_range_count, segarecomp_mod_hook_count);
#elif defined(SEGARECOMP_MOD_NATIVE)
  std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"kind\":\"native\",\"package_identity\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", MOD_ID, MOD_IDENTITY, MOD_CAPABILITIES, segarecomp_mod_range_count);
#else
  std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"package_identity\":\"%s\",\"bytecode_digest\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", MOD_ID, MOD_IDENTITY,
               segarecomp_luau_bytecode_digest, MOD_CAPABILITIES, segarecomp_mod_range_count);
#endif
  return 0;
}

#ifdef SEGARECOMP_MOD_VIEWER
// SEG-138-T010: the snapshot seam of the attached scheduler (the viewer's state service points GenesisSnapshotMachine::mod at it) and the compatibility table that presents the mod's render output.
const GenesisSnapshotModState *segarecomp_scheduled_mod_snapshot(void) { return g.attached ? &g.snapshot : nullptr; }

namespace {
struct Presented {
  luau::OverlayImage image;
  luau::RasterizedLayers layers;
  luau::SpriteSubmission sprites;   // SEG-140-T008: the virtual sprites committed by the last successful render callback
  const GenesisVirtualSpriteSet *set = nullptr;   // SEG-140-T010: the native route's committed set (owned by the adapter, valid until the next render callback)
  bool fixed = false, have = false, have_sprites = false, have_set = false, scenery = false;
} p;

void prepare(void *, const GenesisCompatFrame *frame, GenesisCompatDecision *out) {
  out->accepted = 0;
  out->reason = GENESIS_COMPAT_OK;
  p.have = false;
  p.have_sprites = false;
  p.have_set = false;
  p.scenery = false;
  if ((frame->requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY)) == 0) return;
  if (!g.attached || g.scheduler->fault.faulted != 0U || g.scheduler->disabled != 0U) {   // a faulted mod presents nothing; the guest and the picture go on
    out->reason = GENESIS_COMPAT_FAULT;
    return;
  }
  unsigned accepted = 0;
#ifdef SEGARECOMP_MOD_NATIVE
  if ((frame->requested & GENESIS_COMPAT_SCENERY) != 0U) {
    // SEG-140-T011: a window belongs to exactly one produced frame. The render callback of guest frame N runs right after frame N ended; the host produces the frame that callback describes when the runtime counts N + 2
    // (the producer sees the work-RAM snapshot taken one onset earlier, as for every route). A window committed for any other frame (a rewind, a load, a skipped callback), or none, is never served.
    const bool current = g.scheduler->runtime != nullptr && g.visible_scenery_frame + 2U == g.scheduler->runtime->virtual_frame_count;
    p.scenery = current && g.visible_scenery[0].present && g.visible_scenery[1].present;   // the producer asks both planes for every offscreen cell: a frame is served only with a window for each (an empty plane is a window of blank cells)
    if (p.scenery) accepted |= GENESIS_COMPAT_SCENERY;
  }
#endif
  if ((frame->requested & GENESIS_COMPAT_HUD) != 0U) {
    const RenderView view = render_view();
    const std::vector<luau::OverlayOp> &ops = *view.ops;
    p.have_sprites = view.sprites->total > 0;
    if (p.have_sprites) p.sprites = *view.sprites;
    p.set = view.set;
    p.have_set = view.set != nullptr && view.set->count > 0;
    if (!ops.empty() || p.have_sprites || p.have_set) {
      p.image = luau::rasterize_overlay(ops);
      p.fixed = false;
      for (const luau::OverlayOp &op : ops) p.fixed = p.fixed || op.layer == 0;
      p.layers.clear();
      for (const luau::OverlayLayerDecl &d : *view.layers) p.layers.emplace_back(d, luau::rasterize_layer(d, ops));
      p.have = true;
      accepted |= GENESIS_COMPAT_HUD;
    }
  }
  out->accepted = accepted;
  if (accepted == 0U) return;
  const std::string &scene = *render_view().scene;
  if (!scene.empty() && scene.size() <= GENESIS_COMPAT_SCENE_MAX) std::memcpy(out->scene, scene.c_str(), scene.size() + 1U);
}
// SEG-140-T011: answers only from the committed window of the frame `prepare` accepted; a cell outside it is a failed answer (the host then withdraws the frame's authored-scenery claim), never a guess.
int scenery_word(void *, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
#ifdef SEGARECOMP_MOD_NATIVE
  if (!p.scenery || plane > 1U || word == nullptr) return 0;
  const Mod::SceneryWindow &w = g.visible_scenery[plane];
  if (!w.present) return 0;
  const auto cell = [](long long pixel) { return static_cast<long long>(pixel >= 0 ? pixel / 8 : -((-pixel + 7) / 8)); };   // floor division by 8
  const long long cx = cell(static_cast<long long>(x) - hscroll) - w.x0, cy = cell(static_cast<long long>(row) + vscroll) - w.y0;
  if (cx < 0 || cy < 0 || cx >= static_cast<long long>(w.cols) || cy >= static_cast<long long>(w.rows)) return 0;
  *word = w.words[static_cast<std::size_t>(cy) * w.cols + static_cast<std::size_t>(cx)];
  return 1;
#else
  (void)plane; (void)row; (void)x; (void)hscroll; (void)vscroll; (void)word;
  return 0;
#endif
}
int hud(void *, GenesisCompatOverlay *out) {
  const int hud_drawn = luau::pack_hud(p.have, p.fixed, p.image, p.layers, out);
  bool sprites = p.have_sprites && luau::pack_sprites(p.sprites, out->vsprites);
  if (p.have_set && out->vsprites != nullptr) {   // the native route's set is already the shared transport: copied as submitted
    std::memcpy(out->vsprites, p.set, sizeof *out->vsprites);
    sprites = true;
  }
  return hud_drawn != 0 || sprites ? 1 : 0;
}
const GenesisNativeCompat kTable = {GENESIS_NATIVE_COMPAT_ABI, "scheduled-mod-host", 1, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SPRITES | GENESIS_COMPAT_SCENERY, prepare, scenery_word, hud};
}  // namespace

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  *state_out = nullptr;
  return &kTable;
}
#endif

// Between runner slices: the optional save/restore round trip.
void segarecomp_capture_mod_checkpoint(GenesisRuntime *runtime) {
  if (!g.attached || !g.checkpoint_wanted || g.checkpoint_done || runtime->virtual_frame_count < g.checkpoint_frame) return;
  g.checkpoint_done = true;
  if (runtime->function_hooks != nullptr) g.checkpoint_depth = runtime->function_hooks->depth;
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
  const RenderView view = render_view();
  const luau::OverlayImage image = luau::rasterize_overlay(*view.ops);
  const std::string overlay_hex = view.ops->empty() ? std::string() : (sha256_hex(std::span<const std::uint8_t>(image.rgba.data(), image.rgba.size())));
  std::string hooks_json;
#if defined(SEGARECOMP_MOD_NATIVE) && defined(SEGARECOMP_MOD_HOOKS)   // SEG-138-T012: the hook counters (callbacks delivered by phase, and the loud non-altering skips: abandoned, unproven caller, frame stack full); only a hooked program reports them
  {
    char text[240];
    std::snprintf(text, sizeof text, ",\"hooks\":{\"entry\":%llu,\"exit\":%llu,\"abandoned\":%llu,\"unproven\":%llu,\"overflowed\":%llu,\"checkpoint_depth\":%u}", static_cast<unsigned long long>(s.hook_calls[0]), static_cast<unsigned long long>(s.hook_calls[1]),
                  static_cast<unsigned long long>(s.hook_skips[1]), static_cast<unsigned long long>(s.hook_skips[2]), static_cast<unsigned long long>(s.hook_skips[3]), static_cast<unsigned>(g.checkpoint_depth));
    hooks_json = text;
  }
#endif
  std::fprintf(stderr,
               "MOD_SUMMARY {\"mod\":\"%s\",\"faulted\":%s,\"fault\":\"%s\",\"callbacks\":%llu,\"committed_writes\":%llu,\"write_digest\":\"%s\",\"overlay_ops\":%llu,\"overlay_sha256\":\"%s\","
               "\"nondeterministic\":%s,\"roundtrip\":{\"requested\":%s,\"frame\":%llu,\"status\":\"%s\",\"state_sha256\":\"%s\"},\"loaded_state_sha256\":\"%s\"%s}\n",
               MOD_ID, s.fault.faulted != 0U ? "true" : "false", s.fault.faulted != 0U ? json_escaped(s.fault.text).c_str() : "", static_cast<unsigned long long>(s.callbacks_run),
               static_cast<unsigned long long>(s.write_total), hex(digest, 32).c_str(), static_cast<unsigned long long>(view.ops->size()), overlay_hex.c_str(),
#ifdef SEGARECOMP_MOD_NATIVE
               "false",
#else
               g.provider->nondeterministic() ? "true" : "false",
#endif
               g.checkpoint_wanted ? "true" : "false", static_cast<unsigned long long>(g.checkpoint_frame),
               g.checkpoint_wanted ? (g.checkpoint_done ? g.checkpoint_status.c_str() : "not_reached") : "none", g.checkpoint_digest.c_str(), g.loaded_digest.c_str(), hooks_json.c_str());
  genesis_mod_scheduler_detach(&s);
}

}  // extern "C"
