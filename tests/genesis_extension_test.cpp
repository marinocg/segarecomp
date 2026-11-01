// SEG-118 (ADR 0262): Level 2 extended backgrounds produced from authentic VDP state, presentation only.
//
// Project-authored synthetic VDP scenes (random but deterministic tiles, nametables, scroll tables, palettes), no ROM and no clock.
// The extension pixels are compared with a SMALL INDEPENDENT REFERENCE SAMPLER written from the documented hardware rules in this file; it does
// not call the production sampling helpers. Native invariance is checked on the artifact bytes (incl. digest) and on the whole VDP/scheduler state
// of two identical runtimes, one with and one without an extension target.
// The reference sampler mixes signed scroll arithmetic with unsigned geometry on purpose; keep it readable instead of cast-heavy.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "runtime.h"
#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
void checkf(bool ok, const char *what, unsigned a, unsigned b = 0, unsigned c = 0) {
  if (!ok) { std::printf("FAIL: %s (%u %u %u)\n", what, a, b, c); ++failures; }
}

struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
  uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
  uint32_t below(uint32_t n) { return next() % n; }
};

struct Cfg {
  bool h40 = true, im2 = false;
  unsigned hmode = 0;         // register 11 bits 1-0: 0 full screen, 2 per 8 lines, 3 per line
  bool column_vscroll = false;
  unsigned plane_w_field = 1, plane_h_field = 0;  // register 16 fields (1 = 64 cells, 0 = 32 cells, 3 = 128 cells)
  int window = 0;             // 0 none, 1 left of column 6, 2 right of column 4, 3 top 3 cells, 4 bottom from cell 20 (right-half too)
  unsigned side = 39;
  bool midframe = false;      // per-line CRAM / scroll-mode / plane-size changes after line 90, and a journaled VRAM write at line 120
  bool journal = true;
  uint32_t seed = 1;
};

struct Scene {
  std::vector<uint8_t> vram = std::vector<uint8_t>(GENESIS_VDP_VRAM_BYTES, 0);   // VRAM at the frame boundary (after journal writes)
  std::vector<uint8_t> vram_start = std::vector<uint8_t>(GENESIS_VDP_VRAM_BYTES, 0);
  std::unique_ptr<GenesisRasterFrameState> raster = std::make_unique<GenesisRasterFrameState>();
};

unsigned cells_of(unsigned field) { return field == 0 ? 32u : field == 1 ? 64u : 128u; }

void build_scene(const Cfg &c, Scene &sc) {
  Lcg rnd(c.seed);
  std::memset(sc.raster.get(), 0, sizeof(*sc.raster));
  for (size_t i = 0; i < 0x4000; ++i) sc.vram[i] = (uint8_t)rnd.next();            // tiles 0..511 (8x8) / 0..255 (8x16)
  for (size_t i = 0xC000; i < 0x10000; ++i) sc.vram[i] = 0;
  for (uint32_t base : {0xC000u, 0xE000u})
    for (uint32_t i = 0; i < 0x2000; i += 2) {
      const unsigned w = (rnd.below(2) << 15) | (rnd.below(4) << 13) | (rnd.below(2) << 12) | (rnd.below(2) << 11) | rnd.below(250);
      sc.vram[base + i] = (uint8_t)(w >> 8);
      sc.vram[base + i + 1] = (uint8_t)w;
    }
  for (uint32_t i = 0; i < 224 * 4; i += 2) {                                       // scroll table at 0xFC00
    const unsigned v = rnd.below(1024);
    sc.vram[0xFC00 + i] = (uint8_t)(v >> 8);
    sc.vram[0xFC00 + i + 1] = (uint8_t)v;
  }
  if (c.hmode == 0) { sc.vram[0xFC00] = 0x01; sc.vram[0xFC01] = 0x23; sc.vram[0xFC02] = 0x03; sc.vram[0xFC03] = 0x9A; }  // a clear scroll for A and B
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  for (unsigned i = 0; i < 64; ++i) { const unsigned w = rnd.below(512) << 1; cram[i * 2] = (uint8_t)(w >> 8); cram[i * 2 + 1] = (uint8_t)w; }
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  for (unsigned i = 0; i < GENESIS_VDP_VSRAM_BYTES; i += 2) { const unsigned v = rnd.below(c.im2 ? 2048 : 1024); vsram[i] = (uint8_t)(v >> 8); vsram[i + 1] = (uint8_t)v; }
  if (!c.column_vscroll) { vsram[2] = vsram[0] ^ 0; /* single value per plane */ }
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT] = {};
  regs[0] = 0;
  regs[1] = 0x04;
  regs[2] = 0x30;                       // Plane A at 0xC000
  regs[3] = 0x34;                       // window at 0xD000
  regs[4] = 0x07;                       // Plane B at 0xE000
  regs[5] = 0x7C;
  regs[7] = (uint16_t)rnd.below(64);
  regs[11] = (uint16_t)(c.hmode | (c.column_vscroll ? 4u : 0u));
  regs[12] = (uint16_t)((c.h40 ? 0x81u : 0x00u) | (c.im2 ? 0x06u : 0u));
  regs[13] = 0x3F;
  regs[16] = (uint16_t)(c.plane_w_field | (c.plane_h_field << 4));
  switch (c.window) {
    case 1: regs[17] = 3; regs[18] = 0; break;               // columns left of 6 cells
    case 2: regs[17] = 0x80 | 12; regs[18] = 0; break;      // columns from 12 cells (24 in 16-px units?) -> right
    case 3: regs[17] = 0; regs[18] = 3; break;               // top 3 cell rows
    case 4: regs[17] = 0; regs[18] = 0x80 | 20; break;       // from cell row 20 down
    default: break;
  }
  for (auto &line : sc.raster->lines) {
    std::memcpy(line.registers, regs, sizeof(line.registers));
    std::memcpy(line.cram, cram, sizeof(line.cram));
    std::memcpy(line.vsram, vsram, sizeof(line.vsram));
  }
  sc.vram_start = sc.vram;
  if (c.midframe) {
    uint8_t cram2[GENESIS_VDP_CRAM_BYTES];
    for (unsigned i = 0; i < 64; ++i) { const unsigned w = rnd.below(512) << 1; cram2[i * 2] = (uint8_t)(w >> 8); cram2[i * 2 + 1] = (uint8_t)w; }
    for (unsigned l = 90; l < GENESIS_RASTER_MAX_LINES; ++l) {
      std::memcpy(sc.raster->lines[l].cram, cram2, sizeof(cram2));
      if (l >= 150) {                                         // another scroll mode and another plane width for the last lines
        sc.raster->lines[l].registers[11] = (uint16_t)(((c.hmode == 3) ? 0u : 3u) | (c.column_vscroll ? 4u : 0u));
        sc.raster->lines[l].registers[16] = (uint16_t)((c.plane_h_field ? (c.plane_w_field == 1 ? 0u : 1u) : (c.plane_w_field == 3 ? 0u : 3u)) | (c.plane_h_field << 4));
      }
    }
    // a journaled VRAM write during line 120: retarget a tile's pixels so lines after it differ
    if (c.journal) {
      sc.raster->vram_write_count = 0;
      for (unsigned t = 0; t < 40; ++t) {
        const uint16_t addr = (uint16_t)(t * 32 + 4 * 3);
        sc.raster->vram_writes[sc.raster->vram_write_count++] = {addr, (uint8_t)(0x11 * (1 + t % 14)), 120};
        sc.vram[addr] = (uint8_t)(0x11 * (1 + t % 14));
      }
    }
  }
  sc.raster->has_vram_journal = c.journal ? 1 : 0;
  std::memcpy(sc.raster->vram_start, sc.vram_start.data(), GENESIS_VDP_VRAM_BYTES);
}

// ---- the independent reference sampler -----------------------------------------------------------------------------------
struct Ref {
  const Scene *sc;
  Cfg c;
  unsigned width() const { return c.h40 ? 320u : 256u; }
  unsigned height() const { return c.im2 ? 448u : 224u; }
};

int sext(unsigned v, unsigned bits) { v &= (1u << bits) - 1u; return (v & (1u << (bits - 1))) ? (int)v - (int)(1u << bits) : (int)v; }
int mod(long long v, long long m) { long long r = v % m; return (int)(r < 0 ? r + m : r); }

// The VRAM image visible while output row `row` is drawn.
std::vector<uint8_t> vram_at(const Ref &r, unsigned field_line) {
  std::vector<uint8_t> v(r.sc->vram_start);
  if (r.sc->raster->has_vram_journal)
    for (unsigned i = 0; i < r.sc->raster->vram_write_count; ++i)
      if (r.sc->raster->vram_writes[i].line < field_line) v[r.sc->raster->vram_writes[i].address] = r.sc->raster->vram_writes[i].value;
  return v;
}

struct Pix { bool opaque; unsigned prio, pal, idx; };

Pix ref_plane(const Ref &r, const GenesisRasterLine &ln, const std::vector<uint8_t> &vram, bool plane_b, long long x, unsigned row, unsigned vgroup) {
  const unsigned field_line = r.c.im2 ? row / 2 : row;
  const unsigned wcells = cells_of(ln.registers[16] & 3), hcells = cells_of((ln.registers[16] >> 4) & 3);
  const unsigned cellh = r.c.im2 ? 16 : 8;
  const unsigned mode = ln.registers[11] & 3;
  const unsigned entry = mode == 0 ? 0 : (mode == 2 ? (field_line & ~7u) : field_line);
  const unsigned haddr = ((ln.registers[13] & 0x3F) << 10) + entry * 4 + (plane_b ? 2 : 0);
  const int hs = sext((vram[haddr] << 8) | vram[haddr + 1], 10);
  const bool colmode = (ln.registers[11] & 4) != 0;
  const unsigned vaddr = (plane_b ? 2 : 0) + (colmode ? vgroup * 4 : 0);
  const int vs = sext((ln.vsram[vaddr] << 8) | ln.vsram[vaddr + 1], r.c.im2 ? 11 : 10);
  const int px = mod(x - hs, (long long)wcells * 8), py = mod((long long)row + vs, (long long)hcells * cellh);
  const unsigned base = plane_b ? ((ln.registers[4] & 7) << 13) : ((ln.registers[2] & 0x38) << 10);
  const unsigned naddr = base + ((py / cellh) * wcells + (px / 8)) * 2;
  const unsigned w = (vram[naddr] << 8) | vram[naddr + 1];
  unsigned tx = px % 8, ty = py % cellh;
  if (w & 0x800) tx = 7 - tx;
  if (w & 0x1000) ty = cellh - 1 - ty;
  const unsigned taddr = (w & 0x7FF) * (r.c.im2 ? 64 : 32) + ty * 4 + tx / 2;
  const unsigned nib = (tx & 1) ? (vram[taddr] & 15) : (vram[taddr] >> 4);
  return {nib != 0, (w >> 15) & 1, (w >> 13) & 3, nib};
}

bool ref_window(const Ref &r, const GenesisRasterLine &ln, unsigned x, unsigned row) {
  const unsigned cellh = r.c.im2 ? 16 : 8;
  const unsigned hb = (ln.registers[17] & 31) * 2 * 8;       // boundary in pixels (2-cell units)
  const unsigned vb = (ln.registers[18] & 31) * cellh;
  const bool hr = ln.registers[17] & 0x80, vd = ln.registers[18] & 0x80;
  const bool horiz = hr ? x >= hb : x < hb;
  const bool vert = vd ? row >= vb : row < vb;
  return horiz || vert;
}

unsigned ref_index(const Ref &r, const GenesisRasterLine &ln, const std::vector<uint8_t> &vram, long long x, unsigned row, bool left, unsigned groups) {
  const bool wedge = ref_window(r, ln, left ? 0u : r.width() - 1u, row);
  const unsigned g = left ? 0u : groups - 1u;
  Pix a = wedge ? Pix{false, 0, 0, 0} : ref_plane(r, ln, vram, false, x, row, g);
  Pix b = ref_plane(r, ln, vram, true, x, row, g);
  if (a.opaque && a.prio) return a.pal * 16 + a.idx;
  if (b.opaque && b.prio) return b.pal * 16 + b.idx;
  if (a.opaque) return a.pal * 16 + a.idx;
  if (b.opaque) return b.pal * 16 + b.idx;
  return ln.registers[7] & 0x3F;
}

void rgb_of(const uint8_t *cram, unsigned idx, uint8_t out[3]) {
  const unsigned w = (cram[idx * 2] << 8) | cram[idx * 2 + 1];
  out[0] = (uint8_t)(((w >> 1) & 7) * 255 / 7); out[1] = (uint8_t)(((w >> 5) & 7) * 255 / 7); out[2] = (uint8_t)(((w >> 9) & 7) * 255 / 7);
}

// Artifact -> RGB without the production conversion.
void native_rgb(const GenesisFrameArtifact &f, unsigned x, unsigned row, uint8_t out[3]) {
  const unsigned slot = f.row_palette[row];
  const uint8_t *cram = slot == 0 ? f.palette_snapshot : f.extra_palettes[slot - 1];
  rgb_of(cram, f.pixels[(size_t)row * f.width + x], out);
}

struct PlanCtx { unsigned side; };
int plan_fn(void *ctx, unsigned, unsigned, unsigned *side) {
  const unsigned s = static_cast<PlanCtx *>(ctx)->side;
  if (s == 0) return -1;
  *side = s;
  return 0;
}

std::vector<uint8_t> g_surface(GENESIS_EXT_SURFACE_BYTES);

struct Outcome { int rc; GenesisExtensionResult res; bool native_ok; };

// Runs the production producer for a scene and verifies every pixel against the reference. Returns the production result.
Outcome run_and_compare(const Cfg &c, const char *name, bool expect_ok = true) {
  Scene sc;
  build_scene(c, sc);
  auto native = std::make_unique<GenesisFrameArtifact>();
  std::memset(native.get(), 0, sizeof(*native));
  Outcome out{};
  out.native_ok = genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) == 0;
  if (!out.native_ok) { check(!expect_ok, name); return out; }
  auto before = std::make_unique<GenesisFrameArtifact>(*native);
  PlanCtx pc{c.side};
  GenesisFrameExtension ext{};
  ext.plan = plan_fn;
  ext.plan_ctx = &pc;
  ext.pixels = g_surface.data();
  ext.capacity = g_surface.size();
  std::memset(g_surface.data(), 0xA5, g_surface.size());
  out.rc = genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &out.res);
  check(std::memcmp(before.get(), native.get(), sizeof(*native)) == 0, "the native artifact is not modified by the extension call");
  if (out.rc != 0) { check(!expect_ok, name); return out; }
  check(out.res.status == GENESIS_EXT_AVAILABLE || out.res.status == GENESIS_EXT_PARTIAL, name);
  Ref ref{&sc, c};
  const unsigned W = native->width, H = native->height, S = c.side, pitch = (W + 2 * S) * 3;
  checkf(out.res.native_w == W && out.res.native_h == H && out.res.side == S && out.res.pitch == pitch, "result geometry", out.res.native_w, out.res.side, out.res.pitch);
  bool window_edge = false, colvs = false;
  unsigned bad = 0, bad_centre = 0;
  for (unsigned row = 0; row < H; ++row) {
    const unsigned fl = c.im2 ? row / 2 : row;
    const GenesisRasterLine &ln = sc.raster->lines[fl];
    const std::vector<uint8_t> vram = sc.raster->has_vram_journal ? vram_at(ref, fl) : sc.vram;
    const unsigned groups = W / 16;
    window_edge |= ref_window(ref, ln, 0, row) || ref_window(ref, ln, W - 1, row);
    if (ln.registers[11] & 4) {
      for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned g = 1; g < groups; ++g)
          if (std::memcmp(&ln.vsram[plane * 2], &ln.vsram[plane * 2], 2) == 0 &&
              (ln.vsram[plane * 2 + g * 4] != ln.vsram[plane * 2] || ln.vsram[plane * 2 + g * 4 + 1] != ln.vsram[plane * 2 + 1])) colvs = true;
    }
    for (unsigned i = 0; i < S; ++i) {
      for (int left = 1; left >= 0; --left) {
        const long long x = left ? (long long)i - (long long)S : (long long)W + i;
        uint8_t want[3];
        rgb_of(ln.cram, ref_index(ref, ln, vram, x, row, left != 0, groups), want);
        const uint8_t *got = g_surface.data() + (size_t)row * pitch + (left ? i : S + W + i) * 3;
        if (std::memcmp(want, got, 3) != 0) ++bad;
      }
    }
    for (unsigned x = 0; x < W; ++x) {
      uint8_t want[3];
      native_rgb(*native, x, row, want);
      if (std::memcmp(want, g_surface.data() + (size_t)row * pitch + (S + x) * 3, 3) != 0) ++bad_centre;
    }
  }
  checkf(bad == 0, name, bad);
  checkf(bad_centre == 0, "the surface centre equals the native picture", bad_centre);
  const uint8_t want_flags = (uint8_t)((window_edge ? GENESIS_EXT_FLAG_WINDOW_EDGE : 0) | (colvs ? GENESIS_EXT_FLAG_COLUMN_VSCROLL : 0) |
                                       (c.journal ? 0 : GENESIS_EXT_FLAG_NO_VRAM_JOURNAL));
  checkf(out.res.flags == want_flags, "partial caveat flags", out.res.flags, want_flags);
  check((out.res.status == GENESIS_EXT_AVAILABLE) == (want_flags == 0), "AVAILABLE exactly when there is no caveat");
  return out;
}

void synthetic_fixtures() {
  struct Item { const char *name; Cfg c; };
  std::vector<Item> items;
  auto add = [&](const char *n, Cfg c) { items.push_back({n, c}); };
  Cfg base;
  add("H40 full-screen scroll", base);
  { Cfg c = base; c.h40 = false; c.side = 71; add("H32 full-screen scroll", c); }
  { Cfg c = base; c.hmode = 3; add("per-line scroll", c); }
  { Cfg c = base; c.hmode = 2; add("per-8-line scroll", c); }
  { Cfg c = base; c.hmode = 3; c.h40 = false; c.side = 71; add("H32 per-line", c); }
  { Cfg c = base; c.column_vscroll = true; add("column vscroll (partial)", c); }
  { Cfg c = base; c.column_vscroll = true; c.h40 = false; c.side = 71; add("H32 column vscroll", c); }
  { Cfg c = base; c.plane_w_field = 0; add("32-cell wide planes: wraps inside the native width", c); }
  { Cfg c = base; c.plane_w_field = 3; c.plane_h_field = 0; add("128-cell wide planes", c); }
  { Cfg c = base; c.plane_w_field = 1; c.plane_h_field = 1; c.hmode = 3; add("64x64 planes, per-line", c); }
  { Cfg c = base; c.window = 1; add("window over the left columns", c); }
  { Cfg c = base; c.window = 2; add("window over the right columns", c); }
  { Cfg c = base; c.window = 3; add("window over the top rows (edges covered)", c); }
  { Cfg c = base; c.window = 4; c.hmode = 3; add("window at the bottom rows", c); }
  { Cfg c = base; c.im2 = true; c.side = 100; add("interlace mode 2", c); }
  { Cfg c = base; c.im2 = true; c.hmode = 3; c.h40 = false; c.side = 100; add("H32 interlace mode 2 per-line", c); }
  { Cfg c = base; c.midframe = true; c.hmode = 3; add("mid-frame CRAM/scroll/size and VRAM journal", c); }
  { Cfg c = base; c.midframe = true; c.journal = false; add("no VRAM journal (partial)", c); }
  { Cfg c = base; c.side = 1; add("one-pixel extension", c); }
  { Cfg c = base; c.side = GENESIS_EXT_MAX_SIDE; c.hmode = 3; add("maximum extension", c); }
  for (uint32_t seed = 2; seed < 14; ++seed) {
    Cfg c = base;
    c.seed = seed;
    c.h40 = seed % 2 == 0;
    c.side = c.h40 ? 39 : 71;
    c.hmode = (seed % 3 == 0) ? 3 : ((seed % 3 == 1) ? 2 : 0);
    c.column_vscroll = seed % 4 == 0;
    c.plane_w_field = (seed % 3 == 2) ? 3u : seed % 3;
    c.plane_h_field = c.plane_w_field == 3 ? 0u : (seed / 3) % 2;
    c.window = (int)(seed % 5);
    c.im2 = seed % 7 == 0;
    c.midframe = seed % 2 == 1;
    items.push_back({"seeded mix", c});
  }
  for (auto &it : items) {
    const int before = failures;
    const Outcome o = run_and_compare(it.c, it.name);
    (void)o;
    if (failures != before) std::printf("  ^ seed %u h40 %d hmode %u colvs %d pw %u ph %u win %d im2 %d mid %d native_ok %d rc %d status %u\n", it.c.seed, it.c.h40, it.c.hmode, it.c.column_vscroll, it.c.plane_w_field, it.c.plane_h_field, it.c.window, it.c.im2, it.c.midframe, o.native_ok, o.rc, o.res.status);
  }
  // the specific caveats appear where the scene creates them
  { Cfg c = base; c.window = 1; const Outcome o = run_and_compare(c, "window left"); check(o.res.status == GENESIS_EXT_PARTIAL && (o.res.flags & GENESIS_EXT_FLAG_WINDOW_EDGE), "window over an edge is reported Partial"); }
  { Cfg c = base; const Outcome o = run_and_compare(c, "plain"); check(o.res.status == GENESIS_EXT_AVAILABLE && o.res.flags == 0, "a plain scene is Available"); }
  { Cfg c = base; c.hmode = 3; const Outcome o = run_and_compare(c, "line scroll"); check(o.res.status == GENESIS_EXT_AVAILABLE, "per-line scroll is derivable: Available"); }
  { Cfg c = base; c.column_vscroll = true; const Outcome o = run_and_compare(c, "col"); check(o.res.status == GENESIS_EXT_PARTIAL && (o.res.flags & GENESIS_EXT_FLAG_COLUMN_VSCROLL), "differing column vscroll is Partial"); }
  { Cfg c = base; c.midframe = true; c.journal = false; const Outcome o = run_and_compare(c, "nojournal"); check(o.res.status == GENESIS_EXT_PARTIAL && (o.res.flags & GENESIS_EXT_FLAG_NO_VRAM_JOURNAL), "missing journal is Partial"); }
}

void failures_and_edges() {
  Cfg c;
  Scene sc;
  build_scene(c, sc);
  auto native = std::make_unique<GenesisFrameArtifact>();
  std::memset(native.get(), 0, sizeof(*native));
  check(genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) == 0, "native renders");
  PlanCtx pc{39};
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.pixels = g_surface.data(); ext.capacity = g_surface.size();
  GenesisExtensionResult r{};
  const size_t need = (size_t)(320 + 2 * 39) * 3 * 224;

  // undersized targets: typed fallback, nothing written
  for (size_t cap : {size_t(0), size_t(1), need - 1}) {
    std::memset(g_surface.data(), 0x5A, need);
    ext.capacity = cap;
    check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &r) != 0 && r.status == GENESIS_EXT_UNAVAILABLE && r.reason == GENESIS_EXT_REASON_TARGET_TOO_SMALL, "undersized buffer is Unavailable/TARGET_TOO_SMALL");
    bool untouched = true;
    for (size_t i = 0; i < need; ++i) untouched &= g_surface[i] == 0x5A;
    check(untouched, "an undersized buffer is never written");
  }
  ext.capacity = need;
  check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &r) == 0 && r.status == GENESIS_EXT_AVAILABLE, "exactly sized buffer works");
  // exact-size guard bytes after the surface are untouched
  g_surface[need] = 0xEE; g_surface[need + 1] = 0xEF;
  ext.capacity = need;
  (void)genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &r);
  check(g_surface[need] == 0xEE && g_surface[need + 1] == 0xEF, "no write past the surface");

  // invalid arguments
  check(genesis_vdp_produce_extension(nullptr, sc.raster.get(), native.get(), &ext, &r) != 0 && r.status == GENESIS_EXT_UNAVAILABLE, "NULL VRAM");
  check(genesis_vdp_produce_extension(sc.vram.data(), nullptr, native.get(), &ext, &r) != 0, "NULL raster");
  check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), nullptr, &ext, &r) != 0, "NULL native");
  check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), nullptr, &r) != 0, "NULL target");
  check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, nullptr) != 0, "NULL result");
  { GenesisFrameExtension e2 = ext; e2.pixels = nullptr; check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &e2, &r) != 0, "NULL surface"); }
  { GenesisFrameExtension e2 = ext; e2.plan = nullptr; check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &e2, &r) != 0 && r.reason == GENESIS_EXT_REASON_NO_PLAN, "no plan callback"); }
  for (unsigned s : {0u, GENESIS_EXT_MAX_SIDE + 1u, 100000u}) {
    PlanCtx p2{s};
    GenesisFrameExtension e2 = ext; e2.plan_ctx = &p2;
    check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &e2, &r) != 0 && r.status == GENESIS_EXT_UNAVAILABLE, "zero / oversized extension request");
  }
  // a native artifact that disagrees with the raster state
  { auto bad = std::make_unique<GenesisFrameArtifact>(*native); bad->width = 256;
    check(genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), bad.get(), &ext, &r) != 0 && r.reason == GENESIS_EXT_REASON_NATIVE_GEOMETRY, "native/raster geometry mismatch"); }
  // unsupported display modes fail the native render, so there is nothing to extend
  { Scene s2; build_scene(c, s2); for (auto &l : s2.raster->lines) l.registers[12] = 0x80; auto f = std::make_unique<GenesisFrameArtifact>();
    std::memset(f.get(), 0, sizeof(*f));
    check(genesis_vdp_produce_extension(s2.vram.data(), s2.raster.get(), native.get(), &ext, &r) != 0, "reserved display mode is refused by the extension too"); }
}

// Register fuzzing: whatever the registers say, no out-of-bounds access (run under sanitizers in CI) and a typed answer.
void fuzz_registers() {
  Lcg rnd(77);
  unsigned ok = 0, refused = 0;
  Cfg c;
  Scene sc;
  build_scene(c, sc);
  auto native = std::make_unique<GenesisFrameArtifact>();
  PlanCtx pc{39};
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.pixels = g_surface.data(); ext.capacity = g_surface.size();
  for (unsigned iter = 0; iter < 400; ++iter) {
    uint16_t mutated[GENESIS_VDP_REGISTER_COUNT];
    for (unsigned r = 0; r < GENESIS_VDP_REGISTER_COUNT; ++r) mutated[r] = (uint16_t)(rnd.below(3) == 0 ? rnd.next() & 0xFF : sc.raster->lines[0].registers[r]);
    for (auto &line : sc.raster->lines) std::memcpy(line.registers, mutated, sizeof(mutated));
    if (rnd.below(4) == 0)  // some frames also change scroll/plane registers per line
      for (unsigned l = 40; l < GENESIS_RASTER_MAX_LINES; ++l) {
        sc.raster->lines[l].registers[11] = (uint16_t)rnd.below(16);
        sc.raster->lines[l].registers[16] = (uint16_t)(rnd.next() & 0x33);
        sc.raster->lines[l].registers[13] = (uint16_t)(rnd.next() & 0x3F);
      }
    std::memset(native.get(), 0, sizeof(*native));
    GenesisExtensionResult r{};
    if (genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) != 0) { ++refused; continue; }
    const int rc = genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &r);
    check((rc == 0) == (r.status == GENESIS_EXT_AVAILABLE || r.status == GENESIS_EXT_PARTIAL), "return code and status agree");
    ++ok;
    if (iter % 50 == 49) build_scene(Cfg{}, sc);
  }
  check(ok > 0 && refused > 0, "the fuzz exercised both supported and refused register files");
}

// ---- runtime integration: publication, sequence stamping, native invariance, steady-state allocation -----------------------
constexpr uint32_t kRamBegin = 0x00FF0000u;
struct Rig {
  std::unique_ptr<GenesisRuntime> r = std::make_unique<GenesisRuntime>();
  GenesisFrameArtifact frame;
  GenesisLiveFrameObserver observer;
  std::unique_ptr<GenesisRasterCapture> capture = std::make_unique<GenesisRasterCapture>();
  uint32_t pc = 0x200u;
  Rig(const Cfg &cfg, GenesisFrameExtension *ext) {
    std::memset(r.get(), 0, sizeof(*r));
    std::memset(&frame, 0, sizeof(frame));
    std::memset(&observer, 0, sizeof(observer));
    std::memset(capture.get(), 0, sizeof(*capture));
    r->pc = 0x200u; r->sr = 0x2000u; r->a[7] = kRamBegin + 0x8000u;
    Scene sc;
    build_scene(cfg, sc);
    std::memcpy(r->devices.vdp.registers, sc.raster->lines[0].registers, sizeof(r->devices.vdp.registers));
    std::memcpy(r->devices.vdp.cram, sc.raster->lines[0].cram, sizeof(r->devices.vdp.cram));
    std::memcpy(r->devices.vdp.vsram, sc.raster->lines[0].vsram, sizeof(r->devices.vdp.vsram));
    std::memcpy(r->devices.vdp.vram, sc.vram.data(), GENESIS_VDP_VRAM_BYTES);
    observer.producer = genesis_vdp_produce_frame;
    observer.latest = &frame;
    capture->producer = genesis_vdp_produce_frame_raster;
    capture->extension = ext;
    r->live_frame_observer = &observer;
    r->live_raster = capture.get();
  }
  void run_frame() {
    const uint64_t vf = r->virtual_frame_count;
    while (r->virtual_frame_count == vf) pc = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc).next_pc;
  }
};

void runtime_integration() {
  Cfg c;
  c.hmode = 3;
  PlanCtx pc{39};
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.producer = genesis_vdp_produce_extension; ext.pixels = g_surface.data(); ext.capacity = g_surface.size();
  Rig with(c, &ext), without(c, nullptr);

  // not requested: no extension work, the result says NONE and is stamped with the frame
  std::memset(g_surface.data(), 0x33, 64);
  with.run_frame(); without.run_frame();
  check(with.observer.sequence == 1 && ext.result.sequence == 1 && ext.result.status == GENESIS_EXT_NONE && g_surface[0] == 0x33 && ext.produced == 0, "not requested: nothing produced, result stamped NONE");

  // requested: the frame published is stamped with its own sequence
  ext.request = 1;
  with.run_frame(); without.run_frame();
  check(with.observer.sequence == 2 && ext.result.sequence == 2 && (ext.result.status == GENESIS_EXT_AVAILABLE || ext.result.status == GENESIS_EXT_PARTIAL) && ext.produced == 1, "requested: result stamped with the published sequence");
  check(std::memcmp(&with.frame, &without.frame, sizeof(with.frame)) == 0, "native artifact bytes (incl. digest) identical with and without the extension");
  check(std::memcmp(with.frame.frame_digest, without.frame.frame_digest, 32) == 0, "frame digest identical");
  check(with.observer.sequence == without.observer.sequence && with.r->virtual_frame_count == without.r->virtual_frame_count &&
            with.r->scheduler.master_ticks == without.r->scheduler.master_ticks && with.r->pc == without.r->pc,
        "guest time, frame count and pc identical");
  check(std::memcmp(&with.r->devices.vdp, &without.r->devices.vdp, sizeof(with.r->devices.vdp)) == 0, "VDP hardware state identical");
  // the centre of the surface is that very frame
  { const size_t pitch = ext.result.pitch; bool same = true;
    for (unsigned row = 0; row < with.frame.height && same; row += 9)
      for (unsigned x = 0; x < with.frame.width && same; x += 5) { uint8_t want[3]; native_rgb(with.frame, x, row, want); same = std::memcmp(want, &g_surface[row * pitch + (ext.result.side + x) * 3], 3) == 0; }
    check(same, "surface centre equals the published native frame"); }

  // a mid-run mode switch back and forth: the switch takes effect at the next frame boundary and leaves the guest alone
  ext.request = 0;
  with.run_frame(); without.run_frame();
  check(ext.result.status == GENESIS_EXT_NONE && ext.result.sequence == 3, "switching to Original: next frame is NONE");
  ext.request = 1;
  with.run_frame(); without.run_frame();
  check(ext.result.sequence == 4 && ext.result.status != GENESIS_EXT_NONE && ext.produced == 2, "switching back: the next frame is extended again");
  check(std::memcmp(&with.frame, &without.frame, sizeof(with.frame)) == 0 && with.r->scheduler.master_ticks == without.r->scheduler.master_ticks, "still identical after the switches");

  // forced failure: an undersized surface -> UNAVAILABLE stamped with the same sequence; recovery on the next frame
  const size_t good = ext.capacity;
  ext.capacity = 1000;
  with.run_frame(); without.run_frame();
  check(ext.result.sequence == 5 && ext.result.status == GENESIS_EXT_UNAVAILABLE && ext.result.reason == GENESIS_EXT_REASON_TARGET_TOO_SMALL && ext.failed == 1, "undersized surface: typed UNAVAILABLE for that frame only");
  check(std::memcmp(&with.frame, &without.frame, sizeof(with.frame)) == 0, "native frame identical after a failed extension");
  ext.capacity = good;
  with.run_frame(); without.run_frame();
  check(ext.result.sequence == 6 && ext.result.status != GENESIS_EXT_UNAVAILABLE, "recovers on the next frame");

  // no producer / no surface configured but requested
  { GenesisFrameExtension e2{}; e2.request = 1; Rig r2(c, &e2); r2.run_frame();
    check(e2.result.status == GENESIS_EXT_UNAVAILABLE && e2.result.reason == GENESIS_EXT_REASON_INVALID_REQUEST && e2.result.sequence == 1, "request without a producer is UNAVAILABLE, not a crash"); }

  // a rejected native frame (reserved display mode) publishes nothing and leaves the previous result stale (sequence unchanged)
  const GenesisExtensionResult before = ext.result;
  with.r->devices.vdp.registers[12] = 0x80;
  with.run_frame();
  check(with.observer.sequence == 6 && std::memcmp(&before, &ext.result, sizeof before) == 0, "a rejected frame does not touch the result: a consumer sees a stale sequence");
  with.r->devices.vdp.registers[12] = 0x81;
  with.run_frame(); with.run_frame();

  // steady state: no heap traffic while frames are produced with the extension requested
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  { with.run_frame();
    const struct mallinfo2 a = mallinfo2();
    for (int i = 0; i < 6; ++i) with.run_frame();
    const struct mallinfo2 b = mallinfo2();
    check(a.uordblks == b.uordblks && a.arena == b.arena, "no heap allocation in the steady state"); }
#endif
#endif
}

}  // namespace

int main() {
  synthetic_fixtures();
  failures_and_edges();
  fuzz_registers();
  runtime_integration();
  if (failures != 0) { std::printf("genesis_extension_test: %d failure(s)\n", failures); return 1; }
  std::printf("genesis_extension_test: ok\n");
  return 0;
}
