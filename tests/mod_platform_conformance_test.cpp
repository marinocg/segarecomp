// SEG-146-T004 (ADR 0299): the portable Luau and native conformance suite, parameterized by platform.
//
// ONE table-driven suite runs the same scenarios for every registered platform adapter (libs/media/src/mod_adapter.cpp) with synthetic fixtures and asserts what the platform DECLARES it supports:
// either `pass`, or the exact fail-closed diagnostic. A scenario first asks the adapter registry (check_capabilities) whether the platform grants what the scenario needs; if it does not, the
// outcome IS the registry's diagnostic (E_CAPABILITY_UNSUPPORTED). If it does, the platform's runtime executor runs the scenario. Only Genesis has a mod scheduler today; a platform that
// declares support but has no executor yields E_RUNTIME_UNAVAILABLE, so flipping a declaration without the runtime behind it fails the suite (the suite is the progress gate).
// A new platform adds a row to `kRows` (and an executor when it gains a runtime), not a new suite. No commercial inputs, no ROM, no host clock.
#include "genesis_snapshot.h"
#include "mod_scheduler.h"
#include "runtime.h"
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_phase_provider.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace segarecomp;

namespace {

int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) {
    std::printf("FAIL: %s\n", message.c_str());
    ++failures;
  }
}
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
std::string join(const std::vector<std::string> &v) { std::string s; for (const std::string &x : v) s += (s.empty() ? "" : ",") + x; return s; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The declarations: what each platform says it supports. `pass`, or the exact diagnostic. This table is the contract the suite enforces.
// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char *const kScenarios[] = {"capability_negotiation", "sdk_negotiation", "phase_ordering", "typed_guest_access", "mod_state_round_trip", "sandbox_failure", "incompatible_target", "audio_command_mapping"};

struct Row {
  std::string platform;
  std::map<std::string, std::string> expect;                // scenario -> "pass" | diagnostic
  std::map<std::string, std::string> declared_capabilities; // capability -> support level, for every capability that is not `unsupported`
};

const char *const kUnsupported = "E_CAPABILITY_UNSUPPORTED";

std::vector<Row> make_rows() {
  std::vector<Row> rows;
  {   // Genesis: the reference adapter. Every row passes, except the audio-command mapping, which the registry declares but the scheduler does not serve until SEG-145-T008 (then this flips to pass).
    Row r;
    r.platform = "genesis";
    for (const char *s : kScenarios) r.expect[s] = "pass";
    r.expect["audio_command_mapping"] = "E_CAPABILITY_UNSUPPORTED:runtime";
    r.declared_capabilities = {
        {"host.log", "experimental"}, {"frame.work_ram", "supported-with-limitations"}, {"frame.vdp", "supported-with-limitations"}, {"scenery.words", "supported-with-limitations"},
        {"overlay.rgba", "supported-with-limitations"}, {"memory.read.m68k.work_ram", "experimental"}, {"memory.write.m68k.work_ram", "experimental"}, {"input.read", "experimental"},
        {"input.intercept", "experimental"}, {"hook.function", "experimental"}, {"state.mod", "experimental"}, {"audio.event.read", "experimental"}, {"audio.command.send", "experimental"}};
    rows.push_back(r);
  }
  for (const char *platform : {"master-system", "sega-cd"}) {   // lifecycle + host.log only until SEG-147 / SEG-148 raise them
    Row r;
    r.platform = platform;
    for (const char *s : kScenarios) r.expect[s] = kUnsupported;
    for (const char *s : {"capability_negotiation", "sdk_negotiation", "sandbox_failure", "incompatible_target"}) r.expect[s] = "pass";
    r.expect["audio_command_mapping"] = "E_CAPABILITY_UNSUPPORTED:declared";   // flips with SEG-145-T013
    r.declared_capabilities = {{"host.log", "experimental"}};
    rows.push_back(r);
  }
  return rows;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Scenario machinery
// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Scenario {
  const char *name;
  std::function<std::vector<std::string>(const mod::Adapter &)> required;   // what the scenario needs from the platform
  std::function<std::string(const mod::Adapter &)> run;                      // executes it on a platform that grants all of it
};

// ---- the Genesis synthetic program and scheduler rig (the same shape as genesis_mod_scheduler_test.cpp) ----
GenesisControlTransfer synthetic_dispatch(GenesisRuntime *rt) {
  rt->d[2] += 1U;
  rt->work_ram[0x10] = static_cast<uint8_t>(rt->work_ram[0x10] + 1U);
  return genesis_runtime_retire_m68k_instruction(rt, 4U, rt->pc);
}
void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  r.devices.vdp.registers[1] = 0x44;
  r.devices.vdp.registers[12] = 0x81;
}
bool run_to_frame(GenesisRuntime &r, uint64_t frame) {
  for (uint32_t guard = 0; r.virtual_frame_count < frame; ++guard)
    if (guard > 100000U || genesis_runtime_run(&r, synthetic_dispatch, 997U).kind != GENESIS_RUNNER_RESOURCE_LIMIT) return false;
  return true;
}
const GenesisModRange kRanges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE}};

struct Probe {   // what the native provider records
  std::vector<std::string> trace;
  uint32_t n = 0;
  uint32_t read16 = 0, read8 = 0, odd_status = 0;
  bool exercise_access = false;
  bool bad_range = false;
};
void probe_phase(void *state, const SegaModHost *h) {
  auto *p = static_cast<Probe *>(state);
  p->trace.push_back(std::string(genesis_mod_phase_name(h->phase)) + ":" + std::to_string(h->frame));
  if (h->phase == SEGA_MOD_PHASE_FRAME_BEGIN) {
    ++p->n;
    if (p->exercise_access && h->frame == 0U) {
      h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 4, 0x11223344U);
      h->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 2, &p->read16);
      h->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x103, 1, &p->read8);
    } else if (p->bad_range && h->frame == 1U) {
      p->odd_status = static_cast<uint32_t>(h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x800, 1, 1));   // outside every declared range
    }
    h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x110, 4, p->n);
  }
}
int probe_save(void *state, uint8_t *out, uint32_t cap_bytes, uint32_t *len) {
  const auto *p = static_cast<Probe *>(state);
  if (cap_bytes < 4U) return 1;
  for (unsigned i = 0; i < 4U; ++i) out[i] = static_cast<uint8_t>(p->n >> (8U * i));
  *len = 4U;
  return 0;
}
int probe_load(void *state, const uint8_t *in, uint32_t len) {
  auto *p = static_cast<Probe *>(state);
  if (len != 4U) return 1;
  p->n = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) | (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
  return 0;
}

struct GenesisRig {
  std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();
  std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
  Probe probe;
  SegaModProvider provider{};
  std::string rejection;
  std::vector<std::string> log;
  static void sink(void *ctx, const char *line, uint32_t len) { static_cast<GenesisRig *>(ctx)->log.emplace_back(line, len); }
  GenesisSnapshotModState snap{};
  GenesisSnapshotMachine machine{};
  GenesisSnapshotIdentity id{};
  bool attach(uint32_t phases, uint64_t caps, bool with_state) {
    setup(*rt);
    provider.size = sizeof provider;
    provider.abi_major = SEGA_MOD_ABI_MAJOR;
    provider.platform = SEGA_MOD_PLATFORM_GENESIS;
    provider.name = "conformance";
    provider.implementation_version = 1;
    provider.phase = probe_phase;
    if (with_state) { provider.save_state = probe_save; provider.load_state = probe_load; }
    GenesisModConfig c{};
    c.mod_id = "conformance";
    c.provider = &provider;
    c.provider_state = &probe;
    c.phase_mask = phases;
    c.required[0] = caps;
    c.ranges = kRanges;
    c.range_count = 1U;
    c.state_schema = with_state ? 1U : 0U;
    c.log_sink = sink;
    c.log_ctx = this;
    const char *why = "";
    const bool ok = genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0;
    rejection = why;
    genesis_mod_snapshot_state(sched.get(), &snap);
    machine.runtime = rt.get();
    machine.mod = &snap;
    std::memset(id.source_sha256, 0x77, 32U);
    return ok;
  }
  ~GenesisRig() { genesis_mod_scheduler_detach(sched.get()); }
  std::string hash() {
    GenesisSnapshotBuffer b{nullptr, 0};
    if (genesis_snapshot_capture(&machine, &id, nullptr, &b) != GENESIS_SNAPSHOT_OK) return "capture-failed";
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint32_t i = b.size - 32U; i < b.size; ++i) { s += h[b.data[i] >> 4U]; s += h[b.data[i] & 15U]; }
    genesis_snapshot_buffer_free(&b);
    return s;
  }
};

std::string genesis_only(const mod::Adapter &a, const std::function<std::string()> &run) {
  return a.platform == "genesis" ? run() : "E_RUNTIME_UNAVAILABLE";   // declared support without a runtime executor is a failing declaration
}

// ---- portable Luau sandbox: the same VM on every platform, with only the capabilities the platform grants (host.log) ----
struct LuauProbe {
  std::string cause;
  std::vector<std::string> lines;
  bool ok = false;
};
LuauProbe g_luau;
void luau_sink(const char *cause) { g_luau.cause = cause; }
void luau_log(const char *s, uint32_t n) { g_luau.lines.emplace_back(s, n); }

LuauProbe luau_run_reset(const std::string &source) {
  g_luau = LuauProbe{};
  mod::Manifest man;
  man.id = "portable";
  man.version = "1.0.0";
  man.name = "Portable";
  man.platform = "genesis";   // the manifest platform only labels the package; the VM and the host log API are platform independent
  man.source_kind = "cartridge-image";
  man.revisions.push_back({"rev-a", std::string(64, 'a'), ""});
  man.capabilities = {"host.log"};
  mod::Payload p;
  p.kind = "luau-source";
  p.path = "scripts/main.luau";
  p.size = source.size();
  const std::vector<std::uint8_t> bytes(source.begin(), source.end());
  p.sha256 = sha256_hex(bytes);
  p.role = "script";
  man.payloads.push_back(p);
  const mod::Package pkg = mod::read_package(mod::pack_package(man, {{"scripts/main.luau", bytes}}));
  if (!pkg.ok) { g_luau.cause = "package:" + pkg.rejection.code; return g_luau; }
  const luau::CompileResult c = luau::compile_mod(pkg);
  if (!c.ok) { g_luau.cause = "compile:" + c.rejection.code; return g_luau; }
  luau::Limits limits;
  limits.callback_units = 20000;
  limits.callback_watchdog_ms = 0;   // deterministic budget only
  limits.frame_watchdog_ms = 0;
  luau::HostConfig host;
  host.capabilities = {"host.log"};
  std::string error;
  auto provider = luau::PhaseProvider::create(pkg.identity, c.compiled, limits, host, luau_sink, error);
  if (!provider) { g_luau.cause = "create:" + error; return g_luau; }
  SegaModHost h{};
  h.size = sizeof h;
  h.abi_major = SEGA_MOD_ABI_MAJOR;
  h.phase = SEGA_MOD_PHASE_RESET;
  h.frame = 0;
  h.log = luau_log;
  provider->provider()->phase(provider->state(), &h);
  g_luau.ok = g_luau.cause.empty();
  return g_luau;
}

std::vector<Scenario> make_scenarios() {
  std::vector<Scenario> s;
  const auto space = [](const mod::Adapter &a) { return std::string(a.writable_space); };

  s.push_back({"capability_negotiation", [](const mod::Adapter &) { return std::vector<std::string>{"host.log"}; },
               [](const mod::Adapter &) { return std::string("pass"); }});   // the per-capability declaration table is verified in run_row (it needs the row)

  s.push_back({"sdk_negotiation", [](const mod::Adapter &) { return std::vector<std::string>{"host.log"}; }, [](const mod::Adapter &a) {
                 SegaModProvider p{};
                 p.size = sizeof p;
                 p.abi_major = SEGA_MOD_ABI_MAJOR;
                 p.platform = a.platform_id;
                 p.phase = [](void *, const SegaModHost *) {};
                 SegaModHost host{};
                 host.size = sizeof host;
                 host.abi_major = SEGA_MOD_ABI_MAJOR;
                 host.abi_minor = SEGA_MOD_ABI_MINOR;
                 host.platform = a.platform_id;
                 uint64_t required[2] = {cap(SEGA_MOD_CAP_HOST_LOG), 0};
                 host.granted[0] = cap(SEGA_MOD_CAP_HOST_LOG);
                 if (sega_mod_negotiate(&p, &host, required) != SEGA_MOD_NEG_OK) return std::string("E_NEGOTIATION_FAILED");
                 required[0] |= cap(SEGA_MOD_CAP_MEMORY_WRITE);   // a capability the host does not grant is never dropped
                 if (sega_mod_negotiate(&p, &host, required) != SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED) return std::string("E_NEGOTIATION_DROPPED_CAPABILITY");
                 required[0] = cap(SEGA_MOD_CAP_HOST_LOG);
                 host.platform = a.platform_id % 3U + 1U;   // another platform's host
                 if (sega_mod_negotiate(&p, &host, required) != SEGA_MOD_NEG_E_ABI_PLATFORM) return std::string("E_NEGOTIATION_WRONG_PLATFORM_ACCEPTED");
                 return std::string("pass");
               }});

  s.push_back({"phase_ordering", [](const mod::Adapter &) { return std::vector<std::string>{"input.read"}; }, [](const mod::Adapter &a) {
                 return genesis_only(a, []() -> std::string {
                   GenesisRig r;
                   if (!r.attach(bit(SEGA_MOD_PHASE_RESET) | bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END) | bit(SEGA_MOD_PHASE_RENDER),
                                 cap(SEGA_MOD_CAP_INPUT_READ) | cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE), false))
                     return "E_ATTACH:" + r.rejection;
                   if (!run_to_frame(*r.rt, 3U)) return "E_RUN";
                   const std::vector<std::string> expect = {"reset:0", "frame-begin:0", "input:0", "post-input:0", "frame-end:0", "render:0", "frame-begin:1", "input:1", "post-input:1", "frame-end:1", "render:1"};
                   if (r.probe.trace.size() < expect.size() || !std::equal(expect.begin(), expect.end(), r.probe.trace.begin())) return "E_ORDER:" + join(r.probe.trace);
                   return "pass";
                 });
               }});

  s.push_back({"typed_guest_access", [space](const mod::Adapter &a) { return std::vector<std::string>{"memory.read." + space(a), "memory.write." + space(a)}; }, [](const mod::Adapter &a) {
                 return genesis_only(a, []() -> std::string {
                   GenesisRig r;
                   r.probe.exercise_access = true;
                   if (!r.attach(bit(SEGA_MOD_PHASE_FRAME_BEGIN), cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE), false)) return "E_ATTACH:" + r.rejection;
                   if (!run_to_frame(*r.rt, 2U)) return "E_RUN";
                   // big-endian, width-typed: the u32 0x11223344 lands as bytes 11 22 33 44; the u16 read sees 0x1122; the u8 at +3 sees 0x44
                   if (r.rt->work_ram[0x100] != 0x11 || r.rt->work_ram[0x103] != 0x44 || r.probe.read16 != 0x1122U || r.probe.read8 != 0x44U) return "E_ENDIAN";
                   // a write outside every declared range faults the callback deterministically
                   GenesisRig bad;
                   bad.probe.bad_range = true;
                   if (!bad.attach(bit(SEGA_MOD_PHASE_FRAME_BEGIN), cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE), false)) return "E_ATTACH:" + bad.rejection;
                   if (!run_to_frame(*bad.rt, 3U)) return "E_RUN";
                   if (bad.sched->fault.faulted == 0U || std::string(bad.sched->fault.cause) != "range") return "E_RANGE_NOT_ENFORCED";
                   return "pass";
                 });
               }});

  s.push_back({"mod_state_round_trip", [](const mod::Adapter &) { return std::vector<std::string>{"state.mod"}; }, [](const mod::Adapter &a) {
                 return genesis_only(a, []() -> std::string {
                   GenesisRig r;
                   if (!r.attach(bit(SEGA_MOD_PHASE_FRAME_BEGIN), cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE) | cap(SEGA_MOD_CAP_STATE_MOD), true)) return "E_ATTACH:" + r.rejection;
                   if (!run_to_frame(*r.rt, 5U)) return "E_RUN";
                   GenesisSnapshotBuffer s1{nullptr, 0};
                   if (genesis_snapshot_capture(&r.machine, &r.id, nullptr, &s1) != GENESIS_SNAPSHOT_OK) return "E_CAPTURE";
                   const std::string saved = r.hash();
                   if (!run_to_frame(*r.rt, 12U)) { genesis_snapshot_buffer_free(&s1); return "E_RUN"; }
                   const bool restored = genesis_snapshot_restore(&r.machine, &r.id, s1.data, s1.size) == GENESIS_SNAPSHOT_OK;
                   genesis_snapshot_buffer_free(&s1);
                   return restored && r.hash() == saved ? "pass" : "E_ROUND_TRIP";
                 });
               }});

  s.push_back({"sandbox_failure", [](const mod::Adapter &) { return std::vector<std::string>{"host.log"}; }, [](const mod::Adapter &a) {
                 // portable part: the same Luau VM on every platform
                 const LuauProbe good = luau_run_reset("function reset() sega.log(\"hello\") end\n");
                 if (!good.ok || good.lines != std::vector<std::string>{"hello"}) return std::string("E_LUAU_LIFECYCLE");
                 const LuauProbe err = luau_run_reset("function reset() error(\"boom\") end\n");
                 if (err.ok || err.cause != "error") return std::string("E_LUAU_ERROR_CLASS:") + err.cause;
                 const LuauProbe budget = luau_run_reset("function reset() while true do end end\n");
                 if (budget.ok || budget.cause != "budget") return std::string("E_LUAU_BUDGET_CLASS:") + budget.cause;
                 if (a.platform != "genesis") return std::string("pass");
                 // Genesis: a native capability violation is the contract's E_MOD_FAULT, the guest continues
                 GenesisRig r;
                 r.probe.bad_range = true;
                 if (!r.attach(bit(SEGA_MOD_PHASE_FRAME_BEGIN), cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE), false)) return "E_ATTACH:" + r.rejection;
                 if (!run_to_frame(*r.rt, 4U)) return std::string("E_RUN");
                 if (r.sched->disabled != 1U || std::string(r.sched->fault.text) != "E_MOD_FAULT cause=range mod=conformance phase=frame-begin frame=1") return "E_FAULT_DIAGNOSTIC:" + std::string(r.sched->fault.text);
                 return std::string("pass");
               }});

  s.push_back({"incompatible_target", [](const mod::Adapter &) { return std::vector<std::string>{"host.log"}; }, [](const mod::Adapter &a) {
                 if (mod::find_adapter("no-such-platform") != nullptr) return std::string("E_PLATFORM_GUESSED");
                 for (const mod::Adapter &other : mod::adapters()) {
                   if (other.platform == a.platform) continue;
                   SegaModProvider p{};
                   p.size = sizeof p;
                   p.abi_major = SEGA_MOD_ABI_MAJOR;
                   p.platform = other.platform_id;   // a provider built for another platform
                   p.phase = [](void *, const SegaModHost *) {};
                   SegaModHost host{};
                   host.size = sizeof host;
                   host.abi_major = SEGA_MOD_ABI_MAJOR;
                   host.abi_minor = SEGA_MOD_ABI_MINOR;
                   host.platform = a.platform_id;
                   const uint64_t required[2] = {0, 0};
                   if (sega_mod_negotiate(&p, &host, required) != SEGA_MOD_NEG_E_ABI_PLATFORM) return "E_ACCEPTED_FOREIGN_PROVIDER:" + std::string(other.platform);
                 }
                 return std::string("pass");
               }});

  s.push_back({"audio_command_mapping", [](const mod::Adapter &) { return std::vector<std::string>{"audio.command.send"}; }, [](const mod::Adapter &a) {
                 // declared by the registry, but is it SERVED? The Genesis scheduler grants no audio capability until SEG-145-T008, and refuses it at attach with the stable class.
                 return genesis_only(a, []() -> std::string {
                   GenesisRig r;
                   if (r.attach(bit(SEGA_MOD_PHASE_FRAME_BEGIN), cap(SEGA_MOD_CAP_AUDIO_COMMAND_SEND), false)) return "pass";
                   return r.rejection == kUnsupported ? "E_CAPABILITY_UNSUPPORTED:runtime" : "E_ATTACH:" + r.rejection;
                 });
               }});
  return s;
}

// The result of one scenario on one platform.
std::string run_scenario(const Scenario &sc, const mod::Adapter &adapter) {
  if (const auto rejection = mod::check_capabilities(adapter, sc.required(adapter), {})) return rejection->code + ":declared";
  return sc.run(adapter);
}
// The declared-support text: a bare class means "the registry refused", `:declared` / `:runtime` say where the refusal came from; rows spell the class only when the source is unambiguous.
bool matches(const std::string &expected, const std::string &actual) {
  if (expected == actual) return true;
  return expected == std::string(kUnsupported) && actual == std::string(kUnsupported) + ":declared";
}

// Runs the whole table; returns every mismatch (empty = the platforms behave exactly as declared).
std::vector<std::string> run_rows(const std::vector<Row> &rows, const std::vector<Scenario> &scenarios) {
  std::vector<std::string> bad;
  for (const mod::Adapter &adapter : mod::adapters()) {
    const Row *row = nullptr;
    for (const Row &r : rows)
      if (r.platform == adapter.platform) row = &r;
    if (row == nullptr) { bad.push_back(std::string(adapter.platform) + ": no conformance row (a registered platform must declare one)"); continue; }
    for (const Scenario &sc : scenarios) {
      const auto it = row->expect.find(sc.name);
      if (it == row->expect.end()) { bad.push_back(std::string(adapter.platform) + "/" + sc.name + ": no declared expectation"); continue; }
      const std::string actual = run_scenario(sc, adapter);
      if (!matches(it->second, actual)) bad.push_back(std::string(adapter.platform) + "/" + sc.name + ": declared '" + it->second + "', observed '" + actual + "'");
    }
    // the per-capability declaration table (capability negotiation, every catalogue entry)
    for (const mod::CapabilityInfo &c : mod::capability_catalogue()) {
      const std::string level = mod::support_name(mod::capability_support(adapter, c));
      const auto d = row->declared_capabilities.find(std::string(c.name));
      const std::string declared = d == row->declared_capabilities.end() ? "unsupported" : d->second;
      if (level != declared) bad.push_back(std::string(adapter.platform) + "/capability " + std::string(c.name) + ": declared '" + declared + "', registry says '" + level + "'");
      const auto rejection = mod::check_capabilities(adapter, {std::string(c.name)}, {});
      if ((declared == "unsupported") != (rejection.has_value() && rejection->code == kUnsupported)) bad.push_back(std::string(adapter.platform) + "/check " + std::string(c.name) + ": admission disagrees with the declaration");
    }
    if (const auto unknown = mod::check_capabilities(adapter, {"no.such.capability"}, {}); !unknown || unknown->code != "E_CAPABILITY_UNKNOWN") bad.push_back(std::string(adapter.platform) + ": an unknown capability must be E_CAPABILITY_UNKNOWN");
  }
  for (const Row &r : rows)
    if (mod::find_adapter(r.platform) == nullptr) bad.push_back(r.platform + ": a conformance row for a platform that is not registered");
  return bad;
}

}  // namespace

int main() {
  const std::vector<Scenario> scenarios = make_scenarios();
  const std::vector<Row> rows = make_rows();
  check(mod::adapters().size() == rows.size() && mod::adapters().size() == 3U, "three registered platform adapters, three declared rows");

  // 1. every platform behaves exactly as declared
  const std::vector<std::string> bad = run_rows(rows, scenarios);
  for (const std::string &b : bad) check(false, b);
  int pass = 0, refused = 0;
  for (const Row &r : rows)
    for (const auto &e : r.expect) (e.second == "pass" ? pass : refused) += 1;
  std::printf("conformance matrix: %d scenario rows pass, %d assert a fail-closed diagnostic\n", pass, refused);

  // 2. negative tests: a deliberately wrong declaration makes the suite fail, naming the platform and the scenario
  {
    std::vector<Row> wrong = rows;
    wrong[1].expect["mod_state_round_trip"] = "pass";   // Master System does NOT support mod state: claiming it must fail
    const std::vector<std::string> r = run_rows(wrong, scenarios);
    check(r.size() == 1U && r[0].find("master-system/mod_state_round_trip") != std::string::npos, "claiming Master System passes mod state fails exactly that row: " + join(r));
  }
  {
    std::vector<Row> wrong = rows;
    wrong[0].expect["phase_ordering"] = "E_CAPABILITY_UNSUPPORTED";   // Genesis passes phase ordering: understating it must fail too
    const std::vector<std::string> r = run_rows(wrong, scenarios);
    check(r.size() == 1U && r[0].find("genesis/phase_ordering") != std::string::npos, "understating Genesis fails exactly that row: " + join(r));
  }
  {
    std::vector<Row> wrong = rows;
    wrong[2].declared_capabilities["memory.write.main.work_ram"] = "experimental";   // a capability the registry marks unsupported
    const std::vector<std::string> r = run_rows(wrong, scenarios);
    check(!r.empty() && r[0].find("sega-cd/capability memory.write.main.work_ram") != std::string::npos, "declaring an unsupported capability fails the capability table: " + join(r));
  }
  {
    std::vector<Row> wrong = rows;
    wrong[2].expect["audio_command_mapping"] = "pass";   // the audio mapping row flips only with SEG-145-T013
    check(!run_rows(wrong, scenarios).empty(), "flipping the Sega CD audio row without the runtime fails");
  }
  {
    std::vector<Row> missing = rows;
    missing.pop_back();   // a registered platform without a row
    const std::vector<std::string> r = run_rows(missing, scenarios);
    check(!r.empty() && r[0].find("sega-cd: no conformance row") != std::string::npos, "a registered platform without a row fails: " + join(r));
    std::vector<Row> stale = rows;
    Row ghost = rows[1];
    ghost.platform = "saturn";
    stale.push_back(ghost);
    check(!run_rows(stale, scenarios).empty(), "a row for an unregistered platform fails");
  }

  // 3. the portable Luau fixtures are platform independent: the identical diagnostics on every platform
  {
    const LuauProbe a = luau_run_reset("function reset() error(\"x\") end\n");
    const LuauProbe b = luau_run_reset("function reset() error(\"x\") end\n");
    check(a.cause == "error" && a.cause == b.cause, "the Luau failure class is reproducible");
  }
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("mod_platform_conformance_tests: ok\n");
  return 0;
}
