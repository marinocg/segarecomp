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
// SEG-141-T003 (ADR 0327): a program with several mods (SEGARECOMP_MOD_SET) links both provider kinds; a single-mod program links exactly the one it carries.
#if defined(SEGARECOMP_MOD_NATIVE) || defined(SEGARECOMP_MOD_SET)
#define SEGARECOMP_MOD_HAS_NATIVE 1
#endif
#if !defined(SEGARECOMP_MOD_NATIVE) || defined(SEGARECOMP_MOD_SET)
#define SEGARECOMP_MOD_HAS_LUAU 1
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
#include "mod_set_artifact.h"
}

#include <algorithm>
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
#ifndef SEGARECOMP_MOD_SET
#ifdef SEGARECOMP_MOD_NATIVE
extern const char segarecomp_mod_identity[];             // package identity (64 hex)
extern const char segarecomp_mod_capabilities[];         // comma separated catalogue names the build granted
extern const char segarecomp_mod_id[];
extern const unsigned long long segarecomp_mod_caps_required[2], segarecomp_mod_caps_optional[2];   // bitsets by capability id
const SegaModProvider *segarecomp_mod_provider(void **state_out);                                    // the package's ABI 2 entry symbol
#else
extern const unsigned char segarecomp_luau_artifact[];
extern const unsigned long segarecomp_luau_artifact_size;
extern const char segarecomp_luau_identity[];
extern const char segarecomp_luau_bytecode_digest[];
extern const char segarecomp_luau_capabilities[];
extern const unsigned long segarecomp_luau_memory_bytes, segarecomp_luau_budget_units;
extern const char segarecomp_luau_mod_id[];
#endif
// The scheduled-path constants (written by the build next to the artifact).
extern const unsigned long segarecomp_mod_state_schema;
extern const unsigned long segarecomp_mod_range_count;
extern const unsigned long segarecomp_mod_ranges[][4];   // space, offset, length, modes
// ADR 0321: the materialised bytes of the declared m68k.rom ranges (concatenated in range order). Every NATIVE artifact (generated or hand-written fixture) defines them, empty when no ROM range is declared; a Luau artifact cannot declare a ROM range and neither defines nor references them.
#ifdef SEGARECOMP_MOD_NATIVE
extern const unsigned char segarecomp_mod_rom_data[];
#endif
#endif
extern const char segarecomp_mod_effective_sha256[];
#ifdef SEGARECOMP_MOD_AUDIO
// SEG-145-T008: the package's declared command/event mappings, one row per (mapping, value). Only a program built from a package that declares them defines and compiles these.
struct segarecomp_mod_audio_row { const char *mapping; const char *name; unsigned long kind, offset, width, value; };
extern const unsigned long segarecomp_mod_audio_count;
extern const segarecomp_mod_audio_row segarecomp_mod_audio[];
#endif
#ifdef SEGARECOMP_MOD_HOOKS
#ifndef SEGARECOMP_MOD_SET
// SEG-138-T012 (ADR 0308): the function hooks of the built program: the artifact's bindings (generated site id, the hook id the provider is called with), the hook phases it registers, and the generated site table.
extern const unsigned long segarecomp_mod_hook_count, segarecomp_mod_hook_phases;
struct segarecomp_mod_hook_binding { const char *id; unsigned long hook_id; };
extern const segarecomp_mod_hook_binding segarecomp_mod_hooks[];
#endif
const GenesisHookSiteInfo *genesis_generated_hook_sites(size_t *count);
#endif
}

namespace {

using namespace segarecomp;

// One running mod: its row of the artifact and everything that is its own. The scheduler instance is where its grants, ranges, staged effects, fault, witness and state live.
struct Member {
  SegarecompModMemberRow row{};
#ifdef SEGARECOMP_MOD_HAS_LUAU
  std::unique_ptr<luau::PhaseProvider> luau;
#endif
  SegaModProvider native_table{};            // the package's provider table (for the drawing mod: with `phase` wrapped)
  std::unique_ptr<GenesisModScheduler> scheduler{std::make_unique<GenesisModScheduler>()};
  GenesisModRange ranges[GENESIS_MOD_RANGES_MAX]{};
  GenesisModHookBinding bindings[GENESIS_MOD_HOOKS_MAX]{};   // SEG-138-T012: must outlive the scheduler
  GenesisModHookScope scopes[GENESIS_MOD_HOOKS_MAX]{};
  bool native() const { return row.native_entry != nullptr; }
};

// The program's mods and the render output of the ONE mod that draws (`lead`): a build refuses a set in which two mods declare presentation output (ADR 0327).
struct Mod {
  std::vector<std::unique_ptr<Member>> members;
  Member *lead = nullptr;
  std::unique_ptr<GenesisModSet> set;
#ifdef SEGARECOMP_MOD_HAS_LUAU
  luau::PhaseProvider *provider = nullptr;   // the lead's script provider; null when the lead is native
#endif
#ifdef SEGARECOMP_MOD_HAS_NATIVE
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
  // SEG-152-T007 (ADR 0320 decision 8 addendum): the per-plane per-row generic mask, staged / committed / cleared exactly like the windows (2 x 56 bytes staged + 2 x 56 visible; no provider pointer is retained).
  segarecomp::scenery::RowMask staged_row_mask[2], visible_row_mask[2];
  segarecomp::scenery::RowOrigin staged_row_origin[2], visible_row_origin[2];   // SEG-152-T017
  bool staged_row_origin_submitted[2] = {false, false};
  bool staged_row_mask_submitted[2] = {false, false};   // at most one submission per plane per callback
  std::uint64_t visible_scenery_frame = 0;   // the guest frame whose render callback committed `visible_scenery` (and the ADR 0315 placement below: they are committed together)
  // ADR 0315: the frame-bound asymmetric placement. `margin` is the per-side margin the provider saw when it chose `left_px`; -1 = none declared.
  bool exact_served = false;                  // ADR 0320: the committed output was produced by THIS prepare() call (an exact-frame provider); it is never carried to another frame
  std::uint32_t exact_side = 0;               // the per-side margin of the frame an exact-frame callback is running for (== SegaGenesisFrameView::side, so view_margin and the view agree)
  bool exact_running = false;
  std::int64_t staged_left_px = -1, visible_left_px = -1;
  std::uint32_t staged_left_margin = 0, visible_left_margin = 0;
#endif
  GenesisModScheduler *scheduler = nullptr;   // the lead's
  GenesisSnapshotModState snapshot{};
  GenesisModAudioEntry audio[GENESIS_MOD_AUDIO_ENTRIES_MAX]{};
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

void hex_bytes(const char *text, std::uint8_t out[32]) {
  for (unsigned i = 0; i < 32U; ++i) out[i] = static_cast<std::uint8_t>(std::strtoul(std::string(text + i * 2U, 2U).c_str(), nullptr, 16));
}

// A saved state belongs to the effective image and to the variant: one mod's package identity, or a set's composite identity (membership, order, member builds and the composed image).
const char *variant_identity_text() {
#ifdef SEGARECOMP_MOD_SET
  return segarecomp_mod_set_identity;
#else
  return g.lead->row.identity;
#endif
}

void snapshot_identity(GenesisSnapshotIdentity &id) {
  std::memset(&id, 0, sizeof id);
  hex_bytes(segarecomp_mod_effective_sha256, id.source_sha256);
  hex_bytes(variant_identity_text(), id.variant_sha256);
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
#ifdef SEGARECOMP_MOD_HAS_LUAU
  if (g.provider != nullptr) return {&g.provider->overlay(), &g.provider->layers(), &g.provider->scene(), &g.provider->sprites(), nullptr, 0, 0, 0U, 0U};
#endif
#ifdef SEGARECOMP_MOD_HAS_NATIVE
  return {&g.visible.overlay, &g.visible.layers, &g.visible.scene, &g.visible.sprites, g.have_visible_set ? g.visible_set.get() : nullptr, g.visible.fallback_x, g.visible.fallback_y, g.visible.declined, g.visible.decline_reason};
#else
  std::abort();   // a Luau-only program always has a script provider here
#endif
}

#ifdef SEGARECOMP_MOD_HAS_NATIVE
// ---- SEG-138-T011: the typed ABI 2 render API (SegaGenesisRenderApi). The rules are luau_api.cpp's (shared with the Luau bindings); a refusal is a fault of the callback with the scheduler's stable cause. ----
bool in_render() { return g.scheduler != nullptr && g.scheduler->in_callback != 0U && g.scheduler->phase == SEGA_MOD_PHASE_RENDER; }
int refused_phase() { genesis_mod_fault_current("phase"); return SEGA_MOD_E_PHASE; }
// The render table is published for overlay.rgba OR scenery.words (a scenery-only provider needs scenery_window), so every overlay/HUD output call checks its own grant: a scenery-only provider cannot draw a HUD through
// the shared table. Returns 0 when the call may proceed, else the status (phase is checked first, as everywhere).
int overlay_gate() {
  if (!in_render()) return refused_phase();
  if (!sega_mod_is_granted(&g.scheduler->host, SEGA_MOD_CAP_OVERLAY_RGBA)) { genesis_mod_fault_current("capability"); return SEGA_MOD_E_CAPABILITY; }
  return SEGA_MOD_OK;
}
int answer(const luau::OverlayCall &r) {
  if (r.status == luau::OverlayCall::Status::ok) return SEGA_MOD_OK;
  const bool budget = r.status == luau::OverlayCall::Status::budget;
  genesis_mod_fault_current(budget ? "budget" : "range");
  return budget ? SEGA_MOD_E_BUDGET : SEGA_MOD_E_RANGE;
}
int api_overlay_layer(std::uint32_t index, std::uint32_t anchor, std::uint32_t reference, std::int32_t dx, std::int32_t dy, std::uint32_t w, std::uint32_t h, std::uint32_t z) {
  if (const int gate = overlay_gate(); gate != SEGA_MOD_OK) return gate;
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
int api_overlay_target(std::uint32_t index) { const int gate = overlay_gate(); return gate != SEGA_MOD_OK ? gate : answer(luau::overlay_select_target(g.staged, g.target, index)); }
int api_overlay_rect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba) {
  const int gate = overlay_gate();
  return gate != SEGA_MOD_OK ? gate : answer(luau::overlay_rect(g.staged, g.target, x, y, w, h, rgba));
}
int api_overlay_text(std::int32_t x, std::int32_t y, const char *ascii, std::uint32_t len, std::uint32_t rgba) {
  if (const int gate = overlay_gate(); gate != SEGA_MOD_OK) return gate;
  if (ascii == nullptr && len != 0U) { genesis_mod_fault_current("range"); return SEGA_MOD_E_RANGE; }
  return answer(luau::overlay_text(g.staged, g.target, x, y, std::string_view(ascii != nullptr ? ascii : "", len), rgba));
}
// SEG-152-T005 (ADR 0320 decision 6): the bounded RGBA bitmap calls: validation, clamping, budgets and notes are luau_api.cpp's (one implementation for the staged output the one compositor consumes).
int api_overlay_bitmap(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h, const std::uint8_t *rgba, std::uint32_t stride_bytes) {
  const int gate = overlay_gate();
  return gate != SEGA_MOD_OK ? gate : answer(luau::overlay_bitmap(g.staged, g.target, x, y, w, h, rgba, stride_bytes));
}
int api_overlay_layer_bitmap(std::uint32_t index, std::uint32_t anchor, std::uint32_t reference, std::int32_t dx, std::int32_t dy, std::uint32_t w, std::uint32_t h, std::uint32_t z) {
  if (const int gate = overlay_gate(); gate != SEGA_MOD_OK) return gate;
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
  const int gate = overlay_gate();
  return gate != SEGA_MOD_OK ? gate : answer(luau::overlay_layer_clip(g.staged, index, x, y, w, h));
}
int api_overlay_fallback(std::uint32_t x, std::uint32_t y) { const int gate = overlay_gate(); return gate != SEGA_MOD_OK ? gate : answer(luau::overlay_fallback(g.staged, x, y)); }
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
  if (const int gate = overlay_gate(); gate != SEGA_MOD_OK) return gate;   // documented as needing overlay.rgba
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
// SEG-152-T007: marks screen rows of one plane as answered by the VDP's own plane as a provider word (ABI 1 own_plane). Needs `scenery.words`, like scenery_window; validated before anything is staged.
int api_scenery_generic_rows(std::uint32_t plane, const std::uint8_t *mask, std::uint32_t rows, std::uint32_t mask_bytes) {
  if (!in_render()) return refused_phase();
  if (!sega_mod_is_granted(&g.scheduler->host, SEGA_MOD_CAP_SCENERY_WORDS)) { genesis_mod_fault_current("capability"); return SEGA_MOD_E_CAPABILITY; }
  if (!segarecomp::scenery::mask_in_range(plane, mask, rows, mask_bytes) || g.staged_row_mask_submitted[plane]) {
    genesis_mod_fault_current("range");
    return SEGA_MOD_E_RANGE;
  }
  segarecomp::scenery::mask_store(g.staged_row_mask[plane], mask, mask_bytes);
  g.staged_row_mask_submitted[plane] = true;
  return SEGA_MOD_OK;
}
// SEG-152-T017 (ABI minor 3): per-row world-origin delta in whole plane cells (see mod_scenery_window.hpp). Needs `scenery.words`; validated before anything is staged; one submission per plane per callback.
int api_scenery_row_origin(std::uint32_t plane, const std::int16_t *delta_cells, std::uint32_t rows, std::uint32_t bytes) {
  if (!in_render()) return refused_phase();
  if (!sega_mod_is_granted(&g.scheduler->host, SEGA_MOD_CAP_SCENERY_WORDS)) { genesis_mod_fault_current("capability"); return SEGA_MOD_E_CAPABILITY; }
  if (!segarecomp::scenery::origin_in_range(plane, delta_cells, rows, bytes) || g.staged_row_origin_submitted[plane]) {
    genesis_mod_fault_current("range");
    return SEGA_MOD_E_RANGE;
  }
  segarecomp::scenery::origin_store(g.staged_row_origin[plane], delta_cells, rows);
  g.staged_row_origin_submitted[plane] = true;
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
                                   api_overlay_bitmap, api_overlay_layer_bitmap, api_overlay_layer_clip, api_overlay_fallback, api_decline, api_scenery_generic_rows, api_scenery_row_origin};

// The provider's `phase` entry, wrapped: a render callback starts from an empty staging area, and what it staged becomes visible only if the callback finished without a fault (a faulted callback leaves nothing).
void wrapped_phase(void *state, const SegaModHost *host) {
  const bool render = host->phase == SEGA_MOD_PHASE_RENDER;
  if (render) {
    g.staged = luau::Output{}; g.target = 0; g.visible = luau::Output{}; g.have_staged_set = false; g.have_visible_set = false;
    for (auto &plane_windows : g.staged_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};
    for (auto &plane_windows : g.visible_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};   // nothing of an earlier frame survives the start of a render callback
    for (unsigned plane = 0; plane < 2U; ++plane) { g.staged_row_mask[plane] = segarecomp::scenery::RowMask{}; g.visible_row_mask[plane] = segarecomp::scenery::RowMask{}; g.staged_row_origin[plane] = segarecomp::scenery::RowOrigin{}; g.visible_row_origin[plane] = segarecomp::scenery::RowOrigin{}; g.staged_row_origin_submitted[plane] = false; g.staged_row_mask_submitted[plane] = false; }
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
    for (unsigned plane = 0; plane < 2U; ++plane) { std::swap(g.visible_row_mask[plane], g.staged_row_mask[plane]); std::swap(g.visible_row_origin[plane], g.staged_row_origin[plane]); }
    g.visible_scenery_frame = host->frame;
    g.visible_left_px = g.staged_left_px;
    g.visible_left_margin = g.staged_left_margin;
  }
  if (render) {
    g.staged_left_px = -1;
    g.staged = luau::Output{}; g.have_staged_set = false;
    for (auto &plane_windows : g.staged_scenery) for (Mod::SceneryWindow &w : plane_windows) w = Mod::SceneryWindow{};
    for (unsigned plane = 0; plane < 2U; ++plane) { g.staged_row_mask[plane] = segarecomp::scenery::RowMask{}; g.staged_row_mask_submitted[plane] = false; g.staged_row_origin[plane] = segarecomp::scenery::RowOrigin{}; g.staged_row_origin_submitted[plane] = false; }
  }
}
#endif

}  // namespace

extern "C" {

}  // extern "C"

namespace {

bool refuse_begin(const std::string &cls) {
  g.refused = true; g.refusal = cls;
  std::fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", cls.c_str());
  return false;
}

// The rows of the program's mods, in set order. A single-mod program describes its own artifact symbols as one row.
std::vector<SegarecompModMemberRow> artifact_rows() {
#ifdef SEGARECOMP_MOD_SET
  return std::vector<SegarecompModMemberRow>(segarecomp_mod_set_members, segarecomp_mod_set_members + segarecomp_mod_set_count);
#else
  SegarecompModMemberRow row{};
#ifdef SEGARECOMP_MOD_NATIVE
  row.id = segarecomp_mod_id; row.identity = segarecomp_mod_identity; row.capabilities = segarecomp_mod_capabilities;
  row.required[0] = segarecomp_mod_caps_required[0]; row.required[1] = segarecomp_mod_caps_required[1];
  row.optional[0] = segarecomp_mod_caps_optional[0]; row.optional[1] = segarecomp_mod_caps_optional[1];
  row.native_entry = segarecomp_mod_provider;
  row.rom_data = segarecomp_mod_rom_data;
#else
  row.id = segarecomp_luau_mod_id; row.identity = segarecomp_luau_identity; row.capabilities = segarecomp_luau_capabilities;
  row.luau_artifact = segarecomp_luau_artifact; row.luau_artifact_size = segarecomp_luau_artifact_size; row.luau_bytecode_digest = segarecomp_luau_bytecode_digest;
  row.luau_memory_bytes = segarecomp_luau_memory_bytes; row.luau_budget_units = segarecomp_luau_budget_units;
#endif
  row.state_schema = segarecomp_mod_state_schema;
  row.range_count = segarecomp_mod_range_count;
  row.ranges = segarecomp_mod_ranges;
  return {row};
#endif
}

// Loads one member's provider and fills its scheduler configuration. False = refused (reported).
bool configure(Member &m, bool lead, GenesisModConfig &c) {
  c = GenesisModConfig{};
  c.mod_id = m.row.id;
  if (m.native()) {
#ifdef SEGARECOMP_MOD_HAS_NATIVE
    void *native_state = nullptr;
    const SegaModProvider *native = m.row.native_entry(&native_state);
    if (native == nullptr || native->phase == nullptr) return refuse_begin("E_ABI_ENTRY_MISSING");
    std::memcpy(&m.native_table, native, native->size < sizeof m.native_table ? native->size : sizeof m.native_table);   // a table smaller than the ABI's is refused by the scheduler's negotiation (E_ABI_TABLE_SIZE), never read past
    if (lead) {   // only the drawing mod stages render output; the others have no render API (no presentation capability)
      g.native_phase_entry = native->phase;
      m.native_table.phase = wrapped_phase;
      c.render_api = &g_render_api;
    }
    c.provider = &m.native_table;
    c.provider_state = native_state;
    c.phase_mask = (1U << SEGA_MOD_PHASE_RESET) | (1U << SEGA_MOD_PHASE_FRAME_BEGIN) | (1U << SEGA_MOD_PHASE_INPUT) | (1U << SEGA_MOD_PHASE_POST_INPUT) | (1U << SEGA_MOD_PHASE_FRAME_END) | (1U << SEGA_MOD_PHASE_RENDER);
    c.required[0] = m.row.required[0]; c.required[1] = m.row.required[1];
    c.optional[0] = m.row.optional[0]; c.optional[1] = m.row.optional[1];
#ifdef SEGARECOMP_MOD_VIEWER
    c.vdp_helpers = &g_vdp_helpers;   // SEG-152-T004: the host's own pure VDP functions (ABI 1 GenesisCompatHost), re-exported through the ABI 2 Genesis table
#endif
#else
    return refuse_begin("E_PROVIDER_ENTRY");
#endif
  } else {
#ifdef SEGARECOMP_MOD_HAS_LUAU
    const std::span<const std::uint8_t> stored(m.row.luau_artifact, static_cast<std::size_t>(m.row.luau_artifact_size));
    const luau::CompileResult loaded = luau::load_stored(m.row.identity, stored, m.row.luau_bytecode_digest);
    if (!loaded.ok) return refuse_begin(loaded.rejection.code);
    luau::HostConfig host;
    for (const char *p = m.row.capabilities; *p != '\0';) {
      const char *end = std::strchr(p, ',');
      const std::string name = end != nullptr ? std::string(p, end) : std::string(p);
      if (!name.empty()) host.capabilities.push_back(name);
      if (end == nullptr) break;
      p = end + 1;
    }
    luau::Limits limits = luau::policy_limits();
    if (m.row.luau_memory_bytes != 0 && m.row.luau_memory_bytes < limits.memory_bytes) limits.memory_bytes = m.row.luau_memory_bytes;
    if (m.row.luau_budget_units != 0 && m.row.luau_budget_units < limits.callback_units) limits.callback_units = m.row.luau_budget_units;
    std::string error;
    m.luau = luau::PhaseProvider::create(m.row.identity, loaded.compiled, limits, host, [](const char *cause) { genesis_mod_fault_current(cause); }, error);
    if (!m.luau) return refuse_begin(error);
    c.provider = m.luau->provider();
    c.provider_state = m.luau->state();
    c.phase_mask = m.luau->phase_mask();
    m.luau->capabilities(c.required);
    if ((c.required[0] >> SEGA_MOD_CAP_FRAME_VDP & 1U) != 0U) return refuse_begin("E_CAPABILITY_UNSUPPORTED");   // ADR 0320: the exact-frame callback is a native ABI 2 feature
#else
    return refuse_begin("E_PROVIDER_ENTRY");
#endif
  }
#ifdef SEGARECOMP_MOD_HOOKS
  if (m.native()) {   // SEG-138-T012: every binding must name a site of the generated table (the build checked the same; a program must never run with a binding that can never fire), and the hook phases are registered with it
    std::size_t site_count = 0;
    const GenesisHookSiteInfo *sites = genesis_generated_hook_sites(&site_count);
#ifdef SEGARECOMP_MOD_SET
    const std::size_t hook_count = m.row.hook_count;
    const std::uint32_t hook_phases = static_cast<std::uint32_t>(m.row.hook_phases);
#else
    const std::size_t hook_count = segarecomp_mod_hook_count;
    const std::uint32_t hook_phases = static_cast<std::uint32_t>(segarecomp_mod_hook_phases);
#endif
    if (hook_count > GENESIS_MOD_HOOKS_MAX) return refuse_begin("E_HOOK_BINDING");
    for (std::size_t i = 0; i < hook_count; ++i) {
#ifdef SEGARECOMP_MOD_SET
      const char *id = m.row.hooks[i].id;
      m.bindings[i] = GenesisModHookBinding{id, static_cast<std::uint32_t>(m.row.hooks[i].hook_id)};
      m.scopes[i] = GenesisModHookScope{static_cast<std::uint32_t>(m.row.hooks[i].modes), static_cast<std::uint32_t>(m.row.hooks[i].reads)};
#else
      const char *id = segarecomp_mod_hooks[i].id;
      m.bindings[i] = GenesisModHookBinding{id, static_cast<std::uint32_t>(segarecomp_mod_hooks[i].hook_id)};
#endif
      bool found = false;
      for (std::size_t site = 0; site < site_count; ++site) found = found || std::strcmp(sites[site].id, id) == 0;
      if (!found) return refuse_begin("E_HOOK_BINDING");
    }
    if (hook_count != 0U) {
      c.hooks = m.bindings;
      c.hook_scopes = m.scopes;
      c.hook_count = static_cast<std::uint32_t>(hook_count);
      c.sites = sites;
      c.site_count = static_cast<std::uint32_t>(site_count);
      c.phase_mask |= hook_phases;
    }
  }
#endif
  std::uint32_t rom_bytes = 0;   // the ROM bytes are the sum of the ROM-space range lengths; the scheduler checks the sum against its bound and refuses a mismatch
  for (unsigned long i = 0; i < m.row.range_count && i < GENESIS_MOD_RANGES_MAX; ++i) {
    m.ranges[i] = GenesisModRange{static_cast<std::uint32_t>(m.row.ranges[i][0]), static_cast<std::uint32_t>(m.row.ranges[i][1]), static_cast<std::uint32_t>(m.row.ranges[i][2]), static_cast<std::uint32_t>(m.row.ranges[i][3])};
    if (m.ranges[i].space == SEGA_GEN_SPACE_M68K_ROM) rom_bytes += m.ranges[i].length;
  }
  c.ranges = m.ranges;
  c.range_count = static_cast<std::uint32_t>(m.row.range_count);
  c.rom_data = rom_bytes != 0 ? m.row.rom_data : nullptr;   // a Luau program has no ROM range (the capability is native-only), so rom_bytes is 0 there
  c.rom_data_bytes = rom_bytes;
#ifdef SEGARECOMP_MOD_AUDIO
  for (unsigned long i = 0; i < segarecomp_mod_audio_count && i < GENESIS_MOD_AUDIO_ENTRIES_MAX; ++i) {
    const segarecomp_mod_audio_row &r = segarecomp_mod_audio[i];
    g.audio[i] = GenesisModAudioEntry{r.mapping, r.name, static_cast<std::uint32_t>(r.kind), static_cast<std::uint32_t>(r.offset), static_cast<std::uint32_t>(r.width), static_cast<std::uint32_t>(r.value)};
  }
  c.audio = g.audio;
  c.audio_count = static_cast<std::uint32_t>(segarecomp_mod_audio_count);   // a count above the bound is refused by the scheduler, never truncated
#endif
  c.state_schema = static_cast<std::uint32_t>(m.row.state_schema);
  c.log_sink = log_sink;
  hex_bytes(m.row.identity, c.identity_sha256);
  return true;
}

bool draws(const SegarecompModMemberRow &row) {
  const std::string names = std::string(",") + row.capabilities + ",";
  return names.find(",overlay.rgba,") != std::string::npos || names.find(",scenery.words,") != std::string::npos || names.find(",frame.vdp,") != std::string::npos;
}

}  // namespace

extern "C" {

// 0 = the mods are attached; nonzero = refused (the program must not run a half-configured mod: the caller exits).
int segarecomp_capture_mod_begin(GenesisRuntime *runtime) {
  const std::vector<SegarecompModMemberRow> rows = artifact_rows();
  if (rows.empty() || rows.size() > GENESIS_MOD_SET_MEMBERS_MAX) { (void)refuse_begin("E_SET_SIZE"); return 1; }
  g.members.clear();
  for (const SegarecompModMemberRow &row : rows) {
    g.members.push_back(std::make_unique<Member>());
    g.members.back()->row = row;
  }
  g.lead = g.members.front().get();
  for (const auto &m : g.members)
    if (draws(m->row)) { g.lead = m.get(); break; }
  g.scheduler = g.lead->scheduler.get();
  std::vector<GenesisModConfig> configs(g.members.size());
  std::vector<GenesisModScheduler *> schedulers;
  for (std::size_t i = 0; i < g.members.size(); ++i) {
    if (!configure(*g.members[i], g.members[i].get() == g.lead, configs[i])) return 1;
    schedulers.push_back(g.members[i]->scheduler.get());
  }
#ifdef SEGARECOMP_MOD_HAS_LUAU
  g.provider = g.lead->luau.get();
#endif
  const char *why = "";
#ifdef SEGARECOMP_MOD_SET
  std::uint8_t identity[32];
  hex_bytes(segarecomp_mod_set_identity, identity);
  g.set = std::make_unique<GenesisModSet>();
  std::uint32_t member = 0;
  if (genesis_mod_set_attach(g.set.get(), runtime, schedulers.data(), configs.data(), static_cast<std::uint32_t>(configs.size()), identity, &why, &member) != 0) { (void)refuse_begin(why); return 1; }
#else
  if (genesis_mod_scheduler_attach(schedulers.front(), runtime, &configs.front(), &why) != 0) { (void)refuse_begin(why); return 1; }
#endif
  g.attached = true;
#ifdef SEGARECOMP_MOD_SET
  genesis_mod_set_snapshot_state(g.set.get(), &g.snapshot);
#else
  genesis_mod_snapshot_state(g.scheduler, &g.snapshot);
#endif
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
  for (const auto &m : g.members) {
    char hooks_text[32] = "";
#if defined(SEGARECOMP_MOD_HOOKS)
    if (m->native()) std::snprintf(hooks_text, sizeof hooks_text, ",\"hooks\":%u", static_cast<unsigned>(m->scheduler->config.hook_count));
#endif
    if (m->native())
      std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"kind\":\"native\",\"package_identity\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu%s}\n", m->row.id, m->row.identity, m->row.capabilities, m->row.range_count, hooks_text);
    else
      std::fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"package_identity\":\"%s\",\"bytecode_digest\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", m->row.id, m->row.identity, m->row.luau_bytecode_digest, m->row.capabilities, m->row.range_count);
  }
#ifdef SEGARECOMP_MOD_SET
  std::fprintf(stderr, "MOD_SET_LOADED {\"set_identity\":\"%s\",\"mods\":%zu}\n", segarecomp_mod_set_identity, g.members.size());
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
#ifdef SEGARECOMP_MOD_HAS_NATIVE
  g.exact_served = false;
#endif
  if ((frame->requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY)) == 0) return;
  if (!g.attached || g.scheduler->fault.faulted != 0U || g.scheduler->disabled != 0U) {   // a faulted mod presents nothing; the guest and the picture go on
    out->reason = GENESIS_COMPAT_FAULT;
    return;
  }
  unsigned accepted = 0;
#ifdef SEGARECOMP_MOD_HAS_NATIVE
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
    const int ran = genesis_mod_scheduler_present(g.scheduler, &ef);
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
    p.scenery = current && (g.visible_scenery[0][0].present || g.visible_scenery[1][0].present || g.visible_row_mask[0].any || g.visible_row_mask[1].any);   // ADR 0314: a frame is served when at least one plane has a window (an empty plane is a window of blank cells); a plane without one is left to the generic extension
    if (p.scenery) accepted |= GENESIS_COMPAT_SCENERY;
    // The windows of a callback that declared a placement were built for THAT view: when the placement is not valid for this frame's geometry they are not served either (they would answer the wrong
    // cells), so the scenery falls back to the generic extension. The HUD and sprite contributions are independent of the view and stay accepted. A provider that declared no placement is untouched.
    if (declared && !p.left_ok && p.scenery) { p.scenery = false; accepted &= ~static_cast<unsigned>(GENESIS_COMPAT_SCENERY); }
  }
#endif
  const RenderView view_now = render_view();
  std::uint32_t declined = view_now.declined;
#ifdef SEGARECOMP_MOD_HAS_NATIVE
  if (!current) declined = 0U;   // a refusal belongs to the frame of the callback that made it: the same epoch guard as the windows and the placement (a skipped callback, restore, rewind or any discontinuity drops it)
#endif   // SEG-152-T006: components the provider refused for this frame (never a fault): their output is discarded, the generic path serves them
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
#ifdef SEGARECOMP_MOD_HAS_NATIVE
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
#ifdef SEGARECOMP_MOD_HAS_NATIVE
  if (declined == (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES)) p.left_ok = false;   // a wholly declined frame is generic: no placement either
  if (p.left_ok) { out->left_px = static_cast<std::int32_t>(g.visible_left_px); out->has_left_px = 1U; }   // with the verdict OK even when no component is accepted (ADR 0315)
#endif
  if (accepted == 0U) return;
  const std::string &scene = *render_view().scene;
  if (!scene.empty() && scene.size() <= GENESIS_COMPAT_SCENE_MAX) std::memcpy(out->scene, scene.c_str(), scene.size() + 1U);
}
// SEG-140-T011: answers only from the committed window of the frame `prepare` accepted; a cell outside it is a failed answer (the host then withdraws the frame's authored-scenery claim), never a guess.
int scenery_word(void *, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
#ifdef SEGARECOMP_MOD_HAS_NATIVE
  if (!p.scenery || plane > 1U || word == nullptr) return 0;
  return segarecomp::scenery::answer(g.visible_row_mask[plane], g.visible_scenery[plane], static_cast<int>(row), x, hscroll, vscroll, word, GENESIS_COMPAT_WORD_VDP_START, GENESIS_COMPAT_WORD_GENERIC, &g.visible_row_origin[plane]);   // SEG-152-T017: the per-row origin delta; SEG-152-T007: a marked screen row is the VDP's own word as a provider word (honoured only on this adapter's trusted attach); ADR 0314: a plane without a window is left to the generic extension
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

}  // extern "C"

namespace {
// One mod's report as a JSON object. The checkpoint, loaded-state and overlay fields describe the whole program in a single-mod run; a set reports them once (MOD_SET_SUMMARY).
std::string member_summary(const Member &m, bool lead) {
  GenesisModScheduler &s = *m.scheduler;
  std::uint8_t digest[32];
  genesis_mod_write_log_digest(&s, digest);
  std::size_t op_count = 0;
  std::string overlay_hex;
  if (lead) {
    const RenderView view = render_view();
    op_count = view.ops->size();
    if (op_count != 0U) {
      const luau::OverlayImage image = luau::rasterize_overlay(*view.ops);
      overlay_hex = sha256_hex(std::span<const std::uint8_t>(image.rgba.data(), image.rgba.size()));
    }
  }
  bool nondeterministic = false;
#ifdef SEGARECOMP_MOD_HAS_LUAU
  nondeterministic = m.luau && m.luau->nondeterministic();
#endif
  char text[1024];
  std::snprintf(text, sizeof text, "\"mod\":\"%s\",\"faulted\":%s,\"fault\":\"%s\",\"callbacks\":%llu,\"committed_writes\":%llu,\"write_digest\":\"%s\",\"overlay_ops\":%llu,\"overlay_sha256\":\"%s\",\"nondeterministic\":%s",
                m.row.id, s.fault.faulted != 0U ? "true" : "false", s.fault.faulted != 0U ? json_escaped(s.fault.text).c_str() : "", static_cast<unsigned long long>(s.callbacks_run),
                static_cast<unsigned long long>(s.write_total), hex(digest, 32).c_str(), static_cast<unsigned long long>(op_count), overlay_hex.c_str(), nondeterministic ? "true" : "false");
  return text;
}
// SEG-138-T012: the hook counters (callbacks delivered by phase, and the loud non-altering skips: abandoned, unproven caller, frame stack full); only a hooked mod reports them
std::string hook_summary(const Member &m) {
#ifdef SEGARECOMP_MOD_HOOKS
  if (!m.native() || m.scheduler->config.hook_count == 0U) return {};
  const GenesisModScheduler &s = *m.scheduler;
  char text[240];
  std::snprintf(text, sizeof text, ",\"hooks\":{\"entry\":%llu,\"exit\":%llu,\"abandoned\":%llu,\"unproven\":%llu,\"overflowed\":%llu,\"checkpoint_depth\":%u}", static_cast<unsigned long long>(s.hook_calls[0]), static_cast<unsigned long long>(s.hook_calls[1]),
                static_cast<unsigned long long>(s.hook_skips[1]), static_cast<unsigned long long>(s.hook_skips[2]), static_cast<unsigned long long>(s.hook_skips[3]), static_cast<unsigned>(g.checkpoint_depth));
  return text;
#else
  (void)m;
  return {};
#endif
}
}  // namespace

extern "C" {

// After the run: the MOD_SUMMARY line (a set: MOD_SET_SUMMARY with one entry per mod in set order) and the detach.
void segarecomp_capture_mod_end(GenesisRuntime *runtime) {
  (void)runtime;
  if (!g.attached) return;
  char state[512];
  std::snprintf(state, sizeof state, "\"roundtrip\":{\"requested\":%s,\"frame\":%llu,\"status\":\"%s\",\"state_sha256\":\"%s\"},\"loaded_state_sha256\":\"%s\"", g.checkpoint_wanted ? "true" : "false",
                static_cast<unsigned long long>(g.checkpoint_frame), g.checkpoint_wanted ? (g.checkpoint_done ? g.checkpoint_status.c_str() : "not_reached") : "none", g.checkpoint_digest.c_str(), g.loaded_digest.c_str());
#ifdef SEGARECOMP_MOD_SET
  std::string members;
  for (const auto &m : g.members) members += std::string(members.empty() ? "{" : ",{") + member_summary(*m, m.get() == g.lead) + hook_summary(*m) + "}";
  std::fprintf(stderr, "MOD_SET_SUMMARY {\"set_identity\":\"%s\",\"members\":[%s],%s}\n", segarecomp_mod_set_identity, members.c_str(), state);
  genesis_mod_set_detach(g.set.get());
#else
  std::fprintf(stderr, "MOD_SUMMARY {%s,%s%s}\n", member_summary(*g.lead, true).c_str(), state, hook_summary(*g.lead).c_str());
  genesis_mod_scheduler_detach(g.scheduler);
#endif
}

}  // extern "C"
