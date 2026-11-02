// SEG-124 (ADR 0267): the generic native-patch hook of the extension producer, the work-RAM snapshot timing of the runtime and the public SDK contract.
//
// Project-authored synthetic scenes only (no ROM, no clock). The hook is exercised with a MOCK provider (so the generic code is proven game-independent); no game provider exists in the product; a real
// provider is verified in its own package's tests (the Sonic 1 one lives in the private harness).
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "runtime.h"
#include "segarecomp/patch.hpp"
#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
void checkf(bool ok, const char *what, long a, long b = 0) {
  if (!ok) { std::printf("FAIL: %s (%ld %ld)\n", what, a, b); ++failures; }
}

std::vector<uint8_t> g_surface(GENESIS_EXT_SURFACE_BYTES);
std::vector<uint8_t> g_overlay(GENESIS_COMPAT_OVERLAY_BYTES);
std::vector<uint8_t> g_ram(GENESIS_COMPAT_RAM_BYTES);

struct PlanCtx { unsigned side; };
int plan_fn(void *ctx, unsigned, unsigned, unsigned *side_out) { *side_out = static_cast<PlanCtx *>(ctx)->side; return 0; }

// ---- a small deterministic scene: 64x32 planes, per-line scroll, solid-colour tiles ----------------------------------------
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
  uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
  uint32_t below(uint32_t n) { return next() % n; }
};

struct Scene {
  std::vector<uint8_t> vram = std::vector<uint8_t>(GENESIS_VDP_VRAM_BYTES, 0);
  std::unique_ptr<GenesisRasterFrameState> raster = std::make_unique<GenesisRasterFrameState>();
};

// Tile t (1..15) is a solid tile of palette index t; tile 0 is transparent. Nametables get random words over those tiles.
void build_scene(Scene &sc, uint32_t seed, int hscroll_a = -40, int hscroll_b = -23) {
  Lcg rnd(seed);
  std::memset(sc.raster.get(), 0, sizeof(*sc.raster));
  for (unsigned t = 1; t < 16; ++t)
    for (unsigned i = 0; i < 32; ++i) sc.vram[t * 32 + i] = (uint8_t)(t * 0x11);
  for (uint32_t base : {0xC000u, 0xE000u})
    for (uint32_t i = 0; i < 0x2000; i += 2) {
      const unsigned w = (rnd.below(2) << 15) | (rnd.below(4) << 13) | rnd.below(16);   // priority, palette line, tile 0..15 (no flips)
      sc.vram[base + i] = (uint8_t)(w >> 8);
      sc.vram[base + i + 1] = (uint8_t)w;
    }
  for (unsigned l = 0; l < 224; ++l) {                                  // per-line scroll table at 0xFC00
    const int a = hscroll_a, b = hscroll_b + (int)(l / 8);
    sc.vram[0xFC00 + l * 4] = (uint8_t)((a & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 1] = (uint8_t)a;
    sc.vram[0xFC00 + l * 4 + 2] = (uint8_t)((b & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 3] = (uint8_t)b;
  }
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  for (unsigned i = 0; i < 64; ++i) { const unsigned w = ((i * 5 + 3) % 8) << 1 | ((i * 3 + 1) % 8) << 5 | ((i * 7 + 2) % 8) << 9; cram[i * 2] = (uint8_t)(w >> 8); cram[i * 2 + 1] = (uint8_t)w; }
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES] = {};
  vsram[1] = 0x20; vsram[3] = 0x40;                                      // vertical scroll A = 32, B = 64
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT] = {};
  regs[1] = 0x04; regs[2] = 0x30; regs[3] = 0x34; regs[4] = 0x07; regs[5] = 0x7C; regs[7] = 2; regs[11] = 3; regs[12] = 0x81; regs[13] = 0x3F; regs[16] = 1;
  for (auto &line : sc.raster->lines) {
    std::memcpy(line.registers, regs, sizeof(line.registers));
    std::memcpy(line.cram, cram, sizeof(line.cram));
    std::memcpy(line.vsram, vsram, sizeof(line.vsram));
  }
  sc.raster->has_vram_journal = 1;
  std::memcpy(sc.raster->vram_start, sc.vram.data(), GENESIS_VDP_VRAM_BYTES);
}

// ---- the mock provider --------------------------------------------------------------------------------------------------------
constexpr unsigned kAllCaps = GENESIS_CAP_FRAME_WORK_RAM | GENESIS_CAP_FRAME_VDP | GENESIS_CAP_SCENERY_WORDS | GENESIS_CAP_OVERLAY_RGBA;
struct Mock {
  unsigned accept = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD;
  uint8_t reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
  int word_mode = 0;               // 0: a fixed solid word; 1: refuse every cell (provider fault); 2: the VDP's own cell (identity)
  uint16_t fixed_word = 5;         // solid tile 5, palette 0, low priority
  const uint8_t *vram = nullptr;
  unsigned prepared = 0, cells = 0, huds = 0;
  const uint8_t *seen_ram = nullptr, *seen_vram = nullptr;
  const GenesisCompatLine *seen_lines = nullptr;
  const GenesisCompatHost *seen_host = nullptr;
  unsigned ovl_w = 12, ovl_h = 9;
  bool bad_overlay = false;
} g_mock;

void mock_prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Mock *m = static_cast<Mock *>(state);
  ++m->prepared;
  m->seen_ram = f->work_ram;
  m->seen_vram = f->vram;
  m->seen_lines = f->lines;
  m->seen_host = f->host;
  out->accepted = m->accept & f->requested;
  out->reason = m->reason;
}
int mock_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  Mock *m = static_cast<Mock *>(state);
  ++m->cells;
  if (m->word_mode == 1) return 0;
  if (m->word_mode == 2) {
    const unsigned px = (unsigned)(x - hscroll) & 511U, py = (unsigned)((int)row + vscroll) & 255U;
    const uint32_t base = plane == 0 ? 0xC000u : 0xE000u;
    const uint32_t a = base + ((py / 8U) * 64U + px / 8U) * 2U;
    *word = (uint16_t)((m->vram[a] << 8) | m->vram[a + 1]);
    return 1;
  }
  *word = m->fixed_word;
  return 1;
}
int mock_hud(void *state, GenesisCompatOverlay *o) {
  Mock *m = static_cast<Mock *>(state);
  ++m->huds;
  o->width = (uint16_t)(m->bad_overlay ? GENESIS_COMPAT_OVERLAY_MAX_W + 1 : m->ovl_w);
  o->height = (uint16_t)m->ovl_h;
  if (!m->bad_overlay) for (unsigned i = 0; i < m->ovl_w * m->ovl_h; ++i) { o->rgba[i * 4] = 200; o->rgba[i * 4 + 3] = 255; }
  return 1;
}
GenesisNativeCompat g_provider = {GENESIS_NATIVE_COMPAT_ABI, "synthetic.mock", 1, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, mock_prepare, mock_word, mock_hud};

struct Out { int rc; GenesisExtensionResult res; std::vector<uint8_t> surface; };

Out produce(const Scene &sc, GenesisFrameExtension &ext, unsigned side = 39) {
  PlanCtx pc{side};
  auto native = std::make_unique<GenesisFrameArtifact>();
  Out o{};
  if (genesis_vdp_produce_frame_raster(sc.vram.data(), sc.raster.get(), native.get()) != 0) { o.rc = -99; return o; }
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.producer = genesis_vdp_produce_extension; ext.pixels = g_surface.data(); ext.capacity = g_surface.size(); ext.request = 2;
  std::memset(g_surface.data(), 0xAA, g_surface.size());
  o.rc = genesis_vdp_produce_extension(sc.vram.data(), sc.raster.get(), native.get(), &ext, &o.res);
  if (o.rc == 0) o.surface.assign(g_surface.begin(), g_surface.begin() + (size_t)o.res.pitch * o.res.native_h);
  return o;
}

void attach(GenesisFrameExtension &ext, bool valid = true) {
  ext.compat = &g_provider;
  ext.compat_state = &g_mock;
  ext.compat_components = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD;
  ext.compat_caps = kAllCaps;
  ext.compat_ram = g_ram.data();
  ext.compat_ram_valid = valid ? 1 : 0;
  ext.overlay_rgba = g_overlay.data();
}

void generic_hook() {
  Scene sc;
  build_scene(sc, 11);
  g_mock = Mock{};
  g_mock.vram = sc.vram.data();
  // baseline: no provider
  GenesisFrameExtension plain{};
  const Out base = produce(sc, plain);
  check(base.rc == 0 && base.res.compat_accepted == 0 && base.res.authored == 0 && base.res.overlay_w == 0, "no provider: nothing compat in the result");
  const unsigned side = base.res.side, W = base.res.native_w, H = base.res.native_h;
  const size_t pitch = base.res.pitch;
  auto centre_equal = [&](const Out &a, const Out &b) {
    for (unsigned r = 0; r < H; ++r)
      if (std::memcmp(&a.surface[r * pitch + side * 3], &b.surface[r * pitch + side * 3], (size_t)W * 3) != 0) return false;
    return true;
  };
  // provider serving a fixed solid word: every side pixel is that tile's colour; the native centre is untouched
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data();
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.res.compat_accepted == (GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD) && o.res.compat_reason == 0, "accepted frame: components reported");
    check(centre_equal(o, base), "accepted frame: the native centre is byte-identical to the generic frame");
    // the left strip must differ from the generic one (the provider replaced the wrapped nametable) and be one colour per line
    bool differs = false, one_colour = true;
    for (unsigned r = 0; r < H; ++r) {
      for (unsigned i = 1; i < side; ++i) one_colour = one_colour && std::memcmp(&o.surface[r * pitch], &o.surface[r * pitch + i * 3], 3) == 0 && std::memcmp(&o.surface[r * pitch + (side + W) * 3], &o.surface[r * pitch + (side + W + i) * 3], 3) == 0;
      differs = differs || std::memcmp(&o.surface[r * pitch], &base.surface[r * pitch], (size_t)side * 3) != 0;
    }
    check(differs && one_colour, "accepted frame: the side columns are the provider's cells, not the wrapped nametable");
    check(o.res.authored == 1, "accepted frame with scenery and no degradation is authored");
    check(o.res.overlay_w == 12 && o.res.overlay_h == 9 && g_mock.huds == 1, "the HUD overlay is reported with its size");
    check(g_mock.cells > 0 && g_mock.prepared == 1, "prepare once per frame, cells only for off-screen columns");
    // the provider is asked for exactly the off-screen columns: 2 planes x 2 sides x side x H cells
    checkf(g_mock.cells == 2u * 2u * side * H, "cell calls = planes x sides x side x rows", g_mock.cells, 2u * 2u * side * H);
    // identity words (the VDP's own cell at the same plane coordinates) reproduce the generic frame
    GenesisFrameExtension ext2{}; attach(ext2); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.word_mode = 2;
    const Out same = produce(sc, ext2);
    check(same.rc == 0 && same.surface == base.surface, "a provider returning the VDP's own cells reproduces the generic frame byte for byte");
  }
  // declined frame: byte-identical to generic, accurate reason, never authored
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.accept = 0; g_mock.reason = GENESIS_COMPAT_BAD_SCROLL;
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_accepted == 0 && o.res.compat_reason == GENESIS_COMPAT_BAD_SCROLL && o.res.authored == 0 && o.res.overlay_w == 0, "declined frame: generic output, the provider's reason, not authored, no HUD");
    check(g_mock.cells == 0, "declined frame: no cell is requested");
  }
  // HUD only: scenery stays generic and is not authored
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.accept = GENESIS_COMPAT_HUD;
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_accepted == GENESIS_COMPAT_HUD && o.res.authored == 0 && o.res.overlay_w == 12, "HUD only: generic scenery, not authored, overlay present");
  }
  // scenery only: no overlay
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.accept = GENESIS_COMPAT_SCENERY;
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.res.authored == 1 && o.res.overlay_w == 0 && g_mock.huds == 0, "scenery only: no overlay and no HUD call");
  }
  // descriptor enables only one component: the other is never requested
  {
    GenesisFrameExtension ext{}; attach(ext); ext.compat_components = GENESIS_COMPAT_HUD; g_mock = Mock{}; g_mock.vram = sc.vram.data();
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.res.compat_accepted == GENESIS_COMPAT_HUD && o.surface == base.surface && g_mock.cells == 0, "only the descriptor's components are requested");
  }
  // a provider that fails a cell it accepted: generic fallback for that cell, FAULT reported, never authored
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.word_mode = 1;
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.authored == 0 && o.res.compat_reason == GENESIS_COMPAT_FAULT, "a failing provider falls back per cell, is reported as FAULT and is not authored");
  }
  // stale snapshot
  {
    GenesisFrameExtension ext{}; attach(ext, false); g_mock = Mock{}; g_mock.vram = sc.vram.data();
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && g_mock.prepared == 0 && o.res.compat_reason == GENESIS_COMPAT_STALE_STATE && o.res.authored == 0, "no fresh snapshot: the provider is not even asked");
  }
  // ABI mismatch / missing prepare / no RAM buffer
  {
    GenesisNativeCompat bad = g_provider; bad.abi_version = 99;
    GenesisFrameExtension ext{}; attach(ext); ext.compat = &bad; g_mock = Mock{}; g_mock.vram = sc.vram.data();
    Out o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_reason == GENESIS_COMPAT_FAULT && g_mock.prepared == 0, "a provider with another ABI version is refused");
    bad = g_provider; bad.prepare = nullptr; ext.compat = &bad; o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_reason == GENESIS_COMPAT_FAULT, "a provider without prepare is refused");
    ext.compat = &g_provider; ext.compat_ram = nullptr; o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_reason == GENESIS_COMPAT_STALE_STATE, "no snapshot buffer: stale, generic");
    ext.compat_ram = g_ram.data(); ext.compat_components = 0; o = produce(sc, ext);
    check(o.rc == 0 && o.surface == base.surface && o.res.compat_accepted == 0, "no enabled component: generic");
  }
  // an invalid overlay is dropped (never read), the frame is otherwise served
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); g_mock.bad_overlay = true;
    const Out o = produce(sc, ext);
    check(o.rc == 0 && o.res.overlay_w == 0 && o.res.overlay_h == 0 && o.res.authored == 1, "an oversize overlay is dropped, the frame stays valid");
  }
  // not requested: no provider call at all
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data();
    GenesisExtensionResult r{}; uint8_t vram[GENESIS_VDP_VRAM_BYTES] = {}; GenesisRasterFrameState raster{}; GenesisFrameArtifact native{};
    ext.request = 0;
    genesis_frame_extension_run(&ext, vram, &raster, &native, &r);
    check(r.status == GENESIS_EXT_NONE && g_mock.prepared == 0, "request 0: no provider call");
  }
  // window plane covering an edge: provenance is not authored (the frame is Partial)
  {
    Scene w; build_scene(w, 11);
    for (auto &line : w.raster->lines) { line.registers[17] = 3; line.registers[18] = 0; }
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = w.vram.data();
    const Out o = produce(w, ext);
    check(o.rc == 0 && o.res.status == GENESIS_EXT_PARTIAL && (o.res.flags & GENESIS_EXT_FLAG_WINDOW_EDGE) && o.res.authored == 0, "a window-covered edge degrades the frame: not authored");
  }
  // determinism
  {
    GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data();
    const Out a = produce(sc, ext); const Out b = produce(sc, ext);
    check(a.surface == b.surface && std::memcmp(&a.res, &b.res, sizeof a.res) == 0, "identical inputs give identical output");
  }
  // widths: every plan side from 1 to 256 produces a valid frame whose centre equals the generic centre
  for (unsigned side : {1u, 16u, 37u, 39u, 60u, 100u, 256u}) {
    GenesisFrameExtension e0{}, e1{}; attach(e1); g_mock = Mock{}; g_mock.vram = sc.vram.data();
    const Out a = produce(sc, e0, side), b = produce(sc, e1, side);
    bool ok = a.rc == 0 && b.rc == 0 && b.res.side == side;
    const size_t p = b.res.pitch;
    for (unsigned r = 0; ok && r < b.res.native_h; ++r) ok = std::memcmp(&a.surface[r * p + side * 3], &b.surface[r * p + side * 3], 320 * 3) == 0;
    checkf(ok, "native centre identical at this side width", side);
  }
}

// ---- runtime integration: snapshot timing, native invariance, allocation ----------------------------------------------------------
constexpr uint32_t kRamBegin = 0x00FF0000u;
struct Rig {
  std::unique_ptr<GenesisRuntime> r = std::make_unique<GenesisRuntime>();
  GenesisFrameArtifact frame;
  GenesisLiveFrameObserver observer;
  std::unique_ptr<GenesisRasterCapture> capture = std::make_unique<GenesisRasterCapture>();
  uint32_t pc = 0x200u;
  Rig(GenesisFrameExtension *ext) {
    std::memset(r.get(), 0, sizeof(*r));
    std::memset(&frame, 0, sizeof(frame));
    std::memset(&observer, 0, sizeof(observer));
    std::memset(capture.get(), 0, sizeof(*capture));
    r->pc = 0x200u; r->sr = 0x2000u; r->a[7] = kRamBegin + 0x8000u;
    Scene sc;
    build_scene(sc, 5);
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

uint8_t g_seen_marker[64];
unsigned g_seen_count = 0, g_seen_valid_frames = 0;
void snapshot_prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  (void)state;
  if (g_seen_count < 64) g_seen_marker[g_seen_count] = f->work_ram[0x1234];
  ++g_seen_count;
  ++g_seen_valid_frames;
  out->accepted = f->requested;
  out->reason = 0;
}

void runtime_snapshot() {
  GenesisNativeCompat snap = g_provider;
  snap.prepare = snapshot_prepare;
  snap.scenery_word = mock_word;
  g_mock = Mock{};
  PlanCtx pc{39};
  GenesisFrameExtension ext{};
  ext.plan = plan_fn; ext.plan_ctx = &pc; ext.producer = genesis_vdp_produce_extension; ext.pixels = g_surface.data(); ext.capacity = g_surface.size();
  ext.compat = &snap; ext.compat_state = &g_mock; ext.compat_components = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD; ext.compat_caps = kAllCaps; ext.compat_ram = g_ram.data(); ext.overlay_rgba = g_overlay.data();
  ext.request = 2;
  Rig with(&ext), without(nullptr);
  g_seen_count = 0;
  // Frame 1 is published at the first onset: no snapshot exists yet (the stamp is 0): the provider must not be asked.
  with.r->work_ram[0x1234] = 1; without.r->work_ram[0x1234] = 1;
  with.run_frame(); without.run_frame();
  check(ext.result.sequence == 1 && g_seen_count == 0 && ext.result.compat_reason == GENESIS_COMPAT_STALE_STATE && ext.result.compat_accepted == 0, "first frame: no snapshot yet, the provider is not asked");
  check(ext.compat_ram_stamp == with.r->virtual_frame_count, "a snapshot is taken right after each publication, stamped with the frame counter");
  // Frame k's provider sees the work RAM of the PREVIOUS onset (the state the game used to prepare frame k).
  for (unsigned k = 2; k <= 6; ++k) {
    with.r->work_ram[0x1234] = (uint8_t)(10 * k); without.r->work_ram[0x1234] = (uint8_t)(10 * k);
    with.run_frame(); without.run_frame();
    // RAM was set to 10k before this frame's onset; the snapshot taken at the previous onset held 10(k-1) (k == 2: the value 1).
    const uint8_t want = k == 2 ? 1 : (uint8_t)(10 * (k - 1));
    checkf(g_seen_count == k - 1 && g_seen_marker[k - 2] == want && ext.result.compat_accepted != 0, "the provider sees the previous onset's work RAM", g_seen_marker[k - 2], want);
  }
  // a missed snapshot (the extension was detached for a frame) makes the next frame stale instead of using old state
  with.capture->extension = nullptr;
  with.run_frame(); without.run_frame();
  with.capture->extension = &ext;
  const unsigned before = g_seen_count;
  with.run_frame(); without.run_frame();
  check(g_seen_count == before && ext.result.compat_reason == GENESIS_COMPAT_STALE_STATE, "after a frame without a snapshot the state is stale: the provider is not asked");
  with.run_frame(); without.run_frame();
  check(g_seen_count == before + 1 && ext.result.compat_accepted != 0, "the following frame is served again");
  // the provider and the snapshot never change the guest or the native frame
  check(std::memcmp(&with.frame, &without.frame, sizeof(with.frame)) == 0 && std::memcmp(with.frame.frame_digest, without.frame.frame_digest, 32) == 0, "native artifact identical to a run without the provider");
  check(with.r->scheduler.master_ticks == without.r->scheduler.master_ticks && with.r->pc == without.r->pc && with.r->virtual_frame_count == without.r->virtual_frame_count &&
            std::memcmp(&with.r->devices.vdp, &without.r->devices.vdp, sizeof(with.r->devices.vdp)) == 0 && std::memcmp(with.r->work_ram, without.r->work_ram, sizeof(with.r->work_ram)) == 0,
        "guest time, pc, VDP and work RAM identical with and without the provider");
  // not requested: the snapshot is still refreshed (so a later request is fresh), nothing is produced
  ext.request = 0;
  with.run_frame();
  ext.request = 2;
  const unsigned c0 = g_seen_count;
  with.run_frame();
  check(g_seen_count == c0 + 1, "snapshots are kept fresh while the extension is not requested");
  // steady state: no heap traffic
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  { with.run_frame();
    const struct mallinfo2 a = mallinfo2();
    for (int i = 0; i < 6; ++i) with.run_frame();
    const struct mallinfo2 b = mallinfo2();
    check(a.uordblks == b.uordblks && a.arena == b.arena, "no heap allocation with a provider in the steady state"); }
#endif
#endif
}

// ---- the public SDK contract: capability gating, helper table, layout --------------------------------------------------------------
void sdk_contract() {
  Scene sc; build_scene(sc, 4242);
  { GenesisFrameExtension ext{}; attach(ext); g_mock = Mock{}; g_mock.vram = sc.vram.data(); const Out o = produce(sc, ext);
    check(o.rc == 0 && g_mock.prepared == 1 && g_mock.seen_ram == g_ram.data() && g_mock.seen_vram != nullptr && g_mock.seen_lines != nullptr && g_mock.seen_host != nullptr, "every granted capability is delivered"); }
  { GenesisFrameExtension ext{}; attach(ext); ext.compat_caps = GENESIS_CAP_SCENERY_WORDS | GENESIS_CAP_OVERLAY_RGBA; g_mock = Mock{}; g_mock.vram = sc.vram.data(); const Out o = produce(sc, ext);
    check(o.rc == 0 && g_mock.prepared == 1 && g_mock.seen_ram == nullptr && g_mock.seen_vram == nullptr && g_mock.seen_lines == nullptr && g_mock.seen_host != nullptr, "an undeclared capability is never exposed (no RAM, no VDP state)"); }
  { GenesisFrameExtension ext{}; attach(ext, false); ext.compat_caps = GENESIS_CAP_SCENERY_WORDS; g_mock = Mock{}; g_mock.vram = sc.vram.data(); const Out o = produce(sc, ext);
    check(o.rc == 0 && g_mock.prepared == 1, "a provider that does not need work RAM is not made stale by a missing snapshot"); }
  const GenesisCompatHost *h = genesis_native_compat_host();
  check(h != nullptr && h->size == sizeof(GenesisCompatHost) && h->read_hscroll && h->read_vscroll && h->vscroll_is_per_column && h->cram_rgb, "the host helper table is complete");
  int32_t v = 0; uint8_t rgb[3] = {1, 2, 3};
  check(h->read_hscroll(sc.vram.data(), sc.raster->lines[0].registers, 0U, 0U, &v) == 0 && h->read_hscroll(sc.vram.data(), sc.raster->lines[0].registers, 2U, 0U, &v) != 0, "hscroll helper validates the plane");
  check(h->cram_rgb(sc.raster->lines[0].cram, 0U, rgb) == 0 && h->cram_rgb(sc.raster->lines[0].cram, 64U, rgb) != 0, "cram helper validates the index");
  check(GENESIS_COMPAT_OVERLAY_BYTES == 128U * 256U * 4U && sizeof(GenesisCompatLine) == sizeof(GenesisRasterLine), "SDK layout constants");
}

}  // namespace

int main() {
  generic_hook();
  runtime_snapshot();
  sdk_contract();
  if (failures != 0) { std::printf("genesis_native_compat_test: %d failure(s)\n", failures); return 1; }
  std::printf("genesis_native_compat_test: ok\n");
  return 0;
}
