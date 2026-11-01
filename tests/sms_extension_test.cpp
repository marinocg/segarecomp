// SEG-121 (ADR 0264) adds the Level 3 sprite spill (see the second half of this file): the same independent-reference approach, written from the SMS sprite rules.
// SEG-120: Master System Level 2 background extension (ADR 0263), renderer level. The producer runs inside sms_render_line, so every case injects
// VDP state line by line (the scanline seam) and checks the published extension against an INDEPENDENT reference sampler written here from the Mode 4
// rules; the production sampling code is never called. Native output is compared with an extension-off run of the same scenario.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "sms_render.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& m) {
  if (!ok) {
    std::printf("FAIL: %s\n", m.c_str());
    ++failures;
  }
}

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

// ---- independent reference (Mode 4 background only; screen x may be negative or >= 256) ----
struct RefState {
  const SmsVdp* v;
  unsigned vscroll;  // R9 latched at line 0
  unsigned height;
};
unsigned ref_pattern_pixel(const SmsVdp& v, unsigned pattern, unsigned row, unsigned col) {
  unsigned c = 0;
  for (unsigned plane = 0; plane < 4; ++plane) {
    const uint8_t b = v.vram[(pattern * 32 + row * 4 + plane) % SMS_VDP_VRAM_SIZE];
    c |= static_cast<unsigned>((b >> (7 - col)) & 1) << plane;
  }
  return c;
}
struct BgInfo {
  unsigned value;   // 6-bit CRAM value
  unsigned color;   // pattern colour index (0 = transparent)
  bool prio;        // name-table priority bit
  bool gap;         // backdrop: display disabled or the fine-scroll gap
};
BgInfo ref_bg_info(const RefState& st, int xs, unsigned line) {
  const SmsVdp& v = *st.v;
  const unsigned r0 = v.reg[0];
  const unsigned backdrop = v.cram[16 + (v.reg[7] & 15)] & 63u;
  if ((v.reg[1] & 0x40) == 0) return {backdrop, 0, false, true};
  const bool tall = st.height == 224;
  const unsigned name_base = tall ? (((v.reg[2] & 0x0Cu) << 10) | 0x700u) : ((v.reg[2] & 0x0Eu) << 10);
  const int hscroll = ((r0 & 0x40) != 0 && line < 16) ? 0 : v.reg[8];
  const int fine = hscroll & 7;
  const int xmod = ((xs % 256) + 256) % 256;
  if (xmod < fine) return {backdrop, 0, false, true};
  const int src = (((xs - hscroll) % 256) + 256) % 256;  // direct signed evaluation, not x & 255 first
  const unsigned ry = tall ? ((line + st.vscroll) & 255u) : ((line + st.vscroll) % 224u);
  const unsigned entry_addr = (name_base + (ry >> 3) * 64u + static_cast<unsigned>(src >> 3) * 2u) % SMS_VDP_VRAM_SIZE;
  const unsigned entry = v.vram[entry_addr] | (static_cast<unsigned>(v.vram[entry_addr + 1]) << 8);
  unsigned prow = ry & 7u, pcol = static_cast<unsigned>(src) & 7u;
  if (entry & 0x400u) prow = 7u - prow;
  if (entry & 0x200u) pcol = 7u - pcol;
  const unsigned color = ref_pattern_pixel(v, entry & 0x1FFu, prow, pcol);
  const unsigned pal = (entry >> 11) & 1u;
  return {static_cast<unsigned>(v.cram[pal * 16 + color] & 63u), color, ((entry >> 12) & 1u) != 0, false};
}
// the 6-bit CRAM value of the background at (signed screen x, line); the periodic fine-scroll gap is the backdrop
unsigned ref_bg(const RefState& st, int xs, unsigned line) { return ref_bg_info(st, xs, line).value; }

// ---- SEG-121: the sprite spill reference (the SMS sprite rules written out; X is an 8-bit field, the shift bit subtracts 8) ----
// Returns, for output column xs (any integer), the sprite colour index (0 = none) drawn there on `line` by the first 8 sprites of the line in SAT order.
struct SpriteRef {
  int sx[8];
  unsigned row[8], tile[8];
  unsigned count = 0;
  unsigned zoom = 1, width = 8;
  bool overflow = false;
};
SpriteRef ref_select_sprites(const SmsVdp& v, unsigned line, unsigned height) {
  SpriteRef r;
  const unsigned sat = (static_cast<unsigned>(v.reg[5] & 0x7E)) << 7;
  const bool tall16 = (v.reg[1] & 0x02) != 0;
  r.zoom = (v.reg[1] & 0x01) ? 2u : 1u;
  r.width = 8 * r.zoom;
  const unsigned h = (tall16 ? 16u : 8u) * r.zoom;
  for (unsigned i = 0; i < 64; ++i) {
    const unsigned y = v.vram[sat + i];
    if (height == 192 && y == 0xD0) break;
    const unsigned rel = (line + 256 - (y + 1)) % 256;
    if (rel >= h) continue;
    if (r.count == 8) { r.overflow = true; break; }
    const unsigned base = sat + 0x80 + 2 * i;
    r.sx[r.count] = static_cast<int>(v.vram[base]) - ((v.reg[0] & 0x08) ? 8 : 0);
    unsigned tile = v.vram[base + 1];
    if (tall16) tile &= 0xFE;
    r.tile[r.count] = tile + ((v.reg[6] & 0x04) ? 256 : 0);
    r.row[r.count] = rel / r.zoom;
    ++r.count;
  }
  return r;
}
unsigned ref_sprite_color(const SmsVdp& v, const SpriteRef& r, int xs) {
  for (unsigned k = 0; k < r.count; ++k) {
    if (xs < r.sx[k] || xs >= r.sx[k] + static_cast<int>(r.width)) continue;
    const unsigned col = static_cast<unsigned>(xs - r.sx[k]) / r.zoom;
    const unsigned c = ref_pattern_pixel(v, r.tile[k] + (r.row[k] >> 3), r.row[k] & 7u, col);
    if (c != 0) return c;
  }
  return 0;
}

bool g_l3 = false;           // the scenario requests the Level 3 sprite spill
unsigned g_ref_pixels = 0;   // visible spilled sprite pixels the reference expects in the frame in progress

struct Rig {
  SmsVdp vdp;
  SmsRenderer r;
  SmsFrameRecord records[8];
  SmsExtension ext;
};
Rig* make_rig(bool attach) {
  Rig* g = new Rig();
  std::memset(g, 0, sizeof *g);
  sms_vdp_reset(&g->vdp);
  sms_renderer_init(&g->r, &g->vdp, g->records, 8);
  if (attach) g->r.ext = &g->ext;
  return g;
}

void fill_scene(SmsVdp& v, Rng& rng, bool tall, bool sprites) {
  for (auto& b : v.vram) b = static_cast<uint8_t>(rng.next());
  for (unsigned i = 0; i < 32; ++i) v.cram[i] = static_cast<uint8_t>(rng.next() & 63u);
  v.reg[0] = 0x04 | (tall ? 0x02 : 0);
  v.reg[1] = 0x40 | (tall ? 0x10 : 0);
  v.reg[2] = tall ? 0x0E : 0x0E;
  v.reg[5] = 0xFF;  // SAT at 0x3F00
  v.reg[6] = 0x00;
  v.reg[7] = static_cast<uint8_t>(rng.next() & 15);
  v.reg[8] = 0;
  v.reg[9] = 0;
  // keep the sprite attribute table valid-looking: either on (random) or parked past the screen
  if (!sprites)
    for (unsigned i = 0; i < 64; ++i) v.vram[0x3F00 + i] = 0xD0;
}

using Hook = std::function<void(SmsVdp&, unsigned /*line*/)>;

struct FrameOut {
  std::vector<uint8_t> native;
  uint8_t sha[32];
  uint8_t overflow, collision;
  SmsExtResult res;
  std::vector<uint8_t> left, right;  // [line][side]
};

// Runs one frame of the scenario and (when attached) checks the extension against the reference line by line.
bool run_frame(Rig& g, const Hook& hook, uint64_t frame, std::vector<std::vector<unsigned>>* want_left, std::vector<std::vector<unsigned>>* want_right,
               uint32_t req192, uint32_t req224) {
  for (unsigned line = 0; line < 224; ++line) {
    if (hook) hook(g.vdp, line);
    if (sms_vdp_mode(&g.vdp) == SMS_VDP_MODE_UNSUPPORTED) {
      sms_render_line(&g.r, &g.vdp, line, frame);
      return false;
    }
    if (line == 0) {
      // the reference snapshot of R9 / height happens at line 0 exactly like the renderer
    }
    const uint64_t before = g.r.frames_completed;
    const unsigned height = static_cast<unsigned>(sms_vdp_mode(&g.vdp));
    if (line >= height) break;
    RefState st{&g.vdp, line == 0 ? static_cast<unsigned>(g.vdp.reg[9]) : g.r.vscroll, height};
    if (line == 0) {
      if (want_left) want_left->clear();
      if (want_right) want_right->clear();
      g_ref_pixels = 0;
    }
    const unsigned side_hint = height == 224 ? req224 : req192;
    if (want_left && side_hint) {
      std::vector<unsigned> l, r;
      const SpriteRef sprites = ref_select_sprites(g.vdp, line, height);
      for (unsigned i = 0; i < side_hint; ++i) {
        const int xl = -static_cast<int>(side_hint) + static_cast<int>(i), xr = 256 + static_cast<int>(i);
        BgInfo bl = ref_bg_info(st, xl, line), br = ref_bg_info(st, xr, line);
        if (g_l3 && (g.vdp.reg[1] & 0x40)) {  // the native line draws sprites only while the display is on
          for (int side = 0; side < 2; ++side) {
            BgInfo& b = side == 0 ? bl : br;
            const unsigned c = ref_sprite_color(g.vdp, sprites, side == 0 ? xl : xr);
            if (c != 0 && !(b.prio && b.color != 0 && !b.gap)) { b.value = g.vdp.cram[16 + c] & 63u; ++g_ref_pixels; }
          }
        }
        l.push_back(bl.value);
        r.push_back(br.value);
      }
      want_left->push_back(l);
      want_right->push_back(r);
    }
    sms_render_line(&g.r, &g.vdp, line, frame);
    if (g.r.frames_completed != before) return true;
  }
  return false;
}

struct Result {
  bool completed;
  SmsExtResult res;
  std::vector<uint8_t> native;
  uint8_t sha[32];
  uint8_t overflow, collision;
  bool lines_match = true;
  unsigned mismatches = 0;
  unsigned ref_pixels = 0;
};

// Plays `frames` frames of a scenario; hook(frame, vdp, line). Returns per-frame results of the last frame.
struct Scenario {
  std::function<void(SmsVdp&, Rng&)> setup;
  std::function<void(unsigned frame, SmsVdp&, unsigned line)> hook;
  unsigned frames = 1;
  uint32_t seed = 1;
};

Result play(const Scenario& sc, bool attach, uint32_t req192, uint32_t req224, bool verify_ref, std::function<void(Rig&, unsigned)> between = nullptr) {
  Rig* g = make_rig(attach);
  Rng rng(sc.seed);
  sc.setup(g->vdp, rng);
  Result out{};
  for (unsigned f = 0; f < sc.frames; ++f) {
    if (attach) {
      g->ext.request_side_192 = req192;
      g->ext.request_side_224 = req224;
      g->ext.request_sprites = g_l3 ? 1 : 0;
    }
    if (between) between(*g, f);
    std::vector<std::vector<unsigned>> wl, wr;
    // the hook is applied before the reference of a line is computed, so the reference sees the per-line state
    auto hook = [&](SmsVdp& v, unsigned line) {
      if (sc.hook) sc.hook(f, v, line);
    };
    out.completed = run_frame(*g, hook, f, &wl, &wr, attach && verify_ref ? req192 : 0, attach && verify_ref ? req224 : 0);
    if (attach && verify_ref && out.completed && g->ext.result.cls == SMS_EXT_WRAPPED) {
      const SmsExtResult& r = g->ext.result;
      for (unsigned y = 0; y < r.height; ++y) {
        for (unsigned i = 0; i < r.side; ++i) {
          if (g->ext.left[y][i] != wl[y][i] || g->ext.right[y][i] != wr[y][i]) {
            ++out.mismatches;
            out.lines_match = false;
          }
        }
      }
    }
    if (out.completed) {
      out.ref_pixels = g_ref_pixels;
      out.res = g->ext.result;
      out.native.assign(g->r.last, g->r.last + static_cast<size_t>(SMS_FB_WIDTH) * g->r.last_height);
      std::memcpy(out.sha, g->r.records[g->r.record_count - 1].sha256, 32);
      out.overflow = g->vdp.sprite_overflow;
      out.collision = g->vdp.sprite_collision;
    }
  }
  delete g;
  return out;
}

bool same_native(const Result& a, const Result& b) {
  return a.completed == b.completed && a.native == b.native && std::memcmp(a.sha, b.sha, 32) == 0 && a.overflow == b.overflow && a.collision == b.collision;
}

void expect_wrapped(const Scenario& sc, unsigned height, const char* name) {
  const uint32_t side = height == 224 ? 71 : 42;
  const Result on = play(sc, true, side, side, true);
  const Result off = play(sc, false, 0, 0, false);
  check(on.completed, std::string(name) + ": frame completes");
  check(on.res.cls == SMS_EXT_WRAPPED, std::string(name) + ": WRAPPED, got " + sms_ext_class_name(on.res.cls) + "/" + sms_ext_reason_name(on.res.reason));
  check(on.res.side == side && on.res.height == height, std::string(name) + ": geometry");
  check(on.lines_match, std::string(name) + ": extension equals the independent reference (mismatches " + std::to_string(on.mismatches) + ")");
  check(same_native(on, off), std::string(name) + ": native frame, SHA-256 and sprite flags identical to the extension-off run");
}

Scenario base_scene(bool tall, bool sprites, uint32_t seed) {
  Scenario s;
  s.seed = seed;
  s.setup = [=](SmsVdp& v, Rng& r) { fill_scene(v, r, tall, sprites); };
  return s;
}

void test_wrap_and_scroll_values() {
  const unsigned hs[] = {0, 1, 7, 8, 9, 100, 128, 200, 255};
  for (unsigned h : hs) {
    for (int tall = 0; tall < 2; ++tall) {
      Scenario s = base_scene(tall, true, 1000 + h);
      s.setup = [=](SmsVdp& v, Rng& r) {
        fill_scene(v, r, tall, true);
        v.reg[8] = static_cast<uint8_t>(h);
        v.reg[9] = static_cast<uint8_t>(h * 3);
      };
      expect_wrapped(s, tall ? 224 : 192, ("hscroll " + std::to_string(h) + (tall ? " 224" : " 192")).c_str());
    }
  }
}

void test_provenance_is_pure_repetition() {
  // The central claim: every extension sample equals the NATIVE background sample at x & 255 (evaluated by the reference at an in-viewport x),
  // for random state, i.e. no sample is additional scenery. This is why no DISTINCT class can be produced in Mode 4.
  Rng rng(77);
  for (int iter = 0; iter < 40; ++iter) {
    Rig* g = make_rig(true);
    fill_scene(g->vdp, rng, iter & 1, false);
    g->vdp.reg[8] = static_cast<uint8_t>(rng.next());
    g->vdp.reg[9] = static_cast<uint8_t>(rng.next());
    g->vdp.reg[0] |= (iter % 3 == 0) ? 0x40 : 0;  // top-row hscroll lock sometimes
    g->ext.request_side_192 = g->ext.request_side_224 = 60;
    for (unsigned line = 0; line < (iter & 1 ? 224u : 192u); ++line) sms_render_line(&g->r, &g->vdp, line, 0);
    check(g->ext.result.cls == SMS_EXT_WRAPPED, "provenance: wrapped");
    const unsigned h = g->ext.result.height;
    RefState st{&g->vdp, g->r.vscroll, h};
    for (unsigned y = 0; y < h; ++y)
      for (unsigned i = 0; i < 60; ++i) {
        // line-dependent registers (R0 lock) are constant here, so the final state is every line's state
        const unsigned native_l = ref_bg(st, static_cast<int>(256 - 60 + i), y), native_r = ref_bg(st, static_cast<int>(i), y);
        if (g->ext.left[y][i] != native_l || g->ext.right[y][i] != native_r) check(false, "provenance: extension sample is not the native sample at x & 255");
      }
    delete g;
  }
  check(SMS_EXT_DISTINCT != SMS_EXT_WRAPPED, "classes are distinct values");
}

void test_scroll_locks_and_mask() {
  {  // top two tile rows: horizontal scroll lock is a function of the screen row, so the extension honours it per line
    Scenario s = base_scene(false, false, 5);
    s.setup = [](SmsVdp& v, Rng& r) {
      fill_scene(v, r, false, false);
      v.reg[0] |= 0x40;
      v.reg[8] = 77;
    };
    expect_wrapped(s, 192, "hscroll lock rows 0-1");
  }
  for (unsigned bit : {0x80u, 0x20u}) {  // vertical scroll lock / left-column blank: undefined off screen -> UNAVAILABLE with a typed reason
    Scenario s = base_scene(false, true, 6);
    s.setup = [=](SmsVdp& v, Rng& r) {
      fill_scene(v, r, false, true);
      v.reg[0] = static_cast<uint8_t>(v.reg[0] | bit);
      v.reg[8] = 13;
    };
    const Result on = play(s, true, 42, 42, false), off = play(s, false, 0, 0, false);
    check(on.res.cls == SMS_EXT_UNAVAILABLE, "lock/mask frame is UNAVAILABLE");
    check(on.res.reason == (bit == 0x80 ? SMS_EXT_REASON_VSCROLL_LOCK : SMS_EXT_REASON_LEFT_MASK), "typed reason");
    check(same_native(on, off), "native unchanged under an unavailable extension");
  }
  {  // a mask raised mid-frame at line 100 poisons the whole frame (never a half-extended picture)
    Scenario s = base_scene(false, false, 7);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) { v.reg[0] = static_cast<uint8_t>(line >= 100 ? 0x24 : 0x04); };
    const Result on = play(s, true, 42, 42, false);
    check(on.res.cls == SMS_EXT_UNAVAILABLE && on.res.reason == SMS_EXT_REASON_LEFT_MASK, "mid-frame mask -> UNAVAILABLE");
  }
  {  // lock/mask do not matter while the display is disabled (the whole line is the backdrop)
    Scenario s = base_scene(false, false, 8);
    s.setup = [](SmsVdp& v, Rng& r) {
      fill_scene(v, r, false, false);
      v.reg[0] |= 0xA0;
      v.reg[1] &= static_cast<uint8_t>(~0x40);
    };
    expect_wrapped(s, 192, "display disabled with lock+mask bits");
  }
}

void test_modes() {
  {  // 240 lines (and other unsupported shapes): nothing rendered, nothing extended, typed counter
    Scenario s = base_scene(false, false, 9);
    s.setup = [](SmsVdp& v, Rng& r) {
      fill_scene(v, r, false, false);
      v.reg[0] = 0x06;
      v.reg[1] = 0x48 | 0x40;  // M4 + M2 + M3 without M1 = 240 lines
    };
    Rig* g = make_rig(true);
    Rng rng(9);
    s.setup(g->vdp, rng);
    check(sms_vdp_mode(&g->vdp) == SMS_VDP_MODE_UNSUPPORTED, "240-line mode is unsupported");
    g->ext.request_side_192 = g->ext.request_side_224 = 42;
    for (unsigned line = 0; line < 240; ++line) sms_render_line(&g->r, &g->vdp, line, 0);
    check(g->r.frames_completed == 0 && g->r.last_height == 0, "240-line mode renders no frame");
    check(g->ext.result.cls == SMS_EXT_NONE && g->ext.unsupported_frames == 1, "240-line mode: no extension, one typed unsupported frame");
    delete g;
  }
  {  // a 192 -> 224 height change takes effect at the frame boundary with the matching side
    Scenario s = base_scene(false, false, 10);
    s.frames = 3;
    s.hook = [](unsigned f, SmsVdp& v, unsigned line) {
      if (line == 0) {
        v.reg[0] = f == 1 ? 0x06 : 0x04;
        v.reg[1] = f == 1 ? 0x50 : 0x40;
      }
    };
    const Result on = play(s, true, 42, 71, true);
    check(on.res.cls == SMS_EXT_WRAPPED && on.res.height == 192 && on.res.side == 42, "after the 224 frame the next 192 frame uses side 42");
    check(on.lines_match, "height change fixture matches the reference");
  }
}

void test_midframe_changes() {
  {  // scroll, CRAM, nametable and pattern changes between lines: each line is produced from its own state
    Scenario s = base_scene(false, true, 11);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) {
      if (line % 7 == 0) v.reg[8] = static_cast<uint8_t>(line * 5 + 3);
      if (line % 11 == 0) v.cram[(line / 11) % 32] = static_cast<uint8_t>((line * 13) & 63);
      if (line == 40) v.vram[0x3800 + 2 * 3] ^= 0xFF;     // a name-table entry byte
      if (line == 90) v.vram[(v.reg[2] & 0x0E) * 0x400 + 64 * 2 + 4] ^= 0x7E;
      if (line == 120) for (unsigned i = 0; i < 64; ++i) v.vram[i * 32 + 5] ^= 0x5A;  // pattern data
      if (line == 150) v.reg[7] = 3;
    };
    expect_wrapped(s, 192, "mid-frame scroll/CRAM/nametable/pattern/backdrop");
  }
  {  // vertical scroll R9 is frame-latched: a mid-frame write must not reach the extension either
    Scenario s = base_scene(false, false, 12);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) { if (line == 60) v.reg[9] = 0x55; };
    expect_wrapped(s, 192, "mid-frame R9 write ignored until the next frame");
  }
  {  // display disabled for a band of lines
    Scenario s = base_scene(false, true, 13);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) { v.reg[1] = static_cast<uint8_t>(line >= 50 && line < 70 ? 0x00 : 0x40); };
    expect_wrapped(s, 192, "display disabled band");
  }
}

void test_request_latching() {
  Scenario s = base_scene(false, true, 14);
  s.frames = 4;
  const uint32_t reqs[] = {0, 42, 42, 0};
  Rig* g = make_rig(true);
  Rng rng(14);
  s.setup(g->vdp, rng);
  SmsExtClass seen[4];
  for (unsigned f = 0; f < 4; ++f) {
    g->ext.request_side_192 = reqs[f];
    for (unsigned line = 0; line < 192; ++line) {
      if (f == 1 && line == 10) g->ext.request_side_192 = 0;    // a mid-frame change must not touch the frame in progress
      if (f == 0 && line == 10) g->ext.request_side_192 = 42;   // ... in either direction
      sms_render_line(&g->r, &g->vdp, line, f);
    }
    seen[f] = g->ext.result.cls;
    check(g->ext.result.sequence == g->r.frames_completed, "result carries the frame identity");
  }
  check(seen[0] == SMS_EXT_NONE && seen[1] == SMS_EXT_WRAPPED && seen[2] == SMS_EXT_WRAPPED && seen[3] == SMS_EXT_NONE,
        "request is latched at line 0 of each frame (mid-frame changes apply to the next frame)");
  delete g;
}

void test_geometry_and_reset() {
  Scenario s = base_scene(false, false, 15);
  const Result big = play(s, true, SMS_EXT_MAX_SIDE + 1, SMS_EXT_MAX_SIDE + 1, false);
  check(big.res.cls == SMS_EXT_UNAVAILABLE && big.res.reason == SMS_EXT_REASON_GEOMETRY, "oversized request is a typed GEOMETRY failure");
  const Result max = play(s, true, SMS_EXT_MAX_SIDE, SMS_EXT_MAX_SIDE, true);
  check(max.res.cls == SMS_EXT_WRAPPED && max.lines_match, "the maximum side is produced exactly");
  const Result one = play(s, true, 1, 1, true);
  check(one.res.cls == SMS_EXT_WRAPPED && one.lines_match, "side 1");
  Rig* g = make_rig(true);
  g->ext.result.cls = SMS_EXT_WRAPPED;
  g->ext.request_side_192 = 9;
  sms_extension_reset(&g->ext);
  check(g->ext.result.cls == SMS_EXT_NONE && g->ext.request_side_192 == 9, "reset clears the result and keeps the request");
  sms_extension_reset(nullptr);
  delete g;
  check(std::string(sms_ext_class_name(SMS_EXT_WRAPPED)) == "wrapped" && std::string(sms_ext_reason_name(SMS_EXT_REASON_LEFT_MASK)) == "left_column_mask",
        "stable names");
}

void test_no_stale_rows() {
  // A good frame followed by a frame with an unextendable line: the second result is UNAVAILABLE and the previously published pixels are not
  // claimed by it (class != WRAPPED, side 0), so a host cannot present them.
  Rig* g = make_rig(true);
  Rng rng(16);
  fill_scene(g->vdp, rng, false, false);
  g->ext.request_side_192 = 42;
  for (unsigned line = 0; line < 192; ++line) sms_render_line(&g->r, &g->vdp, line, 0);
  check(g->ext.result.cls == SMS_EXT_WRAPPED, "good frame");
  for (unsigned line = 0; line < 192; ++line) {
    if (line == 100) g->vdp.reg[0] |= 0x80;
    sms_render_line(&g->r, &g->vdp, line, 1);
  }
  check(g->ext.result.cls == SMS_EXT_UNAVAILABLE && g->ext.result.side == 0 && g->ext.result.sequence == 2, "unextendable row -> frame UNAVAILABLE, new identity");
  delete g;
}

void test_fuzz_native_invariance() {
  // Random registers/VRAM/CRAM and mid-frame writes, sprites on: the native output, SHA-256 and status flags never depend on the extension.
  Rng rng(2026);
  for (int iter = 0; iter < 60; ++iter) {
    const uint32_t seed = rng.next();
    Scenario s;
    s.seed = seed;
    s.frames = 3;
    s.setup = [=](SmsVdp& v, Rng& r) {
      for (auto& b : v.vram) b = static_cast<uint8_t>(r.next());
      for (auto& c : v.cram) c = static_cast<uint8_t>(r.next() & 63);
      for (unsigned i = 0; i < 11; ++i) v.reg[i] = static_cast<uint8_t>(r.next());
      v.reg[0] |= 0x04;  // mostly Mode 4
      v.reg[1] |= 0x40;
    };
    s.hook = [=](unsigned f, SmsVdp& v, unsigned line) {
      Rng r(seed + f * 977 + line);
      if (r.next() % 9 == 0) v.reg[r.next() % 11] = static_cast<uint8_t>(r.next());
      if (r.next() % 9 == 0) v.vram[r.next() % SMS_VDP_VRAM_SIZE] = static_cast<uint8_t>(r.next());
      if (r.next() % 17 == 0) v.cram[r.next() % 32] = static_cast<uint8_t>(r.next() & 63);
    };
    const Result on = play(s, true, 42 + seed % 30, 71, false), off = play(s, false, 0, 0, false);
    check(same_native(on, off), "fuzz " + std::to_string(iter) + ": native identical with and without the extension");
    check(on.res.cls == SMS_EXT_NONE || on.res.cls == SMS_EXT_WRAPPED || on.res.cls == SMS_EXT_UNAVAILABLE, "fuzz: valid class");
    check(on.res.cls != SMS_EXT_DISTINCT, "fuzz: never DISTINCT");
  }
}

void test_records_exclude_extension() {
  // The frame record list and the SHA-256 contain only the native frame: identical record counts and bytes with the extension on.
  Scenario s = base_scene(false, true, 17);
  s.frames = 5;
  const Result on = play(s, true, 42, 42, false), off = play(s, false, 0, 0, false);
  check(same_native(on, off), "five frames: records identical");
  static_assert(sizeof(SmsFrameRecord) == 8 + 4 + 4 + 32, "the frame record layout is unchanged");
}


// ---- SEG-121: Level 3 sprite spill ------------------------------------------------------------------------------------------
struct SprSpec { unsigned x, y, tile; };

// A scene with random patterns / name table / palettes and an authored sprite table: entry k = specs[k]; the rest is parked (the 0xD0 terminator in
// 192-line mode; below the picture in 224-line mode, where there is no terminator).
Scenario sprite_scene(bool tall, std::vector<SprSpec> specs, unsigned r0_extra, unsigned r1_extra, uint32_t seed) {
  Scenario s;
  s.seed = seed;
  s.setup = [=](SmsVdp& v, Rng& r) {
    fill_scene(v, r, tall, true);
    for (unsigned i = 0; i < 64; ++i) v.vram[0x3F00 + i] = tall ? 0xF0 : 0xD0;
    for (unsigned k = 0; k < specs.size(); ++k) {
      v.vram[0x3F00 + k] = static_cast<uint8_t>(specs[k].y);
      v.vram[0x3F80 + 2 * k] = static_cast<uint8_t>(specs[k].x);
      v.vram[0x3F80 + 2 * k + 1] = static_cast<uint8_t>(specs[k].tile);
    }
    v.reg[0] = static_cast<uint8_t>(v.reg[0] | r0_extra);
    v.reg[1] = static_cast<uint8_t>(v.reg[1] | r1_extra);
  };
  return s;
}

// Level 3 against the reference; `min_pixels` > 0 additionally requires visible spilled pixels (the scene is built to have them).
Result expect_l3(const Scenario& sc, unsigned height, const char* name, unsigned min_pixels, bool may_spill = false) {
  const uint32_t side = height == 224 ? 71 : 42;
  g_l3 = true;
  const Result on = play(sc, true, side, side, true);
  g_l3 = false;
  const Result off = play(sc, false, 0, 0, false);
  const Result l2 = play(sc, true, side, side, true);
  check(on.completed, std::string(name) + ": frame completes");
  check(on.res.cls == SMS_EXT_WRAPPED && on.res.sprites == 1, std::string(name) + ": a WRAPPED Level 3 frame");
  check(on.lines_match, std::string(name) + ": extension equals the independent reference (mismatches " + std::to_string(on.mismatches) + ")");
  check(on.res.sprite_pixels == on.ref_pixels, std::string(name) + ": spilled pixel count " + std::to_string(on.res.sprite_pixels) + " vs reference " + std::to_string(on.ref_pixels));
  check(on.res.sprite_pixels >= min_pixels, std::string(name) + ": visible spill expected");
  check(min_pixels != 0 || may_spill || on.res.sprite_pixels == 0, std::string(name) + ": no spill expected");
  check(same_native(on, off), std::string(name) + ": native frame, SHA-256 and sprite flags identical to the extension-off run");
  check(l2.res.cls == SMS_EXT_WRAPPED && l2.res.sprites == 0 && l2.res.sprite_pixels == 0 && l2.lines_match, std::string(name) + ": the Level 2 request of the same scene has no sprite content");
  check(same_native(l2, off), std::string(name) + ": Level 2 native identical too");
  return on;
}

void test_l3_edges() {
  for (int tall = 0; tall < 2; ++tall) {
    const unsigned h = tall ? 224 : 192;
    const std::string t = tall ? " 224" : " 192";
    expect_l3(sprite_scene(tall, {{255, 40, 20}}, 0, 0, 301), h, ("X=255, 8 wide (7 px spill)" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{252, 40, 21}}, 0, 0, 302), h, ("X=252 crosses the right edge by 4" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{248, 40, 22}}, 0, 0, 303), h, ("X=248 ends exactly at the edge: no spill" + t).c_str(), 0);
    expect_l3(sprite_scene(tall, {{248, 40, 22}}, 0, 0x01, 304), h, ("X=248 zoomed: 8 px spill" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{255, 40, 24}}, 0, 0x01, 305), h, ("X=255 zoomed: maximum 15 px spill" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{0, 40, 23}}, 0, 0, 306), h, ("X=0 without the shift bit: no spill" + t).c_str(), 0);
    expect_l3(sprite_scene(tall, {{0, 40, 23}}, 0x08, 0, 307), h, ("X=0 with the shift bit: wholly outside on the left" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{4, 40, 25}}, 0x08, 0, 308), h, ("X=4 with the shift bit: 4 px spill" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{0, 40, 26}}, 0x08, 0x01, 309), h, ("X=0 shift + zoom: 8 px spill on the left" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{255, 40, 28}}, 0, 0x02, 310), h, ("8x16 sprite at X=255" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{255, 40, 28}}, 0, 0x03, 311), h, ("8x16 zoomed sprite at X=255" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{1, 40, 30}}, 0x08, 0x02, 312), h, ("8x16 with the shift bit near X=0" + t).c_str(), 1);
    expect_l3(sprite_scene(tall, {{100, 40, 30}, {140, 60, 31}}, 0, 0, 313), h, ("sprites wholly inside: no spill" + t).c_str(), 0);
    // sprite pattern base (R6 bit 2) is honoured by the spill as by the native line
    expect_l3(sprite_scene(tall, {{255, 40, 3}}, 0, 0, 314), h, ("X=255 from the second pattern bank" + t).c_str(), 1);
  }
  // R6 bit 2 selects tiles 256..511
  { Scenario s = sprite_scene(false, {{255, 40, 3}, {0, 90, 7}}, 0x08, 0, 315);
    auto setup = s.setup; s.setup = [=](SmsVdp& v, Rng& r) { setup(v, r); v.reg[6] = 0x04; };
    expect_l3(s, 192, "pattern bank 1 with edge sprites", 1); }
}

void test_l3_transparency_priority() {
  // a fully transparent pattern spills nothing
  { Scenario s = sprite_scene(false, {{255, 40, 50}, {0, 80, 50}}, 0x08, 0, 320);
    auto setup = s.setup; s.setup = [=](SmsVdp& v, Rng& r) { setup(v, r); for (unsigned i = 0; i < 32; ++i) v.vram[50 * 32 + i] = 0; };
    expect_l3(s, 192, "transparent pattern", 0); }
  // background priority: every name-table entry has the priority bit, so the sprite hides behind every non-zero background pixel
  { Scenario s = sprite_scene(false, {{255, 40, 51}}, 0, 0x01, 321);
    auto setup = s.setup; s.setup = [=](SmsVdp& v, Rng& r) { setup(v, r); for (unsigned a = 0x3800; a < 0x3800 + 0x700; a += 2) v.vram[a + 1] |= 0x10; };
    const Result r = expect_l3(s, 192, "all-priority background hides the spill behind non-zero pixels", 0, true);
    (void)r; }
  // no priority bit anywhere: the spill is drawn over the background wherever the sprite is opaque
  { Scenario s = sprite_scene(false, {{255, 40, 51}}, 0, 0x01, 322);
    auto setup = s.setup; s.setup = [=](SmsVdp& v, Rng& r) { setup(v, r); for (unsigned a = 0x3800; a < 0x3800 + 0x700; a += 2) v.vram[a + 1] &= 0xEF; };
    expect_l3(s, 192, "no background priority: spill drawn", 1); }
  // overlapping sprites at the edge: the earlier SAT entry wins, a transparent pixel lets the later one through
  expect_l3(sprite_scene(false, {{252, 40, 52}, {254, 42, 53}, {250, 41, 54}}, 0, 0, 323), 192, "overlapping sprites at the right edge", 1);
  expect_l3(sprite_scene(false, {{2, 40, 52}, {1, 42, 53}, {0, 41, 54}}, 0x08, 0, 324), 192, "overlapping sprites at the left edge", 1);
}

void test_l3_hardware_limits() {
  // nine sprites on one line: the ninth is dropped by the 8-per-line limit and never spills; the overflow flag is the native one
  { std::vector<SprSpec> v;
    for (unsigned k = 0; k < 8; ++k) v.push_back({100 + 3 * k, 40, 60 + k});
    v.push_back({255, 40, 70});   // the ninth sprite of the line
    Scenario s = sprite_scene(false, v, 0, 0, 330);
    const Result r = expect_l3(s, 192, "ninth sprite on a line", 0);
    check(r.overflow != 0, "ninth sprite: the native overflow flag is set"); }
  // the same eight plus an edge sprite among the first eight does spill
  { std::vector<SprSpec> v;
    for (unsigned k = 0; k < 7; ++k) v.push_back({100 + 3 * k, 40, 60 + k});
    v.push_back({255, 40, 70});
    expect_l3(sprite_scene(false, v, 0, 0, 331), 192, "eighth sprite on a line spills", 1); }
  // the 0xD0 terminator ends the walk in 192-line mode, and does not exist in 224-line mode
  { Scenario s = sprite_scene(false, {{10, 0xD0, 61}, {255, 40, 62}}, 0, 0, 333);          // entry 0 is the terminator: entry 1 is never drawn
    expect_l3(s, 192, "terminator hides the later edge sprite (192 lines)", 0);
    Scenario t = sprite_scene(true, {{10, 0xD0, 61}, {255, 40, 62}}, 0, 0, 334);           // 224-line mode: 0xD0 is an ordinary Y and the walk continues
    expect_l3(t, 224, "no terminator in 224-line mode: the later edge sprite spills", 1); }
  // sprites far down or above the picture draw nothing; a Y at the bottom wraps the visible band only
  expect_l3(sprite_scene(false, {{255, 0xB7, 61}}, 0, 0, 335), 192, "edge sprite at the last lines", 1);
  expect_l3(sprite_scene(false, {{255, 0xFF, 61}}, 0, 0, 336), 192, "edge sprite at Y = 255 (top of the picture)", 1);
  // SAT base from R5 (bit 0 and bit 7 ignored): a different table position is walked
  { Scenario s = sprite_scene(false, {{255, 40, 62}}, 0, 0, 337);
    auto setup = s.setup; s.setup = [=](SmsVdp& v, Rng& r) { setup(v, r); v.reg[5] = 0x7D; };  // 0x3E80 << ... table moves
    expect_l3(s, 192, "SAT base register with the ignored low bit", 0, true); }
}

void test_l3_bounded_spill() {
  // The spill can never reach beyond 8 columns on the left and 15 on the right, however wide the surface is.
  for (unsigned side : {20u, 80u}) {
    Rig* g = make_rig(true);
    Rng rng(340 + side);
    fill_scene(g->vdp, rng, false, true);
    for (unsigned i = 0; i < 64; ++i) g->vdp.vram[0x3F00 + i] = 0xD0;
    for (unsigned k = 0; k < 8; ++k) {                       // eight zoomed sprites at X = 255 and eight (other lines) with the shift bit at X = 0
      g->vdp.vram[0x3F00 + k] = static_cast<uint8_t>(20 + 20 * k);
      g->vdp.vram[0x3F80 + 2 * k] = 255;
      g->vdp.vram[0x3F80 + 2 * k + 1] = static_cast<uint8_t>(80 + k);
    }
    g->vdp.reg[0] |= 0x08; g->vdp.reg[1] |= 0x01;
    for (unsigned k = 0; k < 8; ++k) g->vdp.vram[0x3F80 + 2 * k] = (k & 1) ? 255 : 0;
    g->ext.request_side_192 = side; g->ext.request_side_224 = side; g->ext.request_sprites = 1;
    for (unsigned line = 0; line < 192; ++line) sms_render_line(&g->r, &g->vdp, line, 0);
    SmsRenderer* r2 = &g->r;
    (void)r2;
    check(g->ext.result.cls == SMS_EXT_WRAPPED && g->ext.result.sprites == 1 && g->ext.result.sprite_pixels > 0, "bounded spill: produced");
    Rig* l2 = make_rig(true);
    std::memcpy(&l2->vdp, &g->vdp, sizeof l2->vdp);
    l2->ext.request_side_192 = side; l2->ext.request_side_224 = side; l2->ext.request_sprites = 0;
    for (unsigned line = 0; line < 192; ++line) sms_render_line(&l2->r, &l2->vdp, line, 0);
    unsigned far_differences = 0, near_differences = 0;
    for (unsigned y = 0; y < 192; ++y)
      for (unsigned i = 0; i < side; ++i) {
        const bool left_near = i + SMS_EXT_SPILL_LEFT >= side, right_near = i < SMS_EXT_SPILL_RIGHT;
        if (g->ext.left[y][i] != l2->ext.left[y][i]) (left_near ? near_differences : far_differences)++;
        if (g->ext.right[y][i] != l2->ext.right[y][i]) (right_near ? near_differences : far_differences)++;
      }
    check(far_differences == 0, "bounded spill: nothing beyond 8 columns left / 15 columns right differs from Level 2 (side " + std::to_string(side) + ")");
    check(near_differences > 0, "bounded spill: the edge columns do differ");
    delete l2;
    delete g;
  }
}

void test_l3_fallback_and_latching() {
  // Level 2 impossible (left-column mask or vertical-scroll lock): the frame is UNAVAILABLE, with no invented background for the sprites
  for (unsigned bit : {0x80u, 0x20u}) {
    Scenario s = sprite_scene(false, {{255, 40, 20}, {0, 60, 21}}, bit | 0x08, 0, 350);
    g_l3 = true;
    const Result on = play(s, true, 42, 42, false);
    g_l3 = false;
    const Result off = play(s, false, 0, 0, false);
    check(on.res.cls == SMS_EXT_UNAVAILABLE && on.res.sprites == 0 && on.res.sprite_pixels == 0 && on.res.side == 0, "Level 3 over an unavailable background is UNAVAILABLE with no sprite content");
    check(same_native(on, off), "native unchanged under an unavailable Level 3 request");
  }
  // the 240-line mode renders nothing, so nothing is extended
  { Rig* g = make_rig(true);
    Rng rng(351);
    fill_scene(g->vdp, rng, false, true);
    g->vdp.reg[0] = 0x06; g->vdp.reg[1] = 0x48 | 0x40;
    g->ext.request_side_192 = g->ext.request_side_224 = 42; g->ext.request_sprites = 1;
    for (unsigned line = 0; line < 240; ++line) sms_render_line(&g->r, &g->vdp, line, 0);
    check(g->r.frames_completed == 0 && g->ext.result.cls == SMS_EXT_NONE && g->ext.result.sprites == 0 && g->ext.unsupported_frames == 1, "240-line mode: unsupported, no Level 3 content");
    delete g; }
  // a mask raised mid-frame poisons the whole frame, sprites included
  { Scenario s = sprite_scene(false, {{255, 40, 20}}, 0, 0, 352);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) { v.reg[0] = static_cast<uint8_t>(line >= 100 ? 0x24 : 0x04); };
    g_l3 = true;
    const Result on = play(s, true, 42, 42, false);
    g_l3 = false;
    check(on.res.cls == SMS_EXT_UNAVAILABLE && on.res.sprites == 0, "mid-frame mask: the Level 3 frame is UNAVAILABLE as a whole"); }
  // display disabled for a band of lines: no sprites there, the rest still spills
  { Scenario s = sprite_scene(false, {{255, 40, 20}, {255, 120, 21}}, 0, 0, 353);
    s.hook = [](unsigned, SmsVdp& v, unsigned line) { v.reg[1] = static_cast<uint8_t>(line >= 36 && line < 60 ? 0x00 : 0x40); };
    expect_l3(s, 192, "display disabled band", 1); }
  // the request is latched at line 0: a mid-frame change applies to the next frame only (Original -> Level 2 -> Level 3 -> Level 2 -> Original)
  { Rig* g = make_rig(true);
    Rng rng(354);
    fill_scene(g->vdp, rng, false, true);
    for (unsigned i = 0; i < 64; ++i) g->vdp.vram[0x3F00 + i] = 0xD0;
    g->vdp.vram[0x3F00] = 40; g->vdp.vram[0x3F80] = 255; g->vdp.vram[0x3F81] = 20;
    const struct { uint32_t side; uint8_t sprites; } plan[] = {{0, 0}, {42, 0}, {42, 1}, {42, 0}, {0, 0}, {42, 1}};
    unsigned expect_sprites[] = {0, 0, 1, 0, 0, 1};
    for (unsigned f = 0; f < 6; ++f) {
      g->ext.request_side_192 = plan[f].side; g->ext.request_sprites = plan[f].sprites;
      for (unsigned line = 0; line < 192; ++line) {
        if (line == 20) g->ext.request_sprites = static_cast<uint8_t>(!plan[f].sprites);   // a mid-frame flip must not touch this frame
        if (line == 30) g->ext.request_side_192 = 0;
        sms_render_line(&g->r, &g->vdp, line, f);
      }
      const SmsExtResult& r = g->ext.result;
      check(r.sequence == g->r.frames_completed, "latch: result carries the frame identity");
      check(r.sprites == expect_sprites[f], "latch: frame " + std::to_string(f) + " content follows the request at its line 0");
      check((plan[f].side == 0) == (r.cls == SMS_EXT_NONE), "latch: Original frames carry no extension");
      check(r.sprites == 0 || r.sprite_pixels > 0, "latch: the Level 3 frame has spill pixels");
    }
    delete g; }
}

void test_l3_fuzz() {
  // Random sprite tables over random scenes (192/224, zoom, 8x16, shift, pattern banks, hscroll/vscroll, mid-frame register writes): the extension
  // equals the reference whenever it is produced, the native frame never depends on it, and the status flags are the native ones.
  Rng rng(4242);
  unsigned produced = 0, spilled = 0;
  for (int iter = 0; iter < 80; ++iter) {
    const uint32_t seed = rng.next();
    const bool tall = (seed & 1) != 0;
    std::vector<SprSpec> specs;
    const unsigned n = 4 + (seed >> 3) % 40;
    Rng r2(seed);
    for (unsigned k = 0; k < n; ++k) {
      const unsigned edge = r2.next() % 3;   // mostly sprites near the edges, some anywhere
      const unsigned x = edge == 0 ? 240 + r2.next() % 16 : (edge == 1 ? r2.next() % 16 : r2.next() % 256);
      specs.push_back({x, tall ? (r2.next() % 224) : (r2.next() % 192), r2.next() % 256});
    }
    Scenario s = sprite_scene(tall, specs, (seed & 2) ? 0x08 : 0, ((seed >> 1) & 1 ? 0x01 : 0) | ((seed >> 2) & 1 ? 0x02 : 0), seed);
    s.frames = 2;
    s.hook = [=](unsigned f, SmsVdp& v, unsigned line) {
      Rng r(seed + f * 31 + line);
      if (r.next() % 23 == 0) v.reg[8] = static_cast<uint8_t>(r.next());
      if (r.next() % 41 == 0) v.cram[16 + r.next() % 16] = static_cast<uint8_t>(r.next() & 63);
      if (r.next() % 53 == 0) v.vram[0x3F80 + r.next() % 128] = static_cast<uint8_t>(r.next());   // an X/N pair changes between lines
    };
    const unsigned height = tall ? 224 : 192;
    const uint32_t side = tall ? 71 : 42;
    g_l3 = true;
    const Result on = play(s, true, side, side, true);
    g_l3 = false;
    const Result off = play(s, false, 0, 0, false);
    check(same_native(on, off), "fuzz " + std::to_string(iter) + ": native identical with and without the Level 3 spill");
    check(on.res.cls == SMS_EXT_WRAPPED, "fuzz: wrapped");
    if (on.res.cls == SMS_EXT_WRAPPED) {
      ++produced;
      check(on.lines_match, "fuzz " + std::to_string(iter) + ": spill equals the reference (" + std::to_string(on.mismatches) + " mismatches)");
      check(on.res.sprite_pixels == on.ref_pixels && on.res.height == height, "fuzz: pixel count matches the reference");
      if (on.res.sprite_pixels > 0) ++spilled;
    }
  }
  check(produced > 60 && spilled > 20, "fuzz: the cases exercised both empty and non-empty spills");
}

void test_l3_records_unchanged() {
  // the frame records / SHA-256 never include the spill: identical with and without Level 3 over many frames
  Scenario s = sprite_scene(false, {{255, 40, 20}, {0, 90, 21}, {252, 150, 22}}, 0x08, 0x01, 360);
  s.frames = 5;
  g_l3 = true;
  const Result on = play(s, true, 42, 42, false);
  g_l3 = false;
  const Result off = play(s, false, 0, 0, false);
  check(same_native(on, off), "five Level 3 frames: records identical");
}

}  // namespace

int main() {
  test_wrap_and_scroll_values();
  test_provenance_is_pure_repetition();
  test_scroll_locks_and_mask();
  test_modes();
  test_midframe_changes();
  test_request_latching();
  test_geometry_and_reset();
  test_no_stale_rows();
  test_fuzz_native_invariance();
  test_records_exclude_extension();
  test_l3_edges();
  test_l3_transparency_priority();
  test_l3_hardware_limits();
  test_l3_bounded_spill();
  test_l3_fallback_and_latching();
  test_l3_fuzz();
  test_l3_records_unchanged();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::puts("sms_extension_test OK");
  return 0;
}
