// SEG-138-T001/T002 (ADR 0297): the Luau binding of the Genesis mod phase scheduler. The same scripted behavior is run as a native ABI 2 provider and as a Luau mod under ONE scheduler on a
// project-authored synthetic program; the phase trace, the guest state, the committed write log, the log lines and every violation class must be identical. Synthetic scripts only.
#include "genesis_snapshot.h"
#include "mod_scheduler.h"
#include "runtime.h"
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_phase_provider.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace segarecomp;

namespace {

uint32_t rd_le(const uint8_t *b, unsigned n) { uint32_t v = 0; for (unsigned i = 0; i < n; ++i) v |= static_cast<uint32_t>(b[i]) << (8U * i); return v; }

int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) {
    std::printf("FAIL: %s\n", message.c_str());
    ++failures;
  }
}

// ---- the synthetic program (same shape as genesis_mod_scheduler_test.cpp) ----
GenesisControlTransfer synthetic_dispatch(GenesisRuntime *rt) {
  GenesisRuntimeStop stop{};
  uint32_t value = 0;
  if (rt->d[7] == 0U) {
    value = 0x40U;
    (void)genesis_route_access(rt, 0xA10009U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop);
    value = 0x40U;
    (void)genesis_route_access(rt, 0xA10003U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop);
    rt->d[7] = 1U;
  }
  value = 0;
  (void)genesis_route_access(rt, 0xA10003U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop);
  rt->d[2] += value;
  rt->work_ram[0x10] = static_cast<uint8_t>(rt->work_ram[0x10] + 1U);
  rt->work_ram[0x120] = static_cast<uint8_t>(rt->work_ram[0x120] + 1U);
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
    if (slices > 40U * static_cast<uint32_t>(frames) + 64U) {
      check(false, "run_frames: the virtual frame counter does not advance");
      return;
    }
    const GenesisControlTransfer t = genesis_runtime_run(&r, synthetic_dispatch, 997U);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) {
      check(false, "the synthetic program stopped unexpectedly");
      return;
    }
  }
}

std::string hex(const uint8_t *d, std::size_t n) {
  static const char *h = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += h[d[i] >> 4U];
    s += h[d[i] & 15U];
  }
  return s;
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
  genesis_sha256_final(&sha, out);
  return hex(out, 32);
}

constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
const GenesisModRange kRanges[] = {
    {SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE},
    {SEGA_GEN_SPACE_M68K_WORK_RAM, 0x400U, 0x10U, GENESIS_MOD_ACCESS_READ},
};

// ---- the Luau mod ----
std::vector<std::uint8_t> bytes_of(const std::string &s) { return {s.begin(), s.end()}; }

struct LuauMod {
  mod::Package package;
  luau::CompiledMod compiled;
  std::unique_ptr<luau::PhaseProvider> provider;
  std::string error;
  std::vector<std::string> caps;
};

LuauMod make_luau(const std::string &source, std::vector<std::string> caps, unsigned state_schema = 0) {
  LuauMod m;
  mod::Manifest man;
  man.id = "parity";
  man.version = "1.0.0";
  man.name = "Phase parity";
  man.platform = "genesis";
  man.source_kind = "cartridge-image";
  man.revisions.push_back({"rev-a", std::string(64, 'a'), ""});
  std::sort(caps.begin(), caps.end());
  man.capabilities = caps;
  man.state_schema = state_schema;
  mod::Payload p;
  p.kind = "luau-source";
  p.path = "scripts/main.luau";
  p.size = source.size();
  p.sha256 = sha256_hex(bytes_of(source));
  p.role = "script";
  man.payloads.push_back(p);
  m.package = mod::read_package(mod::pack_package(man, {{"scripts/main.luau", bytes_of(source)}}));
  if (!m.package.ok) {
    std::printf("package rejected: %s %s\n", m.package.rejection.code.c_str(), m.package.rejection.message.c_str());
    std::exit(2);
  }
  const luau::CompileResult c = luau::compile_mod(m.package);
  if (!c.ok) {
    std::printf("compile failed: %s\n", c.rejection.message.c_str());
    std::exit(2);
  }
  m.compiled = c.compiled;
  m.caps = caps;
  luau::Limits limits;
  limits.callback_watchdog_ms = 0;
  limits.frame_watchdog_ms = 0;
  luau::HostConfig host;
  host.capabilities = caps;
  m.provider = luau::PhaseProvider::create(m.package.identity, m.compiled, limits, host, genesis_mod_fault_current, m.error);
  return m;
}

// ---- one run, either provider ----
struct Outcome {
  std::string guest, write_log, fault_cause, fault_text, trace;
  std::vector<std::string> lines;
  std::string rejection;
  bool faulted = false;
  uint64_t write_records = 0;
};

struct LogCollector {
  std::vector<std::string> lines;
  static void sink(void *ctx, const char *line, uint32_t len) { static_cast<LogCollector *>(ctx)->lines.emplace_back(line, len); }
};

Outcome run(const SegaModProvider *provider, void *state, uint32_t mask, uint64_t required[2], const GenesisModRange *ranges, uint32_t range_count, uint64_t frames, uint8_t pad1,
            const std::vector<std::pair<uint32_t, uint64_t>> *trace_out = nullptr) {
  (void)trace_out;
  auto rt_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &rt = *rt_heap;
  setup(rt);
  rt.pad1 = pad1;
  auto *sched = new GenesisModScheduler;
  LogCollector logs;
  GenesisModConfig c{};
  c.mod_id = "parity";
  c.provider = provider;
  c.provider_state = state;
  c.phase_mask = mask;
  c.required[0] = required[0];
  c.required[1] = required[1];
  c.ranges = ranges;
  c.range_count = range_count;
  c.log_sink = LogCollector::sink;
  c.log_ctx = &logs;
  Outcome o;
  const char *why = "";
  if (genesis_mod_scheduler_attach(sched, &rt, &c, &why) != 0) {
    o.rejection = why;
    delete sched;
    return o;
  }
  run_frames(rt, frames);
  uint8_t d[32];
  genesis_mod_write_log_digest(sched, d);
  o.write_log = hex(d, 32);
  o.guest = guest_digest(rt);
  o.faulted = sched->fault.faulted != 0;
  o.fault_cause = sched->fault.cause != nullptr ? sched->fault.cause : "";
  o.fault_text = sched->fault.text;
  o.lines = logs.lines;
  o.write_records = sched->write_log_count;
  delete sched;
  return o;
}

// ---- the native twin of the parity script ----
std::vector<std::string> g_native_trace;
void native_phase(void *, const SegaModHost *h) {
  uint32_t v = 0;
  g_native_trace.push_back(std::string(genesis_mod_phase_name(h->phase)) + ":" + std::to_string(h->frame));
  switch (h->phase) {
    case SEGA_MOD_PHASE_RESET: h->log("reset", 5); break;
    case SEGA_MOD_PHASE_FRAME_BEGIN: {
      h->write(1, 0x100, 4, 0x11223344U + static_cast<uint32_t>(h->frame));
      h->read(1, 0x100, 2, &v);
      const std::string s = "v=" + std::to_string(v);
      h->log(s.data(), static_cast<uint32_t>(s.size()));
      break;
    }
    case SEGA_MOD_PHASE_INPUT:
      if (h->frame == 1) h->input_set(1, GENESIS_PAD_UP);
      break;
    case SEGA_MOD_PHASE_POST_INPUT:
      h->input_get(1, &v);
      h->write(1, 0x110, 2, v);
      break;
    case SEGA_MOD_PHASE_FRAME_END:
      h->read(1, 0x120, 1, &v);
      h->write(1, 0x121, 1, (v + 1U) % 256U);
      break;
    default: break;
  }
}

const char *kParityScript =
    "function reset() sega.log(\"reset\") end\n"
    "function frame_begin(f)\n"
    "  sega.mem.write(\"m68k.work_ram\", 0x100, 4, 0x11223344 + f)\n"
    "  local v = sega.mem.read(\"m68k.work_ram\", 0x100, 2)\n"
    "  sega.log(\"v=\" .. v)\n"
    "end\n"
    "function input(f) if f == 1 then sega.input.set(1, 1) end end\n"
    "function post_input(f)\n"
    "  local p = sega.input.get(1)\n"
    "  sega.mem.write(\"m68k.work_ram\", 0x110, 2, p)\n"
    "end\n"
    "function frame_end(f)\n"
    "  local c = sega.mem.read(\"m68k.work_ram\", 0x120, 1)\n"
    "  sega.mem.write(\"m68k.work_ram\", 0x121, 1, (c + 1) % 256)\n"
    "end\n"
    "function render(f) sega.overlay.text(0, 0, \"F\" .. f, 0xFFFFFFFF) sega.overlay.rect(0, 12, 8, 2, 0xFF0000FF) end\n";

const std::vector<std::string> kParityCaps = {"host.log", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "input.read", "input.intercept", "overlay.rgba"};

void parity_of_the_same_behavior() {
  LuauMod lm = make_luau(kParityScript, kParityCaps);
  check(lm.provider != nullptr, "the parity mod loads: " + lm.error);
  if (!lm.provider) return;
  check(lm.provider->phase_mask() == (bit(SEGA_MOD_PHASE_RESET) | bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END) |
                                         bit(SEGA_MOD_PHASE_RENDER)),
        "the script's defined callbacks are the registered phases");
  uint64_t req[2];
  lm.provider->capabilities(req);
  check((req[0] & cap(SEGA_MOD_CAP_MEMORY_WRITE)) != 0U && (req[0] & cap(SEGA_MOD_CAP_INPUT_INTERCEPT)) != 0U, "capability names map to the catalogue ids");
  const Outcome luau = run(lm.provider->provider(), lm.provider->state(), lm.provider->phase_mask(), req, kRanges, 2U, 4U, GENESIS_PAD_RIGHT);

  SegaModProvider native;
  std::memset(&native, 0, sizeof(native));
  native.size = sizeof(native);
  native.abi_major = SEGA_MOD_ABI_MAJOR;
  native.platform = SEGA_MOD_PLATFORM_GENESIS;
  native.name = "native-twin";
  native.phase = native_phase;
  g_native_trace.clear();
  const Outcome nat = run(&native, nullptr, lm.provider->phase_mask(), req, kRanges, 2U, 4U, GENESIS_PAD_RIGHT);

  check(!luau.faulted && !nat.faulted, "neither run faults: " + luau.fault_text + " / " + nat.fault_text);
  check(luau.guest == nat.guest, "C11 and Luau produce the same guest state");
  check(luau.write_log == nat.write_log && luau.write_records == nat.write_records && nat.write_records >= 10U, "C11 and Luau commit the same writes (" + std::to_string(nat.write_records) + " records)");
  check(luau.lines == nat.lines && !nat.lines.empty(), "C11 and Luau log the same lines");
  // The mod really changed the guest: it differs from Vanilla.
  auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla = *vanilla_heap;
  setup(vanilla);
  vanilla.pad1 = GENESIS_PAD_RIGHT;
  run_frames(vanilla, 4U);
  check(guest_digest(vanilla) != nat.guest, "the mods change the guest");
  // the Luau render produced an overlay through the same scheduler
  check(!lm.provider->overlay().empty(), "the render callback ran in the render phase and produced its overlay");
  // replay: byte-identical
  LuauMod again = make_luau(kParityScript, kParityCaps);
  const Outcome replay = run(again.provider->provider(), again.provider->state(), again.provider->phase_mask(), req, kRanges, 2U, 4U, GENESIS_PAD_RIGHT);
  check(replay.guest == luau.guest && replay.write_log == luau.write_log && replay.lines == luau.lines, "a second Luau run is byte-identical (replay)");
}

// ---- the identical matrix, native vs Luau ----
struct Case {
  const char *what;
  uint32_t phase;
  bool write;
  uint32_t space, offset, width;
  uint32_t value;
  std::vector<std::string> luau_caps;   // declared capabilities (both providers get the same set)
};

const char *space_name(uint32_t id) {
  static const char *const names[] = {"bogus", "m68k.work_ram", "m68k.rom", "z80.ram", "vdp.vram", "vdp.cram", "vdp.vsram"};
  return id < 7U ? names[id] : "bogus";
}

const char *callback_of(uint32_t phase) {
  switch (phase) {
    case SEGA_MOD_PHASE_RESET: return "reset";
    case SEGA_MOD_PHASE_FRAME_BEGIN: return "frame_begin";
    case SEGA_MOD_PHASE_INPUT: return "input";
    case SEGA_MOD_PHASE_POST_INPUT: return "post_input";
    case SEGA_MOD_PHASE_FRAME_END: return "frame_end";
    default: return "render";
  }
}

struct OneOp {
  bool write;
  uint32_t space, offset, width, value;
  uint32_t phase;
} g_one;
int g_one_status = -1;
void one_op_phase(void *, const SegaModHost *h) {
  if (h->phase != g_one.phase) return;
  uint32_t v = 0;
  g_one_status = g_one.write ? h->write(g_one.space, g_one.offset, g_one.width, g_one.value) : h->read(g_one.space, g_one.offset, g_one.width, &v);
}

// Returns the fault cause ("" = none) of the native provider and of the Luau mod for the same single access.
std::pair<std::string, std::string> both(const Case &c) {
  uint64_t req[2] = {0, 0};
  for (const std::string &name : c.luau_caps)
    if (const mod::CapabilityInfo *info = mod::find_capability(name)) req[0] |= UINT64_C(1) << info->id;
  // ranges that name only modes the declared capabilities allow (a range for an undeclared mode is refused at load, tested elsewhere)
  uint32_t modes = 0;
  if ((req[0] & cap(SEGA_MOD_CAP_MEMORY_READ)) != 0U) modes |= GENESIS_MOD_ACCESS_READ;
  if ((req[0] & cap(SEGA_MOD_CAP_MEMORY_WRITE)) != 0U) modes |= GENESIS_MOD_ACCESS_WRITE;
  const GenesisModRange ranges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, modes}, {SEGA_GEN_SPACE_M68K_WORK_RAM, 0x400U, 0x10U, modes & GENESIS_MOD_ACCESS_READ}};
  const uint32_t range_count = modes == 0U ? 0U : (modes & GENESIS_MOD_ACCESS_READ) != 0U ? 2U : 1U;

  g_one = {c.write, c.space, c.offset, c.width, c.value, c.phase};
  SegaModProvider native;
  std::memset(&native, 0, sizeof(native));
  native.size = sizeof(native);
  native.abi_major = SEGA_MOD_ABI_MAJOR;
  native.platform = SEGA_MOD_PLATFORM_GENESIS;
  native.name = "one-op";
  native.phase = one_op_phase;
  const Outcome n = run(&native, nullptr, bit(c.phase), req, ranges, range_count, 1U, 0U);

  std::string stmt;
  if (c.write) stmt = "sega.mem.write(\"" + std::string(space_name(c.space)) + "\", " + std::to_string(c.offset) + ", " + std::to_string(c.width) + ", " + std::to_string(c.value) + ")";
  else stmt = "local v = sega.mem.read(\"" + std::string(space_name(c.space)) + "\", " + std::to_string(c.offset) + ", " + std::to_string(c.width) + ")";
  const std::string source = std::string("function ") + callback_of(c.phase) + "(f) " + stmt + " end\n";
  LuauMod lm = make_luau(source, c.luau_caps);
  if (!lm.provider) {   // an undeclared binding may fault at load: the same stable class
    const std::string cls = lm.error.find("cause=capability") != std::string::npos ? "capability" : lm.error;
    return {n.fault_cause + n.rejection, cls};
  }
  const Outcome l = run(lm.provider->provider(), lm.provider->state(), bit(c.phase), req, ranges, range_count, 1U, 0U);
  return {n.fault_cause + n.rejection, l.fault_cause + l.rejection};
}

void matrix_parity() {
  const std::vector<std::string> rw = {"host.log", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram"};
  const std::vector<std::string> ro = {"host.log", "memory.read.m68k.work_ram"};
  const std::vector<std::string> wo = {"host.log", "memory.write.m68k.work_ram"};
  const uint32_t phases[] = {SEGA_MOD_PHASE_RESET, SEGA_MOD_PHASE_FRAME_BEGIN, SEGA_MOD_PHASE_INPUT, SEGA_MOD_PHASE_POST_INPUT, SEGA_MOD_PHASE_FRAME_END, SEGA_MOD_PHASE_RENDER};
  int compared = 0;
  for (const uint32_t phase : phases)
    for (const uint32_t width : {1U, 2U, 4U})
      for (const bool write : {false, true})
        for (const auto *caps : {&rw, &ro, &wo}) {
          if (phase == SEGA_MOD_PHASE_RENDER && !write) continue;   // render reads: a native provider reads the frame copy; Luau uses sega.frame.value (see luau_render_read_is_a_phase_fault)
          Case c{"matrix", phase, write, 1U, 0x108U, width, 0x7FU, *caps};
          const auto [native, luau] = both(c);
          check(native.rfind("E_", 0) != 0, "matrix attach was accepted (a refusal would make the comparison vacuous)");
          check(native == luau, std::string("matrix ") + callback_of(phase) + (write ? " write" : " read") + " w" + std::to_string(width) + " caps=" + std::to_string(caps->size()) + ": C11 '" + native +
                                    "' Luau '" + luau + "'");
          ++compared;
        }
  check(compared == 99, "matrix size (phase x width x direction x capability set, minus the 9 render reads that are native-only by contract)");
  const std::vector<Case> bad = {
      {"below the range", SEGA_MOD_PHASE_FRAME_END, false, 1, 0x0FE, 2, 0, rw},      {"above the range", SEGA_MOD_PHASE_FRAME_END, false, 1, 0x200, 1, 0, rw},
      {"straddles the end", SEGA_MOD_PHASE_FRAME_END, false, 1, 0x1FE, 4, 0, rw},     {"write to a read-only range", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x400, 1, 1, rw},
      {"undeclared offset", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x300, 1, 1, rw},      {"odd word read", SEGA_MOD_PHASE_FRAME_END, false, 1, 0x101, 2, 0, rw},
      {"odd long write", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x101, 4, 0, rw},         {"odd byte write is legal", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x103, 1, 9, rw},
      {"width 3", SEGA_MOD_PHASE_FRAME_END, false, 1, 0x100, 3, 0, rw},               {"value does not fit u8", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x100, 1, 0x100, rw},
      {"value does not fit u16", SEGA_MOD_PHASE_FRAME_END, true, 1, 0x100, 2, 0x10000, rw}, {"ROM is read-only", SEGA_MOD_PHASE_FRAME_END, true, 2, 0, 1, 0, rw},
      {"Z80 RAM is read-only", SEGA_MOD_PHASE_FRAME_END, true, 3, 0, 1, 0, rw},       {"VRAM is read-only", SEGA_MOD_PHASE_FRAME_END, true, 4, 0, 2, 0, rw},
      {"VRAM read is not provided", SEGA_MOD_PHASE_FRAME_END, false, 4, 0, 2, 0, rw}, {"unknown space read", SEGA_MOD_PHASE_FRAME_END, false, 0, 0, 1, 0, rw},
      {"unknown space write", SEGA_MOD_PHASE_FRAME_END, true, 0, 0, 1, 0, rw},        {"4 GiB offset", SEGA_MOD_PHASE_FRAME_END, false, 1, 0xFFFFFFFEU, 4, 0, rw},
  };
  for (const Case &c : bad) {
    const auto [native, luau] = both(c);
    check(native == luau, std::string("access '") + c.what + "': C11 '" + native + "' Luau '" + luau + "'");
    ++compared;
  }
  // spot-check the classes themselves (so equal-but-wrong cannot pass)
  check(both(bad[0]).first == "range" && both(bad[3]).first == "range" && both(bad[5]).first == "alignment" && both(bad[7]).first.empty() && both(bad[11]).first == "readonly" &&
            both(bad[14]).first == "space",
        "the classes are the documented ones");
}

// ---- Luau-only faults go through the scheduler's fault path: the same E_MOD_FAULT handling ----
void luau_fault_isolation() {
  const char *script =
      "function frame_end(f)\n"
      "  sega.mem.write(\"m68k.work_ram\", 0x100, 4, 0xDEADBEEF)\n"
      "  if f == 1 then error(\"boom\") end\n"
      "end\n";
  LuauMod lm = make_luau(script, {"host.log", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram"});
  check(lm.provider != nullptr, "fault script loads");
  uint64_t req[2];
  lm.provider->capabilities(req);
  const Outcome o = run(lm.provider->provider(), lm.provider->state(), lm.provider->phase_mask(), req, kRanges, 2U, 4U, 0U);
  check(o.faulted && o.fault_cause == "error" && o.fault_text == "E_MOD_FAULT cause=error mod=parity phase=frame-end frame=1", "a script error faults the callback: " + o.fault_text);
  check(o.write_records == 1U, "frame 0's write committed; frame 1's staged write was discarded with the faulting callback");
  auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla = *vanilla_heap;
  setup(vanilla);
  run_frames(vanilla, 4U);
  check(o.guest != guest_digest(vanilla), "the committed frame-0 write is the only residue (documented fault-isolation boundary)");
  // budget exhaustion in a Luau callback
  LuauMod loop = make_luau("function frame_begin(f) while true do end end\n", {"host.log"});
  check(loop.provider != nullptr, "loop script loads");
  uint64_t none[2] = {cap(SEGA_MOD_CAP_HOST_LOG), 0};
  const Outcome b = run(loop.provider->provider(), loop.provider->state(), loop.provider->phase_mask(), none, nullptr, 0U, 2U, 0U);
  check(b.faulted && b.fault_cause == "budget" && b.fault_text.find("phase=frame-begin frame=0") != std::string::npos, "an infinite loop is a deterministic budget fault: " + b.fault_text);
  auto vanilla2_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla2 = *vanilla2_heap;
  setup(vanilla2);
  run_frames(vanilla2, 2U);
  check(b.guest == guest_digest(vanilla2), "after a fault with no committed effect the guest equals Vanilla");
}

// ---- Luau: sega.mem.read in render is a phase fault even with memory.read AND frame.work_ram (the native snapshot read does not authorize the Luau binding) ----
void luau_render_read_is_a_phase_fault() {
  LuauMod lm = make_luau("function render(f) local v = sega.mem.read(\"m68k.work_ram\", 0x108, 2) end\n", {"host.log", "memory.read.m68k.work_ram", "frame.work_ram"});
  check(lm.provider != nullptr, "render-read script loads: " + lm.error);
  if (!lm.provider) return;
  const GenesisModRange kReadRange[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ}};
  uint64_t req[2];
  lm.provider->capabilities(req);
  check((req[0] & cap(SEGA_MOD_CAP_FRAME_WORK_RAM)) != 0U && (req[0] & cap(SEGA_MOD_CAP_MEMORY_READ)) != 0U, "both capabilities are granted");
  const Outcome o = run(lm.provider->provider(), lm.provider->state(), lm.provider->phase_mask(), req, kReadRange, 1U, 3U, 0U);
  check(o.faulted && o.fault_cause == "phase" && o.fault_text == "E_MOD_FAULT cause=phase mod=parity phase=render frame=0", "deterministic phase fault: " + o.fault_text);
  auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla = *vanilla_heap;
  setup(vanilla);
  run_frames(vanilla, 3U);
  check(o.guest == guest_digest(vanilla), "the guest continues unmodified");
  LuauMod again = make_luau("function render(f) local v = sega.mem.read(\"m68k.work_ram\", 0x108, 2) end\n", {"host.log", "memory.read.m68k.work_ram", "frame.work_ram"});
  const Outcome o2 = run(again.provider->provider(), again.provider->state(), again.provider->phase_mask(), req, kReadRange, 1U, 3U, 0U);
  check(o2.fault_text == o.fault_text && o2.guest == o.guest, "the fault is reproducible");
}

// ---- the Luau mod that only observes (render overlay) leaves the guest bit-identical to Vanilla ----
void luau_observer_is_bit_identical() {
  LuauMod lm = make_luau("function render(f) sega.overlay.text(0, 0, \"F\" .. f, 0xFFFFFFFF) end\nfunction frame_end(f) sega.log(\"e\") end\n", {"host.log", "overlay.rgba"});
  check(lm.provider != nullptr, "observer loads");
  uint64_t req[2];
  lm.provider->capabilities(req);
  const Outcome o = run(lm.provider->provider(), lm.provider->state(), lm.provider->phase_mask(), req, nullptr, 0U, 4U, GENESIS_PAD_LEFT);
  auto vanilla_heap = std::make_unique<GenesisRuntime>();   // 150 KB: never on the (1 MB on Windows) stack
  GenesisRuntime &vanilla = *vanilla_heap;
  setup(vanilla);
  vanilla.pad1 = GENESIS_PAD_LEFT;
  run_frames(vanilla, 4U);
  check(!o.faulted && o.guest == guest_digest(vanilla), "an observer mod leaves the guest bit-identical to Vanilla");
}

// ---- SEG-138-T006 (ADR 0299): the Luau mod's one serialized value is its state in the save-state container, through the same scheduler seam as a native provider's blob ----
const char *kStateScript =
    "local n = 0\n"
    "local log = {}\n"
    "function frame_begin(f)\n"
    "  n = n + 1\n"
    "  log[#log + 1] = n\n"
    "  sega.mem.write(\"m68k.work_ram\", 0x100, 4, n)\n"
    "end\n"
    "function save() return { n = n, tail = log[#log] } end\n"
    "function load(v) n = v.n; log = { v.tail } end\n";
const std::vector<std::string> kStateCaps = {"host.log", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "state.mod"};

void luau_state_round_trip() {
  struct Machine {
    LuauMod mod;
    std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();
    std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
    GenesisSnapshotModState snap{};
    GenesisSnapshotMachine machine{};
    GenesisSnapshotIdentity id{};
    uint64_t req[2] = {0, 0};
    explicit Machine(unsigned schema) : mod(make_luau(kStateScript, kStateCaps, schema)) {
      if (!mod.provider) { check(false, "state script loads: " + mod.error); return; }
      setup(*rt);
      mod.provider->capabilities(req);
      GenesisModConfig c{};
      c.mod_id = "parity";
      c.provider = mod.provider->provider();
      c.provider_state = mod.provider->state();
      c.phase_mask = mod.provider->phase_mask();
      c.required[0] = req[0];
      c.ranges = kRanges;
      c.range_count = 1U;
      c.state_schema = schema;
      for (unsigned i = 0; i < 32U; ++i) c.identity_sha256[i] = static_cast<uint8_t>(std::strtoul(mod.package.identity.substr(i * 2U, 2U).c_str(), nullptr, 16));
      const char *why = "";
      check(genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0, std::string("the Luau state mod attaches: ") + why);
      genesis_mod_snapshot_state(sched.get(), &snap);
      machine.runtime = rt.get();
      machine.mod = &snap;
      std::memset(id.source_sha256, 0x33, 32U);
    }
    ~Machine() { genesis_mod_scheduler_detach(sched.get()); }
    std::string hash() {
      GenesisSnapshotBuffer b{nullptr, 0};
      if (genesis_snapshot_capture(&machine, &id, nullptr, &b) != GENESIS_SNAPSHOT_OK) return "capture-failed";
      const std::string h = hex(b.data + b.size - 32U, 32U);
      genesis_snapshot_buffer_free(&b);
      return h;
    }
  };
  Machine a(3);
  if (!a.mod.provider) return;
  run_frames(*a.rt, 6U);
  GenesisSnapshotBuffer s1{nullptr, 0};
  check(genesis_snapshot_capture(&a.machine, &a.id, nullptr, &s1) == GENESIS_SNAPSHOT_OK, "a Luau mod's state captures");
  const std::string saved = a.hash();
  run_frames(*a.rt, 14U);
  const std::string straight = a.hash();
  check(straight != saved, "the Luau state advanced");
  check(genesis_snapshot_restore(&a.machine, &a.id, s1.data, s1.size) == GENESIS_SNAPSHOT_OK, "restore of the Luau mod's saved state");
  check(a.hash() == saved, "save -> run -> restore => identical state hash including the Luau value");
  run_frames(*a.rt, 14U);
  check(a.hash() == straight, "the restored Luau mod continues exactly as the straight run (its restored counter drives the RAM it writes)");
  // another package identity or schema is refused before anything is loaded
  Machine other_schema(4);
  run_frames(*other_schema.rt, 3U);
  const std::string before = other_schema.hash();
  check(genesis_snapshot_restore(&other_schema.machine, &other_schema.id, s1.data, s1.size) == GENESIS_SNAPSHOT_MOD_MISMATCH && other_schema.hash() == before, "a Luau restore with another state schema is E_STATE_MOD_MISMATCH and changes nothing");
  // a crafted blob that the strict deserializer refuses: the whole restore is refused, the guest is untouched, the (now faulted) VM is disabled with cause `state`
  std::vector<uint8_t> bad(s1.data, s1.data + s1.size);
  const uint32_t count = rd_le(bad.data() + 28, 2U);
  uint32_t off = 0, len = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *e = bad.data() + 156U + i * 12U;
    if (rd_le(e, 2U) == 11U) { off = rd_le(e + 4, 4U); len = rd_le(e + 8, 4U); }
  }
  check(len > 76U, "MODSTATE found");
  // the blob is the tail of the section: its length is the u32 that precedes it; the first blob byte is the root type tag, which no deserializer accepts as 0xFF
  uint32_t blob_len = 0;
  for (uint32_t k = 1; k < 200U && blob_len == 0U; ++k) {
    const uint32_t cand = rd_le(&bad[off + len - k - 4U], 4U);
    if (cand == k) blob_len = k;
  }
  check(blob_len != 0U, "the Luau blob was located");
  bad[off + len - blob_len] = 0xFFU;
  GenesisSha256 sha;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, bad.data(), static_cast<uint32_t>(bad.size() - 32U));
  genesis_sha256_final(&sha, bad.data() + bad.size() - 32U);
  uint8_t ram_before[65536];
  std::memcpy(ram_before, a.rt->work_ram, sizeof ram_before);
  const uint64_t frame_before = a.rt->virtual_frame_count;
  const GenesisSnapshotStatus st = genesis_snapshot_restore(&a.machine, &a.id, bad.data(), bad.size());
  check(st == GENESIS_SNAPSHOT_MOD_REJECTED, "a malformed Luau state is E_STATE_MOD_REJECTED (got " + std::to_string(st) + ")");
  check(std::memcmp(ram_before, a.rt->work_ram, sizeof ram_before) == 0 && a.rt->virtual_frame_count == frame_before, "the guest is untouched by the refused Luau state");
  genesis_snapshot_buffer_free(&s1);
}

}  // namespace

int main() {
  luau_state_round_trip();
  parity_of_the_same_behavior();
  matrix_parity();
  luau_fault_isolation();
  luau_render_read_is_a_phase_fault();
  luau_observer_is_bit_identical();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("luau_phase_scheduler_tests: OK\n");
  return 0;
}
