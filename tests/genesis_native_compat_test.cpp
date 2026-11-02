// SEG-124-T001 (ADR 0267): the generic native-compatibility hook of the extension producer, the work-RAM snapshot timing of the runtime, and the Sonic 1 provider.
//
// Project-authored synthetic scenes only (no ROM, no clock). The hook is exercised with a MOCK provider (so the generic code is proven game-independent); the Sonic 1 provider
// is exercised on a synthetic work-RAM image with an independently computed expectation. Real-game evidence lives in the harness backlog record.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "native_compat_registry.h"
#include "runtime.h"
#include "segarecomp/patch.hpp"
#include "sonic1_widescreen.h"
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
struct Mock {
  unsigned accept = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD;
  uint8_t reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
  int word_mode = 0;               // 0: a fixed solid word; 1: refuse every cell (provider fault); 2: the VDP's own cell (identity)
  uint16_t fixed_word = 5;         // solid tile 5, palette 0, low priority
  const uint8_t *vram = nullptr;
  unsigned prepared = 0, cells = 0, huds = 0;
  const uint8_t *seen_ram = nullptr;
  unsigned ovl_w = 12, ovl_h = 9;
  bool bad_overlay = false;
} g_mock;

void mock_prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Mock *m = static_cast<Mock *>(state);
  ++m->prepared;
  m->seen_ram = f->work_ram;
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
GenesisNativeCompat g_provider = {GENESIS_NATIVE_COMPAT_ABI, "genesis.fixture.native-widescreen.v1", 1, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, mock_prepare, mock_word, mock_hud};

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
  ext.compat = &snap; ext.compat_state = &g_mock; ext.compat_components = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD; ext.compat_ram = g_ram.data(); ext.overlay_rgba = g_overlay.data();
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

// ---- the Sonic 1 provider on a synthetic work-RAM image ---------------------------------------------------------------------------
// Layout (synthetic, project-authored; same memory map the provider documents): chunk table at 0, layout at 0xA400, blocks at 0xB000, scroll buffer at 0xCC00.
struct Level {
  std::vector<uint8_t> ram = std::vector<uint8_t>(65536, 0);
  void w16(unsigned a, unsigned v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
  unsigned r16(unsigned a) const { return (ram[a] << 8) | ram[a + 1]; }
};

// The independent expectation of a cell word: re-derived from first principles here.
int expected_word(const Level &L, unsigned plane, int cx, int cy) {
  if (cx < 0 || cy < 0 || cx >= 2048 || cy >= 256) return 0;
  const unsigned x = cx * 8, y = cy * 8;
  const unsigned chunk = L.ram[0xA400 + plane * 0x40 + (y / 256) * 0x80 + x / 256] & 0x7F;
  if (!chunk) return 0;
  const unsigned bw = L.r16((chunk - 1) * 0x200 + ((y % 256) / 16 * 16 + (x % 256) / 16) * 2);
  const unsigned block = bw & 0x3FF;
  unsigned col = (x % 16) / 8, row = (y % 16) / 8;
  const bool fx = bw & 0x800, fy = bw & 0x1000;
  if (fx) col = 1 - col;
  if (fy) row = 1 - row;
  unsigned word = L.r16(0xB000 + block * 8 + (row * 2 + col) * 2);
  if (fx) word ^= 0x800;
  if (fy) word ^= 0x1000;
  return (int)word;
}

void make_level(Level &L, uint32_t seed) {
  Lcg rnd(seed);
  for (unsigned c = 0; c < 0x52; ++c)
    for (unsigned i = 0; i < 256; ++i) L.w16(c * 0x200 + i * 2, (rnd.below(4) << 11) | rnd.below(0x200));   // flips (bits 11/12) and block ids
  for (unsigned b = 0; b < 0x200; ++b)
    for (unsigned i = 0; i < 4; ++i) L.w16(0xB000 + b * 8 + i * 2, (rnd.below(2) << 15) | (rnd.below(4) << 13) | rnd.below(0x400));
  for (unsigned row = 0; row < 4; ++row)
    for (unsigned i = 0; i < 40; ++i) { L.ram[0xA400 + row * 0x80 + i] = (uint8_t)(1 + rnd.below(0x51)); L.ram[0xA400 + row * 0x80 + 0x40 + i] = (uint8_t)(rnd.below(3) == 0 ? 0 : 1 + rnd.below(0x51)); }
  L.ram[0xF600] = 0x0C;       // level running
  L.ram[0xFE12] = 3;          // lives
  L.w16(0xFE20, 37);          // rings
}

// A consistent frame: camera (camx, camy), background camera (bgx, bgy), per-line scroll tables in RAM and VDP agreeing, and the VDP nametables holding exactly the world
// cells the picture shows (what the game would have drawn there); `fill_rest` decides what every other ring cell holds (0 = left as it is).
void set_scroll(Level &L, Scene &sc, int camx, int camy, int bgx, int bgy, int band_step = 3, bool paint = true) {
  L.w16(0xF700, camx); L.w16(0xF704, camy); L.w16(0xF708, bgx); L.w16(0xF70C, bgy); L.w16(0xF710, bgx); L.w16(0xF7F0, 0x70);
  for (unsigned l = 0; l < 224; ++l) {
    const int a = -camx, b = -(bgx + (int)(l / 8) * band_step);
    L.w16(0xCC00 + l * 4, (unsigned)a & 0xFFFF); L.w16(0xCC00 + l * 4 + 2, (unsigned)b & 0xFFFF);
    sc.vram[0xFC00 + l * 4] = (uint8_t)(((unsigned)a & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 1] = (uint8_t)a;
    sc.vram[0xFC00 + l * 4 + 2] = (uint8_t)(((unsigned)b & 0x3FF) >> 8); sc.vram[0xFC00 + l * 4 + 3] = (uint8_t)b;
  }
  if (paint)
    for (unsigned l = 0; l < 224; ++l)
      for (int x = 0; x < 320; ++x) {
        const int offb = bgx + (int)(l / 8) * band_step;
        const int fgw = expected_word(L, 0, (x + camx) / 8, ((int)l + camy) / 8), bgw = x + offb >= 0 ? expected_word(L, 1, (x + offb) / 8, ((int)l + bgy) / 8) : 0;
        const unsigned fa = 0xC000 + (((((int)l + camy) & 255) / 8) * 64 + (((x + camx) & 511) / 8)) * 2, ba = 0xE000 + (((((int)l + bgy) & 255) / 8) * 64 + (((x + offb) & 511) / 8)) * 2;
        sc.vram[fa] = (uint8_t)(fgw >> 8); sc.vram[fa + 1] = (uint8_t)fgw; sc.vram[ba] = (uint8_t)(bgw >> 8); sc.vram[ba + 1] = (uint8_t)bgw;
      }
  std::memcpy(sc.raster->vram_start, sc.vram.data(), GENESIS_VDP_VRAM_BYTES);
  for (auto &line : sc.raster->lines) { line.vsram[0] = (uint8_t)(camy >> 8 & 3); line.vsram[1] = (uint8_t)camy; line.vsram[2] = (uint8_t)(bgy >> 8 & 3); line.vsram[3] = (uint8_t)bgy; }
}

struct Frame {
  GenesisCompatFrame f{};
  GenesisCompatDecision d{};
};
Frame prepare_frame(const Level &L, const Scene &sc, void *&state, const GenesisNativeCompat *&p, unsigned requested = 3, unsigned height = 224) {
  Frame fr;
  p = sonic1_widescreen_provider(&state);
  fr.f.work_ram = L.ram.data(); fr.f.raster = sc.raster.get(); fr.f.vram = sc.raster->vram_start; fr.f.width = 320; fr.f.height = height; fr.f.side = 39; fr.f.requested = requested;
  fr.d.accepted = 0xFF; fr.d.reason = 0xFF;
  p->prepare(state, &fr.f, &fr.d);
  return fr;
}

void sonic1_provider() {
  void *state = nullptr;
  const GenesisNativeCompat *p = nullptr;
  Level L; make_level(L, 21);
  Scene sc; build_scene(sc, 3);
  // plain world lookups against the independent expectation (inside, edges, flips, empty chunks, outside)
  {
    unsigned wrong = 0, total = 0, nonzero = 0;
    for (unsigned plane = 0; plane < 2; ++plane)
      for (int cy = -3; cy < 70; ++cy)
        for (int cx = -5; cx < 330; ++cx) {
          const int want = expected_word(L, plane, cx, cy), got = sonic1_world_cell(L.ram.data(), plane, cx, cy);
          ++total; wrong += want != got; nonzero += want > 0;
        }
    checkf(wrong == 0 && nonzero > 1000, "world cells equal the independent derivation (layout, chunks, blocks, flips)", wrong, nonzero);
    check(sonic1_world_cell(L.ram.data(), 0, -1, 10) == 0 && sonic1_world_cell(L.ram.data(), 0, 2048, 10) == 0 && sonic1_world_cell(L.ram.data(), 0, 10, 256) == 0 && sonic1_world_cell(L.ram.data(), 1, 10, -1) == 0, "outside the level is a blank cell");
    check(sonic1_world_cell(nullptr, 0, 0, 0) == -1 && sonic1_world_cell(L.ram.data(), 2, 0, 0) == -1, "null image / bad plane");
    Level bad = L; bad.ram[0xA400 + 5] = 0x53;
    check(sonic1_world_cell(bad.ram.data(), 0, 5 * 32, 0) == -1, "a chunk index beyond the chunk table is an invalid image");
    bad.ram[0xA400 + 5] = 0xD2;   // loop flag bit does not change the chunk
    L.ram[0xA400 + 5] = 0x12; const int a = sonic1_world_cell(L.ram.data(), 0, 5 * 32, 0); L.ram[0xA400 + 5] = 0x92; const int b = sonic1_world_cell(L.ram.data(), 0, 5 * 32, 0); L.ram[0xA400 + 5] = 0x12;
    check(a == b, "the high bit of a layout byte is not part of the chunk index");
  }
  // an accepted frame
  set_scroll(L, sc, 1234, 100, 400, 14);
  Frame fr = prepare_frame(L, sc, state, p);
  check(fr.d.accepted == 3 && fr.d.reason == 0, "a consistent level frame is accepted (scenery and HUD)");
  // scenery words for the off-screen columns equal the world cells at the world coordinates the game's scroll defines
  {
    unsigned wrong = 0, total = 0;
    for (unsigned plane = 0; plane < 2; ++plane)
      for (unsigned row = 0; row < 224; row += 3)
        for (int x = -39; x < 359; ++x) {
          if (x >= 0 && x < 320) continue;
          const int hs = plane == 0 ? -1234 : -(400 + (int)(row / 8) * 3);
          uint16_t word = 0xFFFF;
          const int ok = p->scenery_word(state, plane, row, x, hs, plane == 0 ? 100 : 14, &word);
          const int off = -hs, voff = plane == 0 ? 100 : 14;
          const int wx = x + off, wy = (int)row + voff;
          const int want = expected_word(L, plane, (wx >= 0 ? wx / 8 : -((-wx + 7) / 8)), (wy / 8));
          ++total; wrong += !ok || (int)word != want;
        }
    checkf(wrong == 0 && total > 5000, "scenery words equal the independent derivation for off-screen columns", wrong, total);
  }
  // THE reconstruction test: the provider's scenery words do not depend on the (wrapped) VDP nametable. The VDP ring buffer is made to hold the exact world content only for the cells in
  // VIEW (what the frame-identity guard looks at) and, everywhere else (all the off-screen columns the extension would read), garbage in one scene, blank in another and the true content in a third:
  // the off-screen words are identical in all three; they are derived from the level data, never from the wrapped map. Wholesale disagreement INSIDE the view is a different scene and is declined.
  {
    auto collect = [&](Scene &scene, unsigned *accepted = nullptr, unsigned *reason = nullptr, unsigned requested = 3) {
      void *st = nullptr; const GenesisNativeCompat *pp = nullptr;
      Frame f = prepare_frame(L, scene, st, pp, requested);
      if (accepted) *accepted = f.d.accepted;
      if (reason) *reason = f.d.reason;
      std::vector<uint32_t> words;
      if ((f.d.accepted & 2) == 0) return words;
      for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned row = 0; row < 224; row += 2)
          for (int x = -39; x < 359; ++x) {
            if (x >= 0 && x < 320) continue;
            const int hs = plane == 0 ? -1234 : -(400 + (int)(row / 8) * 3);
            uint16_t w = 0xFFFF;
            words.push_back(pp->scenery_word(st, plane, row, x, hs, plane == 0 ? 100 : 14, &w) ? w : 0x10000u);
          }
      return words;
    };
    auto in_view = [&](Scene &scene, int fill_mode) {   // fill_mode 0: garbage elsewhere (kept), 1: blank elsewhere
      std::vector<uint8_t> keep = scene.vram;
      for (uint32_t base : {0xC000u, 0xE000u}) { if (fill_mode == 1) std::memset(&scene.vram[base], 0, 0x2000); }
      set_scroll(L, scene, 1234, 100, 400, 14);    // paints the in-view cells exactly
      std::memcpy(scene.raster->vram_start, scene.vram.data(), GENESIS_VDP_VRAM_BYTES);
    };
    Scene garbage; build_scene(garbage, 999);
    Scene blank; build_scene(blank, 1);
    Scene exact; build_scene(exact, 2);
    in_view(garbage, 0); in_view(blank, 1); in_view(exact, 0);
    for (unsigned plane = 0; plane < 2; ++plane)   // the ring buffer filled with the TRUE world content around the camera (the off-screen cells too)
      for (int cy = 0; cy < 32; ++cy)
        for (int cx = 0; cx < 64; ++cx) {
          const int wcx = (plane == 0 ? 1234 : 400) / 8 - 20 + cx, wcy = ((plane == 0 ? 100 : 14) / 8 + cy);
          const uint32_t a = (plane == 0 ? 0xC000u : 0xE000u) + (((wcy & 31) * 64) + (wcx & 63)) * 2;
          const int want = expected_word(L, plane, wcx, wcy);
          const int vx = wcx * 8 - (plane == 0 ? 1234 : 400);   // only cells that are NOT in view: the in-view paint stays authoritative
          if (vx < -16 || vx >= 336) { exact.vram[a] = (uint8_t)(want >> 8); exact.vram[a + 1] = (uint8_t)want; }
        }
    std::memcpy(exact.raster->vram_start, exact.vram.data(), GENESIS_VDP_VRAM_BYTES);
    unsigned acc_a = 0, acc_b = 0, acc_c = 0, rs = 0;
    const std::vector<uint32_t> a = collect(garbage, &acc_a), b = collect(blank, &acc_b), c = collect(exact, &acc_c);
    checkf(acc_a == 3 && acc_b == 3 && acc_c == 3, "the frame is accepted over garbage, blank and exact off-screen nametable content", acc_a, acc_b);
    check(a.size() > 1000 && a == b && a == c, "off-screen scenery words are identical over garbage, blank and exact nametable content: not a redisplay of the VDP's wrapped map");
    unsigned unresolved = 0; for (uint32_t w : a) unresolved += w > 0xFFFF;
    check(unresolved == 0, "every off-screen cell is answered");
    // wholesale disagreement in view (another scene's nametables): declined with the identity reason, nothing is served
    Scene other; build_scene(other, 4242);
    set_scroll(L, other, 1234, 100, 400, 14, 3, false);
    const std::vector<uint32_t> d = collect(other, &acc_a, &rs, GENESIS_COMPAT_SCENERY);
    checkf(d.empty() && acc_a == 0 && rs == GENESIS_COMPAT_MISMATCH, "a picture that is not this world state is declined as MISMATCH", acc_a, rs);
    // a few differing in-view cells (a one-frame streaming lag) stay served; many do not
    { Scene lag; build_scene(lag, 5); set_scroll(L, lag, 1234, 100, 400, 14);
      for (int i = 0; i < 12; ++i) lag.vram[0xC000 + (size_t)(i * 7 % 64) * 2 + (size_t)(i / 8) * 128 + 1] ^= 0x13;
      std::memcpy(lag.raster->vram_start, lag.vram.data(), GENESIS_VDP_VRAM_BYTES);
      collect(lag, &acc_a); check(acc_a == 3, "a handful of lagging in-view cells does not decline the frame");
      for (uint32_t i = 0; i < 0x1000; i += 2) lag.vram[0xC000 + i + 1] ^= 0x55;
      std::memcpy(lag.raster->vram_start, lag.vram.data(), GENESIS_VDP_VRAM_BYTES);
      collect(lag, &acc_a, &rs, GENESIS_COMPAT_SCENERY); checkf(acc_a == 0 && rs == GENESIS_COMPAT_MISMATCH, "most in-view foreground cells wrong: declined", acc_a, rs); }
    // the provider never changes the VRAM it is given
    { Scene keep; build_scene(keep, 77); set_scroll(L, keep, 1234, 100, 400, 14); const std::vector<uint8_t> before = keep.vram; collect(keep);
      check(keep.vram == before, "the provider left the VRAM it was given untouched"); }
    set_scroll(L, sc, 1234, 100, 400, 14);   // restore the main scene
    fr = prepare_frame(L, sc, state, p);
  }
  // HUD: the game's own HUD pieces, rasterized from the frame's VRAM tiles and CRAM (independent reference below), no panel, rows 1:1, flash by the object's own rule
  {
    struct Piece { unsigned x, y, cw, ch, tile, group; };
    static const Piece pieces[] = {{16, 8, 4, 2, 0x6CA, 0}, {48, 8, 4, 2, 0x6E2, 0}, {80, 8, 4, 2, 0x6EA, 0}, {16, 24, 4, 2, 0x6DA, 1}, {56, 24, 4, 2, 0x6F2, 0},
                                   {16, 40, 4, 2, 0x6D2, 2}, {48, 40, 1, 2, 0x6CA, 2}, {64, 40, 3, 2, 0x6FA, 0}, {16, 200, 2, 2, 0x7D4, 0}, {32, 200, 4, 2, 0x7D8, 0}};
    Scene hs; build_scene(hs, 31);
    Lcg rnd(99);
    for (uint32_t i = 0x6CAu * 32; i < 0x7E0u * 32; ++i) hs.vram[i] = (uint8_t)rnd.next();   // the HUD's art as the game left it (random nibbles; index 0 = transparent)
    for (unsigned l = 0; l < 224; ++l) for (unsigned i = 0; i < 128; ++i) hs.raster->lines[l].cram[i] = (uint8_t)((i * 37 + l * 11) & 0xEE);   // per-line palettes
    set_scroll(L, hs, 1234, 100, 400, 14);
    std::memcpy(hs.raster->vram_start, hs.vram.data(), GENESIS_VDP_VRAM_BYTES);
    auto render = [&](Level &lv, std::vector<uint8_t> &out, unsigned *w = nullptr, unsigned *h = nullptr, unsigned *fx = nullptr, unsigned *fy = nullptr) {
      void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(lv, hs, st, pp);
      out.assign(GENESIS_COMPAT_OVERLAY_BYTES, 0xCD);
      GenesisCompatOverlay o{}; o.rgba = out.data(); o.capacity = out.size();
      const int ok = (f.d.accepted & GENESIS_COMPAT_HUD) != 0 && pp->hud(st, &o);
      if (ok) { if (w) *w = o.width; if (h) *h = o.height; if (fx) *fx = o.fallback_x; if (fy) *fy = o.fallback_y; }
      return ok != 0;
    };
    // the independent reference: expected RGBA of the overlay for a given palette-line choice
    auto reference = [&](unsigned red_mask) {
      std::vector<uint8_t> ref((size_t)96 * 224 * 4, 0);
      for (const Piece &pc : pieces)
        for (unsigned cx = 0; cx < pc.cw; ++cx) for (unsigned cy = 0; cy < pc.ch; ++cy) for (unsigned row = 0; row < 8; ++row) for (unsigned col = 0; col < 8; ++col) {
          const unsigned tile = pc.tile + cx * pc.ch + cy, y = pc.y + cy * 8 + row, x = pc.x + cx * 8 + col;
          const uint8_t byte = hs.vram[tile * 32 + row * 4 + col / 2];
          const unsigned idx = (col & 1) ? (byte & 15u) : (byte >> 4);
          const unsigned pal = (pc.group != 0 && (red_mask & (1u << pc.group))) ? 1 : 0;
          if (idx == 0) continue;
          const uint8_t *cram = hs.raster->lines[y].cram;
          const unsigned e = (cram[(pal * 16 + idx) * 2] << 8) | cram[(pal * 16 + idx) * 2 + 1];
          uint8_t *d = &ref[((size_t)y * 96 + (x - 16)) * 4];
          d[0] = (uint8_t)(((e >> 1) & 7) * 36); d[1] = (uint8_t)(((e >> 5) & 7) * 36); d[2] = (uint8_t)(((e >> 9) & 7) * 36); d[3] = 255;
        }
      return ref;
    };
    std::vector<uint8_t> img; unsigned w = 0, h = 0, fx = 0, fy = 0;
    Level base = L; base.w16(0xFE20, 25); base.ram[0xFE05] = 0;     // rings: no flash
    check(render(base, img, &w, &h, &fx, &fy) && w == 96 && h == 224 && fx == 16 && fy == 0, "the HUD overlay covers picture rows 1:1 and the columns the original used, with the original's place as its fallback");
    std::vector<uint8_t> ref0 = reference(0);
    unsigned wrong = 0, opaque = 0, transparent_wrong = 0;
    for (size_t i = 0; i < ref0.size(); i += 4) {
      const bool ro = ref0[i + 3] != 0, po = img[i + 3] != 0;
      if (ro) { ++opaque; for (int k = 0; k < 3; ++k) wrong += (std::abs((int)ref0[i + k] - (int)img[i + k]) > 36); wrong += !po; }
      else transparent_wrong += po;
    }
    checkf(opaque > 500 && wrong == 0 && transparent_wrong == 0, "the overlay equals the independent rasterization of the game's HUD pieces (tiles, palette lines, rows), with nothing drawn elsewhere (no panel)", wrong, transparent_wrong);
    for (size_t i = 3; i < ref0.size(); i += 4) if (img[i] != 0 && img[i] != 255) { check(false, "overlay alpha is 0 or 255 only"); break; }
    auto same = [&](const std::vector<uint8_t> &x, const std::vector<uint8_t> &y) {   // the 96x224 overlay, colours within the decoder's rounding
      for (size_t i = 0; i < ref0.size(); i += 4) {
        if (x[i + 3] != y[i + 3]) return false;
        if (x[i + 3] != 0) for (int k = 0; k < 3; ++k) if (std::abs((int)x[i + k] - (int)y[i + k]) > 36) return false;
      }
      return true;
    };
    // the digits are the game's tiles: changing the counters in RAM changes nothing, changing the tile art changes the overlay
    { Level v = base; v.w16(0xFE20, 77); v.ram[0xFE12] = 9; v.ram[0xFE26 + 3] = 5; std::vector<uint8_t> other; check(render(v, other) && same(other, img), "the digits come from the frame's own tiles: RAM counters do not change the picture"); }
    { hs.vram[0x6FAu * 32 + 5] ^= 0x1F; std::memcpy(hs.raster->vram_start, hs.vram.data(), GENESIS_VDP_VRAM_BYTES); std::vector<uint8_t> other; check(render(base, other) && !same(other, img), "a changed digit tile changes the overlay"); hs.vram[0x6FAu * 32 + 5] ^= 0x1F; std::memcpy(hs.raster->vram_start, hs.vram.data(), GENESIS_VDP_VRAM_BYTES); }
    // flash: with no rings the RINGS label uses palette line 1 in the flash phase's low state, TIME too at nine minutes; never with rings
    { Level a0 = L; a0.w16(0xFE20, 0); a0.ram[0xFE05] = 0; std::vector<uint8_t> r1, r2, r3, r4;
      const std::vector<uint8_t> red_rings = reference(1u << 2), red_both = reference((1u << 2) | (1u << 1));
      check(render(a0, r1) && same(r1, red_rings) && !same(r1, ref0), "no rings, flash phase low: the RINGS label is on the red palette line");
      Level a1 = a0; a1.ram[0xFE05] = 8; check(render(a1, r2) && same(r2, ref0), "no rings, flash phase high: normal colours");
      Level a2 = a0; a2.ram[0xFE23] = 9; check(render(a2, r3) && same(r3, red_both) && !same(r3, red_rings), "no rings at nine minutes: TIME and RINGS labels red");
      Level a3 = base; a3.ram[0xFE23] = 9; check(render(a3, r4) && same(r4, ref0), "with rings nothing flashes (even at nine minutes)"); }
    // the HUD is offered in a running level only and only with plausible counters; unsupported zones keep it
    { Level z = base; z.ram[0xFE10] = 6; check(render(z, img), "another zone: the HUD is still drawn (no scenery)"); }
    { Level z = base; z.ram[0xF600] = 0x8C; check(!render(z, img), "the level start sequence: no HUD"); }
    // too small a buffer: nothing drawn; an unrequested HUD is never drawn
    { void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(base, hs, st, pp); GenesisCompatOverlay tiny{}; uint8_t small[16]; tiny.rgba = small; tiny.capacity = sizeof small; check(f.d.accepted && pp->hud(st, &tiny) == 0, "an undersized overlay buffer draws nothing");
      Frame fh = prepare_frame(base, hs, st, pp, GENESIS_COMPAT_SCENERY); GenesisCompatOverlay o6{}; o6.rgba = g_overlay.data(); o6.capacity = g_overlay.size(); check(fh.d.accepted == GENESIS_COMPAT_SCENERY && pp->hud(st, &o6) == 0, "an unrequested HUD is never drawn"); }
    fr = prepare_frame(L, sc, state, p);
  }
  // declines: every unverified situation falls back, with the right reason and no half-state
  auto declined = [&](const char *what, Level &lv, Scene &s, unsigned reason, unsigned height = 224) {
    void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(lv, s, st, pp, GENESIS_COMPAT_SCENERY, height);
    checkf(f.d.accepted == 0 && f.d.reason == reason, what, f.d.accepted, f.d.reason);
    uint16_t word = 0xFFFF;
    check(pp->scenery_word(st, 0, 0, -5, 0, 0, &word) == 0, "a declined frame answers no cell");
  };
  { Level lv = L; lv.ram[0xF600] = 0x8C; declined("level start sequence is declined", lv, sc, GENESIS_COMPAT_UNSUPPORTED_SCENE);
    void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(lv, sc, st, pp); check(f.d.accepted == 0, "the level start sequence gets neither scenery nor HUD"); }
  { // the HUD needs only plausible counters in any running level; the scenery needs the verified zone
    Level lv = L; lv.ram[0xFE10] = 6; void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(lv, sc, st, pp);
    check(f.d.accepted == GENESIS_COMPAT_HUD && f.d.reason == 0, "another zone: HUD only, no scenery");
    lv = L; lv.ram[0xFE24] = 60; f = prepare_frame(lv, sc, st, pp); check(f.d.accepted == GENESIS_COMPAT_SCENERY, "an impossible clock: no HUD, scenery unaffected");
    lv = L; lv.ram[0xFE23] = 10; f = prepare_frame(lv, sc, st, pp); check(f.d.accepted == GENESIS_COMPAT_SCENERY, "an impossible minute count: no HUD");
    lv = L; lv.ram[0xFE26] = 0xFF; f = prepare_frame(lv, sc, st, pp); check(f.d.accepted == GENESIS_COMPAT_SCENERY, "an impossible score: no HUD"); }
  { Level lv = L; lv.ram[0xF600] = 0x04; declined("title screen is declined", lv, sc, GENESIS_COMPAT_UNSUPPORTED_SCENE); }
  { Level lv = L; lv.ram[0xFE10] = 7; declined("another zone is declined", lv, sc, GENESIS_COMPAT_UNSUPPORTED_SCENE); }
  { Level lv = L; lv.ram[0xFE11] = 4; declined("another act is declined", lv, sc, GENESIS_COMPAT_UNSUPPORTED_SCENE); }
  { Level lv = L; lv.w16(0xF700, 0x4001); declined("a camera outside the level is declined", lv, sc, GENESIS_COMPAT_BAD_STATE); }
  { Level lv = L; lv.w16(0xF704, 0x800); declined("a camera row outside the layout is declined", lv, sc, GENESIS_COMPAT_BAD_STATE); }
  { Level lv = L; lv.ram[0xA400 + 7] = 0x60; declined("a layout byte past the chunk table is declined", lv, sc, GENESIS_COMPAT_BAD_STATE); }
  { Level lv = L; lv.ram[0xA400 + 0x40 + 9] = 0x7F; declined("a background layout byte past the chunk table is declined", lv, sc, GENESIS_COMPAT_BAD_STATE); }
  { Level lv = L; lv.w16(0xCC00 + 10 * 4, 0xFFFF); declined("RAM scroll disagreeing with the VDP scroll (foreground) is declined", lv, sc, GENESIS_COMPAT_BAD_SCROLL); }
  { Level lv = L; lv.w16(0xCC00 + 100 * 4 + 2, 0x0123); declined("RAM scroll disagreeing with the VDP scroll (background) is declined", lv, sc, GENESIS_COMPAT_BAD_SCROLL); }
  { Scene s2; build_scene(s2, 3); Level lv = L; set_scroll(lv, s2, 1234, 100, 400, 14); s2.raster->lines[50].vsram[1] ^= 0x01; declined("VDP vertical scroll disagreeing with the camera is declined", lv, s2, GENESIS_COMPAT_BAD_SCROLL); }
  { Scene s2; build_scene(s2, 3); Level lv = L; set_scroll(lv, s2, 1234, 100, 400, 14); s2.raster->lines[10].registers[16] = 0x11; declined("another plane size is declined", lv, s2, GENESIS_COMPAT_BAD_SCROLL); }
  { Scene s2; build_scene(s2, 3); Level lv = L; set_scroll(lv, s2, 1234, 100, 400, 14); for (auto &ln : s2.raster->lines) ln.registers[11] = 7; declined("per-column vertical scroll is declined", lv, s2, GENESIS_COMPAT_BAD_SCROLL); }
  { Scene s2; build_scene(s2, 3); Level lv = L; set_scroll(lv, s2, 1234, 100, 400, 14); for (auto &ln : s2.raster->lines) ln.registers[11] = 1; declined("a prohibited scroll mode is declined", lv, s2, GENESIS_COMPAT_BAD_SCROLL); }
  { declined("an interlaced-height frame is declined", L, sc, GENESIS_COMPAT_BAD_STATE, 448); }
  { void *st; const GenesisNativeCompat *pp = sonic1_widescreen_provider(&st); GenesisCompatFrame f{}; GenesisCompatDecision d{0xFF, 0xFF}; f.requested = 3; f.height = 224; pp->prepare(st, &f, &d);
    check(d.accepted == 0 && d.reason == GENESIS_COMPAT_BAD_STATE, "no RAM / raster / VRAM pointer: declined"); }
  // the validated scenery scope, zone by zone and act by act (independent table; a widening of the provider's table must be mirrored here, with its validation evidence recorded in the backlog)
  {
    static const unsigned expected[6] = {0x07, 0x0F, 0x07, 0x07, 0x00, 0x00};   // Green Hill acts 1-3
    unsigned served = 0, total = 0;
    for (unsigned zone = 0; zone < 8; ++zone)
      for (unsigned act = 0; act < 6; ++act) {
        Level lv = L; lv.ram[0xFE10] = (uint8_t)zone; lv.ram[0xFE11] = (uint8_t)act;
        void *st; const GenesisNativeCompat *pp; Frame f = prepare_frame(lv, sc, st, pp);
        const bool want = zone < 6 && ((expected[zone] >> act) & 1u) != 0;
        ++total; served += (f.d.accepted & GENESIS_COMPAT_SCENERY) != 0;
        checkf(((f.d.accepted & GENESIS_COMPAT_SCENERY) != 0) == want && (f.d.accepted & GENESIS_COMPAT_HUD) != 0 && sonic1_scenery_scope(zone, act) == (want ? 1 : 0), "scenery scope per zone/act", zone, act);
      }
    checkf(served > 0 && total == 48, "the scope table is exercised", served, total);
  }
  // the audited identity
  check(std::strcmp(p->capability, "genesis.sonic1.native-widescreen.v1") == 0 && p->implementation_version == SONIC1_NATIVE_IMPLEMENTATION && p->abi_version == GENESIS_NATIVE_COMPAT_ABI &&
            p->components == (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY),
        "the provider identity");
}

// ---- the registry agrees with the audited capability table of the package importer -----------------------------------------------
void registry_agreement() {
  namespace patch = segarecomp::patch;
  unsigned buildable = 0;
  for (std::size_t i = 0; i < sizeof(patch::kNativeCapabilities) / sizeof(patch::kNativeCapabilities[0]); ++i) {
    const patch::NativeCapability &c = patch::kNativeCapabilities[i];
    void *state = nullptr;
    const GenesisNativeCompat *p = genesis_native_compat_select((unsigned)i, c.implementation_version, &state);
    if (c.test_fixture) { check(p == nullptr, "a fixture capability has no provider in the registry"); continue; }
    ++buildable;
    check(p != nullptr && state != nullptr && std::strcmp(p->capability, c.id) == 0 && p->implementation_version == c.implementation_version && p->components == c.components,
          "registry index, capability id, implementation version and components agree with the package capability table");
    check(genesis_native_compat_select((unsigned)i, c.implementation_version + 1, &state) == nullptr, "another implementation version is not served");
  }
  check(buildable == 1, "exactly the audited Sonic 1 capability is buildable");
  void *state = nullptr;
  check(genesis_native_compat_select(2, 1, &state) == nullptr && genesis_native_compat_select(0xFFFFFFFFu, 1, &state) == nullptr, "unknown provider indexes are refused");
  check(patch::kNativeComponentHud == GENESIS_COMPAT_HUD && patch::kNativeComponentScenery == GENESIS_COMPAT_SCENERY, "component bits match the package importer");
}

}  // namespace

int main() {
  generic_hook();
  runtime_snapshot();
  sonic1_provider();
  registry_agreement();
  if (failures != 0) { std::printf("genesis_native_compat_test: %d failure(s)\n", failures); return 1; }
  std::printf("genesis_native_compat_test: ok\n");
  return 0;
}
