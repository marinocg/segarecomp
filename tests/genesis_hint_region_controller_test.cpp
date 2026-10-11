// SEG-055: runtime-level synthetic fixtures for
//   * the VDP H-interrupt counter and level-4 admission (line schedule, enable gating, SR masking, IRQ6/IRQ4 priority,
//     missing-handler fail-closed behavior, STOP wake-up),
//   * console region profiles (Japan NTSC / USA NTSC / Europe PAL) driving the guest-visible version register, selection
//     from the host API and from SEGARECOMP_GENESIS_REGION, PAL refusal, and a guest-visible region check taking
//     different compatibility branches under different hardware profiles,
//   * independent Player 1 / Player 2 controller ports.
// Project-authored synthetic state only; no ROM, no host clock. Interrupt-frame returns (RTE) are modelled by restoring
// SR/SP, exactly what the guest's RTE does.
#include "runtime.h"
#include "../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" void genesis_m68k_enter_stopped_state(GenesisRuntime *runtime);

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

constexpr uint32_t kRamBegin = 0x00FF0000u;
constexpr uint32_t kHint = 0x500u;
constexpr uint32_t kVint = 0x300u;
constexpr uint32_t kMain = 0x200u;

std::unique_ptr<GenesisRuntime> make_runtime() {
  auto r = std::make_unique<GenesisRuntime>();
  std::memset(r.get(), 0, sizeof(*r));
  r->pc = kMain;
  r->sr = 0x2000u;
  r->a[7] = kRamBegin + 0x8000u;
  r->devices.vdp.registers[1] = 0x04;  // Mode 5; IE0 clear unless a test sets it
  return r;
}

void install_hint_handler(GenesisRuntime &r) {
  r.software_exception_handler_entry[GENESIS_M68K_VECTOR_LEVEL4_AUTOVECTOR] = kHint;
  r.software_exception_handler_present[GENESIS_M68K_VECTOR_LEVEL4_AUTOVECTOR] = 1;
}

struct Entry { unsigned line; uint32_t handler; };
struct Trace {
  std::vector<Entry> entries;
  bool stopped = false;
  GenesisControlTransfer stop{};
};

// Retires 4-cycle instructions until `frames` virtual frames have elapsed, recording every accepted interrupt (its handler
// and the scanline it was accepted on) and modelling the handler's RTE.
Trace drive(GenesisRuntime &r, unsigned frames, uint16_t restore_sr = 0x2000u) {
  Trace trace;
  uint32_t pc = r.pc;
  const uint64_t target = r.virtual_frame_count + frames;
  while (r.virtual_frame_count < target) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&r, 4u, pc);
    if (t.kind == GENESIS_STOP) {
      trace.stopped = true;
      trace.stop = t;
      return trace;
    }
    if (t.next_pc == kHint || t.next_pc == kVint) {
      trace.entries.push_back({(unsigned)((r.scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_LINE) % GENESIS_NTSC_LINES_PER_FRAME), t.next_pc});
      r.sr = restore_sr;  // RTE: restores SR (mask) and pops the six-byte frame
      r.a[7] += 6u;
      pc = kMain;
    } else {
      pc = t.next_pc;
    }
  }
  return trace;
}

std::vector<unsigned> lines_of(const Trace &t, uint32_t handler, unsigned from_frame_index, unsigned frame_count,
                               uint64_t frame_start_line) {
  std::vector<unsigned> out;
  (void)from_frame_index;
  (void)frame_count;
  (void)frame_start_line;
  for (const Entry &e : t.entries)
    if (e.handler == handler) out.push_back(e.line);
  return out;
}

void test_hint_counter_schedule() {
  auto r = make_runtime();
  install_hint_handler(*r);
  r->devices.vdp.registers[0] = 0x10;   // H-interrupt enable
  r->devices.vdp.registers[10] = 9;     // every 10th line
  const Trace t = drive(*r, 3);
  check(!t.stopped, "H-interrupt run does not stop");
  const std::vector<unsigned> lines = lines_of(t, kHint, 0, 3, 0);
  check(lines.size() == 66u, "reg #10 = 9: 22 H-interrupts per 224-line active display over 3 frames (66)");
  bool all = lines.size() == 66u;
  for (std::size_t i = 0; all && i < lines.size(); ++i) {
    const unsigned within_frame = (unsigned)(i % 22u);
    all = lines[i] == 9u + 10u * within_frame;
  }
  check(all, "H-interrupts occur on lines 9, 19, 29, ... 219 of every frame (N + 1 line period, counter reloaded each frame)");

  auto every = make_runtime();
  install_hint_handler(*every);
  every->devices.vdp.registers[0] = 0x10;
  every->devices.vdp.registers[10] = 0;
  const Trace e = drive(*every, 2);
  // One on each of the 224 active lines, plus the documented extra latch when the counter is still zero as line 224 begins.
  check(!e.stopped && lines_of(e, kHint, 0, 2, 0).size() == 450u, "reg #10 = 0: one H-interrupt on each active line plus the line-224 latch (225 per frame)");

  auto big = make_runtime();
  install_hint_handler(*big);
  big->devices.vdp.registers[0] = 0x10;
  big->devices.vdp.registers[10] = 250;  // longer than the active display: never fires except through the line-224 rule
  const Trace b = drive(*big, 2);
  check(!b.stopped && lines_of(b, kHint, 0, 2, 0).empty(), "a counter longer than the active display raises no H-interrupt");
}

void test_hint_enable_gating_and_latch() {
  auto r = make_runtime();
  install_hint_handler(*r);
  r->devices.vdp.registers[0] = 0x00;  // disabled
  r->devices.vdp.registers[10] = 9;
  const Trace off = drive(*r, 1);
  check(!off.stopped && off.entries.empty(), "H-interrupt disabled (register #0 bit 4 clear): nothing is taken");
  check(r->raster.hint_pending == 1u, "the request is latched while disabled");
  // Enabling later takes the latched request at the next retirement boundary, then clears it.
  r->devices.vdp.registers[0] = 0x10;
  const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, kMain);
  check(t.next_pc == kHint && r->raster.hint_pending == 0u, "a latched H-interrupt is taken once it is enabled, and the latch clears");
}

void test_hint_sr_mask_and_priority() {
  auto r = make_runtime();
  install_hint_handler(*r);
  r->devices.vdp.registers[0] = 0x10;
  r->devices.vdp.registers[10] = 9;
  r->sr = 0x2400u;  // mask 4 blocks level 4
  const Trace masked = drive(*r, 1, 0x2400u);
  check(!masked.stopped && masked.entries.empty() && r->raster.hint_pending == 1u, "interrupt mask 4 blocks the level-4 H-interrupt (stays pending)");
  r->sr = 0x2300u;  // mask 3 admits it
  const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, kMain);
  check(t.next_pc == kHint && (r->sr & 0x0700u) == 0x0400u && (r->sr & 0x2000u) == 0x2000u,
        "accepted H-interrupt enters supervisor mode and raises the mask to level 4");

  // Line 224: reg #10 = 224 latches the H-interrupt on the same boundary as the VBlank onset; level 6 wins, level 4 follows.
  auto p = make_runtime();
  install_hint_handler(*p);
  p->irq6_handler_entry = kVint;
  p->irq6_handler_present = 1u;
  p->devices.vdp.registers[0] = 0x10;
  p->devices.vdp.registers[1] = 0x24;  // Mode 5 + IE0
  p->devices.vdp.registers[10] = 224;
  const Trace pr = drive(*p, 3);
  check(!pr.stopped && pr.entries.size() >= 4u, "priority run produced interrupts");
  bool ordered = pr.entries.size() >= 4u;
  for (std::size_t i = 0; ordered && i + 1u < pr.entries.size(); i += 2u)
    ordered = pr.entries[i].handler == kVint && pr.entries[i + 1u].handler == kHint && pr.entries[i].line == 224u && pr.entries[i + 1u].line == 224u;
  check(ordered, "coincident VBlank (level 6) and H-interrupt (level 4): level 6 is taken first, level 4 right after its RTE");
}

void test_hint_missing_handler_fails_closed() {
  auto r = make_runtime();  // no vector-28 handler installed
  r->devices.vdp.registers[0] = 0x10;
  r->devices.vdp.registers[10] = 9;
  const Trace t = drive(*r, 1);
  check(t.stopped && t.stop.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
            t.stop.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
        "an enabled H-interrupt without a build-resolved handler stops fail-closed (never silently dropped)");
  auto q = make_runtime();  // disabled: no handler is needed
  q->devices.vdp.registers[0] = 0x00;
  q->devices.vdp.registers[10] = 9;
  check(!drive(*q, 1).stopped, "no handler is needed while the H-interrupt is disabled");
}

void test_hint_stop_wake() {
  // STOP #$2000 (mask 0) with H-interrupt enabled: wakes at the first latch (line 9).
  auto r = make_runtime();
  install_hint_handler(*r);
  r->devices.vdp.registers[0] = 0x10;
  r->devices.vdp.registers[10] = 9;
  genesis_m68k_enter_stopped_state(r.get());
  const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, kMain);
  check(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == kHint &&
            r->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_LINE == 9u,
        "STOP wakes on the H-interrupt at its scanline");
  // STOP #$2400 (mask 4) with only the H-interrupt as a source cannot wake: explicit diagnostic, no spinning.
  auto m = make_runtime();
  install_hint_handler(*m);
  m->sr = 0x2400u;
  m->devices.vdp.registers[0] = 0x10;
  m->devices.vdp.registers[10] = 9;
  genesis_m68k_enter_stopped_state(m.get());
  const GenesisControlTransfer s = genesis_runtime_retire_m68k_instruction(m.get(), 4u, kMain);
  check(s.kind == GENESIS_STOP && s.stop.diagnostic_category == GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE,
        "STOP with a mask the H-interrupt cannot pass and no other source ends with stopped_without_wake_source");
  // STOP #$2400 with VBlank enabled keeps the pre-existing single-jump wake at the next onset: the timing is exactly what an
  // identical machine without any H-interrupt configuration produces.
  auto make_vblank_stop = [](bool hint_enabled) {
    auto v = make_runtime();
    install_hint_handler(*v);
    v->sr = 0x2400u;
    v->irq6_handler_entry = kVint;
    v->irq6_handler_present = 1u;
    v->devices.vdp.registers[0] = hint_enabled ? 0x10 : 0x00;
    v->devices.vdp.registers[1] = 0x24;
    v->devices.vdp.registers[10] = 9;
    genesis_m68k_enter_stopped_state(v.get());
    return v;
  };
  auto with_hint = make_vblank_stop(true);
  auto without_hint = make_vblank_stop(false);
  const GenesisControlTransfer w = genesis_runtime_retire_m68k_instruction(with_hint.get(), 4u, kMain);
  const GenesisControlTransfer x = genesis_runtime_retire_m68k_instruction(without_hint.get(), 4u, kMain);
  check(w.kind == GENESIS_CONTINUE_AT_PC && w.next_pc == kVint && x.next_pc == kVint &&
            with_hint->scheduler.master_ticks == without_hint->scheduler.master_ticks &&
            with_hint->scheduler.master_ticks >= GENESIS_NTSC_VBLANK_ONSET_TICK,
        "STOP with a mask that blocks level 4 still wakes at the VBlank onset with exactly the pre-H-interrupt timing");
}

// ---- region ------------------------------------------------------------------------------------------------------
void set_env(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr) setenv(name, value, 1);
  else unsetenv(name);
#endif
}

uint32_t read_version(GenesisRuntime &r) {
  uint32_t value = 0xDEADBEEFu;
  GenesisRuntimeStop stop{};
  check(genesis_route_access(&r, SEGARECOMP_GENESIS_CONTROLLER_VERSION_BYTE_ADDRESS, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK,
        "version register byte read is routed");
  return value;
}

GenesisControlTransfer one_step_dispatch(GenesisRuntime *r) { return genesis_runtime_retire_m68k_instruction(r, 4u, r->pc + 2u); }

void test_region_profiles_and_version_register() {
  // Distinct, hardware-encoded values: bit7 overseas, bit6 PAL, bit5 no expansion unit.
  check(segarecomp_genesis_region_version_value(SEGARECOMP_GENESIS_REGION_JAPAN_NTSC) == 0x20u, "Japan NTSC version register = 0x20 (domestic, 60 Hz)");
  check(segarecomp_genesis_region_version_value(SEGARECOMP_GENESIS_REGION_USA_NTSC) == 0xA0u, "USA NTSC version register = 0xA0 (overseas, 60 Hz)");
  check(segarecomp_genesis_region_version_value(SEGARECOMP_GENESIS_REGION_EUROPE_PAL) == 0xE0u, "Europe PAL version register = 0xE0 (overseas, 50 Hz)");
  check(SEGARECOMP_GENESIS_CONTROLLER_VERSION_POLICY_VALUE == segarecomp_genesis_region_version_value(SEGARECOMP_GENESIS_REGION_USA_NTSC),
        "the historical policy value is exactly the USA NTSC profile (default compatibility)");
  check(segarecomp_genesis_region_is_supported(SEGARECOMP_GENESIS_REGION_JAPAN_NTSC) && segarecomp_genesis_region_is_supported(SEGARECOMP_GENESIS_REGION_USA_NTSC) &&
            !segarecomp_genesis_region_is_supported(SEGARECOMP_GENESIS_REGION_EUROPE_PAL),
        "the two 60 Hz profiles are operational; PAL is representable but not supported");

  set_env("SEGARECOMP_GENESIS_REGION", nullptr);
  auto d = make_runtime();
  check(read_version(*d) == 0xA0u, "a zero-initialized runtime is the USA NTSC console (default behavior preserved)");

  for (unsigned profile = 0; profile < 3u; ++profile) {
    auto r = make_runtime();
    check(genesis_runtime_set_region(r.get(), profile) == 0, "region selection accepted before the guest starts");
    check(read_version(*r) == segarecomp_genesis_region_version_value(profile), "the configured profile determines the version register");
  }
  auto bad = make_runtime();
  check(genesis_runtime_set_region(bad.get(), 3u) == -1 && bad->region_profile == 0u, "an unknown profile is rejected without changing the console");
  auto late = make_runtime();
  (void)genesis_runtime_retire_m68k_instruction(late.get(), 4u, kMain);
  check(genesis_runtime_set_region(late.get(), 1u) == -1 && late->region_profile == 0u, "the region cannot change once the guest has started (stable for the whole run)");

  unsigned p = 99;
  check(genesis_region_profile_from_name("Japan", &p) == 0 && p == 1u && genesis_region_profile_from_name("ntsc-u", &p) == 0 && p == 0u &&
            genesis_region_profile_from_name("PAL", &p) == 0 && p == 2u && genesis_region_profile_from_name("mars", &p) == -1,
        "region names parse (case-insensitive aliases) and unknown names are rejected");
  check(std::strcmp(genesis_region_profile_name(1), "japan") == 0 && std::strcmp(genesis_region_profile_name(0), "usa") == 0 &&
            std::strcmp(genesis_region_profile_name(2), "europe") == 0,
        "stable profile names");

  // Selection through the runner's environment variable (the existing launcher channel).
  set_env("SEGARECOMP_GENESIS_REGION", "japan");
  auto j = make_runtime();
  const GenesisControlTransfer jr = genesis_runtime_run(j.get(), one_step_dispatch, 3u);
  check(jr.kind == GENESIS_RUNNER_RESOURCE_LIMIT && j->region_profile == 1u && read_version(*j) == 0x20u,
        "SEGARECOMP_GENESIS_REGION=japan selects the Japan NTSC console before the first step");
  set_env("SEGARECOMP_GENESIS_REGION", "pal");
  auto pal = make_runtime();
  const GenesisControlTransfer pr = genesis_runtime_run(pal.get(), one_step_dispatch, 3u);
  check(pr.kind == GENESIS_STOP && pr.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_REGION_TIMING && pal->scheduler.master_ticks == 0u,
        "a PAL console is refused honestly before any guest instruction runs (50 Hz timing is not implemented)");
  set_env("SEGARECOMP_GENESIS_REGION", "mars");
  auto unk = make_runtime();
  const GenesisControlTransfer ur = genesis_runtime_run(unk.get(), one_step_dispatch, 3u);
  check(ur.kind == GENESIS_STOP && ur.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_REGION_TIMING && unk->scheduler.master_ticks == 0u,
        "an unrecognized region name fails closed");
  set_env("SEGARECOMP_GENESIS_REGION", "auto");
  auto a = make_runtime();
  check(genesis_runtime_run(a.get(), one_step_dispatch, 3u).kind == GENESIS_RUNNER_RESOURCE_LIMIT && a->region_profile == 0u, "auto keeps the default USA NTSC console");
  set_env("SEGARECOMP_GENESIS_REGION", "japan");
  auto host_wins = make_runtime();
  check(genesis_runtime_set_region(host_wins.get(), 0u) == 0 && genesis_runtime_run(host_wins.get(), one_step_dispatch, 3u).kind == GENESIS_RUNNER_RESOURCE_LIMIT &&
            host_wins->region_profile == 0u,
        "an explicit host selection outranks the environment");
  set_env("SEGARECOMP_GENESIS_REGION", nullptr);
}

// A synthetic "cartridge" whose own code performs the classic region check: it reads the hardware version register and
// compares the overseas bit with what its header declares. The image is identical for every profile; only the configured
// console differs. d0 receives 1 (accepted), 2 (the cartridge's own region-lockout screen).
uint32_t g_header_overseas = 0;  // header declares a domestic-only cartridge
GenesisControlTransfer region_check_game(GenesisRuntime *r) {
  if (r->pc == 0x200u) {
    uint32_t version = 0;
    GenesisRuntimeStop stop{};
    if (genesis_route_access(r, SEGARECOMP_GENESIS_CONTROLLER_VERSION_BYTE_ADDRESS, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &version, &stop) != GENESIS_ACCESS_OK)
      return genesis_internal_dispatch_inconsistency_stop(r);
    const uint32_t console_overseas = (version & SEGARECOMP_GENESIS_VERSION_OVERSEAS_BIT) != 0u ? 1u : 0u;
    return genesis_runtime_retire_m68k_instruction(r, 20u, console_overseas == g_header_overseas ? 0x300u : 0x400u);
  }
  GenesisControlTransfer done{};
  done.kind = GENESIS_COMPLETE;
  r->d[0] = r->pc == 0x300u ? 1u : 2u;
  return done;
}

void test_region_sensitive_guest_branches() {
  set_env("SEGARECOMP_GENESIS_REGION", nullptr);
  g_header_overseas = 0;  // a domestic (Japan-only) cartridge
  auto jp = make_runtime();
  check(genesis_runtime_set_region(jp.get(), SEGARECOMP_GENESIS_REGION_JAPAN_NTSC) == 0, "select Japan NTSC");
  const GenesisControlTransfer a = genesis_runtime_run(jp.get(), region_check_game, 16u);
  check(a.kind == GENESIS_COMPLETE && jp->d[0] == 1u, "Japan NTSC console: the domestic cartridge's own region check passes (no ROM change)");
  auto us = make_runtime();
  const GenesisControlTransfer b = genesis_runtime_run(us.get(), region_check_game, 16u);
  check(b.kind == GENESIS_COMPLETE && us->d[0] == 2u,
        "USA NTSC console: the same image reaches its own lockout screen -- a normal guest outcome, not an internal failure");
  g_header_overseas = 1;  // an overseas cartridge
  auto us2 = make_runtime();
  check(genesis_runtime_run(us2.get(), region_check_game, 16u).kind == GENESIS_COMPLETE && us2->d[0] == 1u, "USA NTSC console accepts an overseas cartridge");
  auto jp2 = make_runtime();
  (void)genesis_runtime_set_region(jp2.get(), SEGARECOMP_GENESIS_REGION_JAPAN_NTSC);
  check(genesis_runtime_run(jp2.get(), region_check_game, 16u).kind == GENESIS_COMPLETE && jp2->d[0] == 2u, "Japan NTSC console reports its own lockout for an overseas-only image");
}

// ---- controllers ---------------------------------------------------------------------------------------------------
uint32_t read_port(GenesisRuntime &r, uint32_t address) {
  uint32_t value = 0xDEADBEEFu;
  GenesisRuntimeStop stop{};
  check(genesis_route_access(&r, address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK, "controller data port read is routed");
  return value;
}
void write_port(GenesisRuntime &r, uint32_t address, uint32_t value) {
  GenesisRuntimeStop stop{};
  check(genesis_route_access(&r, address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK, "controller port write is routed");
}

void test_independent_player_ports() {
  auto r = make_runtime();
  // TH pins are outputs (CTRL1/CTRL2 bit 6 = 1); DATA bit 6 selects the TH level.
  write_port(*r, 0xA10009, 0x40);
  write_port(*r, 0xA1000B, 0x40);
  write_port(*r, 0xA10003, 0x40);
  write_port(*r, 0xA10005, 0x40);
  genesis_runtime_set_pad1(r.get(), GENESIS_PAD_UP | GENESIS_PAD_A);
  genesis_runtime_set_pad2(r.get(), GENESIS_PAD_DOWN | GENESIS_PAD_START);
  // TH = 1: "?1CBRLDU": 0x40 | active-low (C B R L D U). P1 pressed Up; P2 pressed Down.
  check(read_port(*r, 0xA10003) == 0x7Eu, "Player 1, TH=1: only Up is pressed (0x7E)");
  check(read_port(*r, 0xA10005) == 0x7Du, "Player 2, TH=1: only Down is pressed (0x7D), independent of Player 1");
  // TH = 0: "?0SA00DU": Player 1 holds Up and A, Player 2 holds Down and Start.
  write_port(*r, 0xA10003, 0x00);
  write_port(*r, 0xA10005, 0x00);
  check(read_port(*r, 0xA10003) == 0x22u, "Player 1, TH=0: Up and A pressed (0x22)");
  check(read_port(*r, 0xA10005) == 0x11u, "Player 2, TH=0: Down and Start pressed (0x11)");
  // Changing one pad never changes the other port.
  genesis_runtime_set_pad2(r.get(), 0);
  check(read_port(*r, 0xA10003) == 0x22u && read_port(*r, 0xA10005) == 0x33u, "releasing Player 2 leaves Player 1 untouched; a released port reads all-released");
  genesis_runtime_set_pad1(r.get(), 0);
  genesis_runtime_set_pad2(r.get(), GENESIS_PAD_B | GENESIS_PAD_C | GENESIS_PAD_LEFT | GENESIS_PAD_RIGHT);
  write_port(*r, 0xA10003, 0x40);
  write_port(*r, 0xA10005, 0x40);
  check(read_port(*r, 0xA10003) == 0x7Fu && read_port(*r, 0xA10005) == 0x43u, "Player 2 B/C/Left/Right at TH=1 (0x43) while Player 1 is released (0x7F)");
  genesis_runtime_set_pad1(nullptr, 1);  // null-safe host calls
  genesis_runtime_set_pad2(nullptr, 1);
}
}  // namespace

int main() {
  test_hint_counter_schedule();
  test_hint_enable_gating_and_latch();
  test_hint_sr_mask_and_priority();
  test_hint_missing_handler_fails_closed();
  test_hint_stop_wake();
  test_region_profiles_and_version_register();
  test_region_sensitive_guest_branches();
  test_independent_player_ports();
  if (failures != 0) std::printf("%d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
