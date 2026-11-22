// SEG-152-T001: THE ONE ABI 1 / ABI 2 parity comparison harness (SEG-152-T008 extends it into the complete parity gate; it must not create a second harness, digest or fixture format).
//
// What it is. A deterministic, redistributable (project-authored, no ROM, no clock) twin-provider runner built on the REAL extension producer (`genesis_vdp_produce_extension`, the very code the
// viewer runs) and on the PRODUCTION ABI 2 window contract (`platforms/genesis/viewer/mod_scenery_window.hpp`, the limits and the lookup `mod_capture.cpp` itself uses). The same VDP scene is produced
// twice: once through an ABI 1 reference provider (`GenesisNativeCompat::scenery_word` answering from a world model, exactly the shape of the Sonic providers) and once through an ABI 2 window provider
// (a builder that submits at most two bounded name-table windows per plane for the placement it declares, answered by the host's own lookup). The two extension surfaces are then compared.
//
//   fixture format  `Spec`: per-plane per-line hscroll (as the VDP table holds it, 10-bit signed), per-line registered origin multiple `k` (world x = x - hscroll + 512 k, the S3K `bg_register` shape),
//                   per-line ABI 1 fallback flag (the VDP's own wrapped word: the S3K `own_plane` shape), per-line answer region (a same plane coordinate answered differently under another raster
//                   condition), per-plane vscroll, per-line speed (guest pixels per frame, for the render(N) timing) and an optional world clamp (level edges).
//   digest          `row_digest` / `surface_digest`: FNV-1a 64 over the produced RGB rows (one implementation; frame, surface and overlay digests of T008 are added here, not beside it).
//   runner          `run_case`: scene x aspect side x placement `left_px` x timing -> both surfaces, the per-row verdicts and the aggregate.
//   classifier      `classify_rows`: per row either IDENTICAL pixels or the exact class of the difference: LIMIT:window (a requested cell is outside every window), LIMIT:per-line-fallback (the same plane
//                   coordinate is answered by the VDP's own word on one line and by authored data on another), LIMIT:condition-dependent (the same plane coordinate has two authored answers under different
//                   raster conditions), or UNEXPLAINED (a harness failure: no cause found). PROVIDER_ERROR:placement marks a builder that used another placement than the one it declared; it is never a limit.
//
// Timings. `Exact` builds the windows from the scroll the host applies to the produced frame (the exact-frame view). `Predicted` builds them from the scroll of two frames earlier (the render(N)
// callback feeds the frame produced when the runtime counts N + 2, ADR 0309) and pads every span by the largest per-line displacement, without extrapolation (what a provider that cannot see the produced
// frame can do). Everything the model does not do (guest execution, snapshot ownership, save/load) belongs to the viewer-level tests; this harness answers one question deterministically: can bounded
// windows reproduce what the ABI 1 provider answers, pixel for pixel.
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "mod_scenery_window.hpp"
#include "runtime.h"
#include "vdp_render.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace parity {

constexpr int W = 320, H = 224;
constexpr unsigned kPlaneCols = 64, kPlaneRows = 32;   // the fixture VDP plane size (reg 16 = 1): 512 x 256 pixels

// ---- fixture format ---------------------------------------------------------------------------------------------------------------
struct Spec {
  std::string id;
  int hs[2][H] = {};          // the VDP-applied horizontal scroll per plane and line, in [-512, 511]
  int k[2][H] = {};           // registered origin multiple (512 px each): world x = x - hscroll + 512 k
  int vs[2] = {0, 0};         // vertical scroll (full screen)
  int speed[2][H] = {};       // pixels per guest frame (the Predicted timing)
  uint8_t fallback[2][H] = {};   // 1: the ABI 1 provider answers this line with the VDP's own wrapped word
  uint8_t region[2][H] = {};     // the answer class of the world model on this line
  int clamp_w = 0;            // > 0: world columns clamp to [0, clamp_w) cells (level edges: the edge column repeats outward)
};

inline int wrap10(long v) { return static_cast<int>(((v + 512) & 1023) - 512); }
inline long floor8(long v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); }

inline uint32_t mix(uint32_t a) {
  a ^= a >> 16; a *= 0x7FEB352DU; a ^= a >> 15; a *= 0x846CA68BU; a ^= a >> 16;
  return a;
}
// The authored world: a pure function of the world cell and the line's answer region (never of the camera).
inline uint16_t world_word(const Spec &s, unsigned plane, long wx, long wy, unsigned region) {
  if (s.clamp_w > 0) wx = std::max(0L, std::min(wx, static_cast<long>(s.clamp_w) - 1));
  const uint32_t h = mix(static_cast<uint32_t>(wx * 73856093L) ^ static_cast<uint32_t>(wy * 19349663L) ^ (plane * 83492791U) ^ (region * 2654435761U));
  const unsigned tile = (plane == 0U && ((h >> 20) & 1U) != 0U) ? 0U : 1U + h % 15U;   // half of plane A is transparent, so plane B is observable through it
  return static_cast<uint16_t>((((h >> 8) & 3U) << 13) | tile);
}

// ---- the VDP scene ----------------------------------------------------------------------------------------------------------------
struct Scene {
  std::vector<uint8_t> vram = std::vector<uint8_t>(GENESIS_VDP_VRAM_BYTES, 0);
  std::unique_ptr<GenesisRasterFrameState> raster = std::make_unique<GenesisRasterFrameState>();
  uint16_t vram_word(unsigned plane, unsigned cx, unsigned cy) const {   // the VDP's own (wrapped) name-table word
    const unsigned a = (plane == 0U ? 0xC000U : 0xE000U) + ((cy % kPlaneRows) * kPlaneCols + (cx % kPlaneCols)) * 2U;
    return static_cast<uint16_t>((vram[a] << 8) | vram[a + 1U]);
  }
};

inline void build_scene(Scene &sc, const Spec &spec) {
  std::memset(sc.raster.get(), 0, sizeof(*sc.raster));
  for (unsigned t = 1; t < 16; ++t)
    for (unsigned i = 0; i < 32; ++i) sc.vram[t * 32 + i] = static_cast<uint8_t>(t * 0x11);
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned cy = 0; cy < kPlaneRows; ++cy)
      for (unsigned cx = 0; cx < kPlaneCols; ++cx) {   // random VDP content (a different mix than the world), so authored and generic cells are distinguishable
        const uint32_t h = mix(0xA5A5A5A5U ^ (plane << 20) ^ (cy << 8) ^ cx);
        const unsigned w = (((h >> 9) & 3U) << 13) | ((plane == 0U && ((h >> 21) & 1U) != 0U) ? 0U : 1U + h % 15U);
        const uint32_t a = (plane == 0U ? 0xC000U : 0xE000U) + (cy * kPlaneCols + cx) * 2U;
        sc.vram[a] = static_cast<uint8_t>(w >> 8);
        sc.vram[a + 1U] = static_cast<uint8_t>(w);
      }
  for (unsigned l = 0; l < 256U; ++l) {   // per-line scroll table at 0xFC00 (256 entries of 4 bytes end the 64 KiB VRAM)
    const unsigned row = l < static_cast<unsigned>(H) ? l : static_cast<unsigned>(H) - 1U;
    const int a = spec.hs[0][row], b = spec.hs[1][row];
    sc.vram[0xFC00 + l * 4] = static_cast<uint8_t>((a & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 1] = static_cast<uint8_t>(a);
    sc.vram[0xFC00 + l * 4 + 2] = static_cast<uint8_t>((b & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 3] = static_cast<uint8_t>(b);
  }
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  for (unsigned i = 0; i < 64; ++i) { const unsigned w = ((i * 5 + 3) % 8) << 1 | ((i * 3 + 1) % 8) << 5 | ((i * 7 + 2) % 8) << 9; cram[i * 2] = static_cast<uint8_t>(w >> 8); cram[i * 2 + 1] = static_cast<uint8_t>(w); }
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES] = {};
  vsram[0] = static_cast<uint8_t>(spec.vs[0] >> 8); vsram[1] = static_cast<uint8_t>(spec.vs[0]);
  vsram[2] = static_cast<uint8_t>(spec.vs[1] >> 8); vsram[3] = static_cast<uint8_t>(spec.vs[1]);
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT] = {};
  regs[1] = 0x04; regs[2] = 0x30; regs[3] = 0x34; regs[4] = 0x07; regs[5] = 0x7C; regs[7] = 2; regs[11] = 3; regs[12] = 0x81; regs[13] = 0x3F; regs[16] = 1;   // per-line hscroll, full-screen vscroll, 64 x 32
  for (auto &line : sc.raster->lines) {
    std::memcpy(line.registers, regs, sizeof(line.registers));
    std::memcpy(line.cram, cram, sizeof(line.cram));
    std::memcpy(line.vsram, vsram, sizeof(line.vsram));
  }
  sc.raster->has_vram_journal = 1;
  std::memcpy(sc.raster->vram_start, sc.vram.data(), GENESIS_VDP_VRAM_BYTES);
}

// ---- what a line asks: plane cell keys ------------------------------------------------------------------------------------------------
// The host asks the provider for every off-picture pixel of both planes and answers a cell by cell(x - hscroll), cell(row + vscroll): the plane coordinate. These are the unwrapped coordinates.
struct Span { long c0, c1; };   // inclusive plane-cell columns

inline Span left_span(int hs, int left_px) { return {floor8(-left_px - hs), floor8(-1 - hs)}; }
inline Span right_span(int hs, int right_px) { return {floor8(W - hs), floor8(W + right_px - 1 - hs)}; }

// The ABI 1 answer for plane cell (pc, pcy) on `row` (the Sonic providers' shape: a world function of the registered offset, or the VDP's own word).
inline uint16_t abi1_answer(const Spec &s, const Scene &sc, unsigned plane, int row, long pc, long pcy) {
  if (s.fallback[plane][row] != 0U) return sc.vram_word(plane, static_cast<unsigned>(((pc % 64) + 64) % 64), static_cast<unsigned>(((pcy % 32) + 32) % 32));
  return world_word(s, plane, pc + 64L * s.k[plane][row], pcy, s.region[plane][row]);
}

// ---- providers --------------------------------------------------------------------------------------------------------------------
struct Provider {
  enum Kind { Abi1, Abi2 } kind = Abi1;
  const Spec *spec = nullptr;
  const Scene *scene = nullptr;
  int declared_left = 0;
  segarecomp::scenery::Window windows[2][2];
  unsigned cells = 0;
};
inline void provider_prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Provider *p = static_cast<Provider *>(state);
  out->accepted = GENESIS_COMPAT_SCENERY & f->requested;
  out->reason = GENESIS_COMPAT_OK;
  out->left_px = p->declared_left;
  out->has_left_px = 1;
}
inline int provider_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  Provider *p = static_cast<Provider *>(state);
  ++p->cells;
  if (plane > 1U || row >= static_cast<unsigned>(H)) return 0;
  if (p->kind == Provider::Abi1) {   // the reference: the registered world, never constrained by a window
    const long pc = floor8(static_cast<long>(x) - hscroll), pcy = floor8(static_cast<long>(row) + vscroll);
    *word = abi1_answer(*p->spec, *p->scene, plane, static_cast<int>(row), pc, pcy);
    return 1;
  }
  if (!p->windows[plane][0].present && !p->windows[plane][1].present) return GENESIS_COMPAT_WORD_GENERIC;   // ADR 0314
  return segarecomp::scenery::lookup(p->windows[plane], static_cast<int>(row), x, hscroll, vscroll, word);
}
inline int provider_hud(void *, GenesisCompatOverlay *) { return 0; }
inline GenesisNativeCompat g_table = {GENESIS_NATIVE_COMPAT_ABI, "parity.twin", 1, GENESIS_COMPAT_SCENERY, provider_prepare, provider_word, provider_hud};

// ---- the ABI 2 window builder (the provider model) -------------------------------------------------------------------------------------
enum class Timing { Exact, Predicted };
struct Built {
  segarecomp::scenery::Window w[2][2];
  int needed[2] = {0, 0};        // windows the plane needs under the production limits (greedy, left and right separately)
  bool feasible[2] = {true, true};
  unsigned max_words[2] = {0, 0};
  unsigned total_words = 0;
};

struct CellSpan { long cy, c0, c1; bool left; };

inline bool fits(long cols, long rows) {
  return cols >= 1 && rows >= 1 && cols <= static_cast<long>(segarecomp::scenery::kColsMax) && rows <= static_cast<long>(segarecomp::scenery::kRowsMax) &&
         cols * rows <= static_cast<long>(segarecomp::scenery::kWordsMax);
}
struct Rect { long cy0, cy1, c0, c1; bool any = false; };
inline Rect hull(const std::vector<CellSpan> &spans, size_t from, size_t to, int side_filter /* -1 any, 0 left, 1 right */) {
  Rect r{};
  for (size_t i = from; i < to; ++i) {
    const CellSpan &s = spans[i];
    if (side_filter >= 0 && (s.left ? 0 : 1) != side_filter) continue;
    if (!r.any) { r = Rect{s.cy, s.cy, s.c0, s.c1, true}; continue; }
    r.cy0 = std::min(r.cy0, s.cy); r.cy1 = std::max(r.cy1, s.cy); r.c0 = std::min(r.c0, s.c0); r.c1 = std::max(r.c1, s.c1);
  }
  return r;
}
inline bool rect_fits(const Rect &r) { return !r.any || fits(r.c1 - r.c0 + 1, r.cy1 - r.cy0 + 1); }

// Builds the windows of one plane for a declared placement. `built_left` is the placement the builder computes for (a stale or wrong margin is a PROVIDER_ERROR of the test, never a limit).
inline void build_plane(const Spec &s, const Scene &sc, unsigned plane, int side, int built_left, Timing timing, Built &out) {
  const int left_px = built_left, right_px = 2 * side - built_left;
  long pad = 0;
  if (timing == Timing::Predicted) {
    long maxd = 0;
    for (int r = 0; r < H; ++r) maxd = std::max(maxd, static_cast<long>(std::labs(s.speed[plane][r])));
    pad = (2 * maxd + 7) / 8 + 1;
  }
  std::vector<CellSpan> spans;   // per plane-cell row: the merged column spans of the left and the right extension
  std::map<std::tuple<long, long>, uint16_t> words;   // first asking line wins: a plane cell has ONE word in a window
  std::map<long, std::pair<Span, Span>> by_cy;
  std::map<long, int> first_row;   // the first line of each plane cell row: the provider's answer for a padded cell no line asked yet (a pure function of the world, so the padding is exact)
  for (int r = 0; r < H; ++r) {
    first_row.emplace(floor8(static_cast<long>(r) + s.vs[plane]), r);
    const int hs = timing == Timing::Predicted ? wrap10(s.hs[plane][r] - 2L * s.speed[plane][r]) : s.hs[plane][r];
    const long cy = floor8(static_cast<long>(r) + s.vs[plane]);
    const Span l = left_span(hs, left_px), rr = right_span(hs, right_px);
    auto it = by_cy.find(cy);
    if (it == by_cy.end()) by_cy[cy] = {l, rr};
    else { it->second.first = {std::min(it->second.first.c0, l.c0), std::max(it->second.first.c1, l.c1)}; it->second.second = {std::min(it->second.second.c0, rr.c0), std::max(it->second.second.c1, rr.c1)}; }
    if (left_px > 0) for (long c = l.c0; c <= l.c1; ++c) words.emplace(std::make_tuple(c, cy), abi1_answer(s, sc, plane, r, c, cy));
    if (right_px > 0) for (long c = rr.c0; c <= rr.c1; ++c) words.emplace(std::make_tuple(c, cy), abi1_answer(s, sc, plane, r, c, cy));
  }
  for (const auto &kv : by_cy) {
    if (left_px > 0) spans.push_back({kv.first, kv.second.first.c0 - pad, kv.second.first.c1 + pad, true});
    if (right_px > 0) spans.push_back({kv.first, kv.second.second.c0 - pad, kv.second.second.c1 + pad, false});
  }
  // Candidate partitions into at most two rectangles: everything; left | right; a split at each plane-cell row. The first that fits the production limits is submitted.
  std::vector<Rect> chosen;
  {
    const Rect all = hull(spans, 0, spans.size(), -1);
    if (!all.any) chosen = {};
    else if (rect_fits(all)) chosen = {all};
    else {
      const Rect l = hull(spans, 0, spans.size(), 0), rr = hull(spans, 0, spans.size(), 1);
      if (rect_fits(l) && rect_fits(rr)) { if (l.any) chosen.push_back(l); if (rr.any) chosen.push_back(rr); }
      else {
        std::vector<long> cys;
        for (const auto &kv : by_cy) cys.push_back(kv.first);
        for (size_t split = 1; split < cys.size() && chosen.empty(); ++split) {
          std::vector<CellSpan> top, bottom;
          for (const CellSpan &sp : spans) (sp.cy < cys[split] ? top : bottom).push_back(sp);
          const Rect a = hull(top, 0, top.size(), -1), b = hull(bottom, 0, bottom.size(), -1);
          if (rect_fits(a) && rect_fits(b)) chosen = {a, b};
        }
      }
    }
  }
  // Column slicing: full-height windows of at most min(128, 4096 / rows) columns covering the union of every span. The greedy cover from the left is optimal for intervals, so `needed` is the minimum
  // number of such windows; two of them are a feasible partition (the host answers a cell from the first window that contains it, so a span may straddle two windows).
  {
    long ymin = 0, ymax = -1, cap = static_cast<long>(segarecomp::scenery::kColsMax);
    for (const CellSpan &sp : spans) { if (ymax < ymin) { ymin = ymax = sp.cy; } ymin = std::min(ymin, sp.cy); ymax = std::max(ymax, sp.cy); }
    if (ymax >= ymin) cap = std::min(cap, static_cast<long>(segarecomp::scenery::kWordsMax) / (ymax - ymin + 1));
    std::vector<std::pair<long, long>> iv;
    for (const CellSpan &sp : spans) iv.push_back({sp.c0, sp.c1});
    std::sort(iv.begin(), iv.end());
    std::vector<Rect> slices;
    long covered_to = 0; bool have = false;
    for (const auto &p : iv) {
      if (have && p.second <= covered_to) continue;
      const long start = (have && p.first <= covered_to) ? covered_to + 1 : p.first;
      Rect r{ymin, ymax, start, start + cap - 1, true};
      slices.push_back(r);
      covered_to = r.c1; have = true;
    }
    out.needed[plane] = static_cast<int>(slices.size());
    if (!chosen.empty()) out.needed[plane] = std::min(out.needed[plane], static_cast<int>(chosen.size()));
    if (chosen.empty() && slices.size() <= 2 && !spans.empty()) chosen = slices;
    else if (chosen.empty() && !spans.empty()) {   // more than two windows needed: the best the provider can do is the first two slices (the rest is unanswered)
      out.feasible[plane] = false;
      chosen = {slices[0], slices[1]};
    }
  }
  for (size_t n = 0; n < chosen.size() && n < 2; ++n) {
    segarecomp::scenery::Window &w = out.w[plane][n];
    w.present = true; w.x0 = static_cast<int32_t>(chosen[n].c0); w.y0 = static_cast<int32_t>(chosen[n].cy0);
    w.cols = static_cast<uint32_t>(chosen[n].c1 - chosen[n].c0 + 1); w.rows = static_cast<uint32_t>(chosen[n].cy1 - chosen[n].cy0 + 1);
    static const uint16_t kNonNull = 0U;   // the production range check (limits, origin bound) is the adapter's own; the words are filled below
    if (!segarecomp::scenery::submission_in_range(plane, w.x0, w.y0, w.cols, w.rows, &kNonNull, false)) { w = segarecomp::scenery::Window{}; continue; }
    w.words.assign(static_cast<size_t>(w.cols) * w.rows, 0U);
    for (uint32_t cy = 0; cy < w.rows; ++cy)
      for (uint32_t c = 0; c < w.cols; ++c) {
        const auto it = words.find(std::make_tuple(w.x0 + static_cast<long>(c), w.y0 + static_cast<long>(cy)));
        const long wy = w.y0 + static_cast<long>(cy), wx = w.x0 + static_cast<long>(c);
        const auto fr = first_row.find(wy);
        if (it != words.end()) w.words[static_cast<size_t>(cy) * w.cols + c] = it->second;
        else if (fr != first_row.end()) w.words[static_cast<size_t>(cy) * w.cols + c] = abi1_answer(s, sc, plane, fr->second, wx, wy);
      }
    out.max_words[plane] = std::max(out.max_words[plane], static_cast<unsigned>(w.words.size()));
    out.total_words += static_cast<unsigned>(w.words.size());
  }
}

// ---- production, digest ------------------------------------------------------------------------------------------------------------
struct PlanCtx { unsigned side; };
inline int plan_fn(void *ctx, unsigned, unsigned, unsigned *side_out) { *side_out = static_cast<PlanCtx *>(ctx)->side; return 0; }

inline uint64_t fnv1a64(const uint8_t *d, size_t n, uint64_t h = 14695981039346656037ULL) {
  for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 1099511628211ULL; }
  return h;
}
struct Produced { int rc = -1; GenesisExtensionResult res{}; std::vector<uint8_t> surface; size_t pitch = 0; };
inline uint64_t row_digest(const Produced &p, int row) { return fnv1a64(&p.surface[static_cast<size_t>(row) * p.pitch], p.pitch); }
inline uint64_t surface_digest(const Produced &p) { uint64_t h = 14695981039346656037ULL; for (int r = 0; r < H; ++r) h = fnv1a64(&p.surface[static_cast<size_t>(r) * p.pitch], p.pitch, h); return h; }

inline Produced produce(const Scene &sc, Provider &prov, unsigned side) {
  static std::vector<uint8_t> surface(GENESIS_EXT_SURFACE_BYTES), overlay(GENESIS_COMPAT_OVERLAY_BYTES), ram(GENESIS_COMPAT_RAM_BYTES);
  PlanCtx pc{side};
  auto native = std::make_unique<GenesisFrameArtifact>();
  Produced o;
  if (genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) != 0) return o;
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.producer = genesis_vdp_produce_extension; ext.pixels = surface.data(); ext.capacity = surface.size(); ext.request = 2;
  ext.compat = &g_table; ext.compat_state = &prov; ext.compat_components = GENESIS_COMPAT_SCENERY; ext.compat_caps = GENESIS_CAP_SCENERY_WORDS | GENESIS_CAP_FRAME_VDP | GENESIS_CAP_FRAME_WORK_RAM;
  ext.compat_ram = ram.data(); ext.compat_ram_valid = 1; ext.overlay_rgba = overlay.data(); ext.layer_rgba = nullptr;
  std::fill(surface.begin(), surface.end(), static_cast<uint8_t>(0xAA));
  o.rc = genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &o.res);
  if (o.rc == 0) { o.pitch = o.res.pitch; o.surface.assign(surface.begin(), surface.begin() + static_cast<size_t>(o.res.pitch) * o.res.native_h); }
  return o;
}

// ---- classifier -------------------------------------------------------------------------------------------------------------------
enum class RowClass { Identical, Window, PerLineFallback, ConditionDependent, Unexplained };
inline const char *name(RowClass c) {
  switch (c) { case RowClass::Identical: return "PASS"; case RowClass::Window: return "LIMIT:window"; case RowClass::PerLineFallback: return "LIMIT:per-line-fallback";
               case RowClass::ConditionDependent: return "LIMIT:condition-dependent"; default: return "UNEXPLAINED"; }
}
struct Verdict {
  std::string scenario;   // PASS or the (single) LIMIT class of the differing rows; "PROVIDER_ERROR:placement" when the builder used another placement than it declared
  unsigned rows_differ = 0, rows_window = 0, rows_fallback = 0, rows_condition = 0, rows_unexplained = 0;
  unsigned windows[2] = {0, 0}, words = 0, needed[2] = {0, 0};
  unsigned abi1_calls = 0, abi2_calls = 0;   // scenery_word calls of the producer: the ABI 1 provider answers each in its own code, the ABI 2 host answers them from <= `words` window words
  bool feasible = true;
  uint64_t digest1 = 0, digest2 = 0;
};

// The truth of every asked plane cell, computed independently of the builder from the scene alone: how many distinct answers it has over all lines that ask it and whether one comes from a fallback line.
struct CellTruth { uint16_t first = 0; bool conflict = false, any_fallback = false; };
inline std::map<std::tuple<unsigned, long, long>, CellTruth> asked_cells(const Spec &s, const Scene &sc, int side, int left_px) {
  std::map<std::tuple<unsigned, long, long>, CellTruth> truth;
  const int right_px = 2 * side - left_px;
  for (unsigned plane = 0; plane < 2; ++plane)
    for (int r = 0; r < H; ++r) {
      const long cy = floor8(static_cast<long>(r) + s.vs[plane]);
      auto visit = [&](Span sp) {
        for (long c = sp.c0; c <= sp.c1; ++c) {
          const uint16_t a = abi1_answer(s, sc, plane, r, c, cy);
          auto it = truth.find({plane, c, cy});
          if (it == truth.end()) { CellTruth t; t.first = a; t.any_fallback = s.fallback[plane][r] != 0U; truth[{plane, c, cy}] = t; continue; }
          if (it->second.first != a) it->second.conflict = true;
          if (s.fallback[plane][r] != 0U) it->second.any_fallback = true;
        }
      };
      if (left_px > 0) visit(left_span(s.hs[plane][r], left_px));
      if (right_px > 0) visit(right_span(s.hs[plane][r], right_px));
    }
  return truth;
}

inline Verdict run_case(const Spec &s, int side, int left_px, Timing timing, int built_left = -1) {
  Scene sc;
  build_scene(sc, s);
  Provider ref, twin;
  ref.kind = Provider::Abi1; ref.spec = &s; ref.scene = &sc; ref.declared_left = left_px;
  twin.kind = Provider::Abi2; twin.spec = &s; twin.scene = &sc; twin.declared_left = left_px;
  Built built;
  for (unsigned plane = 0; plane < 2; ++plane) build_plane(s, sc, plane, side, built_left < 0 ? left_px : built_left, timing, built);
  for (unsigned plane = 0; plane < 2; ++plane) for (unsigned n = 0; n < 2; ++n) twin.windows[plane][n] = built.w[plane][n];
  const Produced a = produce(sc, ref, static_cast<unsigned>(side)), b = produce(sc, twin, static_cast<unsigned>(side));
  Verdict v;
  v.feasible = built.feasible[0] && built.feasible[1];
  for (unsigned plane = 0; plane < 2; ++plane) { v.needed[plane] = static_cast<unsigned>(built.needed[plane]); for (unsigned n = 0; n < 2; ++n) v.windows[plane] += built.w[plane][n].present ? 1U : 0U; }
  v.words = built.total_words; v.abi1_calls = ref.cells; v.abi2_calls = twin.cells;
  if (a.rc != 0 || b.rc != 0 || a.pitch != b.pitch || a.surface.size() != b.surface.size()) { v.scenario = "HARNESS_ERROR"; return v; }
  v.digest1 = surface_digest(a); v.digest2 = surface_digest(b);
  std::map<std::tuple<unsigned, long, long>, CellTruth> truth;
  bool truth_built = false;
  for (int r = 0; r < H; ++r) {
    if (row_digest(a, r) == row_digest(b, r) && std::memcmp(&a.surface[static_cast<size_t>(r) * a.pitch], &b.surface[static_cast<size_t>(r) * b.pitch], a.pitch) == 0) continue;
    ++v.rows_differ;
    if (!truth_built) { truth = asked_cells(s, sc, side, left_px); truth_built = true; }   // only a difference needs its cause
    bool window = false, fallback = false, condition = false;
    for (unsigned plane = 0; plane < 2; ++plane) {
      const long cy = floor8(static_cast<long>(r) + s.vs[plane]);
      for (int sidef = 0; sidef < 2; ++sidef) {
        const int px = sidef == 0 ? left_px : 2 * side - left_px;
        if (px <= 0) continue;
        const Span sp = sidef == 0 ? left_span(s.hs[plane][r], px) : right_span(s.hs[plane][r], px);
        for (long c = sp.c0; c <= sp.c1; ++c) {
          bool in_window = false;
          for (const auto &w : twin.windows[plane]) if (w.present && c >= w.x0 && c < w.x0 + static_cast<long>(w.cols) && cy >= w.y0 && cy < w.y0 + static_cast<long>(w.rows)) in_window = true;
          if (!in_window) window = true;
          const CellTruth &t = truth.at({plane, c, cy});
          if (t.conflict) (t.any_fallback ? fallback : condition) = true;
        }
      }
    }
    if (window) ++v.rows_window; else if (fallback) ++v.rows_fallback; else if (condition) ++v.rows_condition; else ++v.rows_unexplained;
  }
  if (built_left >= 0 && built_left != left_px) v.scenario = v.rows_differ != 0 ? "PROVIDER_ERROR:placement" : "PASS";
  else if (v.rows_unexplained != 0) v.scenario = "UNEXPLAINED";
  else if (v.rows_window != 0) v.scenario = "LIMIT:window";
  else if (v.rows_fallback != 0) v.scenario = "LIMIT:per-line-fallback";
  else if (v.rows_condition != 0) v.scenario = "LIMIT:condition-dependent";
  else v.scenario = "PASS";
  return v;
}

}  // namespace parity
