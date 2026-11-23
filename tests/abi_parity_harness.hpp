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
struct JournalWrite { unsigned line, plane, cell_row; };   // SEG-152-T007: a mid-frame write of every name-table word of one plane cell row (the word is inverted) applied when `line` begins
struct Spec {
  std::string id;
  int hs[2][H] = {};          // the VDP-applied horizontal scroll per plane and line, in [-512, 511]
  int k[2][H] = {};           // registered origin multiple (512 px each): world x = x - hscroll + 512 k
  int vs[2] = {0, 0};         // vertical scroll (full screen)
  int speed[2][H] = {};       // pixels per guest frame (the Predicted timing)
  uint8_t fallback[2][H] = {};   // 1: the ABI 1 provider answers this line with the VDP's own wrapped word
  uint8_t region[2][H] = {};     // the answer class of the world model on this line
  int clamp_w = 0;            // > 0: world columns clamp to [0, clamp_w) cells (level edges: the edge column repeats outward)
  uint8_t generic2[2][H] = {};   // SEG-152-T007: the ABI 1 provider answers this line GENESIS_COMPAT_WORD_GENERIC (a plane deliberately left to the generic extension, ADR 0314: uncredited)
  std::vector<JournalWrite> journal;   // SEG-152-T007: mid-frame name-table writes (the producer replays them; an ABI 1 provider reads the frame-start VRAM)
  int window_from_row = -1;    // SEG-152-T007: >= 0 (a multiple of 8): the window plane covers the full width from that row down (a status band: window-plane edge / HUD rows)
  bool no_journal = false;     // SEG-152-T007: the frame carries no VRAM journal (GENESIS_EXT_FLAG_NO_VRAM_JOURNAL)
  // SEG-152-T008: the presentation gate's extra state of the same fixture (all default to "absent", which leaves every earlier scenario byte-identical).
  uint8_t pal[H] = {};         // per-line palette variant: the CRAM of that output line has every colour channel raised by this many steps (0 = the base palette): raster palette effects (water, gradients)
  uint8_t fade = 0;            // 0..7: every line's CRAM is darkened by this many steps (a fade or a transition in progress)
  bool hud_tiles = false;      // the game's own HUD glyph tiles are present in VRAM (tile indices kHudTile0..+3), drawn by the provider from the frame view
  uint8_t scene_id = 0;        // work RAM byte 0: the game's scene (the gate's provider logic decides acceptance from it, as the production providers do)
  uint8_t bad_hud = 0;         // work RAM byte 3: 1 = the HUD state is inconsistent (the provider refuses the HUD only)
};
constexpr unsigned kHudTile0 = 0x40;   // the HUD glyph tiles of Spec::hud_tiles (VRAM 0x800..0x87F): not referenced by any name table of the fixture

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
  if (spec.hud_tiles)   // SEG-152-T008: four glyph tiles (4 bpp, colour indices 0..7 by a fixed formula) of the game's own HUD
    for (unsigned t = 0; t < 4; ++t)
      for (unsigned row = 0; row < 8; ++row)
        for (unsigned col = 0; col < 4; ++col) {
          const unsigned a = (kHudTile0 + t) * 32 + row * 4 + col;
          const unsigned hi = ((row * 3 + col * 2 + t) % 8), lo = ((row + col * 5 + t * 3) % 8);
          sc.vram[a] = static_cast<uint8_t>(hi << 4 | lo);
        }
  vsram[0] = static_cast<uint8_t>(spec.vs[0] >> 8); vsram[1] = static_cast<uint8_t>(spec.vs[0]);
  vsram[2] = static_cast<uint8_t>(spec.vs[1] >> 8); vsram[3] = static_cast<uint8_t>(spec.vs[1]);
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT] = {};
  regs[1] = 0x04; regs[2] = 0x30; regs[3] = 0x34; regs[4] = 0x07; regs[5] = 0x7C; regs[7] = 2; regs[11] = 3; regs[12] = 0x81; regs[13] = 0x3F; regs[16] = 1;   // per-line hscroll, full-screen vscroll, 64 x 32
  if (spec.window_from_row >= 0) regs[18] = static_cast<uint16_t>(0x80U | (spec.window_from_row / 8));
  unsigned line_no = 0;
  for (auto &line : sc.raster->lines) {
    std::memcpy(line.registers, regs, sizeof(line.registers));
    std::memcpy(line.cram, cram, sizeof(line.cram));
    std::memcpy(line.vsram, vsram, sizeof(line.vsram));
    const unsigned raise = line_no < static_cast<unsigned>(H) ? spec.pal[line_no] : 0U;   // SEG-152-T008: the per-line palette effect and the fade, on the CRAM words of that line
    if (raise != 0U || spec.fade != 0U)
      for (unsigned i = 0; i < 64; ++i) {
        unsigned w = static_cast<unsigned>(line.cram[i * 2] << 8 | line.cram[i * 2 + 1]);
        unsigned out = 0;
        for (unsigned ch = 0; ch < 3; ++ch) {
          int c = static_cast<int>((w >> (1 + 4 * ch)) & 7U) + static_cast<int>(raise) - static_cast<int>(spec.fade);
          c = c < 0 ? 0 : (c > 7 ? 7 : c);
          out |= static_cast<unsigned>(c) << (1 + 4 * ch);
        }
        line.cram[i * 2] = static_cast<uint8_t>(out >> 8); line.cram[i * 2 + 1] = static_cast<uint8_t>(out);
      }
    ++line_no;
  }
  sc.raster->has_vram_journal = spec.no_journal ? 0 : 1;
  std::memcpy(sc.raster->vram_start, sc.vram.data(), GENESIS_VDP_VRAM_BYTES);
  for (const JournalWrite &j : spec.journal)   // `sc.vram` stays the frame-start image (what an ABI 1 provider is handed); the journal evolves the producer's own copy
    for (unsigned cx = 0; cx < kPlaneCols; ++cx)
      for (unsigned b = 0; b < 2; ++b) {
        const uint32_t a = (j.plane == 0U ? 0xC000U : 0xE000U) + ((j.cell_row % kPlaneRows) * kPlaneCols + cx) * 2U + b;
        sc.raster->vram_writes[sc.raster->vram_write_count++] = GenesisRasterVramWrite{static_cast<uint16_t>(a), static_cast<uint8_t>(sc.vram[a] ^ (b == 0U ? 0x60U : 0x0DU)), static_cast<uint8_t>(j.line)};
      }
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
  segarecomp::scenery::RowMask mask[2];   // SEG-152-T007: the per-row generic mask (the production type and row test)
  int mask_code = GENESIS_COMPAT_WORD_VDP_START;   // what the adapter answers for a marked row (3); a control run answers GENESIS_COMPAT_WORD_GENERIC instead
  uint8_t private_words = 1U;   // the trusted attach (GenesisFrameExtension::compat_private_words)
  unsigned components = GENESIS_COMPAT_SCENERY;   // SEG-152-T008: the components the host will use (the gate adds HUD)
  uint8_t ram[4] = {0, 0, 0, 0};   // SEG-152-T008: work RAM bytes 0..3 of the previous-onset snapshot the provider is handed (scene id, frame number, fade counter, bad-HUD flag)
  unsigned cells = 0;
  const GenesisNativeCompat *table = nullptr;   // optional: another table (the gate cases name the table like the adapter's)
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
    if (p->spec->generic2[plane][row] != 0U) return GENESIS_COMPAT_WORD_GENERIC;
    *word = abi1_answer(*p->spec, *p->scene, plane, static_cast<int>(row), pc, pcy);
    return 1;
  }
  return segarecomp::scenery::answer(p->mask[plane], p->windows[plane], static_cast<int>(row), x, hscroll, vscroll, word, p->mask_code, GENESIS_COMPAT_WORD_GENERIC);   // the adapter's own answer (mask, ADR 0314 generic plane, window)
}
inline int provider_hud(void *, GenesisCompatOverlay *) { return 0; }
inline GenesisNativeCompat &twin_table() {
  static GenesisNativeCompat table = {GENESIS_NATIVE_COMPAT_ABI, "parity.twin", 1, GENESIS_COMPAT_SCENERY, provider_prepare, provider_word, provider_hud};
  return table;
}

// ---- the ABI 2 window builder (the provider model) -------------------------------------------------------------------------------------
enum class Timing { Exact, Predicted };
// SEG-152-T007: how the ABI 2 twin is built. `use_mask`: the builder marks every fallback row of a plane in the per-row mask and builds the windows from the other rows only (the mask-using builder);
// `mask_code`/`private_words` select the host answer for a marked row (the code-3 gate cases) and the trusted-attach flag.
struct Options {
  bool use_mask = false;
  int mask_code = GENESIS_COMPAT_WORD_VDP_START;
  uint8_t private_words = 1U;
  bool mask_plane[2] = {true, true};   // which planes submit a mask
  bool clear_mask = false;             // negative control: the builder computed the mask-aware windows but the submission never reaches the host
};
struct Built {
  segarecomp::scenery::Window w[2][2];
  int needed[2] = {0, 0};        // windows the plane needs under the production limits (greedy, left and right separately)
  bool feasible[2] = {true, true};
  unsigned max_words[2] = {0, 0};
  unsigned total_words = 0;
};

// Minimum cover of the union of closed integer intervals by windows of exactly `cap` columns (the greedy cover from the left is optimal). Every interval is covered completely, however many windows that takes;
// overlapping and adjacent coverage is merged (a window starts at the leftmost uncovered column); `cap < 1` (no window can hold even one full-height column) yields no window, which the caller reports as infeasible.
inline std::vector<std::pair<long, long>> cover_columns(std::vector<std::pair<long, long>> intervals, long cap) {
  std::vector<std::pair<long, long>> out;
  if (cap < 1) return out;
  std::sort(intervals.begin(), intervals.end());
  long covered_to = 0;
  bool have = false;
  for (const auto &p : intervals) {
    if (p.second < p.first || (have && p.second <= covered_to)) continue;
    long start = (have && p.first <= covered_to) ? covered_to + 1 : p.first;
    for (;;) {
      out.push_back({start, start + cap - 1});
      covered_to = start + cap - 1;
      have = true;
      if (covered_to >= p.second) break;
      start = covered_to + 1;
    }
  }
  return out;
}

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
inline bool row_skipped(const Spec &s, const Options &opt, unsigned plane, int r) { return s.generic2[plane][r] != 0U || (opt.use_mask && opt.mask_plane[plane] && s.fallback[plane][r] != 0U); }
inline void build_plane(const Spec &s, const Scene &sc, unsigned plane, int side, int built_left, Timing timing, Built &out, const Options &opt = Options{}) {
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
    if (row_skipped(s, opt, plane, r)) continue;   // answered by the mask (or left to the generic extension): no authored word is needed for it
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
  // Column slicing: full-height windows of at most min(128, 4096 / rows) columns covering the union of every span (`cover_columns`). The greedy cover from the left is optimal for intervals, so
  // `needed` is the minimum number of such windows; two of them are a feasible partition (the host answers a cell from the first window that contains it, so a span may straddle windows).
  {
    long ymin = 0, ymax = -1;
    for (const CellSpan &sp : spans) { if (ymax < ymin) { ymin = ymax = sp.cy; } ymin = std::min(ymin, sp.cy); ymax = std::max(ymax, sp.cy); }
    const long rows = ymax - ymin + 1;
    const long cap = ymax >= ymin && rows <= static_cast<long>(segarecomp::scenery::kRowsMax) ? std::min(static_cast<long>(segarecomp::scenery::kColsMax), static_cast<long>(segarecomp::scenery::kWordsMax) / rows) : 0L;
    std::vector<std::pair<long, long>> iv;
    for (const CellSpan &sp : spans) iv.push_back({sp.c0, sp.c1});
    const std::vector<std::pair<long, long>> cols = cover_columns(iv, cap);
    std::vector<Rect> slices;
    for (const auto &c : cols) slices.push_back(Rect{ymin, ymax, c.first, c.second, true});
    out.needed[plane] = static_cast<int>(slices.size());
    if (!chosen.empty()) out.needed[plane] = std::min(out.needed[plane], static_cast<int>(chosen.size()));
    if (chosen.empty() && !spans.empty()) {
      if (slices.size() <= 2) chosen = slices;
      else { out.feasible[plane] = false; chosen = {slices[0], slices[1]}; }   // a true representation limit: more windows than the contract allows; the best a provider can do is the first two
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
struct Produced {
  int rc = -1; GenesisExtensionResult res{}; std::vector<uint8_t> surface; size_t pitch = 0;
  std::vector<uint8_t> overlay, layers;   // SEG-152-T008: the overlay rows the producer reported (overlay_h rows of 128 RGBA pixels) and the layer image bytes the descriptors address
};
// The coverage of two produced frames: accepted components, reason, the strict `authored` verdict and the per-row credit (all must be equal to ABI 1: no tolerance).
inline bool coverage_equal(const Produced &a, const Produced &b) {
  return a.res.compat_accepted == b.res.compat_accepted && a.res.compat_reason == b.res.compat_reason && a.res.authored == b.res.authored && a.res.authored_row_count == b.res.authored_row_count &&
         std::memcmp(a.res.authored_rows, b.res.authored_rows, sizeof a.res.authored_rows) == 0;
}
// SEG-152-T008: the ONE presentation digest: the produced surface, the verdict (components, reason, authored, per-row credit, scene token, placement shift, HUD rows) and the overlay and layer output, folded with the same FNV-1a
// as the surface digest. `Mismatch` names the first component that differs: what a failing gate reports.
inline uint64_t overlay_digest(const Produced &p) { uint64_t h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.overlay_w), sizeof p.res.overlay_w); h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.overlay_h), sizeof p.res.overlay_h, h); h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.overlay_fx), sizeof p.res.overlay_fx, h); h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.overlay_fy), sizeof p.res.overlay_fy, h); return fnv1a64(p.overlay.data(), p.overlay.size(), h); }
inline uint64_t layers_digest(const Produced &p) { uint64_t h = fnv1a64(&p.res.layer_count, 1); h = fnv1a64(&p.res.layer_bad, 1, h); h = fnv1a64(reinterpret_cast<const uint8_t *>(p.res.layers), sizeof p.res.layers, h); return fnv1a64(p.layers.data(), p.layers.size(), h); }
inline uint64_t verdict_digest(const Produced &p) {
  uint64_t h = fnv1a64(&p.res.compat_accepted, 1); h = fnv1a64(&p.res.compat_reason, 1, h); h = fnv1a64(&p.res.authored, 1, h);
  h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.authored_row_count), sizeof p.res.authored_row_count, h); h = fnv1a64(p.res.authored_rows, sizeof p.res.authored_rows, h);
  h = fnv1a64(reinterpret_cast<const uint8_t *>(p.res.scene), sizeof p.res.scene, h); h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.shift), sizeof p.res.shift, h);
  h = fnv1a64(reinterpret_cast<const uint8_t *>(&p.res.hud_rows), sizeof p.res.hud_rows, h); h = fnv1a64(&p.res.flags, 1, h);
  return fnv1a64(&p.res.status, 1, h);
}
inline uint64_t row_digest(const Produced &p, int row) { return fnv1a64(&p.surface[static_cast<size_t>(row) * p.pitch], p.pitch); }
inline uint64_t surface_digest(const Produced &p) { uint64_t h = 14695981039346656037ULL; for (int r = 0; r < H; ++r) h = fnv1a64(&p.surface[static_cast<size_t>(r) * p.pitch], p.pitch, h); return h; }
inline uint64_t presentation_digest(const Produced &p) {
  uint64_t h = fnv1a64(p.surface.data(), p.surface.size());
  for (const uint64_t d : {verdict_digest(p), overlay_digest(p), layers_digest(p)}) h = fnv1a64(reinterpret_cast<const uint8_t *>(&d), sizeof d, h);
  return h;
}
// The named failing assertions of a comparison between a reference (ABI 1) and a twin (ABI 2): empty means the presentation is identical component by component.
inline std::vector<std::string> presentation_mismatches(const Produced &a, const Produced &b) {
  std::vector<std::string> m;
  if (a.rc != b.rc) { m.push_back("rc"); return m; }
  if (a.rc != 0) return m;
  if (a.res.compat_accepted != b.res.compat_accepted) m.push_back("accepted-components");
  if (a.res.compat_reason != b.res.compat_reason) m.push_back("reason");
  if (a.surface != b.surface) m.push_back("surface");
  if (overlay_digest(a) != overlay_digest(b)) m.push_back("overlay");
  if (layers_digest(a) != layers_digest(b)) m.push_back("layers");
  if (std::strcmp(a.res.scene, b.res.scene) != 0) m.push_back("scene");
  if (a.res.shift != b.res.shift) m.push_back("placement");
  if (a.res.authored != b.res.authored || a.res.authored_row_count != b.res.authored_row_count || std::memcmp(a.res.authored_rows, b.res.authored_rows, sizeof a.res.authored_rows) != 0) m.push_back("coverage");
  if (m.empty() && presentation_digest(a) != presentation_digest(b)) m.push_back("digest");
  return m;
}

inline Produced produce(const Scene &sc, Provider &prov, unsigned side) {
  static std::vector<uint8_t> surface(GENESIS_EXT_SURFACE_BYTES), overlay(GENESIS_COMPAT_OVERLAY_BYTES), ram(GENESIS_COMPAT_RAM_BYTES), layer(GENESIS_COMPAT_LAYER_BYTES);
  PlanCtx pc{side};
  auto native = std::make_unique<GenesisFrameArtifact>();
  Produced o;
  if (genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) != 0) return o;
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.producer = genesis_vdp_produce_extension; ext.pixels = surface.data(); ext.capacity = surface.size(); ext.request = 2;
  ext.compat = prov.table != nullptr ? prov.table : &twin_table(); ext.compat_state = &prov; ext.compat_components = prov.components; ext.compat_caps = GENESIS_CAP_SCENERY_WORDS | GENESIS_CAP_FRAME_VDP | GENESIS_CAP_FRAME_WORK_RAM | ((prov.components & GENESIS_COMPAT_HUD) != 0U ? GENESIS_CAP_OVERLAY_RGBA : 0U);
  ext.compat_private_words = prov.private_words; ext.compat_ram = ram.data(); ext.compat_ram_valid = 1; ext.overlay_rgba = overlay.data(); ext.layer_rgba = layer.data();
  std::fill(ram.begin(), ram.end(), static_cast<uint8_t>(0)); std::memcpy(ram.data(), prov.ram, sizeof prov.ram);
  std::fill(overlay.begin(), overlay.end(), static_cast<uint8_t>(0xEE)); std::fill(layer.begin(), layer.end(), static_cast<uint8_t>(0xEE));   // poison: only what the producer's own clear and the provider write is observable
  std::fill(surface.begin(), surface.end(), static_cast<uint8_t>(0xAA));
  o.rc = genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &o.res);
  if (o.rc == 0) {
    o.pitch = o.res.pitch; o.surface.assign(surface.begin(), surface.begin() + static_cast<size_t>(o.res.pitch) * o.res.native_h);
    o.overlay.assign(overlay.begin(), overlay.begin() + static_cast<size_t>(o.res.overlay_h) * GENESIS_COMPAT_OVERLAY_MAX_W * 4U);
    size_t used = 0;
    for (unsigned i = 0; i < o.res.layer_count && i < GENESIS_COMPAT_LAYERS_MAX; ++i) {
      const GenesisCompatLayer &l = o.res.layers[i];
      if ((o.res.layer_bad >> i & 1U) == 0U) used = std::max(used, static_cast<size_t>(l.offset) + static_cast<size_t>(l.width) * l.height * 4U);
    }
    o.layers.assign(layer.begin(), layer.begin() + used);
  }
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
  unsigned uncovered = 0;   // requested (plane, line, plane cell) triples outside every submitted window, counted independently of the pixels
  bool coverage_equal = false;   // SEG-152-T007: compat_accepted/reason, authored, authored_rows and authored_row_count of the twin equal the ABI 1 reference
  unsigned authored_rows1 = 0, authored_rows2 = 0;   // authored_row_count of the reference and of the twin
  uint8_t authored1 = 0, authored2 = 0, accepted1 = 0, accepted2 = 0, reason1 = 0, reason2 = 0;
  unsigned abi1_calls = 0, abi2_calls = 0;   // scenery_word calls of the producer: the ABI 1 provider answers each in its own code, the ABI 2 host answers them from <= `words` window words
  bool feasible = true;
  uint64_t digest1 = 0, digest2 = 0;
};

// The truth of every asked plane cell, computed independently of the builder from the scene alone: how many distinct answers it has over all lines that ask it and whether one comes from a fallback line.
struct CellTruth { uint16_t first = 0; bool conflict = false, any_fallback = false; };
inline std::map<std::tuple<unsigned, long, long>, CellTruth> asked_cells(const Spec &s, const Scene &sc, int side, int left_px, const Options &opt = Options{}) {
  std::map<std::tuple<unsigned, long, long>, CellTruth> truth;
  const int right_px = 2 * side - left_px;
  for (unsigned plane = 0; plane < 2; ++plane)
    for (int r = 0; r < H; ++r) {
      if (row_skipped(s, opt, plane, r)) continue;
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

// SEG-152-T008: builds the ABI 2 twin's scenery submission (the bounded windows and, for the mask-using builder, the per-row mask) for a declared placement and installs it in `twin`. One implementation for
// `run_case` and for the presentation gate (which arms its twins with it): there is no second builder.
inline void arm_twin(Provider &twin, const Spec &s, const Scene &sc, int side, int built_left, Timing timing, const Options &opt, Built &built) {
  for (unsigned plane = 0; plane < 2; ++plane) build_plane(s, sc, plane, side, built_left, timing, built, opt);
  if (opt.use_mask)   // the mask-using builder submits, per plane, the rows its model answers with the VDP's own word (bit r & 7 of byte r >> 3, produced screen rows)
    for (unsigned plane = 0; plane < 2; ++plane) {
      if (!opt.mask_plane[plane]) continue;
      uint8_t bytes[(H + 7) / 8] = {};
      for (int r = 0; r < H; ++r) if (s.fallback[plane][r] != 0U) bytes[r >> 3] = static_cast<uint8_t>(bytes[r >> 3] | (1U << (r & 7)));
      if (!segarecomp::scenery::mask_in_range(plane, bytes, H, (H + 7) / 8)) { std::printf("HARNESS: the builder's own mask is out of range\n"); std::abort(); }
      if (!opt.clear_mask) segarecomp::scenery::mask_store(twin.mask[plane], bytes, (H + 7) / 8);
    }
  for (unsigned plane = 0; plane < 2; ++plane) for (unsigned n = 0; n < 2; ++n) twin.windows[plane][n] = built.w[plane][n];
}

inline Verdict run_case(const Spec &s, int side, int left_px, Timing timing, int built_left = -1, const Options &opt = Options{}) {
  Scene sc;
  build_scene(sc, s);
  Provider ref, twin;
  ref.kind = Provider::Abi1; ref.spec = &s; ref.scene = &sc; ref.declared_left = left_px;
  twin.kind = Provider::Abi2; twin.spec = &s; twin.scene = &sc; twin.declared_left = left_px;
  twin.mask_code = opt.mask_code; twin.private_words = opt.private_words;
  Built built;
  arm_twin(twin, s, sc, side, built_left < 0 ? left_px : built_left, timing, opt, built);
  const Produced a = produce(sc, ref, static_cast<unsigned>(side)), b = produce(sc, twin, static_cast<unsigned>(side));
  Verdict v;
  v.feasible = built.feasible[0] && built.feasible[1];
  for (unsigned plane = 0; plane < 2; ++plane) { v.needed[plane] = static_cast<unsigned>(built.needed[plane]); for (unsigned n = 0; n < 2; ++n) v.windows[plane] += built.w[plane][n].present ? 1U : 0U; }
  v.words = built.total_words; v.abi1_calls = ref.cells; v.abi2_calls = twin.cells;
  if (a.rc != 0 || b.rc != 0 || a.pitch != b.pitch || a.surface.size() != b.surface.size()) { v.scenario = "HARNESS_ERROR"; return v; }
  v.digest1 = surface_digest(a); v.digest2 = surface_digest(b);
  v.coverage_equal = coverage_equal(a, b);
  v.authored_rows1 = a.res.authored_row_count; v.authored_rows2 = b.res.authored_row_count; v.authored1 = a.res.authored; v.authored2 = b.res.authored;
  v.accepted1 = a.res.compat_accepted; v.accepted2 = b.res.compat_accepted; v.reason1 = a.res.compat_reason; v.reason2 = b.res.compat_reason;
  for (unsigned plane = 0; plane < 2; ++plane)
    for (int r = 0; r < H; ++r) {
      if (row_skipped(s, opt, plane, r)) continue;
      const long cy = floor8(static_cast<long>(r) + s.vs[plane]);
      for (int sidef = 0; sidef < 2; ++sidef) {
        const int px = sidef == 0 ? left_px : 2 * side - left_px;
        if (px <= 0) continue;
        const Span sp = sidef == 0 ? left_span(s.hs[plane][r], px) : right_span(s.hs[plane][r], px);
        for (long c = sp.c0; c <= sp.c1; ++c) {
          bool in = false;
          for (const auto &w : twin.windows[plane]) if (w.present && c >= w.x0 && c < w.x0 + static_cast<long>(w.cols) && cy >= w.y0 && cy < w.y0 + static_cast<long>(w.rows)) in = true;
          if (!in) ++v.uncovered;
        }
      }
    }
  std::map<std::tuple<unsigned, long, long>, CellTruth> truth;
  bool truth_built = false;
  for (int r = 0; r < H; ++r) {
    if (row_digest(a, r) == row_digest(b, r) && std::memcmp(&a.surface[static_cast<size_t>(r) * a.pitch], &b.surface[static_cast<size_t>(r) * b.pitch], a.pitch) == 0) continue;
    ++v.rows_differ;
    if (!truth_built) { truth = asked_cells(s, sc, side, left_px, opt); truth_built = true; }   // only a difference needs its cause
    bool window = false, fallback = false, condition = false;
    for (unsigned plane = 0; plane < 2; ++plane) {
      if (row_skipped(s, opt, plane, r)) continue;
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
