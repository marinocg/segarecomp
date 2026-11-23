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
#include "mod_scenery_window.hpp"
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
  // W04 gap B: the epoch of the committed virtual-sprite set. A set that depicts the game's own art (GAME_STATE_VERIFIED) is bound to the guest frame whose end-of-frame work-RAM copy the render callback ran against: it is
  // served only for the frame the runtime produces two guest frames later, and only when the snapshot that frame's producer sees is byte-identical (FNV-1a 64) to that copy.
  std::uint64_t visible_set_frame = 0, visible_set_digest = 0;
  bool visible_set_digest_valid = false;
  // SEG-140-T011 (ADR 0309): the typed scenery_window feature. One bounded name-table window per plane, staged during the render callback, committed with it, bound to the one frame it was rendered for.
  using SceneryWindow = segarecomp::scenery::Window;   // mod_scenery_window.hpp: the limits and the lookup shared with the ABI 1 / ABI 2 parity harness
  SceneryWindow staged_scenery[2][2], visible_scenery[2][2];   // [plane][window]: ADR 0313, up to two windows per plane (the playfield rows and a split-scroll band can need disjoint cell ranges)
  std::uint64_t visible_scenery_frame = 0;   // the guest frame whose render callback committed `visible_scenery` (and the ADR 0315 placement below: they are committed together)
  // ADR 0315: the frame-bound asymmetric placement. `margin` is the per-side margin the provider saw when it chose `left_px`; -1 = none declared.
  bool exact_served = false;                  // ADR 0320: the committed output was produced by THIS prepare() call (an exact-frame provider); it is never carried to another frame
  std::uint32_t exact_side = 0;               // the per-side margin of the frame an exact-frame callback is running for (== SegaGenesisFrameView::side, so view_margin and the view agree)
  bool exact_running = false;
  std::int64_t staged_left_px = -1, visible_left_px = -1;
  std::uint32_t staged_left_margin = 0, visible_left_margin = 0;
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
  int fallback_x, fallback_y;           // SEG-152-T005: the fixed overlay's ABI 1 fallback position (native route only; 0 for Luau)
  std::uint32_t declined, decline_reason;   // SEG-152-T006: the non-fault decline of the committed callback (native route only)
};
RenderView render_view() {
#ifdef SEGARECOMP_MOD_NATIVE
  return {&g.visible.overlay, &g.visible.layers, &g.visible.scene, &g.visible.sprites, g.have_visible_set ? g.visible_set.get() : nullptr, g.visible.fallback_x, g.visible.fallback_y, g.visible.declined, g.visible.decline_reason};
#else
  return {&g.provider->overlay(), &g.provider->layers(), &g.provider->scene(), &g.provider->sprites(), nullptr, 0, 0, 0U, 0U};
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
// SEG-152-T005 (ADR 0320 decision 6): the bounded RGBA bitmap calls: validation, clamping, budgets and notes are luau_api.cpp's (one implementation for the staged output the one compositor consumes).
int api_overlay_bitmap(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h, const std::uint8_t *rgba, std::uint32_t stride_bytes) {
  return in_render() ? answer(luau::overlay_bitmap(g.staged, g.target, x, y, w, h, rgba, stride_bytes)) : refused_phase();
}
int api_overlay_layer_bitmap(std::uint32_t index, std::uint32_t anchor, std::uint32_t reference, std::int32_t dx, std::int32_t dy, std::uint32_t w, std::uint32_t h, std::uint32_t z) {
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
  return answer(luau::overlay_declare_bitmap_layer(g.staged, d));
}
int api_overlay_layer_clip(std::uint32_t index, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  return in_render() ? answer(luau::overlay_layer_clip(g.staged, index, x, y, w, h)) : refused_phase();
}
int api_overlay_fallback(std::uint32_t x, std::uint32_t y) { return in_render() ? answer(luau::overlay_fallback(g.staged, x, y)) : refused_phase(); }
// SEG-152-T006 (ADR 0320 decision 7): a refusal is not a fault. The provider-selectable reasons are the ABI 1 provider-owned vocabulary; STALE_STATE and FAULT are host-originated (a provider must not impersonate them).
int api_decline(std::uint32_t components, std::uint32_t reason) {
  if (!in_render()) return refused_phase();
  const std::uint32_t known = SEGA_GEN_COMPONENT_HUD | SEGA_GEN_COMPONENT_SCENERY | SEGA_GEN_COMPONENT_SPRITES;
  const bool selectable = reason == SEGA_GEN_REASON_UNSUPPORTED_SCENE || reason == SEGA_GEN_REASON_BAD_STATE || reason == SEGA_GEN_REASON_BAD_SCROLL || reason == SEGA_GEN_REASON_MISMATCH;
  if (components == 0U || (components & ~known) != 0U || !selectable) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }
  if (g.staged.declined == 0U) g.staged.decline_reason = reason;   // the first reason wins; the components accumulate
  g.staged.declined |= components;
  return SEGA_MOD_OK;
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
std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t n) {
  std::uint64_t h = 14695981039346656037ULL;
  for (std::size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
  return h;
}
int api_scenery_window(std::uint32_t plane, std::int32_t cell_x0, std::int32_t cell_y0, std::uint32_t cols, std::uint32_t rows, const std::uint16_t *words) {
  if (!in_render()) return refused_phase();
  if (!sega_mod_is_granted(&g.scheduler->host, SEGA_MOD_CAP_SCENERY_WORDS)) { genesis_mod_fault_current("capability"); return SEGA_MOD_E_CAPABILITY; }
  if (!segarecomp::scenery::submission_in_range(plane, cell_x0, cell_y0, cols, rows, words, plane < 2U && g.staged_scenery[plane][0].present && g.staged_scenery[plane][1].present)) {
    genesis_mod_fault_current("range");   // one window per plane per callback, inside the declared bounds
    return SEGA_MOD_E_RANGE;
  }
  Mod::SceneryWindow &w = g.staged_scenery[plane][g.staged_scenery[plane][0].present ? 1U : 0U];   // the first free slot of the plane; a third window was refused above (range fault)
  w.present = true;
  w.x0 = cell_x0;
  w.y0 = cell_y0;
  w.cols = cols;
  w.rows = rows;
  w.words.assign(words, words + static_cast<std::size_t>(cols) * rows);
  return SEGA_MOD_OK;
}
// ADR 0315: the per-side margin of the host's most recently PUBLISHED extension result, read from the runtime's own publication (never cached): zero for Original, 4:3, a failed production or before the first one,
// because the runtime publishes a result (status NONE / UNAVAILABLE, side 0) for every frame, including the ones that return before prepare().
std::uint32_t current_margin() {
  if (g.scheduler == nullptr || g.scheduler->runtime == nullptr || g.scheduler->runtime->live_raster == nullptr || g.scheduler->runtime->live_raster->extension == nullptr) return 0U;
  const GenesisExtensionResult &r = g.scheduler->runtime->live_raster->extension->result;
  const GenesisFrameExtension *ext = g.scheduler->runtime->live_raster->extension;
  return ext->extension_shown != 0U && (r.status == GENESIS_EXT_AVAILABLE || r.status == GENESIS_EXT_PARTIAL) ? r.side : 0U;
}
int api_view_margin(std::uint32_t *margin_px) {
  if (!in_render()) return refused_phase();
  if (margin_px == nullptr) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }
  *margin_px = g.exact_running ? g.exact_side : current_margin();   // inside an exact-frame callback the margin is the one of the frame being produced
  return SEGA_MOD_OK;
}
int api_view_left(std::uint32_t left_px, std::uint32_t margin_px) {
  if (!in_render()) return refused_phase();
  if (left_px > 2U * 672U || margin_px > 672U) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }   // 672 = SV_EXT_MAX_SIDE; whether it fits the planned total is the host's validation at production
  g.staged_left_px = static_cast<std::int64_t>(left_px);
  g.staged_left_margin = margin_px;   // the margin the provider computed for, not the one the host happens to show at the call
  return SEGA_MOD_OK;
}
#ifdef SEGARECOMP_MOD_VIEWER
// SEG-152-T004 (ADR 0320 decision 5): the typed helper table re-exports the host's existing functions; there is no second VDP interpretation here.
extern "C" const GenesisCompatHost *genesis_native_compat_host(void);
SegaGenesisVdpHelpers make_vdp_helpers() {
  const GenesisCompatHost *h = genesis_native_compat_host();
  SegaGenesisVdpHelpers t{};
  t.size = sizeof t;
  t.read_hscroll = h->read_hscroll;
  t.read_vscroll = h->read_vscroll;
  t.vscroll_is_per_column = h->vscroll_is_per_column;
  t.cram_rgb = h->cram_rgb;
  return t;
}
const SegaGenesisVdpHelpers g_vdp_helpers = make_vdp_helpers();
static_assert(SEGA_GEN_COMPONENT_HUD == GENESIS_COMPAT_HUD && SEGA_GEN_COMPONENT_SCENERY == GENESIS_COMPAT_SCENERY && SEGA_GEN_COMPONENT_SPRITES == GENESIS_COMPAT_SPRITES &&
              SEGA_GEN_REASON_UNSUPPORTED_SCENE == GENESIS_COMPAT_UNSUPPORTED_SCENE && SEGA_GEN_REASON_BAD_STATE == GENESIS_COMPAT_BAD_STATE && SEGA_GEN_REASON_BAD_SCROLL == GENESIS_COMPAT_BAD_SCROLL &&
              SEGA_GEN_REASON_MISMATCH == GENESIS_COMPAT_MISMATCH, "the ABI 2 component and reason values are the ABI 1 ones");
static_assert(sizeof(SegaGenesisFrameLine) == sizeof(GenesisCompatLine) && offsetof(SegaGenesisFrameLine, cram) == offsetof(GenesisCompatLine, cram) && offsetof(SegaGenesisFrameLine, vsram) == offsetof(GenesisCompatLine, vsram),
              "the ABI 2 frame line has the layout of the ABI 1 compat line");
#endif
SegaGenesisRenderApi g_render_api = {sizeof(SegaGenesisRenderApi), api_overlay_layer, api_overlay_target, api_overlay_rect, api_overlay_text, api_scene, api_virtual_sprites, api_scenery_window, api_view_margin, api_view_left, {nullptr, nullptr, nullptr, nullptr},
                                   api_overlay_bitmap, api_overlay_layer_bitmap, api_overlay_layer_clip, api_overlay_fallback, api_decline};

// The provider's `phase` entry, wrapped: a render callback starts from an empty staging area, and what it staged becomes visible only if the callback finished without a fault (a faulted callback leaves nothing).
void wrapped_phase(void *state, const SegaModHost *host) {
  const bool render = host->phase == SEGA_MOD_PHASE_RENDER;
  if (render) {
    g.staged = luau::Output{}; g.target = 0; g.visible = luau::Output{}; g.have_staged_set = false; g.have_visible_set = false;
    for (auto &plane_windows : g.staged_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};
    for (auto &plane_windows : g.visible_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};   // nothing of an earlier frame survives the start of a render callback
    g.staged_left_px = g.visible_left_px = -1;
  }
  g.native_phase_entry(state, host);
  if (render && g.scheduler->fault_status == SEGA_MOD_OK && g.scheduler->external_cause == nullptr) {
    g.visible = std::move(g.staged);
    if (g.have_staged_set) {
      std::swap(g.visible_set, g.staged_set); g.have_visible_set = true;
      g.visible_set_frame = host->frame;
      g.visible_set_digest_valid = g.scheduler->render_copy_valid != 0U;
      g.visible_set_digest = g.visible_set_digest_valid ? fnv1a64(g.scheduler->render_copy, GENESIS_MOD_WORK_RAM_BYTES) : 0U;
    }
    for (unsigned plane = 0; plane < 2U; ++plane) for (unsigned k = 0; k < 2U; ++k) std::swap(g.visible_scenery[plane][k], g.staged_scenery[plane][k]);
    g.visible_scenery_frame = host->frame;
    g.visible_left_px = g.staged_left_px;
    g.visible_left_margin = g.staged_left_margin;
  }
  if (render) {
    g.staged_left_px = -1;
    g.staged = luau::Output{}; g.have_staged_set = false;
    for (auto &plane_windows : g.staged_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};
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
#ifdef SEGARECOMP_MOD_VIEWER
  c.vdp_helpers = &g_vdp_helpers;   // SEG-152-T004: the host's own pure VDP functions (ABI 1 GenesisCompatHost), re-exported through the ABI 2 Genesis table
#endif
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
#ifndef SEGARECOMP_MOD_NATIVE
  if ((c.required[0] >> SEGA_MOD_CAP_FRAME_VDP & 1U) != 0U) {   // ADR 0320: the exact-frame callback is a native ABI 2 feature; a Luau script that requires frame.vdp is refused exactly as before
    g.refused = true; g.refusal = "E_CAPABILITY_UNSUPPORTED";
    std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_CAPABILITY_UNSUPPORTED\"}\n");
    return 1;
  }
#endif
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

// SEG-138-T010: the snapshot seam of the attached scheduler (the viewer's state service points GenesisSnapshotMachine::mod at it). SEG-146-T006: the headless program's session checkpoint digest uses it too,
// so it is defined for every build of this file (it is inside the extern "C" block opened above).
const GenesisSnapshotModState *segarecomp_scheduled_mod_snapshot(void) { return g.attached ? &g.snapshot : nullptr; }

#ifdef SEGARECOMP_MOD_VIEWER
// SEG-138-T010: the compatibility table that presents the mod's render output.

namespace {
struct Presented {
  luau::OverlayImage image;
  luau::RasterizedLayers layers;
  luau::SpriteSubmission sprites;   // SEG-140-T008: the virtual sprites committed by the last successful render callback
  const GenesisVirtualSpriteSet *set = nullptr;   // SEG-140-T010: the native route's committed set (owned by the adapter, valid until the next render callback)
  int fallback_x = 0, fallback_y = 0;   // SEG-152-T005: the committed callback's fixed-overlay fallback position
  bool fixed = false, have = false, have_sprites = false, have_set = false, scenery = false, left_ok = false;
} p;

void prepare(void *, const GenesisCompatFrame *frame, GenesisCompatDecision *out) {
  out->accepted = 0;
  out->reason = GENESIS_COMPAT_OK;
  p.have = false;
  p.have_sprites = false;
  p.have_set = false;
  p.scenery = false;
  p.left_ok = false;
#ifdef SEGARECOMP_MOD_NATIVE
  g.exact_served = false;
#endif
  if ((frame->requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY)) == 0) return;
  if (!g.attached || g.scheduler->fault.faulted != 0U || g.scheduler->disabled != 0U) {   // a faulted mod presents nothing; the guest and the picture go on
    out->reason = GENESIS_COMPAT_FAULT;
    return;
  }
  unsigned accepted = 0;
#ifdef SEGARECOMP_MOD_NATIVE
  // ADR 0320 (SEG-152-T003): an exact-frame provider (it required frame.vdp) has its `render` callback run HERE, once per produced frame, with an immutable copy of this frame's data. What it stages is consumed by this very
  // call: there is no epoch to bind, so nothing of another frame can be served. An ordinary provider is untouched (its callback ran after the previous snapshot; the epoch rules below apply).
  const bool exact = g.scheduler->exact_frame != 0U;
  std::uint32_t eff_side = frame->side;
  g.exact_served = false;
  if (exact) {
    const GenesisRasterCapture *raster = g.scheduler->runtime != nullptr ? g.scheduler->runtime->live_raster : nullptr;
    const GenesisFrameExtension *ext = raster != nullptr ? raster->extension : nullptr;
    eff_side = ext == nullptr || ext->extension_shown != 0U ? frame->side : 0U;   // Original and exact 4:3 show no extension: the view reports side 0 (ADR 0315)
    GenesisModExactFrame ef{};
    ef.work_ram = frame->work_ram;
    ef.vram = frame->vram;
    ef.lines = reinterpret_cast<const SegaGenesisFrameLine *>(frame->lines);
    ef.line_count = frame->line_count;
    ef.width = frame->width; ef.height = frame->height; ef.side = eff_side; ef.requested = frame->requested;
    ef.vram_frame_start = raster != nullptr && frame->vram != nullptr && frame->vram == raster->frame.vram_start ? 1U : 0U;   // a journaled frame hands out the frame-start VRAM, any other the producer's end-of-frame image
    g.exact_side = eff_side;
    g.exact_running = true;
    const int ran = genesis_mod_scheduler_present(g.scheduler.get(), &ef);
    g.exact_running = false;
    if (!ran) {
      if (g.scheduler->disabled != 0U || g.scheduler->fault.faulted != 0U) out->reason = GENESIS_COMPAT_FAULT;   // a genuine callback fault: nothing staged is served
      return;
    }
    g.exact_served = true;
  }
  // SEG-140-T011: a window belongs to exactly one produced frame. The render callback of guest frame N runs right after frame N ended; the host produces the frame that callback describes when the runtime counts N + 2
  // (the producer sees the work-RAM snapshot taken one onset earlier, as for every route). A window committed for any other frame (a rewind, a load, a skipped callback), or none, is never served.
  const bool current = g.exact_served || (!exact && g.scheduler->runtime != nullptr && g.visible_scenery_frame + 2U == g.scheduler->runtime->virtual_frame_count);
  // ADR 0315: framing is a render-API feature of its own, independent of scenery: a provider with generic scenery can request a placement without claiming authored scenery. It is served only for the frame it was
  // declared for (the same epoch), only when it was computed for the margin this frame is produced with, and only inside the planned total; anything else is the centred layout.
  const bool declared = current && g.visible_left_px >= 0;
  p.left_ok = declared && eff_side > 0U && g.visible_left_margin == eff_side && g.visible_left_px <= 2 * static_cast<std::int64_t>(eff_side);
  if ((frame->requested & GENESIS_COMPAT_SCENERY) != 0U) {
    p.scenery = current && (g.visible_scenery[0][0].present || g.visible_scenery[1][0].present);   // ADR 0314: a frame is served when at least one plane has a window (an empty plane is a window of blank cells); a plane without one is left to the generic extension
    if (p.scenery) accepted |= GENESIS_COMPAT_SCENERY;
    // The windows of a callback that declared a placement were built for THAT view: when the placement is not valid for this frame's geometry they are not served either (they would answer the wrong
    // cells), so the scenery falls back to the generic extension. The HUD and sprite contributions are independent of the view and stay accepted. A provider that declared no placement is untouched.
    if (declared && !p.left_ok && p.scenery) { p.scenery = false; accepted &= ~static_cast<unsigned>(GENESIS_COMPAT_SCENERY); }
  }
#endif
  const RenderView view_now = render_view();
  const std::uint32_t declined = view_now.declined;   // SEG-152-T006: components the provider refused for this frame (never a fault): their output is discarded, the generic path serves them
  if ((declined & GENESIS_COMPAT_SCENERY) != 0U) { p.scenery = false; accepted &= ~static_cast<unsigned>(GENESIS_COMPAT_SCENERY); }
  if ((frame->requested & GENESIS_COMPAT_HUD) != 0U) {
    const RenderView view = view_now;
    static const std::vector<luau::OverlayOp> no_ops;
    const bool hud_declined = (declined & GENESIS_COMPAT_HUD) != 0U;   // a declined HUD presents no overlay or layer; the virtual sprites are the SPRITES component's and are declined separately
    const std::vector<luau::OverlayOp> &ops = hud_declined ? no_ops : *view.ops;
    p.have_sprites = view.sprites->total > 0 && (declined & GENESIS_COMPAT_SPRITES) == 0U;
    if (p.have_sprites) p.sprites = *view.sprites;
    p.set = view.set;
    p.have_set = view.set != nullptr && view.set->count > 0 && (declined & GENESIS_COMPAT_SPRITES) == 0U;
#ifdef SEGARECOMP_MOD_NATIVE
    if (p.have_set && frame->work_ram != nullptr) {   // W04 gap B (a provider without frame.work_ram has no snapshot: the compositor then rejects game art as UNVERIFIED_STATE, as before): a set naming the game's own art must belong to THIS frame's epoch; otherwise the whole set is dropped (a stale submission, a restore, a rewind or a skipped callback never draws game art)
      bool verified = false;
      for (std::uint32_t i = 0; i < view.set->count && i < GENESIS_VSPRITE_MAX; ++i) verified = verified || view.set->sprites[i].provenance == GENESIS_VSPRITE_GAME_STATE_VERIFIED;
      const bool epoch = g.exact_served ? g.visible_set_digest_valid && fnv1a64(frame->work_ram, GENESIS_MOD_WORK_RAM_BYTES) == g.visible_set_digest   // exact frame: the copy the callback ran against is this frame's own
                                        : g.scheduler->runtime != nullptr && g.visible_set_frame + 2U == g.scheduler->runtime->virtual_frame_count && g.visible_set_digest_valid && frame->work_ram != nullptr &&
                         fnv1a64(frame->work_ram, GENESIS_MOD_WORK_RAM_BYTES) == g.visible_set_digest;
      if (verified && !epoch) p.have_set = false;
    }
#endif
    if (!ops.empty() || p.have_sprites || p.have_set) {
      p.image = luau::rasterize_overlay(ops);
      p.fixed = false;
      for (const luau::OverlayOp &op : ops) p.fixed = p.fixed || op.layer == 0;
      p.layers.clear();
      if (!hud_declined) for (const luau::OverlayLayerDecl &d : *view.layers) p.layers.emplace_back(d, luau::rasterize_layer(d, ops));
      p.fallback_x = view.fallback_x;
      p.fallback_y = view.fallback_y;
      p.have = true;
      accepted |= GENESIS_COMPAT_HUD;
    }
  }
  out->accepted = accepted;
  if (accepted == 0U && declined != 0U) out->reason = static_cast<std::uint8_t>(view_now.decline_reason);   // SEG-152-T006: the provider's diagnostic, reported exactly as an ABI 1 decline (a host reason is never provider-selectable)
#ifdef SEGARECOMP_MOD_NATIVE
  if (declined == (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES)) p.left_ok = false;   // a wholly declined frame is generic: no placement either
  if (p.left_ok) { out->left_px = static_cast<std::int32_t>(g.visible_left_px); out->has_left_px = 1U; }   // with the verdict OK even when no component is accepted (ADR 0315)
#endif
  if (accepted == 0U) return;
  const std::string &scene = *render_view().scene;
  if (!scene.empty() && scene.size() <= GENESIS_COMPAT_SCENE_MAX) std::memcpy(out->scene, scene.c_str(), scene.size() + 1U);
}
// SEG-140-T011: answers only from the committed window of the frame `prepare` accepted; a cell outside it is a failed answer (the host then withdraws the frame's authored-scenery claim), never a guess.
int scenery_word(void *, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
#ifdef SEGARECOMP_MOD_NATIVE
  if (!p.scenery || plane > 1U || word == nullptr) return 0;
  if (!g.visible_scenery[plane][0].present && !g.visible_scenery[plane][1].present) return GENESIS_COMPAT_WORD_GENERIC;   // ADR 0314: this plane has no window: the VDP-derived extension answers it, not a failure
  return segarecomp::scenery::lookup(g.visible_scenery[plane], static_cast<int>(row), x, hscroll, vscroll, word);
#else
  (void)plane; (void)row; (void)x; (void)hscroll; (void)vscroll; (void)word;
  return 0;
#endif
}
int hud(void *, GenesisCompatOverlay *out) {
  const int hud_drawn = luau::pack_hud(p.have, p.fixed, p.image, p.layers, out, p.fallback_x, p.fallback_y);
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
