// SEG-152-T008: the ABI 1 / ABI 2 presentation parity gate (Exit C). It EXTENDS the one comparison harness of SEG-152-T001 (abi_parity_harness.hpp: fixture `Spec`, `Scene`, `produce`, the surface digest, the twin runner
// `arm_twin` and the classifier) and runs T001's and T007's whole suite as its first section (abi_parity_scenery_test.cpp is included, not copied: one set of fixtures, one failure counter). It adds what those
// suites left out: the PRESENTATION of a frame, not only its scenery words. The same project-authored scenes (per-line parallax, split scroll, per-line palettes, a HUD drawn from the game's own tiles, a fade, a
// scene the provider declines, a refused HUD, a plane left generic) are produced twice by the real extension producer (`genesis_vdp_produce_extension`, the code the viewer runs):
//   * an ABI 1 provider (`GenesisNativeCompat`: prepare / scenery_word / hud writing straight into `GenesisCompatOverlay`, the shape of the Sonic providers), and
//   * an ABI 2 twin written to the SAME game logic (`make_plan`): it reads the same frame view, answers scenery through the bounded windows and the per-row mask of the production `scenery::answer`, submits its
//     HUD through the PRODUCTION bitmap/overlay rules (`luau::overlay_bitmap`, `overlay_declare_bitmap_layer`, `overlay_layer_clip`, `overlay_fallback`, `overlay_scene`, `rasterize_*`, `pack_hud`: the very
//     functions the scheduled-mod adapter in platforms/genesis/viewer/mod_capture.cpp calls) and refuses through the adapter's decline semantics.
// The two presentations are compared component by component (accepted components, reason, surface, overlay, layers, scene token, placement, authored coverage) at 4:3 / 16:9 / 21:9 / 32:9 over the placement matrix
// of ADR 0315, and every (scene, aspect) combination has a committed golden digest (abi_parity_gate_goldens.inc: synthetic content only, exact equality, no tolerance). Falsification mutants of the ABI 2 twin
// (a stale scroll, a stale cache key, a stale placement, a skipped callback, a missing refusal, a wrong reason, an oversized bitmap) must each fail a NAMED assertion. Everything the model does not run (the
// SDL3 viewer, the compositor, package building and execution) is the viewer-level suites' (mod_parity_gate_abi2_test.py and the existing mod_*_abi2_test suites).
#define ABI_PARITY_NO_MAIN
#include "abi_parity_scenery_test.cpp"

#include "luau_hud_pack.hpp"
#include "segarecomp/luau_api.hpp"

#include <chrono>
#include <cinttypes>
#include <cstring>
#include <map>
#include <set>

namespace {
namespace luau = segarecomp::luau;

// Index 0 is Original / exact 4:3: the producer still runs for a verified HUD (planned side 39) but nothing of the extension is presented, the view reports side 0, the provider is asked no scenery and requests no placement (ADR 0315).
constexpr int kSide[4] = {39, 39, 101, 238};
const char *const kAspectName[4] = {"4:3", "16:9", "21:9", "32:9"};

// ---- the shared game logic (what a real provider would compute from the frame view) ---------------------------------------------------------------------------------------------------------------------------
struct Bitmap { unsigned w = 0, h = 0; std::vector<uint8_t> px; };
struct LayerArt { unsigned index = 0, anchor = 0, reference = 0, z = 0; int dx = 0, dy = 0; Bitmap bm; bool clip = false; unsigned cx = 0, cy = 0, cw = 0, ch = 0; };
struct Plan {
  unsigned serve = 0, declined = 0;           // components served / refused for this frame
  uint8_t reason = GENESIS_COMPAT_OK;         // the diagnostic when nothing is served
  std::string scene;
  Bitmap fixed;                               // the fixed overlay art (empty = no fixed overlay)
  unsigned fx = 0, fy = 0;                    // the ABI 1 fallback position
  std::vector<LayerArt> layers;
};

// A tile of the game's own art scaled by `k`, coloured with the CRAM of output `line` of the frame view through the host helper (palette line 0), colour index 0 = transparent.
Bitmap tile_art(const GenesisCompatFrame &f, unsigned tile, unsigned k, unsigned line, bool half_alpha) {
  Bitmap b; b.w = b.h = 8U * k; b.px.assign(static_cast<size_t>(b.w) * b.h * 4U, 0);
  const unsigned ln = line < f.line_count ? line : f.line_count - 1U;
  for (unsigned y = 0; y < b.h; ++y)
    for (unsigned x = 0; x < b.w; ++x) {
      const unsigned sx = x / k, sy = y / k;
      const uint8_t byte = f.vram[tile * 32U + sy * 4U + sx / 2U];
      const unsigned n = (sx & 1U) != 0U ? (byte & 15U) : (byte >> 4);
      uint8_t *o = &b.px[(static_cast<size_t>(y) * b.w + x) * 4U];
      uint8_t rgb[3] = {200, 100, 50};
      if (n == 0U) { o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2]; o[3] = 0; continue; }
      (void)f.host->cram_rgb(f.lines[ln].cram, n, rgb);
      o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2]; o[3] = (half_alpha && n == 7U) ? 128U : 255U;
    }
  return b;
}
// The fixed overlay: the four HUD tiles at 2x; the top half of the strip is coloured with the palette of the first line and the bottom half with the palette of the last line (a per-line palette effect).
Bitmap fixed_art(const GenesisCompatFrame &f) {
  Bitmap b; b.w = 64; b.h = 16; b.px.assign(static_cast<size_t>(b.w) * b.h * 4U, 0);
  for (unsigned t = 0; t < 4; ++t) {
    const Bitmap top = tile_art(f, kHudTile0 + t, 2, 0, false), bottom = tile_art(f, kHudTile0 + t, 2, f.line_count - 1U, false);
    for (unsigned y = 0; y < 16; ++y)
      for (unsigned x = 0; x < 16; ++x)
        std::memcpy(&b.px[(static_cast<size_t>(y) * b.w + t * 16U + x) * 4U], &(y < 8U ? top : bottom).px[(static_cast<size_t>(y) * 16U + x) * 4U], 4);
  }
  return b;
}

// The game's decision for a frame, from the work RAM snapshot of the previous onset and the frame view only: byte 0 = scene (0 main, 1 pause: HUD only, 2 unsupported), byte 2 = fade counter, byte 3 = HUD state bad.
// `ignore_refusals` is the falsification mutant "the provider forgot to refuse".
Plan make_plan(const GenesisCompatFrame &f, bool ignore_refusals = false, uint8_t wrong_reason = 0) {
  Plan p;
  const uint8_t scene = f.work_ram[0], fade = f.work_ram[2], bad_hud = f.work_ram[3];
  const unsigned want = f.requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY);
  p.serve = want;
  p.scene = scene == 1U ? "pause" : "main";
  if (!ignore_refusals) {
    if (scene == 2U) { p.serve = 0; p.reason = GENESIS_COMPAT_UNSUPPORTED_SCENE; }
    else if (fade != 0U) { p.serve = 0; p.reason = GENESIS_COMPAT_MISMATCH; }
    else {
      if (scene == 1U) p.serve &= ~static_cast<unsigned>(GENESIS_COMPAT_SCENERY);
      if (bad_hud != 0U) { p.serve &= ~static_cast<unsigned>(GENESIS_COMPAT_HUD); p.reason = GENESIS_COMPAT_BAD_STATE; }
    }
    if (wrong_reason != 0U && p.serve == 0U) p.reason = wrong_reason;
  }
  p.declined = want & ~p.serve;
  if (p.serve == 0U) p.scene.clear();
  if ((p.serve & GENESIS_COMPAT_HUD) != 0U) {
    p.fixed = fixed_art(f); p.fx = 8; p.fy = 200;
    { LayerArt l; l.index = 1; l.anchor = 2; l.reference = 0; l.z = 0; l.dx = -4; l.dy = 8; l.bm = tile_art(f, kHudTile0, 4, 100, true); l.clip = true; l.cx = 4; l.cy = 4; l.cw = 24; l.ch = 24; p.layers.push_back(l); }
    { LayerArt l; l.index = 2; l.anchor = 8; l.reference = 1; l.z = 1; l.dx = -2; l.dy = -2; l.bm = tile_art(f, kHudTile0 + 3, 2, 0, false); p.layers.push_back(l); }
    { LayerArt l; l.index = 3; l.anchor = 4; l.reference = 0; l.z = 2; l.dx = 0; l.dy = 0; l.bm = tile_art(f, kHudTile0 + 1, 1, f.line_count - 1U, false); p.layers.push_back(l); }
  }
  return p;
}

// ---- the twins -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Mutant { None, StaleScroll, StaleCacheKey, StalePlacement, SkippedCallback, MissingRefusal, WrongReason, OversizedBitmap };
const char *mutant_name(Mutant m) {
  switch (m) { case Mutant::None: return "none"; case Mutant::StaleScroll: return "stale-scroll"; case Mutant::StaleCacheKey: return "stale-cache-key"; case Mutant::StalePlacement: return "stale-placement";
               case Mutant::SkippedCallback: return "skipped-callback"; case Mutant::MissingRefusal: return "missing-refusal"; case Mutant::WrongReason: return "wrong-reason"; default: return "oversized-bitmap"; }
}

struct Twin : Provider {   // a Provider (the harness's scenery state) plus the presentation state of one twin
  bool abi2 = false;
  Mutant mutant = Mutant::None;
  bool original = false;       // Original / 4:3: the view reports side 0 (no placement request); the host asks no scenery
  unsigned fault_at = 0;       // > 0: the Nth scenery_word call is unanswered (a genuine provider failure), in either twin
  unsigned prepared = 0;
  // ABI 2 adapter model: the staged output of the callback, packed into the host overlay by `hud`.
  bool have = false, fixed = false;
  luau::OverlayImage image;
  luau::RasterizedLayers layers;
  unsigned fallback_x = 0, fallback_y = 0;
  unsigned bitmap_bytes = 0, rejected_calls = 0;
  bool faulted = false;         // an invalid render-API call faults the callback: staged output is discarded and the mod stays disabled (the scheduler's rule; the host then reports GENESIS_COMPAT_FAULT)
  Plan plan;
};

int gate_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  Twin *t = static_cast<Twin *>(state);
  if (t->fault_at != 0U && t->cells + 1U == t->fault_at) { ++t->cells; return 0; }
  return provider_word(state, plane, row, x, hscroll, vscroll, word);
}
void set_decision(Twin *t, const Plan &plan, GenesisCompatDecision *out, unsigned served) {
  out->accepted = served;
  out->reason = plan.reason;
  if (served != 0U) {
    std::memcpy(out->scene, plan.scene.c_str(), plan.scene.size() + 1U);
    if (!t->original) { out->left_px = t->declared_left; out->has_left_px = 1; }
  }
}

// ABI 1: the reference provider writes straight into the host's overlay (the Sonic providers' shape).
void prepare1(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Twin *t = static_cast<Twin *>(state);
  t->plan = make_plan(*f);
  set_decision(t, t->plan, out, t->plan.serve);
}
int hud1(void *state, GenesisCompatOverlay *out) {
  Twin *t = static_cast<Twin *>(state);
  const Plan &p = t->plan;
  if (p.fixed.w == 0U) return 0;
  for (unsigned y = 0; y < p.fixed.h; ++y) std::memcpy(out->rgba + static_cast<size_t>(y) * GENESIS_COMPAT_OVERLAY_MAX_W * 4U, &p.fixed.px[static_cast<size_t>(y) * p.fixed.w * 4U], static_cast<size_t>(p.fixed.w) * 4U);
  out->width = GENESIS_COMPAT_OVERLAY_MAX_W; out->height = GENESIS_COMPAT_OVERLAY_MAX_H;
  out->fallback_x = static_cast<uint16_t>(p.fx); out->fallback_y = static_cast<uint16_t>(p.fy);
  size_t offset = 0;
  out->layer_count = 0;
  for (const LayerArt &l : p.layers) {
    std::memcpy(out->layer_rgba + offset, l.bm.px.data(), l.bm.px.size());
    GenesisCompatLayer &d = out->layers[out->layer_count++];
    std::memset(&d, 0, sizeof d);
    d.width = static_cast<uint16_t>(l.bm.w); d.height = static_cast<uint16_t>(l.bm.h); d.anchor = static_cast<uint8_t>(l.anchor); d.reference = static_cast<uint8_t>(l.reference); d.z = static_cast<uint8_t>(l.z);
    d.dx = static_cast<int16_t>(l.dx); d.dy = static_cast<int16_t>(l.dy); d.offset = static_cast<uint32_t>(offset);
    if (l.clip) { d.has_clip = 1; d.clip_x = static_cast<uint16_t>(l.cx); d.clip_y = static_cast<uint16_t>(l.cy); d.clip_w = static_cast<uint16_t>(l.cw); d.clip_h = static_cast<uint16_t>(l.ch); }
    offset += l.bm.px.size();
  }
  return 1;
}

// ABI 2: the same plan submitted through the production render API rules and packed by the production packer.
void prepare2(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Twin *t = static_cast<Twin *>(state);
  ++t->prepared;
  t->have = false; t->fixed = false; t->layers.clear(); t->image = luau::OverlayImage{}; t->bitmap_bytes = 0; t->rejected_calls = 0;
  if (t->faulted) { out->accepted = 0; out->reason = GENESIS_COMPAT_FAULT; return; }   // a disabled mod presents nothing
  if (t->mutant == Mutant::SkippedCallback) { out->accepted = 0; out->reason = GENESIS_COMPAT_STALE_STATE; return; }   // the callback never ran for this frame
  t->plan = make_plan(*f, t->mutant == Mutant::MissingRefusal, t->mutant == Mutant::WrongReason ? static_cast<uint8_t>(GENESIS_COMPAT_BAD_STATE) : 0);
  const Plan &p = t->plan;
  luau::Output o;
  const auto call = [&](const luau::OverlayCall &c) { if (c.status != luau::OverlayCall::Status::ok) { ++t->rejected_calls; t->faulted = true; } return c.status == luau::OverlayCall::Status::ok; };   // production semantics: an invalid call is a fault of the whole callback (mod_capture.cpp `answer`)
  if (!p.fixed.px.empty()) {
    (void)call(luau::overlay_fallback(o, p.fx, p.fy));
    (void)call(luau::overlay_bitmap(o, 0, 0, 0, p.fixed.w, p.fixed.h, p.fixed.px.data(), static_cast<uint64_t>(p.fixed.w) * 4U));
    for (const LayerArt &l : p.layers) {
      unsigned w = l.bm.w, h = l.bm.h;
      std::vector<uint8_t> big;
      const uint8_t *px = l.bm.px.data();
      if (t->mutant == Mutant::OversizedBitmap && l.index == 2U) { w = 257; h = 16; big.assign(static_cast<size_t>(w) * h * 4U, 0x55); px = big.data(); }   // one pixel wider than a bitmap layer may be
      luau::OverlayLayerDecl d; d.index = static_cast<int>(l.index); d.anchor = static_cast<int>(l.anchor); d.reference = static_cast<int>(l.reference); d.dx = l.dx; d.dy = l.dy; d.w = static_cast<int>(w); d.h = static_cast<int>(h); d.z = static_cast<int>(l.z);
      int target = 0;
      if (!call(luau::overlay_declare_bitmap_layer(o, d))) break;   // the callback faulted: nothing after the invalid call runs
      (void)call(luau::overlay_select_target(o, target, l.index));
      (void)call(luau::overlay_bitmap(o, target, 0, 0, w, h, px, static_cast<uint64_t>(w) * 4U));
      if (l.clip) (void)call(luau::overlay_layer_clip(o, l.index, l.cx, l.cy, l.cw, l.ch));
    }
  }
  if (!t->faulted && !p.scene.empty()) (void)call(luau::overlay_scene(o, p.scene));   // a refused frame names no scene
  if (t->faulted) { out->accepted = 0; out->reason = GENESIS_COMPAT_FAULT; return; }   // staged output of a faulted callback is discarded: no HUD, no layer, no scenery, no placement, no scene token
  t->bitmap_bytes = static_cast<unsigned>(o.bitmap_bytes);
  unsigned served = p.serve & ~p.declined;
  if ((served & GENESIS_COMPAT_HUD) != 0U) {
    if (o.overlay.empty()) served &= ~static_cast<unsigned>(GENESIS_COMPAT_HUD);   // nothing was staged: the adapter accepts the HUD component only for staged output
    else {
      t->image = luau::rasterize_overlay(o.overlay);
      for (const luau::OverlayOp &op : o.overlay) t->fixed = t->fixed || op.layer == 0;
      for (const luau::OverlayLayerDecl &d : o.layers) t->layers.emplace_back(d, luau::rasterize_layer(d, o.overlay));
      t->fallback_x = static_cast<unsigned>(o.fallback_x); t->fallback_y = static_cast<unsigned>(o.fallback_y);
      t->have = true;
    }
  }
  set_decision(t, p, out, served);
}
int hud2(void *state, GenesisCompatOverlay *out) {
  Twin *t = static_cast<Twin *>(state);
  return luau::pack_hud(t->have, t->fixed, t->image, t->layers, out, static_cast<int>(t->fallback_x), static_cast<int>(t->fallback_y)) != 0 ? 1 : 0;
}

GenesisNativeCompat table1 = {GENESIS_NATIVE_COMPAT_ABI, "parity.gate-abi1", 1, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, prepare1, gate_word, hud1};
GenesisNativeCompat table2 = {GENESIS_NATIVE_COMPAT_ABI, "scheduled-mod-host", 1, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, prepare2, gate_word, hud2};

// ---- the ABI 2 builder cache (provider-side warm state) ---------------------------------------------------------------------------------------------------------------------------------------------------------
uint64_t spec_key(const Spec &s, bool with_scroll) {
  uint64_t h = fnv1a64(reinterpret_cast<const uint8_t *>(s.vs), sizeof s.vs);
  if (with_scroll) h = fnv1a64(reinterpret_cast<const uint8_t *>(s.hs), sizeof s.hs, h);
  for (const auto *a : {&s.k[0][0], &s.k[1][0]}) h = fnv1a64(reinterpret_cast<const uint8_t *>(a), sizeof(int) * H, h);
  h = fnv1a64(&s.fallback[0][0], sizeof s.fallback, h);
  h = fnv1a64(&s.generic2[0][0], sizeof s.generic2, h);
  return h;
}
struct BuilderCache {   // what an ABI 2 provider keeps between frames: the windows it built for a (scroll, placement) key
  std::map<std::tuple<uint64_t, int, int>, Built> built;
  void clear() { built.clear(); }
};

struct Pair {
  Produced a, b;
  std::vector<std::string> mismatches;
  unsigned abi1_calls = 0, abi2_calls = 0, words = 0, bitmap_bytes = 0, rejected = 0;
  double ms1 = 0, ms2 = 0;
};

uint8_t fade_counter(const Spec &s) { return s.fade; }

Pair run_pair(const Spec &s, int side, int left, Mutant m = Mutant::None, BuilderCache *cache = nullptr, unsigned fault_at = 0, int stale_left = -1, bool original = false) {
  Scene sc;
  build_scene(sc, s);
  const bool hud = s.hud_tiles;
  Twin ref, twin;
  for (Twin *t : {&ref, &twin}) {
    t->spec = &s; t->scene = &sc; t->declared_left = left; t->original = original; t->components = (original ? 0U : GENESIS_COMPAT_SCENERY) | (hud ? GENESIS_COMPAT_HUD : 0U);
    t->ram[0] = s.scene_id; t->ram[1] = 0; t->ram[2] = fade_counter(s); t->ram[3] = s.bad_hud; t->fault_at = fault_at;
  }
  ref.kind = Provider::Abi1; ref.table = &table1;
  twin.kind = Provider::Abi2; twin.table = &table2; twin.abi2 = true; twin.mutant = m;
  Options opt; opt.use_mask = true; opt.use_origin = s.origin_delta;
  Spec built_spec = s;
  if (m == Mutant::StaleScroll)   // the provider built its windows from the scroll of the previous frame (3 px behind)
    for (unsigned p = 0; p < 2; ++p) for (int r = 0; r < H; ++r) built_spec.hs[p][r] = wrap10(built_spec.hs[p][r] + 3);
  const int built_left = stale_left >= 0 ? stale_left : left;   // the placement the provider built its windows for (the declared one unless a case says otherwise)
  Built built;
  const std::tuple<uint64_t, int, int> key{spec_key(built_spec, m != Mutant::StaleCacheKey), side, built_left};
  if (cache != nullptr && cache->built.count(key) != 0U) built = cache->built.at(key);   // warm: the provider reuses what it built
  else { Provider scratch; arm_twin(scratch, built_spec, sc, side, built_left, Timing::Exact, opt, built); if (cache != nullptr) cache->built[key] = built; }
  for (unsigned plane = 0; plane < 2; ++plane) {
    for (unsigned n = 0; n < 2; ++n) twin.windows[plane][n] = built.w[plane][n];
    uint8_t bytes[(H + 7) / 8] = {};
    for (int r = 0; r < H; ++r) if (s.fallback[plane][r] != 0U) bytes[r >> 3] = static_cast<uint8_t>(bytes[r >> 3] | (1U << (r & 7)));
    segarecomp::scenery::mask_store(twin.mask[plane], bytes, (H + 7) / 8);
    if (s.origin_delta) {   // SEG-152-T017: delta[r] = 64 k[r] whole plane cells, the submission of an ABI minor 3 provider (cached windows are world-keyed)
      int16_t delta[H];
      for (int r = 0; r < H; ++r) delta[r] = static_cast<int16_t>(64 * s.k[plane][r]);
      segarecomp::scenery::origin_store(twin.origin[plane], delta, H);
    }
  }
  Pair out;
  const auto t0 = std::chrono::steady_clock::now();
  out.a = produce(sc, ref, static_cast<unsigned>(side));
  const auto t1 = std::chrono::steady_clock::now();
  out.b = produce(sc, twin, static_cast<unsigned>(side));
  const auto t2 = std::chrono::steady_clock::now();
  out.ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count(); out.ms2 = std::chrono::duration<double, std::milli>(t2 - t1).count();
  out.mismatches = presentation_mismatches(out.a, out.b);
  out.abi1_calls = ref.cells; out.abi2_calls = twin.cells; out.words = built.total_words; out.bitmap_bytes = twin.bitmap_bytes; out.rejected = twin.rejected_calls;
  return out;
}

// ---- the presentation scene set ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Spec gate_base(const char *id) { Spec s = base_spec(id); s.hud_tiles = true; return s; }
struct GateScene { Spec spec; const char *what; unsigned want_accepted; uint8_t want_reason; };
const unsigned kBoth = GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY;
std::vector<GateScene> gate_scenes() {
  std::vector<GateScene> v;
  { Spec s = gate_base("parallax-hud"); set_plane(s, 1, [](int r) { return -(100 + (r / 38) * 40); }); v.push_back({s, "per-line parallax plane, HUD from game tiles", kBoth, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("split-scroll-hud"); set_plane(s, 0, [](int r) { return r < 160 ? -700 : -20; }); set_plane(s, 1, [](int r) { return r < 160 ? -350 : -40; }); s.vs[1] = 9; v.push_back({s, "split scroll (two camera regions) with a vertical scroll", kBoth, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("line-palette"); for (int r = 0; r < H; ++r) s.pal[r] = static_cast<uint8_t>((r / 32) % 4); v.push_back({s, "per-line palette effect: the HUD reads the first and the last line's CRAM", kBoth, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("fade-decline"); s.fade = 3; v.push_back({s, "a fade in progress: the frame is declined as a mismatch", 0U, GENESIS_COMPAT_MISMATCH}); }
  { Spec s = gate_base("unsupported-scene"); s.scene_id = 2; v.push_back({s, "a scene the provider is not verified for", 0U, GENESIS_COMPAT_UNSUPPORTED_SCENE}); }
  { Spec s = gate_base("pause-hud-only"); s.scene_id = 1; v.push_back({s, "a scene whose scenery is left generic: only the HUD is served", GENESIS_COMPAT_HUD, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("bad-hud"); s.bad_hud = 1; for (int r = 100; r < H; ++r) s.fallback[1][r] = 1; v.push_back({s, "HUD state refused (BAD_STATE), scenery served with a per-row generic mask", GENESIS_COMPAT_SCENERY, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("generic-plane"); for (int r = 0; r < H; ++r) { s.generic2[0][r] = 1; s.fallback[1][r] = (r % 3 == 0) ? 1 : 0; } v.push_back({s, "plane A left to the generic extension, plane B mixed rows", kBoth, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("origin-bands-hud"); s.origin_delta = true; set_plane(s, 1, [](int) { return -150; }, [](int r) { return r < 100 ? 0 : 1; }); set_plane(s, 0, [](int r) { return -(300 + (r / 40) * 16); }, [](int r) { return (r / 3) % 2; }); v.push_back({s, "ABI 2 minor 3: line-dependent world origins on both planes (per-row origin delta), HUD from game tiles", kBoth, GENESIS_COMPAT_OK}); }
  { Spec s = gate_base("window-band-hud"); s.window_from_row = 192; set_plane(s, 1, [](int r) { return -(60 + (r / 56) * 30); }); v.push_back({s, "window-plane status band rows with the HUD", kBoth, GENESIS_COMPAT_OK}); }
  return v;
}

std::vector<int> placements(int side) { return side == 0 ? std::vector<int>{0} : std::vector<int>{0, side / 2, side, side + side / 2, 2 * side}; }   // left-aligned, intermediate, centred, intermediate, right-aligned

struct Golden { const char *id; uint64_t digest; };
const Golden kGoldens[] = {
#include "abi_parity_gate_goldens.inc"
};

std::set<std::string> gate_ran;
bool print_goldens = false;
uint64_t golden_digest(const std::string &id, uint64_t d) {
  if (print_goldens) std::printf("    {\"%s\", 0x%016" PRIx64 "ULL},\n", id.c_str(), d);
  return d;
}
void check_golden(const std::string &id, uint64_t d) {
  golden_digest(id, d);
  if (print_goldens) return;
  for (const Golden &g : kGoldens) if (id == g.id) { check(g.digest == d, "golden digest of " + id + " (expected " + std::to_string(g.digest) + ", got " + std::to_string(d) + ")"); return; }
  check(false, "a golden digest exists for " + id);
}

void presentation_matrix() {
  unsigned cases = 0, served = 0, refused = 0;
  for (const GateScene &g : gate_scenes())
    for (int ai = 0; ai < 4; ++ai) {
      const int side = kSide[ai];
      const bool original = ai == 0;
      uint64_t combined = 14695981039346656037ULL;
      const unsigned want_acc = original ? (g.want_accepted & GENESIS_COMPAT_HUD) : g.want_accepted;
      const uint8_t want_reason = want_acc == 0U && g.want_accepted != 0U ? static_cast<uint8_t>(GENESIS_COMPAT_BAD_STATE) : g.want_reason;   // Original asks no scenery: a scene whose only served component is scenery is declined (the HUD refusal's reason)
      for (const int left : original ? std::vector<int>{side} : placements(side)) {
        const Pair p = run_pair(g.spec, side, left, Mutant::None, nullptr, 0, -1, original);
        ++cases;
        char what[200];
        std::snprintf(what, sizeof what, "%s %s left_px=%d", g.spec.id.c_str(), kAspectName[ai], left);
        check(p.a.rc == 0 && p.b.rc == 0, std::string("both twins produce a frame: ") + what);
        check(p.mismatches.empty(), std::string("ABI 2 presentation equals ABI 1 (first difference: ") + (p.mismatches.empty() ? "none" : p.mismatches[0]) + "): " + what);
        check(p.a.res.compat_accepted == want_acc && p.a.res.compat_reason == (want_acc != 0U ? static_cast<uint8_t>(GENESIS_COMPAT_OK) : want_reason), std::string("the verdict is the scene's (accepted ") + std::to_string(p.a.res.compat_accepted) + ", reason " + std::to_string(p.a.res.compat_reason) + "): " + what);
        check(p.abi1_calls == p.abi2_calls, std::string("no additional host cell queries: ") + what);
        if (want_acc == 0U) { ++refused; check(p.a.res.overlay_w == 0U && p.a.res.layer_count == 0U && p.b.res.overlay_w == 0U && p.b.res.layer_count == 0U && p.a.res.shift == 0, std::string("a refused frame presents no HUD and no placement: ") + what); }
        else ++served;
        if ((want_acc & GENESIS_COMPAT_HUD) != 0U) check(p.b.res.overlay_w == 128U && p.b.res.layer_count == 3U && p.b.res.layer_bad == 0U && !p.b.overlay.empty() && !p.b.layers.empty(), std::string("the HUD and its three layers are presented: ") + what);
        else if (want_acc != 0U) check(p.b.res.overlay_w == 0U && p.b.res.layer_count == 0U, std::string("a refused HUD presents no overlay and no layer, the scenery stays: ") + what);
        if (!original && want_acc != 0U) check(p.a.res.shift == p.b.res.shift && p.a.res.shift == left - side, std::string("placement: the picture sits at left_px: ") + what);
        if (original) check(p.a.res.shift == 0 && p.b.res.shift == 0, std::string("Original / 4:3 requests no placement: ") + what);
        const uint64_t d = presentation_digest(p.a);
        combined = fnv1a64(reinterpret_cast<const uint8_t *>(&d), sizeof d, combined);
        std::printf("PARITY gate %s: %s accepted %u reason %u digest %016" PRIx64 "\n", what, p.mismatches.empty() ? "PASS" : "FAIL", p.a.res.compat_accepted, p.a.res.compat_reason, d);
      }
      check_golden(g.spec.id + ":" + kAspectName[ai], combined);
      gate_ran.insert("presentation:" + g.spec.id + ":" + kAspectName[ai]);
    }
  std::printf("PARITY gate presentation totals: %u cases (%u served, %u refused)\n", cases, served, refused);
}

// ---- falsification: each mutant of the ABI 2 twin must fail a NAMED assertion; its control (the unmutated twin on the same case) must pass ------------------------------------------------------------------------
bool has(const std::vector<std::string> &m, const char *name) { return std::find(m.begin(), m.end(), name) != m.end(); }
void falsification() {
  const std::vector<GateScene> scenes = gate_scenes();
  const auto scene = [&](const char *id) -> const Spec & { for (const GateScene &g : scenes) if (g.spec.id == id) return g.spec; std::abort(); };
  struct Case { Mutant m; const char *scene; int side, left, stale_left; const char *expect; const char *expect2; };
  const Case cases[] = {
      {Mutant::StaleScroll, "parallax-hud", 238, 238, -1, "surface", "coverage"},
      {Mutant::StaleCacheKey, "split-scroll-hud", 101, 101, -1, "surface", "coverage"},
      {Mutant::StalePlacement, "parallax-hud", 238, 238, 119, "surface", "coverage"},
      {Mutant::SkippedCallback, "parallax-hud", 238, 238, -1, "accepted-components", "overlay"},
      {Mutant::MissingRefusal, "unsupported-scene", 101, 101, -1, "accepted-components", "overlay"},
      {Mutant::MissingRefusal, "fade-decline", 238, 119, -1, "accepted-components", "layers"},
      {Mutant::WrongReason, "fade-decline", 238, 238, -1, "reason", "reason"},
      {Mutant::OversizedBitmap, "parallax-hud", 238, 238, -1, "accepted-components", "reason"},
  };
  for (const Case &c : cases) {
    const Spec &s = scene(c.scene);
    BuilderCache warm;
    if (c.m == Mutant::StaleCacheKey) { Spec previous = s; for (unsigned p = 0; p < 2; ++p) for (int r = 0; r < H; ++r) previous.hs[p][r] = wrap10(previous.hs[p][r] - 40); (void)run_pair(previous, c.side, c.left, c.m, &warm); }   // the previous frame's windows stay under the same (incomplete) key
    const Pair control = run_pair(s, c.side, c.left);
    const Pair bad = run_pair(s, c.side, c.left, c.m, c.m == Mutant::StaleCacheKey ? &warm : nullptr, 0, c.stale_left);
    char what[160];
    std::snprintf(what, sizeof what, "%s on %s at %s", mutant_name(c.m), c.scene, c.side == 238 ? "32:9" : "21:9");
    check(control.mismatches.empty(), std::string("the control (the unmutated twin) passes: ") + what);
    check(has(bad.mismatches, c.expect) || has(bad.mismatches, c.expect2), std::string("the gate fails the mutant with the named assertion '") + c.expect + "': " + what);
    check(presentation_digest(control.b) != presentation_digest(bad.b) || c.m == Mutant::WrongReason, std::string("the mutant's presentation digest differs: ") + what);
    std::printf("PARITY falsification %s: failing assertions [%s]\n", what, [&] { static std::string joined; joined.clear(); for (const std::string &m : bad.mismatches) joined += (joined.empty() ? "" : ",") + m; return joined.c_str(); }());
    gate_ran.insert(std::string("falsify:") + mutant_name(c.m));
  }
  // the oversized bitmap is rejected by the production range rule (a named call status), not silently repaired
  const Pair oversized = run_pair(scene("parallax-hud"), 238, 238, Mutant::OversizedBitmap);
  check(oversized.rejected > 0 && oversized.b.res.compat_accepted == 0U && oversized.b.res.compat_reason == GENESIS_COMPAT_FAULT && oversized.b.res.layer_count == 0U && oversized.b.res.overlay_w == 0U && oversized.b.res.shift == 0 && oversized.b.res.authored == 0U &&
        oversized.b.res.scene[0] == '\0' && oversized.a.res.layer_count == 3U,
        "an oversized bitmap layer is refused by the production overlay rules and FAULTS the callback: no HUD, no layer, no scenery credit, no placement, no scene token survives (accepted 0, reason FAULT), while the ABI 1 reference presents its three layers");
}

// ---- scenario matrix: sequences (fade, aspect transitions, replay, warm/cold provider state, save/load, rewind), the genuine fault and the stale-placement rule -------------------------------------------------------------
struct Session { BuilderCache cache; unsigned frame = 0; };
std::vector<uint64_t> run_sequence(Session &ses, unsigned from, unsigned count, bool use_cache) {
  std::vector<uint64_t> out;
  const std::vector<GateScene> scenes = gate_scenes();
  for (unsigned i = from; i < from + count; ++i) {
    Spec s = scenes[0].spec;   // parallax-hud as the playfield; the fade counter and the camera advance with the frame number
    s.fade = static_cast<uint8_t>(i % 8U >= 2U && i % 8U <= 4U ? i % 8U - 1U : 0U);
    set_plane(s, 1, [&](int r) { return -(100 + static_cast<int>(i) * 3 + (r / 38) * 40); });
    const int side = kSide[1 + i % 3U], left = side / 2 + static_cast<int>(i % 3U) * (side / 4);   // the aspect changes every frame (16:9, 21:9, 32:9) and the placement with it
    const Pair p = run_pair(s, side, left, Mutant::None, use_cache ? &ses.cache : nullptr);
    check(p.mismatches.empty(), "sequence frame " + std::to_string(i) + " equals ABI 1");
    out.push_back(presentation_digest(p.b));
  }
  ses.frame = from + count;
  return out;
}
void sequences() {
  Session a, b, c, d;
  const std::vector<uint64_t> uninterrupted = run_sequence(a, 0, 24, true);
  check(uninterrupted == run_sequence(b, 0, 24, true), "deterministic replay twice: identical digests");
  check(uninterrupted == run_sequence(c, 0, 24, false), "cache cleared (no provider-side reuse) equals the warm provider state");
  bool fade_seen = false, served_seen = false;
  { const std::vector<GateScene> scenes = gate_scenes(); for (unsigned i = 0; i < 8U; ++i) { Spec s = scenes[0].spec; s.fade = static_cast<uint8_t>(i % 8U >= 2U && i % 8U <= 4U ? i % 8U - 1U : 0U); (s.fade != 0U ? fade_seen : served_seen) = true; } }
  check(fade_seen && served_seen, "the sequence contains both fade (refused) and served frames");
  // save/load and rewind: a snapshot of the provider state at frame 10, restored after the run continued, reproduces the uninterrupted tail
  Session live;
  const std::vector<uint64_t> head = run_sequence(live, 0, 10, true);
  const Session snapshot = live;
  const std::vector<uint64_t> tail = run_sequence(live, 10, 14, true);
  Session restored = snapshot;
  const std::vector<uint64_t> after_load = run_sequence(restored, 10, 14, true);
  check(after_load == tail && head.size() + tail.size() == uninterrupted.size() && std::equal(head.begin(), head.end(), uninterrupted.begin()) && std::equal(tail.begin(), tail.end(), uninterrupted.begin() + 10), "save/load: the restored run reproduces the uninterrupted one");
  Session rewound = snapshot;
  rewound.cache.clear();
  check(run_sequence(rewound, 10, 14, true) == tail, "rewind to frame 10 with a cleared cache reproduces the same frames");
  (void)d;
  gate_ran.insert("sequence:fade"); gate_ran.insert("sequence:aspect-transition"); gate_ran.insert("sequence:replay"); gate_ran.insert("sequence:cache-cleared-vs-warm"); gate_ran.insert("sequence:save-load"); gate_ran.insert("sequence:rewind");
  std::printf("PARITY gate sequence: 24 frames, aspect changes every frame, digest chain %016" PRIx64 "\n", [&] { uint64_t h = 14695981039346656037ULL; for (const uint64_t x : uninterrupted) h = fnv1a64(reinterpret_cast<const uint8_t *>(&x), sizeof x, h); return h; }());
  check_golden("sequence:24-frame-chain", [&] { uint64_t h = 14695981039346656037ULL; for (const uint64_t x : uninterrupted) h = fnv1a64(reinterpret_cast<const uint8_t *>(&x), sizeof x, h); return h; }());
}

void fault_and_placement_rules() {
  const std::vector<GateScene> scenes = gate_scenes();
  const Spec &s = scenes[0].spec;
  const Pair clean = run_pair(s, 238, 238);
  const Pair faulty = run_pair(s, 238, 238, Mutant::None, nullptr, 7);   // the 7th scenery_word call of either twin is unanswered
  check(faulty.mismatches.empty() && faulty.a.res.compat_reason == GENESIS_COMPAT_FAULT && faulty.b.res.compat_reason == GENESIS_COMPAT_FAULT && faulty.a.res.authored == 0 && faulty.b.res.authored == 0, "a genuine provider failure (an unanswered cell) is the same FAULT verdict, authored 0, in both twins");
  check(clean.a.res.authored == clean.b.res.authored && clean.a.res.compat_reason == GENESIS_COMPAT_OK && presentation_digest(clean.b) != presentation_digest(faulty.b), "the fault leaves no residue: the next frame is served again");
  const Pair after = run_pair(s, 238, 238);
  check(presentation_digest(after.b) == presentation_digest(clean.b), "the frame after a fault equals the frame before it");
  gate_ran.insert("fault-injection");
  // ADR 0315: a stale or out-of-range placement is never reinterpreted: both twins get the centred layout
  for (int ai = 1; ai < 4; ++ai) {
    const int side = kSide[ai];
    const Pair centred = run_pair(s, side, side);
    const Pair stale = run_pair(s, side, 2 * side + 40, Mutant::None, nullptr, 0, side);   // beyond the planned total: the producer ignores the request
    check(stale.mismatches.empty() && stale.a.res.shift == 0 && stale.b.res.shift == 0 && presentation_digest(stale.a) == presentation_digest(centred.a) && presentation_digest(stale.b) == presentation_digest(centred.b), std::string("an out-of-range placement is the centred layout in both twins: ") + kAspectName[ai]);
    const Pair negative = run_pair(s, side, -5, Mutant::None, nullptr, 0, side);
    check(negative.mismatches.empty() && negative.a.res.shift == 0, std::string("a negative placement is the centred layout in both twins: ") + kAspectName[ai]);
  }
  gate_ran.insert("stale-placement-rejection");
}

// The cost and memory of the exact-frame view and of the bitmap transport, per produced frame, on the fixture (deterministic counts; the wall-clock numbers are informational and never asserted).
void cost_and_memory_report() {
  const std::vector<GateScene> scenes = gate_scenes();
  const size_t view_bytes = GENESIS_COMPAT_RAM_BYTES + GENESIS_COMPAT_VRAM_BYTES + sizeof(GenesisCompatLine) * GENESIS_COMPAT_MAX_LINES;
  std::printf("PARITY cost exact-frame view per produced frame: work RAM %u + VRAM %u + %u line records x %zu = %zu bytes copied by the host (immutable, host-owned)\n", GENESIS_COMPAT_RAM_BYTES, GENESIS_COMPAT_VRAM_BYTES,
              GENESIS_COMPAT_MAX_LINES, sizeof(GenesisCompatLine), view_bytes);
  std::printf("PARITY memory host buffers (fixed, shared by ABI 1 and ABI 2): overlay %u bytes, layers %u bytes; staged ABI 2 bitmap budget per callback 393216 bytes\n", GENESIS_COMPAT_OVERLAY_BYTES, GENESIS_COMPAT_LAYER_BYTES);
  for (int ai = 1; ai < 4; ++ai)
    for (const GateScene &g : scenes) {
      if (g.spec.id != "parallax-hud" && g.spec.id != "split-scroll-hud") continue;
      const Pair p = run_pair(g.spec, kSide[ai], kSide[ai]);
      std::printf("PARITY cost %s %s centred: abi1 scenery calls %u, abi2 host answers %u from %u window words (%u bytes) + 2 x %u mask bytes, staged bitmap bytes %u, produce ms abi1 %.2f abi2 %.2f (informational)\n", g.spec.id.c_str(), kAspectName[ai],
                  p.abi1_calls, p.abi2_calls, p.words, p.words * 2U, (H + 7) / 8, p.bitmap_bytes, p.ms1, p.ms2);
      check(p.words <= 4U * segarecomp::scenery::kWordsMax && p.bitmap_bytes <= 393216U && p.abi1_calls == p.abi2_calls, "cost is bounded: windows <= 4 x 4096 words, staged bitmaps within the per-callback budget, no extra host queries");
    }
}

// ---- completeness: the gate fails (it never skips) when a mandatory case did not run ----------------------------------------------------------------------------------------------------------------------------
void completeness() {
  const char *const scenery[] = {"constant", "parallax-few", "parallax-many", "parallax-interleaved", "split-scroll", "fallback-cell-rows", "fallback-mid-cell", "origin-multiple", "origin-multiple-mid-cell", "raster-condition",
                                 "raster-condition-cell-row", "level-start", "level-end", "wrap-crossing", "fast-camera"};
  const char *const masks[] = {"fallback-mid-cell", "plane-a-only", "plane-b-only", "different-masks", "shared-cell", "alternating", "boundary-in-cell", "both-masked", "all-rows-masked", "mask-only", "mask-one-plane-windows-other",
                               "window-band", "journal-write", "no-journal", "parallax-mask"};
  const char *const scenes[] = {"parallax-hud", "split-scroll-hud", "line-palette", "fade-decline", "unsupported-scene", "pause-hud-only", "bad-hud", "generic-plane", "window-band-hud"};
  unsigned required = 0;
  const auto need = [&](const std::string &id, bool in_scenery) { ++required; check((in_scenery ? ran.count(id) : gate_ran.count(id)) != 0U, "MANDATORY case ran: " + id); };
  for (const char *id : scenery) for (const char *asp : {"16:9", "21:9", "32:9"}) { need(std::string("scenario-exact:") + id + ":" + asp, true); need(std::string("scenario-predicted:") + id + ":" + asp, true); }
  for (const char *id : masks) for (const char *asp : {"16:9", "21:9", "32:9"}) need(std::string("mask:") + id + ":" + asp, true);
  need("mask-448-producer", true); need("code3-gate", true);
  for (const char *id : scenes) for (const char *asp : kAspectName) need(std::string("presentation:") + id + ":" + asp, false);
  for (const char *id : {"stale-scroll", "stale-cache-key", "stale-placement", "skipped-callback", "missing-refusal", "wrong-reason", "oversized-bitmap"}) need(std::string("falsify:") + id, false);
  for (const char *id : {"sequence:fade", "sequence:aspect-transition", "sequence:replay", "sequence:cache-cleared-vs-warm", "sequence:save-load", "sequence:rewind", "fault-injection", "stale-placement-rejection"}) need(id, false);
  check(required == 15U * 6U + 15U * 3U + 2U + 9U * 4U + 7U + 8U, "the mandatory list has the expected size (" + std::to_string(required) + ")");
  std::printf("PARITY gate completeness: %u mandatory cases, none missing=%s\n", required, failures == 0 ? "yes" : "see FAIL lines");
  // T001's recorded WATCHED classes: documented synthetic limits with no real-provider failure (T001 decision, stays unimplemented); they must keep failing deterministically, never be silently passed or excused
  const Scenario *watched[2] = {nullptr, nullptr};
  static const std::vector<Scenario> all = scenarios();
  for (const Scenario &sc : all) { if (sc.spec.id == "origin-multiple-mid-cell") watched[0] = &sc; if (sc.spec.id == "raster-condition") watched[1] = &sc; }
  for (const Scenario *w : watched) {
    check(w != nullptr, "the watched T001 class is present");
    if (w == nullptr) continue;
    const Verdict v = run_case(w->spec, 238, 238, Timing::Exact);
    check(v.scenario == "LIMIT:condition-dependent" && v.rows_differ > 0, "WATCHED (T001: synthetic only, not implemented, not parity): " + w->spec.id + " still fails as LIMIT:condition-dependent");
    std::printf("PARITY gate WATCHED %s: %s (%u rows differ; no real-provider scene exhibits it)\n", w->spec.id.c_str(), v.scenario.c_str(), v.rows_differ);
  }
}
}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--print-goldens") == 0) print_goldens = true;
  if (run_scenery_suite() != 0) std::printf("abi_parity_gate_test: the T001/T007 scenery section failed\n");
  presentation_matrix();
  falsification();
  sequences();
  fault_and_placement_rules();
  cost_and_memory_report();
  if (!print_goldens) completeness();
  if (failures != 0) { std::printf("abi_parity_gate_test: %d failure(s)\n", failures); return 1; }
  std::printf("abi_parity_gate_test: ok\n");
  return 0;
}
