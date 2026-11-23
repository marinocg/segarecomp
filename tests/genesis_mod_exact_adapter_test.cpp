// SEG-152-T003 (ADR 0320): the viewer's scheduled-mod adapter (platforms/genesis/viewer/mod_capture.cpp, compiled here with -DSEGARECOMP_MOD_NATIVE -DSEGARECOMP_MOD_VIEWER exactly as a built program is) drives
// an exact-frame provider from the producer seam. The test calls the compatibility table's `prepare` as the producer would, with synthetic GenesisCompatFrames, and reads back what the provider's output
// presents: every contribution (overlay colour, scenery word, placement, scene) must be a function of the frame it was produced for and of no other. No ROM, no SDL, no wall clock.
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
typedef struct ArtistState {
  unsigned calls;
  uint64_t last_frame;
  uint32_t last_side, margin_seen, view_seen;
  uint32_t last_requested, last_flags;
  int fault_next;
  int decline_placement;
} ArtistState;
extern ArtistState g_artist;
}

namespace {
int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) { std::printf("FAIL: %s\n", message.c_str()); ++failures; }
}

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

struct Frame {
  std::array<uint8_t, 65536> ram{}, vram{};
  std::array<GenesisCompatLine, 224> lines{};
  GenesisCompatFrame cf{};
  void make(uint8_t tag, unsigned side, unsigned requested) {
    ram.fill(tag);
    vram.fill(0);
    vram[0] = tag;
    vram[1] = static_cast<uint8_t>(tag ^ 0x5A);
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

struct Presented {
  GenesisCompatDecision dec{};
  bool drawn = false;
  uint8_t px[4] = {0, 0, 0, 0};
  int word_status = -1, outside_status = -1;
  uint16_t word = 0;
};

// Calls the table exactly like vdp_render.c does for one produced frame.
Presented produce(const GenesisNativeCompat *table, void *state, Frame &f) {
  static std::vector<uint8_t> overlay(GENESIS_COMPAT_OVERLAY_BYTES), layers(GENESIS_COMPAT_LAYER_BYTES);
  static GenesisVirtualSpriteSet sprites;
  Presented p;
  table->prepare(state, &f.cf, &p.dec);
  if ((p.dec.accepted & GENESIS_COMPAT_HUD) != 0U) {
    std::fill(overlay.begin(), overlay.end(), 0);
    GenesisCompatOverlay out{};
    out.rgba = overlay.data(); out.capacity = overlay.size(); out.layer_rgba = layers.data(); out.layer_capacity = layers.size(); out.vsprites = &sprites;
    p.drawn = table->hud(state, &out) != 0;
    std::memcpy(p.px, overlay.data(), 4U);
  }
  if ((p.dec.accepted & GENESIS_COMPAT_SCENERY) != 0U) {
    p.word_status = table->scenery_word(state, 0U, 3, -4, 0, 0, &p.word);       // inside the provider's window (plane cell (-1, 0))
    uint16_t other = 0;
    p.outside_status = table->scenery_word(state, 0U, 3, -20, 0, 0, &other);    // plane cell (-3, 0): not answered by the window
  }
  return p;
}
}  // namespace

int main() {
  std::unique_ptr<GenesisRuntime> rt{new GenesisRuntime};
  std::memset(static_cast<void *>(rt.get()), 0, sizeof(*rt));
  rt->pc = 0x200U; rt->sr = 0x2700U; rt->devices.vdp.registers[1] = 0x44; rt->devices.vdp.registers[12] = 0x81;
  auto capture = std::make_unique<GenesisRasterCapture>();
  std::memset(static_cast<void *>(capture.get()), 0, sizeof(*capture));
  auto ext = std::make_unique<GenesisFrameExtension>();
  std::memset(static_cast<void *>(ext.get()), 0, sizeof(*ext));
  ext->extension_shown = 1U;
  capture->extension = ext.get();
  rt->live_raster = capture.get();
  check(segarecomp_capture_mod_begin(rt.get()) == 0, "the exact-frame provider attaches through the viewer adapter");
  GenesisModScheduler *s = nullptr;
  void *state = nullptr;
  const GenesisNativeCompat *table = segarecomp_patch_provider(&state);
  check(table != nullptr, "the adapter publishes the compatibility table");
  (void)s;
  const unsigned want = GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY;
  auto frame = std::make_unique<Frame>();

  run_frames(*rt, 3U);
  check(g_artist.calls == 0U, "the ordinary after-snapshot render never runs for an exact-frame provider (the guest ran three frames)");

  // ---- frame exactness: the output of each produced frame is that frame's, never another's ----
  frame->make(0x11U, 96U, want);
  Presented a = produce(table, state, *frame);
  check(g_artist.calls == 1U && g_artist.view_seen == 1U && g_artist.last_frame + 1U == rt->virtual_frame_count, "one callback for the produced frame; host->frame is the produced frame");
  check(a.dec.accepted == want && a.drawn && a.px[0] == 0x11U && a.px[1] == (0x11U ^ 0x5AU) && a.px[2] == 0U && a.px[3] == 0xFFU, "the overlay colour is the one computed from THIS frame's view");
  check(a.word_status == 1 && a.word == (0x2000U | 0x11U) && a.outside_status != 1 && a.outside_status != GENESIS_COMPAT_WORD_GENERIC, "the scenery word comes from THIS frame's window; a cell outside it is a failed answer, never a guess");
  check(a.dec.has_left_px == 1U && a.dec.left_px == 48 && g_artist.margin_seen == 96U && g_artist.last_side == 96U, "the placement is bound to this frame's planned side, and view_margin equals the view's side");
  check(std::string(a.dec.scene) == "exact", "the scene token is this frame's");
  check(g_artist.last_requested == want && g_artist.last_flags == 0U, "the view carries the requested components; a frame the test marks non-journaled keeps flags clear");

  frame->make(0x22U, 96U, want);
  Presented b = produce(table, state, *frame);
  check(g_artist.calls == 2U && b.px[0] == 0x22U && b.word == (0x2000U | 0x22U), "the next frame shows its own colour and word, nothing of the previous frame");

  // a frame the producer does not present (no call) leaves nothing behind: the following frame is again its own
  run_frames(*rt, 5U);
  frame->make(0x33U, 96U, want);
  Presented c = produce(table, state, *frame);
  check(g_artist.calls == 3U && c.px[0] == 0x33U && c.word == (0x2000U | 0x33U), "after a skipped producer call there are no stale pixels, windows or placement");

  // ---- a request without HUD/SCENERY is not a presentation: no callback ----
  frame->make(0x44U, 96U, GENESIS_COMPAT_SPRITES);
  Presented sp = produce(table, state, *frame);
  check(g_artist.calls == 3U && sp.dec.accepted == 0U, "a frame that requests neither HUD nor scenery does not call the provider");

  // ---- Original / exact 4:3: the extension is not shown: side 0, HUD honoured, no placement, no scenery served as extension ----
  ext->extension_shown = 0U;
  frame->make(0x55U, 96U, want);
  Presented o = produce(table, state, *frame);
  check(g_artist.last_side == 0U && g_artist.margin_seen == 0U && o.dec.has_left_px == 0U && (o.dec.accepted & GENESIS_COMPAT_HUD) != 0U && o.px[0] == 0x55U, "Original: the view reports side 0, the HUD is honoured, no placement is requested");
  ext->extension_shown = 1U;

  // ---- ADR 0315: a placement computed for another margin than this frame's is never served, nor are the windows built for it; the HUD stays ----
  g_artist.decline_placement = 1;
  frame->make(0x66U, 96U, want);
  Presented m = produce(table, state, *frame);
  check(m.dec.has_left_px == 0U && (m.dec.accepted & GENESIS_COMPAT_SCENERY) == 0U && (m.dec.accepted & GENESIS_COMPAT_HUD) != 0U && m.px[0] == 0x66U, "a placement for another margin is withheld with its windows (centred layout); the HUD is independent of the view");
  g_artist.decline_placement = 0;

  // ---- aspect change: the next frame is produced with another side and the provider sees exactly that side ----
  frame->make(0x77U, 40U, want);
  Presented w = produce(table, state, *frame);
  check(g_artist.last_side == 40U && g_artist.margin_seen == 40U && w.dec.has_left_px == 1U && w.dec.left_px == 20 && w.px[0] == 0x77U, "after an aspect change the view, view_margin and the served placement are for the new side only");

  // ---- a genuine fault: nothing staged is served, the mod is disabled, the guest and every later frame stay generic ----
  const unsigned calls = g_artist.calls;
  g_artist.fault_next = 1;
  frame->make(0x88U, 96U, want);
  Presented f = produce(table, state, *frame);
  check(g_artist.calls == calls + 1U && f.dec.accepted == 0U && f.dec.reason == GENESIS_COMPAT_FAULT, "a callback fault discards its output and reports FAULT");
  frame->make(0x99U, 96U, want);
  Presented after = produce(table, state, *frame);
  check(g_artist.calls == calls + 1U && after.dec.accepted == 0U && after.dec.reason == GENESIS_COMPAT_FAULT, "a faulted mod presents nothing and is not called again");
  run_frames(*rt, 9U);
  check(rt->virtual_frame_count >= 9U, "the guest continues");
  segarecomp_capture_mod_end(rt.get());
  if (failures != 0) { std::printf("genesis_mod_exact_adapter_tests: %d FAILED\n", failures); return 1; }
  std::puts("genesis_mod_exact_adapter_tests: OK");
  return 0;
}
