// SEG-138-T001/T002 (ADR 0297): the Genesis mod phase scheduler and its work-RAM/input services, driven by a native ABI 2 trace provider on a project-authored synthetic program
// (no ROM, no host clock). The Luau adapter is checked against the same scheduler by luau_phase_scheduler_test.cpp.
#include "mod_scheduler.h"
#include "runtime.h"

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

// ---- the synthetic program: one 4-cycle "instruction" per dispatch. The guest configures controller port 1 once, then reads its pad byte on every dispatch and keeps the
// last byte it saw in d[1] and a running sum in d[2]; it also writes a counter into work RAM so the guest state depends on mod writes. ----
std::vector<std::pair<uint64_t, uint8_t>> g_pad_seen;   // (frame, pad byte) the first time the guest reads in each frame
uint64_t g_dispatches = 0;

GenesisControlTransfer synthetic_dispatch(GenesisRuntime *rt) {
  ++g_dispatches;
  GenesisRuntimeStop stop{};
  uint32_t value = 0;
  if (rt->d[7] == 0U) {
    value = 0x40U;
    (void)genesis_route_access(rt, 0xA10009U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop);   // CTRL1: TH is an output
    value = 0x40U;
    (void)genesis_route_access(rt, 0xA10003U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop);   // DATA1: TH = 1
    rt->d[7] = 1U;
  }
  value = 0;
  (void)genesis_route_access(rt, 0xA10003U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop);
  rt->d[1] = value;
  rt->d[2] += value;
  if (g_pad_seen.empty() || g_pad_seen.back().first != rt->virtual_frame_count) g_pad_seen.emplace_back(rt->virtual_frame_count, static_cast<uint8_t>(value));
  rt->work_ram[0x10] = static_cast<uint8_t>(rt->work_ram[0x10] + 1U);   // guest-owned byte outside every declared mod range
  rt->work_ram[0x120] = static_cast<uint8_t>(rt->work_ram[0x120] + 1U); // guest-owned byte INSIDE the mod's range (the mod may overwrite it)
  return genesis_runtime_retire_m68k_instruction(rt, 4U, rt->pc);
}

void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  r.devices.vdp.registers[1] = 0x44;   // display enabled, interrupts off: the frame boundary is a pure virtual boundary
  r.devices.vdp.registers[12] = 0x81;
  g_pad_seen.clear();
  g_dispatches = 0;
}

// Runs until `frames` virtual frame boundaries were crossed (checked after every slice of dispatches).
void run_frames(GenesisRuntime &r, uint64_t frames) {
  while (r.virtual_frame_count < frames) {
    const GenesisControlTransfer t = genesis_runtime_run(&r, synthetic_dispatch, 997U);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) {
      check(false, "the synthetic program stopped unexpectedly");
      return;
    }
  }
}

std::string guest_digest(const GenesisRuntime &r) {
  GenesisSha256 sha;
  uint8_t out[32];
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, r.work_ram, sizeof(r.work_ram));
  for (uint32_t v : r.d) {
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24U), static_cast<uint8_t>(v >> 16U), static_cast<uint8_t>(v >> 8U), static_cast<uint8_t>(v)};
    genesis_sha256_update(&sha, b, 4U);
  }
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(&r.scheduler.master_ticks), sizeof(r.scheduler.master_ticks));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(&r.virtual_frame_count), sizeof(r.virtual_frame_count));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(r.devices.vdp.registers), sizeof(r.devices.vdp.registers));
  genesis_sha256_final(&sha, out);
  static const char *hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : out) {
    s += hex[b >> 4U];
    s += hex[b & 15U];
  }
  return s;
}

// ---- the scripted native provider ----
struct Op {
  enum Kind { read, write, input_get, input_set, log } kind;
  uint32_t space = SEGA_GEN_SPACE_M68K_WORK_RAM, offset = 0, width = 1, value = 0;
};
struct Trace {
  uint32_t phase;
  uint64_t frame;
  uint64_t ticks;
  uint64_t dispatches;
};
struct Script {
  uint32_t phase = 0;      // run `ops` in this phase
  uint64_t frame = 0;
  std::vector<Op> ops;
  bool done = false;
};
struct Probe {
  std::vector<Trace> trace;
  std::vector<int> status;
  std::vector<uint32_t> values;
  std::vector<std::string> lines;
  GenesisRuntime *runtime = nullptr;
  Script script;
  int init_result = 0;
  int phase_calls_after_fault = 0;
} g_probe;

int probe_init(void *, const SegaModHost *) { return g_probe.init_result; }

void probe_phase(void *, const SegaModHost *host) {
  g_probe.trace.push_back({host->phase, host->frame, g_probe.runtime->scheduler.master_ticks, g_dispatches});
  Script &s = g_probe.script;
  if (s.done || s.phase != host->phase || s.frame != host->frame) return;
  s.done = true;
  for (const Op &op : s.ops) {
    uint32_t v = 0;
    int st = SEGA_MOD_OK;
    switch (op.kind) {
      case Op::read: st = host->read(op.space, op.offset, op.width, &v); break;
      case Op::write: st = host->write(op.space, op.offset, op.width, op.value); break;
      case Op::input_get: st = host->input_get(op.offset, &v); break;
      case Op::input_set: st = host->input_set(op.offset, op.value); break;
      case Op::log: host->log("line", op.width); break;
    }
    g_probe.status.push_back(st);
    g_probe.values.push_back(v);
  }
}

SegaModProvider make_provider() {
  SegaModProvider p;
  std::memset(&p, 0, sizeof(p));
  p.size = sizeof(SegaModProvider);
  p.abi_major = SEGA_MOD_ABI_MAJOR;
  p.required_minor = 0;
  p.platform = SEGA_MOD_PLATFORM_GENESIS;
  p.name = "trace";
  p.implementation_version = 1;
  p.init = probe_init;
  p.phase = probe_phase;
  return p;
}

constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint32_t kAllPhases = bit(SEGA_MOD_PHASE_RESET) | bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END) |
                                bit(SEGA_MOD_PHASE_RENDER);
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
constexpr uint64_t kMemCaps = cap(SEGA_MOD_CAP_HOST_LOG) | cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE) | cap(SEGA_MOD_CAP_INPUT_READ) | cap(SEGA_MOD_CAP_INPUT_INTERCEPT);

const GenesisModRange kRanges[] = {
    {SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE},   // [0x100, 0x200) read + write
    {SEGA_GEN_SPACE_M68K_WORK_RAM, 0x400U, 0x10U, GENESIS_MOD_ACCESS_READ},                               // [0x400, 0x410) read only
};

struct Harness {
  std::unique_ptr<GenesisRuntime> rt_heap{new GenesisRuntime};   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &rt = *rt_heap;
  GenesisModScheduler *sched = new GenesisModScheduler;
  SegaModProvider provider = make_provider();
  std::vector<std::string> sink;
  const char *rejection = "";
  int attach_result = -1;
  ~Harness() { delete sched; }
  static void log_sink(void *ctx, const char *line, uint32_t len) { static_cast<Harness *>(ctx)->sink.emplace_back(line, len); }
  std::vector<GenesisModRange> custom;   // used instead of kRanges when non-empty
  void init(uint32_t mask, uint64_t required = kMemCaps, bool with_ranges = true) {
    setup(rt);
    g_probe = Probe{};
    g_probe.runtime = &rt;
    GenesisModConfig c{};
    c.mod_id = "trace-mod";
    c.provider = &provider;
    c.phase_mask = mask;
    c.required[0] = required;
    c.ranges = !custom.empty() ? custom.data() : (with_ranges ? kRanges : nullptr);
    c.range_count = !custom.empty() ? static_cast<uint32_t>(custom.size()) : (with_ranges ? 2U : 0U);
    c.log_sink = log_sink;
    c.log_ctx = this;
    attach_result = genesis_mod_scheduler_attach(sched, &rt, &c, &rejection);
  }
};

// ---- T001: phase order, zero guest time ----
void phase_order_and_zero_guest_time() {
  Harness h;
  h.init(kAllPhases);
  check(h.attach_result == 0, "attach");
  run_frames(h.rt, 3U);
  const std::vector<Trace> &t = g_probe.trace;
  std::vector<std::pair<uint32_t, uint64_t>> expected = {{SEGA_MOD_PHASE_RESET, 0}};
  for (uint64_t f = 0; f < 3; ++f) {
    expected.push_back({SEGA_MOD_PHASE_FRAME_BEGIN, f});
    expected.push_back({SEGA_MOD_PHASE_INPUT, f});
    expected.push_back({SEGA_MOD_PHASE_POST_INPUT, f});
    expected.push_back({SEGA_MOD_PHASE_FRAME_END, f});
    expected.push_back({SEGA_MOD_PHASE_RENDER, f});
  }
  for (uint32_t p : {SEGA_MOD_PHASE_FRAME_BEGIN, SEGA_MOD_PHASE_INPUT, SEGA_MOD_PHASE_POST_INPUT}) expected.push_back({p, 3});
  check(t.size() == expected.size(), "trace length " + std::to_string(t.size()) + " vs " + std::to_string(expected.size()));
  for (std::size_t i = 0; i < t.size() && i < expected.size(); ++i)
    check(t[i].phase == expected[i].first && t[i].frame == expected[i].second, "phase " + std::to_string(i) + " is " + genesis_mod_phase_name(t[i].phase) + "(" + std::to_string(t[i].frame) + ")");
  check(t[0].dispatches == 0U && t[1].dispatches == 0U && t[3].dispatches == 0U, "reset/frame-begin/post-input of frame 0 run before the first dispatch");
  check(t[4].dispatches > 20000U, "frame-end(0) runs only after the frame dispatches, got " + std::to_string(t[4].dispatches));
  // The boundary callbacks (frame-end, render, frame-begin, input, post-input) happen on one dispatch boundary: same dispatch count, same guest clock.
  for (std::size_t i = 4; i + 4 < t.size(); i += 5) {
    check(t[i].ticks == t[i + 1].ticks && t[i].ticks == t[i + 2].ticks && t[i].ticks == t[i + 4].ticks && t[i].dispatches == t[i + 4].dispatches, "one boundary, zero guest time (" + std::to_string(i) + ")");
  }
  check(h.rt.scheduler.master_ticks == g_dispatches * 28U, "callbacks never advanced the guest clock (7 master ticks per cycle x 4 cycles per dispatch)");
  check(!h.sched->fault.faulted && h.sched->callbacks_run == t.size() + 1U, "no fault; every callback counted (init plus the phases)");
}

// ---- T001: unregistered phases are not called; init is called once, before reset ----
void phase_registration() {
  Harness h;
  h.init(bit(SEGA_MOD_PHASE_FRAME_END) | bit(SEGA_MOD_PHASE_RENDER));
  run_frames(h.rt, 2U);
  bool only = true;
  for (const Trace &t : g_probe.trace) only = only && (t.phase == SEGA_MOD_PHASE_FRAME_END || t.phase == SEGA_MOD_PHASE_RENDER);
  check(only && g_probe.trace.size() == 4U, "only the registered phases run");
  Harness bad;
  bad.init(bit(SEGA_MOD_PHASE_HOOK_ENTRY));
  check(bad.attach_result == 1 && std::string(bad.rejection) == "E_PHASE_UNSUPPORTED", "a hook phase is refused (SEG-138-T004)");
  bad.init(0U);
  check(bad.attach_result == 1 && std::string(bad.rejection) == "E_PHASE_UNSUPPORTED", "an empty phase set is refused");
  bad.init(bit(0));
  check(bad.attach_result == 1 && std::string(bad.rejection) == "E_PHASE_UNSUPPORTED", "phase bit 0 is refused");
  bad.init(kAllPhases, cap(SEGA_MOD_CAP_HOOK_FUNCTION));
  check(bad.attach_result == 1 && std::string(bad.rejection) == "E_CAPABILITY_UNSUPPORTED", "a required capability the scheduler cannot grant is never dropped");
  check(bad.rt.frame_hooks == nullptr, "a rejected attach leaves the runtime untouched");
  bad.provider.platform = SEGA_MOD_PLATFORM_MASTER_SYSTEM;
  bad.init(kAllPhases);
  check(bad.attach_result == 1 && std::string(bad.rejection) == "E_ABI_PLATFORM", "another platform's provider is refused");
  Harness twin;
  twin.init(kAllPhases);
  GenesisModConfig c{};
  c.mod_id = "second";
  c.provider = &twin.provider;
  c.phase_mask = kAllPhases;
  GenesisModScheduler *other = new GenesisModScheduler;
  const char *why = "";
  check(genesis_mod_scheduler_attach(other, &twin.rt, &c, &why) == 1 && std::string(why) == "E_SCHEDULER_BUSY", "one scheduler per runtime (single mod)");
  delete other;
  // init failure disables the mod before reset and is a stable fault
  Harness failing;
  failing.init(kAllPhases);
  g_probe.init_result = 1;
  run_frames(failing.rt, 1U);
  check(failing.sched->disabled && failing.sched->fault.faulted && std::string(failing.sched->fault.cause) == "provider" && g_probe.trace.empty(), "a failing init disables the mod; no phase runs");
}

// ---- T001: render is read-only; the violation is a deterministic fault ----
std::string render_violation(const Op &op, uint64_t required, std::string &cause, bool ranges = true) {
  Harness h;
  h.init(kAllPhases, required, ranges);
  g_probe.script.phase = SEGA_MOD_PHASE_RENDER;
  g_probe.script.frame = 0;
  g_probe.script.ops = {op};
  run_frames(h.rt, 3U);
  cause = h.sched->fault.cause != nullptr ? h.sched->fault.cause : "";
  // after the fault the mod never runs again
  uint64_t after = 0;
  for (const Trace &t : g_probe.trace)
    if (t.frame > 0 && (t.phase == SEGA_MOD_PHASE_RENDER || t.phase == SEGA_MOD_PHASE_FRAME_END)) ++after;
  check(after == 0U || !h.sched->fault.faulted, "no callback runs after the fault");
  check(h.sched->disabled == h.sched->fault.faulted, "fault disables the mod");
  return std::string(h.sched->fault.text) + "|" + guest_digest(h.rt);
}

void render_phase_is_read_only() {
  std::string cause;
  const Op write{Op::write, SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 2, 0xBEEF}, set{Op::input_set, 0, 1, 1, 0x01}, get{Op::input_get, 0, 1, 1, 0};
  const std::string first = render_violation(write, kMemCaps, cause);
  check(cause == "phase", "a work-RAM write in render faults with cause phase");
  check(first.rfind("E_MOD_FAULT cause=phase mod=trace-mod phase=render frame=0|", 0) == 0, "stable diagnostic text: " + first.substr(0, 60));
  check(render_violation(write, kMemCaps, cause) == first, "the render violation is deterministic (diagnostic and guest state identical)");
  check(render_violation(set, kMemCaps, cause) != "" && cause == "phase", "input_set in render faults with cause phase");
  render_violation(get, kMemCaps, cause);
  check(cause == "phase", "input_get in render faults with cause phase");
  render_violation({Op::read, SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 2, 0}, kMemCaps, cause);
  check(cause == "capability", "a render read needs frame.work_ram (the copy), not memory.read: capability fault");
  render_violation(write, cap(SEGA_MOD_CAP_HOST_LOG), cause, false);
  check(cause == "capability", "without memory.write the same write is a capability fault");
  // The write never reached guest memory: the guest digest equals a render that did nothing at frame 0 up to the fault.
  Harness plain;
  plain.init(kAllPhases);
  run_frames(plain.rt, 3U);
  check(guest_digest(plain.rt) + "" != "", "baseline run");
  // render may read the post-frame-end copy when frame.work_ram is granted
  Harness copy;
  copy.init(kAllPhases, kMemCaps | cap(SEGA_MOD_CAP_FRAME_WORK_RAM));
  g_probe.script.phase = SEGA_MOD_PHASE_RENDER;
  g_probe.script.ops = {{Op::read, SEGA_GEN_SPACE_M68K_WORK_RAM, 0x10, 1, 0}};
  run_frames(copy.rt, 1U);
  for (const Trace &t : g_probe.trace)
    if (t.phase == SEGA_MOD_PHASE_RENDER) g_probe.trace.assign(1, t);
  check(!copy.sched->fault.faulted && g_probe.status.size() == 1 && g_probe.status[0] == SEGA_MOD_OK && g_probe.values[0] == (g_probe.trace.back().dispatches & 0xFFU), "render reads the frame copy taken at the boundary");
  // The frame.work_ram grant is the complete 64 KiB copy (ABI 2 section 4), independent of declared ranges; the copy bound and alignment still hold.
  struct SnapshotCase { uint32_t offset, width; const char *cause; } snapshot[] = {
      {0xFFFC, 4, ""},            // inside the copy, outside every declared range
      {0x0200, 2, ""},            // just above the declared range
      {0xFFFE, 4, "range"},       // straddles the end of the copy
      {0x10000, 1, "range"},      // beyond the copy
      {0xFFFFFFFE, 4, "range"},   // overflow
      {0x0101, 2, "alignment"},   // odd word
  };
  for (const SnapshotCase &c : snapshot) {
    Harness x;
    x.init(kAllPhases, kMemCaps | cap(SEGA_MOD_CAP_FRAME_WORK_RAM));
    g_probe.script.phase = SEGA_MOD_PHASE_RENDER;
    g_probe.script.ops = {{Op::read, SEGA_GEN_SPACE_M68K_WORK_RAM, c.offset, c.width, 0}};
    run_frames(x.rt, 2U);
    const std::string got = x.sched->fault.faulted ? x.sched->fault.cause : "";
    check(got == c.cause, "render snapshot read at " + std::to_string(c.offset) + " w" + std::to_string(c.width) + ": '" + got + "' want '" + c.cause + "'");
    if (got.empty()) continue;
    // the fault disables the mod and the guest continues exactly as Vanilla
    auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
    GenesisRuntime &vanilla = *vanilla_heap;
    setup(vanilla);
    run_frames(vanilla, 2U);
    check(x.sched->disabled && guest_digest(x.rt) == guest_digest(vanilla), "a faulted render read leaves the guest equal to Vanilla");
  }
}

// ---- T001: vanilla runs are bit-identical with a scheduler attached ----
void vanilla_is_bit_identical() {
  auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla = *vanilla_heap;
  setup(vanilla);
  run_frames(vanilla, 4U);
  const std::string base = guest_digest(vanilla);
  const auto seen_vanilla = g_pad_seen;
  for (const uint32_t mask : {kAllPhases, kAllPhases & ~bit(SEGA_MOD_PHASE_INPUT), bit(SEGA_MOD_PHASE_RENDER)}) {
    Harness h;
    h.init(mask);
    run_frames(h.rt, 4U);
    check(guest_digest(h.rt) == base, "a no-op trace provider (mask " + std::to_string(mask) + ") leaves the guest bit-identical to Vanilla");
    check(g_pad_seen == seen_vanilla, "the guest read the same pad bytes");
  }
  // detached again: the runtime is back to the Vanilla seam
  Harness d;
  d.init(kAllPhases);
  genesis_mod_scheduler_detach(d.sched);
  check(d.rt.frame_hooks == nullptr && d.rt.mod_pad_active == 0U, "detach restores the Vanilla seam");
}

// ---- T001: input interception ----
void input_interception() {
  Harness h;
  h.init(kAllPhases);
  h.rt.pad1 = GENESIS_PAD_RIGHT;
  g_probe.script.phase = SEGA_MOD_PHASE_INPUT;
  g_probe.script.frame = 1;
  g_probe.script.ops = {{Op::input_get, 0, 1, 0, 0}, {Op::input_set, 0, 1, 0, GENESIS_PAD_UP | GENESIS_PAD_A}, {Op::input_get, 0, 1, 0, 0}, {Op::input_get, 0, 2, 0, 0}};
  run_frames(h.rt, 3U);
  check(g_probe.status.size() == 4U && g_probe.status[0] == 0 && g_probe.status[1] == 0, "input services answer in the input phase");
  check(g_probe.values[0] == GENESIS_PAD_RIGHT && g_probe.values[2] == (GENESIS_PAD_UP | GENESIS_PAD_A) && g_probe.values[3] == 0U, "get sees the sample, then the staged change; port 2 untouched");
  // frame 0 saw the physical state (no change), frame 1 the intercepted one, frame 2 again the physical state (the mod only changes frame 1)
  uint8_t seen[3] = {0xFF, 0xFF, 0xFF};
  for (const auto &p : g_pad_seen)
    if (p.first < 3U) seen[p.first] = p.second;
  const uint8_t released = 0x7FU;   // TH=1, every direction/button line released (active low)
  check(seen[0] == released - 0x08U, "frame 0: the guest reads RIGHT (physical)");
  check(seen[1] == released - 0x01U - 0x00U, "frame 1: the guest reads UP only (intercepted; A needs TH=0 and is not on this read) got " + std::to_string(seen[1]));
  check(seen[2] == released - 0x08U, "frame 2: the physical state again");
  check(h.rt.pad1 == GENESIS_PAD_RIGHT, "the physical pad is retained for recordings");
  // post-input sees the FINAL input; frame-begin the previous frame's final
  Harness f;
  f.init(kAllPhases);
  f.rt.pad1 = GENESIS_PAD_LEFT;
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  g_probe.script.frame = 1;
  g_probe.script.ops = {{Op::input_get, 0, 1, 0, 0}};
  run_frames(f.rt, 2U);
  check(g_probe.values.size() == 1U && g_probe.values[0] == GENESIS_PAD_LEFT, "frame-begin reads the previous frame's final input");
  // violations are faults
  struct Case { uint32_t phase; Op op; uint64_t caps; const char *cause; } cases[] = {
      {SEGA_MOD_PHASE_POST_INPUT, {Op::input_set, 0, 1, 0, 1}, kMemCaps, "phase"},
      {SEGA_MOD_PHASE_FRAME_BEGIN, {Op::input_set, 0, 1, 0, 1}, kMemCaps, "phase"},
      {SEGA_MOD_PHASE_INPUT, {Op::input_set, 0, 1, 0, 1}, kMemCaps & ~cap(SEGA_MOD_CAP_INPUT_INTERCEPT), "capability"},
      {SEGA_MOD_PHASE_INPUT, {Op::input_get, 0, 1, 0, 0}, kMemCaps & ~cap(SEGA_MOD_CAP_INPUT_READ), "capability"},
      {SEGA_MOD_PHASE_INPUT, {Op::input_set, 0, 1, 0, 0x100}, kMemCaps, "range"},
      {SEGA_MOD_PHASE_INPUT, {Op::input_set, 0, 3, 0, 1}, kMemCaps, "range"},
      {SEGA_MOD_PHASE_RESET, {Op::input_get, 0, 1, 0, 0}, kMemCaps, "phase"},
  };
  for (const Case &c : cases) {
    Harness x;
    x.init(kAllPhases, c.caps);
    g_probe.script.phase = c.phase;
    g_probe.script.ops = {c.op};
    run_frames(x.rt, 1U);
    check(x.sched->fault.faulted && std::string(x.sched->fault.cause) == c.cause, std::string("input violation in ") + genesis_mod_phase_name(c.phase) + " -> " + c.cause);
    check(x.rt.mod_pad_active == 0U, "a faulted mod no longer intercepts");
  }
  // an input fault discards the staged change: the guest reads the physical state
  Harness y;
  y.init(kAllPhases);
  y.rt.pad1 = GENESIS_PAD_RIGHT;
  g_probe.script.phase = SEGA_MOD_PHASE_INPUT;
  g_probe.script.ops = {{Op::input_set, 0, 1, 0, GENESIS_PAD_UP}, {Op::input_set, 0, 1, 0, 0x1FF}};
  run_frames(y.rt, 2U);
  check(g_pad_seen[0].second == 0x7FU - 0x08U, "a faulted callback's staged input is discarded");
}

// ---- T002: bounded, endian-aware, capability-scoped work-RAM access ----
void wram_endianness_and_staging() {
  Harness h;
  h.init(kAllPhases);
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  g_probe.script.ops = {{Op::write, 1, 0x100, 4, 0x11223344U}, {Op::write, 1, 0x110, 2, 0xA1B2U}, {Op::write, 1, 0x121, 1, 0x5AU}, {Op::read, 1, 0x100, 4, 0},
                        {Op::read, 1, 0x100, 2, 0}, {Op::read, 1, 0x102, 2, 0}, {Op::read, 1, 0x103, 1, 0}, {Op::read, 1, 0x110, 2, 0}};
  g_dispatches = 0;
  genesis_runtime_run(&h.rt, synthetic_dispatch, 1U);   // start(): reset + frame-begin(0) run before the first dispatch
  check(!h.sched->fault.faulted, "legal accesses do not fault");
  check(h.rt.work_ram[0x100] == 0x11 && h.rt.work_ram[0x101] == 0x22 && h.rt.work_ram[0x102] == 0x33 && h.rt.work_ram[0x103] == 0x44, "u32 is stored big-endian");
  check(h.rt.work_ram[0x110] == 0xA1 && h.rt.work_ram[0x111] == 0xB2 && h.rt.work_ram[0x121] == 0x5A, "u16 big-endian, u8 verbatim");
  const std::vector<uint32_t> want = {0, 0, 0, 0x11223344U, 0x1122U, 0x3344U, 0x44U, 0xA1B2U};
  check(g_probe.values == want, "reads observe the staged writes of the same callback, big-endian");
  check(h.sched->write_log_count == 3U && h.sched->write_log[0].width == 4U && h.sched->write_log[0].value == 0x11223344U && h.sched->write_log[0].phase == SEGA_MOD_PHASE_FRAME_BEGIN,
        "the write log records the committed writes");
}

struct Matrix {
  const char *name;
  Op op;
  uint32_t phase;
  uint64_t caps;
  const char *cause;   // "" = legal
};

void wram_matrix() {
  const uint32_t phases[] = {SEGA_MOD_PHASE_RESET, SEGA_MOD_PHASE_FRAME_BEGIN, SEGA_MOD_PHASE_INPUT, SEGA_MOD_PHASE_POST_INPUT, SEGA_MOD_PHASE_FRAME_END, SEGA_MOD_PHASE_RENDER};
  int cases = 0;
  for (const uint32_t phase : phases) {
    const bool can_write = phase == SEGA_MOD_PHASE_FRAME_BEGIN || phase == SEGA_MOD_PHASE_POST_INPUT || phase == SEGA_MOD_PHASE_FRAME_END;
    const bool can_read = phase != SEGA_MOD_PHASE_RENDER;
    for (const uint32_t width : {1U, 2U, 4U}) {
      for (const bool is_write : {false, true}) {
        for (const int capset : {0, 1}) {   // 0: capability declared; 1: not declared
          Harness h;
          const uint64_t caps = kMemCaps & ~(capset == 1 ? (is_write ? cap(SEGA_MOD_CAP_MEMORY_WRITE) : cap(SEGA_MOD_CAP_MEMORY_READ)) : 0U);
          if (capset == 1) h.custom = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, is_write ? GENESIS_MOD_ACCESS_READ : GENESIS_MOD_ACCESS_WRITE}};   // the range names only the other mode
          h.init(kAllPhases, caps);
          g_probe.script.phase = phase;
          g_probe.script.ops = {{is_write ? Op::write : Op::read, SEGA_GEN_SPACE_M68K_WORK_RAM, 0x108, width, 0x7Fu}};
          run_frames(h.rt, 1U);
          const char *expected = capset == 1 ? "capability" : (is_write ? (can_write ? "" : "phase") : (can_read ? "" : "capability"));
          const std::string got = h.sched->fault.faulted ? h.sched->fault.cause : "";
          check(got == expected, std::string(is_write ? "write" : "read") + " w" + std::to_string(width) + " in " + genesis_mod_phase_name(phase) + (capset ? " undeclared" : " declared") + ": got '" + got +
                                     "' want '" + expected + "'");
          ++cases;
        }
      }
    }
  }
  check(cases == 72, "matrix size");
  // undeclared or illegal access is a stable class at run time
  struct Bad { Op op; const char *cause; } bad[] = {
      {{Op::read, 1, 0x0FE, 2, 0}, "range"},            // below the declared range
      {{Op::read, 1, 0x200, 1, 0}, "range"},            // just above it
      {{Op::read, 1, 0x1FE, 4, 0}, "range"},            // straddles the end
      {{Op::write, 1, 0x400, 1, 1}, "range"},           // read-only range
      {{Op::write, 1, 0x300, 1, 1}, "range"},           // undeclared
      {{Op::read, 1, 0xFFFE, 2, 0}, "range"},           // work RAM but undeclared
      {{Op::read, 1, 0xFFFFFFFE, 4, 0}, "range"},       // overflow
      {{Op::read, 1, 0x101, 2, 0}, "alignment"},        // odd word
      {{Op::write, 1, 0x102, 4, 0}, ""},                // even long: legal
      {{Op::write, 1, 0x101, 4, 0}, "alignment"},
      {{Op::write, 1, 0x103, 1, 9}, ""},                // byte at an odd address is legal
      {{Op::read, 1, 0x100, 3, 0}, "range"},            // width 3
      {{Op::write, 1, 0x100, 1, 0x100}, "range"},       // value does not fit u8
      {{Op::write, 1, 0x100, 2, 0x10000}, "range"},
      {{Op::write, SEGA_GEN_SPACE_M68K_ROM, 0, 1, 0}, "readonly"},
      {{Op::write, SEGA_GEN_SPACE_Z80_RAM, 0, 1, 0}, "readonly"},
      {{Op::write, SEGA_GEN_SPACE_VDP_VRAM, 0, 2, 0}, "readonly"},
      {{Op::write, SEGA_GEN_SPACE_VDP_CRAM, 0, 2, 0}, "readonly"},
      {{Op::read, SEGA_GEN_SPACE_VDP_VRAM, 0, 2, 0}, "space"},
      {{Op::read, 99, 0, 1, 0}, "space"},
      {{Op::write, 99, 0, 1, 0}, "space"},
  };
  for (const Bad &b : bad) {
    Harness h;
    h.init(kAllPhases);
    g_probe.script.phase = SEGA_MOD_PHASE_FRAME_END;
    g_probe.script.ops = {b.op};
    run_frames(h.rt, 1U);
    const std::string got = h.sched->fault.faulted ? h.sched->fault.cause : "";
    check(got == b.cause, "access (kind " + std::to_string(b.op.kind) + ", space " + std::to_string(b.op.space) + ", off " + std::to_string(b.op.offset) + ", w " + std::to_string(b.op.width) + "): '" + got +
                              "' want '" + b.cause + "'");
    if (!h.sched->fault.faulted) continue;
    check(std::string(h.sched->fault.text).find("phase=frame-end frame=0") != std::string::npos, "the diagnostic names mod, phase and frame");
  }
  // declared ranges are validated at load
  Harness h;
  h.init(kAllPhases);
  const GenesisModRange outside[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0xFF00U, 0x200U, GENESIS_MOD_ACCESS_READ}};
  const GenesisModRange empty[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x10U, 0U, GENESIS_MOD_ACCESS_READ}};
  const GenesisModRange vdp[] = {{SEGA_GEN_SPACE_VDP_VRAM, 0x10U, 4U, GENESIS_MOD_ACCESS_READ}};
  const GenesisModRange nomode[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x10U, 4U, 0U}};
  const GenesisModRange write_without_cap[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x10U, 4U, GENESIS_MOD_ACCESS_WRITE}};
  for (const auto &decl : {std::vector<GenesisModRange>(outside, outside + 1), std::vector<GenesisModRange>(empty, empty + 1), std::vector<GenesisModRange>(vdp, vdp + 1),
                           std::vector<GenesisModRange>(nomode, nomode + 1)}) {
    GenesisModConfig c{};
    c.mod_id = "bad-range";
    c.provider = &h.provider;
    c.phase_mask = kAllPhases;
    c.required[0] = kMemCaps;
    c.ranges = decl.data();
    c.range_count = static_cast<uint32_t>(decl.size());
    GenesisModScheduler *s = new GenesisModScheduler;
    auto r_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
    GenesisRuntime &r = *r_heap;
    setup(r);
    const char *why = "";
    check(genesis_mod_scheduler_attach(s, &r, &c, &why) == 1 && std::string(why) == "E_RANGE_DECLARATION", "an invalid declared range is refused at load");
    delete s;
  }
  GenesisModConfig c{};
  c.mod_id = "no-write-cap";
  c.provider = &h.provider;
  c.phase_mask = kAllPhases;
  c.required[0] = cap(SEGA_MOD_CAP_MEMORY_READ);
  c.ranges = write_without_cap;
  c.range_count = 1;
  GenesisModScheduler *s = new GenesisModScheduler;
  auto r_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &r = *r_heap;
  setup(r);
  const char *why = "";
  check(genesis_mod_scheduler_attach(s, &r, &c, &why) == 1 && std::string(why) == "E_RANGE_DECLARATION", "a write range without memory.write is refused at load");
  delete s;
}

void wram_budgets_and_atomicity() {
  {   // 1024 operations fit, the 1025th faults and NOTHING of that callback is committed
    Harness h;
    h.init(kAllPhases);
    g_probe.script.phase = SEGA_MOD_PHASE_FRAME_END;
    for (uint32_t i = 0; i < 1025U; ++i) g_probe.script.ops.push_back({Op::write, 1, 0x100 + (i % 64U) * 2U, 2, i & 0xFFFFU});
    run_frames(h.rt, 1U);
    check(h.sched->fault.faulted && std::string(h.sched->fault.cause) == "budget", "the 1025th write is E_WRITE_BUDGET (cause budget)");
    check(h.sched->write_log_count == 0U && h.rt.work_ram[0x100] == 0U, "a faulted callback commits nothing (atomic)");
    check(g_probe.status.back() == SEGA_MOD_E_BUDGET && g_probe.status[1023] == SEGA_MOD_OK, "the status of the offending call is E_BUDGET");
  }
  {
    Harness h;
    h.init(kAllPhases);
    g_probe.script.phase = SEGA_MOD_PHASE_FRAME_END;
    for (uint32_t i = 0; i < 1024U; ++i) g_probe.script.ops.push_back({Op::write, 1, 0x100 + (i % 64U) * 2U, 2, i & 0xFFFFU});
    run_frames(h.rt, 1U);
    check(!h.sched->fault.faulted && h.sched->write_log_count == 1024U, "exactly 1024 writes are accepted and logged");
    // later write wins; the log keeps staging order
    check(h.rt.work_ram[0x100] == 0x03 && h.rt.work_ram[0x101] == 0xC0, "the last staged write to an offset wins (value 960)");
  }
  {   // a fault later in the callback discards the earlier writes; the guest continues unmodified
    Harness h;
    h.init(kAllPhases);
    g_probe.script.phase = SEGA_MOD_PHASE_FRAME_END;
    g_probe.script.frame = 1;
    g_probe.script.ops = {{Op::write, 1, 0x100, 4, 0xDEADBEEFU}, {Op::write, 1, 0x300, 1, 1}};
    run_frames(h.rt, 3U);
    check(h.sched->fault.faulted && h.rt.work_ram[0x100] == 0U, "staged writes before the violation are discarded");
    auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
    GenesisRuntime &vanilla = *vanilla_heap;
    setup(vanilla);
    run_frames(vanilla, 3U);
    // The mod made no committed change, so the guest is bit-identical to Vanilla from the fault on.
    check(guest_digest(h.rt) == guest_digest(vanilla), "after the fault the guest continues exactly as Vanilla");
  }
  {   // a mod write is visible to the guest (it changes guest state) and is applied before the next frame
    Harness h;
    h.init(kAllPhases);
    g_probe.script.phase = SEGA_MOD_PHASE_FRAME_END;
    g_probe.script.frame = 0;
    g_probe.script.ops = {{Op::write, 1, 0x120, 1, 0x40}};
    run_frames(h.rt, 2U);
    auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
    GenesisRuntime &vanilla = *vanilla_heap;
    setup(vanilla);
    run_frames(vanilla, 2U);
    check(guest_digest(h.rt) != guest_digest(vanilla), "a committed mod write changes the guest");
  }
}

// ---- T002: replay reproduces the writes byte-identically ----
std::pair<std::string, std::string> replay_run() {
  Harness h;
  h.init(kAllPhases);
  h.rt.pad1 = GENESIS_PAD_B;
  g_probe.script.phase = SEGA_MOD_PHASE_POST_INPUT;
  g_probe.script.frame = 2;
  g_probe.script.ops = {{Op::write, 1, 0x140, 4, 0xCAFEF00DU}, {Op::write, 1, 0x146, 2, 0x1234U}, {Op::write, 1, 0x149, 1, 0x77U}};
  run_frames(h.rt, 4U);
  uint8_t digest[32];
  genesis_mod_write_log_digest(h.sched, digest);
  char hex[65];
  for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return {hex, guest_digest(h.rt)};
}

void replay_is_byte_identical() {
  const auto a = replay_run(), b = replay_run();
  check(a == b && a.first.size() == 64U, "two runs with the same input produce the same write-log digest and guest digest");
  Harness h;
  h.init(kAllPhases);
  uint8_t empty[32];
  genesis_mod_write_log_digest(h.sched, empty);
  uint8_t after[32];
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  g_probe.script.ops = {{Op::write, 1, 0x100, 2, 7}};
  genesis_runtime_run(&h.rt, synthetic_dispatch, 1U);
  genesis_mod_write_log_digest(h.sched, after);
  check(std::memcmp(empty, after, 32) != 0, "a committed write changes the digest");
  uint8_t again[32];
  genesis_mod_write_log_digest(h.sched, again);
  check(std::memcmp(after, again, 32) == 0, "reading the digest does not disturb the stream");
  // a faulted callback's staged writes never enter the digest
  Harness f;
  f.init(kAllPhases);
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  g_probe.script.ops = {{Op::write, 1, 0x100, 2, 7}, {Op::write, 1, 0x300, 1, 1}};
  genesis_runtime_run(&f.rt, synthetic_dispatch, 1U);
  uint8_t faulted[32];
  genesis_mod_write_log_digest(f.sched, faulted);
  check(f.sched->fault.faulted && std::memcmp(faulted, empty, 32) == 0, "uncommitted writes do not affect the digest");
}

// ---- T002: the digest covers EVERY committed write, also those the bounded diagnostic log no longer retains ----
int g_variant = 0;
void many_writes_phase(void *, const SegaModHost *h) {
  if (h->phase != SEGA_MOD_PHASE_FRAME_END) return;
  for (uint32_t i = 0; i < 1024U; ++i) h->write(1, 0x100 + (i % 64U) * 2U, 2, (i + (g_variant != 0 && h->frame >= 4U ? 1U : 0U)) & 0xFFFFU);
}

std::string overflow_digest(int variant, uint32_t &dropped, uint32_t &retained) {
  g_variant = variant;
  Harness h;
  h.init(bit(SEGA_MOD_PHASE_FRAME_END));
  h.provider.phase = many_writes_phase;
  GenesisModConfig c = h.sched->config;
  genesis_mod_scheduler_detach(h.sched);
  c.provider = &h.provider;
  const char *why = "";
  check(genesis_mod_scheduler_attach(h.sched, &h.rt, &c, &why) == 0, "re-attach for the overflow run");
  run_frames(h.rt, 6U);
  dropped = static_cast<uint32_t>(h.sched->write_log_dropped);
  retained = h.sched->write_log_count;
  uint8_t d[32];
  genesis_mod_write_log_digest(h.sched, d);
  char hex[65];
  for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
  return hex;
}

void digest_covers_dropped_records() {
  uint32_t d0 = 0, r0 = 0, d1 = 0, r1 = 0, d2 = 0, r2 = 0;
  const std::string a = overflow_digest(0, d0, r0), b = overflow_digest(1, d1, r1), a2 = overflow_digest(0, d2, r2);
  check(r0 == GENESIS_MOD_WRITE_LOG_MAX && d0 == 6U * 1024U - GENESIS_MOD_WRITE_LOG_MAX && r1 == r0 && d1 == d0, "both histories overflow the bounded log identically (retained " + std::to_string(r0) + ", dropped " + std::to_string(d0) + ")");
  check(a != b, "histories identical in the first 4096 records and in the dropped count, different afterwards, have different digests");
  check(a == a2, "identical complete histories have identical digests");
}

void logging_budget() {
  Harness h;
  h.init(kAllPhases);
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  g_probe.script.ops = {{Op::log, 0, 0, 4, 0}, {Op::log, 0, 0, 4, 0}};
  genesis_runtime_run(&h.rt, synthetic_dispatch, 1U);
  check(h.sink.size() == 2U && h.sink[0] == "line", "log lines of a successful callback reach the sink");
  Harness f;
  f.init(kAllPhases);
  g_probe.script.phase = SEGA_MOD_PHASE_FRAME_BEGIN;
  for (int i = 0; i < 17; ++i) g_probe.script.ops.push_back({Op::log, 0, 0, 4, 0});
  genesis_runtime_run(&f.rt, synthetic_dispatch, 1U);
  check(f.sched->fault.faulted && std::string(f.sched->fault.cause) == "budget" && f.sink.size() == 1U && f.sink[0].rfind("E_MOD_FAULT", 0) == 0, "a 17th log line faults; only the diagnostic reaches the sink");
}

// ---- T001: overhead without mods (recorded as evidence, not a flaky assertion on absolute time) ----
void overhead_without_mods() {
  auto timed = [](bool attach) {
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      Harness h;
      if (attach) h.init(0U + bit(SEGA_MOD_PHASE_RENDER));
      else setup(h.rt);
      const auto t0 = std::chrono::steady_clock::now();
      run_frames(h.rt, 20U);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      best = ms < best ? ms : best;
    }
    return best;
  };
  const double vanilla = timed(false), attached = timed(true);
  std::printf("OVERHEAD_EVIDENCE vanilla_ms=%.3f scheduler_attached_ms=%.3f ratio=%.4f frames=20\n", vanilla, attached, attached / vanilla);
  // Wall-clock time is evidence only (a shared CI host makes any threshold flaky); the structural guarantee is the NULL-by-default seam, and bit-identity is asserted above.
}

}  // namespace

int main() {
  phase_order_and_zero_guest_time();
  phase_registration();
  render_phase_is_read_only();
  vanilla_is_bit_identical();
  input_interception();
  wram_endianness_and_staging();
  wram_matrix();
  wram_budgets_and_atomicity();
  replay_is_byte_identical();
  digest_covers_dropped_records();
  logging_budget();
  overhead_without_mods();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_mod_scheduler_tests: OK\n");
  return 0;
}
