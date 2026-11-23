// SEG-152-T003/T004 (ADR 0320): the exact-frame presentation seam of the Genesis mod scheduler and the VDP helper table, driven by a synthetic C11 ABI 2 provider
// (tests/fixtures/mod_sdk/exact_frame_provider.c) on a project-authored synthetic program. No ROM, no host clock, no viewer: the scheduler entry `genesis_mod_scheduler_present` is exactly what the viewer
// adapter calls from the producer seam, fed here with a synthetic GenesisCompatFrame-equivalent. The adapter side (render output bound to the produced frame) is covered by mod_exact_frame_abi2_test.py.
#include "mod_scheduler.h"
#include "native_compat.h"
#include "runtime.h"
#include "exact_frame_observation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) {
    std::printf("FAIL: %s\n", message.c_str());
    ++failures;
  }
}

// ---- the synthetic program: one 4-cycle "instruction" per dispatch; it keeps a counter in work RAM so the guest state is observable ----
GenesisControlTransfer synthetic_dispatch(GenesisRuntime *rt) {
  rt->work_ram[0x10] = static_cast<uint8_t>(rt->work_ram[0x10] + 1U);
  rt->work_ram[0x120] = static_cast<uint8_t>(rt->work_ram[0x120] + 3U);
  return genesis_runtime_retire_m68k_instruction(rt, 4U, rt->pc);
}

void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  r.devices.vdp.registers[1] = 0x44;
  r.devices.vdp.registers[12] = 0x81;
}

void run_frames(GenesisRuntime &r, uint64_t frames) {
  for (uint32_t slices = 0; r.virtual_frame_count < frames; ++slices) {
    if (slices > 40U * static_cast<uint32_t>(frames) + 64U) { check(false, "the virtual frame counter does not advance"); return; }
    const GenesisControlTransfer t = genesis_runtime_run(&r, synthetic_dispatch, 997U);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) { check(false, "the synthetic program stopped unexpectedly"); return; }
  }
}

std::string guest_digest(const GenesisRuntime &r) {
  GenesisSha256 sha;
  uint8_t out[32];
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, r.work_ram, sizeof(r.work_ram));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(&r.scheduler.master_ticks), sizeof(r.scheduler.master_ticks));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(&r.virtual_frame_count), sizeof(r.virtual_frame_count));
  genesis_sha256_final(&sha, out);
  static const char *hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : out) { s += hex[b >> 4U]; s += hex[b & 15U]; }
  return s;
}

uint64_t fnv(const void *data, size_t n) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}

constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint32_t kAllPhases = bit(SEGA_MOD_PHASE_RESET) | bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END) | bit(SEGA_MOD_PHASE_RENDER);
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }

// ---- the data of one produced frame, deterministic and distinct per frame: what the producer would hand to ABI 1 `prepare` ----
struct FrameData {
  std::array<uint8_t, 65536> ram{}, vram{};
  std::array<SegaGenesisFrameLine, 224> lines{};
  uint32_t line_count = 224U, width = 320U, height = 224U, side = 96U, requested = 7U;
  bool frame_start = true;
  void fill(uint32_t seed) {
    for (size_t i = 0; i < ram.size(); ++i) ram[i] = static_cast<uint8_t>((i * 7U + seed * 13U) >> 1U);
    for (size_t i = 0; i < vram.size(); ++i) vram[i] = static_cast<uint8_t>(i * 5U + seed * 29U);
    for (size_t l = 0; l < lines.size(); ++l) {
      for (size_t i = 0; i < 24U; ++i) lines[l].registers[i] = static_cast<uint16_t>(l * 3U + i + seed);
      for (size_t i = 0; i < 128U; ++i) lines[l].cram[i] = static_cast<uint8_t>(i + l + seed * 3U);
      for (size_t i = 0; i < 80U; ++i) lines[l].vsram[i] = static_cast<uint8_t>(i * 2U + l + seed);
    }
    // a mid-frame scroll / palette / register change: the second half of the frame differs from the first
    for (size_t l = 112U; l < lines.size(); ++l) { lines[l].registers[11] ^= 0x0004U; lines[l].registers[13] += 1U; lines[l].cram[10] ^= 0x0eU; lines[l].vsram[0] += 8U; }
  }
  GenesisModExactFrame view() const {
    GenesisModExactFrame f{};
    f.work_ram = ram.data(); f.vram = vram.data(); f.lines = lines.data(); f.line_count = line_count;
    f.width = width; f.height = height; f.side = side; f.requested = requested; f.vram_frame_start = frame_start ? 1U : 0U;
    return f;
  }
};

struct Harness {
  std::unique_ptr<GenesisRuntime> rt_heap{new GenesisRuntime};
  GenesisRuntime &rt = *rt_heap;
  std::unique_ptr<GenesisModScheduler> sched{new GenesisModScheduler};
  const char *rejection = "";
  int attach_result = -1;
  void init(uint64_t required, uint64_t optional, uint32_t mask, unsigned provider_minor = 1U, bool helpers = true) {
    setup(rt);
    g_exact_obs = ExactObservation{};
    g_exact_required_minor = provider_minor;
    void *state = nullptr;
    static SegaGenesisVdpHelpers table;
    const GenesisCompatHost *h = genesis_native_compat_host();   // the host's own functions, re-exported (ADR 0320 decision 5)
    table.size = sizeof table; table.read_hscroll = h->read_hscroll; table.read_vscroll = h->read_vscroll; table.vscroll_is_per_column = h->vscroll_is_per_column; table.cram_rgb = h->cram_rgb;
    GenesisModConfig c{};
    c.mod_id = "exact-frame-mod";
    c.provider = exact_frame_provider(&state);
    c.provider_state = state;
    c.phase_mask = mask;
    c.required[0] = required;
    c.optional[0] = optional;
    c.vdp_helpers = helpers ? &table : nullptr;
    attach_result = genesis_mod_scheduler_attach(sched.get(), &rt, &c, &rejection);
  }
};

constexpr uint64_t kExact = cap(SEGA_MOD_CAP_HOST_LOG) | cap(SEGA_MOD_CAP_FRAME_VDP) | cap(SEGA_MOD_CAP_FRAME_WORK_RAM);
constexpr uint64_t kOrdinary = cap(SEGA_MOD_CAP_HOST_LOG) | cap(SEGA_MOD_CAP_FRAME_WORK_RAM);

void test_opt_in_and_view() {
  std::puts("STEP opt_in_and_view");
  Harness h;
  h.init(kExact, 0U, kAllPhases);
  check(h.attach_result == 0, std::string("an exact-frame provider (frame.vdp required) attaches: ") + h.rejection);
  check(h.sched->exact_frame == 1U && sega_mod_is_granted(&h.sched->host, SEGA_MOD_CAP_FRAME_VDP), "frame.vdp is granted and selects the exact-frame callback");
  check(h.sched->features.size == sizeof(SegaGenesisFeatures) && h.sched->features.frame == nullptr && h.sched->features.vdp != nullptr, "the table is extended; the view is NULL outside the callback; the helper table is published");
  FrameData d;
  d.fill(1U);
  run_frames(h.rt, 5U);
  const unsigned render_before = g_exact_obs.render_calls;
  check(render_before == 0U, "the ordinary after-snapshot render never runs for an exact-frame provider (never called twice for one logical frame)");
  const uint64_t count = h.rt.virtual_frame_count;
  const GenesisModExactFrame f = d.view();
  check(genesis_mod_scheduler_present(h.sched.get(), &f) == 1, "present runs the callback");
  check(g_exact_obs.render_calls == 1U, "exactly one render callback per produced frame");
  check(g_exact_obs.last_render_frame == count - 1U, "host->frame is the produced frame (the runtime's frame count - 1)");
  check(g_exact_obs.view_seen && g_exact_obs.size == sizeof(SegaGenesisFrameView), "the provider sees the view and its size");
  check(g_exact_obs.width == 320U && g_exact_obs.height == 224U && g_exact_obs.side == 96U && g_exact_obs.requested == 7U && g_exact_obs.line_count == 224U, "geometry, side, requested and line count are the producer's");
  check(g_exact_obs.flags == SEGA_GEN_FRAME_VRAM_FRAME_START, "a journaled frame marks the VRAM as the frame-start state");
  check(g_exact_obs.work_ram_present && g_exact_obs.ram_hash == fnv(d.ram.data(), d.ram.size()), "work RAM is byte-identical to the previous-onset copy the producer holds");
  check(g_exact_obs.vram_hash == fnv(d.vram.data(), d.vram.size()), "VRAM is byte-identical to the producer's");
  check(g_exact_obs.lines_hash == fnv(d.lines.data(), d.lines.size() * sizeof(SegaGenesisFrameLine)), "registers, CRAM and VSRAM of every line (including the mid-frame change) are byte-identical");
  check(g_exact_obs.ram_read_status == SEGA_MOD_OK && g_exact_obs.ram_read_value == d.ram[0x10], "read() of work RAM in the exact-frame callback serves the same copy as the view");
  check(h.sched->features.frame == nullptr && g_exact_obs.kept != nullptr, "the accessor is NULL again outside the callback");
  check(g_exact_obs.vram_ptr != d.vram.data() && g_exact_obs.vram_ptr == h.sched->view_vram, "the view is a host-owned copy, never the producer's or the guest's buffer");
  // immutability: mutating every source after the callback changes nothing the provider was shown
  const uint64_t shown = fnv(h.sched->view_vram, 65536U);
  std::memset(d.vram.data(), 0xAA, d.vram.size());
  std::memset(d.ram.data(), 0x55, d.ram.size());
  std::memset(h.rt.work_ram, 0x33, sizeof h.rt.work_ram);
  check(fnv(h.sched->view_vram, 65536U) == shown && g_exact_obs.vram_hash == shown, "mutating the producer and the guest after the callback does not change the view");
  check(h.sched->disabled == 0U && h.sched->fault.faulted == 0U, "no fault");
}

void test_helpers_equal_abi1() {
  std::puts("STEP helpers_equal_abi1");
  Harness h;
  h.init(kExact, 0U, kAllPhases);
  run_frames(h.rt, 2U);
  const GenesisCompatHost *host = genesis_native_compat_host();
  int cases = 0;
  for (uint32_t mode = 0; mode < 12U; ++mode) {
    FrameData d;
    d.fill(mode + 40U);
    SegaGenesisFrameLine &l0 = d.lines[0];
    l0.registers[11] = static_cast<uint16_t>(mode % 4U | (mode >= 4U && mode < 8U ? 4U : 0U) | ((mode % 3U) << 1U));   // horizontal mode bits and the vertical-scroll bit
    l0.registers[12] = static_cast<uint16_t>(mode >= 8U ? 0x06U : 0x00U);                                          // interlace on/off
    l0.registers[13] = static_cast<uint16_t>(0x0400U + (mode * 0x40U) % 0x3C00U);
    const GenesisModExactFrame f = d.view();
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 1, "present for the helper differential");
    int32_t a = 0, b = 0, va = 0, vb = 0;
    uint8_t rgb[3] = {0, 0, 0};
    const int ra = host->read_hscroll(d.vram.data(), l0.registers, 0U, 0U, &a), rb = host->read_hscroll(d.vram.data(), l0.registers, 1U, 0U, &b);
    const int rva = host->read_vscroll(l0.vsram, l0.registers, 0U, 0U, &va), rvb = host->read_vscroll(l0.vsram, l0.registers, 1U, 0U, &vb);
    const int rpc = host->vscroll_is_per_column(l0.registers), rrgb = host->cram_rgb(l0.cram, 5U, rgb);
    check(g_exact_obs.hscroll_a == ra && (ra != 0 || g_exact_obs.hscroll_a_value == a) && g_exact_obs.hscroll_b == rb && (rb != 0 || g_exact_obs.hscroll_b_value == b), "hscroll equals the ABI 1 host (value and return code)");
    check(g_exact_obs.vscroll_a == rva && (rva != 0 || g_exact_obs.vscroll_a_value == va) && g_exact_obs.vscroll_b == rvb && (rvb != 0 || g_exact_obs.vscroll_b_value == vb), "vscroll equals the ABI 1 host");
    check((g_exact_obs.per_column != 0) == (rpc != 0) && g_exact_obs.rgb_status == rrgb && (rrgb != 0 || std::memcmp(g_exact_obs.rgb, rgb, 3U) == 0), "per-column mode and CRAM to RGB equal the ABI 1 host");
    ++cases;
  }
  check(cases == 12, "twelve register/mode combinations were compared");
  // out-of-range and NULL arguments: the SAME function pointers as ABI 1, so the same return codes
  const SegaGenesisVdpHelpers *t = h.sched->features.vdp;
  check(t != nullptr && t->read_hscroll == host->read_hscroll && t->read_vscroll == host->read_vscroll && t->vscroll_is_per_column == host->vscroll_is_per_column && t->cram_rgb == host->cram_rgb,
        "the helper members are the host's existing functions, not wrappers");
  FrameData d;
  d.fill(3U);
  int32_t v = 0;
  uint8_t rgb[3];
  check(t->read_hscroll(d.vram.data(), d.lines[0].registers, 0U, 9999U, &v) == host->read_hscroll(d.vram.data(), d.lines[0].registers, 0U, 9999U, &v), "out-of-range line: same code as ABI 1");
  check(t->read_hscroll(d.vram.data(), d.lines[0].registers, 7U, 0U, &v) == host->read_hscroll(d.vram.data(), d.lines[0].registers, 7U, 0U, &v), "out-of-range plane: same code as ABI 1");
  check(t->read_vscroll(d.lines[0].vsram, d.lines[0].registers, 0U, 9999U, &v) == host->read_vscroll(d.lines[0].vsram, d.lines[0].registers, 0U, 9999U, &v), "out-of-range column: same code as ABI 1");
  check(t->cram_rgb(d.lines[0].cram, 64U, rgb) == host->cram_rgb(d.lines[0].cram, 64U, rgb), "out-of-range CRAM index: same code as ABI 1");
  check(t->read_hscroll(nullptr, d.lines[0].registers, 0U, 0U, &v) == host->read_hscroll(nullptr, d.lines[0].registers, 0U, 0U, &v) && t->read_hscroll(d.vram.data(), d.lines[0].registers, 0U, 0U, nullptr) == host->read_hscroll(d.vram.data(), d.lines[0].registers, 0U, 0U, nullptr) &&
        t->cram_rgb(nullptr, 0U, rgb) == host->cram_rgb(nullptr, 0U, rgb) && t->read_vscroll(nullptr, d.lines[0].registers, 0U, 0U, &v) == host->read_vscroll(nullptr, d.lines[0].registers, 0U, 0U, &v), "NULL arguments: same code as ABI 1");
}

// Interlace mode 2 (SEG-152, ADR 0320): the output is 448 rows high but the raster capture holds 224 FIELD lines (GENESIS_RASTER_MAX_LINES); the renderer maps output row r to field line r / 2. The producer therefore
// hands ABI 1 `prepare` (and the view) height = 448 with line_count = 224: `line_count` counts captured field/raster lines, not output pixel rows, and nothing of the VDP state is dropped.
static_assert(GENESIS_FRAME_MAX_HEIGHT == 448U && GENESIS_RASTER_MAX_LINES == 224U && SEGA_GEN_FRAME_MAX_LINES == GENESIS_RASTER_MAX_LINES, "the view holds every line the capture holds");
static_assert(sizeof(SegaGenesisFrameLine) == sizeof(GenesisCompatLine), "the view line is the capture line");

void test_interlace_field_lines() {
  std::puts("STEP interlace_field_lines");
  Harness h;
  h.init(kExact, 0U, kAllPhases);
  run_frames(h.rt, 2U);
  FrameData d;
  d.fill(77U);
  d.height = 448U;   // interlace mode 2: 448 output rows
  d.line_count = std::min<uint32_t>(d.height, GENESIS_RASTER_MAX_LINES);   // exactly what vdp_render.c hands ABI 1 prepare: min(height, GENESIS_RASTER_MAX_LINES)
  check(d.line_count == 224U, "the producer's line count for a 448-high frame is the 224 captured field lines");
  const GenesisModExactFrame f = d.view();
  check(genesis_mod_scheduler_present(h.sched.get(), &f) == 1, "a 448-high frame is presented");
  check(g_exact_obs.height == 448U && g_exact_obs.line_count == 224U, "the view reports the 448-row picture and 224 field-line records");
  check(g_exact_obs.lines_hash == fnv(d.lines.data(), 224U * sizeof(SegaGenesisFrameLine)), "every captured field line (registers, CRAM, VSRAM) is present and byte-identical: none is dropped");
  // output rows r and r + 1 (2k, 2k + 1) read field line k: the view's record k describes both
  const GenesisCompatHost *host = genesis_native_compat_host();
  int32_t a = 0, b = 0;
  uint16_t regs[24];
  std::memcpy(regs, d.lines[10].registers, sizeof regs);
  regs[12] |= 0x0006U;   // LSM = 11: interlace mode 2
  check(host->read_hscroll(d.vram.data(), regs, 0U, 20U, &a) == host->read_hscroll(d.vram.data(), regs, 0U, 21U, &b) && a == b, "in interlace mode 2 output rows 20 and 21 share field line 10's horizontal scroll (row r maps to field line r / 2)");
}

void test_flags_and_capabilities() {
  std::puts("STEP flags_and_capabilities");
  {   // a frame without a VRAM journal is NOT declined: the provider is called, the flag is clear and the VRAM is still a host-owned copy
    Harness h;
    h.init(kExact, 0U, kAllPhases);
    run_frames(h.rt, 2U);
    FrameData d;
    d.fill(9U);
    d.frame_start = false;
    const GenesisModExactFrame f = d.view();
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 1 && g_exact_obs.render_calls == 1U, "a non-journaled frame still calls the provider");
    check(g_exact_obs.flags == 0U && g_exact_obs.vram_hash == fnv(d.vram.data(), d.vram.size()) && g_exact_obs.vram_ptr == h.sched->view_vram, "the view says honestly it is not a frame-start snapshot and is a host-owned copy");
  }
  {   // frame.work_ram and frame.vdp are independent: without frame.work_ram the RAM view and read() are refused, the VDP view stays
    Harness h;
    h.init(cap(SEGA_MOD_CAP_HOST_LOG) | cap(SEGA_MOD_CAP_FRAME_VDP), 0U, kAllPhases);
    run_frames(h.rt, 2U);
    FrameData d;
    d.fill(11U);
    const GenesisModExactFrame f = d.view();
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0 && h.sched->disabled == 1U, "read() of work RAM without frame.work_ram is a capability fault (as before)");
    check(g_exact_obs.render_calls == 1U && !g_exact_obs.work_ram_present, "and the RAM view of that callback was NULL");
  }
  {   // a missing previous-onset copy with frame.work_ram granted: the RAM view is NULL and a read is a phase fault
    Harness h;
    h.init(kExact, 0U, kAllPhases);
    run_frames(h.rt, 2U);
    FrameData d;
    d.fill(12U);
    GenesisModExactFrame f = d.view();
    f.work_ram = nullptr;
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0 && h.sched->disabled == 1U && std::string(h.sched->fault.cause) == "phase", "read() without a valid copy is SEGA_MOD_E_PHASE and a fault");
  }
  {   // bad input: no VRAM -> the callback is not run, nothing faults
    Harness h;
    h.init(kExact, 0U, kAllPhases);
    FrameData d;
    d.fill(1U);
    GenesisModExactFrame f = d.view();
    f.vram = nullptr;
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0 && h.sched->disabled == 0U && g_exact_obs.render_calls == 0U, "a frame without VRAM is not presented and is not a fault");
  }
}

void test_optional_and_ordinary() {
  std::puts("STEP optional_and_ordinary");
  uint64_t ordinary_frames_seen = 0;
  std::string digest_ordinary;
  {   // frame.vdp declared OPTIONAL: never granted, no change of timing: the ordinary after-snapshot render runs, there is no view and no helper
    Harness h;
    h.init(kOrdinary, cap(SEGA_MOD_CAP_FRAME_VDP), kAllPhases, 0U);
    check(h.attach_result == 0 && !sega_mod_is_granted(&h.sched->host, SEGA_MOD_CAP_FRAME_VDP) && h.sched->exact_frame == 0U && h.sched->features.vdp == nullptr, "an optional frame.vdp is not granted and selects nothing");
    run_frames(h.rt, 6U);
    check(g_exact_obs.render_calls >= 5U && g_exact_obs.frame_ptr_outside_render == 0U && !g_exact_obs.view_seen, "the ordinary render runs every frame and sees no view");
    check(g_exact_obs.last_render_frame + 1U == h.rt.virtual_frame_count, "ordinary render keeps its meaning: the frame that just ended");
    FrameData d;
    d.fill(1U);
    const GenesisModExactFrame f = d.view();
    check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0, "present refuses a non-exact provider");
    ordinary_frames_seen = g_exact_obs.render_calls;
    digest_ordinary = guest_digest(h.rt);
  }
  {   // a provider built before the minor existed (required_minor 0, no frame.vdp) runs unchanged on the extended host: identical guest digest to a run with no scheduler at all
    Harness h;
    h.init(kOrdinary, 0U, kAllPhases, 0U);
    run_frames(h.rt, 6U);
    check(g_exact_obs.render_calls == ordinary_frames_seen && guest_digest(h.rt) == digest_ordinary, "an unchanged provider has identical callback counts and guest digest");
    std::unique_ptr<GenesisRuntime> bare{new GenesisRuntime};
    setup(*bare);
    run_frames(*bare, 6U);
    check(guest_digest(*bare) == digest_ordinary, "and the same guest digest as a run without any scheduler");
  }
  {   // negotiation: a provider requiring the new minimum on a host built with the previous minor fails with the stable class
    void *state = nullptr;
    g_exact_required_minor = 1U;
    const SegaModProvider *p = exact_frame_provider(&state);
    SegaGenesisFeatures features{};
    features.size = sizeof features;
    SegaModHost host{};
    host.size = sizeof host; host.abi_major = SEGA_MOD_ABI_MAJOR; host.abi_minor = 0U; host.platform = SEGA_MOD_PLATFORM_GENESIS; host.platform_features = &features; host.platform_features_size = sizeof features;
    host.granted[0] = kExact;
    const uint64_t req[2] = {kExact, 0U};
    check(sega_mod_negotiate(p, &host, req) == SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW && std::string(sega_mod_negotiation_class(SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW)) == "E_ABI_MINOR_TOO_NEW", "an older host (minor 0) refuses a provider requiring minor 1 with E_ABI_MINOR_TOO_NEW");
    host.abi_minor = SEGA_MOD_ABI_MINOR;
    check(SEGA_MOD_ABI_MINOR == 1U && sega_mod_negotiate(p, &host, req) == SEGA_MOD_NEG_OK, "the extended host (minor 1) accepts it");
    g_exact_required_minor = 1U;
  }
  {   // attach-time class: an exact-frame provider that registers no render phase is refused
    Harness h;
    h.init(kExact, 0U, kAllPhases & ~bit(SEGA_MOD_PHASE_RENDER));
    check(h.attach_result == 1 && std::string(h.rejection) == "E_PHASE_UNSUPPORTED", "an exact-frame provider without a render phase is refused");
  }
  {   // an older host cannot grant the capability at all: the scheduler's supported set is the single source
    uint64_t supported[2];
    genesis_mod_scheduler_supported(supported);
    check((supported[0] & cap(SEGA_MOD_CAP_FRAME_VDP)) != 0U, "frame.vdp is grantable by this scheduler");
  }
}

void test_fault_and_r1() {
  std::puts("STEP fault_and_r1");
  // R1: the guest digest is identical with presentation on, off and skipped on some frames
  std::string on, off, skipped;
  for (int mode = 0; mode < 3; ++mode) {
    Harness h;
    h.init(kExact, 0U, kAllPhases);
    FrameData d;
    for (uint64_t frame = 1U; frame <= 8U; ++frame) {
      run_frames(h.rt, frame);
      d.fill(static_cast<uint32_t>(frame));
      const GenesisModExactFrame f = d.view();
      if (mode == 0 || (mode == 2 && frame % 3U != 0U)) (void)genesis_mod_scheduler_present(h.sched.get(), &f);
    }
    (mode == 0 ? on : mode == 1 ? off : skipped) = guest_digest(h.rt);
    if (mode == 0) check(g_exact_obs.render_calls == 8U, "presentation on: one callback per produced frame");
    if (mode == 2) check(g_exact_obs.render_calls == 6U, "a skipped producer call is not called and nothing is carried");
    if (mode == 1) check(g_exact_obs.render_calls == 0U, "presentation off: no callback at all");
  }
  check(on == off && off == skipped, "guest digests are identical with presentation on, off and skipped on some frames (R1)");
  // a genuine fault: discards, disables, records E_MOD_FAULT; the guest continues; the disabled flag survives into the snapshot state
  Harness h;
  h.init(kExact, 0U, kAllPhases);
  run_frames(h.rt, 3U);
  FrameData d;
  d.fill(2U);
  const GenesisModExactFrame f = d.view();
  g_exact_obs.fault_next = 1;
  check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0 && h.sched->disabled == 1U && h.sched->fault.faulted == 1U && std::string(h.sched->fault.text).find("E_MOD_FAULT") == 0U, "a callback fault disables the mod and records E_MOD_FAULT");
  check(h.sched->features.frame == nullptr, "the view is NULL again after a fault");
  const unsigned calls = g_exact_obs.render_calls;
  check(genesis_mod_scheduler_present(h.sched.get(), &f) == 0 && g_exact_obs.render_calls == calls, "a disabled provider is not called again");
  run_frames(h.rt, 6U);
  GenesisSnapshotModState snap{};
  genesis_mod_snapshot_state(h.sched.get(), &snap);
  check(h.rt.virtual_frame_count >= 6U && snap.ctx == h.sched.get(), "the guest continues and the mod-state seam is available for save/load");
}

// SEG-152-T003 (the cost of the frame view, measured like the scheduler's OVERHEAD_EVIDENCE): fixed host-owned buffers, nothing allocated per frame. The numbers do not depend on the aspect: the view always
// copies the same 64 KiB + 64 KiB + 224 lines (the 32:9 planned side changes no size here, only the producer's own extension work, which is not part of this cost).
void test_cost() {
  std::puts("STEP cost");
  using clock = std::chrono::steady_clock;
  const int n = 4000;
  Harness h;
  h.init(kExact, 0U, kAllPhases);
  run_frames(h.rt, 2U);
  FrameData d;
  d.fill(5U);
  const GenesisModExactFrame f = d.view();
  auto ns_per = [&](auto &&body) {
    const auto t0 = clock::now();
    for (int i = 0; i < n; ++i) body(i);
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count()) / n;
  };
  volatile uint8_t sink = 0;
  std::vector<uint8_t> ram(65536), vram(65536), lines(224U * sizeof(SegaGenesisFrameLine));
  const double ram_copy = ns_per([&](int i) { d.ram[0] = static_cast<uint8_t>(i); std::memcpy(ram.data(), d.ram.data(), ram.size()); sink = ram[static_cast<size_t>(i) & 1023U]; });
  const double vram_copy = ns_per([&](int i) { d.vram[0] = static_cast<uint8_t>(i); std::memcpy(vram.data(), d.vram.data(), vram.size()); sink = vram[static_cast<size_t>(i) & 1023U]; });
  const double line_copy = ns_per([&](int i) { d.lines[0].registers[0] = static_cast<uint16_t>(i); std::memcpy(lines.data(), d.lines.data(), lines.size()); sink = lines[static_cast<size_t>(i) & 1023U]; });
  g_exact_obs.light = 1;
  const double total = ns_per([&](int) { (void)genesis_mod_scheduler_present(h.sched.get(), &f); });
  g_exact_obs.light = 0;
  const double budget = 1e9 / 60.0;
  const double callback = total - ram_copy - vram_copy - line_copy;
  std::printf("EXACT_FRAME_COST ram_copy_ns=%.0f vram_copy_ns=%.0f field_lines_copy_ns=%.0f callback_overhead_ns=%.0f present_total_ns=%.0f frame_budget_share_percent=%.4f frames=%d\n", ram_copy, vram_copy, line_copy,
              callback < 0.0 ? 0.0 : callback, total, 100.0 * total / budget, n);
  check(g_exact_obs.render_calls >= static_cast<unsigned>(n), "the measured loop ran the callback for every frame");
  check(total < 0.05 * budget, "the exact-frame view costs less than 5% of a 60 Hz frame budget (a generous bound against noise; the measured share is reported above)");
  (void)sink;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "";   // `genesis_mod_vdp_helper_tests` (SEG-152-T004) runs only the helper differential; `genesis_mod_exact_frame_cost` only the measurement
  if (mode == "helpers") {
    test_helpers_equal_abi1();
  } else if (mode == "cost") {
    test_cost();
  } else {
    test_opt_in_and_view();
    test_helpers_equal_abi1();
    test_interlace_field_lines();
    test_flags_and_capabilities();
    test_optional_and_ordinary();
    test_fault_and_r1();
  }
  if (failures != 0) {
    std::printf("genesis_mod_exact_frame_tests: %d FAILED\n", failures);
    return 1;
  }
  std::puts("genesis_mod_exact_frame_tests: OK");
  return 0;
}
