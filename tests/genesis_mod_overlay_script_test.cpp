// SEG-152-T005 / SEG-152-T006 (ADR 0320 decisions 6 and 7): the viewer's scheduled-mod adapter (platforms/genesis/viewer/mod_capture.cpp, compiled here with -DSEGARECOMP_MOD_NATIVE -DSEGARECOMP_MOD_VIEWER exactly as a
// built program is) is driven by an exact-frame provider that replays a scripted sequence of render-API calls (fixtures/mod_sdk/overlay_script_provider.c). The test calls the compatibility table the way the producer does
// (`prepare`, then `hud` into host-owned buffers) and compares the transport (GenesisCompatOverlay) with an independent oracle: a hand-built ABI 1 provider's own writes and a reference rasterizer. No ROM, no SDL.
//   genesis_mod_overlay_script_tests bitmap    -> bounded RGBA bitmaps (T005), registered as genesis_mod_bitmap_tests
//   genesis_mod_overlay_script_tests refusal   -> non-fault refusal vs fault (T006), registered as genesis_mod_refusal_tests
#include "mod_scheduler.h"
#include "native_compat.h"
#include "runtime.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
int segarecomp_capture_mod_begin(GenesisRuntime *runtime);
void segarecomp_capture_mod_end(GenesisRuntime *runtime);
const GenesisNativeCompat *segarecomp_patch_provider(void **state_out);
enum { OP_RECT = 1, OP_TEXT, OP_BITMAP, OP_TARGET, OP_LAYER, OP_LAYER_BITMAP, OP_CLIP, OP_FALLBACK, OP_DECLINE, OP_SCENE, OP_WINDOW, OP_SPRITES, OP_LAYER_LEGACY_RAW, OP_MASK, OP_ORIGIN };
typedef struct ScriptOp {
  uint32_t kind;
  int32_t a[8];
  const uint8_t *px;
  uint32_t len;
} ScriptOp;
typedef struct OverlayScript {
  unsigned count, calls, ran;
  uint64_t last_frame;
  uint32_t has_render_api, has_frame;
  uint32_t probe_phase, phase_seen;
  int phase_status;
  int statuses[96];
  ScriptOp ops[96];
} OverlayScript;
extern OverlayScript g_script;
}

namespace {
int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) { std::printf("FAIL: %s\n", message.c_str()); ++failures; }
}

constexpr unsigned kAll = GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES;

GenesisControlTransfer synthetic_dispatch(GenesisRuntime *rt) {
  rt->work_ram[0x10] = static_cast<uint8_t>(rt->work_ram[0x10] + 1U);
  return genesis_runtime_retire_m68k_instruction(rt, 4U, rt->pc);
}
void run_frames(GenesisRuntime &r, uint64_t frames) {
  for (uint32_t slices = 0; r.virtual_frame_count < frames; ++slices) {
    if (slices > 40U * static_cast<uint32_t>(frames) + 64U) { check(false, "the frame counter does not advance"); return; }
    const GenesisControlTransfer t = genesis_runtime_run(&r, synthetic_dispatch, 997U);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) { check(false, "the synthetic program stopped"); return; }
  }
}

// ---- the script ----
struct Script {
  Script() { const unsigned calls = g_script.calls; std::memset(&g_script, 0, sizeof g_script); g_script.calls = calls; }   // the call counter belongs to the harness (one per attached adapter)
  ScriptOp &add(uint32_t kind) {
    ScriptOp &o = g_script.ops[g_script.count++];
    std::memset(&o, 0, sizeof o);
    o.kind = kind;
    return o;
  }
  void rect(int x, int y, int w, int h, uint32_t rgba) { ScriptOp &o = add(OP_RECT); o.a[0] = x; o.a[1] = y; o.a[2] = w; o.a[3] = h; o.a[4] = static_cast<int32_t>(rgba); }
  void text(int x, int y, const char *s, uint32_t rgba) { ScriptOp &o = add(OP_TEXT); o.a[0] = x; o.a[1] = y; o.a[2] = static_cast<int32_t>(rgba); o.px = reinterpret_cast<const uint8_t *>(s); o.len = static_cast<uint32_t>(std::strlen(s)); }
  void bitmap(int x, int y, int w, int h, const uint8_t *px, int stride = -1) { ScriptOp &o = add(OP_BITMAP); o.a[0] = x; o.a[1] = y; o.a[2] = w; o.a[3] = h; o.a[4] = stride < 0 ? w * 4 : stride; o.px = px; }
  void target(int i) { add(OP_TARGET).a[0] = i; }
  void layer(int i, int anchor, int ref, int dx, int dy, int w, int h, int z) { ScriptOp &o = add(OP_LAYER); o.a[0] = i; o.a[1] = anchor; o.a[2] = ref; o.a[3] = dx; o.a[4] = dy; o.a[5] = w; o.a[6] = h; o.a[7] = z; }
  void layer_bitmap(int i, int anchor, int ref, int dx, int dy, int w, int h, int z) { layer(i, anchor, ref, dx, dy, w, h, z); g_script.ops[g_script.count - 1U].kind = OP_LAYER_BITMAP; }
  void clip(int i, int x, int y, int w, int h) { ScriptOp &o = add(OP_CLIP); o.a[0] = i; o.a[1] = x; o.a[2] = y; o.a[3] = w; o.a[4] = h; }
  void fallback(int x, int y) { ScriptOp &o = add(OP_FALLBACK); o.a[0] = x; o.a[1] = y; }
  void decline(unsigned mask, unsigned reason) { ScriptOp &o = add(OP_DECLINE); o.a[0] = static_cast<int32_t>(mask); o.a[1] = static_cast<int32_t>(reason); }
  void scene(const char *s) { ScriptOp &o = add(OP_SCENE); o.px = reinterpret_cast<const uint8_t *>(s); o.len = static_cast<uint32_t>(std::strlen(s)); }
  void window(unsigned plane, int cx, int cy, unsigned word) { ScriptOp &o = add(OP_WINDOW); o.a[0] = static_cast<int32_t>(plane); o.a[1] = cx; o.a[2] = cy; o.a[3] = static_cast<int32_t>(word); }
  void sprites() { add(OP_SPRITES); }
  void mask(unsigned plane, unsigned rows, unsigned mask_bytes, const uint8_t *bits) { ScriptOp &o = add(OP_MASK); o.a[0] = static_cast<int32_t>(plane); o.a[1] = static_cast<int32_t>(rows); o.a[2] = static_cast<int32_t>(mask_bytes); o.px = bits; }
  void origin(unsigned plane, unsigned rows, unsigned bytes, const int16_t *delta) { ScriptOp &o = add(OP_ORIGIN); o.a[0] = static_cast<int32_t>(plane); o.a[1] = static_cast<int32_t>(rows); o.a[2] = static_cast<int32_t>(bytes); o.px = reinterpret_cast<const uint8_t *>(delta); }   // SEG-152-T017
};

// ---- an attached adapter over a synthetic runtime (fresh per scenario: a fault disables the mod for good) ----
struct Frame {
  std::array<uint8_t, 65536> ram{}, vram{};
  std::array<GenesisCompatLine, 224> lines{};
  GenesisCompatFrame cf{};
  void make(uint8_t tag, unsigned side, unsigned requested) {
    ram.fill(tag);
    vram.fill(0);
    vram[0] = tag;
    std::memset(lines.data(), 0, sizeof(lines));
    cf = GenesisCompatFrame{};
    cf.host = genesis_native_compat_host();
    cf.work_ram = ram.data();
    cf.vram = vram.data();
    cf.lines = lines.data();
    cf.line_count = 224U;
    cf.width = 320U; cf.height = 224U; cf.side = side; cf.requested = requested;
  }
};

struct Transport {
  GenesisCompatDecision dec{};
  bool drawn = false;
  std::vector<uint8_t> fixed = std::vector<uint8_t>(GENESIS_COMPAT_OVERLAY_BYTES), layers = std::vector<uint8_t>(GENESIS_COMPAT_LAYER_BYTES);
  GenesisCompatOverlay ov{};
  uint32_t sprite_count = 0;
  int word_status = -1, outside_status = -1;
  uint16_t word = 0;
};

struct Harness {
  std::unique_ptr<GenesisRuntime> rt{new GenesisRuntime};
  std::unique_ptr<GenesisRasterCapture> capture{new GenesisRasterCapture};
  std::unique_ptr<GenesisFrameExtension> ext{new GenesisFrameExtension};
  std::unique_ptr<Frame> frame{new Frame};
  std::unique_ptr<GenesisVirtualSpriteSet> sprites{new GenesisVirtualSpriteSet};
  const GenesisNativeCompat *table = nullptr;
  void *state = nullptr;
  Harness() {
    std::memset(static_cast<void *>(rt.get()), 0, sizeof(*rt));
    rt->pc = 0x200U; rt->sr = 0x2700U; rt->devices.vdp.registers[1] = 0x44; rt->devices.vdp.registers[12] = 0x81;
    std::memset(static_cast<void *>(capture.get()), 0, sizeof(*capture));
    std::memset(static_cast<void *>(ext.get()), 0, sizeof(*ext));
    ext->extension_shown = 1U;
    capture->extension = ext.get();
    rt->live_raster = capture.get();
    g_script.calls = 0U; g_script.count = 0U;   // nothing of an earlier scenario runs during the warm-up frames
    check(segarecomp_capture_mod_begin(rt.get()) == 0, "the script provider attaches through the viewer adapter");
    table = segarecomp_patch_provider(&state);
    run_frames(*rt, 3U);
  }
  ~Harness() { segarecomp_capture_mod_end(rt.get()); }
  // Calls the table exactly like vdp_render.c does for one produced frame.
  Transport produce(uint8_t tag = 0x11U, unsigned requested = GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY) {
    Transport t;
    frame->make(tag, 96U, requested);
    table->prepare(state, &frame->cf, &t.dec);
    if ((t.dec.accepted & GENESIS_COMPAT_HUD) != 0U) {
      std::memset(sprites.get(), 0, sizeof *sprites);
      t.ov.rgba = t.fixed.data(); t.ov.capacity = t.fixed.size(); t.ov.layer_rgba = t.layers.data(); t.ov.layer_capacity = t.layers.size(); t.ov.vsprites = sprites.get();
      t.drawn = table->hud(state, &t.ov) != 0;
      t.sprite_count = sprites->count;
    }
    if ((t.dec.accepted & GENESIS_COMPAT_SCENERY) != 0U) {
      t.word_status = table->scenery_word(state, 0U, 3, -4, 0, 0, &t.word);   // plane cell (-1, 0)
      uint16_t other = 0;
      t.outside_status = table->scenery_word(state, 0U, 3, -20, 0, 0, &other);
    }
    return t;
  }
};

// ---- images and the reference rasterizer (an oracle independent of the host implementation) ----
std::vector<uint8_t> pattern(int w, int h, unsigned seed) {
  static const uint8_t alphas[6] = {0, 1, 127, 128, 254, 255};
  std::vector<uint8_t> img(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);
  for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
    img[i * 4U] = static_cast<uint8_t>(i * 7U + seed + 1U);
    img[i * 4U + 1U] = static_cast<uint8_t>(i * 13U + seed * 3U + 2U);
    img[i * 4U + 2U] = static_cast<uint8_t>(i * 29U + 5U);
    img[i * 4U + 3U] = alphas[(i + seed) % 6U];
  }
  return img;
}
struct Canvas {
  int w, h;
  std::vector<uint8_t> px;
  Canvas(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, 0) {}
  uint8_t *at(int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? nullptr : &px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4U]; }
  void replace(int x, int y, int bw, int bh, const std::vector<uint8_t> &img) {
    for (int r = 0; r < bh; ++r)
      for (int c = 0; c < bw; ++c)
        if (uint8_t *d = at(x + c, y + r)) std::memcpy(d, &img[(static_cast<size_t>(r) * static_cast<size_t>(bw) + static_cast<size_t>(c)) * 4U], 4U);
  }
  void blend_px(uint8_t *dst, uint32_t rgba) {   // straight-alpha source-over in integers, as the host's rect/text rule
    const uint64_t sr = (rgba >> 24U) & 255U, sg = (rgba >> 16U) & 255U, sb = (rgba >> 8U) & 255U, sa = rgba & 255U;
    if (sa == 0) return;
    if (sa == 255) { dst[0] = static_cast<uint8_t>(sr); dst[1] = static_cast<uint8_t>(sg); dst[2] = static_cast<uint8_t>(sb); dst[3] = 255; return; }
    const uint64_t da = dst[3], scaled = sa * 255U + da * (255U - sa);
    dst[0] = static_cast<uint8_t>((sr * sa * 255U + dst[0] * da * (255U - sa) + scaled / 2U) / scaled);
    dst[1] = static_cast<uint8_t>((sg * sa * 255U + dst[1] * da * (255U - sa) + scaled / 2U) / scaled);
    dst[2] = static_cast<uint8_t>((sb * sa * 255U + dst[2] * da * (255U - sa) + scaled / 2U) / scaled);
    dst[3] = static_cast<uint8_t>((scaled + 127U) / 255U);
  }
  void rect(int x, int y, int rw, int rh, uint32_t rgba) {
    for (int r = y; r < y + rh; ++r)
      for (int c = x; c < x + rw; ++c)
        if (uint8_t *d = at(c, r)) blend_px(d, rgba);
  }
};
bool same(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t n) { return a.size() >= n && b.size() >= n && std::memcmp(a.data(), b.data(), n) == 0; }
bool fixed_equals(const Transport &t, const Canvas &c) { return t.ov.width == 128 && t.ov.height == 256 && same(t.fixed, c.px, c.px.size()); }
std::vector<uint8_t> crop(const std::vector<uint8_t> &img, int w, int sx, int sy, int cw, int ch) {
  std::vector<uint8_t> out(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4U);
  for (int r = 0; r < ch; ++r) std::memcpy(&out[static_cast<size_t>(r) * static_cast<size_t>(cw) * 4U], &img[(static_cast<size_t>(sy + r) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4U], static_cast<size_t>(cw) * 4U);
  return out;
}
bool faulted(const Transport &t) { return t.dec.accepted == 0U && t.dec.reason == GENESIS_COMPAT_FAULT; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void bitmap_tests() {
  // ---- a full-size bitmap onto an empty fixed overlay reproduces the ABI 1 bytes for every alpha; nothing else is drawn ----
  {
    Harness h;
    const std::vector<uint8_t> img = pattern(128, 256, 3);
    Script s; s.bitmap(0, 0, 128, 256, img.data());
    const Transport t = h.produce();
    check(g_script.has_render_api == 1U && g_script.has_frame == 1U && g_script.statuses[0] == 0, "the bitmap call succeeds inside the exact-frame callback");
    check(t.dec.accepted == GENESIS_COMPAT_HUD && t.drawn && t.ov.width == 128 && t.ov.height == 256 && same(t.fixed, img, img.size()), "a 128 x 256 bitmap equals the ABI 1 GenesisCompatOverlay::rgba bytes (alpha 0 with RGB, 1, 127, 128, 254, 255 survive)");
    check(t.ov.layer_count == 0U && t.ov.fallback_x == 0 && t.ov.fallback_y == 0, "no layer, no fallback: the transport of an old provider");
    // the same image through an ABI 1 provider's own writes
    GenesisCompatOverlay twin{};
    std::vector<uint8_t> twin_px(GENESIS_COMPAT_OVERLAY_BYTES);
    std::memcpy(twin_px.data(), img.data(), img.size());
    twin.width = 128; twin.height = 256;
    check(std::memcmp(twin_px.data(), t.fixed.data(), twin_px.size()) == 0 && twin.width == t.ov.width && twin.height == t.ov.height, "pixel equality with the ABI 1 twin for the fixed overlay");
  }
  // ---- untouched region and transparent data under an origin; stride with padding; clamping; fully outside ----
  {
    Harness h;
    const std::vector<uint8_t> img = pattern(30, 10, 5);
    std::vector<uint8_t> padded(static_cast<size_t>(30 * 4 + 8) * 10U, 0xEE);
    for (int r = 0; r < 10; ++r) std::memcpy(&padded[static_cast<size_t>(r) * (30U * 4U + 8U)], &img[static_cast<size_t>(r) * 30U * 4U], 30U * 4U);
    Script s; s.bitmap(10, 20, 30, 10, padded.data(), 30 * 4 + 8);
    Canvas c(128, 256); c.replace(10, 20, 30, 10, img);
    const Transport t = h.produce();
    check(fixed_equals(t, c), "stride padding is ignored; pixels outside the bitmap stay untouched (zero)");
  }
  {
    Harness h;
    const std::vector<uint8_t> img = pattern(16, 16, 9);
    Script s; s.bitmap(-4, -3, 16, 16, img.data()); s.bitmap(120, 250, 16, 16, img.data()); s.bitmap(300, 0, 16, 16, img.data()); s.bitmap(0, -16, 16, 16, img.data());
    Canvas c(128, 256);
    c.replace(-4, -3, 16, 16, img); c.replace(120, 250, 16, 16, img);
    const Transport t = h.produce();
    check(g_script.ran == 4U && g_script.statuses[0] == 0 && g_script.statuses[2] == 0 && fixed_equals(t, c), "an origin outside the target clamps (a bitmap partly or fully outside never faults and never moves)");
  }
  // ---- interleaving with rect and text in submission order (golden image) ----
  {
    Harness h;
    const std::vector<uint8_t> a = pattern(16, 16, 1), b = pattern(16, 16, 2), c3 = pattern(8, 8, 4);
    Script s;
    s.bitmap(0, 0, 16, 16, a.data());
    s.rect(4, 4, 8, 8, 0x11223380U);     // blends over the bitmap bytes with the unchanged rule
    s.bitmap(8, 8, 16, 16, b.data());    // replaces the earlier rect/bitmap where it covers
    s.rect(0, 0, 2, 2, 0xFF000040U);
    s.bitmap(10, 10, 8, 8, c3.data());   // bitmap over bitmap
    s.rect(30, 30, 4, 4, 0x80808000U);   // a fully transparent rect leaves the destination unchanged
    Canvas c(128, 256);
    c.replace(0, 0, 16, 16, a); c.rect(4, 4, 8, 8, 0x11223380U); c.replace(8, 8, 16, 16, b); c.rect(0, 0, 2, 2, 0xFF000040U); c.replace(10, 10, 8, 8, c3); c.rect(30, 30, 4, 4, 0x80808000U);
    const Transport t = h.produce();
    check(fixed_equals(t, c), "bitmap / rect / bitmap-over-bitmap in one callback equal the image of applying them in submission order (rect blend, bitmap replacement)");
  }
  {
    Harness h;   // text is drawn with the host's font: its coverage is measured on its own, then composed over a bitmap
    Script s; s.text(2, 2, "A1", 0xFFFFFFFFU);
    const Transport only_text = h.produce();
    Harness h2;
    const std::vector<uint8_t> a = pattern(24, 24, 6);
    Script s2; s2.bitmap(0, 0, 24, 24, a.data()); s2.text(2, 2, "A1", 0xFFFFFFFFU);
    const Transport mixed = h2.produce();
    Canvas c(128, 256); c.replace(0, 0, 24, 24, a);
    for (int y = 0; y < 256; ++y)
      for (int x = 0; x < 128; ++x) {
        const uint8_t *tp = &only_text.fixed[(static_cast<size_t>(y) * 128U + static_cast<size_t>(x)) * 4U];
        if (tp[3] != 0) c.blend_px(c.at(x, y), 0xFFFFFFFFU);
      }
    check(only_text.drawn && fixed_equals(mixed, c), "text after a bitmap blends over its bytes with the existing result");
    Harness h3;
    const std::vector<uint8_t> a3 = pattern(24, 24, 6);
    Script s3; s3.text(2, 2, "A1", 0xFFFFFFFFU); s3.bitmap(0, 0, 24, 24, a3.data());
    Canvas c3(128, 256); c3.replace(0, 0, 24, 24, a3);
    check(fixed_equals(h3.produce(), c3), "text before a bitmap is replaced where the bitmap covers it");
  }
  // ---- rect/text-only output stays what the unchanged rule gives (explicit expected pixels) ----
  {
    Harness h;
    Script s; s.rect(0, 0, 2, 1, 0x11223380U); s.rect(0, 0, 1, 1, 0xFF0000FFU); s.rect(1, 0, 1, 1, 0x00FF0080U);
    Canvas c(128, 256); c.rect(0, 0, 2, 1, 0x11223380U); c.rect(0, 0, 1, 1, 0xFF0000FFU); c.rect(1, 0, 1, 1, 0x00FF0080U);
    const Transport t = h.produce();
    check(fixed_equals(t, c) && t.fixed[0] == 255 && t.fixed[3] == 255 && t.fixed[7] != 0, "rect-only output is exactly the existing blend");
  }
  // ---- layers: ABI 1 twin equality at every anchor and reference, dx/dy, z, clip, fallback ----
  for (int anchor = 0; anchor <= 8; ++anchor)
    for (int ref = 0; ref <= 1; ++ref) {
      Harness h;
      const std::vector<uint8_t> img = pattern(96, 80, static_cast<unsigned>(anchor * 2 + ref));
      const std::vector<uint8_t> fixed_img = pattern(40, 20, 11);
      Script s;
      s.layer_bitmap(2, anchor, ref, anchor - 4, ref * 3 - 1, 96, 80, 3);
      s.target(2); s.bitmap(0, 0, 96, 80, img.data());
      s.clip(2, 5, 6, 40, 30);
      s.target(0); s.bitmap(0, 0, 40, 20, fixed_img.data());
      s.fallback(7, 9);
      const Transport t = h.produce();
      // the ABI 1 provider's own writes
      GenesisCompatOverlay twin{};
      Canvas c(128, 256); c.replace(0, 0, 40, 20, fixed_img);
      twin.width = 128; twin.height = 256; twin.fallback_x = 7; twin.fallback_y = 9; twin.layer_count = 1;
      GenesisCompatLayer &l = twin.layers[0];
      std::memset(&l, 0, sizeof l);
      l.width = 96; l.height = 80; l.anchor = static_cast<uint8_t>(anchor); l.reference = static_cast<uint8_t>(ref); l.z = 3; l.has_clip = 1;
      l.dx = static_cast<int16_t>(anchor - 4); l.dy = static_cast<int16_t>(ref * 3 - 1); l.clip_x = 5; l.clip_y = 6; l.clip_w = 40; l.clip_h = 30; l.offset = 0;
      const std::string where = " (anchor " + std::to_string(anchor) + ", reference " + std::to_string(ref) + ")";
      check(t.ov.layer_count == 1U && std::memcmp(&t.ov.layers[0], &l, sizeof l) == 0 && same(t.layers, img, img.size()) && fixed_equals(t, c) && t.ov.fallback_x == 7 && t.ov.fallback_y == 9,
            "the layer transport (size, anchor, reference, dx/dy, z, clip, offset, pixels) and the fallback equal the ABI 1 provider's" + where);
    }
  {
    Harness h;   // z ordering and the four 128 x 128 bitmap layers fill the transport exactly
    std::vector<std::vector<uint8_t>> imgs;
    Script s;
    for (int i = 1; i <= 4; ++i) {
      imgs.push_back(pattern(128, 128, static_cast<unsigned>(i)));
      s.layer_bitmap(i, 4, 0, 0, 0, 128, 128, 4 - i);
      s.target(i); s.bitmap(0, 0, 128, 128, imgs.back().data());
    }
    const Transport t = h.produce();
    bool ok = t.ov.layer_count == 4U && g_script.ran == 12U;
    for (int i = 0; ok && i < 4; ++i) ok = t.ov.layers[i].z == 3 - i && t.ov.layers[i].offset == static_cast<uint32_t>(i) * 65536U;
    check(ok, "four bitmap layers of 128 x 128 (exactly the 65536-pixel transport) are accepted in declaration order with their z");
    check(same(t.layers, imgs[0], imgs[0].size()) && std::memcmp(&t.layers[3U * 65536U], imgs[3].data(), imgs[3].size()) == 0, "each layer image is placed at its own offset");
  }
  {
    Harness h;   // a 256 x 256 bitmap layer: exactly at the limit
    const std::vector<uint8_t> img = pattern(256, 256, 8);
    Script s; s.layer_bitmap(1, 0, 0, 0, 0, 256, 256, 0); s.target(1); s.bitmap(0, 0, 256, 256, img.data());
    const Transport t = h.produce();
    check(g_script.ran == 3U && g_script.statuses[2] == 0 && t.ov.layer_count == 1U && t.ov.layers[0].width == 256 && t.ov.layers[0].height == 256 && same(t.layers, img, img.size()), "a 256 x 256 bitmap layer (the ABI 1 maximum) is accepted and byte-exact");
  }
  // ---- limits: one over is a fault, nothing is presented ----
  {
    Harness h;
    const std::vector<uint8_t> big = pattern(128, 256, 1);
    Script s; s.bitmap(0, 0, 128, 256, big.data()); s.bitmap(0, 0, 128, 256, big.data()); s.bitmap(0, 0, 128, 256, big.data());
    const Transport ok = h.produce();
    check(g_script.ran == 3U && g_script.statuses[2] == 0 && ok.drawn, "exactly 393216 submitted bytes (3 x 128 x 256 x 4) are accepted");
    Harness h2;
    const uint8_t one[4] = {1, 2, 3, 4};
    Script s2; s2.bitmap(0, 0, 128, 256, big.data()); s2.bitmap(0, 0, 128, 256, big.data()); s2.bitmap(0, 0, 128, 256, big.data()); s2.bitmap(0, 0, 1, 1, one);
    const Transport over = h2.produce();
    check(g_script.statuses[3] == SEGA_MOD_E_BUDGET && faulted(over) && !over.drawn, "one byte-quantum over the budget is SEGA_MOD_E_BUDGET, the staged output is discarded and the mod is disabled");
  }
  {
    Harness h;
    const uint8_t px[4] = {9, 9, 9, 255};
    Script s; for (int i = 0; i < 8; ++i) s.bitmap(i, 0, 1, 1, px);
    const Transport ok = h.produce();
    check(g_script.ran == 8U && ok.drawn, "exactly 8 bitmap requests are accepted");
    Harness h2;
    Script s2; for (int i = 0; i < 9; ++i) s2.bitmap(i, 0, 1, 1, px);
    const Transport over = h2.produce();
    check(g_script.statuses[8] == SEGA_MOD_E_BUDGET && faulted(over), "the 9th bitmap request is a budget fault");
    Harness h3;   // a bitmap does not consume the 64 rect/text requests; the 65th rect still faults exactly as before
    Script s3; for (int i = 0; i < 8; ++i) s3.bitmap(i, 0, 1, 1, px);
    for (int i = 0; i < 64; ++i) s3.rect(i, 10, 1, 1, 0xFF0000FFU);
    const Transport mixed = h3.produce();
    check(g_script.ran == 72U && mixed.drawn, "8 bitmaps do not consume the 64-op rect/text budget");
    Harness h4;
    Script s4; for (int i = 0; i < 65; ++i) s4.rect(i, 10, 1, 1, 0xFF0000FFU);
    check(g_script.statuses[0] == 0 && (h4.produce(), g_script.ran == 65U && g_script.statuses[64] == SEGA_MOD_E_BUDGET), "the 65th rect is rejected exactly as before");
  }
  {
    Harness h;   // the transport's 65536-pixel budget is shared by every layer: a bitmap layer plus anything beyond it is a budget fault
    Script s; s.layer_bitmap(1, 0, 0, 0, 0, 256, 256, 0); s.layer(2, 0, 0, 0, 0, 1, 1, 1);
    check(faulted(h.produce()) && g_script.statuses[1] == SEGA_MOD_E_BUDGET, "layers beyond the shared 65536-pixel budget are refused (budget)");
    Harness h2;
    Script s2; s2.layer_bitmap(1, 0, 0, 0, 0, 257, 256, 0);
    check(faulted(h2.produce()) && g_script.statuses[0] == SEGA_MOD_E_RANGE, "a bitmap layer wider than 256 is a range fault");
    Harness h3;
    Script s3; s3.layer(1, 0, 0, 0, 0, 129, 8, 0);
    check(faulted(h3.produce()) && g_script.statuses[0] == SEGA_MOD_E_RANGE, "a 129-wide overlay_layer is still rejected exactly as before (no global constant changed)");
    Harness h4;
    Script s4; s4.layer_bitmap(5, 0, 0, 0, 0, 8, 8, 0);
    check(faulted(h4.produce()) && g_script.statuses[0] == SEGA_MOD_E_RANGE, "layer index 5 is a range fault");
  }
  // ---- malformed arguments: range fault, never a read past the buffer (tight allocations make a sanitizer run catch an over-read) ----
  {
    struct Case { const char *name; int x, y, w, h, stride; bool null_px; };
    const Case cases[] = {{"NULL pixels", 0, 0, 4, 4, 16, true},   {"zero width", 0, 0, 0, 4, 16, false}, {"zero height", 0, 0, 4, 0, 16, false}, {"stride too small", 0, 0, 4, 4, 15, false},
                          {"wider than the target", 0, 0, 129, 1, 516, false}, {"taller than the target", 0, 0, 1, 257, 4, false}, {"origin beyond the clamp", -(1 << 21), 0, 4, 4, 16, false}};
    for (const Case &k : cases) {
      Harness h;
      const size_t bytes = static_cast<size_t>(k.stride) * static_cast<size_t>(k.h > 0 ? k.h : 1);   // tight allocation: under a sanitizer any read past the rejected arguments is caught
      std::unique_ptr<uint8_t[]> exact(new uint8_t[bytes]);
      std::memset(exact.get(), 0x5A, bytes);
      Script s; s.bitmap(k.x, k.y, k.w, k.h, k.null_px ? nullptr : exact.get(), k.stride);
      const Transport t = h.produce();
      check(g_script.statuses[0] == SEGA_MOD_E_RANGE && faulted(t) && !t.drawn, std::string("a malformed bitmap (") + k.name + ") is a range fault with no partial staging");
    }
  }
  // ---- bitmap layers accept only bitmaps; legacy layers accept bitmaps; clip and fallback validation ----
  {
    Harness h;
    Script s; s.layer_bitmap(1, 0, 0, 0, 0, 16, 16, 0); s.target(1); s.rect(0, 0, 4, 4, 0xFF0000FFU);
    check(faulted(h.produce()) && g_script.statuses[2] == SEGA_MOD_E_RANGE, "a bitmap layer rejects rect");
    Harness h2;
    Script s2; s2.layer_bitmap(1, 0, 0, 0, 0, 16, 16, 0); s2.target(1); s2.text(0, 0, "x", 0xFF0000FFU);
    check(faulted(h2.produce()) && g_script.statuses[2] == SEGA_MOD_E_RANGE, "a bitmap layer rejects text");
    Harness h3;
    const std::vector<uint8_t> img = pattern(16, 16, 2);
    Script s3; s3.layer(1, 0, 0, 0, 0, 16, 16, 0); s3.target(1); s3.rect(0, 0, 2, 2, 0xFF0000FFU); s3.bitmap(0, 0, 16, 16, img.data());
    const Transport t3 = h3.produce();
    check(t3.ov.layer_count == 1U && g_script.ran == 4U && same(t3.layers, img, img.size()), "a legacy 128-limit layer accepts a bitmap (replacing the rect under it)");
    Harness h4;
    Script s4; s4.layer_bitmap(1, 0, 0, 0, 0, 16, 16, 0); s4.clip(1, 10, 10, 7, 1);
    check(faulted(h4.produce()) && g_script.statuses[1] == SEGA_MOD_E_RANGE, "a clip outside its layer is a range fault");
    Harness h5;
    Script s5; s5.clip(1, 0, 0, 1, 1);
    check(faulted(h5.produce()) && g_script.statuses[0] == SEGA_MOD_E_RANGE, "a clip for an undeclared layer is a range fault");
    Harness h6;
    Script s6; s6.fallback(70000, 0);
    check(faulted(h6.produce()) && g_script.statuses[0] == SEGA_MOD_E_RANGE, "a fallback beyond 65535 is a range fault");
    Harness h7;
    Script s7; s7.fallback(1, 2); s7.fallback(65535, 65535); s7.rect(0, 0, 1, 1, 0xFF0000FFU);
    const Transport t7 = h7.produce();
    check(t7.ov.fallback_x == 65535 && t7.ov.fallback_y == 65535, "the last fallback call wins");
  }
  // ---- regression, old providers: legacy output unchanged (no fallback, no clip, same layers, same notes) ----
  {
    Harness h;
    Script s;
    s.layer(1, 4, 0, 3, -2, 128, 128, 0); s.layer(2, 8, 1, 0, 0, 128, 128, 1); s.layer(3, 0, 0, 0, 0, 128, 128, 2); s.layer(4, 2, 0, 0, 0, 128, 128, 3);
    s.target(1); s.rect(0, 0, 10, 10, 0x00FF00FFU); s.target(0); s.text(1, 1, "HUD", 0xFFFFFFFFU);
    const Transport t = h.produce();
    check(t.ov.layer_count == 4U && g_script.ran == 8U && t.ov.fallback_x == 0 && t.ov.fallback_y == 0 && t.ov.layers[0].has_clip == 0 && t.ov.layers[0].width == 128 && t.ov.layers[0].dx == 3 && t.ov.layers[0].dy == -2,
          "four legacy 128 x 128 layers with rect/text: accepted, no clip, no fallback (an old provider's transport is unchanged)");
    Harness h2;
    Script s2; s2.text(0, 0, "this text is deliberately longer than sixty-four bytes so that the host cuts it to the limit", 0xFFFFFFFFU);
    check(h2.produce().drawn && g_script.statuses[0] == 0, "an over-long text is truncated with a note, not rejected, exactly as before");
  }
  // ---- frame exactness and determinism ----
  {
    Harness h;
    const std::vector<uint8_t> a = pattern(20, 20, 1), b = pattern(20, 20, 2);
    Script s; s.bitmap(0, 0, 20, 20, a.data());
    const Transport first = h.produce(0x11U);
    Canvas ca(128, 256); ca.replace(0, 0, 20, 20, a);
    check(first.drawn && fixed_equals(first, ca), "frame one presents its bitmap");
    const Transport skipped = h.produce(0x22U, GENESIS_COMPAT_SPRITES);   // a produced frame that asks for neither HUD nor scenery does not call the provider
    check(skipped.dec.accepted == 0U && !skipped.drawn && g_script.calls == 1U, "a bitmap is not carried to a frame that did not run the callback");
    Script s2; s2.bitmap(0, 0, 20, 20, b.data());
    const Transport second = h.produce(0x33U);
    Canvas cb(128, 256); cb.replace(0, 0, 20, 20, b);
    check(fixed_equals(second, cb) && g_script.calls == 2U, "the next frame presents only its own bitmap (an aspect change or a restore re-runs the callback)");
    Harness d1, d2;
    Script s3; s3.layer_bitmap(1, 4, 1, 2, 3, 20, 20, 1); s3.target(1); s3.bitmap(0, 0, 20, 20, a.data()); s3.clip(1, 1, 1, 8, 8); s3.fallback(3, 4);
    const Transport r1 = d1.produce(), r2 = d2.produce();
    check(r1.fixed == r2.fixed && r1.layers == r2.layers && std::memcmp(&r1.ov.layers[0], &r2.ov.layers[0], sizeof(GenesisCompatLayer)) == 0 && r1.ov.fallback_x == r2.ov.fallback_x, "replay yields an identical transport");
  }
  // ---- a faulted callback discards its bitmaps with the rest of the output ----
  {
    Harness h;
    const std::vector<uint8_t> a = pattern(8, 8, 1);
    Script s; s.bitmap(0, 0, 8, 8, a.data()); s.layer_bitmap(9, 0, 0, 0, 0, 8, 8, 0);
    const Transport t = h.produce();
    check(faulted(t) && !t.drawn, "a fault after a staged bitmap discards it with the other output");
    const unsigned calls = g_script.calls;
    const Transport after = h.produce(0x44U);
    check(g_script.calls == calls && faulted(after), "the mod stays disabled and is not asked again");
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void refusal_tests() {
  const unsigned reasons[4] = {GENESIS_COMPAT_UNSUPPORTED_SCENE, GENESIS_COMPAT_BAD_SCROLL, GENESIS_COMPAT_MISMATCH, GENESIS_COMPAT_BAD_STATE};
  uint64_t guest_digest_declined = 0, guest_digest_plain = 0;
  const auto ram_hash = [](GenesisRuntime &rt) { uint64_t v = 1469598103934665603ULL; for (size_t i = 0; i < 4096U; ++i) { v ^= rt.work_ram[i]; v *= 1099511628211ULL; } return v; };
  // ---- every reason for every component subset: no fault, the mod stays enabled, the next frame is accepted ----
  for (const unsigned reason : reasons)
    for (unsigned mask = 1U; mask <= kAll; ++mask) {
      Harness h;
      Script s; s.rect(0, 0, 8, 8, 0x102030FFU); s.scene("refusal"); s.window(0, -1, 0, 0x2011U); s.sprites(); s.decline(mask, reason);
      const Transport t = h.produce(0x11U, kAll);
      const std::string where = " (reason " + std::to_string(reason) + ", mask " + std::to_string(mask) + ")";
      const bool hud_served = (mask & GENESIS_COMPAT_HUD) == 0U || (mask & GENESIS_COMPAT_SPRITES) == 0U;   // the HUD bit carries the overlay and the sprites: it is served while either is not declined
      const unsigned expected = ((mask & GENESIS_COMPAT_SCENERY) == 0U ? GENESIS_COMPAT_SCENERY : 0U) | (hud_served ? GENESIS_COMPAT_HUD : 0U);
      check(g_script.statuses[g_script.ran - 1U] == 0 && t.dec.accepted == expected, "the declared components are withdrawn and the others served" + where);
      check(expected != 0U || (t.dec.reason == reason && t.dec.scene[0] == '\0'), "a wholly declined frame reports the provider's diagnostic reason and no scene" + where);
      if ((mask & GENESIS_COMPAT_HUD) != 0U && (mask & GENESIS_COMPAT_SPRITES) != 0U) check(!t.drawn, "a declined HUD with declined sprites presents no overlay" + where);
      if ((mask & GENESIS_COMPAT_HUD) == 0U) check(t.drawn && t.fixed[3] == 255, "an accepted HUD still presents its overlay" + where);
      if ((mask & GENESIS_COMPAT_HUD) != 0U && (mask & GENESIS_COMPAT_SPRITES) == 0U) check(t.drawn && t.sprite_count == 1U && t.ov.width == 0, "a declined HUD presents no overlay pixels while the sprites stay" + where);
      if ((mask & GENESIS_COMPAT_SPRITES) != 0U && (mask & GENESIS_COMPAT_HUD) == 0U) check(t.drawn && t.sprite_count == 0U, "declined sprites are not presented while the HUD stays" + where);
      if ((mask & GENESIS_COMPAT_SCENERY) == 0U) check(t.word_status == 1 && t.word == 0x2011U, "an accepted scenery window answers" + where);
      // the next frame can be accepted: the mod is still enabled
      Script s2; s2.rect(0, 0, 8, 8, 0xFF0000FFU);
      const Transport next = h.produce(0x22U, kAll);
      check(g_script.calls == 2U && (next.dec.accepted & GENESIS_COMPAT_HUD) != 0U && next.drawn && next.dec.reason == GENESIS_COMPAT_OK, "the mod stays enabled: the next frame is accepted" + where);
      run_frames(*h.rt, 9U);
      guest_digest_declined = ram_hash(*h.rt);
    }
  {
    Harness h;   // the same guest without any refusal
    Script s; s.rect(0, 0, 8, 8, 0xFF0000FFU);
    h.produce(0x11U, kAll); h.produce(0x22U, kAll);
    run_frames(*h.rt, 9U);
    guest_digest_plain = ram_hash(*h.rt);
  }
  check(guest_digest_declined == guest_digest_plain, "a refusal never changes the guest (work RAM digest equal with and without)");
  // ---- a refusal with no output: legal 'nothing to add'; a refusal of a component that was not requested is accepted ----
  {
    Harness h;
    Script s;
    const Transport none = h.produce(0x11U, kAll);
    check(none.dec.accepted == 0U && none.dec.reason == GENESIS_COMPAT_OK && g_script.calls == 1U, "no output and no refusal is the legal nothing-to-add (verdict OK)");
    Script s2; s2.decline(GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_MISMATCH); s2.rect(0, 0, 4, 4, 0xFF0000FFU);
    check((h.produce(0x22U, GENESIS_COMPAT_HUD).dec.accepted & GENESIS_COMPAT_HUD) != 0U, "declining a component that was not requested is harmless");
  }
  // ---- calling it twice: the union of the components, the first reason wins ----
  {
    Harness h;
    Script s; s.rect(0, 0, 4, 4, 0xFF0000FFU); s.scene("twice"); s.window(0, -1, 0, 0x2011U); s.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_BAD_STATE); s.decline(GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_MISMATCH);
    const Transport t = h.produce(0x11U, kAll);
    check(g_script.ran == 5U && t.dec.accepted == 0U && t.dec.reason == GENESIS_COMPAT_BAD_STATE, "two declines declare the union and the first reason wins");
  }
  // ---- impersonation, an unknown reason and an empty/unknown mask are range faults; the whole frame is discarded ----
  {
    const struct { unsigned mask, reason; const char *name; } bad[] = {{GENESIS_COMPAT_HUD, GENESIS_COMPAT_STALE_STATE, "STALE_STATE"}, {GENESIS_COMPAT_HUD, GENESIS_COMPAT_FAULT, "FAULT"}, {GENESIS_COMPAT_HUD, GENESIS_COMPAT_OK, "OK"},
                                                                       {GENESIS_COMPAT_HUD, 7U, "an unknown reason (7)"}, {GENESIS_COMPAT_HUD, 99U, "an unknown reason (99)"}, {0U, GENESIS_COMPAT_MISMATCH, "an empty mask"},
                                                                       {8U, GENESIS_COMPAT_MISMATCH, "an unknown component bit"}, {GENESIS_COMPAT_HUD | 16U, GENESIS_COMPAT_MISMATCH, "a known and an unknown bit"}};
    for (const auto &b : bad) {
      Harness h;
      Script s; s.rect(0, 0, 4, 4, 0xFF0000FFU); s.decline(b.mask, b.reason);
      const Transport t = h.produce(0x11U, kAll);
      check(g_script.statuses[1] == SEGA_MOD_E_RANGE && faulted(t) && !t.drawn, std::string("an invalid refusal (") + b.name + ") is a range fault that discards the callback's output");
      const unsigned calls = g_script.calls;
      check(faulted(h.produce(0x22U, kAll)) && g_script.calls == calls, std::string("the mod is disabled after ") + b.name);
    }
  }
  // ---- genuine faults stay faults: a wrong-range call, an oversize bitmap ----
  {
    Harness h;
    Script s; s.rect(0, 0, 4, 4, 0xFF0000FFU); s.layer(99, 0, 0, 0, 0, 4, 4, 0);
    const Transport t = h.produce(0x11U, kAll);
    check(faulted(t) && !t.drawn, "an invalid call (bad range) discards the frame's output and presents generically");
    Harness h2;
    const std::vector<uint8_t> img = pattern(8, 8, 1);
    Script s2; s2.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_MISMATCH); s2.bitmap(0, 0, 129, 1, img.data(), 129 * 4);
    check(faulted(h2.produce(0x11U, kAll)) && g_script.statuses[1] == SEGA_MOD_E_RANGE, "a refusal followed by an oversize bitmap is still a fault (the refusal does not excuse it)");
  }
  // ---- parity of verdicts with an ABI 1 twin over a scripted sequence of states ----
  {
    struct State { const char *name; unsigned mask, reason; };
    const State states[] = {{"fade", GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_MISMATCH},
                            {"wrong scroll", GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_BAD_SCROLL},
                            {"other level", GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_UNSUPPORTED_SCENE},
                            {"bad state", GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_SPRITES, GENESIS_COMPAT_BAD_STATE},
                            {"accepted", 0U, GENESIS_COMPAT_OK}};
    Harness h;
    for (const State &st : states) {
      Script s;
      if (st.mask != 0U) s.decline(st.mask, st.reason);
      s.rect(0, 0, 4, 4, 0xFF0000FFU); s.scene("parity"); s.window(0, -1, 0, 0x2011U);
      // the ABI 1 twin's verdict for the same state: a provider that declines writes accepted = 0 and the reason, otherwise it serves the requested components
      GenesisCompatDecision twin{};
      twin.accepted = st.mask != 0U ? 0U : (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY);
      twin.reason = static_cast<uint8_t>(st.reason);
      const Transport t = h.produce(0x11U, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY);
      check(t.dec.accepted == twin.accepted && (twin.accepted != 0U || t.dec.reason == twin.reason), std::string("ABI 1 and ABI 2 twins report the same verdict and reason for: ") + st.name);
    }
  }
  // ---- no stale acceptance: nothing carries from one frame to the next ----
  {
    Harness h;
    Script s; s.rect(0, 0, 4, 4, 0xFF0000FFU); s.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_MISMATCH);
    check(h.produce(0x11U, GENESIS_COMPAT_HUD).dec.accepted == 0U, "a refused frame is refused");
    Script s2; s2.rect(0, 0, 4, 4, 0xFF0000FFU);
    const Transport next = h.produce(0x22U, GENESIS_COMPAT_HUD);
    check(next.dec.accepted == GENESIS_COMPAT_HUD && next.drawn, "the next frame is accepted: a refusal never carries over");
    Script s3; s3.rect(0, 0, 4, 4, 0xFF0000FFU); s3.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_MISMATCH);
    h.produce(0x33U, GENESIS_COMPAT_HUD);
    const Transport skipped = h.produce(0x44U, GENESIS_COMPAT_SPRITES);   // no callback for this produced frame
    check(skipped.dec.accepted == 0U && g_script.calls == 4U - 1U, "a produced frame that does not run the callback neither inherits an acceptance nor a refusal");
  }
  // ---- already non-fault: no output is not served; a plane without a window is generic; an unanswered cell inside a window plane is a generic cell plus a failed answer ----
  {
    Harness h;
    Script s; s.window(0, -1, 0, 0x2011U);
    const Transport t = h.produce(0x11U, GENESIS_COMPAT_SCENERY);
    check(t.dec.accepted == GENESIS_COMPAT_SCENERY && t.word_status == 1 && t.outside_status != 1 && t.outside_status != GENESIS_COMPAT_WORD_GENERIC, "an unanswered cell inside a window plane is a failed answer, never a guess (the host then reports the authored claim as failed)");
    uint16_t w = 0;
    check(h.table->scenery_word(h.state, 1U, 3, -4, 0, 0, &w) == GENESIS_COMPAT_WORD_GENERIC, "a plane without a window is left to the generic extension (ADR 0314), not a failure");
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The ordinary (after-snapshot) render route: the callback of guest frame N is served for the frame the producer makes when the runtime counts N + 2. A refusal belongs to that frame exactly like the windows.
void ordinary_tests() {
  Harness h;
  const std::vector<uint8_t> img = pattern(8, 8, 1);
  {
    Script s; s.rect(0, 0, 4, 4, 0xFF0000FFU); s.window(0, -1, 0, 0x2011U); s.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_MISMATCH);
    run_frames(*h.rt, 8U);
    check(g_script.calls > 0U && g_script.has_frame == 0U && g_script.has_render_api == 1U && g_script.statuses[2] == 0, "the ordinary render callback ran after the snapshots (no exact-frame view)");
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    const Transport own = h.produce(0x11U);
    check(own.dec.accepted == GENESIS_COMPAT_SCENERY && !own.drawn && own.word_status == 1, "the produced frame the callback describes (N + 2) gets its refusal: HUD withdrawn, scenery served, mod enabled");
    h.rt->virtual_frame_count = g_script.last_frame + 7U;   // a skipped callback: the frame sequence jumped
    const Transport skipped = h.produce(0x12U);
    check(skipped.dec.reason == GENESIS_COMPAT_OK && (skipped.dec.accepted & GENESIS_COMPAT_SCENERY) == 0U && (skipped.dec.accepted & GENESIS_COMPAT_HUD) != 0U, "after a skipped callback the earlier refusal is not observed (the declared HUD is not withdrawn from another frame)");
    h.rt->virtual_frame_count = g_script.last_frame;   // rewind / load: the counter goes back
    const Transport rewound = h.produce(0x13U);
    check((rewound.dec.accepted & GENESIS_COMPAT_HUD) != 0U && (rewound.dec.accepted & GENESIS_COMPAT_SCENERY) == 0U, "after a rewind or load the earlier refusal is not observed either");
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    const Transport again = h.produce(0x14U);
    check(again.dec.accepted == GENESIS_COMPAT_SCENERY, "the same epoch is honoured again (presentation re-enabled / aspect change re-asks with the same callback)");
    const Transport side = [&] { h.frame->make(0x15U, 40U, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY); Transport t; h.table->prepare(h.state, &h.frame->cf, &t.dec); return t; }();
    check(side.dec.accepted == GENESIS_COMPAT_SCENERY, "an aspect change (another planned side) keeps the refusal of the callback's own frame");
  }
  {
    Script s2; s2.rect(0, 0, 4, 4, 0xFF0000FFU);   // the next callback has no refusal: nothing carries over
    run_frames(*h.rt, h.rt->virtual_frame_count + 4U);
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    const Transport t = h.produce(0x16U);
    check((t.dec.accepted & GENESIS_COMPAT_HUD) != 0U && t.drawn && t.dec.reason == GENESIS_COMPAT_OK, "a later callback without a refusal replaces it");
  }
  {
    Harness h2;   // overlay.rgba granted: the bitmap is accepted on the ordinary route too
    Script s3; s3.bitmap(0, 0, 8, 8, img.data());
    run_frames(*h2.rt, 6U);
    check(g_script.statuses[0] == 0, "with overlay.rgba granted the bitmap call succeeds");
    h2.rt->virtual_frame_count = g_script.last_frame + 2U;
    Canvas c(128, 256); c.replace(0, 0, 8, 8, img);
    check(fixed_equals(h2.produce(0x21U), c), "and its bytes reach the transport");
  }
}

// A provider granted ONLY scenery.words receives the render table (it needs scenery_window) but no overlay output call.
void capability_tests() {
  const std::vector<uint8_t> img = pattern(8, 8, 1);
  {
    Harness h;
    Script s; s.window(0, -1, 0, 0x2011U);
    run_frames(*h.rt, 8U);
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    const Transport t = h.produce(0x11U, GENESIS_COMPAT_SCENERY);
    check(g_script.has_render_api == 1U && g_script.statuses[0] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY && t.word_status == 1 && t.word == 0x2011U, "a scenery-only provider gets the render table and its scenery window is served unchanged");
    Harness h1;
    Script s1; s1.window(0, -1, 0, 0x2011U); s1.scene("only-scenery"); s1.decline(GENESIS_COMPAT_HUD, GENESIS_COMPAT_MISMATCH);
    run_frames(*h1.rt, 8U);
    check(g_script.ran == 3U && g_script.statuses[1] == 0 && g_script.statuses[2] == 0, "scene and decline need no overlay grant");
  }
  {
    Harness h;
    Script s; s.window(0, -1, 0, 0x2011U); s.bitmap(0, 0, 8, 8, img.data());
    run_frames(*h.rt, 8U);
    check(g_script.statuses[0] == 0 && g_script.statuses[1] == SEGA_MOD_E_CAPABILITY, "a bitmap without overlay.rgba is SEGA_MOD_E_CAPABILITY");
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    const Transport t = h.produce(0x11U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD);
    check(faulted(t) && !t.drawn && t.word_status == -1, "the callback faulted: its scenery window and every output are discarded and nothing is presented");
  }
  struct Gated { const char *name; void (*build)(Script &, const uint8_t *); };
  const Gated gated[] = {
      {"overlay_rect", [](Script &s, const uint8_t *) { s.rect(0, 0, 2, 2, 0xFF0000FFU); }},
      {"overlay_text", [](Script &s, const uint8_t *) { s.text(0, 0, "x", 0xFF0000FFU); }},
      {"overlay_layer", [](Script &s, const uint8_t *) { s.layer(1, 0, 0, 0, 0, 8, 8, 0); }},
      {"overlay_target", [](Script &s, const uint8_t *) { s.target(0); }},
      {"overlay_bitmap", [](Script &s, const uint8_t *p) { s.bitmap(0, 0, 8, 8, p); }},
      {"overlay_layer_bitmap", [](Script &s, const uint8_t *) { s.layer_bitmap(1, 0, 0, 0, 0, 8, 8, 0); }},
      {"overlay_layer_clip", [](Script &s, const uint8_t *) { s.clip(1, 0, 0, 1, 1); }},
      {"overlay_fallback", [](Script &s, const uint8_t *) { s.fallback(1, 1); }},
      {"virtual_sprites", [](Script &s, const uint8_t *) { s.sprites(); }}};
  for (const Gated &g : gated) {
    Harness h;
    Script s; g.build(s, img.data());
    run_frames(*h.rt, 8U);
    check(g_script.statuses[0] == SEGA_MOD_E_CAPABILITY, std::string(g.name) + " without overlay.rgba is SEGA_MOD_E_CAPABILITY (a fault with cause capability)");
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    check(faulted(h.produce(0x11U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD)), std::string(g.name) + ": the mod is disabled and presents nothing");
  }
}

// ---- SEG-152-T007: the per-row generic scenery mask through the real adapter -----------------------------------------------------------------------------------------------
constexpr int kVdpStart = GENESIS_COMPAT_WORD_VDP_START;
// One scenery answer of the committed output of the frame `produce` described (plane, produced screen row, screen column, no scroll).
int answer_at(Harness &h, unsigned plane, unsigned row, int x, uint16_t *word) { return h.table->scenery_word(h.state, plane, row, x, 0, 0, word); }
std::vector<uint8_t> rows_bits(std::initializer_list<unsigned> set, unsigned rows) {
  std::vector<uint8_t> bits((rows + 7U) / 8U, 0U);
  for (const unsigned r : set) if (r < rows) bits[r >> 3U] = static_cast<uint8_t>(bits[r >> 3U] | (1U << (r & 7U)));
  return bits;
}

void rows_tests(bool exact) {
  const auto commit = [&](Harness &h) -> Transport {   // exact route: the produced frame is its own callback; ordinary route: the callback of guest frame N describes the frame the runtime counts N + 2
    if (!exact) { run_frames(*h.rt, h.rt->virtual_frame_count + 4U); h.rt->virtual_frame_count = g_script.last_frame + 2U; }
    return h.produce(0x11U, GENESIS_COMPAT_SCENERY);
  };
  const auto fault_case = [&](const char *what, void (*build)(Script &, const uint8_t *)) {
    Harness h;
    static const uint8_t ok[1] = {0x01U};
    Script s; s.window(0, -1, 0, 0x2011U); s.mask(0, 8, 1, ok);   // a valid window and mask first: the fault must discard them together
    build(s, ok);
    const Transport t = commit(h);
    check(g_script.statuses[g_script.ran - 1U] == SEGA_MOD_E_RANGE, std::string(what) + ": a range fault");
    check(faulted(t) && t.word_status == -1, std::string(what) + ": the whole callback is discarded (no partial staging: windows and masks together), the mod is disabled and nothing is presented");
  };
  // ---- the answer: a marked row is the host-private code 3, before any window; an unmarked row follows the window rules; a plane without a window is generic (ADR 0314) ----
  {
    Harness h;
    const std::vector<uint8_t> m = rows_bits({5, 6, 200}, 224);
    Script s; s.mask(0, 224, 28, m.data());
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[0] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY, "a mask with a set bit and no window serves the frame (an all-own_plane frame is authored under ABI 1)");
    check(answer_at(h, 0, 5, -3, &w) == kVdpStart && answer_at(h, 0, 6, -300, &w) == kVdpStart && answer_at(h, 0, 200, 400, &w) == kVdpStart, "marked screen rows answer the private code, whatever the column");
    check(answer_at(h, 0, 7, -3, &w) == GENESIS_COMPAT_WORD_GENERIC && answer_at(h, 0, 4, -3, &w) == GENESIS_COMPAT_WORD_GENERIC, "unmarked rows of a plane without a window are left to the generic extension");
    check(answer_at(h, 1, 5, -3, &w) == GENESIS_COMPAT_WORD_GENERIC, "the other plane is independent: it submitted no mask");
    check(answer_at(h, 0, 224, -3, &w) == GENESIS_COMPAT_WORD_GENERIC && answer_at(h, 0, 447, -3, &w) == GENESIS_COMPAT_WORD_GENERIC && answer_at(h, 0, 100000, -3, &w) == GENESIS_COMPAT_WORD_GENERIC, "rows at or beyond `rows` are not marked");
  }
  {
    Harness h;   // an empty mask has no effect and does not count toward the served rule
    const std::vector<uint8_t> none(28, 0);
    Script s; s.mask(0, 224, 28, none.data());
    const Transport t = commit(h);
    check(g_script.statuses[0] == 0 && (t.dec.accepted & GENESIS_COMPAT_SCENERY) == 0U, "a mask without a set bit does not serve the frame");
  }
  for (const bool mask_first : {true, false}) {   // independent of windows, in either order
    Harness h;
    const std::vector<uint8_t> m = rows_bits({3}, 8);
    Script s;
    if (mask_first) { s.mask(0, 8, 1, m.data()); s.window(0, -1, 0, 0x2011U); } else { s.window(0, -1, 0, 0x2011U); s.mask(0, 8, 1, m.data()); }
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[0] == 0 && g_script.statuses[1] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY, "mask and window are both accepted in either order");
    check(answer_at(h, 0, 3, -4, &w) == kVdpStart && answer_at(h, 0, 3, -300, &w) == kVdpStart, "a marked row skips the plane's windows (authored words of that row are ignored)");
    check(answer_at(h, 0, 4, -4, &w) == 1 && w == 0x2011U && answer_at(h, 0, 4, -20, &w) == 0, "an unmarked row of a windowed plane is still answered by its window; a cell outside every window is a failed answer");
  }
  // ---- accepted sizes ----
  for (const unsigned rows : {1U, 7U, 8U, 9U, 224U, 448U}) {
    Harness h;
    const std::vector<uint8_t> m = rows_bits({0, rows - 1U}, rows);
    Script s; s.mask(1, rows, (rows + 7U) / 8U, m.data());
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[0] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY && answer_at(h, 1, 0, -3, &w) == kVdpStart, "rows " + std::to_string(rows) + " is accepted");
    if (rows - 1U < 224U) check(answer_at(h, 1, rows - 1U, -3, &w) == kVdpStart, "the last covered row is marked, rows " + std::to_string(rows));
    else check(answer_at(h, 1, rows - 1U, -3, &w) == kVdpStart, "a 448-row mask marks a row above row 223 as well (interlace mode 2 frames), rows 448");
  }
  {
    Harness h;   // a 448-bit mask with marks above and below row 224, all rows generic, and a mask longer than a 224-row frame
    std::vector<uint8_t> all(56, 0xFFU);
    Script s; s.mask(0, 448, 56, all.data()); s.mask(1, 224, 28, all.data());
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[0] == 0 && g_script.statuses[1] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY && answer_at(h, 0, 0, -3, &w) == kVdpStart && answer_at(h, 0, 223, -3, &w) == kVdpStart && answer_at(h, 0, 224, -3, &w) == kVdpStart && answer_at(h, 0, 447, -3, &w) == kVdpStart && answer_at(h, 1, 223, -3, &w) == kVdpStart && answer_at(h, 1, 224, -3, &w) == GENESIS_COMPAT_WORD_GENERIC, "all rows generic on both planes; rows 224..447 are marked by a 448-row mask only");
  }
  {
    Harness h;   // no rows generic: a full-length empty mask next to a window changes nothing
    const std::vector<uint8_t> none(56, 0);
    Script s; s.window(0, -1, 0, 0x2011U); s.mask(0, 448, 56, none.data());
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[1] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY && answer_at(h, 0, 3, -4, &w) == 1 && w == 0x2011U, "no rows generic: the window answers as it does without a mask");
  }
  // ---- range faults, each rolled back with the valid window and mask staged before it ----
  static const uint8_t pad[1] = {0x20U};
  fault_case("bad plane", [](Script &s, const uint8_t *b) { s.mask(2, 8, 1, b); });
  fault_case("rows 0", [](Script &s, const uint8_t *b) { s.mask(1, 0, 0, b); });
  fault_case("rows 449", [](Script &s, const uint8_t *b) { static const uint8_t big[57] = {1}; (void)b; s.mask(1, 449, 57, big); });
  fault_case("NULL mask", [](Script &s, const uint8_t *) { s.mask(1, 8, 1, nullptr); });
  fault_case("mask_bytes too short", [](Script &s, const uint8_t *) { static const uint8_t two[2] = {1, 0}; s.mask(1, 16, 1, two); });
  fault_case("mask_bytes too long", [](Script &s, const uint8_t *b) { s.mask(1, 8, 2, b); });
  fault_case("mask_bytes zero", [](Script &s, const uint8_t *b) { s.mask(1, 8, 0, b); });
  fault_case("non-zero padding bits", [](Script &s, const uint8_t *) { s.mask(1, 5, 1, pad); });
  fault_case("duplicate submission for one plane", [](Script &s, const uint8_t *b) { s.mask(0, 8, 1, b); });
  {
    Harness h;   // a fault after a valid mask does not resurrect the previous frame's committed state
    const std::vector<uint8_t> m = rows_bits({3}, 8);
    Script s; s.mask(0, 8, 1, m.data());
    commit(h);
    Script s2; s2.mask(0, 8, 1, m.data()); s2.mask(0, 0, 0, nullptr);
    const Transport t = commit(h);
    check(faulted(t) && t.word_status == -1, "a fault discards the new mask and does not resurrect the previous one");
  }
  if (!exact) {
    // ---- frame binding (ordinary route): the mask is never served for a frame other than the one its callback described ----
    Harness h;
    const std::vector<uint8_t> m = rows_bits({3}, 8);
    Script s; s.mask(0, 8, 1, m.data());
    run_frames(*h.rt, 8U);
    const uint64_t frame = g_script.last_frame;
    h.rt->virtual_frame_count = frame + 2U;
    uint16_t w = 0;
    const Transport own = h.produce(0x11U, GENESIS_COMPAT_SCENERY);
    check((own.dec.accepted & GENESIS_COMPAT_SCENERY) != 0U && answer_at(h, 0, 3, -4, &w) == kVdpStart, "the frame the callback described is served");
    h.rt->virtual_frame_count = frame + 7U;   // a skipped callback
    check((h.produce(0x12U, GENESIS_COMPAT_SCENERY).dec.accepted & GENESIS_COMPAT_SCENERY) == 0U, "after a skipped callback the mask is not served");
    h.rt->virtual_frame_count = frame;   // restore / rewind
    check((h.produce(0x13U, GENESIS_COMPAT_SCENERY).dec.accepted & GENESIS_COMPAT_SCENERY) == 0U, "after a restore or rewind the mask is not served");
    h.rt->virtual_frame_count = frame + 2U;   // the same epoch again (presentation re-enabled)
    check((h.produce(0x14U, GENESIS_COMPAT_SCENERY).dec.accepted & GENESIS_COMPAT_SCENERY) != 0U, "the same epoch is served again");
    h.frame->make(0x15U, 40U, GENESIS_COMPAT_SCENERY);   // an aspect change keeps the callback's own frame
    Transport side; h.table->prepare(h.state, &h.frame->cf, &side.dec);
    check((side.dec.accepted & GENESIS_COMPAT_SCENERY) != 0U && answer_at(h, 0, 3, -4, &w) == kVdpStart, "an aspect change re-asks with the same callback");
    Script s2; s2.mask(1, 8, 1, m.data());   // the next callback replaces the mask: plane A's mark is gone, plane B's is present
    run_frames(*h.rt, h.rt->virtual_frame_count + 4U);
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    h.produce(0x16U, GENESIS_COMPAT_SCENERY);
    check(answer_at(h, 0, 3, -4, &w) == GENESIS_COMPAT_WORD_GENERIC && answer_at(h, 1, 3, -4, &w) == kVdpStart, "a later callback replaces the earlier mask: nothing of it survives");
  }
}

// Without scenery.words the call is SEGA_MOD_E_CAPABILITY (a fault of the callback), like scenery_window.
void rows_capability_tests() {
  Harness h;
  const std::vector<uint8_t> m = rows_bits({3}, 8);
  Script s; s.mask(0, 8, 1, m.data());
  run_frames(*h.rt, 8U);
  check(g_script.has_render_api == 1U && g_script.statuses[0] == SEGA_MOD_E_CAPABILITY, "scenery_generic_rows without scenery.words is SEGA_MOD_E_CAPABILITY");
  h.rt->virtual_frame_count = g_script.last_frame + 2U;
  check(faulted(h.produce(0x11U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD)), "the callback faulted: the mod presents nothing");
}

// Outside the render phase the call is SEGA_MOD_E_PHASE (a fault of the callback), distinct from SEGA_MOD_E_CAPABILITY, which needs the render phase first.
void rows_phase_tests() {
  Harness h;
  Script s;
  g_script.probe_phase = SEGA_MOD_PHASE_FRAME_END;
  run_frames(*h.rt, 8U);
  check(g_script.phase_seen == 1U && g_script.phase_status == SEGA_MOD_E_PHASE && g_script.phase_status != SEGA_MOD_E_CAPABILITY, "scenery_generic_rows outside the render phase is SEGA_MOD_E_PHASE (not SEGA_MOD_E_CAPABILITY)");
  h.rt->virtual_frame_count = g_script.last_frame + 2U;
  check(faulted(h.produce(0x11U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD)), "the out-of-phase call faulted the callback: the mod presents nothing");
}
// ---- SEG-152-T017: scenery_row_origin (ABI minor 3) on the real adapter ----
void origin_tests(bool exact) {
  const auto commit = [&](Harness &h) -> Transport {
    if (!exact) { run_frames(*h.rt, h.rt->virtual_frame_count + 4U); h.rt->virtual_frame_count = g_script.last_frame + 2U; }
    return h.produce(0x11U, GENESIS_COMPAT_SCENERY);
  };
  static int16_t delta[448];
  for (int16_t &d : delta) d = 0;
  delta[3] = 11;   // screen row 3: the window cell asked is plane cell + 11
  delta[5] = -4;
  {   // the window sits at world cell 10; row 3 asks plane cell -1 and is answered from world cell 10; row 4 (delta 0) asks cell -1 and fails; a mask keeps precedence
    Harness h;
    const std::vector<uint8_t> m = rows_bits({5}, 8);
    Script s; s.window(0, 10, 0, 0x2011U); s.origin(0, 8, 16, delta); s.mask(0, 8, 1, m.data());
    const Transport t = commit(h);
    uint16_t w = 0;
    check(g_script.statuses[1] == 0 && g_script.statuses[2] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY, "scenery_row_origin is accepted with a window and a mask");
    check(answer_at(h, 0, 3, -4, &w) == 1 && w == 0x2011U, "row 3 asks plane cell -1 and is answered from world cell -1 + 11");
    check(answer_at(h, 0, 4, -4, &w) == 0 && answer_at(h, 0, 3, -20, &w) == 0, "a row without a delta (or a cell outside the window) is a failed answer");
    check(answer_at(h, 0, 5, -4, &w) == kVdpStart, "the per-row mask keeps precedence over the origin delta");
    check(answer_at(h, 1, 3, -4, &w) == GENESIS_COMPAT_WORD_GENERIC, "the other plane submitted nothing and has no window");
  }
  {   // 448 rows are accepted; a submission of all-zero deltas changes nothing
    Harness h;
    Script s; s.window(0, -1, 0, 0x2011U); s.origin(0, 448, 896, delta); s.origin(1, 1, 2, delta);
    const Transport t = commit(h);
    check(g_script.statuses[1] == 0 && g_script.statuses[2] == 0 && t.dec.accepted == GENESIS_COMPAT_SCENERY, "448 rows (896 bytes) and a one-row submission are in range");
  }
  const auto fault_case = [&](const char *what, void (*build)(Script &)) {
    Harness h;
    Script s; s.window(0, -1, 0, 0x2011U); s.origin(0, 8, 16, delta);   // valid staging first: the fault must discard it together with everything else
    build(s);
    const Transport t = commit(h);
    check(g_script.statuses[g_script.ran - 1U] == SEGA_MOD_E_RANGE, std::string(what) + ": a range fault");
    check(faulted(t) && t.word_status == -1, std::string(what) + ": the whole callback is discarded and nothing is presented");
  };
  fault_case("bad plane", [](Script &s) { s.origin(2, 8, 16, delta); });
  fault_case("rows 0", [](Script &s) { s.origin(1, 0, 0, delta); });
  fault_case("rows 449", [](Script &s) { s.origin(1, 449, 898, delta); });
  fault_case("NULL delta", [](Script &s) { s.origin(1, 8, 16, nullptr); });
  fault_case("bytes too short", [](Script &s) { s.origin(1, 8, 15, delta); });
  fault_case("bytes too long", [](Script &s) { s.origin(1, 8, 17, delta); });
  fault_case("duplicate submission for one plane", [](Script &s) { s.origin(0, 8, 16, delta); });
  if (!exact) {   // frame binding: the delta lives with the callback that described the frame and is replaced by the next one
    Harness h;
    Script s; s.window(0, 10, 0, 0x2011U); s.origin(0, 8, 16, delta);
    run_frames(*h.rt, 8U);
    const uint64_t frame = g_script.last_frame;
    h.rt->virtual_frame_count = frame + 2U;
    uint16_t w = 0;
    check((h.produce(0x11U, GENESIS_COMPAT_SCENERY).dec.accepted & GENESIS_COMPAT_SCENERY) != 0U && answer_at(h, 0, 3, -4, &w) == 1 && w == 0x2011U, "the frame the callback described is served with its delta");
    h.rt->virtual_frame_count = frame + 7U;
    check((h.produce(0x12U, GENESIS_COMPAT_SCENERY).dec.accepted & GENESIS_COMPAT_SCENERY) == 0U, "after a skipped callback the windows (and their delta) are not served");
    Script s2; s2.window(0, 10, 0, 0x2011U);   // the next callback submits no delta: nothing of the earlier one survives
    run_frames(*h.rt, h.rt->virtual_frame_count + 4U);
    h.rt->virtual_frame_count = g_script.last_frame + 2U;
    h.produce(0x13U, GENESIS_COMPAT_SCENERY);
    check(answer_at(h, 0, 3, -4, &w) == 0, "a later callback replaces the earlier delta: nothing of it survives");
  }
}
void origin_capability_tests() {
  static int16_t delta[8] = {};
  Harness h;
  Script s; s.origin(0, 8, 16, delta);
  run_frames(*h.rt, 8U);
  check(g_script.has_render_api == 1U && g_script.statuses[0] == SEGA_MOD_E_CAPABILITY, "scenery_row_origin without scenery.words is SEGA_MOD_E_CAPABILITY");
  h.rt->virtual_frame_count = g_script.last_frame + 2U;
  check(faulted(h.produce(0x11U, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD)), "the callback faulted: the mod presents nothing");
}
}  // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "bitmap") bitmap_tests();
  else if (mode == "refusal") refusal_tests();
  else if (mode == "ordinary") ordinary_tests();
  else if (mode == "capability") capability_tests();
  else if (mode == "rows_exact") rows_tests(true);
  else if (mode == "rows_ordinary") rows_tests(false);
  else if (mode == "rows_capability") rows_capability_tests();
  else if (mode == "rows_phase") rows_phase_tests();
  else if (mode == "origin_exact") origin_tests(true);
  else if (mode == "origin_ordinary") origin_tests(false);
  else if (mode == "origin_capability") origin_capability_tests();
  else { std::puts("usage: genesis_mod_overlay_script_tests bitmap|refusal"); return 2; }
  if (failures != 0) { std::printf("genesis_mod_overlay_script_tests %s: %d FAILED\n", mode.c_str(), failures); return 1; }
  std::printf("genesis_mod_overlay_script_tests %s: OK\n", mode.c_str());
  return 0;
}
